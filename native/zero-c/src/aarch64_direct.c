#include "zero.h"
#include "aarch64_direct.h"
#include "aarch64_emit.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  A64_DIRECT_SCRATCH_SLOT_COUNT = 32u,
  A64_DIRECT_SCRATCH_SLOT_BYTES = 8u
};

// Forward decls: a record-returning function reserves a 16-byte slot above the scratch region to
// hold the caller's sret pointer (x8 at entry, clobbered by every BL), so field-store-into-sret and
// `ret r` sites can reload it. These are defined alongside frame sizing below.
static bool a64_returns_record(const IrFunction *fun);
static unsigned a64_sret_reserved_bytes(const IrFunction *fun);
static unsigned a64_sret_slot_offset(const IrFunction *fun);

static bool a64_diag(ZDiag *diag, const char *message, int line, int column, const char *actual) {
  if (diag) {
    diag->code = 4004;
    diag->line = line > 0 ? line : 1;
    diag->column = column > 0 ? column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct AArch64 backend subset");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "unsupported construct");
    snprintf(diag->help, sizeof(diag->help), "choose a supported direct target or restrict this program to AArch64 supported direct-backend constructs");
  }
  return false;
}

static bool a64_return_literal(const IrFunction *fun, uint32_t *out, ZDiag *diag) {
  if (!fun) return a64_diag(diag, "direct AArch64 backend requires a function", 1, 1, "missing function");
  if (fun->param_count != 0) {
    return a64_diag(diag, "direct AArch64 backend supports exported functions without parameters", fun->line, fun->column, fun->name);
  }
  if (fun->return_type != IR_TYPE_VOID && fun->return_type != IR_TYPE_U8 && fun->return_type != IR_TYPE_I32 && fun->return_type != IR_TYPE_U32 && fun->return_type != IR_TYPE_USIZE) {
    return a64_diag(diag, "direct AArch64 backend supports primitive 32-bit-or-smaller integer returns", fun->line, fun->column, fun->name);
  }
  *out = 0;
  if (fun->return_type == IR_TYPE_VOID) {
    if (fun->instr_len == 0) return true;
    if (fun->instr_len == 1 && fun->instrs[0].kind == IR_INSTR_RETURN && !fun->instrs[0].value) return true;
    return false;
  }
  if (fun->instr_len == 1 && fun->instrs[0].kind == IR_INSTR_RETURN && fun->instrs[0].value &&
      fun->instrs[0].value->kind == IR_VALUE_INT && fun->instrs[0].value->int_value <= 65535) {
    *out = (uint32_t)fun->instrs[0].value->int_value;
    return true;
  }
  return false;
}

static bool a64_type_is_scalar32(IrTypeKind type) {
  return type == IR_TYPE_BOOL || type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_I32 || type == IR_TYPE_U32 || type == IR_TYPE_USIZE;
}

static bool a64_type_is_scalar64(IrTypeKind type) {
  return type == IR_TYPE_I64 || type == IR_TYPE_U64;
}

static bool a64_type_is_scalar(IrTypeKind type) {
  return a64_type_is_scalar32(type) || a64_type_is_scalar64(type);
}

static bool a64_type_is_unsigned(IrTypeKind type) {
  return type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_USIZE || type == IR_TYPE_U32 || type == IR_TYPE_U64;
}

static bool a64_type_is_f64(IrTypeKind type) { return type == IR_TYPE_F64; }
static bool a64_type_is_float(IrTypeKind type) { return type == IR_TYPE_F32 || type == IR_TYPE_F64; }

static bool a64_const_u32_value(const IrValue *value, unsigned *out) {
  if (!value || value->kind != IR_VALUE_INT || value->int_value > UINT32_MAX) return false;
  if (out) *out = (unsigned)value->int_value;
  return true;
}

/* Byte size and log2 of a typed-span element. 1-byte elements (u8/i8/Bool) have log2 0, 2-byte
   (u16) log2 1, 8-byte (i64/u64/f64) log2 3, everything else (i32/u32/usize/f32) is 4 bytes /
   log2 2. u16 is currently exercised only as a codec read result width. */
static unsigned a64_elem_byte_size(IrTypeKind type) {
  switch (type) {
    case IR_TYPE_U8: case IR_TYPE_I8: case IR_TYPE_BOOL: return 1;
    case IR_TYPE_U16: return 2;
    case IR_TYPE_I64: case IR_TYPE_U64: case IR_TYPE_F64: return 8;
    default: return 4;
  }
}
static unsigned a64_elem_log2(IrTypeKind type) {
  switch (type) {
    case IR_TYPE_U8: case IR_TYPE_I8: case IR_TYPE_BOOL: return 0;
    case IR_TYPE_U16: return 1;
    case IR_TYPE_I64: case IR_TYPE_U64: case IR_TYPE_F64: return 3;
    default: return 2;
  }
}

static unsigned a64_cond_for_compare(IrCompareOp op, bool uns) {
  switch (op) {
    case IR_CMP_EQ: return 0;
    case IR_CMP_NE: return 1;
    case IR_CMP_LT: return uns ? 3 : 11;
    case IR_CMP_LE: return uns ? 9 : 13;
    case IR_CMP_GT: return uns ? 8 : 12;
    case IR_CMP_GE: return uns ? 2 : 10;
  }
  return 0;
}

static unsigned a64_invert_cond(unsigned cond) {
  return cond ^ 1u;
}

/* Float comparison condition codes. FCMP sets N=0,Z=0,C=1,V=1 on unordered (NaN), so these are
   chosen so every NaN/unordered ordering reads false except `!=`. */
static unsigned a64_float_cond_for_compare(IrCompareOp op) {
  switch (op) {
    case IR_CMP_EQ: return 0;  // EQ
    case IR_CMP_NE: return 1;  // NE
    case IR_CMP_LT: return 4;  // MI
    case IR_CMP_LE: return 9;  // LS
    case IR_CMP_GT: return 12; // GT
    case IR_CMP_GE: return 10; // GE
  }
  return 0;
}

/* CSET reg, <cond> : materialize a boolean from NZCV via the CSINC alias, which encodes the
   inverted condition. */
static void a64_emit_cset(ZBuf *text, unsigned gpr, unsigned cond) {
  z_aarch64_append_u32(text, 0x1a9f07e0u | (((cond ^ 1u) & 15u) << 12) | (gpr & 31u));
}

static unsigned a64_slot_offset(unsigned local_index) {
  return local_index * 8u;
}

static unsigned a64_local_slot_offset(const IrFunction *fun, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  if (fun && local_index < fun->local_len && fun->locals[local_index].frame_offset > 0 && frame_size >= fun->locals[local_index].frame_offset) {
    return frame_size - fun->locals[local_index].frame_offset + slot_offset;
  }
  // The fallback (no IR-set frame_offset) sits above both the scratch region and the sret slot so a
  // record-returning function never aliases the saved x8 pointer.
  return A64_DIRECT_SCRATCH_SLOT_COUNT * A64_DIRECT_SCRATCH_SLOT_BYTES + a64_sret_reserved_bytes(fun) + a64_slot_offset(local_index) + slot_offset;
}

static void a64_emit_load_local_w(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  z_aarch64_emit_load_w_sp(text, reg, a64_local_slot_offset(fun, local_index, slot_offset, frame_size));
}

static void a64_emit_load_local_x(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  z_aarch64_emit_load_x_sp(text, reg, a64_local_slot_offset(fun, local_index, slot_offset, frame_size));
}

static void a64_emit_store_local_w(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  z_aarch64_emit_store_w_sp(text, reg, a64_local_slot_offset(fun, local_index, slot_offset, frame_size));
}

static void a64_emit_store_local_x(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  z_aarch64_emit_store_x_sp(text, reg, a64_local_slot_offset(fun, local_index, slot_offset, frame_size));
}

// Load the pointer stashed in slot 0 of a ref<Record>/mutref<Record> local into `ptr_reg`.
// Use BEFORE any subsequent ldr/str with `ptr_reg` as the base; the ref-record slot itself is
// untouched. Mirrors the host macho64 macho_emit_load_ref_record_ptr.
static void a64_emit_load_ref_record_ptr(ZBuf *text, const IrFunction *fun, unsigned ptr_reg, unsigned local_index, unsigned frame_size) {
  a64_emit_load_local_x(text, fun, ptr_reg, local_index, 0, frame_size);
}

static bool a64_scratch_slot(unsigned slot, unsigned *offset, const IrValue *value, ZDiag *diag) {
  if (slot >= A64_DIRECT_SCRATCH_SLOT_COUNT) {
    return a64_diag(diag, "direct AArch64 expression nesting exceeds scratch register spill capacity", value ? value->line : 1, value ? value->column : 1, "expression too deep");
  }
  *offset = slot * A64_DIRECT_SCRATCH_SLOT_BYTES;
  return true;
}

static bool a64_emit_store_scratch(ZBuf *text, unsigned reg, IrTypeKind type, unsigned slot, const IrValue *value, ZDiag *diag) {
  unsigned offset = 0;
  if (!a64_scratch_slot(slot, &offset, value, diag)) return false;
  if (a64_type_is_scalar64(type)) z_aarch64_emit_store_x_sp(text, reg, offset);
  else z_aarch64_emit_store_w_sp(text, reg, offset);
  return true;
}

static bool a64_emit_load_scratch(ZBuf *text, unsigned reg, IrTypeKind type, unsigned slot, const IrValue *value, ZDiag *diag) {
  unsigned offset = 0;
  if (!a64_scratch_slot(slot, &offset, value, diag)) return false;
  if (a64_type_is_scalar64(type)) z_aarch64_emit_load_x_sp(text, reg, offset);
  else z_aarch64_emit_load_w_sp(text, reg, offset);
  return true;
}

/* Spill/reload an FP register to the same SP-relative scratch slot the integer paths use; one
   8-byte slot holds an f32 or f64 spill cleanly. */
static bool a64_emit_store_scratch_v(ZBuf *text, unsigned vreg, bool is64, unsigned slot, const IrValue *value, ZDiag *diag) {
  unsigned offset = 0;
  if (!a64_scratch_slot(slot, &offset, value, diag)) return false;
  z_aarch64_emit_str_v_off(text, vreg, 31, offset, is64);
  return true;
}

static bool a64_emit_load_scratch_v(ZBuf *text, unsigned vreg, bool is64, unsigned slot, const IrValue *value, ZDiag *diag) {
  unsigned offset = 0;
  if (!a64_scratch_slot(slot, &offset, value, diag)) return false;
  z_aarch64_emit_ldr_v_off(text, vreg, 31, offset, is64);
  return true;
}

/* Load/store an FP register from a local slot, addressed exactly as the integer local helpers do
   (SP-relative, via the shared slot-offset computation). */
static void a64_emit_load_local_v(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned slot_offset, bool is64, unsigned frame_size) {
  z_aarch64_emit_ldr_v_off(text, vreg, 31, a64_local_slot_offset(fun, local_index, slot_offset, frame_size), is64);
}

static void a64_emit_store_local_v(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned slot_offset, bool is64, unsigned frame_size) {
  z_aarch64_emit_str_v_off(text, vreg, 31, a64_local_slot_offset(fun, local_index, slot_offset, frame_size), is64);
}

static void a64_emit_u32_bounds_check(ZBuf *text, unsigned index_reg, unsigned len_reg) {
  z_aarch64_emit_cmp_w(text, index_reg, len_reg);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 3);
  z_aarch64_emit_brk(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
}

// 64-bit bounds check: traps when index_reg (treated as a 32-bit usize zero-extended into x) is not
// strictly less than len_reg (full 64-bit). Used for byte-view indexing where the len slot is now
// 64-bit so spans backed by >4 GiB mappings (e.g. large model files) bounds-check correctly.
static void a64_emit_u64_bounds_check(ZBuf *text, unsigned index_reg, unsigned len_reg) {
  z_aarch64_emit_cmp_x(text, index_reg, len_reg);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 3); // unsigned lower
  z_aarch64_emit_brk(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
}

// A raising function uses the packed fallible ABI: the value lives in the low 32 bits of x0 (or in
// v0 for float results) and the error tag lives in the high 32 bits of x0. Helpers mirror the macho
// AArch64 emitter so check/rescue/raise lower identically across hosted (Mach-O) and ELF.
static bool a64_is_main_function(const IrFunction *fun) {
  return fun && fun->is_exported && fun->name && strcmp(fun->name, "main") == 0;
}

static bool a64_function_propagates_to_process_exit(const IrFunction *fun) {
  return fun && (fun->raises || (a64_is_main_function(fun) && fun->return_type == IR_TYPE_I32 && fun->value_return_type == IR_TYPE_VOID));
}

// True when the prologue must spill x20/x21 and seed them with argc/argv. Currently
// applied only to the exported `main` on backends that opt in via ctx->seed_main_process_args
// (the bare ELF aarch64 exe path; obj+link goes through musl crt0 which seeds args natively).
static bool a64_function_seeds_process_args(const IrFunction *fun, const ZAArch64DirectContext *ctx) {
  return ctx && ctx->seed_main_process_args && a64_is_main_function(fun);
}

// MOVZ Xd, #imm16, LSL #32 — write the error code into the high 32 bits of `reg` (the packed-tag
// position), leaving the low 32 bits zero. Used by `raise` and the rescue-fallback error reset.
static void a64_emit_packed_error_reg(ZBuf *text, unsigned reg, unsigned code_value) {
  z_aarch64_emit_movz_x(text, reg, ((uint64_t)code_value) << 32);
}

// Extract the 32-bit error tag from `packed_reg`'s high half into `condition_reg` and CMP against
// XZR. NE = error (propagate / fall back); EQ = ok (success continues). Mirrors the macho helper.
static void a64_emit_error_condition_reg(ZBuf *text, unsigned condition_reg, unsigned packed_reg) {
  z_aarch64_emit_lsr_x_imm(text, condition_reg, packed_reg, 32);
  z_aarch64_emit_cmp_x(text, condition_reg, 31);
}

static void a64_emit_cast_normalize_reg(ZBuf *text, unsigned reg, IrTypeKind source, IrTypeKind target) {
  switch (target) {
    case IR_TYPE_BOOL:
    case IR_TYPE_U8:
      z_aarch64_emit_uxtb_w(text, reg, reg);
      return;
    case IR_TYPE_U16:
      z_aarch64_emit_uxth_w(text, reg, reg);
      return;
    case IR_TYPE_I32:
    case IR_TYPE_U32:
    case IR_TYPE_USIZE:
      z_aarch64_emit_mov_w(text, reg, reg);
      return;
    case IR_TYPE_I64:
    case IR_TYPE_U64:
      if (source == IR_TYPE_I32) z_aarch64_emit_sxtw_x(text, reg, reg);
      else if (source == IR_TYPE_U32 || source == IR_TYPE_USIZE) {
        // No truncation: a USIZE/U32-typed register may already hold a full 64-bit value when the
        // source is a 64-bit-wide len slot (Span<u8>/Mapping/etc.). Standard 32-bit ops
        // zero-extend their result into the X register, so any non-len source still presents a
        // valid 64-bit zero-extended value here.
      } else if (!a64_type_is_scalar64(source)) z_aarch64_emit_mov_w(text, reg, reg);
      return;
    default:
      return;
  }
}

static void a64_emit_binary_reg(ZBuf *text, IrBinaryOp op, unsigned dst, unsigned lhs, unsigned rhs, bool wide) {
  if (op == IR_BIN_ADD) {
    if (wide) z_aarch64_emit_add_x_reg(text, dst, lhs, rhs);
    else z_aarch64_emit_add_w_reg(text, dst, lhs, rhs);
  } else if (op == IR_BIN_SUB) {
    if (wide) z_aarch64_emit_sub_x_reg(text, dst, lhs, rhs);
    else z_aarch64_emit_sub_w_reg(text, dst, lhs, rhs);
  } else if (op == IR_BIN_MUL) {
    if (wide) z_aarch64_emit_mul_x_reg(text, dst, lhs, rhs);
    else z_aarch64_emit_mul_w_reg(text, dst, lhs, rhs);
  }
}

static bool a64_record_data_patch(ZAArch64DirectContext *ctx, size_t patch_offset, unsigned data_offset, ZDiag *diag, const IrValue *value) {
  if (!ctx || !ctx->record_data_patch) return a64_diag(diag, "direct AArch64 readonly data patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  return ctx->record_data_patch(ctx->patch_user, patch_offset, data_offset, value, diag);
}

static ZAArch64MathSymbol a64_math_symbol_for_value(IrValueKind kind) {
  switch (kind) {
    case IR_VALUE_MATH_EXPF: return Z_AARCH64_MATH_EXPF;
    case IR_VALUE_MATH_COSF: return Z_AARCH64_MATH_COSF;
    case IR_VALUE_MATH_SINF: return Z_AARCH64_MATH_SINF;
    case IR_VALUE_MATH_POWF: return Z_AARCH64_MATH_POWF;
    case IR_VALUE_MATH_FABSF: return Z_AARCH64_MATH_FABSF;
    case IR_VALUE_MATH_FLOORF: return Z_AARCH64_MATH_FLOORF;
    default: return Z_AARCH64_MATH_SQRTF;
  }
}

static bool a64_record_call_patch(ZAArch64DirectContext *ctx, size_t patch_offset, ZAArch64MathSymbol symbol, ZDiag *diag, const IrValue *value) {
  if (!ctx || !ctx->record_call_patch) return a64_diag(diag, "direct AArch64 libm call requires an executable/object target that binds libm", value ? value->line : 1, value ? value->column : 1, "missing call-patch context");
  return ctx->record_call_patch(ctx->patch_user, patch_offset, symbol, value, diag);
}

static bool a64_record_user_call_patch(ZAArch64DirectContext *ctx, size_t patch_offset, unsigned callee_index, ZDiag *diag, const IrValue *value) {
  if (!ctx || !ctx->record_user_call_patch) return a64_diag(diag, "direct AArch64 user-function call requires an executable/object target that binds user functions", value ? value->line : 1, value ? value->column : 1, "missing user-call-patch context");
  return ctx->record_user_call_patch(ctx->patch_user, patch_offset, callee_index, value, diag);
}

// AAPCS argument-slot accounting: BYTE_VIEW takes two int slots (ptr + 32-bit len), RECORD takes
// one (a pointer in one X reg). Plain scalars/floats also take one slot (in their respective bank).
static unsigned a64_abi_slots_for_param(const IrLocal *local) { return local && local->type == IR_TYPE_BYTE_VIEW ? 2u : 1u; }

static bool a64_emit_rodata_ptr_literal(ZBuf *text, unsigned reg, unsigned data_offset, ZAArch64DirectContext *ctx, const IrValue *value, ZDiag *diag) {
  while (((text->len + 8) % 8) != 0) z_aarch64_emit_nop(text);
  z_aarch64_emit_ldr_x_literal8(text, reg);
  z_aarch64_emit_b_offset_words(text, 3);
  size_t patch_offset = text->len;
  z_aarch64_append_u64(text, 0);
  return a64_record_data_patch(ctx, patch_offset, data_offset, diag, value);
}

static bool a64_emit_byte_view_ptr_at(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);
static bool a64_emit_byte_view_len_at(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);
static bool a64_emit_value_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);
static bool a64_emit_float_value_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);
static bool a64_emit_span_index_addr(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);
static bool a64_emit_call_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);
static void a64_emit_epilogue(ZBuf *text, unsigned frame_size, bool restore_process_args);
static bool a64_emit_check_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);
static bool a64_emit_rescue_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);
static bool a64_emit_float_check_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);
static bool a64_emit_float_rescue_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag);

static bool a64_emit_byte_view_len_at(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!view) return a64_diag(diag, "direct AArch64 byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (view->data_len > 65535) return a64_diag(diag, "direct AArch64 byte-view length is too large for this backend", view->line, view->column, "large byte view");
    z_aarch64_emit_movz_w(text, reg, view->data_len);
    return true;
  }
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    a64_emit_load_local_x(text, fun, reg, view->local_index, 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field length lives 8 bytes past the field's ptr, as a 64-bit byte/element count.
    // ref-record: deref the stashed pointer first, then load len at
    // [ptr_reg + field_offset + 8].
    if (fun->locals[view->local_index].is_ref) {
      unsigned ptr_reg = reg == 9 ? 8 : 9;
      a64_emit_load_ref_record_ptr(text, fun, ptr_reg, view->local_index, frame_size);
      z_aarch64_emit_load_x_imm(text, reg, ptr_reg, view->field_offset + 8);
      return true;
    }
    a64_emit_load_local_x(text, fun, reg, view->local_index, view->field_offset + 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    // `first.value` on a Maybe<MutSpan<u8>> local: span length lives at slot +16 (64-bit byte count).
    a64_emit_load_local_x(text, fun, reg, view->local_index, 16, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    // Reinterpreted element count = underlying byte length >> log2(sizeof(T)); a 1-byte element
    // (i8) keeps the byte length unchanged.
    if (!a64_emit_byte_view_len_at(text, fun, view->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
    unsigned shift = a64_elem_log2(view->element_type);
    if (shift > 0) z_aarch64_emit_lsr_x_imm(text, reg, reg, shift);
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    unsigned end = 0;
    if ((!view->index || a64_const_u32_value(view->index, &start)) &&
        a64_const_u32_value(view->right, &end) && end >= start && end - start <= 65535) {
      z_aarch64_emit_movz_w(text, reg, end - start);
      return true;
    }
    if ((!view->index || a64_const_u32_value(view->index, &start)) && view->right) {
      if (!a64_emit_value_to_reg_at(text, fun, view->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
      if (start > 0) z_aarch64_emit_sub_x_imm(text, reg, reg, start);
      return true;
    }
    if (view->index && view->right) {
      unsigned tmp = reg == 8 ? 9 : 8;
      if (!a64_emit_value_to_reg_at(text, fun, view->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
      if (!a64_emit_store_scratch(text, reg, IR_TYPE_U64, scratch_slot, view->right, diag)) return false;
      if (!a64_emit_value_to_reg_at(text, fun, view->index, tmp, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!a64_emit_load_scratch(text, reg, IR_TYPE_U64, scratch_slot, view->right, diag)) return false;
      a64_emit_binary_reg(text, IR_BIN_SUB, reg, reg, tmp, true);
      return true;
    }
  }
  return a64_diag(diag, "direct AArch64 byte-view length currently requires a literal, constant slice, or byte-view local", view->line, view->column, "unsupported byte view length");
}

static bool a64_emit_byte_view_ptr_at(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!view) return a64_diag(diag, "direct AArch64 byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    a64_emit_load_local_x(text, fun, reg, view->local_index, 0, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field: the 8-byte pointer lives at the field offset within the record local.
    // ref-record: deref stashed ptr first, then load the span ptr from
    // [deref + field_offset].
    if (fun->locals[view->local_index].is_ref) {
      unsigned ptr_reg = reg == 9 ? 8 : 9;
      a64_emit_load_ref_record_ptr(text, fun, ptr_reg, view->local_index, frame_size);
      z_aarch64_emit_load_x_imm(text, reg, ptr_reg, view->field_offset);
      return true;
    }
    a64_emit_load_local_x(text, fun, reg, view->local_index, view->field_offset, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    // `first.value` on a Maybe<MutSpan<u8>> local: span pointer lives at slot +8.
    a64_emit_load_local_x(text, fun, reg, view->local_index, 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    // Any value-typed array binds as a typed span — ptr = &arr[0]. Element scaling for
    // INDEX_LOAD/STORE / BYTE_SLICE rides on the carrying IR_VALUE's element_type.
    if (!local->is_array) return a64_diag(diag, "direct AArch64 byte-view array source must be a fixed array local", view->line, view->column, "unsupported array view");
    z_aarch64_emit_add_x_sp_imm(text, reg, a64_local_slot_offset(fun, view->array_index, 0, frame_size));
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) return a64_emit_rodata_ptr_literal(text, reg, view->data_offset, ctx, view, diag);
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    // A reinterpret keeps the same base pointer (zero-copy); only the element count changes.
    return a64_emit_byte_view_ptr_at(text, fun, view->left, reg, frame_size, scratch_slot, ctx, diag);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    unsigned shift = a64_elem_log2(view->element_type);
    if (!a64_emit_byte_view_ptr_at(text, fun, view->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
    if (!view->index) return true;
    if (a64_const_u32_value(view->index, &start)) {
      // Slice bounds are element indices, so the byte offset is start << log2(sizeof(T)).
      unsigned byte_start = start << shift;
      if (byte_start > 4095) return a64_diag(diag, "direct AArch64 byte slice constant start is too large", view->line, view->column, "unsupported byte slice");
      if (byte_start > 0) z_aarch64_emit_add_x_imm(text, reg, reg, byte_start);
      return true;
    }
    unsigned tmp = reg == 8 ? 9 : 8;
    if (!a64_emit_store_scratch(text, reg, IR_TYPE_U64, scratch_slot, view, diag)) return false;
    if (!a64_emit_value_to_reg_at(text, fun, view->index, tmp, frame_size, scratch_slot + 1, ctx, diag)) return false;
    if (!a64_emit_load_scratch(text, reg, IR_TYPE_U64, scratch_slot, view, diag)) return false;
    if (shift > 0) z_aarch64_emit_add_x_reg_lsl(text, reg, reg, tmp, shift);
    else z_aarch64_emit_add_x_reg(text, reg, reg, tmp);
    return true;
  }
  return a64_diag(diag, "direct AArch64 value is not a supported byte view", view->line, view->column, "unsupported byte view");
}

// Compute the element address (ptr + index * sizeof(T)) of a typed-span local into x9, with a
// bounds check (index < span.len traps). The index is materialized into w8 first, so callers must
// not rely on x8 surviving; x9 holds the element address on return.
static bool a64_emit_span_index_addr(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (local_index >= fun->local_len) return a64_diag(diag, "direct AArch64 span index local is out of range", index ? index->line : 1, index ? index->column : 1, "invalid span local");
  const IrLocal *local = &fun->locals[local_index];
  unsigned shift = a64_elem_log2(local->element_type);
  IrTypeKind index_type = index ? index->type : IR_TYPE_U32;
  if (!index || !a64_emit_value_to_reg_at(text, fun, index, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 8, index_type, scratch_slot, index, diag)) return false;
  a64_emit_load_local_x(text, fun, 9, local_index, 8, frame_size);
  a64_emit_u64_bounds_check(text, 8, 9);
  if (!a64_emit_load_scratch(text, 8, index_type, scratch_slot, index, diag)) return false;
  a64_emit_load_local_x(text, fun, 9, local_index, 0, frame_size);
  if (shift > 0) z_aarch64_emit_add_x_reg_lsl(text, 9, 9, 8, shift);
  else z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  return true;
}

// Emit a float-typed value into an FP register (S/D). Mirrors the integer dispatcher: floats are
// always routed here from their context (local-set, return, binary/compare operands, casts). This
// backend has no function calls or records, so only literals, locals, casts and binaries appear.
// Non-commutative binaries evaluate left-first, spilling the left operand to the scratch slot across
// the right's evaluation; there is no multiply-add fusion.
static bool a64_emit_float_value_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value) return a64_diag(diag, "direct AArch64 float expression is missing", 1, 1, "missing expression");
  bool is64 = a64_type_is_f64(value->type);
  switch (value->kind) {
    case IR_VALUE_FLOAT:
      // Materialize the IEEE bit pattern in an integer scratch register, then FMOV into the FP
      // register. f32 patterns fit in w8 (movz/movk); f64 patterns use the full x8 sequence.
      if (is64) {
        z_aarch64_emit_movz_x(text, 8, (uint64_t)value->int_value);
        z_aarch64_emit_fmov_from_gpr(text, vreg, 8, true);
      } else {
        z_aarch64_emit_movz_w(text, 8, (uint32_t)value->int_value);
        z_aarch64_emit_fmov_from_gpr(text, vreg, 8, false);
      }
      return true;
    case IR_VALUE_LOCAL:
      if (value->local_index >= fun->local_len) return a64_diag(diag, "direct AArch64 local index is out of range", value->line, value->column, "invalid local");
      if (!a64_type_is_float(fun->locals[value->local_index].type)) {
        return a64_diag(diag, "direct AArch64 float load requires a float local", value->line, value->column, "non-float local");
      }
      a64_emit_load_local_v(text, fun, vreg, value->local_index, 0, is64, frame_size);
      return true;
    case IR_VALUE_FIELD_LOAD:
      // Float field read from a record local — LDR s/d at frame slot + field offset.
      if (value->local_index >= fun->local_len) return a64_diag(diag, "direct AArch64 float field load record is out of range", value->line, value->column, "invalid record local");
      if (!fun->locals[value->local_index].is_record) return a64_diag(diag, "direct AArch64 float field load requires record local", value->line, value->column, "non-record local");
      // ref-record FP field load: deref the stashed pointer into x9 and base the FP load
      // off it.
      if (fun->locals[value->local_index].is_ref) {
        a64_emit_load_ref_record_ptr(text, fun, 9, value->local_index, frame_size);
        z_aarch64_emit_ldr_v_off(text, vreg, 9, value->field_offset, is64);
        return true;
      }
      a64_emit_load_local_v(text, fun, vreg, value->local_index, value->field_offset, is64, frame_size);
      return true;
    case IR_VALUE_CAST: {
      if (!value->left) return a64_diag(diag, "direct AArch64 cast missing operand", value->line, value->column, "missing cast operand");
      IrTypeKind src = value->left->type;
      if (a64_type_is_float(src)) {
        // float -> float: widen/narrow when the widths differ, otherwise a plain move.
        if (!a64_emit_float_value_to_vreg_at(text, fun, value->left, vreg, frame_size, scratch_slot, ctx, diag)) return false;
        if (a64_type_is_f64(src) != is64) z_aarch64_emit_fcvt(text, vreg, vreg, is64);
        return true;
      }
      // integer -> float: unsigned u64 via UCVTF from x8, every other integer via SCVTF from w8.
      if (!a64_emit_value_to_reg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
      if (a64_type_is_scalar64(src) && a64_type_is_unsigned(src)) z_aarch64_emit_ucvtf_from_x(text, vreg, 8, is64);
      else z_aarch64_emit_scvtf_from_w(text, vreg, 8, is64);
      return true;
    }
    case IR_VALUE_BINARY: {
      if (value->binary_op != IR_BIN_ADD && value->binary_op != IR_BIN_SUB && value->binary_op != IR_BIN_MUL && value->binary_op != IR_BIN_DIV) {
        return a64_diag(diag, "direct AArch64 float binary operator is unsupported", value->line, value->column, "unsupported operator");
      }
      if (!a64_emit_float_value_to_vreg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
      if (!a64_emit_store_scratch_v(text, 8, is64, scratch_slot, value->left, diag)) return false;
      if (!a64_emit_float_value_to_vreg_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!a64_emit_load_scratch_v(text, 8, is64, scratch_slot, value->left, diag)) return false;
      if (value->binary_op == IR_BIN_ADD) z_aarch64_emit_fadd(text, vreg, 8, 9, is64);
      else if (value->binary_op == IR_BIN_SUB) z_aarch64_emit_fsub(text, vreg, 8, 9, is64);
      else if (value->binary_op == IR_BIN_MUL) z_aarch64_emit_fmul(text, vreg, 8, 9, is64);
      else z_aarch64_emit_fdiv(text, vreg, 8, 9, is64);
      return true;
    }
    case IR_VALUE_CALL:
      // A float-returning Zero function: float args marshal to V0.., the result returns in v0
      // and is moved to vreg, all handled by a64_emit_call_to_reg_at keyed off the value's float
      // type.
      return a64_emit_call_to_reg_at(text, fun, value, vreg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_MATH_SQRTF:
    case IR_VALUE_MATH_EXPF:
    case IR_VALUE_MATH_COSF:
    case IR_VALUE_MATH_SINF:
    case IR_VALUE_MATH_FABSF:
    case IR_VALUE_MATH_FLOORF: {
      // Single-arg libm call (AAPCS FP ABI): argument in s0, result in s0. All std.math libm helpers
      // operate on f32. The `bl` placeholder is bound by the host linker via an R_AARCH64_CALL26
      // relocation against the bare ELF symbol; the FP scratch survives because it lives in frame
      // memory, not a caller-saved register. Only the obj+link path can bind these externals, so a
      // libm program is routed there by the dispatch.
      if (!a64_emit_float_value_to_vreg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
      size_t patch = z_aarch64_emit_bl_placeholder(text);
      if (!a64_record_call_patch(ctx, patch, a64_math_symbol_for_value(value->kind), diag, value)) return false;
      if (vreg != 0) z_aarch64_emit_fmov_reg(text, vreg, 0, false);
      return true;
    }
    case IR_VALUE_MATH_POWF: {
      // Two-arg libm call: arg0 in s0, arg1 in s1. Compute arg0 into s0, spill it to the FP scratch
      // slot, compute arg1 into s1 (its evaluation may itself use the FP scratch or call libm), then
      // reload arg0 into s0 just before the call.
      if (!a64_emit_float_value_to_vreg_at(text, fun, value->left, 0, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!a64_emit_store_scratch_v(text, 0, false, scratch_slot, value->left, diag)) return false;
      if (!a64_emit_float_value_to_vreg_at(text, fun, value->right, 1, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!a64_emit_load_scratch_v(text, 0, false, scratch_slot, value->left, diag)) return false;
      size_t patch = z_aarch64_emit_bl_placeholder(text);
      if (!a64_record_call_patch(ctx, patch, Z_AARCH64_MATH_POWF, diag, value)) return false;
      if (vreg != 0) z_aarch64_emit_fmov_reg(text, vreg, 0, false);
      return true;
    }
    case IR_VALUE_CHECK:
      return a64_emit_float_check_to_vreg_at(text, fun, value, vreg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_RESCUE:
      return a64_emit_float_rescue_to_vreg_at(text, fun, value, vreg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_INDEX_LOAD: {
      // Float typed-span element read: element-scaled address then LDR s/d into the FP register.
      if (value->array_index >= fun->local_len) return a64_diag(diag, "direct AArch64 float index load array is out of range", value->line, value->column, "invalid span local");
      const IrLocal *local = &fun->locals[value->array_index];
      if (local->type != IR_TYPE_BYTE_VIEW || !a64_type_is_float(local->element_type)) {
        return a64_diag(diag, "direct AArch64 float index load requires a float span", value->line, value->column, "non-float span element");
      }
      if (!a64_emit_span_index_addr(text, fun, value->array_index, value->index, frame_size, scratch_slot, ctx, diag)) return false;
      z_aarch64_emit_ldr_v_off(text, vreg, 9, 0, is64);
      return true;
    }
    case IR_VALUE_BYTE_VIEW_READ_FLOAT_LE: {
      // std.codec.readF32Le / readF64Le: read 4 or 8 little-endian bytes at a byte offset. AArch64 is
      // little-endian, so an FP load at ptr+offset yields the value. Bounds-check that offset+size
      // fits within the span length first: the unsigned compare offset+(size-1) < len is equivalent
      // to offset+size <= len and rejects offset overflow. Integer scratch x8..x10 stage the offset,
      // length, and element address before the value lands in the FP register.
      if (!value->left) return a64_diag(diag, "direct AArch64 readF*Le requires a byte view", value->line, value->column, "missing byte view");
      if (!value->index) return a64_diag(diag, "direct AArch64 readF*Le requires an offset", value->line, value->column, "missing offset");
      unsigned scalar_size = is64 ? 8u : 4u;
      IrTypeKind index_type = value->index->type;
      if (!a64_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
      if (!a64_emit_store_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
      if (!a64_emit_byte_view_len_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!a64_emit_load_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
      z_aarch64_emit_add_w_imm(text, 10, 8, scalar_size - 1u); // offset + (size - 1)
      a64_emit_u64_bounds_check(text, 10, 9);
      if (!a64_emit_byte_view_ptr_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!a64_emit_load_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
      z_aarch64_emit_add_x_reg(text, 9, 9, 8);
      z_aarch64_emit_ldr_v_off(text, vreg, 9, 0, is64);
      return true;
    }
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported float value kind %d", value ? (int)value->kind : -1);
      return a64_diag(diag, "direct AArch64 float value kind is unsupported", value->line, value->column, actual);
    }
  }
}

static bool a64_emit_cast_value_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  // A float operand cast to an integer: evaluate into v8 and truncate toward zero into a W result.
  if (value->left && a64_type_is_float(value->left->type)) {
    if (!a64_emit_float_value_to_vreg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
    z_aarch64_emit_fcvtzs_w(text, reg, 8, a64_type_is_f64(value->left->type));
    return true;
  }
  if (!a64_emit_value_to_reg_at(text, fun, value->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
  a64_emit_cast_normalize_reg(text, reg, value->left ? value->left->type : IR_TYPE_UNSUPPORTED, value->type);
  return true;
}

static bool a64_emit_binary_value_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (value->binary_op == IR_BIN_AND) {
    if (!a64_emit_value_to_reg_at(text, fun, value->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
    size_t left_false = z_aarch64_emit_cbz_w_placeholder(text, reg);
    if (!a64_emit_value_to_reg_at(text, fun, value->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
    size_t right_false = z_aarch64_emit_cbz_w_placeholder(text, reg);
    z_aarch64_emit_movz_w(text, reg, 1);
    size_t end_patch = z_aarch64_emit_b_placeholder(text);
    z_aarch64_patch_cond19(text, left_false, text->len);
    z_aarch64_patch_cond19(text, right_false, text->len);
    z_aarch64_emit_movz_w(text, reg, 0);
    z_aarch64_patch_branch26(text, end_patch, text->len);
    return true;
  }
  if (value->binary_op == IR_BIN_OR) {
    if (!a64_emit_value_to_reg_at(text, fun, value->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
    size_t eval_right = z_aarch64_emit_cbz_w_placeholder(text, reg);
    z_aarch64_emit_movz_w(text, reg, 1);
    size_t left_true_end = z_aarch64_emit_b_placeholder(text);
    z_aarch64_patch_cond19(text, eval_right, text->len);
    if (!a64_emit_value_to_reg_at(text, fun, value->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
    size_t right_false = z_aarch64_emit_cbz_w_placeholder(text, reg);
    z_aarch64_emit_movz_w(text, reg, 1);
    size_t right_true_end = z_aarch64_emit_b_placeholder(text);
    z_aarch64_patch_cond19(text, right_false, text->len);
    z_aarch64_emit_movz_w(text, reg, 0);
    z_aarch64_patch_branch26(text, left_true_end, text->len);
    z_aarch64_patch_branch26(text, right_true_end, text->len);
    return true;
  }
  if (value->binary_op != IR_BIN_ADD && value->binary_op != IR_BIN_SUB && value->binary_op != IR_BIN_MUL &&
      value->binary_op != IR_BIN_DIV && value->binary_op != IR_BIN_MOD) {
    return a64_diag(diag, "direct AArch64 binary operator is unsupported", value->line, value->column, "unsupported operator");
  }
  if (!a64_emit_value_to_reg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 8, value->left ? value->left->type : IR_TYPE_I32, scratch_slot, value->left, diag)) return false;
  if (!a64_emit_value_to_reg_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 8, value->left ? value->left->type : IR_TYPE_I32, scratch_slot, value->left, diag)) return false;
  bool wide = a64_type_is_scalar64(value->type);
  if (value->binary_op == IR_BIN_DIV) {
    z_aarch64_emit_div_reg(text, reg, 8, 9, a64_type_is_unsigned(value->type), wide);
  } else if (value->binary_op == IR_BIN_MOD) {
    z_aarch64_emit_div_reg(text, 10, 8, 9, a64_type_is_unsigned(value->type), wide);
    z_aarch64_emit_msub_reg(text, reg, 10, 9, 8, wide);
  } else {
    a64_emit_binary_reg(text, value->binary_op, reg, 8, 9, wide);
  }
  return true;
}

static bool a64_emit_compare_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return a64_diag(diag, "direct AArch64 comparison requires two operands", value->line, value->column, "invalid comparison");
  // A float comparison yields an integer boolean from float operands: spill the left operand to an
  // FP scratch slot across the right's evaluation, FCMP, then materialize the IEEE-correct boolean.
  if (a64_type_is_float(value->left->type)) {
    bool is64 = a64_type_is_f64(value->left->type);
    if (!a64_emit_float_value_to_vreg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
    if (!a64_emit_store_scratch_v(text, 8, is64, scratch_slot, value->left, diag)) return false;
    if (!a64_emit_float_value_to_vreg_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
    if (!a64_emit_load_scratch_v(text, 8, is64, scratch_slot, value->left, diag)) return false;
    z_aarch64_emit_fcmp(text, 8, 9, is64);
    a64_emit_cset(text, reg, a64_float_cond_for_compare(value->compare_op));
    return true;
  }
  if (!a64_emit_value_to_reg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 8, value->left->type, scratch_slot, value->left, diag)) return false;
  if (!a64_emit_value_to_reg_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 8, value->left->type, scratch_slot, value->left, diag)) return false;
  bool wide = a64_type_is_scalar64(value->left->type);
  bool uns = a64_type_is_unsigned(value->left->type);
  if (wide) z_aarch64_emit_cmp_x(text, 8, 9);
  else z_aarch64_emit_cmp_w(text, 8, 9);
  z_aarch64_emit_movz_w(text, reg, 0);
  size_t false_patch = z_aarch64_emit_b_cond_placeholder(text, a64_invert_cond(a64_cond_for_compare(value->compare_op, uns)));
  z_aarch64_emit_movz_w(text, reg, 1);
  z_aarch64_patch_cond19(text, false_patch, text->len);
  return true;
}

static bool a64_emit_byte_copy_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return a64_diag(diag, "direct AArch64 byte copy requires source and destination byte views", value->line, value->column, "missing byte view");
  if (!a64_emit_byte_view_ptr_at(text, fun, value->left, 11, frame_size, scratch_slot, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 11, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  if (!a64_emit_byte_view_len_at(text, fun, value->left, 10, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 10, IR_TYPE_U32, scratch_slot + 1, value->left, diag)) return false;
  if (!a64_emit_byte_view_ptr_at(text, fun, value->right, 12, frame_size, scratch_slot + 2, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 12, IR_TYPE_U64, scratch_slot + 2, value->right, diag)) return false;
  if (!a64_emit_byte_view_len_at(text, fun, value->right, 13, frame_size, scratch_slot + 3, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 10, IR_TYPE_U32, scratch_slot + 1, value->left, diag)) return false;
  if (!a64_emit_load_scratch(text, 11, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  if (!a64_emit_load_scratch(text, 12, IR_TYPE_U64, scratch_slot + 2, value->right, diag)) return false;
  z_aarch64_emit_byte_copy_min_loop(text, reg);
  return true;
}

static bool a64_emit_byte_fill_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return a64_diag(diag, "direct AArch64 byte fill requires a fill byte and destination byte view", value->line, value->column, "missing byte fill input");
  if (!a64_emit_value_to_reg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 8, IR_TYPE_U8, scratch_slot, value->left, diag)) return false;
  if (!a64_emit_byte_view_ptr_at(text, fun, value->right, 11, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 11, IR_TYPE_U64, scratch_slot + 1, value->right, diag)) return false;
  if (!a64_emit_byte_view_len_at(text, fun, value->right, 10, frame_size, scratch_slot + 2, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 8, IR_TYPE_U8, scratch_slot, value->left, diag)) return false;
  if (!a64_emit_load_scratch(text, 11, IR_TYPE_U64, scratch_slot + 1, value->right, diag)) return false;
  z_aarch64_emit_byte_fill_loop(text, reg);
  return true;
}

static bool a64_emit_byte_view_eq_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return a64_diag(diag, "direct AArch64 byte-view equality requires two byte views", value->line, value->column, "missing byte view");
  if (!a64_emit_byte_view_len_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 8, IR_TYPE_U32, scratch_slot, value->left, diag)) return false;
  if (!a64_emit_byte_view_len_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 8, IR_TYPE_U32, scratch_slot, value->left, diag)) return false;
  z_aarch64_emit_cmp_w(text, 8, 9);
  size_t same_len = z_aarch64_emit_b_cond_placeholder(text, 0);
  z_aarch64_emit_movz_w(text, reg, 0);
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, same_len, text->len);
  if (!a64_emit_byte_view_ptr_at(text, fun, value->left, 11, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 11, IR_TYPE_U64, scratch_slot + 1, value->left, diag)) return false;
  if (!a64_emit_byte_view_ptr_at(text, fun, value->right, 12, frame_size, scratch_slot + 2, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 11, IR_TYPE_U64, scratch_slot + 1, value->left, diag)) return false;
  if (!a64_emit_load_scratch(text, 10, IR_TYPE_U32, scratch_slot, value->left, diag)) return false;
  z_aarch64_emit_byte_eq_loop(text, reg);
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

static bool a64_emit_byte_view_index_load_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value->index || !a64_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  if (!a64_emit_byte_view_len_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  a64_emit_u64_bounds_check(text, 8, 9);
  if (!a64_emit_byte_view_ptr_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  z_aarch64_emit_load_b_imm(text, reg, 9, 0);
  return true;
}

static bool a64_emit_byte_view_read_int_le_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  // std.codec.readU16Le / readI32Le / readU32Le / readI64Le / readU64Le: read a 2/4/8-byte
  // little-endian integer at a byte offset. AArch64 is little-endian, so an integer load at
  // ptr+offset already yields the value; same-width signed/unsigned share the raw load. Bounds-check
  // that offset+size fits within the span length before loading: with the unsigned compare
  // offset+(size-1) < len is equivalent to offset+size <= len, and rejects offset overflow.
  if (!value->left) return a64_diag(diag, "direct AArch64 read*Le requires a byte view", value->line, value->column, "missing byte view");
  if (!value->index) return a64_diag(diag, "direct AArch64 read*Le requires an offset", value->line, value->column, "missing offset");
  unsigned scalar_size = a64_elem_byte_size(value->type);
  IrTypeKind index_type = value->index->type;
  if (!a64_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
  if (!a64_emit_byte_view_len_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_add_w_imm(text, 10, 8, scalar_size - 1u); // offset + (size - 1)
  a64_emit_u64_bounds_check(text, 10, 9);
  if (!a64_emit_byte_view_ptr_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!a64_emit_load_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  if (scalar_size == 8) z_aarch64_emit_load_x_imm(text, reg, 9, 0);
  else if (scalar_size == 2) z_aarch64_emit_load_h_imm(text, reg, 9, 0);
  else z_aarch64_emit_load_w_imm(text, reg, 9, 0);
  return true;
}

static bool a64_emit_index_load_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (value->array_index >= fun->local_len) return a64_diag(diag, "direct AArch64 indexed load array is out of range", value->line, value->column, "invalid array local");
  const IrLocal *local = &fun->locals[value->array_index];
  unsigned const_index = 0;
  if (local->type == IR_TYPE_BYTE_VIEW) {
    // Typed-span element read: element-scaled address then a width-appropriate integer load. i8 is
    // sign-extended (LDRSB); u8/Bool zero-extended (LDRB); 8-byte elements via LDR x; else LDR w.
    if (!a64_emit_span_index_addr(text, fun, value->array_index, value->index, frame_size, scratch_slot, ctx, diag)) return false;
    IrTypeKind elem = local->element_type;
    if (elem == IR_TYPE_U8 || elem == IR_TYPE_BOOL) z_aarch64_emit_load_b_imm(text, reg, 9, 0);
    else if (elem == IR_TYPE_I8) z_aarch64_emit_load_sb_imm(text, reg, 9, 0);
    else if (a64_elem_byte_size(elem) == 8) z_aarch64_emit_load_x_imm(text, reg, 9, 0);
    else z_aarch64_emit_load_w_imm(text, reg, 9, 0);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE) &&
      a64_const_u32_value(value->index, &const_index) && const_index < local->array_len) {
    a64_emit_load_local_w(text, fun, reg, value->array_index, const_index * 4u, frame_size);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
    if (!value->index || !a64_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
    if (!a64_emit_store_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
    z_aarch64_emit_movz_w(text, 9, local->array_len);
    a64_emit_u32_bounds_check(text, 8, 9);
    if (!a64_emit_load_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
    z_aarch64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, value->array_index, 0, frame_size));
    z_aarch64_emit_add_x_reg_lsl(text, 9, 9, 8, 2);
    z_aarch64_emit_load_w_imm(text, reg, 9, 0);
    return true;
  }
  if (!local->is_array || (local->element_type != IR_TYPE_U8 && local->element_type != IR_TYPE_BOOL)) return a64_diag(diag, "direct AArch64 indexed load requires [N]u8, [N]Bool, or integer arrays", value->line, value->column, "unsupported array local");
  if (!value->index || !a64_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_movz_w(text, 9, local->array_len);
  a64_emit_u32_bounds_check(text, 8, 9);
  if (!a64_emit_load_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, value->array_index, 0, frame_size));
  z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  z_aarch64_emit_load_b_imm(text, reg, 9, 0);
  return true;
}

// General AAPCS call to a Zero user function. Two-pass: evaluate each arg into x8 (or v8 for
// float), spill it to scratch slots immediately, then once every arg is computed reload them into
// their final ABI registers (X0..X7 / V0..V7) just before the BL. The two-pass shape protects
// args from clobber by later (possibly nested) arg evaluations — the integer/float scratch banks
// are independent, so an FP arg only consumes an FP scratch slot. AAPCS slot counts: BYTE_VIEW
// occupies two int slots (ptr at arg_slot, 64-bit len at arg_slot+1); RECORD takes one (a pointer
// to a record local's frame slot, the callee copies-in on entry). After BL, the result lands in
// x0 / w0 / s0 / d0; if `reg` is not 0/v0, move it into the requested register.
static bool a64_emit_call_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  const IrFunction *callee = ctx && ctx->program && value->callee_index < ctx->program->function_len ? &ctx->program->functions[value->callee_index] : NULL;
  if (!callee) return a64_diag(diag, "direct AArch64 call target is unavailable", value->line, value->column, "invalid callee");
  unsigned abi_slots = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    if (i >= callee->param_count) return a64_diag(diag, "direct AArch64 call parameter metadata is unavailable", value->line, value->column, "invalid callee parameter");
    unsigned slots = a64_abi_slots_for_param(&callee->locals[i]);
    if (abi_slots + slots > 8) return a64_diag(diag, "direct AArch64 call supports at most eight ABI argument slots", value->line, value->column, "too many arguments");
    abi_slots += slots;
  }
  if (scratch_slot + abi_slots >= A64_DIRECT_SCRATCH_SLOT_COUNT) {
    return a64_diag(diag, "direct AArch64 call argument nesting exceeds scratch spill capacity", value->line, value->column, "too many nested call arguments");
  }
  unsigned nested_slot = scratch_slot + abi_slots;
  unsigned arg_slot = scratch_slot;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    const IrLocal *param = &callee->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) {
      if (!a64_emit_byte_view_ptr_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!a64_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      if (!a64_emit_byte_view_len_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!a64_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot + 1, arg, diag)) return false;
      arg_slot += 2;
      continue;
    }
    if (param->type == IR_TYPE_RECORD) {
      // Record args are passed by pointer to a record local's frame slot. The callee-side prologue
      // copies the pointed-to bytes into its own local slot (value semantics).
      if (!arg || arg->kind != IR_VALUE_LOCAL || arg->local_index >= fun->local_len) {
        return a64_diag(diag, "direct AArch64 record argument must be a record local", arg ? arg->line : value->line, arg ? arg->column : value->column, "non-local record arg");
      }
      z_aarch64_emit_add_x_sp_imm(text, 8, a64_local_slot_offset(fun, arg->local_index, 0, frame_size));
      if (!a64_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      arg_slot++;
      continue;
    }
    if (a64_type_is_float(param->type)) {
      bool is64 = a64_type_is_f64(param->type);
      if (!a64_emit_float_value_to_vreg_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!a64_emit_store_scratch_v(text, 8, is64, arg_slot, arg, diag)) return false;
      arg_slot++;
      continue;
    }
    if (!a64_emit_value_to_reg_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
    if (!a64_emit_store_scratch(text, 8, arg ? arg->type : IR_TYPE_I32, arg_slot, arg, diag)) return false;
    arg_slot++;
  }
  arg_slot = scratch_slot;
  unsigned abi_slot = 0;
  unsigned fp_abi_slot = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    const IrLocal *param = &callee->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) {
      if (!a64_emit_load_scratch(text, abi_slot, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      if (!a64_emit_load_scratch(text, abi_slot + 1, IR_TYPE_U64, arg_slot + 1, arg, diag)) return false;
      arg_slot += 2;
      abi_slot += 2;
      continue;
    }
    if (param->type == IR_TYPE_RECORD) {
      if (!a64_emit_load_scratch(text, abi_slot, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      arg_slot++;
      abi_slot++;
      continue;
    }
    if (a64_type_is_float(param->type)) {
      bool is64 = a64_type_is_f64(param->type);
      if (!a64_emit_load_scratch_v(text, fp_abi_slot, is64, arg_slot, arg, diag)) return false;
      arg_slot++;
      fp_abi_slot++;
      continue;
    }
    if (!a64_emit_load_scratch(text, abi_slot, arg ? arg->type : IR_TYPE_I32, arg_slot, arg, diag)) return false;
    arg_slot++;
    abi_slot++;
  }
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  if (!a64_record_user_call_patch(ctx, patch, value->callee_index, diag, value)) return false;
  if (a64_type_is_float(value->type)) {
    if (reg != 0) z_aarch64_emit_fmov_reg(text, reg, 0, a64_type_is_f64(value->type));
  } else if (reg != 0) {
    if (a64_type_is_scalar64(value->type)) z_aarch64_emit_mov_x(text, reg, 0);
    else z_aarch64_emit_mov_w(text, reg, 0);
  }
  return true;
}

// memcpy a record between inline frame slots: copy the source local's bytes to a destination
// — another local's slot, or the caller's sret buffer when dest_index == UINT_MAX. x8/x9 are
// scratch; 8-byte chunks then a 4-byte tail (records are 8- or 4-aligned, so the tail is exact).
// Span fields ride along as raw 16 bytes (ptr then len), preserving the view.
static void a64_emit_record_copy_to(ZBuf *text, const IrFunction *fun, unsigned dest_index, unsigned src_index, unsigned frame_size) {
  unsigned size = src_index < fun->local_len ? fun->locals[src_index].byte_size : 0;
  if (dest_index == UINT_MAX) {
    z_aarch64_emit_load_x_imm(text, 8, 31, a64_sret_slot_offset(fun)); // x8 = caller's sret pointer
  } else {
    z_aarch64_emit_add_x_sp_imm(text, 8, a64_local_slot_offset(fun, dest_index, 0, frame_size)); // x8 = &dest
  }
  z_aarch64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, src_index, 0, frame_size)); // x9 = &src
  unsigned k = 0;
  while (k + 8 <= size) {
    z_aarch64_emit_load_x_imm(text, 10, 9, k);
    z_aarch64_emit_store_x_imm(text, 10, 8, k);
    k += 8;
  }
  if (k + 4 <= size) {
    z_aarch64_emit_load_w_imm(text, 10, 9, k);
    z_aarch64_emit_store_w_imm(text, 10, 8, k);
  }
}

// Copy a record param (passed by pointer in ptr_reg = x0..x7) into its inline frame slot,
// preserving value semantics. x9/x10 are scratch; 8-byte chunks then a 4-byte tail.
static void a64_emit_copy_record_param(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned ptr_reg, unsigned frame_size) {
  unsigned size = local_index < fun->local_len ? fun->locals[local_index].byte_size : 0;
  z_aarch64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, local_index, 0, frame_size)); // x9 = &slot
  unsigned k = 0;
  while (k + 8 <= size) {
    z_aarch64_emit_load_x_imm(text, 10, ptr_reg, k);
    z_aarch64_emit_store_x_imm(text, 10, 9, k);
    k += 8;
  }
  if (k + 4 <= size) {
    z_aarch64_emit_load_w_imm(text, 10, ptr_reg, k);
    z_aarch64_emit_store_w_imm(text, 10, 9, k);
  }
}

// A record-returning call written into a destination buffer: marshal the call's own arguments via
// a64_emit_call_to_reg_at's machinery, then patch x8 (the AAPCS indirect-result register) to point
// at the destination, then bl. `dest_local` >= 0 binds the call result into a local's slot
// (`let x = f()`); `dest_local` < 0 returns straight through to the caller's sret buffer (`ret
// f()`). x8 is set just before bl because the argument emitters use x8 as scratch.
static bool a64_emit_record_call_with_dest(ZBuf *text, const IrFunction *fun, int dest_local, const IrValue *value, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  const IrFunction *callee = ctx && ctx->program && value->callee_index < ctx->program->function_len ? &ctx->program->functions[value->callee_index] : NULL;
  if (!callee) return a64_diag(diag, "direct AArch64 record call target is unavailable", value->line, value->column, "invalid callee");
  unsigned abi_slots = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    if (i >= callee->param_count) return a64_diag(diag, "direct AArch64 record call parameter metadata is unavailable", value->line, value->column, "invalid callee parameter");
    unsigned slots = a64_abi_slots_for_param(&callee->locals[i]);
    if (abi_slots + slots > 8) return a64_diag(diag, "direct AArch64 record call supports at most eight ABI argument slots", value->line, value->column, "too many arguments");
    abi_slots += slots;
  }
  if (scratch_slot + abi_slots >= A64_DIRECT_SCRATCH_SLOT_COUNT) {
    return a64_diag(diag, "direct AArch64 record call argument nesting exceeds scratch spill capacity", value->line, value->column, "too many nested call arguments");
  }
  unsigned nested_slot = scratch_slot + abi_slots;
  unsigned arg_slot = scratch_slot;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    const IrLocal *param = &callee->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) {
      if (!a64_emit_byte_view_ptr_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!a64_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      if (!a64_emit_byte_view_len_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!a64_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot + 1, arg, diag)) return false;
      arg_slot += 2;
      continue;
    }
    if (param->type == IR_TYPE_RECORD) {
      if (!arg || arg->kind != IR_VALUE_LOCAL || arg->local_index >= fun->local_len) {
        return a64_diag(diag, "direct AArch64 record argument must be a record local", arg ? arg->line : value->line, arg ? arg->column : value->column, "non-local record arg");
      }
      z_aarch64_emit_add_x_sp_imm(text, 8, a64_local_slot_offset(fun, arg->local_index, 0, frame_size));
      if (!a64_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      arg_slot++;
      continue;
    }
    if (a64_type_is_float(param->type)) {
      bool is64 = a64_type_is_f64(param->type);
      if (!a64_emit_float_value_to_vreg_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!a64_emit_store_scratch_v(text, 8, is64, arg_slot, arg, diag)) return false;
      arg_slot++;
      continue;
    }
    if (!a64_emit_value_to_reg_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
    if (!a64_emit_store_scratch(text, 8, arg ? arg->type : IR_TYPE_I32, arg_slot, arg, diag)) return false;
    arg_slot++;
  }
  arg_slot = scratch_slot;
  unsigned abi_slot = 0;
  unsigned fp_abi_slot = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    const IrLocal *param = &callee->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) {
      if (!a64_emit_load_scratch(text, abi_slot, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      if (!a64_emit_load_scratch(text, abi_slot + 1, IR_TYPE_U64, arg_slot + 1, arg, diag)) return false;
      arg_slot += 2;
      abi_slot += 2;
      continue;
    }
    if (param->type == IR_TYPE_RECORD) {
      if (!a64_emit_load_scratch(text, abi_slot, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      arg_slot++;
      abi_slot++;
      continue;
    }
    if (a64_type_is_float(param->type)) {
      bool is64 = a64_type_is_f64(param->type);
      if (!a64_emit_load_scratch_v(text, fp_abi_slot, is64, arg_slot, arg, diag)) return false;
      arg_slot++;
      fp_abi_slot++;
      continue;
    }
    if (!a64_emit_load_scratch(text, abi_slot, arg ? arg->type : IR_TYPE_I32, arg_slot, arg, diag)) return false;
    arg_slot++;
    abi_slot++;
  }
  // Set the indirect-result register x8 last so the argument-marshaling stage's x8 scratch use
  // cannot trample it. dest_local < 0 means the caller's own sret pointer (we are returning the
  // call's result straight through to our caller's storage).
  if (dest_local >= 0) {
    z_aarch64_emit_add_x_sp_imm(text, 8, a64_local_slot_offset(fun, (unsigned)dest_local, 0, frame_size));
  } else {
    z_aarch64_emit_load_x_imm(text, 8, 31, a64_sret_slot_offset(fun));
  }
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  return a64_record_user_call_patch(ctx, patch, value->callee_index, diag, value);
}

// Store one field of the record being returned, written through the caller's sret pointer (saved
// in the frame). x8 is reloaded from the slot after each value is materialized so an intervening
// call cannot strand it. Span fields store ptr@offset and len@offset+8.
static bool a64_emit_sret_field_store(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  unsigned slot = a64_sret_slot_offset(fun);
  IrTypeKind vt = instr->value ? instr->value->type : IR_TYPE_I32;
  unsigned fo = instr->field_offset;
  if (vt == IR_TYPE_BYTE_VIEW) {
    if (instr->value && instr->value->kind == IR_VALUE_CALL) {
      // A span-returning call leaves ptr in x0, len in x1; capture both before reloading x8.
      if (!a64_emit_call_to_reg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
      z_aarch64_emit_mov_x(text, 9, 1); // preserve len; x1 is otherwise clobbered by the x8 reload path
      z_aarch64_emit_load_x_imm(text, 8, 31, slot);
      z_aarch64_emit_store_x_imm(text, 0, 8, fo);
      z_aarch64_emit_store_x_imm(text, 9, 8, fo + 8);
    } else {
      if (!a64_emit_byte_view_ptr_at(text, fun, instr->value, 9, frame_size, 0, ctx, diag)) return false;
      z_aarch64_emit_load_x_imm(text, 8, 31, slot);
      z_aarch64_emit_store_x_imm(text, 9, 8, fo);
      if (!a64_emit_byte_view_len_at(text, fun, instr->value, 9, frame_size, 0, ctx, diag)) return false;
      z_aarch64_emit_load_x_imm(text, 8, 31, slot);
      z_aarch64_emit_store_x_imm(text, 9, 8, fo + 8);
    }
    return true;
  }
  if (a64_type_is_float(vt)) {
    bool is64 = a64_type_is_f64(vt);
    if (!a64_emit_float_value_to_vreg_at(text, fun, instr->value, 9, frame_size, 0, ctx, diag)) return false;
    z_aarch64_emit_load_x_imm(text, 8, 31, slot);
    z_aarch64_emit_str_v_off(text, 9, 8, fo, is64);
    return true;
  }
  if (!a64_emit_value_to_reg_at(text, fun, instr->value, 9, frame_size, 0, ctx, diag)) return false;
  z_aarch64_emit_load_x_imm(text, 8, 31, slot);
  if (vt == IR_TYPE_U8 || vt == IR_TYPE_I8 || vt == IR_TYPE_BOOL) z_aarch64_emit_store_b_imm(text, 9, 8, fo);
  else if (a64_type_is_scalar64(vt)) z_aarch64_emit_store_x_imm(text, 9, 8, fo);
  else z_aarch64_emit_store_w_imm(text, 9, 8, fo);
  return true;
}

static bool a64_emit_value_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value) return a64_diag(diag, "direct AArch64 expression is missing", 1, 1, "missing expression");
  switch (value->kind) {
    case IR_VALUE_BOOL:
    case IR_VALUE_INT:
      if (a64_type_is_scalar64(value->type)) z_aarch64_emit_movz_x(text, reg, (uint64_t)value->int_value);
      else z_aarch64_emit_movz_w(text, reg, (uint32_t)value->int_value);
      return true;
    case IR_VALUE_LOCAL:
      if (value->local_index >= fun->local_len) return a64_diag(diag, "direct AArch64 local index is out of range", value->line, value->column, "invalid local");
      if (fun->locals[value->local_index].is_array) return a64_diag(diag, "direct AArch64 fixed array local cannot be used as a scalar", value->line, value->column, "array local");
      if (fun->locals[value->local_index].type == IR_TYPE_BYTE_VIEW) return a64_diag(diag, "direct AArch64 byte-view local cannot be used as a scalar", value->line, value->column, "byte-view local");
      if (a64_type_is_scalar64(fun->locals[value->local_index].type)) a64_emit_load_local_x(text, fun, reg, value->local_index, 0, frame_size);
      else a64_emit_load_local_w(text, fun, reg, value->local_index, 0, frame_size);
      return true;
    case IR_VALUE_CAST: return a64_emit_cast_value_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BINARY: return a64_emit_binary_value_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_COMPARE: return a64_emit_compare_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_VIEW_LEN: return a64_emit_byte_view_len_at(text, fun, value->left, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_COPY: return a64_emit_byte_copy_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_FILL: return a64_emit_byte_fill_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_VIEW_EQ: return a64_emit_byte_view_eq_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_VIEW_INDEX_LOAD: return a64_emit_byte_view_index_load_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_VIEW_READ_INT_LE: return a64_emit_byte_view_read_int_le_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_INDEX_LOAD: return a64_emit_index_load_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_FIELD_LOAD: {
      // Read one field of a record local into a GPR. The local lives in the frame; the field is at
      // a constant offset within it. i8/u8/Bool zero-extend through LDRB; 8-byte fields via LDR x;
      // everything else via LDR w. Span fields are routed through the byte_view ptr/len helpers,
      // not here.
      if (value->local_index >= fun->local_len) return a64_diag(diag, "direct AArch64 field load record is out of range", value->line, value->column, "invalid record local");
      if (!fun->locals[value->local_index].is_record) return a64_diag(diag, "direct AArch64 field load requires record local", value->line, value->column, "non-record local");
      // ref<Record> field load: deref the stashed pointer first, then ldr at field_offset
      // off the pointer. Pick a ptr_reg that doesn't clash with the destination `reg`.
      if (fun->locals[value->local_index].is_ref) {
        unsigned ptr_reg = reg == 9 ? 8 : 9;
        a64_emit_load_ref_record_ptr(text, fun, ptr_reg, value->local_index, frame_size);
        if (value->type == IR_TYPE_U8 || value->type == IR_TYPE_I8 || value->type == IR_TYPE_BOOL) z_aarch64_emit_load_b_imm(text, reg, ptr_reg, value->field_offset);
        else if (a64_type_is_scalar64(value->type)) z_aarch64_emit_load_x_imm(text, reg, ptr_reg, value->field_offset);
        else z_aarch64_emit_load_w_imm(text, reg, ptr_reg, value->field_offset);
        return true;
      }
      if (value->type == IR_TYPE_U8 || value->type == IR_TYPE_I8 || value->type == IR_TYPE_BOOL) {
        z_aarch64_emit_load_b_sp(text, reg, a64_local_slot_offset(fun, value->local_index, value->field_offset, frame_size));
      } else if (a64_type_is_scalar64(value->type)) {
        a64_emit_load_local_x(text, fun, reg, value->local_index, value->field_offset, frame_size);
      } else {
        a64_emit_load_local_w(text, fun, reg, value->local_index, value->field_offset, frame_size);
      }
      return true;
    }
    case IR_VALUE_CALL:
      // An integer-returning Zero function: int args marshal to X0..X7 (with BYTE_VIEW eating
      // two slots), the result returns in x0/w0 and is moved to reg, all by a64_emit_call_to_reg_at
      // keyed off the value's integer type.
      return a64_emit_call_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_ARGS_LEN:
      // argc was stashed in x20 by the main prologue (see a64_emit_function_text);
      // mirror it into the destination register. The result is a 32-bit usize.
      z_aarch64_emit_mov_w(text, reg, 20);
      return true;
    case IR_VALUE_FS_HOST:
      // std.fs.host() is a stateless capability token: mirrors the host macho64 / ELF64 backends
      // which both return 0. The OS-interface lowerings call raw syscalls directly.
      z_aarch64_emit_movz_w(text, reg, 0);
      return true;
    case IR_VALUE_FS_MUNMAP: {
      // std.fs.munmap(&mut m): release a mapping via the raw Linux munmap syscall (215). The Mapping
      // local stores addr@0 (64-bit) and len@8 (64-bit byte count, matching the byte-view store);
      // the full 64-bit len lands in x1. Result type is Void; nothing is produced into `reg`. svc
      // clobbers x0/x1/x8 (caller-saved) but leaves x29/x30/x19+ intact.
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_BYTE_VIEW) {
        return a64_diag(diag, "direct AArch64 std.fs.munmap requires a Mapping local", value->line, value->column, "invalid Mapping");
      }
      a64_emit_load_local_x(text, fun, 0, value->local_index, 0, frame_size); // x0 = addr
      a64_emit_load_local_x(text, fun, 1, value->local_index, 8, frame_size); // x1 = len (64-bit byte count)
      z_aarch64_emit_movz_x(text, 8, 215);                                    // x8 = munmap
      z_aarch64_emit_svc(text, 0);
      (void)ctx;
      return true;
    }
    case IR_VALUE_MATH_ISNANF: {
      // isNaNf returns a Bool (a GPR result), so it is reached here rather than in the FP emitter.
      // NaN is the only value where x != x: FCMP sets V=1 (overflow) on an unordered compare, so
      // `cset reg, VS` yields 1 for NaN and 0 otherwise — inline, no libm call.
      if (!value->left) return a64_diag(diag, "direct AArch64 isNaNf requires a float operand", value->line, value->column, "missing operand");
      if (!a64_emit_float_value_to_vreg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
      z_aarch64_emit_fcmp(text, 8, 8, a64_type_is_f64(value->left->type));
      a64_emit_cset(text, reg, 6); // VS (overflow set = unordered)
      return true;
    }
    case IR_VALUE_CHECK:
      return a64_emit_check_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_RESCUE:
      return a64_emit_rescue_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_MAYBE_HAS:
      // .has on a Maybe<byte-view> or Maybe<scalar> reads the 32-bit flag at slot @0.
      if (value->local_index >= fun->local_len ||
          (fun->locals[value->local_index].type != IR_TYPE_MAYBE_BYTE_VIEW && fun->locals[value->local_index].type != IR_TYPE_MAYBE_SCALAR)) {
        return a64_diag(diag, "direct AArch64 maybe helper requires a Maybe local", value->line, value->column, "invalid maybe local");
      }
      a64_emit_load_local_w(text, fun, reg, value->local_index, 0, frame_size);
      return true;
    case IR_VALUE_MAYBE_VALUE:
      // Scalar payload at @8. Byte-view ptr@8/len@16 are read through a64_emit_byte_view_ptr_at /
      // _len_at when MAYBE_VALUE appears as a span source (Maybe<MutSpan<u8>>); this path covers
      // the scalar case (Maybe<i32> etc.).
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_MAYBE_SCALAR) {
        return a64_diag(diag, "direct AArch64 maybe scalar value requires a Maybe scalar local", value->line, value->column, "invalid maybe value");
      }
      a64_emit_load_local_x(text, fun, reg, value->local_index, 8, frame_size);
      return true;
    default:
      return a64_diag(diag, "direct AArch64 value kind is unsupported", value->line, value->column, "unsupported value");
  }
}

// `check <fallible call>`: evaluate the call into x0 (value in low 32, tag in high 32). On success
// (tag == 0) fall through and move the value into `reg`; on error either propagate by returning
// (x0 still carries the tag, which is what a raising caller wants — process exit reads x0 too) or,
// if the caller is non-fallible, write exit code 1 into w0 and epilogue. Mirrors the macho helper.
static bool a64_emit_check_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value->left || value->left->type != IR_TYPE_I64) return a64_diag(diag, "direct AArch64 check requires a packed fallible call result", value->line, value->column, "non-fallible value");
  if (!a64_emit_value_to_reg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  a64_emit_error_condition_reg(text, 8, 0);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 0); // EQ: tag == 0 → ok
  bool restore_process_args = a64_function_seeds_process_args(fun, ctx);
  if (a64_function_propagates_to_process_exit(fun)) {
    a64_emit_epilogue(text, frame_size, restore_process_args);
  } else {
    z_aarch64_emit_movz_w(text, 0, 1);
    a64_emit_epilogue(text, frame_size, restore_process_args);
  }
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  if (reg != 0) {
    if (a64_type_is_scalar64(value->type)) z_aarch64_emit_mov_x(text, reg, 0);
    else z_aarch64_emit_mov_w(text, reg, 0);
  } else if (!a64_type_is_scalar64(value->type)) {
    // Strip the high-32 tag from the result GPR so downstream consumers see a clean value.
    z_aarch64_emit_mov_w(text, 0, 0);
  }
  return true;
}

// `<fallible call> rescue err <fallback>`: evaluate the call into x0. On error branch to the
// fallback expression, which produces an alternative result into the target reg; on success the
// value is already in x0 and is moved (or high-32 cleared) into `reg`. Mirrors the macho helper.
static bool a64_emit_rescue_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right || value->left->type != IR_TYPE_I64) return a64_diag(diag, "direct AArch64 rescue requires a packed fallible call and fallback", value->line, value->column, "unsupported rescue");
  if (!a64_emit_value_to_reg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  a64_emit_error_condition_reg(text, 8, 0);
  size_t fallback_patch = z_aarch64_emit_b_cond_placeholder(text, 1); // NE: tag != 0 → fallback
  if (reg != 0) {
    if (a64_type_is_scalar64(value->type)) z_aarch64_emit_mov_x(text, reg, 0);
    else z_aarch64_emit_mov_w(text, reg, 0);
  } else if (!a64_type_is_scalar64(value->type)) {
    z_aarch64_emit_mov_w(text, 0, 0);
  }
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, fallback_patch, text->len);
  if (!a64_emit_value_to_reg_at(text, fun, value->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

// Float sibling of a64_emit_check_to_reg_at. `check <float fallible call>`: evaluate the call —
// value lands in v0, tag in x0's high 32 bits (a64_emit_call_to_reg_at leaves x0 untouched when the
// result type is float and reg==0). On error propagate (x0 already carries the tag); on success
// move v0 → vreg.
static bool a64_emit_float_check_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value->left) return a64_diag(diag, "direct AArch64 check requires a fallible call result", value->line, value->column, "non-fallible value");
  if (!a64_emit_float_value_to_vreg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  a64_emit_error_condition_reg(text, 8, 0);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 0); // EQ: tag == 0 → ok
  bool restore_process_args = a64_function_seeds_process_args(fun, ctx);
  if (a64_function_propagates_to_process_exit(fun)) {
    a64_emit_epilogue(text, frame_size, restore_process_args);
  } else {
    z_aarch64_emit_movz_w(text, 0, 1);
    a64_emit_epilogue(text, frame_size, restore_process_args);
  }
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  if (vreg != 0) z_aarch64_emit_fmov_reg(text, vreg, 0, a64_type_is_f64(value->type));
  return true;
}

// Float sibling of a64_emit_rescue_to_reg_at. `<float fallible call> rescue err <fallback>`:
// evaluate the call (value in v0, tag in x0). On error (tag != 0) branch to the fallback
// expression which lands its float result in vreg; on success v0 is moved to vreg.
static bool a64_emit_float_rescue_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return a64_diag(diag, "direct AArch64 rescue requires a fallible call and fallback", value->line, value->column, "unsupported rescue");
  if (!a64_emit_float_value_to_vreg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  a64_emit_error_condition_reg(text, 8, 0);
  size_t fallback_patch = z_aarch64_emit_b_cond_placeholder(text, 1); // NE: tag != 0 → fallback
  if (vreg != 0) z_aarch64_emit_fmov_reg(text, vreg, 0, a64_type_is_f64(value->type));
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, fallback_patch, text->len);
  if (!a64_emit_float_value_to_vreg_at(text, fun, value->right, vreg, frame_size, scratch_slot, ctx, diag)) return false;
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

// A record-returning function receives its destination buffer pointer in x8 (the AAPCS indirect-
// result register). x8 is caller-saved and clobbered by every `bl`, so we spill it to a dedicated
// frame slot in the prologue and reload it for every `ret c` / field-store-into-sret site.
static bool a64_returns_record(const IrFunction *fun) {
  return fun && fun->return_type == IR_TYPE_RECORD;
}

// 16-byte slot reserved between the scratch region and the locals to hold the saved x8 (sret
// pointer), padded to keep the locals 16-aligned.
static unsigned a64_sret_reserved_bytes(const IrFunction *fun) {
  return a64_returns_record(fun) ? 16u : 0u;
}

// Frame-relative offset (from sp) of the sret slot itself — placed immediately above the scratch
// region so it never aliases a scratch slot or a local.
static unsigned a64_sret_slot_offset(const IrFunction *fun) {
  (void)fun;
  return A64_DIRECT_SCRATCH_SLOT_COUNT * A64_DIRECT_SCRATCH_SLOT_BYTES;
}

static size_t a64_function_frame_bytes(const IrFunction *fun) {
  uint32_t ignored = 0;
  if (a64_return_literal(fun, &ignored, NULL)) return 0;
  unsigned base = (unsigned)(fun ? (fun->frame_bytes ? fun->frame_bytes : fun->local_len * 8) : 0);
  return z_aarch64_align(base + A64_DIRECT_SCRATCH_SLOT_COUNT * A64_DIRECT_SCRATCH_SLOT_BYTES + a64_sret_reserved_bytes(fun), 16);
}

size_t z_aarch64_direct_stack_bytes_from_ir(const IrProgram *program) {
  size_t total = 0;
  for (size_t i = 0; program && i < program->function_len; i++) total += a64_function_frame_bytes(&program->functions[i]);
  return total;
}

size_t z_aarch64_direct_max_frame_bytes_from_ir(const IrProgram *program) {
  size_t max_frame = 0;
  for (size_t i = 0; program && i < program->function_len; i++) {
    size_t frame = a64_function_frame_bytes(&program->functions[i]);
    if (frame > max_frame) max_frame = frame;
  }
  return max_frame;
}

static void a64_emit_epilogue(ZBuf *text, unsigned frame_size, bool restore_process_args) {
  if (frame_size > 0) z_aarch64_emit_add_sp_imm(text, frame_size);
  z_aarch64_emit_ldp_x29_x30_sp_post16(text);
  if (restore_process_args) z_aarch64_emit_ldp_x20_x21_sp_post16(text);
  z_aarch64_emit_ret(text);
}

static void a64_emit_void_result(ZBuf *text, const IrFunction *fun) {
  if (fun && fun->return_type == IR_TYPE_VOID) z_aarch64_emit_movz_w(text, 0, 0);
}

static bool a64_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag);

// Dedicated scratch slots near the top of the fixed 32-slot scratch region used to spill
// values (fd, addr/result, size) across the syscall sequence in the mmap helpers. They sit above
// the slots expression evaluation grows into from base 0 (path lowering uses slot 0), so a path
// argument can be lowered without clobbering them. Three distinct slots are needed: fd, size, and
// addr each survive a syscall. Mirrors macho64's MMAP scratch convention (slots 29/30/31).
enum {
  A64_DIRECT_MMAP_SCRATCH_FD = 29u,
  A64_DIRECT_MMAP_SCRATCH_SIZE = 30u,
  A64_DIRECT_MMAP_SCRATCH_ADDR = 31u
};

static void a64_emit_spill_x(ZBuf *text, unsigned reg, unsigned slot) {
  z_aarch64_emit_store_x_sp(text, reg, slot * A64_DIRECT_SCRATCH_SLOT_BYTES);
}
static void a64_emit_reload_x(ZBuf *text, unsigned reg, unsigned slot) {
  z_aarch64_emit_load_x_sp(text, reg, slot * A64_DIRECT_SCRATCH_SLOT_BYTES);
}

// Zero the Maybe<byte-view> at `local_index`: has@0 = 0, ptr@8 = 0, len@16 = 0. Mirrors the host
// macho64 inline-clear sequence used by the mmap failure paths.
static void a64_emit_maybe_byte_view_clear(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned frame_size) {
  z_aarch64_emit_movz_w(text, 8, 0);
  a64_emit_store_local_w(text, fun, 8, local_index, 0, frame_size);
  a64_emit_store_local_x(text, fun, 8, local_index, 8, frame_size);
  a64_emit_store_local_x(text, fun, 8, local_index, 16, frame_size);
}

// Open a file by path, measure its size, and map it read-only via raw Linux syscalls. On return x0
// holds the mapping address (negative on any failure: open / lseek / mmap), x1 the byte length
// (valid only on the success path). The fd is always closed (the mapping survives the close).
// Linux aarch64 syscall ABI: args x0..x5, num x8, trap svc 0, result x0 (negative = -errno). Syscall
// numbers (arch/arm64/include/uapi/asm/unistd.h): openat=56, lseek=62, mmap=222, close=57.
// Mirrors emit_elf64.c's elf_emit_mmap_file_addr_size (raw syscalls path, same Linux constants).
static bool a64_emit_mmap_file_addr_size(ZBuf *text, const IrFunction *fun, const IrValue *path, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  // openat(AT_FDCWD=-100, path, O_RDONLY=0, 0)
  if (!a64_emit_byte_view_ptr_at(text, fun, path, 1, frame_size, 0, ctx, diag)) return false; // x1 = path
  z_aarch64_emit_movn_x(text, 0, 99);                          // x0 = AT_FDCWD = ~99 = -100
  z_aarch64_emit_movz_x(text, 2, 0);                           // x2 = O_RDONLY
  z_aarch64_emit_movz_x(text, 3, 0);                           // x3 = mode
  z_aarch64_emit_movz_x(text, 8, 56);                          // x8 = openat
  z_aarch64_emit_svc(text, 0);                                  // x0 = fd (negative = -errno)
  z_aarch64_emit_cmp_x(text, 0, 31);
  size_t open_fail = z_aarch64_emit_b_cond_placeholder(text, 11); // LT: negative fd -> open failure
  a64_emit_spill_x(text, 0, A64_DIRECT_MMAP_SCRATCH_FD);        // save fd
  // lseek(fd, 0, SEEK_END=2) -> file size
  z_aarch64_emit_movz_x(text, 1, 0);                           // x1 = offset = 0
  z_aarch64_emit_movz_x(text, 2, 2);                           // x2 = SEEK_END
  z_aarch64_emit_movz_x(text, 8, 62);                          // x8 = lseek
  z_aarch64_emit_svc(text, 0);                                  // x0 = size (negative = -errno)
  z_aarch64_emit_cmp_x(text, 0, 31);
  size_t seek_fail = z_aarch64_emit_b_cond_placeholder(text, 11); // LT: lseek error
  a64_emit_spill_x(text, 0, A64_DIRECT_MMAP_SCRATCH_SIZE);     // save size
  // mmap(NULL, size, PROT_READ=1, MAP_PRIVATE=2, fd, 0)
  z_aarch64_emit_movz_x(text, 0, 0);                           // x0 = addr = NULL (kernel chooses)
  a64_emit_reload_x(text, 1, A64_DIRECT_MMAP_SCRATCH_SIZE);    // x1 = len = size
  z_aarch64_emit_movz_x(text, 2, 1);                           // x2 = prot = PROT_READ
  z_aarch64_emit_movz_x(text, 3, 2);                           // x3 = flags = MAP_PRIVATE
  a64_emit_reload_x(text, 4, A64_DIRECT_MMAP_SCRATCH_FD);      // x4 = fd
  z_aarch64_emit_movz_x(text, 5, 0);                           // x5 = offset = 0
  z_aarch64_emit_movz_x(text, 8, 222);                         // x8 = mmap
  z_aarch64_emit_svc(text, 0);                                  // x0 = addr (negative = MAP_FAILED)
  // Close the fd on both the success and MAP_FAILED paths; preserve addr across the close, then hand
  // addr back in x0 and size in x1. A MAP_FAILED (negative) addr flows through unchanged.
  a64_emit_spill_x(text, 0, A64_DIRECT_MMAP_SCRATCH_ADDR);     // save addr (or negative)
  a64_emit_reload_x(text, 0, A64_DIRECT_MMAP_SCRATCH_FD);      // x0 = fd
  z_aarch64_emit_movz_x(text, 8, 57);                          // x8 = close
  z_aarch64_emit_svc(text, 0);
  a64_emit_reload_x(text, 0, A64_DIRECT_MMAP_SCRATCH_ADDR);    // x0 = addr (result)
  a64_emit_reload_x(text, 1, A64_DIRECT_MMAP_SCRATCH_SIZE);    // x1 = size (result)
  size_t done = z_aarch64_emit_b_placeholder(text);
  // Seek failure: close the fd, preserve the negative lseek result across the close, then return it
  // in x0. open_fail lands here with x0 already holding the negative open result (no spill needed).
  z_aarch64_patch_cond19(text, seek_fail, text->len);
  a64_emit_spill_x(text, 0, A64_DIRECT_MMAP_SCRATCH_ADDR);     // preserve negative result
  a64_emit_reload_x(text, 0, A64_DIRECT_MMAP_SCRATCH_FD);      // x0 = fd
  z_aarch64_emit_movz_x(text, 8, 57);                          // x8 = close
  z_aarch64_emit_svc(text, 0);
  a64_emit_reload_x(text, 0, A64_DIRECT_MMAP_SCRATCH_ADDR);    // x0 = negative result
  z_aarch64_patch_cond19(text, open_fail, text->len);
  z_aarch64_patch_branch26(text, done, text->len);
  return true;
}

// Anonymous std.mem.pageAlloc allocation: a fresh kernel-zeroed region via a raw mmap syscall
// (calloc semantics; Linux MAP_ANON|MAP_PRIVATE = 0x20 | 0x2 = 0x22; PROT_READ|PROT_WRITE = 3;
// fd = -1; offset = 0). The size value is evaluated then spilled so it survives the syscall, the
// args are staged in x0..x5, and the result populates the Maybe<MutSpan<u8>> dest local: on success
// has=1@0, ptr@8, len@16 (64-bit byte count); on failure (a negative return) the Maybe is cleared.
// Mirrors emit_elf64.c's elf_emit_anon_mmap_to_local (Linux constants, raw syscall path).
static bool a64_emit_anon_mmap_to_local(ZBuf *text, const IrFunction *fun, const IrValue *size, unsigned local_index, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!size) return a64_diag(diag, "direct AArch64 page allocation requires a byte length", 1, 1, "missing length");
  if (!a64_emit_value_to_reg_at(text, fun, size, 9, frame_size, 0, ctx, diag)) return false;
  a64_emit_spill_x(text, 9, A64_DIRECT_MMAP_SCRATCH_SIZE);     // preserve the length across the syscall
  z_aarch64_emit_mov_x(text, 1, 9);                            // x1 = len
  z_aarch64_emit_movz_x(text, 0, 0);                           // x0 = addr (NULL: kernel chooses)
  z_aarch64_emit_movz_x(text, 2, 3);                           // x2 = prot = PROT_READ|PROT_WRITE
  z_aarch64_emit_movz_x(text, 3, 0x22);                        // x3 = flags = MAP_ANON|MAP_PRIVATE (Linux)
  z_aarch64_emit_movn_x(text, 4, 0);                           // x4 = fd = -1
  z_aarch64_emit_movz_x(text, 5, 0);                           // x5 = offset = 0
  z_aarch64_emit_movz_x(text, 8, 222);                         // x8 = mmap
  z_aarch64_emit_svc(text, 0);                                  // x0 = addr (negative = MAP_FAILED)
  z_aarch64_emit_cmp_x(text, 0, 31);
  size_t fail = z_aarch64_emit_b_cond_placeholder(text, 11);    // LT: signed-less-than -> failure
  z_aarch64_emit_movz_w(text, 9, 1);
  a64_emit_store_local_w(text, fun, 9, local_index, 0, frame_size);  // has = 1
  a64_emit_store_local_x(text, fun, 0, local_index, 8, frame_size);  // ptr
  a64_emit_reload_x(text, 9, A64_DIRECT_MMAP_SCRATCH_SIZE);
  a64_emit_store_local_x(text, fun, 9, local_index, 16, frame_size); // len (64-bit byte count)
  size_t end = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, fail, text->len);
  a64_emit_maybe_byte_view_clear(text, fun, local_index, frame_size);
  z_aarch64_patch_branch26(text, end, text->len);
  return true;
}

// `let m = std.fs.mmap(fs, path)` -> Maybe<owned<Mapping>> (has@0, ptr@8, len@16 64-bit byte count).
// The helper lands addr in x0 and the 64-bit byte length in x1; a negative addr is the
// not-found/failure path, which clears the Maybe. Mirrors elf_emit_fs_mmap_to_local.
static bool a64_emit_fs_mmap_to_local(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned local_index, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!a64_emit_mmap_file_addr_size(text, fun, instr->value->left, frame_size, ctx, diag)) return false;
  z_aarch64_emit_cmp_x(text, 0, 31);
  size_t fail = z_aarch64_emit_b_cond_placeholder(text, 11);    // LT: MAP_FAILED / open failure
  z_aarch64_emit_mov_x(text, 9, 0);                            // preserve addr across the has store
  z_aarch64_emit_movz_w(text, 8, 1);
  a64_emit_store_local_w(text, fun, 8, local_index, 0, frame_size);  // has = 1
  a64_emit_store_local_x(text, fun, 9, local_index, 8, frame_size);  // ptr
  a64_emit_store_local_x(text, fun, 1, local_index, 16, frame_size); // len (64-bit byte count, from x1)
  size_t end = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, fail, text->len);
  a64_emit_maybe_byte_view_clear(text, fun, local_index, frame_size);
  z_aarch64_patch_branch26(text, end, text->len);
  return true;
}

// FixedBufAlloc init {ptr@0 i64, capacity@8 u32, position@12 u32 = 0}. The backing buffer
// is a Span<u8>; its ptr/len are stamped into the allocator slot. PageAlloc carries no
// pre-reserved buffer (each std.mem.allocBytes performs a fresh anonymous mmap via
// a64_emit_anon_mmap_to_local). Zero the allocator slot so it holds no stale pointer/length.
static bool a64_emit_alloc_local_set(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (instr->value && instr->value->kind == IR_VALUE_PAGE_ALLOC) {
    z_aarch64_emit_movz_x(text, 8, 0);
    a64_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
    a64_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
    (void)ctx;
    (void)diag;
    return true;
  }
  if (!instr->value || instr->value->kind != IR_VALUE_FIXED_BUF_ALLOC) {
    return a64_diag(diag, "direct AArch64 FixedBufAlloc local requires std.mem.fixedBufAlloc", instr->line, instr->column, "unsupported allocator initializer");
  }
  if (!a64_emit_byte_view_ptr_at(text, fun, instr->value->left, 8, frame_size, 0, ctx, diag)) return false;
  a64_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
  if (!a64_emit_byte_view_len_at(text, fun, instr->value->left, 8, frame_size, 0, ctx, diag)) return false;
  a64_emit_store_local_w(text, fun, 8, instr->local_index, 8, frame_size);
  z_aarch64_emit_movz_w(text, 8, 0);
  a64_emit_store_local_w(text, fun, 8, instr->local_index, 12, frame_size);
  return true;
}

// `let r = std.mem.allocBytes(alloc, n)` -> Maybe<MutSpan<u8>>. FixedBufAlloc bump with
// overflow detection (cmp new_pos with capacity; on overflow clear the Maybe, else write has=1 +
// ptr=alloc.ptr+position + len=n and bump alloc.position). PageAlloc allocBytes performs a
// fresh anonymous mmap (calloc semantics) via a64_emit_anon_mmap_to_local. Mirrors the host macho64
// path exactly (the overflow-detection variant); elf64's same emitter omits the overflow check
// (pre-existing gap, deferred).
static bool a64_emit_alloc_bytes_to_local(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  const IrValue *value = instr->value;
  if (!value || value->kind != IR_VALUE_ALLOC_BYTES || value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_ALLOC) {
    return a64_diag(diag, "direct AArch64 allocation source is invalid", instr->line, instr->column, "invalid allocation");
  }
  if (fun->locals[value->local_index].is_page_alloc) {
    return a64_emit_anon_mmap_to_local(text, fun, value->left, instr->local_index, frame_size, ctx, diag);
  }
  if (!a64_emit_value_to_reg_at(text, fun, value->left, 10, frame_size, 0, ctx, diag)) return false; // x10 = n
  a64_emit_load_local_w(text, fun, 8, value->local_index, 12, frame_size);                            // x8  = position
  a64_emit_load_local_w(text, fun, 9, value->local_index, 8, frame_size);                             // x9  = capacity
  z_aarch64_emit_add_w_imm(text, 11, 8, 0);                                                            // x11 = position (preserved for new_pos calc)
  a64_emit_binary_reg(text, IR_BIN_ADD, 11, 11, 10, false);                                            // x11 = position + n (new_pos, 32-bit)
  z_aarch64_emit_cmp_w(text, 11, 9);                                                                   // cmp new_pos, capacity
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 9);                                        // LS: new_pos <= capacity → ok
  // Overflow path: clear the Maybe and skip the commit.
  z_aarch64_emit_movz_w(text, 8, 0);
  a64_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
  a64_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
  a64_emit_store_local_x(text, fun, 8, instr->local_index, 16, frame_size);
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  // Success path.
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  z_aarch64_emit_movz_w(text, 12, 1);
  a64_emit_store_local_w(text, fun, 12, instr->local_index, 0, frame_size);                            // Maybe.has = 1
  a64_emit_load_local_x(text, fun, 12, value->local_index, 0, frame_size);                             // x12 = alloc.ptr
  z_aarch64_emit_add_x_reg(text, 12, 12, 8);                                                            // x12 = alloc.ptr + position
  a64_emit_store_local_x(text, fun, 12, instr->local_index, 8, frame_size);                            // Maybe.ptr
  a64_emit_store_local_x(text, fun, 10, instr->local_index, 16, frame_size);                           // Maybe.len = n (64-bit)
  a64_emit_store_local_w(text, fun, 11, value->local_index, 12, frame_size);                           // alloc.position = new_pos
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

// Lower `let s = std.args.get(i)` into the Maybe<Span<u8>> local. argc is in x20 and
// argv (the kernel-pushed char**) is in x21 (the prologue seeded them). Evaluate the index into
// w10, bounds-check against argc; out of range writes has=0 + cleared ptr/len. In range loads
// argv[i] into x12, walks bytes until NUL to compute the C-string length (libc-style), then
// writes has=1 + ptr=x12 + len=count. Mirrors macho_emit_args_get_to_local under the same AAPCS.
static bool a64_emit_args_get_to_local(ZBuf *text, const IrFunction *fun, const IrValue *value, const IrLocal *local, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!value || !value->left) return a64_diag(diag, "direct AArch64 std.args.get requires an index", value ? value->line : 1, value ? value->column : 1, "missing index");
  if (!a64_emit_value_to_reg_at(text, fun, value->left, 10, frame_size, 0, ctx, diag)) return false;
  z_aarch64_emit_cmp_w(text, 10, 20);
  size_t in_range = z_aarch64_emit_b_cond_placeholder(text, 3); // unsigned lower
  z_aarch64_emit_movz_w(text, 8, 0);
  a64_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);
  a64_emit_store_local_x(text, fun, 8, local->index, 8, frame_size);
  a64_emit_store_local_x(text, fun, 8, local->index, 16, frame_size);
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, in_range, text->len);

  z_aarch64_emit_add_x_reg_lsl(text, 12, 21, 10, 3); // x12 = argv + i*8
  z_aarch64_emit_load_x_imm(text, 12, 12, 0);        // x12 = argv[i]
  z_aarch64_emit_movz_w(text, 10, 0);                // w10 = strlen counter
  size_t loop_start = text->len;
  z_aarch64_emit_add_x_reg(text, 13, 12, 10);
  z_aarch64_emit_load_b_imm(text, 14, 13, 0);
  size_t done_patch = z_aarch64_emit_cbz_w_placeholder(text, 14);
  z_aarch64_emit_add_w_imm(text, 10, 10, 1);
  size_t loop_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_branch26(text, loop_patch, loop_start);
  z_aarch64_patch_cond19(text, done_patch, text->len);

  z_aarch64_emit_movz_w(text, 8, 1);
  a64_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);
  a64_emit_store_local_x(text, fun, 12, local->index, 8, frame_size);
  a64_emit_store_local_x(text, fun, 10, local->index, 16, frame_size);
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

// Dispatch a Maybe<byte-view> local initializer. ALLOC_BYTES (FixedBufAlloc bump or
// PageAlloc anon mmap), FS_MMAP (file-backed mmap via raw syscalls), and ARGS_GET
// (std.args.get → Maybe<Span<u8>>) are supported; ENV_GET / FS_READ_ALL / FS_TEMP_NAME are
// out of scope.
static bool a64_emit_maybe_byte_view_local_set(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (instr->value && instr->value->kind == IR_VALUE_ARGS_GET) {
    return a64_emit_args_get_to_local(text, fun, instr->value, &fun->locals[instr->local_index], frame_size, ctx, diag);
  }
  if (instr->value && instr->value->kind == IR_VALUE_ALLOC_BYTES) return a64_emit_alloc_bytes_to_local(text, fun, instr, frame_size, ctx, diag);
  if (instr->value && instr->value->kind == IR_VALUE_FS_MMAP) return a64_emit_fs_mmap_to_local(text, fun, instr, instr->local_index, frame_size, ctx, diag);
  (void)ctx;
  return a64_diag(diag, "direct AArch64 Maybe<byte-view> initializer is unsupported", instr->line, instr->column, "unsupported maybe-byte-view initializer");
}

// Dispatch a Maybe<scalar> local initializer. Supports MAYBE_SCALAR_LITERAL (e.g.
// `Maybe<i32> null`) and the plain fallible-scalar-call path (value lands in x0 with the error
// tag in x0[32:63]). JSON_PARSE_BYTES is out of scope.
static bool a64_emit_maybe_scalar_local_set(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!instr->value) return a64_diag(diag, "direct AArch64 Maybe scalar initializer is missing", instr->line, instr->column, "missing maybe value");
  if (instr->value->kind == IR_VALUE_MAYBE_SCALAR_LITERAL) {
    // {has = data_len != 0, value = int_value}. `Maybe<T> null` has data_len = 0; an `optional`
    // (some) literal has data_len = 1.
    z_aarch64_emit_movz_w(text, 8, instr->value->data_len ? 1u : 0u);
    a64_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
    z_aarch64_emit_movz_x(text, 8, (uint64_t)instr->value->int_value);
    a64_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
    return true;
  }
  // Plain fallible-scalar-call: the call's packed result lands in x0 — payload in low 32, error tag
  // in high 32. Extract the tag via a64_emit_error_condition_reg (writes x8 = tag, sets NZCV), then
  // branch: EQ (tag == 0) → success path (has=1 + value=x0 full 64-bit), else failure clears the
  // Maybe. x0 holds the full 64-bit payload on the success path, so we can store directly without
  // needing a stack save/restore (unlike x86_64 where the tag-check helper clobbers rax).
  if (!a64_emit_value_to_reg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
  a64_emit_error_condition_reg(text, 8, 0);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 0); // EQ: tag == 0 → success
  // Failure: clear has + value.
  z_aarch64_emit_movz_w(text, 9, 0);
  a64_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
  a64_emit_store_local_x(text, fun, 9, instr->local_index, 8, frame_size);
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  // Success.
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  z_aarch64_emit_movz_w(text, 9, 1);
  a64_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
  a64_emit_store_local_x(text, fun, 0, instr->local_index, 8, frame_size);
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

// Record return: common path + ret check + ret rescue. Extracted from a64_emit_instr
// to keep that function under the 120-line budget; mirrors macho_emit_instr's IR_INSTR_RETURN
// record branch verbatim modulo namespace.
static bool a64_emit_record_return(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  const IrValue *call_for_check = NULL;
  const IrValue *rescue_fallback = NULL;
  if (instr->value->kind == IR_VALUE_CHECK && instr->value->left && instr->value->left->kind == IR_VALUE_CALL) {
    call_for_check = instr->value->left;
  } else if (instr->value->kind == IR_VALUE_RESCUE && instr->value->left && instr->value->left->kind == IR_VALUE_CALL) {
    call_for_check = instr->value->left;
    rescue_fallback = instr->value->right;
  }
  bool restore_process_args = a64_function_seeds_process_args(fun, ctx);
  if (call_for_check) {
    if (!a64_emit_record_call_with_dest(text, fun, -1, call_for_check, frame_size, 0, ctx, diag)) return false;
    if (!rescue_fallback) {
      // CHECK path: x1 carries inner call's tag. Reload x0 (sret pointer) and epilogue —
      // on success x1 is 0 (set by inner callee), on failure x1 propagates.
      z_aarch64_emit_load_x_imm(text, 0, 31, a64_sret_slot_offset(fun));
      a64_emit_epilogue(text, frame_size, restore_process_args);
      return true;
    }
    // RESCUE path: on success (x1==0), reload x0 and return. On failure, materialize
    // fallback into our sret target, clear x1 (we're succeeding now), then return.
    z_aarch64_emit_cmp_x(text, 1, 31);
    size_t fallback_patch = z_aarch64_emit_b_cond_placeholder(text, 1); // b.ne fallback
    z_aarch64_emit_load_x_imm(text, 0, 31, a64_sret_slot_offset(fun));
    if (fun->raises) z_aarch64_emit_movz_w(text, 1, 0);
    a64_emit_epilogue(text, frame_size, restore_process_args);
    z_aarch64_patch_cond19(text, fallback_patch, text->len);
    if (rescue_fallback->kind == IR_VALUE_LOCAL) {
      if (rescue_fallback->local_index >= fun->local_len) {
        return a64_diag(diag, "direct AArch64 record return rescue fallback local is out of range", rescue_fallback->line, rescue_fallback->column, "invalid fallback local");
      }
      a64_emit_record_copy_to(text, fun, UINT_MAX, rescue_fallback->local_index, frame_size);
    } else if (rescue_fallback->kind == IR_VALUE_CALL) {
      if (!a64_emit_record_call_with_dest(text, fun, -1, rescue_fallback, frame_size, 0, ctx, diag)) return false;
    } else {
      return a64_diag(diag, "direct AArch64 record return rescue fallback must be a record local or call", rescue_fallback->line, rescue_fallback->column, "unsupported rescue fallback");
    }
    z_aarch64_emit_load_x_imm(text, 0, 31, a64_sret_slot_offset(fun));
    if (fun->raises) z_aarch64_emit_movz_w(text, 1, 0);
    a64_emit_epilogue(text, frame_size, restore_process_args);
    return true;
  }
  if (instr->value->kind == IR_VALUE_CALL) {
    if (!a64_emit_record_call_with_dest(text, fun, -1, instr->value, frame_size, 0, ctx, diag)) return false;
  } else if (instr->value->kind == IR_VALUE_LOCAL) {
    a64_emit_record_copy_to(text, fun, UINT_MAX, instr->value->local_index, frame_size);
  } else {
    return a64_diag(diag, "direct AArch64 record return must be a record local or call", instr->line, instr->column, "unsupported record return");
  }
  // Return the sret pointer in x0 (AAPCS indirect-result convention also defines x0 = result on
  // return from a record-returning function); reload it from the saved slot.
  z_aarch64_emit_load_x_imm(text, 0, 31, a64_sret_slot_offset(fun));
  // A raising record-returning function uses x1 as the error tag carrier; clear it on
  // a successful return so callers see tag=0. (RAISE writes the error code to x1 directly.)
  if (fun->raises) z_aarch64_emit_movz_w(text, 1, 0);
  a64_emit_epilogue(text, frame_size, restore_process_args);
  return true;
}

// `let q = check f()` where f returns a record and raises. Issue the record-returning
// call (writes the record via sret into q's slot AND writes the tag to x1: 0 on success, error
// code on failure). Then test x1: on success fall through; on failure propagate. The record
// buffer is undefined on failure — the caller's CHECK semantics ensure it isn't read.
// Mirrors macho_emit_local_set_record_check + a64_emit_check_to_reg_at combined.
static bool a64_emit_local_set_record_check(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  const IrValue *check = instr->value;
  if (!check->left || check->left->kind != IR_VALUE_CALL || check->left->type != IR_TYPE_RECORD) {
    return a64_diag(diag, "direct AArch64 record check requires a record-returning fallible call", check->line, check->column, "unsupported record check");
  }
  if (!a64_emit_record_call_with_dest(text, fun, (int)fun->locals[instr->local_index].index, check->left, frame_size, 0, ctx, diag)) return false;
  // Test the tag in x1 (low 32 bits carry the error code). On zero fall through.
  z_aarch64_emit_cmp_x(text, 1, 31);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 0); // b.eq ok
  if (a64_function_propagates_to_process_exit(fun)) {
    // Propagation by caller-return shape:
    //  - We return a record AND raise: x1 already carries the tag; reload x0 from the sret slot
    //    so the caller sees its valid pointer alongside non-zero x1. Epilogue doesn't touch x1.
    //  - We are hosted-main (main with i32 ret + Void value): the ELF aarch64 `_start` stub does
    //    `bl main; movz x8, 93; svc 0` — kernel reads x0 as the exit code. Route the tag from
    //    x1 into x0 so a propagated error surfaces as a non-zero exit status.
    //  - We are a non-record raising fn: the packed-tag ABI puts the tag in x0's HIGH 32 bits.
    //    Compose x0 = (x1 << 32) using ubfiz x0, x1, #32, #32 (ubfm x0, x1, #32, #31): bits
    //    [31:0] of x1 → bits [63:32] of x0. sf=1 opc=10 N=1 immr=32 imms=31 Rn=1 Rd=0 = 0xD360_7C20.
    if (fun->return_type == IR_TYPE_RECORD) {
      z_aarch64_emit_load_x_imm(text, 0, 31, a64_sret_slot_offset(fun));
    } else if (a64_is_main_function(fun)) {
      z_aarch64_emit_mov_x(text, 0, 1);
    } else {
      z_aarch64_append_u32(text, 0xd3607c20u);
    }
    a64_emit_epilogue(text, frame_size, a64_function_seeds_process_args(fun, ctx));
  } else {
    // Non-propagating context shouldn't reach here (buildability rejects CHECK in non-fallible
    // contexts); defensive exit-1 + epilogue.
    z_aarch64_emit_movz_w(text, 0, 1);
    a64_emit_epilogue(text, frame_size, a64_function_seeds_process_args(fun, ctx));
  }
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  return true;
}

// `let q = f() rescue r0` where f returns a record and raises. Issue the call with q as
// sret target; on success (x1 == 0) the record is already in q. On failure, materialize the
// fallback record into q — either via a64_emit_record_copy_to (fallback is a record local) or
// another record-returning call (fallback is itself a call).
// Mirrors macho_emit_local_set_record_rescue + a64_emit_rescue_to_reg_at combined.
static bool a64_emit_local_set_record_rescue(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  const IrValue *rescue = instr->value;
  const IrLocal *dest = &fun->locals[instr->local_index];
  if (!rescue->left || rescue->left->kind != IR_VALUE_CALL || rescue->left->type != IR_TYPE_RECORD) {
    return a64_diag(diag, "direct AArch64 record rescue requires a record-returning fallible call", rescue->line, rescue->column, "unsupported record rescue");
  }
  if (!rescue->right || (rescue->right->kind != IR_VALUE_LOCAL && rescue->right->kind != IR_VALUE_CALL)) {
    return a64_diag(diag, "direct AArch64 record rescue fallback must be a record local or call", rescue->line, rescue->column, "unsupported rescue fallback");
  }
  if (!a64_emit_record_call_with_dest(text, fun, (int)dest->index, rescue->left, frame_size, 0, ctx, diag)) return false;
  z_aarch64_emit_cmp_x(text, 1, 31);
  size_t fallback_patch = z_aarch64_emit_b_cond_placeholder(text, 1); // b.ne fallback
  size_t end_patch = z_aarch64_emit_b_placeholder(text); // success: skip fallback
  z_aarch64_patch_cond19(text, fallback_patch, text->len);
  if (rescue->right->kind == IR_VALUE_LOCAL) {
    if (rescue->right->local_index >= fun->local_len) {
      return a64_diag(diag, "direct AArch64 record rescue fallback local is out of range", rescue->right->line, rescue->right->column, "invalid fallback local");
    }
    a64_emit_record_copy_to(text, fun, dest->index, rescue->right->local_index, frame_size);
  } else {
    if (!a64_emit_record_call_with_dest(text, fun, (int)dest->index, rescue->right, frame_size, 0, ctx, diag)) return false;
  }
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

static bool a64_emit_local_set(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (instr->local_index >= fun->local_len) return a64_diag(diag, "direct AArch64 local store is out of range", instr->line, instr->column, "invalid local");
  const IrLocal *local = &fun->locals[instr->local_index];
  if (local->is_record) {
    // `let q = check f()` and `let q = f() rescue r0` — fallible record-returning calls.
    if (instr->value && instr->value->kind == IR_VALUE_CHECK) {
      return a64_emit_local_set_record_check(text, fun, instr, frame_size, ctx, diag);
    }
    if (instr->value && instr->value->kind == IR_VALUE_RESCUE) {
      return a64_emit_local_set_record_rescue(text, fun, instr, frame_size, ctx, diag);
    }
    // `let q = f()` — bind a record-returning call straight into q's slot via sret (no copy).
    if (instr->value && instr->value->kind == IR_VALUE_CALL) {
      return a64_emit_record_call_with_dest(text, fun, (int)local->index, instr->value, frame_size, 0, ctx, diag);
    }
    // `let q = p` / `q = p` — record-to-record value copy (span fields ride along as raw bytes).
    if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
      a64_emit_record_copy_to(text, fun, local->index, instr->value->local_index, frame_size);
      return true;
    }
    return a64_diag(diag, "direct AArch64 record local assignment requires a record value", instr->line, instr->column, "unsupported record set");
  }
  if (local->type == IR_TYPE_BYTE_VIEW) {
    // A span-returning call leaves ptr in x0 and len in x1; store both straight into the slot.
    if (instr->value && instr->value->kind == IR_VALUE_CALL) {
      if (!a64_emit_call_to_reg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
      a64_emit_store_local_x(text, fun, 0, instr->local_index, 0, frame_size);
      a64_emit_store_local_x(text, fun, 1, instr->local_index, 8, frame_size);
      return true;
    }
    if (!a64_emit_byte_view_ptr_at(text, fun, instr->value, 8, frame_size, 0, ctx, diag)) return false;
    a64_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
    if (!a64_emit_byte_view_len_at(text, fun, instr->value, 8, frame_size, 0, ctx, diag)) return false;
    a64_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
    return true;
  }
  // Maybe/allocator subsystem.
  if (local->type == IR_TYPE_ALLOC) return a64_emit_alloc_local_set(text, fun, instr, frame_size, ctx, diag);
  if (local->type == IR_TYPE_MAYBE_BYTE_VIEW) return a64_emit_maybe_byte_view_local_set(text, fun, instr, frame_size, ctx, diag);
  if (local->type == IR_TYPE_MAYBE_SCALAR) return a64_emit_maybe_scalar_local_set(text, fun, instr, frame_size, ctx, diag);
  if (a64_type_is_float(local->type)) {
    bool is64 = a64_type_is_f64(local->type);
    if (!a64_emit_float_value_to_vreg_at(text, fun, instr->value, 8, frame_size, 0, ctx, diag)) return false;
    a64_emit_store_local_v(text, fun, 8, instr->local_index, 0, is64, frame_size);
    return true;
  }
  if (!a64_emit_value_to_reg_at(text, fun, instr->value, 8, frame_size, 0, ctx, diag)) return false;
  if (a64_type_is_scalar64(local->type)) a64_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
  else a64_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
  return true;
}

// Field store for a record local: write `instr->value` to `&local + field_offset`. Dispatches on
// the value's type — span fields take two stores (ptr@offset, 64-bit len@offset+8); float fields
// route through the FP emitter then STR s/d; int fields use scalar STR widths matching the field.
static bool a64_emit_field_store(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (instr->local_index == UINT_MAX) {
    // The record being returned, written through the saved sret pointer (`return <shape literal>`
    // stores its fields this way without a temporary local).
    return a64_emit_sret_field_store(text, fun, instr, frame_size, ctx, diag);
  }
  if (instr->local_index >= fun->local_len) return a64_diag(diag, "direct AArch64 field store record is out of range", instr->line, instr->column, "invalid record local");
  if (!fun->locals[instr->local_index].is_record) return a64_diag(diag, "direct AArch64 field store requires record local", instr->line, instr->column, "non-record local");
  // Field store through a ref<Record> (non-mutable) is rejected — `set p.field …`
  // requires mutref<Record>.
  if (fun->locals[instr->local_index].is_ref && !fun->locals[instr->local_index].is_mutable) {
    return a64_diag(diag, "direct AArch64 field store through ref<Record> requires mutref", instr->line, instr->column, fun->locals[instr->local_index].name ? fun->locals[instr->local_index].name : "ref-record");
  }
  bool target_is_ref = fun->locals[instr->local_index].is_ref;
  if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
    // Span field: store ptr at the field offset and the 64-bit byte/element-count len 8 bytes
    // higher. A span-returning call leaves ptr in x0 and len in x1; any other byte view materializes
    // via the ptr/len helpers.
    if (instr->value->kind == IR_VALUE_CALL) {
      if (!a64_emit_call_to_reg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
      if (target_is_ref) {
        a64_emit_load_ref_record_ptr(text, fun, 9, instr->local_index, frame_size);
        z_aarch64_emit_store_x_imm(text, 0, 9, instr->field_offset);
        z_aarch64_emit_store_x_imm(text, 1, 9, instr->field_offset + 8);
      } else {
        a64_emit_store_local_x(text, fun, 0, instr->local_index, instr->field_offset, frame_size);
        a64_emit_store_local_x(text, fun, 1, instr->local_index, instr->field_offset + 8, frame_size);
      }
      return true;
    }
    if (!a64_emit_byte_view_ptr_at(text, fun, instr->value, 8, frame_size, 0, ctx, diag)) return false;
    if (target_is_ref) {
      a64_emit_load_ref_record_ptr(text, fun, 9, instr->local_index, frame_size);
      z_aarch64_emit_store_x_imm(text, 8, 9, instr->field_offset);
    } else {
      a64_emit_store_local_x(text, fun, 8, instr->local_index, instr->field_offset, frame_size);
    }
    if (!a64_emit_byte_view_len_at(text, fun, instr->value, 8, frame_size, 0, ctx, diag)) return false;
    if (target_is_ref) {
      a64_emit_load_ref_record_ptr(text, fun, 9, instr->local_index, frame_size);
      z_aarch64_emit_store_x_imm(text, 8, 9, instr->field_offset + 8);
    } else {
      a64_emit_store_local_x(text, fun, 8, instr->local_index, instr->field_offset + 8, frame_size);
    }
    return true;
  }
  if (instr->value && a64_type_is_float(instr->value->type)) {
    bool is64 = a64_type_is_f64(instr->value->type);
    if (!a64_emit_float_value_to_vreg_at(text, fun, instr->value, 8, frame_size, 0, ctx, diag)) return false;
    if (target_is_ref) {
      a64_emit_load_ref_record_ptr(text, fun, 9, instr->local_index, frame_size);
      z_aarch64_emit_str_v_off(text, 8, 9, instr->field_offset, is64);
      return true;
    }
    a64_emit_store_local_v(text, fun, 8, instr->local_index, instr->field_offset, is64, frame_size);
    return true;
  }
  if (!a64_emit_value_to_reg_at(text, fun, instr->value, 8, frame_size, 0, ctx, diag)) return false;
  IrTypeKind vt = instr->value ? instr->value->type : IR_TYPE_I32;
  if (target_is_ref) {
    a64_emit_load_ref_record_ptr(text, fun, 9, instr->local_index, frame_size);
    if (vt == IR_TYPE_U8 || vt == IR_TYPE_I8 || vt == IR_TYPE_BOOL) z_aarch64_emit_store_b_imm(text, 8, 9, instr->field_offset);
    else if (a64_type_is_scalar64(vt)) z_aarch64_emit_store_x_imm(text, 8, 9, instr->field_offset);
    else z_aarch64_emit_store_w_imm(text, 8, 9, instr->field_offset);
    return true;
  }
  if (vt == IR_TYPE_U8 || vt == IR_TYPE_I8 || vt == IR_TYPE_BOOL) z_aarch64_emit_store_b_sp(text, 8, a64_local_slot_offset(fun, instr->local_index, instr->field_offset, frame_size));
  else if (a64_type_is_scalar64(vt)) a64_emit_store_local_x(text, fun, 8, instr->local_index, instr->field_offset, frame_size);
  else a64_emit_store_local_w(text, fun, 8, instr->local_index, instr->field_offset, frame_size);
  return true;
}

static bool a64_emit_index_store(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (instr->array_index >= fun->local_len) return a64_diag(diag, "direct AArch64 indexed store array is out of range", instr->line, instr->column, "invalid array local");
  const IrLocal *local = &fun->locals[instr->array_index];
  unsigned const_index = 0;
  if (local->type == IR_TYPE_BYTE_VIEW) {
    // Typed-span element store. The value is materialized before the address so the address
    // scratch (x8/x9) survives. Float elements store via STR s/d; i8/u8 truncate to a byte
    // (STRB); 8-byte elements via STR x; else STR w.
    IrTypeKind elem = local->element_type;
    if (a64_type_is_float(elem)) {
      bool is64 = a64_type_is_f64(elem);
      if (!a64_emit_float_value_to_vreg_at(text, fun, instr->value, 8, frame_size, 0, ctx, diag)) return false;
      if (!a64_emit_span_index_addr(text, fun, instr->array_index, instr->index, frame_size, 0, ctx, diag)) return false;
      z_aarch64_emit_str_v_off(text, 8, 9, 0, is64);
      return true;
    }
    if (!a64_emit_value_to_reg_at(text, fun, instr->value, 10, frame_size, 0, ctx, diag)) return false;
    if (!a64_emit_span_index_addr(text, fun, instr->array_index, instr->index, frame_size, 0, ctx, diag)) return false;
    if (elem == IR_TYPE_U8 || elem == IR_TYPE_BOOL || elem == IR_TYPE_I8) z_aarch64_emit_store_b_imm(text, 10, 9, 0);
    else if (a64_elem_byte_size(elem) == 8) z_aarch64_emit_store_x_imm(text, 10, 9, 0);
    else z_aarch64_emit_store_w_imm(text, 10, 9, 0);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE) &&
      a64_const_u32_value(instr->index, &const_index) && const_index < local->array_len) {
    if (!a64_emit_value_to_reg_at(text, fun, instr->value, 10, frame_size, 0, ctx, diag)) return false;
    a64_emit_store_local_w(text, fun, 10, instr->array_index, const_index * 4u, frame_size);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
    if (!a64_emit_value_to_reg_at(text, fun, instr->value, 10, frame_size, 0, ctx, diag)) return false;
    if (!a64_emit_store_scratch(text, 10, instr->value ? instr->value->type : local->element_type, 0, instr->value, diag)) return false;
    if (!instr->index || !a64_emit_value_to_reg_at(text, fun, instr->index, 8, frame_size, 1, ctx, diag)) return false;
    z_aarch64_emit_movz_w(text, 9, local->array_len);
    a64_emit_u32_bounds_check(text, 8, 9);
    z_aarch64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, instr->array_index, 0, frame_size));
    z_aarch64_emit_add_x_reg_lsl(text, 9, 9, 8, 2);
    if (!a64_emit_load_scratch(text, 10, instr->value ? instr->value->type : local->element_type, 0, instr->value, diag)) return false;
    z_aarch64_emit_store_w_imm(text, 10, 9, 0);
    return true;
  }
  if (!local->is_array || (local->element_type != IR_TYPE_U8 && local->element_type != IR_TYPE_BOOL)) return a64_diag(diag, "direct AArch64 indexed store requires [N]u8, [N]Bool, or integer arrays", instr->line, instr->column, "unsupported array local");
  if (!a64_emit_value_to_reg_at(text, fun, instr->value, 10, frame_size, 0, ctx, diag)) return false;
  if (!a64_emit_store_scratch(text, 10, instr->value ? instr->value->type : local->element_type, 0, instr->value, diag)) return false;
  if (!instr->index || !a64_emit_value_to_reg_at(text, fun, instr->index, 8, frame_size, 1, ctx, diag)) return false;
  z_aarch64_emit_movz_w(text, 9, local->array_len);
  a64_emit_u32_bounds_check(text, 8, 9);
  z_aarch64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, instr->array_index, 0, frame_size));
  z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  if (!a64_emit_load_scratch(text, 10, instr->value ? instr->value->type : local->element_type, 0, instr->value, diag)) return false;
  z_aarch64_emit_store_b_imm(text, 10, 9, 0);
  return true;
}

static bool a64_emit_world_write(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (!instr || !instr->value) return a64_diag(diag, "direct AArch64 World write requires bytes", instr ? instr->line : 1, instr ? instr->column : 1, "missing byte view");
  if (!ctx || !ctx->emit_world_write) return a64_diag(diag, "direct AArch64 World write requires an executable target runtime", instr->line, instr->column, "unsupported instruction");
  if (!a64_emit_byte_view_ptr_at(text, fun, instr->value, 1, frame_size, 0, ctx, diag)) return false;
  if (!a64_emit_byte_view_len_at(text, fun, instr->value, 2, frame_size, 0, ctx, diag)) return false;
  z_aarch64_emit_movz_w(text, 0, instr->field_offset == 2 ? 2u : 1u);
  return ctx->emit_world_write(text, instr, ctx, diag);
}

static bool a64_emit_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  if (instr->kind == IR_INSTR_LOCAL_SET) return a64_emit_local_set(text, fun, instr, frame_size, ctx, diag);
  if (instr->kind == IR_INSTR_INDEX_STORE) return a64_emit_index_store(text, fun, instr, frame_size, ctx, diag);
  if (instr->kind == IR_INSTR_FIELD_STORE) return a64_emit_field_store(text, fun, instr, frame_size, ctx, diag);
  if (instr->kind == IR_INSTR_WORLD_WRITE) return a64_emit_world_write(text, fun, instr, frame_size, ctx, diag);
  if (instr->kind == IR_INSTR_EXPR) return !instr->value || a64_emit_value_to_reg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag);
  bool restore_process_args = a64_function_seeds_process_args(fun, ctx);
  if (instr->kind == IR_INSTR_RETURN) {
    if (instr->value && instr->value->type == IR_TYPE_RECORD) {
      return a64_emit_record_return(text, fun, instr, frame_size, ctx, diag);
    }
    if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
      // Span return: ptr in x0, 64-bit len in x1. A span-returning call passes its result through;
      // a byte_view value lowers via the ptr/len helpers.
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!a64_emit_call_to_reg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
      } else {
        if (!a64_emit_byte_view_ptr_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
        if (!a64_emit_byte_view_len_at(text, fun, instr->value, 1, frame_size, 0, ctx, diag)) return false;
      }
      a64_emit_epilogue(text, frame_size, restore_process_args);
      return true;
    }
    if (instr->value && a64_type_is_float(instr->value->type)) {
      if (!a64_emit_float_value_to_vreg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
      // A raising function returns its float result in v0; x0 is otherwise unused for a float
      // payload, so clear it to flag "no error" (the high-32 tag reads as 0). Integer payloads
      // already carry the tag via their value width because movz_w zero-extends into x0.
      if (fun->raises) z_aarch64_emit_movz_x(text, 0, 0);
    } else if (instr->value && !a64_emit_value_to_reg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) {
      return false;
    }
    // Void-returning raising fns with no explicit value: clear x0 explicitly. a64_emit_void_result
    // also writes movz_w(0,0) for IR_TYPE_VOID returns (zero-extending into x0), which covers the
    // overlapping case, but raising fns may declare non-Void value-return-type while still
    // needing the tag cleared for the success path.
    if (fun->raises && !instr->value) z_aarch64_emit_movz_x(text, 0, 0);
    a64_emit_void_result(text, fun);
    a64_emit_epilogue(text, frame_size, restore_process_args);
    return true;
  }
  if (instr->kind == IR_INSTR_RAISE) {
    // Pack the error code into x0[32:63] then epilogue. The caller's `check` reads the tag from
    // x0's high 32 bits; for main with `!`, the kernel uses x0 as the exit code directly (the tag
    // shifted into the high half does not propagate cleanly to a non-zero exit on Linux because
    // POSIX uses w0, but `check`-driven exit-code 1 still flags failure to the user). Requires a
    // propagating context (`raises` or main with i32 return + void value-return).
    // A raising record-returning function carries the error tag in the low 32 bits of x1
    // (x0 is reserved for the sret pointer per AAPCS). Reload x0 from the sret slot so the caller
    // sees a valid pointer alongside the non-zero tag.
    if (!a64_function_propagates_to_process_exit(fun)) {
      return a64_diag(diag, "direct AArch64 raise requires a fallible function context", instr->line, instr->column, "non-fallible context");
    }
    if (fun->return_type == IR_TYPE_RECORD) {
      z_aarch64_emit_movz_w(text, 1, instr->error_code ? instr->error_code : IR_ERROR_UNKNOWN);
      z_aarch64_emit_load_x_imm(text, 0, 31, a64_sret_slot_offset(fun));
    } else {
      a64_emit_packed_error_reg(text, 0, instr->error_code ? instr->error_code : IR_ERROR_UNKNOWN);
    }
    a64_emit_epilogue(text, frame_size, restore_process_args);
    return true;
  }
  if (instr->kind == IR_INSTR_IF) {
    if (!a64_emit_value_to_reg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
    size_t false_patch = z_aarch64_emit_cbz_w_placeholder(text, 0);
    if (!a64_emit_instrs(text, fun, instr->then_instrs, instr->then_len, frame_size, ctx, diag)) return false;
    if (instr->else_len > 0) {
      size_t end_patch = z_aarch64_emit_b_placeholder(text);
      z_aarch64_patch_cond19(text, false_patch, text->len);
      if (!a64_emit_instrs(text, fun, instr->else_instrs, instr->else_len, frame_size, ctx, diag)) return false;
      z_aarch64_patch_branch26(text, end_patch, text->len);
    } else {
      z_aarch64_patch_cond19(text, false_patch, text->len);
    }
    return true;
  }
  if (instr->kind == IR_INSTR_WHILE) {
    // Trivial `while cond { body }` loop. Evaluate the cond into w0 at the top, exit on
    // zero (cbz_w branch out), emit the body, unconditional branch back to the top. Mirrors
    // macho_emit_instr's WHILE branch verbatim.
    size_t loop_start = text->len;
    if (!a64_emit_value_to_reg_at(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
    size_t false_patch = z_aarch64_emit_cbz_w_placeholder(text, 0);
    if (!a64_emit_instrs(text, fun, instr->then_instrs, instr->then_len, frame_size, ctx, diag)) return false;
    size_t loop_patch = z_aarch64_emit_b_placeholder(text);
    z_aarch64_patch_branch26(text, loop_patch, loop_start);
    z_aarch64_patch_cond19(text, false_patch, text->len);
    return true;
  }
  return a64_diag(diag, "direct AArch64 instruction kind is unsupported", instr->line, instr->column, "unsupported instruction");
}

static bool a64_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, unsigned frame_size, ZAArch64DirectContext *ctx, ZDiag *diag) {
  for (size_t i = 0; i < len; i++) {
    if (!a64_emit_instr(text, fun, &instrs[i], frame_size, ctx, diag)) return false;
  }
  return true;
}

static bool a64_validate_function(const IrFunction *fun, ZDiag *diag) {
  if (!fun) return a64_diag(diag, "direct AArch64 backend requires a function", 1, 1, "missing function");
  // Accept scalar/float/BYTE_VIEW params. RECORD params are handled separately below.
  for (size_t i = 0; i < fun->param_count; i++) {
    const IrLocal *param = &fun->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) continue;
    // Record params arrive as a pointer in one int reg; the prologue copies the bytes into the
    // local's frame slot for value semantics. Compatible with sret returns (which use x8).
    if (param->is_record || param->type == IR_TYPE_RECORD) continue;
    if (!a64_type_is_scalar(param->type) && !a64_type_is_float(param->type)) {
      return a64_diag(diag, "direct AArch64 backend supports only scalar/float/Span<u8>/record parameters", param->line, param->column, param->name);
    }
  }
  // Record + Span returns lower via sret / x0-x1 fat-pointer respectively. Fallible (raises)
  // returns over scalar/float use the packed-tag-in-high-32 ABI to carry the tag in x0.
  // Record-returning raising functions use a different carrier — the tag rides in x1
  // (the AAPCS second return register), which is free for record returns since x0 is the sret
  // pointer per AAPCS indirect-result convention. Span+raises stays rejected (x1 is the span len).
  if (fun->return_type == IR_TYPE_BYTE_VIEW && fun->raises) {
    return a64_diag(diag, "direct AArch64 backend cannot return a Span<u8> from a raising function", fun->line, fun->column, fun->name);
  }
  if (fun->return_type != IR_TYPE_VOID && !a64_type_is_scalar(fun->return_type) && !a64_type_is_float(fun->return_type) &&
      fun->return_type != IR_TYPE_RECORD && fun->return_type != IR_TYPE_BYTE_VIEW) {
    return a64_diag(diag, "direct AArch64 backend supports only Void, scalar, float, record, and Span<u8> returns", fun->line, fun->column, fun->name);
  }
  for (size_t i = 0; i < fun->local_len; i++) {
    const IrLocal *local = &fun->locals[i];
    if (local->type == IR_TYPE_BYTE_VIEW) continue;
    if (local->is_record) continue; // records live in inline frame slots sized by IR layout
    // Maybe/allocator subsystem. PageAlloc (an IR_TYPE_ALLOC with is_page_alloc) routes
    // through the emit-helper diagnostic; the slot reserve itself works the same.
    if (local->type == IR_TYPE_ALLOC || local->type == IR_TYPE_MAYBE_BYTE_VIEW || local->type == IR_TYPE_MAYBE_SCALAR) continue;
    if (local->is_array && (local->element_type == IR_TYPE_U8 || local->element_type == IR_TYPE_BOOL ||
                            local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) continue;
    if (local->is_array || (!a64_type_is_scalar(local->type) && !a64_type_is_float(local->type))) {
      return a64_diag(diag, "direct AArch64 backend supports only primitive scalar/float locals, fixed byte/integer arrays, and records", local->line, local->column, local->name);
    }
  }
  return true;
}

static bool a64_emit_function_text(ZBuf *text, const IrFunction *fun, ZAArch64DirectContext *ctx, ZDiag *diag) {
  uint32_t literal = 0;
  if (a64_return_literal(fun, &literal, NULL)) {
    z_aarch64_emit_literal_return(text, literal);
    return true;
  }
  if (!a64_validate_function(fun, diag)) return false;
  unsigned frame_size = (unsigned)a64_function_frame_bytes(fun);
  // On backends that don't link against a crt0 (the bare ELF aarch64 exe path), the
  // exported `main` is entered straight from `_start` with argc in x0 and argv in x1. Save the
  // callee-saved x20/x21 pair BEFORE the standard fp/lr prologue (pre-indexed sp), then capture
  // argc/argv into x20/x21 after the frame is allocated so std.args.{len,get} can read them
  // anywhere in the body. The matching epilogue restores x20/x21 (post-indexed sp) after fp/lr.
  bool seed_process_args = a64_function_seeds_process_args(fun, ctx);
  if (seed_process_args) z_aarch64_emit_stp_x20_x21_sp_pre16(text);
  z_aarch64_emit_stp_x29_x30_sp_pre16(text);
  z_aarch64_emit_mov_x29_sp(text);
  if (frame_size > 0) z_aarch64_emit_sub_sp_imm(text, frame_size);
  if (seed_process_args) {
    z_aarch64_emit_mov_x(text, 20, 0); // x20 = argc
    z_aarch64_emit_mov_x(text, 21, 1); // x21 = argv
  }
  // A record-returning function gets the destination address in x8 (the AAPCS indirect-result
  // register, not an argument register). Save it so it survives the body's calls; field stores
  // and the return reload it from this slot.
  if (a64_returns_record(fun)) z_aarch64_emit_store_x_imm(text, 8, 31, a64_sret_slot_offset(fun));
  // Spill incoming parameters to their frame slots. Float params arrive in the AAPCS FP bank
  // (V0,V1,..), counted independently of the X0-X7 integer bank. BYTE_VIEW arrives as (ptr in
  // x_n, 64-bit len in x_{n+1}) and is split across the local's two slots (offset 0 = ptr, 8 =
  // len) matching how local-set BYTE_VIEW stores work elsewhere. RECORD arrives as a pointer in
  // x_n; the bytes are copied into the local's frame slot for value semantics.
  unsigned abi_slot = 0;
  unsigned fp_abi_slot = 0;
  for (size_t i = 0; i < fun->param_count; i++) {
    const IrLocal *local = &fun->locals[i];
    if (a64_type_is_float(local->type)) {
      a64_emit_store_local_v(text, fun, fp_abi_slot, (unsigned)i, 0, a64_type_is_f64(local->type), frame_size);
      fp_abi_slot++;
      continue;
    }
    if (local->type == IR_TYPE_BYTE_VIEW) {
      a64_emit_store_local_x(text, fun, abi_slot, (unsigned)i, 0, frame_size);
      a64_emit_store_local_x(text, fun, abi_slot + 1, (unsigned)i, 8, frame_size);
      abi_slot += 2;
      continue;
    }
    if (local->is_record && local->is_ref) {
      // ref<Record>/mutref<Record> param. AAPCS passes a pointer in one int reg; store it
      // in the first 8 bytes of the local's slot. Field load/store go through the pointer (no
      // inline byte copy — that's what differentiates ref-record from by-value record).
      a64_emit_store_local_x(text, fun, abi_slot, (unsigned)i, 0, frame_size);
      abi_slot++;
      continue;
    }
    if (local->is_record) {
      a64_emit_copy_record_param(text, fun, (unsigned)i, abi_slot, frame_size);
      abi_slot++;
      continue;
    }
    if (a64_type_is_scalar64(local->type)) a64_emit_store_local_x(text, fun, abi_slot, (unsigned)i, 0, frame_size);
    else a64_emit_store_local_w(text, fun, abi_slot, (unsigned)i, 0, frame_size);
    abi_slot++;
  }
  if (!a64_emit_instrs(text, fun, fun->instrs, fun->instr_len, frame_size, ctx, diag)) return false;
  if (fun->instr_len == 0 || fun->instrs[fun->instr_len - 1].kind != IR_INSTR_RETURN) {
    a64_emit_void_result(text, fun);
    a64_emit_epilogue(text, frame_size, seed_process_args);
  }
  return true;
}

static const IrFunction *a64_find_main(const IrProgram *ir, unsigned *out_index, ZDiag *diag) {
  const IrFunction *fun = NULL;
  unsigned index = 0;
  for (size_t i = 0; ir && i < ir->function_len; i++) {
    if (ir->functions[i].is_exported && strcmp(ir->functions[i].name, "main") == 0) {
      if (fun) {
        a64_diag(diag, "direct AArch64 executable backend requires exactly one exported main function", ir->functions[i].line, ir->functions[i].column, ir->functions[i].name);
        return NULL;
      }
      fun = &ir->functions[i];
      index = (unsigned)i;
    }
  }
  if (!fun) {
    a64_diag(diag, "direct AArch64 executable backend requires an exported main function", 1, 1, "missing main");
    return NULL;
  }
  if (out_index) *out_index = index;
  return fun;
}

static unsigned a64_rodata_base_offset(const IrProgram *ir) {
  if (!ir || ir->data_segment_len == 0) return 0;
  unsigned base = ir->data_segments[0].offset;
  for (size_t i = 1; i < ir->data_segment_len; i++) {
    if (ir->data_segments[i].offset < base) base = ir->data_segments[i].offset;
  }
  return base;
}

static void a64_append_rodata(ZBuf *rodata, const IrProgram *ir, unsigned base_offset) {
  for (size_t i = 0; ir && i < ir->data_segment_len; i++) {
    const IrDataSegment *segment = &ir->data_segments[i];
    while (rodata->len < segment->offset - base_offset) zbuf_append_char(rodata, 0);
    for (size_t j = 0; j < segment->len; j++) zbuf_append_char(rodata, (char)segment->bytes[j]);
  }
}

bool z_aarch64_direct_emit_function_text(ZBuf *text, const IrFunction *fun, ZAArch64DirectContext *ctx, ZDiag *diag) {
  return a64_emit_function_text(text, fun, ctx, diag);
}

bool z_aarch64_direct_validate_function(const IrFunction *fun, ZDiag *diag) {
  return a64_validate_function(fun, diag);
}

const IrFunction *z_aarch64_direct_find_main(const IrProgram *program, unsigned *out_index, ZDiag *diag) {
  return a64_find_main(program, out_index, diag);
}

unsigned z_aarch64_direct_rodata_base_offset(const IrProgram *program) {
  return a64_rodata_base_offset(program);
}

void z_aarch64_direct_append_rodata(ZBuf *rodata, const IrProgram *program, unsigned base_offset) {
  a64_append_rodata(rodata, program, base_offset);
}
