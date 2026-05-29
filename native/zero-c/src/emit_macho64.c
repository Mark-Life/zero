#include "zero.h"
#include "aarch64_emit.h"
#include "macho_emit_state.h"
#include "macho_format.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void append_u8(ZBuf *buf, unsigned value) {
  zbuf_append_char(buf, (char)(value & 0xffu));
}

#define MACHO_SCRATCH_SLOT_COUNT 32u
#define MACHO_SCRATCH_SLOT_BYTES 8u

static void append_bytes(ZBuf *buf, const char *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) append_u8(buf, (unsigned char)bytes[i]);
}

static bool macho_diag_at(ZDiag *diag, const char *message, int line, int column, const char *actual) {
  if (diag) {
    diag->code = 4004;
    diag->line = line > 0 ? line : 1;
    diag->column = column > 0 ? column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct AArch64 Mach-O object MVP subset");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "unsupported construct");
    snprintf(diag->help, sizeof(diag->help), "choose a supported direct target or restrict this program to exported no-parameter functions returning small integer literals");
  }
  return false;
}

static bool macho_diag(ZDiag *diag, const char *message) {
  return macho_diag_at(diag, message, 1, 1, "unsupported feature");
}

static bool macho_return_literal(const IrFunction *fun, uint32_t *out, ZDiag *diag) {
  if (!fun || fun->param_count != 0) {
    return macho_diag_at(diag, "direct AArch64 Mach-O object backend currently supports exported functions without parameters", fun ? fun->line : 1, fun ? fun->column : 1, fun ? fun->name : "missing function");
  }
  if (fun->return_type != IR_TYPE_U8 && fun->return_type != IR_TYPE_I32 && fun->return_type != IR_TYPE_U32 && fun->return_type != IR_TYPE_USIZE) {
    return macho_diag_at(diag, "direct AArch64 Mach-O object backend currently supports primitive 32-bit-or-smaller integer returns", fun->line, fun->column, fun->name);
  }
  for (size_t i = 0; i < fun->instr_len; i++) {
    const IrInstr *instr = &fun->instrs[i];
    if (instr->kind != IR_INSTR_RETURN || !instr->value || instr->value->kind != IR_VALUE_INT || instr->value->int_value > 65535) continue;
    *out = (uint32_t)instr->value->int_value;
    return true;
  }
  return macho_diag_at(diag, "direct AArch64 Mach-O object backend currently requires a small integer literal return", fun->line, fun->column, fun->name);
}

static bool macho_is_literal_return_function(const IrFunction *fun, uint32_t *out, ZDiag *diag) {
  if (!fun || fun->local_len != 0 || fun->instr_len != 1) return false;
  return macho_return_literal(fun, out, diag);
}

static size_t macho_align(size_t value, size_t alignment) { return z_macho_align(value, alignment); }
static void macho_pad_to(ZBuf *buf, size_t offset) { z_macho_pad_to(buf, offset); }

static bool macho_type_is_scalar32(IrTypeKind type) { return type == IR_TYPE_BOOL || type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_I32 || type == IR_TYPE_U32 || type == IR_TYPE_USIZE; }
static bool macho_type_is_scalar64(IrTypeKind type) { return type == IR_TYPE_I64 || type == IR_TYPE_U64; }
static bool macho_type_is_unsigned(IrTypeKind type) { return type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_USIZE || type == IR_TYPE_U32 || type == IR_TYPE_U64; }
static bool macho_type_is_scalar(IrTypeKind type) { return macho_type_is_scalar32(type) || macho_type_is_scalar64(type); }
static bool macho_type_is_f64(IrTypeKind type) { return type == IR_TYPE_F64; }
static bool macho_type_is_float(IrTypeKind type) { return type == IR_TYPE_F32 || type == IR_TYPE_F64; }

// Byte size and log2 of a typed-span / array element. 1-byte elements (u8/i8/Bool) need no index
// scaling; 2-byte elements (u16) scale by 2; 8-byte elements (i64/u64/f64) scale by 8; everything
// else (i32/u32/usize/f32) by 4. u16 is currently exercised only as a codec read result width.
static unsigned macho_elem_byte_size(IrTypeKind type) {
  switch (type) {
    case IR_TYPE_U8: case IR_TYPE_I8: case IR_TYPE_BOOL: return 1;
    case IR_TYPE_U16: return 2;
    case IR_TYPE_I64: case IR_TYPE_U64: case IR_TYPE_F64: return 8;
    default: return 4;
  }
}
static unsigned macho_elem_log2(IrTypeKind type) {
  switch (type) {
    case IR_TYPE_U8: case IR_TYPE_I8: case IR_TYPE_BOOL: return 0;
    case IR_TYPE_U16: return 1;
    case IR_TYPE_I64: case IR_TYPE_U64: case IR_TYPE_F64: return 3;
    default: return 2;
  }
}

static bool macho_is_main_function(const IrFunction *fun) { return fun && fun->is_exported && fun->name && strcmp(fun->name, "main") == 0; }

static bool macho_function_propagates_to_process_exit(const IrFunction *fun) {
  return fun && (fun->raises || (macho_is_main_function(fun) && fun->return_type == IR_TYPE_I32 && fun->value_return_type == IR_TYPE_VOID));
}

static unsigned macho_abi_slots_for_param(const IrLocal *local) { return local && local->type == IR_TYPE_BYTE_VIEW ? 2u : 1u; }

static void macho_emit_cast_normalize_reg(ZBuf *text, unsigned reg, IrTypeKind source, IrTypeKind target) {
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
      } else if (!macho_type_is_scalar64(source)) z_aarch64_emit_mov_w(text, reg, reg);
      return;
    default:
      return;
  }
}

static unsigned macho_slot_offset(unsigned local_index) { return local_index * 8; }

// Forward declarations for the record/sret ABI helpers (definitions live further down — they
// reference macho_emit_call_to_reg and the value-emitters, so they sit after those, but the
// call-site cases need to call them earlier in the file).
static bool macho_returns_record(const IrFunction *fun);
static unsigned macho_sret_reserved_bytes(const IrFunction *fun);
static unsigned macho_sret_slot_offset(const IrFunction *fun);

static unsigned macho_local_slot_offset(const IrFunction *fun, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  if (fun && local_index < fun->local_len && fun->locals[local_index].frame_offset > 0 && frame_size >= fun->locals[local_index].frame_offset) {
    return frame_size - fun->locals[local_index].frame_offset + slot_offset;
  }
  return MACHO_SCRATCH_SLOT_COUNT * MACHO_SCRATCH_SLOT_BYTES + macho_slot_offset(local_index) + slot_offset;
}

static void macho_emit_load_local_w(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  z_aarch64_emit_load_w_sp(text, reg, offset);
}

static void macho_emit_load_local_x(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  z_aarch64_emit_load_x_sp(text, reg, offset);
}

static void macho_emit_store_local_w(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  z_aarch64_emit_store_w_sp(text, reg, offset);
}

static void macho_emit_store_local_x(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  z_aarch64_emit_store_x_sp(text, reg, offset);
}

static void macho_emit_load_local_b(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  z_aarch64_emit_load_b_sp(text, reg, offset);
}

static void macho_emit_store_local_b(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  z_aarch64_emit_store_b_sp(text, reg, offset);
}

static bool macho_scratch_slot(unsigned slot, unsigned *offset, const IrValue *value, ZDiag *diag) {
  if (slot >= MACHO_SCRATCH_SLOT_COUNT) {
    return macho_diag_at(diag, "direct AArch64 Mach-O expression nesting exceeds scratch register spill capacity", value ? value->line : 1, value ? value->column : 1, "expression too deep");
  }
  *offset = slot * MACHO_SCRATCH_SLOT_BYTES;
  return true;
}

static bool macho_emit_store_scratch(ZBuf *text, unsigned reg, IrTypeKind type, unsigned slot, const IrValue *value, ZDiag *diag) {
  unsigned offset = 0;
  if (!macho_scratch_slot(slot, &offset, value, diag)) return false;
  if (macho_type_is_scalar64(type)) z_aarch64_emit_store_x_sp(text, reg, offset);
  else z_aarch64_emit_store_w_sp(text, reg, offset);
  return true;
}

static bool macho_emit_load_scratch(ZBuf *text, unsigned reg, IrTypeKind type, unsigned slot, const IrValue *value, ZDiag *diag) {
  unsigned offset = 0;
  if (!macho_scratch_slot(slot, &offset, value, diag)) return false;
  if (macho_type_is_scalar64(type)) z_aarch64_emit_load_x_sp(text, reg, offset);
  else z_aarch64_emit_load_w_sp(text, reg, offset);
  return true;
}

// Wide-load (LDR x) the pointer stashed in slot 0 of a ref-record local into `ptr_reg`.
// Use BEFORE any subsequent ldr/str with `ptr_reg` as the base; the ref-record slot itself is
// untouched. Callers that need the deref'd address compute ptr_reg, then build ldr/str with
// the field offset (which fits in the LDR/STR unsigned-imm encoding for typical records).
static void macho_emit_load_ref_record_ptr(ZBuf *text, const IrFunction *fun, unsigned ptr_reg, unsigned local_index, unsigned frame_size) {
  macho_emit_load_local_x(text, fun, ptr_reg, local_index, 0, frame_size);
}

static void macho_emit_load_field(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned field_offset, IrTypeKind type, unsigned frame_size) {
  // ref-record field load — deref the pointer first, then ldr at field_offset off the
  // pointer. ptr_reg = reg (we'll overwrite reg anyway when loading the field). For a wide
  // (64-bit) field we still need reg, so use an alternate scratch (x9 / x8 swap) for the ptr.
  if (local_index < fun->local_len && fun->locals[local_index].is_ref) {
    unsigned ptr_reg = reg == 9 ? 8 : 9;
    macho_emit_load_ref_record_ptr(text, fun, ptr_reg, local_index, frame_size);
    if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) z_aarch64_emit_load_b_imm(text, reg, ptr_reg, field_offset);
    else z_aarch64_emit_load_w_imm(text, reg, ptr_reg, field_offset);
    return;
  }
  if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
    macho_emit_load_local_b(text, fun, reg, local_index, field_offset, frame_size);
  } else {
    macho_emit_load_local_w(text, fun, reg, local_index, field_offset, frame_size);
  }
}

static void macho_emit_store_field(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned field_offset, IrTypeKind type, unsigned frame_size) {
  // mutref-record field store — deref ptr into a separate reg (the value to store is
  // in `reg`), then str at field_offset off the deref.
  if (local_index < fun->local_len && fun->locals[local_index].is_ref) {
    unsigned ptr_reg = reg == 9 ? 8 : 9;
    macho_emit_load_ref_record_ptr(text, fun, ptr_reg, local_index, frame_size);
    if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) z_aarch64_emit_store_b_imm(text, reg, ptr_reg, field_offset);
    else z_aarch64_emit_store_w_imm(text, reg, ptr_reg, field_offset);
    return;
  }
  if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
    macho_emit_store_local_b(text, fun, reg, local_index, field_offset, frame_size);
  } else {
    macho_emit_store_local_w(text, fun, reg, local_index, field_offset, frame_size);
  }
}

static void macho_emit_binary_reg(ZBuf *text, IrBinaryOp op, unsigned dst, unsigned lhs, unsigned rhs, bool wide) {
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

static void macho_emit_u32_bounds_check(ZBuf *text, unsigned index_reg, unsigned len_reg) {
  z_aarch64_emit_cmp_w(text, index_reg, len_reg);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 3); // unsigned lower
  z_aarch64_emit_brk(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
}

// 64-bit bounds check: traps when index_reg (treated as a 32-bit usize zero-extended into x) is not
// strictly less than len_reg (full 64-bit). Used for byte-view indexing where the len slot is now
// 64-bit so spans backed by >4 GiB mappings (e.g. large model files) bounds-check correctly.
static void macho_emit_u64_bounds_check(ZBuf *text, unsigned index_reg, unsigned len_reg) {
  z_aarch64_emit_cmp_x(text, index_reg, len_reg);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 3); // unsigned lower
  z_aarch64_emit_brk(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
}

static void macho_emit_packed_error_reg(ZBuf *text, unsigned reg, unsigned code_value) {
  z_aarch64_emit_movz_x(text, reg, ((uint64_t)code_value) << 32);
}

static void macho_emit_error_condition_reg(ZBuf *text, unsigned condition_reg, unsigned packed_reg) {
  z_aarch64_emit_lsr_x_imm(text, condition_reg, packed_reg, 32);
  z_aarch64_emit_cmp_x(text, condition_reg, 31);
}

static bool macho_const_u32_value(const IrValue *value, unsigned *out) {
  if (!value || value->kind != IR_VALUE_INT || value->int_value > UINT32_MAX) return false;
  if (out) *out = (unsigned)value->int_value;
  return true;
}

static unsigned macho_cond_for_compare(IrCompareOp op) {
  switch (op) {
    case IR_CMP_EQ: return 0;
    case IR_CMP_NE: return 1;
    case IR_CMP_LT: return 11;
    case IR_CMP_LE: return 13;
    case IR_CMP_GT: return 12;
    case IR_CMP_GE: return 10;
  }
  return 0;
}

static unsigned macho_invert_cond(unsigned cond) { return cond ^ 1u; }

/* Float comparison condition codes. FCMP sets N=0,Z=0,C=1,V=1 on unordered (NaN), so these are
   chosen so every NaN/unordered ordering reads false except `!=`. */
static unsigned macho_float_cond_for_compare(IrCompareOp op) {
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
static void macho_emit_cset(ZBuf *text, unsigned gpr, unsigned cond) {
  z_aarch64_append_u32(text, 0x1a9f07e0u | (((cond ^ 1u) & 15u) << 12) | (gpr & 31u));
}

static MachORuntimeHelper macho_runtime_helper_for_value(IrValueKind kind) {
  switch (kind) {
    case IR_VALUE_HTTP_RESULT_OK: return MACHO_RUNTIME_HTTP_RESULT_OK;
    case IR_VALUE_HTTP_RESULT_STATUS: return MACHO_RUNTIME_HTTP_RESULT_STATUS;
    case IR_VALUE_HTTP_RESULT_BODY_LEN: return MACHO_RUNTIME_HTTP_RESULT_BODY_LEN;
    case IR_VALUE_HTTP_RESULT_ERROR: return MACHO_RUNTIME_HTTP_RESULT_ERROR;
    case IR_VALUE_HTTP_RESPONSE_LEN: return MACHO_RUNTIME_HTTP_RESPONSE_LEN;
    case IR_VALUE_HTTP_RESPONSE_HEADERS_LEN: return MACHO_RUNTIME_HTTP_RESPONSE_HEADERS_LEN;
    case IR_VALUE_HTTP_RESPONSE_BODY_OFFSET: return MACHO_RUNTIME_HTTP_RESPONSE_BODY_OFFSET;
    case IR_VALUE_HTTP_HEADER_VALUE: return MACHO_RUNTIME_HTTP_HEADER_VALUE;
    case IR_VALUE_HTTP_HEADER_FOUND: return MACHO_RUNTIME_HTTP_HEADER_FOUND;
    case IR_VALUE_HTTP_HEADER_OFFSET: return MACHO_RUNTIME_HTTP_HEADER_OFFSET;
    case IR_VALUE_HTTP_HEADER_LEN: return MACHO_RUNTIME_HTTP_HEADER_LEN;
    default: return MACHO_RUNTIME_HELPER_COUNT;
  }
}

static bool macho_readonly_data_byte(const IrProgram *program, unsigned offset, unsigned char *out) {
  if (!program) return false;
  for (size_t i = 0; i < program->data_segment_len; i++) {
    const IrDataSegment *segment = &program->data_segments[i];
    if (offset >= segment->offset && offset < segment->offset + segment->len) {
      if (out) *out = segment->bytes[offset - segment->offset];
      return true;
    }
  }
  return false;
}

static bool macho_byte_view_const_len(const IrValue *view, unsigned *out) {
  if (!view) return false;
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (out) *out = view->data_len;
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned base_len = 0;
    if (!macho_byte_view_const_len(view->left, &base_len)) return false;
    unsigned start = 0;
    unsigned end = base_len;
    if (view->index && !macho_const_u32_value(view->index, &start)) return false;
    if (view->right && !macho_const_u32_value(view->right, &end)) return false;
    if (start > end || end > base_len) return false;
    if (out) *out = end - start;
    return true;
  }
  return false;
}

static bool macho_byte_view_const_byte(const IrProgram *program, const IrValue *view, unsigned index, unsigned char *out) {
  if (!view) return false;
  if (view->kind == IR_VALUE_STRING_LITERAL) {
    if (index >= view->data_len) return false;
    return macho_readonly_data_byte(program, view->data_offset + index, out);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned len = 0;
    unsigned start = 0;
    if (!macho_byte_view_const_len(view, &len) || index >= len) return false;
    if (view->index && !macho_const_u32_value(view->index, &start)) return false;
    return macho_byte_view_const_byte(program, view->left, start + index, out);
  }
  return false;
}

static bool macho_emit_rodata_ptr_literal(ZBuf *text, unsigned reg, unsigned data_offset, MachOEmitContext *ctx, const IrValue *value, ZDiag *diag) {
  if (ctx && ctx->pie_relative_data) {
    size_t patch_offset = text->len;
    z_aarch64_emit_adrp_add_placeholder(text, reg);
    return z_macho_record_data_patch(ctx, patch_offset, data_offset, value, diag);
  }
  while (((text->len + 8) % 8) != 0) z_aarch64_emit_nop(text);
  z_aarch64_emit_ldr_x_literal8(text, reg);
  z_aarch64_emit_b_offset_words(text, 3);
  size_t patch_offset = text->len;
  z_aarch64_append_u64(text, data_offset - (ctx ? ctx->rodata_base_offset : 0));
  return z_macho_record_data_patch(ctx, patch_offset, data_offset, value, diag);
}

static bool macho_emit_byte_view_ptr_at(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_byte_view_ptr(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) { return macho_emit_byte_view_ptr_at(text, fun, view, reg, frame_size, 0, ctx, diag); }
static bool macho_emit_byte_view_len_at(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_byte_view_len(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) { return macho_emit_byte_view_len_at(text, fun, view, reg, frame_size, 0, ctx, diag); }
static bool macho_emit_value_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_value_to_reg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) { return macho_emit_value_to_reg_at(text, fun, value, reg, frame_size, 0, ctx, diag); }
static void macho_emit_epilogue(ZBuf *text, unsigned frame_size, bool restore_process_args);

static bool macho_emit_float_value_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_float_value_to_vreg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) { return macho_emit_float_value_to_vreg_at(text, fun, value, vreg, frame_size, 0, ctx, diag); }

/* Spill/reload an FP register to the same SP-relative scratch slot the integer paths use; one
   8-byte slot holds an f32 or f64 spill cleanly. */
static bool macho_emit_store_scratch_v(ZBuf *text, unsigned vreg, bool is64, unsigned slot, const IrValue *value, ZDiag *diag) {
  unsigned offset = 0;
  if (!macho_scratch_slot(slot, &offset, value, diag)) return false;
  z_aarch64_emit_str_v_off(text, vreg, 31, offset, is64);
  return true;
}

static bool macho_emit_load_scratch_v(ZBuf *text, unsigned vreg, bool is64, unsigned slot, const IrValue *value, ZDiag *diag) {
  unsigned offset = 0;
  if (!macho_scratch_slot(slot, &offset, value, diag)) return false;
  z_aarch64_emit_ldr_v_off(text, vreg, 31, offset, is64);
  return true;
}

/* Load/store an FP register from a local or record-field slot, addressed exactly as the integer
   local/field helpers do (SP-relative, via the shared slot-offset computation). When the
   local is a ref<Record>, deref the stashed pointer into x9 and base the FP load/store off it. */
static void macho_emit_load_local_v(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned slot_offset, bool is64, unsigned frame_size) {
  if (local_index < fun->local_len && fun->locals[local_index].is_ref) {
    macho_emit_load_ref_record_ptr(text, fun, 9, local_index, frame_size);
    z_aarch64_emit_ldr_v_off(text, vreg, 9, slot_offset, is64);
    return;
  }
  z_aarch64_emit_ldr_v_off(text, vreg, 31, macho_local_slot_offset(fun, local_index, slot_offset, frame_size), is64);
}

static void macho_emit_store_local_v(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned slot_offset, bool is64, unsigned frame_size) {
  if (local_index < fun->local_len && fun->locals[local_index].is_ref) {
    macho_emit_load_ref_record_ptr(text, fun, 9, local_index, frame_size);
    z_aarch64_emit_str_v_off(text, vreg, 9, slot_offset, is64);
    return;
  }
  z_aarch64_emit_str_v_off(text, vreg, 31, macho_local_slot_offset(fun, local_index, slot_offset, frame_size), is64);
}

static bool macho_emit_json_parse_bytes_call_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!macho_emit_byte_view_ptr_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 0, IR_TYPE_U64, scratch_slot, value ? value->left : NULL, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->left, 1, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 0, IR_TYPE_U64, scratch_slot, value ? value->left : NULL, diag)) return false;
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  return z_macho_record_value_runtime_patch(ctx, MACHO_RUNTIME_JSON_PARSE_BYTES, patch, value, diag);
}

static bool macho_emit_byte_view_len_at(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!view) return macho_diag_at(diag, "direct AArch64 Mach-O byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (view->data_len > 65535) return macho_diag_at(diag, "direct AArch64 Mach-O byte-view length is too large for the current MVP", view->line, view->column, "large byte view");
    z_aarch64_emit_movz_w(text, reg, view->data_len);
    return true;
  }
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    macho_emit_load_local_x(text, fun, reg, view->local_index, 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field length lives 8 bytes past the field's ptr, as a 64-bit byte/element count.
    // ref-record — deref the stashed pointer into x9, then load len at
    // [x9 + field_offset + 8].
    if (fun->locals[view->local_index].is_ref) {
      unsigned ptr_reg = reg == 9 ? 8 : 9;
      macho_emit_load_ref_record_ptr(text, fun, ptr_reg, view->local_index, frame_size);
      z_aarch64_emit_load_x_imm(text, reg, ptr_reg, view->field_offset + 8);
      return true;
    }
    macho_emit_load_local_x(text, fun, reg, view->local_index, view->field_offset + 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    macho_emit_load_local_x(text, fun, reg, view->local_index, 16, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    // Reinterpreted element count = underlying byte length >> log2(sizeof(T)); a 1-byte element
    // (i8) keeps the byte length unchanged.
    if (!macho_emit_byte_view_len_at(text, fun, view->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
    unsigned shift = macho_elem_log2(view->element_type);
    if (shift > 0) z_aarch64_emit_lsr_x_imm(text, reg, reg, shift);
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    unsigned end = 0;
    if ((!view->index || macho_const_u32_value(view->index, &start)) &&
        macho_const_u32_value(view->right, &end) && end >= start && end - start <= 65535) {
      z_aarch64_emit_movz_w(text, reg, end - start);
      return true;
    }
    if ((!view->index || macho_const_u32_value(view->index, &start)) && view->right) {
      if (!macho_emit_value_to_reg_at(text, fun, view->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
      if (start > 0) z_aarch64_emit_sub_x_imm(text, reg, reg, start);
      return true;
    }
    if (view->index && view->right) {
      unsigned tmp = reg == 8 ? 9 : 8;
      if (!macho_emit_value_to_reg_at(text, fun, view->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch(text, reg, IR_TYPE_U64, scratch_slot, view->right, diag)) return false;
      if (!macho_emit_value_to_reg_at(text, fun, view->index, tmp, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!macho_emit_load_scratch(text, reg, IR_TYPE_U64, scratch_slot, view->right, diag)) return false;
      macho_emit_binary_reg(text, IR_BIN_SUB, reg, reg, tmp, true);
      return true;
    }
  }
  (void)ctx;
  return macho_diag_at(diag, "direct AArch64 Mach-O byte-view length currently requires a literal, constant slice, or byte-view local", view->line, view->column, "unsupported byte view length");
}

static bool macho_emit_byte_view_ptr_at(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!view) return macho_diag_at(diag, "direct AArch64 Mach-O byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    macho_emit_load_local_x(text, fun, reg, view->local_index, 0, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field: the 8-byte pointer lives at the field offset within the record local.
    // ref-record — deref stashed ptr first, then load the span ptr from
    // [deref + field_offset].
    if (fun->locals[view->local_index].is_ref) {
      unsigned ptr_reg = reg == 9 ? 8 : 9;
      macho_emit_load_ref_record_ptr(text, fun, ptr_reg, view->local_index, frame_size);
      z_aarch64_emit_load_x_imm(text, reg, ptr_reg, view->field_offset);
      return true;
    }
    macho_emit_load_local_x(text, fun, reg, view->local_index, view->field_offset, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    macho_emit_load_local_x(text, fun, reg, view->local_index, 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    // Any value-typed array binds as a typed span — ptr = &arr[0]. The IR_VALUE_ARRAY_BYTE_VIEW
    // carries element_type so subsequent typed-span ops (INDEX_LOAD/STORE / BYTE_SLICE) scale by sizeof(T).
    if (!local->is_array) return macho_diag_at(diag, "direct AArch64 Mach-O byte-view array source must be a fixed array local", view->line, view->column, "unsupported array view");
    z_aarch64_emit_add_x_sp_imm(text, reg, macho_local_slot_offset(fun, view->array_index, 0, frame_size));
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) {
    return macho_emit_rodata_ptr_literal(text, reg, view->data_offset, ctx, view, diag);
  }
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    // A reinterpret keeps the same base pointer (zero-copy); only the element count changes.
    return macho_emit_byte_view_ptr_at(text, fun, view->left, reg, frame_size, scratch_slot, ctx, diag);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    unsigned shift = macho_elem_log2(view->element_type);
    if (!macho_emit_byte_view_ptr_at(text, fun, view->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
    if (!view->index) return true;
    if (macho_const_u32_value(view->index, &start)) {
      // Slice bounds are element indices, so the byte offset is start << log2(sizeof(T)).
      unsigned byte_start = start << shift;
      if (byte_start > 4095) return macho_diag_at(diag, "direct AArch64 Mach-O byte slice constant start is too large", view->line, view->column, "unsupported byte slice");
      if (byte_start > 0) z_aarch64_emit_add_x_imm(text, reg, reg, byte_start);
      return true;
    }
    unsigned tmp = reg == 8 ? 9 : 8;
    if (!macho_emit_store_scratch(text, reg, IR_TYPE_U64, scratch_slot, view, diag)) return false;
    if (!macho_emit_value_to_reg_at(text, fun, view->index, tmp, frame_size, scratch_slot + 1, ctx, diag)) return false;
    if (!macho_emit_load_scratch(text, reg, IR_TYPE_U64, scratch_slot, view, diag)) return false;
    if (shift > 0) z_aarch64_emit_add_x_reg_lsl(text, reg, reg, tmp, shift);
    else z_aarch64_emit_add_x_reg(text, reg, reg, tmp);
    return true;
  }
  return macho_diag_at(diag, "direct AArch64 Mach-O value is not a supported byte view", view->line, view->column, "unsupported byte view");
}

// Compute the element address (ptr + index * sizeof(T)) of a typed-span local into x9, with a
// bounds check (index < span.len traps). The index is materialized into w8 first, so callers must
// not rely on x8 surviving; x9 holds the element address on return.
static bool macho_emit_span_index_addr(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O span index local is out of range", index ? index->line : 1, index ? index->column : 1, "invalid span local");
  const IrLocal *local = &fun->locals[local_index];
  unsigned shift = macho_elem_log2(local->element_type);
  IrTypeKind index_type = index ? index->type : IR_TYPE_U32;
  if (!index || !macho_emit_value_to_reg_at(text, fun, index, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 8, index_type, scratch_slot, index, diag)) return false;
  macho_emit_load_local_x(text, fun, 9, local_index, 8, frame_size);
  macho_emit_u64_bounds_check(text, 8, 9);
  if (!macho_emit_load_scratch(text, 8, index_type, scratch_slot, index, diag)) return false;
  macho_emit_load_local_x(text, fun, 9, local_index, 0, frame_size);
  if (shift > 0) z_aarch64_emit_add_x_reg_lsl(text, 9, 9, 8, shift);
  else z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  return true;
}

static bool macho_emit_call_to_reg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  const IrFunction *callee = ctx && ctx->program && value->callee_index < ctx->program->function_len ? &ctx->program->functions[value->callee_index] : NULL;
  if (!callee) return macho_diag_at(diag, "direct AArch64 Mach-O call target is unavailable", value->line, value->column, "invalid callee");
  unsigned abi_slots = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    if (i >= callee->param_count) return macho_diag_at(diag, "direct AArch64 Mach-O call parameter metadata is unavailable", value->line, value->column, "invalid callee parameter");
    unsigned slots = macho_abi_slots_for_param(&callee->locals[i]);
    if (abi_slots + slots > 8) return macho_diag_at(diag, "direct AArch64 Mach-O call supports at most eight ABI argument slots", value->line, value->column, "too many arguments");
    abi_slots += slots;
  }
  if (scratch_slot + abi_slots >= MACHO_SCRATCH_SLOT_COUNT) {
    return macho_diag_at(diag, "direct AArch64 Mach-O call argument nesting exceeds scratch spill capacity", value->line, value->column, "too many nested call arguments");
  }
  unsigned nested_slot = scratch_slot + abi_slots;
  unsigned arg_slot = scratch_slot;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    const IrLocal *param = &callee->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) {
      if (!macho_emit_byte_view_ptr_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      if (!macho_emit_byte_view_len_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot + 1, arg, diag)) return false;
      arg_slot += 2;
      continue;
    }
    if (param->type == IR_TYPE_RECORD) {
      // Record args are passed by pointer to a local's slot (the value-copy is done callee-side via
      // the record-param copy in its prologue). The arg must be a record local.
      if (!arg || arg->kind != IR_VALUE_LOCAL || arg->local_index >= fun->local_len) {
        return macho_diag_at(diag, "direct AArch64 Mach-O record argument must be a record local", arg ? arg->line : value->line, arg ? arg->column : value->column, "non-local record arg");
      }
      z_aarch64_emit_add_x_sp_imm(text, 8, macho_local_slot_offset(fun, arg->local_index, 0, frame_size));
      if (!macho_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      arg_slot++;
      continue;
    }
    if (macho_type_is_float(param->type)) {
      bool is64 = macho_type_is_f64(param->type);
      if (!macho_emit_float_value_to_vreg_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch_v(text, 8, is64, arg_slot, arg, diag)) return false;
      arg_slot++;
      continue;
    }
    if (!macho_emit_value_to_reg_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
    if (!macho_emit_store_scratch(text, 8, arg ? arg->type : IR_TYPE_I32, arg_slot, arg, diag)) return false;
    arg_slot++;
  }
  arg_slot = scratch_slot;
  unsigned abi_slot = 0;
  unsigned fp_abi_slot = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    const IrLocal *param = &callee->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) {
      if (!macho_emit_load_scratch(text, abi_slot, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      if (!macho_emit_load_scratch(text, abi_slot + 1, IR_TYPE_U64, arg_slot + 1, arg, diag)) return false;
      arg_slot += 2;
      abi_slot += 2;
      continue;
    }
    if (param->type == IR_TYPE_RECORD) {
      if (!macho_emit_load_scratch(text, abi_slot, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      arg_slot++;
      abi_slot++;
      continue;
    }
    if (macho_type_is_float(param->type)) {
      bool is64 = macho_type_is_f64(param->type);
      if (!macho_emit_load_scratch_v(text, fp_abi_slot, is64, arg_slot, arg, diag)) return false;
      arg_slot++;
      fp_abi_slot++;
      continue;
    }
    if (!macho_emit_load_scratch(text, abi_slot, arg ? arg->type : IR_TYPE_I32, arg_slot, arg, diag)) return false;
    arg_slot++;
    abi_slot++;
  }
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_call_patch(ctx, patch, value->callee_index, value, diag)) return false;
  if (macho_type_is_float(value->type)) {
    if (reg != 0) z_aarch64_emit_fmov_reg(text, reg, 0, macho_type_is_f64(value->type));
  } else if (reg != 0) {
    if (macho_type_is_scalar64(value->type)) z_aarch64_emit_mov_x(text, reg, 0);
    else z_aarch64_emit_mov_w(text, reg, 0);
  }
  return true;
}

// memcpy a record between inline frame slots: copy the source local's bytes to a destination
// — another local's slot, or the caller's sret buffer when dest_index == UINT_MAX. x8/x9 are
// scratch; 8-byte chunks then a 4-byte tail (records are 8- or 4-aligned, so the tail is exact).
// Span fields ride along as raw 16 bytes (ptr then len), preserving the view.
static void macho_emit_record_copy_to(ZBuf *text, const IrFunction *fun, unsigned dest_index, unsigned src_index, unsigned frame_size) {
  unsigned size = src_index < fun->local_len ? fun->locals[src_index].byte_size : 0;
  if (dest_index == UINT_MAX) {
    z_aarch64_emit_load_x_imm(text, 8, 31, macho_sret_slot_offset(fun)); // x8 = caller's sret pointer
  } else {
    z_aarch64_emit_add_x_sp_imm(text, 8, macho_local_slot_offset(fun, dest_index, 0, frame_size)); // x8 = &dest
  }
  z_aarch64_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, src_index, 0, frame_size)); // x9 = &src
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
static void macho_emit_copy_record_param(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned ptr_reg, unsigned frame_size) {
  unsigned size = local_index < fun->local_len ? fun->locals[local_index].byte_size : 0;
  z_aarch64_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, local_index, 0, frame_size)); // x9 = &slot
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
// macho_emit_call_to_reg's machinery, then patch x8 (the AAPCS indirect-result register) to point
// at the destination, then bl. `dest_local` >= 0 binds the call result into a local's slot
// (`let x = f()`); `dest_local` < 0 returns straight through to the caller's sret buffer (`ret
// f()`). x8 is set just before bl because the argument emitters use x8 as scratch.
static bool macho_emit_record_call_with_dest(ZBuf *text, const IrFunction *fun, int dest_local, const IrValue *value, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  const IrFunction *callee = ctx && ctx->program && value->callee_index < ctx->program->function_len ? &ctx->program->functions[value->callee_index] : NULL;
  if (!callee) return macho_diag_at(diag, "direct AArch64 Mach-O call target is unavailable", value->line, value->column, "invalid callee");
  unsigned abi_slots = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    if (i >= callee->param_count) return macho_diag_at(diag, "direct AArch64 Mach-O call parameter metadata is unavailable", value->line, value->column, "invalid callee parameter");
    unsigned slots = macho_abi_slots_for_param(&callee->locals[i]);
    if (abi_slots + slots > 8) return macho_diag_at(diag, "direct AArch64 Mach-O call supports at most eight ABI argument slots", value->line, value->column, "too many arguments");
    abi_slots += slots;
  }
  if (scratch_slot + abi_slots >= MACHO_SCRATCH_SLOT_COUNT) {
    return macho_diag_at(diag, "direct AArch64 Mach-O call argument nesting exceeds scratch spill capacity", value->line, value->column, "too many nested call arguments");
  }
  unsigned nested_slot = scratch_slot + abi_slots;
  unsigned arg_slot = scratch_slot;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    const IrLocal *param = &callee->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) {
      if (!macho_emit_byte_view_ptr_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      if (!macho_emit_byte_view_len_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot + 1, arg, diag)) return false;
      arg_slot += 2;
      continue;
    }
    if (param->type == IR_TYPE_RECORD) {
      if (!arg || arg->kind != IR_VALUE_LOCAL || arg->local_index >= fun->local_len) {
        return macho_diag_at(diag, "direct AArch64 Mach-O record argument must be a record local", arg ? arg->line : value->line, arg ? arg->column : value->column, "non-local record arg");
      }
      z_aarch64_emit_add_x_sp_imm(text, 8, macho_local_slot_offset(fun, arg->local_index, 0, frame_size));
      if (!macho_emit_store_scratch(text, 8, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      arg_slot++;
      continue;
    }
    if (macho_type_is_float(param->type)) {
      bool is64 = macho_type_is_f64(param->type);
      if (!macho_emit_float_value_to_vreg_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch_v(text, 8, is64, arg_slot, arg, diag)) return false;
      arg_slot++;
      continue;
    }
    if (!macho_emit_value_to_reg_at(text, fun, arg, 8, frame_size, nested_slot, ctx, diag)) return false;
    if (!macho_emit_store_scratch(text, 8, arg ? arg->type : IR_TYPE_I32, arg_slot, arg, diag)) return false;
    arg_slot++;
  }
  arg_slot = scratch_slot;
  unsigned abi_slot = 0;
  unsigned fp_abi_slot = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    const IrLocal *param = &callee->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) {
      if (!macho_emit_load_scratch(text, abi_slot, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      if (!macho_emit_load_scratch(text, abi_slot + 1, IR_TYPE_U64, arg_slot + 1, arg, diag)) return false;
      arg_slot += 2;
      abi_slot += 2;
      continue;
    }
    if (param->type == IR_TYPE_RECORD) {
      if (!macho_emit_load_scratch(text, abi_slot, IR_TYPE_U64, arg_slot, arg, diag)) return false;
      arg_slot++;
      abi_slot++;
      continue;
    }
    if (macho_type_is_float(param->type)) {
      bool is64 = macho_type_is_f64(param->type);
      if (!macho_emit_load_scratch_v(text, fp_abi_slot, is64, arg_slot, arg, diag)) return false;
      arg_slot++;
      fp_abi_slot++;
      continue;
    }
    if (!macho_emit_load_scratch(text, abi_slot, arg ? arg->type : IR_TYPE_I32, arg_slot, arg, diag)) return false;
    arg_slot++;
    abi_slot++;
  }
  // Set the indirect-result register x8 last so the argument-marshaling stage's x8 scratch use
  // cannot trample it. dest_local < 0 means the caller's own sret pointer (we are returning the
  // call's result straight through to our caller's storage).
  if (dest_local >= 0) {
    z_aarch64_emit_add_x_sp_imm(text, 8, macho_local_slot_offset(fun, (unsigned)dest_local, 0, frame_size));
  } else {
    z_aarch64_emit_load_x_imm(text, 8, 31, macho_sret_slot_offset(fun));
  }
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  return z_macho_record_call_patch(ctx, patch, value->callee_index, value, diag);
}

// Store one field of the record being returned, written through the caller's sret pointer (saved
// in the frame). x8 is reloaded from the slot after each value is materialized so an intervening
// call cannot strand it. Span fields store ptr@offset and len@offset+8.
static bool macho_emit_sret_field_store(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  unsigned slot = macho_sret_slot_offset(fun);
  IrTypeKind vt = instr->value ? instr->value->type : IR_TYPE_I32;
  unsigned fo = instr->field_offset;
  if (vt == IR_TYPE_BYTE_VIEW) {
    if (instr->value && instr->value->kind == IR_VALUE_CALL) {
      // A span-returning call leaves ptr in x0, len in x1; capture both before reloading x8.
      if (!macho_emit_call_to_reg(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
      z_aarch64_emit_mov_x(text, 9, 1); // preserve len; x1 is otherwise clobbered by the x8 reload path
      z_aarch64_emit_load_x_imm(text, 8, 31, slot);
      z_aarch64_emit_store_x_imm(text, 0, 8, fo);
      z_aarch64_emit_store_x_imm(text, 9, 8, fo + 8);
    } else {
      if (!macho_emit_byte_view_ptr(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
      z_aarch64_emit_load_x_imm(text, 8, 31, slot);
      z_aarch64_emit_store_x_imm(text, 9, 8, fo);
      if (!macho_emit_byte_view_len(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
      z_aarch64_emit_load_x_imm(text, 8, 31, slot);
      z_aarch64_emit_store_x_imm(text, 9, 8, fo + 8);
    }
    return true;
  }
  if (macho_type_is_float(vt)) {
    bool is64 = macho_type_is_f64(vt);
    if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
    z_aarch64_emit_load_x_imm(text, 8, 31, slot);
    z_aarch64_emit_str_v_off(text, 9, 8, fo, is64);
    return true;
  }
  if (!macho_emit_value_to_reg(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
  z_aarch64_emit_load_x_imm(text, 8, 31, slot);
  if (vt == IR_TYPE_U8 || vt == IR_TYPE_I8 || vt == IR_TYPE_BOOL) z_aarch64_emit_store_b_imm(text, 9, 8, fo);
  else if (macho_type_is_scalar64(vt)) z_aarch64_emit_store_x_imm(text, 9, 8, fo);
  else z_aarch64_emit_store_w_imm(text, 9, 8, fo);
  return true;
}

static bool macho_emit_cast_value_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  // A float operand cast to an integer: evaluate into v8 and truncate toward zero into a W result.
  if (value->left && macho_type_is_float(value->left->type)) {
    if (!macho_emit_float_value_to_vreg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
    z_aarch64_emit_fcvtzs_w(text, reg, 8, macho_type_is_f64(value->left->type));
    return true;
  }
  if (!macho_emit_value_to_reg_at(text, fun, value->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
  macho_emit_cast_normalize_reg(text, reg, value->left ? value->left->type : IR_TYPE_UNSUPPORTED, value->type);
  return true;
}

static bool macho_emit_binary_value_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (value->binary_op == IR_BIN_AND) {
    if (!macho_emit_value_to_reg_at(text, fun, value->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
    size_t left_false = z_aarch64_emit_cbz_w_placeholder(text, reg);
    if (!macho_emit_value_to_reg_at(text, fun, value->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
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
    if (!macho_emit_value_to_reg_at(text, fun, value->left, reg, frame_size, scratch_slot, ctx, diag)) return false;
    size_t eval_right = z_aarch64_emit_cbz_w_placeholder(text, reg);
    z_aarch64_emit_movz_w(text, reg, 1);
    size_t left_true_end = z_aarch64_emit_b_placeholder(text);
    z_aarch64_patch_cond19(text, eval_right, text->len);
    if (!macho_emit_value_to_reg_at(text, fun, value->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
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
      value->binary_op != IR_BIN_DIV && value->binary_op != IR_BIN_MOD) return macho_diag_at(diag, "direct AArch64 Mach-O binary operator is unsupported", value->line, value->column, "unsupported operator");
  if (!macho_emit_value_to_reg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 8, value->left ? value->left->type : IR_TYPE_I32, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_value_to_reg_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 8, value->left ? value->left->type : IR_TYPE_I32, scratch_slot, value->left, diag)) return false;
  bool wide = macho_type_is_scalar64(value->type);
  if (value->binary_op == IR_BIN_DIV) {
    z_aarch64_emit_div_reg(text, reg, 8, 9, macho_type_is_unsigned(value->type), wide);
  } else if (value->binary_op == IR_BIN_MOD) {
    z_aarch64_emit_div_reg(text, 10, 8, 9, macho_type_is_unsigned(value->type), wide);
    z_aarch64_emit_msub_reg(text, reg, 10, 9, 8, wide);
  } else {
    macho_emit_binary_reg(text, value->binary_op, reg, 8, 9, wide);
  }
  return true;
}

static bool macho_emit_compare_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) {
    return macho_diag_at(diag, "direct AArch64 Mach-O comparison requires two operands", value->line, value->column, "invalid comparison");
  }
  // A float comparison yields an integer boolean from float operands: spill the left operand to an
  // FP scratch slot across the right's evaluation, FCMP, then materialize the IEEE-correct boolean.
  if (macho_type_is_float(value->left->type)) {
    bool is64 = macho_type_is_f64(value->left->type);
    if (!macho_emit_float_value_to_vreg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
    if (!macho_emit_store_scratch_v(text, 8, is64, scratch_slot, value->left, diag)) return false;
    if (!macho_emit_float_value_to_vreg_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
    if (!macho_emit_load_scratch_v(text, 8, is64, scratch_slot, value->left, diag)) return false;
    z_aarch64_emit_fcmp(text, 8, 9, is64);
    macho_emit_cset(text, reg, macho_float_cond_for_compare(value->compare_op));
    return true;
  }
  if (!macho_emit_value_to_reg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 8, value->left->type, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_value_to_reg_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 8, value->left->type, scratch_slot, value->left, diag)) return false;
  if (macho_type_is_scalar64(value->left->type)) z_aarch64_emit_cmp_x(text, 8, 9);
  else z_aarch64_emit_cmp_w(text, 8, 9);
  z_aarch64_emit_movz_w(text, reg, 0);
  size_t false_patch = z_aarch64_emit_b_cond_placeholder(text, macho_invert_cond(macho_cond_for_compare(value->compare_op)));
  z_aarch64_emit_movz_w(text, reg, 1);
  z_aarch64_patch_cond19(text, false_patch, text->len);
  return true;
}

static bool macho_emit_byte_view_index_load_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  unsigned const_index = 0;
  unsigned char byte = 0;
  if (macho_const_u32_value(value->index, &const_index) &&
      macho_byte_view_const_byte(ctx ? ctx->program : NULL, value->left, const_index, &byte)) {
    z_aarch64_emit_movz_w(text, reg, byte);
    return true;
  }
  if (!value->index || !macho_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  macho_emit_u64_bounds_check(text, 8, 9);
  if (!macho_emit_byte_view_ptr_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  z_aarch64_emit_load_b_imm(text, reg, 9, 0);
  return true;
}

static bool macho_emit_byte_view_read_int_le_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  // std.codec.readU16Le / readI32Le / readU32Le / readI64Le / readU64Le: read a 2/4/8-byte
  // little-endian integer at a byte offset. AArch64 is little-endian, so an integer load at
  // ptr+offset already yields the value; same-width signed/unsigned share the raw load. Bounds-check
  // that offset+size fits within the span length before loading: with the unsigned compare
  // offset+(size-1) < len is equivalent to offset+size <= len, and rejects offset overflow.
  if (!value->left) return macho_diag_at(diag, "direct AArch64 Mach-O read*Le requires a byte view", value->line, value->column, "missing byte view");
  if (!value->index) return macho_diag_at(diag, "direct AArch64 Mach-O read*Le requires an offset", value->line, value->column, "missing offset");
  unsigned scalar_size = macho_elem_byte_size(value->type);
  IrTypeKind index_type = value->index->type;
  if (!macho_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_add_w_imm(text, 10, 8, scalar_size - 1u); // offset + (size - 1)
  macho_emit_u64_bounds_check(text, 10, 9);
  if (!macho_emit_byte_view_ptr_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  if (scalar_size == 8) z_aarch64_emit_load_x_imm(text, reg, 9, 0);
  else if (scalar_size == 2) z_aarch64_emit_load_h_imm(text, reg, 9, 0);
  else z_aarch64_emit_load_w_imm(text, reg, 9, 0);
  return true;
}

static bool macho_emit_byte_copy_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return macho_diag_at(diag, "direct AArch64 Mach-O byte copy requires source and destination byte views", value->line, value->column, "missing byte view");
  if (!macho_emit_byte_view_ptr_at(text, fun, value->left, 11, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 11, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->left, 10, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 10, IR_TYPE_U32, scratch_slot + 1, value->left, diag)) return false;
  if (!macho_emit_byte_view_ptr_at(text, fun, value->right, 12, frame_size, scratch_slot + 2, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 12, IR_TYPE_U64, scratch_slot + 2, value->right, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->right, 13, frame_size, scratch_slot + 3, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 10, IR_TYPE_U32, scratch_slot + 1, value->left, diag)) return false;
  if (!macho_emit_load_scratch(text, 11, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_load_scratch(text, 12, IR_TYPE_U64, scratch_slot + 2, value->right, diag)) return false;
  z_aarch64_emit_byte_copy_min_loop(text, reg);
  return true;
}

static bool macho_emit_byte_fill_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return macho_diag_at(diag, "direct AArch64 Mach-O byte fill requires a fill byte and destination byte view", value->line, value->column, "missing byte fill input");
  if (!macho_emit_value_to_reg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 8, IR_TYPE_U8, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_byte_view_ptr_at(text, fun, value->right, 11, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 11, IR_TYPE_U64, scratch_slot + 1, value->right, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->right, 10, frame_size, scratch_slot + 2, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 8, IR_TYPE_U8, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_load_scratch(text, 11, IR_TYPE_U64, scratch_slot + 1, value->right, diag)) return false;
  z_aarch64_emit_byte_fill_loop(text, reg);
  return true;
}

static bool macho_emit_byte_view_eq_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return macho_diag_at(diag, "direct AArch64 Mach-O byte-view equality requires two byte views", value->line, value->column, "missing byte view");
  if (!macho_emit_byte_view_len_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 8, IR_TYPE_U32, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 8, IR_TYPE_U32, scratch_slot, value->left, diag)) return false;
  z_aarch64_emit_cmp_w(text, 8, 9);
  size_t same_len = z_aarch64_emit_b_cond_placeholder(text, 0);
  z_aarch64_emit_movz_w(text, reg, 0);
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, same_len, text->len);
  if (!macho_emit_byte_view_ptr_at(text, fun, value->left, 11, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 11, IR_TYPE_U64, scratch_slot + 1, value->left, diag)) return false;
  if (!macho_emit_byte_view_ptr_at(text, fun, value->right, 12, frame_size, scratch_slot + 2, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 11, IR_TYPE_U64, scratch_slot + 1, value->left, diag)) return false;
  if (!macho_emit_load_scratch(text, 10, IR_TYPE_U32, scratch_slot, value->left, diag)) return false;
  z_aarch64_emit_byte_eq_loop(text, reg);
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

static bool macho_emit_index_load_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (value->array_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O indexed load array is out of range", value->line, value->column, "invalid array local");
  const IrLocal *local = &fun->locals[value->array_index];
  unsigned const_index = 0;
  if (local->type == IR_TYPE_BYTE_VIEW) {
    // Typed-span element read: element-scaled address then a width-appropriate integer load. i8 is
    // sign-extended (LDRSB); u8/Bool zero-extended (LDRB); 8-byte elements via LDR x; else LDR w.
    if (!macho_emit_span_index_addr(text, fun, value->array_index, value->index, frame_size, scratch_slot, ctx, diag)) return false;
    IrTypeKind elem = local->element_type;
    if (elem == IR_TYPE_U8 || elem == IR_TYPE_BOOL) z_aarch64_emit_load_b_imm(text, reg, 9, 0);
    else if (elem == IR_TYPE_I8) z_aarch64_emit_load_sb_imm(text, reg, 9, 0);
    else if (macho_elem_byte_size(elem) == 8) z_aarch64_emit_load_x_imm(text, reg, 9, 0);
    else z_aarch64_emit_load_w_imm(text, reg, 9, 0);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE) &&
      macho_const_u32_value(value->index, &const_index) && const_index < local->array_len) {
    macho_emit_load_local_w(text, fun, reg, value->array_index, const_index * 4u, frame_size);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
    if (!value->index || !macho_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
    if (!macho_emit_store_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
    z_aarch64_emit_movz_w(text, 9, local->array_len);
    macho_emit_u32_bounds_check(text, 8, 9);
    if (!macho_emit_load_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
    z_aarch64_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, value->array_index, 0, frame_size));
    z_aarch64_emit_add_x_reg_lsl(text, 9, 9, 8, 2);
    z_aarch64_emit_load_w_imm(text, reg, 9, 0);
    return true;
  }
  if (!local->is_array || (local->element_type != IR_TYPE_U8 && local->element_type != IR_TYPE_BOOL)) return macho_diag_at(diag, "direct AArch64 Mach-O indexed load requires [N]u8, [N]Bool, or integer arrays", value->line, value->column, "unsupported array local");
  if (!value->index || !macho_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_movz_w(text, 9, local->array_len);
  macho_emit_u32_bounds_check(text, 8, 9);
  if (!macho_emit_load_scratch(text, 8, value->index ? value->index->type : IR_TYPE_U32, scratch_slot, value->index, diag)) return false;
  z_aarch64_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, value->array_index, 0, frame_size));
  z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  z_aarch64_emit_load_b_imm(text, reg, 9, 0);
  return true;
}

static bool macho_emit_http_fetch_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!macho_emit_byte_view_ptr_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 0, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->left, 1, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 1, IR_TYPE_U32, scratch_slot + 1, value->left, diag)) return false;
  if (!macho_emit_byte_view_ptr_at(text, fun, value->right, 2, frame_size, scratch_slot + 2, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 2, IR_TYPE_U64, scratch_slot + 2, value->right, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->right, 3, frame_size, scratch_slot + 3, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 3, IR_TYPE_U32, scratch_slot + 3, value->right, diag)) return false;
  if (!macho_emit_value_to_reg_at(text, fun, value->index, 4, frame_size, scratch_slot + 4, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 0, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_load_scratch(text, 1, IR_TYPE_U32, scratch_slot + 1, value->left, diag)) return false;
  if (!macho_emit_load_scratch(text, 2, IR_TYPE_U64, scratch_slot + 2, value->right, diag)) return false;
  if (!macho_emit_load_scratch(text, 3, IR_TYPE_U32, scratch_slot + 3, value->right, diag)) return false;
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_value_runtime_patch(ctx, MACHO_RUNTIME_HTTP_FETCH, patch, value, diag)) return false;
  if (reg != 0) z_aarch64_emit_mov_x(text, reg, 0);
  return true;
}

static bool macho_emit_http_result_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!macho_emit_value_to_reg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_value_runtime_patch(ctx, macho_runtime_helper_for_value(value->kind), patch, value, diag)) return false;
  if (reg != 0) z_aarch64_emit_mov_w(text, reg, 0);
  return true;
}

static bool macho_emit_http_response_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!macho_emit_byte_view_ptr_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 0, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->left, 1, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 0, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_value_runtime_patch(ctx, macho_runtime_helper_for_value(value->kind), patch, value, diag)) return false;
  if (reg != 0) z_aarch64_emit_mov_w(text, reg, 0);
  return true;
}

static bool macho_emit_http_header_value_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!macho_emit_byte_view_ptr_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 0, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->left, 1, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 1, IR_TYPE_U32, scratch_slot + 1, value->left, diag)) return false;
  if (!macho_emit_byte_view_ptr_at(text, fun, value->right, 2, frame_size, scratch_slot + 2, ctx, diag)) return false;
  if (!macho_emit_store_scratch(text, 2, IR_TYPE_U64, scratch_slot + 2, value->right, diag)) return false;
  if (!macho_emit_byte_view_len_at(text, fun, value->right, 3, frame_size, scratch_slot + 3, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 0, IR_TYPE_U64, scratch_slot, value->left, diag)) return false;
  if (!macho_emit_load_scratch(text, 1, IR_TYPE_U32, scratch_slot + 1, value->left, diag)) return false;
  if (!macho_emit_load_scratch(text, 2, IR_TYPE_U64, scratch_slot + 2, value->right, diag)) return false;
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_value_runtime_patch(ctx, MACHO_RUNTIME_HTTP_HEADER_VALUE, patch, value, diag)) return false;
  if (reg != 0) z_aarch64_emit_mov_x(text, reg, 0);
  return true;
}

static bool macho_emit_vec_push_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_VEC) return macho_diag_at(diag, "direct AArch64 Mach-O Vec push requires a Vec local", value->line, value->column, "invalid Vec local");
  macho_emit_load_local_w(text, fun, 8, value->local_index, 8, frame_size);
  macho_emit_load_local_w(text, fun, 9, value->local_index, 12, frame_size);
  z_aarch64_emit_cmp_w(text, 8, 9);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 3); // unsigned lower
  z_aarch64_emit_movz_w(text, reg, 0);
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  macho_emit_store_local_w(text, fun, 8, value->local_index, 8, frame_size);
  macho_emit_load_local_x(text, fun, 9, value->local_index, 0, frame_size);
  z_aarch64_emit_add_x_reg(text, 9, 9, 8);
  if (!macho_emit_store_scratch(text, 9, IR_TYPE_U64, scratch_slot, value, diag)) return false;
  if (!macho_emit_value_to_reg_at(text, fun, value->left, 10, frame_size, scratch_slot + 1, ctx, diag)) return false;
  if (!macho_emit_load_scratch(text, 9, IR_TYPE_U64, scratch_slot, value, diag)) return false;
  z_aarch64_emit_store_b_imm(text, 10, 9, 0);
  z_aarch64_emit_add_w_imm(text, 8, 8, 1);
  macho_emit_store_local_w(text, fun, 8, value->local_index, 8, frame_size);
  z_aarch64_emit_movz_w(text, reg, 1);
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

static bool macho_emit_check_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || value->left->type != IR_TYPE_I64) return macho_diag_at(diag, "direct AArch64 Mach-O check requires a packed fallible call result", value->line, value->column, "non-fallible value");
  if (!macho_emit_value_to_reg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  macho_emit_error_condition_reg(text, 8, 0);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 0);
  bool restore_process_args = ctx && ctx->seed_main_process_args && macho_is_main_function(fun);
  if (macho_function_propagates_to_process_exit(fun)) {
    macho_emit_epilogue(text, frame_size, restore_process_args);
  } else {
    z_aarch64_emit_movz_w(text, 0, 1);
    macho_emit_epilogue(text, frame_size, restore_process_args);
  }
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  if (reg != 0) {
    if (macho_type_is_scalar64(value->type)) z_aarch64_emit_mov_x(text, reg, 0);
    else z_aarch64_emit_mov_w(text, reg, 0);
  } else if (!macho_type_is_scalar64(value->type)) {
    z_aarch64_emit_mov_w(text, 0, 0);
  }
  return true;
}

static bool macho_emit_rescue_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right || value->left->type != IR_TYPE_I64) return macho_diag_at(diag, "direct AArch64 Mach-O rescue requires a packed fallible call and fallback", value->line, value->column, "unsupported rescue");
  if (!macho_emit_value_to_reg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  macho_emit_error_condition_reg(text, 8, 0);
  size_t fallback_patch = z_aarch64_emit_b_cond_placeholder(text, 1);
  if (reg != 0) {
    if (macho_type_is_scalar64(value->type)) z_aarch64_emit_mov_x(text, reg, 0);
    else z_aarch64_emit_mov_w(text, reg, 0);
  } else if (!macho_type_is_scalar64(value->type)) {
    z_aarch64_emit_mov_w(text, 0, 0);
  }
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, fallback_patch, text->len);
  if (!macho_emit_value_to_reg_at(text, fun, value->right, reg, frame_size, scratch_slot, ctx, diag)) return false;
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

// `check <float fallible call>`: the callee returns its f32/f64 result in v0 with the error tag in
// x0's high 32 bits (the packed fallible ABI). macho_emit_call_to_reg leaves x0 (the tag) untouched
// for a float result, so evaluate the call into v0, then test the tag. On error propagate by
// returning (x0 already carries the tag); on success the value is in v0 — move it to vreg if needed.
// Float sibling of macho_emit_check_to_reg_at.
static bool macho_emit_float_check_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left) return macho_diag_at(diag, "direct AArch64 Mach-O check requires a fallible call result", value->line, value->column, "non-fallible value");
  if (!macho_emit_float_value_to_vreg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  macho_emit_error_condition_reg(text, 8, 0);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 0);
  bool restore_process_args = ctx && ctx->seed_main_process_args && macho_is_main_function(fun);
  if (macho_function_propagates_to_process_exit(fun)) {
    macho_emit_epilogue(text, frame_size, restore_process_args);
  } else {
    z_aarch64_emit_movz_w(text, 0, 1);
    macho_emit_epilogue(text, frame_size, restore_process_args);
  }
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  if (vreg != 0) z_aarch64_emit_fmov_reg(text, vreg, 0, macho_type_is_f64(value->type));
  return true;
}

// `<float fallible call> rescue err <fallback>`: evaluate the call into v0 (tag in x0). On error
// (tag != 0) branch to the fallback, which lands its float result in the target reg; on success the
// value is already in v0. Float sibling of macho_emit_rescue_to_reg_at.
static bool macho_emit_float_rescue_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return macho_diag_at(diag, "direct AArch64 Mach-O rescue requires a fallible call and fallback", value->line, value->column, "unsupported rescue");
  if (!macho_emit_float_value_to_vreg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
  macho_emit_error_condition_reg(text, 8, 0);
  size_t fallback_patch = z_aarch64_emit_b_cond_placeholder(text, 1);
  if (vreg != 0) z_aarch64_emit_fmov_reg(text, vreg, 0, macho_type_is_f64(value->type));
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, fallback_patch, text->len);
  if (!macho_emit_float_value_to_vreg_at(text, fun, value->right, vreg, frame_size, scratch_slot, ctx, diag)) return false;
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

// Emit a float-typed value into an FP register (S/D). Mirrors the integer dispatcher: floats are
// always routed here from their context (local-set, return, binary/compare operands, call args,
// field store, casts). Non-commutative binaries evaluate left-first, spilling the left operand to
// the scratch slot across the right's evaluation; there is no multiply-add fusion.
static bool macho_emit_float_value_to_vreg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value) return macho_diag_at(diag, "direct AArch64 Mach-O float expression is missing", 1, 1, "missing expression");
  bool is64 = macho_type_is_f64(value->type);
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
      if (value->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O local index is out of range", value->line, value->column, "invalid local");
      if (!macho_type_is_float(fun->locals[value->local_index].type)) {
        return macho_diag_at(diag, "direct AArch64 Mach-O float load requires a float local", value->line, value->column, "non-float local");
      }
      macho_emit_load_local_v(text, fun, vreg, value->local_index, 0, is64, frame_size);
      return true;
    case IR_VALUE_FIELD_LOAD:
      if (value->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O field load record is out of range", value->line, value->column, "invalid record local");
      if (!fun->locals[value->local_index].is_record) return macho_diag_at(diag, "direct AArch64 Mach-O field load requires record local", value->line, value->column, "non-record local");
      macho_emit_load_local_v(text, fun, vreg, value->local_index, value->field_offset, is64, frame_size);
      return true;
    case IR_VALUE_CAST: {
      if (!value->left) return macho_diag_at(diag, "direct AArch64 Mach-O cast missing operand", value->line, value->column, "missing cast operand");
      IrTypeKind src = value->left->type;
      if (macho_type_is_float(src)) {
        // float -> float: widen/narrow when the widths differ, otherwise a plain move.
        if (!macho_emit_float_value_to_vreg_at(text, fun, value->left, vreg, frame_size, scratch_slot, ctx, diag)) return false;
        if (macho_type_is_f64(src) != is64) z_aarch64_emit_fcvt(text, vreg, vreg, is64);
        return true;
      }
      // integer -> float: unsigned u64 via UCVTF from x8, every other integer via SCVTF from w8.
      if (!macho_emit_value_to_reg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
      if (macho_type_is_scalar64(src) && macho_type_is_unsigned(src)) z_aarch64_emit_ucvtf_from_x(text, vreg, 8, is64);
      else z_aarch64_emit_scvtf_from_w(text, vreg, 8, is64);
      return true;
    }
    case IR_VALUE_BINARY: {
      if (value->binary_op != IR_BIN_ADD && value->binary_op != IR_BIN_SUB && value->binary_op != IR_BIN_MUL && value->binary_op != IR_BIN_DIV) {
        return macho_diag_at(diag, "direct AArch64 Mach-O float binary operator is unsupported", value->line, value->column, "unsupported operator");
      }
      if (!macho_emit_float_value_to_vreg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch_v(text, 8, is64, scratch_slot, value->left, diag)) return false;
      if (!macho_emit_float_value_to_vreg_at(text, fun, value->right, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!macho_emit_load_scratch_v(text, 8, is64, scratch_slot, value->left, diag)) return false;
      if (value->binary_op == IR_BIN_ADD) z_aarch64_emit_fadd(text, vreg, 8, 9, is64);
      else if (value->binary_op == IR_BIN_SUB) z_aarch64_emit_fsub(text, vreg, 8, 9, is64);
      else if (value->binary_op == IR_BIN_MUL) z_aarch64_emit_fmul(text, vreg, 8, 9, is64);
      else z_aarch64_emit_fdiv(text, vreg, 8, 9, is64);
      return true;
    }
    case IR_VALUE_CALL:
      // A float-returning Zero function: float args marshal to V0.., the result returns in v0 and is
      // moved to vreg, all handled by macho_emit_call_to_reg keyed off the value's float type.
      return macho_emit_call_to_reg(text, fun, value, vreg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_MATH_SQRTF:
    case IR_VALUE_MATH_EXPF:
    case IR_VALUE_MATH_COSF:
    case IR_VALUE_MATH_SINF:
    case IR_VALUE_MATH_FABSF:
    case IR_VALUE_MATH_FLOORF: {
      // Single-arg libm call (AAPCS FP ABI): argument in s0, result in s0. All std.math libm
      // helpers operate on f32. The undefined external + BRANCH26 relocation are resolved by the
      // host link step against libSystem (-lm).
      if (!macho_emit_float_value_to_vreg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
      size_t patch = z_aarch64_emit_bl_placeholder(text);
      if (!z_macho_record_math_call_patch(ctx, patch, z_macho_math_symbol_for_value(value->kind), value, diag)) return false;
      if (vreg != 0) z_aarch64_emit_fmov_reg(text, vreg, 0, false);
      return true;
    }
    case IR_VALUE_MATH_POWF: {
      // Two-arg libm call: arg0 in s0, arg1 in s1. Compute arg0 into s0, spill it to the FP scratch
      // slot across arg1's evaluation (which may itself call libm or use the scratch), then reload
      // arg0 into s0 just before the call.
      if (!macho_emit_float_value_to_vreg_at(text, fun, value->left, 0, frame_size, scratch_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch_v(text, 0, false, scratch_slot, value->left, diag)) return false;
      if (!macho_emit_float_value_to_vreg_at(text, fun, value->right, 1, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!macho_emit_load_scratch_v(text, 0, false, scratch_slot, value->left, diag)) return false;
      size_t patch = z_aarch64_emit_bl_placeholder(text);
      if (!z_macho_record_math_call_patch(ctx, patch, Z_MACHO_MATH_POWF, value, diag)) return false;
      if (vreg != 0) z_aarch64_emit_fmov_reg(text, vreg, 0, false);
      return true;
    }
    case IR_VALUE_CHECK:
      return macho_emit_float_check_to_vreg_at(text, fun, value, vreg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_RESCUE:
      return macho_emit_float_rescue_to_vreg_at(text, fun, value, vreg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_INDEX_LOAD: {
      // Float typed-span element read: element-scaled address then LDR s/d into the FP register.
      if (value->array_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O float index load array is out of range", value->line, value->column, "invalid span local");
      const IrLocal *local = &fun->locals[value->array_index];
      if (local->type != IR_TYPE_BYTE_VIEW || !macho_type_is_float(local->element_type)) {
        return macho_diag_at(diag, "direct AArch64 Mach-O float index load requires a float span", value->line, value->column, "non-float span element");
      }
      if (!macho_emit_span_index_addr(text, fun, value->array_index, value->index, frame_size, scratch_slot, ctx, diag)) return false;
      z_aarch64_emit_ldr_v_off(text, vreg, 9, 0, is64);
      return true;
    }
    case IR_VALUE_BYTE_VIEW_READ_FLOAT_LE: {
      // std.codec.readF32Le / readF64Le: read 4 or 8 little-endian bytes at a byte offset. AArch64 is
      // little-endian, so an FP load at ptr+offset yields the value. Bounds-check that offset+size
      // fits within the span length first: the unsigned compare offset+(size-1) < len is equivalent
      // to offset+size <= len and rejects offset overflow. Integer scratch x8..x10 stage the offset,
      // length, and element address before the value lands in the FP register.
      if (!value->left) return macho_diag_at(diag, "direct AArch64 Mach-O readF*Le requires a byte view", value->line, value->column, "missing byte view");
      if (!value->index) return macho_diag_at(diag, "direct AArch64 Mach-O readF*Le requires an offset", value->line, value->column, "missing offset");
      unsigned scalar_size = is64 ? 8u : 4u;
      IrTypeKind index_type = value->index->type;
      if (!macho_emit_value_to_reg_at(text, fun, value->index, 8, frame_size, scratch_slot, ctx, diag)) return false;
      if (!macho_emit_store_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
      if (!macho_emit_byte_view_len_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!macho_emit_load_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
      z_aarch64_emit_add_w_imm(text, 10, 8, scalar_size - 1u); // offset + (size - 1)
      macho_emit_u64_bounds_check(text, 10, 9);
      if (!macho_emit_byte_view_ptr_at(text, fun, value->left, 9, frame_size, scratch_slot + 1, ctx, diag)) return false;
      if (!macho_emit_load_scratch(text, 8, index_type, scratch_slot, value->index, diag)) return false;
      z_aarch64_emit_add_x_reg(text, 9, 9, 8);
      z_aarch64_emit_ldr_v_off(text, vreg, 9, 0, is64);
      return true;
    }
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported float value kind %d", value ? (int)value->kind : -1);
      return macho_diag_at(diag, "direct AArch64 Mach-O float value kind is unsupported", value->line, value->column, actual);
    }
  }
}

static bool macho_emit_value_to_reg_at(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned scratch_slot, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value) return macho_diag_at(diag, "direct AArch64 Mach-O expression is missing", 1, 1, "missing expression");
  switch (value->kind) {
    case IR_VALUE_BOOL: case IR_VALUE_INT:
      if (macho_type_is_scalar64(value->type)) z_aarch64_emit_movz_x(text, reg, (uint64_t)value->int_value);
      else z_aarch64_emit_movz_w(text, reg, (uint32_t)value->int_value);
      return true;
    case IR_VALUE_LOCAL:
      if (value->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O local index is out of range", value->line, value->column, "invalid local");
      if (fun->locals[value->local_index].type == IR_TYPE_BYTE_VIEW) {
        return macho_diag_at(diag, "direct AArch64 Mach-O byte-view local cannot be used as a scalar", value->line, value->column, "byte-view local");
      }
      if (macho_type_is_scalar64(fun->locals[value->local_index].type)) macho_emit_load_local_x(text, fun, reg, value->local_index, 0, frame_size);
      else macho_emit_load_local_w(text, fun, reg, value->local_index, 0, frame_size);
      return true;
    case IR_VALUE_CAST: return macho_emit_cast_value_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BINARY:
      return macho_emit_binary_value_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_COMPARE:
      return macho_emit_compare_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_MATH_ISNANF: {
      // isNaNf is inline (no libm symbol): NaN is the only value where x != x. FCMP sets V=1 on
      // unordered, so `cset reg, VS` yields 1 exactly for NaN.
      if (!macho_emit_float_value_to_vreg_at(text, fun, value->left, 8, frame_size, scratch_slot, ctx, diag)) return false;
      z_aarch64_emit_fcmp(text, 8, 8, macho_type_is_f64(value->left ? value->left->type : IR_TYPE_F32));
      macho_emit_cset(text, reg, 6); // VS (overflow set = unordered)
      return true;
    }
    case IR_VALUE_CALL:
      return macho_emit_call_to_reg(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_JSON_PARSE_BYTES:
      if (!macho_emit_json_parse_bytes_call_at(text, fun, value, frame_size, scratch_slot, ctx, diag)) return false;
      if (reg != 0) z_aarch64_emit_mov_x(text, reg, 0);
      return true;
    case IR_VALUE_JSON_VALIDATE_BYTES:
      if (!macho_emit_json_parse_bytes_call_at(text, fun, value, frame_size, scratch_slot, ctx, diag)) return false;
      z_aarch64_emit_cmp_x(text, 0, 31);
      z_aarch64_emit_movz_w(text, reg, 0);
      {
        size_t invalid = z_aarch64_emit_b_cond_placeholder(text, 11); // signed less than
        z_aarch64_emit_movz_w(text, reg, 1);
        z_aarch64_patch_cond19(text, invalid, text->len);
      }
      return true;
    case IR_VALUE_JSON_STREAM_TOKENS_BYTES:
      if (!macho_emit_json_parse_bytes_call_at(text, fun, value, frame_size, scratch_slot, ctx, diag)) return false;
      z_aarch64_emit_cmp_x(text, 0, 31);
      {
        size_t ok = z_aarch64_emit_b_cond_placeholder(text, 10); // signed greater or equal
        if (reg != 0) z_aarch64_emit_mov_x(text, reg, 31);
        else z_aarch64_emit_mov_x(text, 0, 31);
        size_t done = z_aarch64_emit_b_placeholder(text);
        z_aarch64_patch_cond19(text, ok, text->len);
        if (reg != 0) z_aarch64_emit_mov_x(text, reg, 0);
        z_aarch64_patch_branch26(text, done, text->len);
      }
      return true;
    case IR_VALUE_HTTP_FETCH:
      return macho_emit_http_fetch_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_HTTP_RESULT_OK:
    case IR_VALUE_HTTP_RESULT_STATUS:
    case IR_VALUE_HTTP_RESULT_BODY_LEN:
    case IR_VALUE_HTTP_RESULT_ERROR:
    case IR_VALUE_HTTP_HEADER_FOUND:
    case IR_VALUE_HTTP_HEADER_OFFSET:
    case IR_VALUE_HTTP_HEADER_LEN:
      return macho_emit_http_result_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_HTTP_RESPONSE_LEN:
    case IR_VALUE_HTTP_RESPONSE_HEADERS_LEN:
    case IR_VALUE_HTTP_RESPONSE_BODY_OFFSET:
      return macho_emit_http_response_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_HTTP_HEADER_VALUE:
      return macho_emit_http_header_value_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_VEC_LEN:
    case IR_VALUE_VEC_CAPACITY:
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_VEC) return macho_diag_at(diag, "direct AArch64 Mach-O Vec helper requires a Vec local", value->line, value->column, "invalid Vec local");
      macho_emit_load_local_w(text, fun, reg, value->local_index, value->kind == IR_VALUE_VEC_LEN ? 8 : 12, frame_size);
      return true;
    case IR_VALUE_VEC_PUSH:
      return macho_emit_vec_push_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_CHECK:
      return macho_emit_check_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_RESCUE:
      return macho_emit_rescue_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_ARGS_LEN:
      z_aarch64_emit_mov_w(text, reg, 20);
      return true;
    case IR_VALUE_FS_HOST:
      // std.fs.host() yields the host filesystem handle, an i32 token that carries no state on a
      // hosted target (the OS-interface lowerings call libSystem directly). Mirrors the ELF backend,
      // which returns 0.
      z_aarch64_emit_movz_w(text, reg, 0);
      return true;
    case IR_VALUE_FS_MUNMAP: {
      // std.fs.munmap(&mut m): release a mapping via libSystem _munmap(addr, len). The Mapping local
      // stores ptr@0 (64-bit) and len@8 (64-bit byte count, matching the byte-view store). Result
      // type is Void; nothing is produced into `reg`.
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_BYTE_VIEW) {
        return macho_diag_at(diag, "direct AArch64 Mach-O std.fs.munmap requires a Mapping local", value->line, value->column, "invalid Mapping");
      }
      macho_emit_load_local_x(text, fun, 0, value->local_index, 0, frame_size); // addr
      macho_emit_load_local_x(text, fun, 1, value->local_index, 8, frame_size); // len (64-bit byte count)
      size_t patch = z_aarch64_emit_bl_placeholder(text);
      if (!z_macho_record_libc_call_patch(ctx, patch, Z_MACHO_LIBC_MUNMAP, value, diag)) return false;
      return true;
    }
    case IR_VALUE_MAYBE_HAS:
      if (value->local_index >= fun->local_len ||
          (fun->locals[value->local_index].type != IR_TYPE_MAYBE_BYTE_VIEW && fun->locals[value->local_index].type != IR_TYPE_MAYBE_SCALAR)) {
        return macho_diag_at(diag, "direct AArch64 Mach-O maybe helper requires a Maybe local", value->line, value->column, "invalid maybe local");
      }
      macho_emit_load_local_w(text, fun, reg, value->local_index, 0, frame_size);
      return true;
    case IR_VALUE_BYTE_VIEW_LEN:
      return macho_emit_byte_view_len_at(text, fun, value->left, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_COPY:
      return macho_emit_byte_copy_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_FILL:
      return macho_emit_byte_fill_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_VIEW_EQ:
      return macho_emit_byte_view_eq_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_VIEW_INDEX_LOAD:
      return macho_emit_byte_view_index_load_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_BYTE_VIEW_READ_INT_LE:
      return macho_emit_byte_view_read_int_le_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_INDEX_LOAD:
      return macho_emit_index_load_to_reg_at(text, fun, value, reg, frame_size, scratch_slot, ctx, diag);
    case IR_VALUE_FIELD_LOAD:
      if (value->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O field load record is out of range", value->line, value->column, "invalid record local");
      if (!fun->locals[value->local_index].is_record) return macho_diag_at(diag, "direct AArch64 Mach-O field load requires record local", value->line, value->column, "non-record local");
      macho_emit_load_field(text, fun, reg, value->local_index, value->field_offset, value->type, frame_size);
      return true;
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported value kind %d", value ? (int)value->kind : -1);
      return macho_diag_at(diag, "direct AArch64 Mach-O value kind is unsupported", value->line, value->column, actual);
    }
  }
}

// A record-returning function receives its destination buffer pointer in x8 (the AAPCS indirect-
// result register). x8 is caller-saved and clobbered by every `bl`, so we spill it to a dedicated
// frame slot in the prologue and reload it for every `ret c` / field-store-into-sret site.
static bool macho_returns_record(const IrFunction *fun) {
  return fun && fun->return_type == IR_TYPE_RECORD;
}

// 16-byte slot reserved between the scratch region and the locals to hold the saved x8 (sret
// pointer), padded to keep the locals 16-aligned.
static unsigned macho_sret_reserved_bytes(const IrFunction *fun) {
  return macho_returns_record(fun) ? 16u : 0u;
}

// Frame-relative offset (from sp) of the sret slot itself — placed immediately above the scratch
// region so it never aliases a scratch slot or a local.
static unsigned macho_sret_slot_offset(const IrFunction *fun) {
  (void)fun;
  return MACHO_SCRATCH_SLOT_COUNT * MACHO_SCRATCH_SLOT_BYTES;
}

static size_t macho_function_frame_bytes(const IrFunction *fun) {
  uint32_t literal = 0;
  if (macho_is_literal_return_function(fun, &literal, NULL)) return 0;
  unsigned base = (unsigned)(fun ? (fun->frame_bytes ? fun->frame_bytes : fun->local_len * 8) : 0);
  return macho_align(base + MACHO_SCRATCH_SLOT_COUNT * MACHO_SCRATCH_SLOT_BYTES + macho_sret_reserved_bytes(fun), 16);
}

size_t z_macho64_stack_bytes_from_ir(const IrProgram *program) {
  size_t total = 0;
  for (size_t i = 0; program && i < program->function_len; i++) {
    total += macho_function_frame_bytes(&program->functions[i]);
  }
  return total;
}

size_t z_macho64_max_frame_bytes_from_ir(const IrProgram *program) {
  size_t max_frame = 0;
  for (size_t i = 0; program && i < program->function_len; i++) {
    size_t frame = macho_function_frame_bytes(&program->functions[i]);
    if (frame > max_frame) max_frame = frame;
  }
  return max_frame;
}

static unsigned macho_frame_size(const IrFunction *fun) {
  return (unsigned)macho_function_frame_bytes(fun);
}

static void macho_emit_epilogue(ZBuf *text, unsigned frame_size, bool restore_process_args) {
  if (frame_size > 0) z_aarch64_emit_add_sp_imm(text, frame_size);
  z_aarch64_emit_ldp_x29_x30_sp_post16(text);
  if (restore_process_args) z_aarch64_emit_ldp_x20_x21_sp_post16(text);
  z_aarch64_emit_ret(text);
}

static bool macho_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, unsigned frame_size, bool restore_process_args, MachOEmitContext *ctx, ZDiag *diag);

static bool macho_emit_world_write(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!instr || !instr->value) return macho_diag_at(diag, "direct AArch64 Mach-O World write requires bytes", instr ? instr->line : 1, instr ? instr->column : 1, "missing byte view");
  if (!macho_emit_byte_view_ptr(text, fun, instr->value, 1, frame_size, ctx, diag)) return false;
  if (!macho_emit_byte_view_len(text, fun, instr->value, 2, frame_size, ctx, diag)) return false;
  z_aarch64_emit_movz_w(text, 0, instr->field_offset == 2 ? 2u : 1u);
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_instr_runtime_patch(ctx, MACHO_RUNTIME_WORLD_WRITE, patch, instr, diag)) return false;
  size_t ok_patch = z_aarch64_emit_cbz_w_placeholder(text, 0);
  z_aarch64_emit_brk(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  return true;
}

static bool macho_emit_args_get_to_local(ZBuf *text, const IrFunction *fun, const IrValue *value, const IrLocal *local, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value || !value->left) return macho_diag_at(diag, "direct AArch64 Mach-O std.args.get requires an index", value ? value->line : 1, value ? value->column : 1, "missing index");
  if (!macho_emit_value_to_reg(text, fun, value->left, 10, frame_size, ctx, diag)) return false;
  z_aarch64_emit_cmp_w(text, 10, 20);
  size_t in_range = z_aarch64_emit_b_cond_placeholder(text, 3); // unsigned lower
  z_aarch64_emit_movz_w(text, 8, 0);
  macho_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);
  macho_emit_store_local_x(text, fun, 8, local->index, 8, frame_size);
  macho_emit_store_local_x(text, fun, 8, local->index, 16, frame_size);
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, in_range, text->len);

  z_aarch64_emit_add_x_reg_lsl(text, 12, 21, 10, 3);
  z_aarch64_emit_load_x_imm(text, 12, 12, 0);
  z_aarch64_emit_movz_w(text, 10, 0);
  size_t loop_start = text->len;
  z_aarch64_emit_add_x_reg(text, 13, 12, 10);
  z_aarch64_emit_load_b_imm(text, 14, 13, 0);
  size_t done_patch = z_aarch64_emit_cbz_w_placeholder(text, 14);
  z_aarch64_emit_add_w_imm(text, 10, 10, 1);
  size_t loop_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_branch26(text, loop_patch, loop_start);
  z_aarch64_patch_cond19(text, done_patch, text->len);

  z_aarch64_emit_movz_w(text, 8, 1);
  macho_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);
  macho_emit_store_local_x(text, fun, 12, local->index, 8, frame_size);
  macho_emit_store_local_x(text, fun, 10, local->index, 16, frame_size);
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

// Dedicated integer scratch slots, near the top of the fixed 32-slot scratch region, used to spill
// values (fd, addr, size) across `bl` libSystem calls in the mmap helpers. They sit above the slots
// expression evaluation grows into from base 0, so a path/size operand can be lowered without
// clobbering them. Three distinct slots are needed: fd, size, and addr each survive a call.
#define MACHO_MMAP_SCRATCH_FD 29u
#define MACHO_MMAP_SCRATCH_SIZE 30u
#define MACHO_MMAP_SCRATCH_ADDR 31u

static void macho_emit_spill_x(ZBuf *text, unsigned reg, unsigned slot) {
  z_aarch64_emit_store_x_sp(text, reg, slot * MACHO_SCRATCH_SLOT_BYTES);
}
static void macho_emit_reload_x(ZBuf *text, unsigned reg, unsigned slot) {
  z_aarch64_emit_load_x_sp(text, reg, slot * MACHO_SCRATCH_SLOT_BYTES);
}

// Open a file by path, measure its size, and map it read-only via libSystem. On return x0 holds the
// mapping address and x1 the byte length; a negative x0 signals the open/lseek/mmap failure path
// (the fd is closed first). Mirrors the ELF raw-syscall file-mmap sequence, but resolves the
// libSystem externals through the host link step. The size is held in a 64-bit slot, but stored
// back into the Mapping byte-view as a 32-bit count by the caller (matching the span len width);
// files >4 GiB are an accepted deferred limitation.
static bool macho_emit_mmap_file_addr_size(ZBuf *text, const IrFunction *fun, const IrValue *path, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  // _open(path, O_RDONLY=0)
  if (!macho_emit_byte_view_ptr_at(text, fun, path, 0, frame_size, 0, ctx, diag)) return false;
  z_aarch64_emit_movz_x(text, 1, 0); // O_RDONLY
  size_t open_patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, open_patch, Z_MACHO_LIBC_OPEN, path, diag)) return false;
  z_aarch64_emit_cmp_x(text, 0, 31);
  size_t open_fail = z_aarch64_emit_b_cond_placeholder(text, 11); // LT: negative fd -> open failure
  macho_emit_spill_x(text, 0, MACHO_MMAP_SCRATCH_FD); // save fd
  // _lseek(fd, 0, SEEK_END=2) -> file size
  z_aarch64_emit_movz_x(text, 1, 0); // offset = 0
  z_aarch64_emit_movz_x(text, 2, 2); // SEEK_END
  size_t seek_patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, seek_patch, Z_MACHO_LIBC_LSEEK, path, diag)) return false;
  z_aarch64_emit_cmp_x(text, 0, 31);
  size_t seek_fail = z_aarch64_emit_b_cond_placeholder(text, 11); // LT: lseek error
  macho_emit_spill_x(text, 0, MACHO_MMAP_SCRATCH_SIZE); // save size
  // _mmap(NULL, size, PROT_READ=1, MAP_PRIVATE=2, fd, 0)
  z_aarch64_emit_movz_x(text, 0, 0);                // addr = NULL (kernel chooses)
  macho_emit_reload_x(text, 1, MACHO_MMAP_SCRATCH_SIZE); // len = size
  z_aarch64_emit_movz_x(text, 2, 1);                // prot = PROT_READ
  z_aarch64_emit_movz_x(text, 3, 2);                // flags = MAP_PRIVATE
  macho_emit_reload_x(text, 4, MACHO_MMAP_SCRATCH_FD);   // fd
  z_aarch64_emit_movz_x(text, 5, 0);                // offset = 0
  size_t mmap_patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, mmap_patch, Z_MACHO_LIBC_MMAP, path, diag)) return false;
  // Close the fd on both the success and MAP_FAILED paths; preserve addr across the close, then hand
  // addr back in x0 and size in x1. A MAP_FAILED (negative) addr flows through.
  macho_emit_spill_x(text, 0, MACHO_MMAP_SCRATCH_ADDR); // save addr
  macho_emit_reload_x(text, 0, MACHO_MMAP_SCRATCH_FD);  // x0 = fd
  size_t close_patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, close_patch, Z_MACHO_LIBC_CLOSE, path, diag)) return false;
  macho_emit_reload_x(text, 0, MACHO_MMAP_SCRATCH_ADDR);  // x0 = addr (result)
  macho_emit_reload_x(text, 1, MACHO_MMAP_SCRATCH_SIZE);  // x1 = size (result)
  size_t done = z_aarch64_emit_b_placeholder(text);
  // Seek failure: close the fd and return the negative lseek result in x0.
  z_aarch64_patch_cond19(text, seek_fail, text->len);
  macho_emit_spill_x(text, 0, MACHO_MMAP_SCRATCH_ADDR); // preserve negative result across the close
  macho_emit_reload_x(text, 0, MACHO_MMAP_SCRATCH_FD);  // x0 = fd
  size_t seek_fail_close = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, seek_fail_close, Z_MACHO_LIBC_CLOSE, path, diag)) return false;
  macho_emit_reload_x(text, 0, MACHO_MMAP_SCRATCH_ADDR);  // x0 = negative result
  // open_fail lands here with x0 already holding the negative open result.
  z_aarch64_patch_cond19(text, open_fail, text->len);
  z_aarch64_patch_branch26(text, done, text->len);
  return true;
}

// Anonymous std.mem.pageAlloc allocation: a fresh kernel-zeroed region via libSystem _mmap (the
// macOS analog of ELF's MAP_ANON mmap, calloc semantics). The size value is evaluated then spilled
// so it survives the call, the AAPCS integer arguments are set up, and `bl _mmap` is recorded as an
// external relocation. The result populates the Maybe<MutSpan<u8>> dest local: on success has=1@0,
// ptr@8, len@16; on failure (MAP_FAILED, a negative return) the Maybe is cleared. Darwin map flags
// differ from Linux: MAP_ANON=0x1000 (Linux 0x20) and MAP_PRIVATE=0x2 so flags=0x1002;
// PROT_READ|PROT_WRITE=0x3; fd=-1.
static bool macho_emit_anon_mmap_to_local(ZBuf *text, const IrFunction *fun, const IrValue *size, const IrLocal *local, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!size) return macho_diag_at(diag, "direct AArch64 Mach-O page allocation requires a byte length", local ? local->line : 1, local ? local->column : 1, "missing length");
  if (!macho_emit_value_to_reg_at(text, fun, size, 9, frame_size, 0, ctx, diag)) return false;
  macho_emit_spill_x(text, 9, MACHO_MMAP_SCRATCH_SIZE); // preserve the length across the call
  z_aarch64_emit_mov_x(text, 1, 9);          // x1 = len
  z_aarch64_emit_movz_x(text, 0, 0);         // x0 = addr (NULL: kernel chooses)
  z_aarch64_emit_movz_x(text, 2, 3);         // x2 = prot = PROT_READ|PROT_WRITE
  z_aarch64_emit_movz_x(text, 3, 0x1002);    // x3 = flags = MAP_ANON|MAP_PRIVATE (Darwin)
  z_aarch64_emit_movn_x(text, 4, 0);         // x4 = fd = -1
  z_aarch64_emit_movz_x(text, 5, 0);         // x5 = offset = 0
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, patch, Z_MACHO_LIBC_MMAP, size, diag)) return false;
  // mmap returns MAP_FAILED (-1) on error; a valid user-space address is never negative.
  z_aarch64_emit_cmp_x(text, 0, 31);
  size_t fail = z_aarch64_emit_b_cond_placeholder(text, 11); // signed less than -> failure
  z_aarch64_emit_movz_w(text, 9, 1);
  macho_emit_store_local_w(text, fun, 9, local->index, 0, frame_size);  // has = 1
  macho_emit_store_local_x(text, fun, 0, local->index, 8, frame_size);  // ptr
  macho_emit_reload_x(text, 9, MACHO_MMAP_SCRATCH_SIZE);
  macho_emit_store_local_x(text, fun, 9, local->index, 16, frame_size); // len (64-bit byte count)
  size_t end = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, fail, text->len);
  z_aarch64_emit_movz_w(text, 9, 0);
  macho_emit_store_local_w(text, fun, 9, local->index, 0, frame_size);  // has = 0
  macho_emit_store_local_x(text, fun, 9, local->index, 8, frame_size);
  macho_emit_store_local_x(text, fun, 9, local->index, 16, frame_size);
  z_aarch64_patch_branch26(text, end, text->len);
  return true;
}

static bool macho_emit_local_set_byte_view(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  // A span-returning call leaves ptr in x0 and len in x1; store both straight into the slot.
  if (instr->value && instr->value->kind == IR_VALUE_CALL) {
    if (!macho_emit_call_to_reg(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
    macho_emit_store_local_x(text, fun, 0, instr->local_index, 0, frame_size);
    macho_emit_store_local_x(text, fun, 1, instr->local_index, 8, frame_size);
    return true;
  }
  if (!macho_emit_byte_view_ptr(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
  macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
  if (!macho_emit_byte_view_len(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
  macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
  return true;
}

static bool macho_emit_local_set_alloc(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->value && instr->value->kind == IR_VALUE_PAGE_ALLOC) {
    // A PageAlloc carries no pre-reserved buffer — each std.mem.allocBytes performs a fresh
    // anonymous _mmap (see macho_emit_anon_mmap_to_local). Zero the allocator slot so it holds no
    // stale pointer/length; mirrors the ELF64 page-alloc init.
    z_aarch64_emit_movz_x(text, 8, 0);
    macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
    macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
    return true;
  }
  if (!instr->value || instr->value->kind != IR_VALUE_FIXED_BUF_ALLOC) return macho_diag_at(diag, "direct AArch64 Mach-O FixedBufAlloc local requires std.mem.fixedBufAlloc", instr->line, instr->column, "unsupported allocator initializer");
  if (!macho_emit_byte_view_ptr(text, fun, instr->value->left, 8, frame_size, ctx, diag)) return false;
  macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
  if (!macho_emit_byte_view_len(text, fun, instr->value->left, 8, frame_size, ctx, diag)) return false;
  macho_emit_store_local_w(text, fun, 8, instr->local_index, 8, frame_size);
  z_aarch64_emit_movz_w(text, 8, 0);
  macho_emit_store_local_w(text, fun, 8, instr->local_index, 12, frame_size);
  return true;
}

static bool macho_emit_local_set_vec(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!instr->value || instr->value->kind != IR_VALUE_VEC_INIT) return macho_diag_at(diag, "direct AArch64 Mach-O Vec local requires std.mem.vec", instr->line, instr->column, "unsupported Vec initializer");
  if (!macho_emit_byte_view_ptr(text, fun, instr->value->left, 8, frame_size, ctx, diag)) return false;
  macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
  z_aarch64_emit_movz_w(text, 8, 0);
  macho_emit_store_local_w(text, fun, 8, instr->local_index, 8, frame_size);
  if (!macho_emit_byte_view_len(text, fun, instr->value->left, 8, frame_size, ctx, diag)) return false;
  macho_emit_store_local_w(text, fun, 8, instr->local_index, 12, frame_size);
  return true;
}

static bool macho_emit_local_set_maybe_byte_view(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->value && instr->value->kind == IR_VALUE_ARGS_GET) {
    return macho_emit_args_get_to_local(text, fun, instr->value, &fun->locals[instr->local_index], frame_size, ctx, diag);
  }
  if (instr->value && instr->value->kind == IR_VALUE_FS_MMAP) {
    // `let m = std.fs.mmap(fs, path)` -> Maybe<owned<Mapping>> (has@0, ptr@8, len@16). The helper
    // lands addr in x0 and the 64-bit byte length in x1; a negative addr is the not-found/failure
    // path, which clears the Maybe.
    const IrLocal *local = &fun->locals[instr->local_index];
    if (!macho_emit_mmap_file_addr_size(text, fun, instr->value->left, frame_size, ctx, diag)) return false;
    z_aarch64_emit_cmp_x(text, 0, 31);
    size_t fail = z_aarch64_emit_b_cond_placeholder(text, 11); // LT: MAP_FAILED / open failure
    z_aarch64_emit_mov_x(text, 9, 0); // preserve addr across the has store
    z_aarch64_emit_movz_w(text, 8, 1);
    macho_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);  // has = 1
    macho_emit_store_local_x(text, fun, 9, local->index, 8, frame_size);  // ptr
    macho_emit_store_local_x(text, fun, 1, local->index, 16, frame_size); // len (64-bit byte count)
    size_t end = z_aarch64_emit_b_placeholder(text);
    z_aarch64_patch_cond19(text, fail, text->len);
    z_aarch64_emit_movz_w(text, 8, 0);
    macho_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);  // has = 0
    macho_emit_store_local_x(text, fun, 8, local->index, 8, frame_size);
    macho_emit_store_local_x(text, fun, 8, local->index, 16, frame_size);
    z_aarch64_patch_branch26(text, end, text->len);
    return true;
  }
  if (!instr->value || instr->value->kind != IR_VALUE_ALLOC_BYTES || instr->value->local_index >= fun->local_len || fun->locals[instr->value->local_index].type != IR_TYPE_ALLOC) return macho_diag_at(diag, "direct AArch64 Mach-O allocation source is invalid", instr->line, instr->column, "invalid allocation");
  if (fun->locals[instr->value->local_index].is_page_alloc) {
    // PageAlloc: each allocation is a fresh kernel-zeroed _mmap region (calloc semantics), rather
    // than a bump out of a pre-reserved buffer.
    return macho_emit_anon_mmap_to_local(text, fun, instr->value->left, &fun->locals[instr->local_index], frame_size, ctx, diag);
  }
  if (!macho_emit_value_to_reg(text, fun, instr->value->left, 10, frame_size, ctx, diag)) return false;
  macho_emit_load_local_w(text, fun, 8, instr->value->local_index, 12, frame_size);
  macho_emit_load_local_w(text, fun, 9, instr->value->local_index, 8, frame_size);
  z_aarch64_emit_add_w_imm(text, 11, 8, 0);
  macho_emit_binary_reg(text, IR_BIN_ADD, 11, 11, 10, false);
  z_aarch64_emit_cmp_w(text, 11, 9);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 9); // unsigned lower or same
  z_aarch64_emit_movz_w(text, 8, 0);
  macho_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
  macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
  macho_emit_store_local_x(text, fun, 8, instr->local_index, 16, frame_size);
  size_t end_patch = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  z_aarch64_emit_movz_w(text, 12, 1);
  macho_emit_store_local_w(text, fun, 12, instr->local_index, 0, frame_size);
  macho_emit_load_local_x(text, fun, 12, instr->value->local_index, 0, frame_size);
  z_aarch64_emit_add_x_reg(text, 12, 12, 8);
  macho_emit_store_local_x(text, fun, 12, instr->local_index, 8, frame_size);
  macho_emit_store_local_x(text, fun, 10, instr->local_index, 16, frame_size);
  macho_emit_store_local_w(text, fun, 11, instr->value->local_index, 12, frame_size);
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

static bool macho_emit_local_set_maybe_scalar_json_parse(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->value->local_index >= fun->local_len || fun->locals[instr->value->local_index].type != IR_TYPE_ALLOC) {
    return macho_diag_at(diag, "direct AArch64 Mach-O JSON parse allocator is invalid", instr->line, instr->column, "invalid allocator");
  }
  if (!macho_emit_value_to_reg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
  z_aarch64_emit_cmp_x(text, 8, 31);
  size_t fail = z_aarch64_emit_b_cond_placeholder(text, 11); // signed less than
  macho_emit_load_local_w(text, fun, 9, instr->value->local_index, 12, frame_size);
  z_aarch64_emit_mov_w(text, 10, 8);
  macho_emit_binary_reg(text, IR_BIN_ADD, 11, 9, 10, false);
  macho_emit_load_local_w(text, fun, 12, instr->value->local_index, 8, frame_size);
  z_aarch64_emit_cmp_w(text, 11, 12);
  size_t overflow = z_aarch64_emit_b_cond_placeholder(text, 8); // unsigned higher
  z_aarch64_emit_movz_w(text, 9, 1);
  macho_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
  macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
  macho_emit_store_local_w(text, fun, 11, instr->value->local_index, 12, frame_size);
  size_t end = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, fail, text->len);
  z_aarch64_patch_cond19(text, overflow, text->len);
  z_aarch64_emit_movz_w(text, 9, 0);
  macho_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
  macho_emit_store_local_x(text, fun, 9, instr->local_index, 8, frame_size);
  z_aarch64_patch_branch26(text, end, text->len);
  return true;
}

static bool macho_emit_local_set_maybe_scalar(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!instr->value) return macho_diag_at(diag, "direct AArch64 Mach-O Maybe scalar initializer is missing", instr->line, instr->column, "missing maybe value");
  if (instr->value->kind == IR_VALUE_MAYBE_SCALAR_LITERAL) {
    z_aarch64_emit_movz_w(text, 8, instr->value->data_len ? 1u : 0u);
    macho_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
    z_aarch64_emit_movz_x(text, 8, (uint64_t)instr->value->int_value);
    macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
    return true;
  }
  if (instr->value->kind == IR_VALUE_JSON_PARSE_BYTES) {
    return macho_emit_local_set_maybe_scalar_json_parse(text, fun, instr, frame_size, ctx, diag);
  }
  if (!macho_emit_value_to_reg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
  z_aarch64_emit_cmp_x(text, 8, 31);
  size_t fail = z_aarch64_emit_b_cond_placeholder(text, 11); // signed less than
  z_aarch64_emit_movz_w(text, 9, 1);
  macho_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
  macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
  size_t end = z_aarch64_emit_b_placeholder(text);
  z_aarch64_patch_cond19(text, fail, text->len);
  z_aarch64_emit_movz_w(text, 9, 0);
  macho_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
  macho_emit_store_local_x(text, fun, 9, instr->local_index, 8, frame_size);
  z_aarch64_patch_branch26(text, end, text->len);
  return true;
}

static bool macho_emit_local_set_scalar(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!macho_emit_value_to_reg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
  if (macho_type_is_scalar64(fun->locals[instr->local_index].type)) macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
  else macho_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
  return true;
}

static bool macho_emit_local_set_float(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  bool is64 = macho_type_is_f64(fun->locals[instr->local_index].type);
  if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
  macho_emit_store_local_v(text, fun, 8, instr->local_index, 0, is64, frame_size);
  return true;
}

// `let q = check f()` where f returns a record and raises. Issue the record-returning
// call (writes the record via sret into q's slot AND writes the tag to x1: 0 on success, error
// code on failure). Then test x1: on success fall through; on failure propagate. The record
// buffer is undefined on failure — the caller's CHECK semantics ensure it isn't read.
//
// Mirrors macho_emit_check_to_reg_at combined with macho_emit_record_call_with_dest.
static bool macho_emit_local_set_record_check(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  const IrValue *check = instr->value;
  if (!check->left || check->left->kind != IR_VALUE_CALL || check->left->type != IR_TYPE_RECORD) {
    return macho_diag_at(diag, "direct AArch64 Mach-O record check requires a record-returning fallible call", check->line, check->column, "unsupported record check");
  }
  if (!macho_emit_record_call_with_dest(text, fun, (int)fun->locals[instr->local_index].index, check->left, frame_size, 0, ctx, diag)) return false;
  // Test the tag in x1 (low 32 bits carry the error code). On zero fall through.
  z_aarch64_emit_cmp_x(text, 1, 31);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 0); // b.eq ok
  bool restore_process_args = ctx && ctx->seed_main_process_args && macho_is_main_function(fun);
  if (macho_function_propagates_to_process_exit(fun)) {
    // Propagation by caller-return shape:
    //  - We return a record AND raise: x1 already carries the tag; reload x0 from the sret slot
    //    so the caller sees its valid pointer alongside non-zero x1.
    //  - We are hosted-main (Void return mapped to I32 exit code): main has no sret; route the
    //    tag from x1 into x0 (low 32) so the OS reads the error code as the exit code.
    //  - We are a non-record raising fn: the packed-tag ABI puts the tag in x0's HIGH 32 bits.
    //    Compose x0 = (x1 << 32) by shifting; use ubfiz x0, x1, #32, #32.
    if (fun->return_type == IR_TYPE_RECORD) {
      z_aarch64_emit_load_x_imm(text, 0, 31, macho_sret_slot_offset(fun));
    } else if (macho_is_main_function(fun)) {
      z_aarch64_emit_mov_x(text, 0, 1);
    } else {
      // ubfiz x0, x1, #32, #32 (== ubfm x0, x1, #32, #31): bits [31:0] of x1 → bits [63:32] of x0.
      // Encoding: sf=1 opc=10 N=1 immr=(64-32)&63=32 imms=32-1=31 Rn=1 Rd=0 = 0xD360_7C20.
      z_aarch64_append_u32(text, 0xd3607c20u);
    }
    macho_emit_epilogue(text, frame_size, restore_process_args);
  } else {
    // Non-propagating context shouldn't reach here (CHECK is illegal in a non-fallible context).
    z_aarch64_emit_movz_w(text, 0, 1);
    macho_emit_epilogue(text, frame_size, restore_process_args);
  }
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  return true;
}

// `let q = f() rescue r0` where f returns a record and raises. Issue the call with q as
// sret target; on success (x1 == 0) the record is already in q. On failure, materialize the
// fallback record into q — either by record_copy_to (fallback is a record local) or another
// record-returning call (fallback is itself a call). Mirrors macho_emit_rescue_to_reg_at
// combined with macho_emit_record_call_with_dest.
static bool macho_emit_local_set_record_rescue(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  const IrValue *rescue = instr->value;
  const IrLocal *dest = &fun->locals[instr->local_index];
  if (!rescue->left || rescue->left->kind != IR_VALUE_CALL || rescue->left->type != IR_TYPE_RECORD) {
    return macho_diag_at(diag, "direct AArch64 Mach-O record rescue requires a record-returning fallible call", rescue->line, rescue->column, "unsupported record rescue");
  }
  if (!rescue->right || (rescue->right->kind != IR_VALUE_LOCAL && rescue->right->kind != IR_VALUE_CALL)) {
    return macho_diag_at(diag, "direct AArch64 Mach-O record rescue fallback must be a record local or call", rescue->line, rescue->column, "unsupported rescue fallback");
  }
  if (!macho_emit_record_call_with_dest(text, fun, (int)dest->index, rescue->left, frame_size, 0, ctx, diag)) return false;
  z_aarch64_emit_cmp_x(text, 1, 31);
  size_t fallback_patch = z_aarch64_emit_b_cond_placeholder(text, 1); // b.ne fallback
  size_t end_patch = z_aarch64_emit_b_placeholder(text); // success: skip fallback
  z_aarch64_patch_cond19(text, fallback_patch, text->len);
  if (rescue->right->kind == IR_VALUE_LOCAL) {
    if (rescue->right->local_index >= fun->local_len) {
      return macho_diag_at(diag, "direct AArch64 Mach-O record rescue fallback local is out of range", rescue->right->line, rescue->right->column, "invalid fallback local");
    }
    macho_emit_record_copy_to(text, fun, dest->index, rescue->right->local_index, frame_size);
  } else {
    if (!macho_emit_record_call_with_dest(text, fun, (int)dest->index, rescue->right, frame_size, 0, ctx, diag)) return false;
  }
  z_aarch64_patch_branch26(text, end_patch, text->len);
  return true;
}

static bool macho_emit_local_set_record(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  const IrLocal *local = &fun->locals[instr->local_index];
  // `let q = check f()` and `let q = f() rescue r0` — fallible record-returning calls.
  if (instr->value && instr->value->kind == IR_VALUE_CHECK) {
    return macho_emit_local_set_record_check(text, fun, instr, frame_size, ctx, diag);
  }
  if (instr->value && instr->value->kind == IR_VALUE_RESCUE) {
    return macho_emit_local_set_record_rescue(text, fun, instr, frame_size, ctx, diag);
  }
  // `let q = f()` — bind a record-returning call straight into q's slot via sret (no copy).
  if (instr->value && instr->value->kind == IR_VALUE_CALL) {
    return macho_emit_record_call_with_dest(text, fun, (int)local->index, instr->value, frame_size, 0, ctx, diag);
  }
  // `let q = p` / `q = p` — record-to-record value copy (span fields ride along as raw bytes).
  if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
    macho_emit_record_copy_to(text, fun, local->index, instr->value->local_index, frame_size);
    return true;
  }
  return macho_diag_at(diag, "direct AArch64 Mach-O record local assignment requires a record value", instr->line, instr->column, "unsupported record set");
}

static bool macho_emit_local_set(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O local store is out of range", instr->line, instr->column, "invalid local");
  if (fun->locals[instr->local_index].is_record) return macho_emit_local_set_record(text, fun, instr, frame_size, ctx, diag);
  switch (fun->locals[instr->local_index].type) {
    case IR_TYPE_BYTE_VIEW: return macho_emit_local_set_byte_view(text, fun, instr, frame_size, ctx, diag);
    case IR_TYPE_ALLOC: return macho_emit_local_set_alloc(text, fun, instr, frame_size, ctx, diag);
    case IR_TYPE_VEC: return macho_emit_local_set_vec(text, fun, instr, frame_size, ctx, diag);
    case IR_TYPE_MAYBE_BYTE_VIEW: return macho_emit_local_set_maybe_byte_view(text, fun, instr, frame_size, ctx, diag);
    case IR_TYPE_MAYBE_SCALAR: return macho_emit_local_set_maybe_scalar(text, fun, instr, frame_size, ctx, diag);
    case IR_TYPE_F32: case IR_TYPE_F64: return macho_emit_local_set_float(text, fun, instr, frame_size, ctx, diag);
    default: return macho_emit_local_set_scalar(text, fun, instr, frame_size, ctx, diag);
  }
}

static bool macho_emit_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, bool restore_process_args, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->kind == IR_INSTR_WORLD_WRITE) {
    return macho_emit_world_write(text, fun, instr, frame_size, ctx, diag);
  }
  if (instr->kind == IR_INSTR_LOCAL_SET) return macho_emit_local_set(text, fun, instr, frame_size, ctx, diag);
  if (instr->kind == IR_INSTR_FIELD_STORE) {
    // local_index == UINT_MAX targets the record being returned, written through the saved sret
    // pointer (`return <shape literal>` stores its fields this way without a temporary local).
    if (instr->local_index == UINT_MAX) {
      return macho_emit_sret_field_store(text, fun, instr, frame_size, ctx, diag);
    }
    if (instr->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O field store record is out of range", instr->line, instr->column, "invalid record local");
    if (!fun->locals[instr->local_index].is_record) return macho_diag_at(diag, "direct AArch64 Mach-O field store requires record local", instr->line, instr->column, "non-record local");
    // Field store through a ref<Record> is rejected — `set p.x …` requires mutref<Record>.
    if (fun->locals[instr->local_index].is_ref && !fun->locals[instr->local_index].is_mutable) {
      return macho_diag_at(diag, "direct AArch64 Mach-O field store through ref<Record> requires mutref", instr->line, instr->column, fun->locals[instr->local_index].name ? fun->locals[instr->local_index].name : "ref-record");
    }
    if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
      // Span field: store ptr at the field offset and the 64-bit byte/element-count len 8 bytes
      // higher. A span-returning call leaves ptr in x0 and len in x1; any other byte view
      // materializes via the ptr/len helpers.
      bool target_is_ref = fun->locals[instr->local_index].is_ref;
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!macho_emit_call_to_reg(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
        if (target_is_ref) {
          macho_emit_load_ref_record_ptr(text, fun, 9, instr->local_index, frame_size);
          z_aarch64_emit_store_x_imm(text, 0, 9, instr->field_offset);
          z_aarch64_emit_store_x_imm(text, 1, 9, instr->field_offset + 8);
        } else {
          macho_emit_store_local_x(text, fun, 0, instr->local_index, instr->field_offset, frame_size);
          macho_emit_store_local_x(text, fun, 1, instr->local_index, instr->field_offset + 8, frame_size);
        }
        return true;
      }
      if (!macho_emit_byte_view_ptr(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      if (target_is_ref) {
        macho_emit_load_ref_record_ptr(text, fun, 9, instr->local_index, frame_size);
        z_aarch64_emit_store_x_imm(text, 8, 9, instr->field_offset);
      } else {
        macho_emit_store_local_x(text, fun, 8, instr->local_index, instr->field_offset, frame_size);
      }
      if (!macho_emit_byte_view_len(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      if (target_is_ref) {
        macho_emit_load_ref_record_ptr(text, fun, 9, instr->local_index, frame_size);
        z_aarch64_emit_store_x_imm(text, 8, 9, instr->field_offset + 8);
      } else {
        macho_emit_store_local_x(text, fun, 8, instr->local_index, instr->field_offset + 8, frame_size);
      }
      return true;
    }
    if (instr->value && macho_type_is_float(instr->value->type)) {
      bool is64 = macho_type_is_f64(instr->value->type);
      if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      macho_emit_store_local_v(text, fun, 8, instr->local_index, instr->field_offset, is64, frame_size);
      return true;
    }
    if (!macho_emit_value_to_reg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
    macho_emit_store_field(text, fun, 8, instr->local_index, instr->field_offset, instr->value ? instr->value->type : IR_TYPE_I32, frame_size);
    return true;
  }
  if (instr->kind == IR_INSTR_INDEX_STORE) {
    if (instr->array_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O indexed store array is out of range", instr->line, instr->column, "invalid array local");
    const IrLocal *local = &fun->locals[instr->array_index];
    unsigned const_index = 0;
    if (local->type == IR_TYPE_BYTE_VIEW) {
      // Typed-span element store. The value is materialized before the address so the address
      // scratch (x8/x9) survives. Float elements store via STR s/d; i8/u8 truncate to a byte
      // (STRB); 8-byte elements via STR x; else STR w.
      IrTypeKind elem = local->element_type;
      if (macho_type_is_float(elem)) {
        bool is64 = macho_type_is_f64(elem);
        if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
        if (!macho_emit_span_index_addr(text, fun, instr->array_index, instr->index, frame_size, 0, ctx, diag)) return false;
        z_aarch64_emit_str_v_off(text, 8, 9, 0, is64);
        return true;
      }
      if (!macho_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
      if (!macho_emit_span_index_addr(text, fun, instr->array_index, instr->index, frame_size, 0, ctx, diag)) return false;
      if (elem == IR_TYPE_U8 || elem == IR_TYPE_BOOL || elem == IR_TYPE_I8) z_aarch64_emit_store_b_imm(text, 10, 9, 0);
      else if (macho_elem_byte_size(elem) == 8) z_aarch64_emit_store_x_imm(text, 10, 9, 0);
      else z_aarch64_emit_store_w_imm(text, 10, 9, 0);
      return true;
    }
    if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE) &&
        macho_const_u32_value(instr->index, &const_index) && const_index < local->array_len) {
      if (!macho_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
      macho_emit_store_local_w(text, fun, 10, instr->array_index, const_index * 4u, frame_size);
      return true;
    }
    if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
      if (!macho_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
      if (!instr->index || !macho_emit_value_to_reg(text, fun, instr->index, 8, frame_size, ctx, diag)) return false;
      z_aarch64_emit_movz_w(text, 9, local->array_len);
      macho_emit_u32_bounds_check(text, 8, 9);
      z_aarch64_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, instr->array_index, 0, frame_size));
      z_aarch64_emit_add_x_reg_lsl(text, 9, 9, 8, 2);
      z_aarch64_emit_store_w_imm(text, 10, 9, 0);
      return true;
    }
    if (!local->is_array || (local->element_type != IR_TYPE_U8 && local->element_type != IR_TYPE_BOOL)) return macho_diag_at(diag, "direct AArch64 Mach-O indexed store requires [N]u8, [N]Bool, or integer arrays", instr->line, instr->column, "unsupported array local");
    if (!macho_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
    if (!instr->index || !macho_emit_value_to_reg(text, fun, instr->index, 8, frame_size, ctx, diag)) return false;
    z_aarch64_emit_movz_w(text, 9, local->array_len);
    macho_emit_u32_bounds_check(text, 8, 9);
    z_aarch64_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, instr->array_index, 0, frame_size));
    z_aarch64_emit_add_x_reg(text, 9, 9, 8);
    z_aarch64_emit_store_b_imm(text, 10, 9, 0);
    return true;
  }
  if (instr->kind == IR_INSTR_EXPR) {
    return !instr->value || macho_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag);
  }
  if (instr->kind == IR_INSTR_RETURN) {
    if (instr->value && instr->value->type == IR_TYPE_RECORD) {
      // Record return through the caller's sret pointer (x8 spilled in our prologue). A
      // record-returning call passes our sret pointer straight through; a record local is copied
      // byte-by-byte into the caller's buffer. `ret check f()` and `ret f() rescue r0`
      // route the inner call's sret straight into our own sret target (no temp local), with the
      // CHECK/RESCUE tag testing the second-return register x1 between the call and the return.
      const IrValue *call_for_check = NULL;
      const IrValue *rescue_fallback = NULL;
      if (instr->value->kind == IR_VALUE_CHECK && instr->value->left && instr->value->left->kind == IR_VALUE_CALL) {
        call_for_check = instr->value->left;
      } else if (instr->value->kind == IR_VALUE_RESCUE && instr->value->left && instr->value->left->kind == IR_VALUE_CALL) {
        call_for_check = instr->value->left;
        rescue_fallback = instr->value->right;
      }
      if (call_for_check) {
        if (!macho_emit_record_call_with_dest(text, fun, -1, call_for_check, frame_size, 0, ctx, diag)) return false;
        if (!rescue_fallback) {
          // CHECK path: x1 carries inner call's tag. Reload x0 (sret pointer) and epilogue —
          // on success x1 is 0 (set by inner callee), on failure x1 propagates.
          z_aarch64_emit_load_x_imm(text, 0, 31, macho_sret_slot_offset(fun));
          macho_emit_epilogue(text, frame_size, restore_process_args);
          return true;
        }
        // RESCUE path: on success (x1==0), reload x0 and return. On failure, materialize
        // fallback into our sret target, clear x1 (we're succeeding now), then return.
        z_aarch64_emit_cmp_x(text, 1, 31);
        size_t fallback_patch = z_aarch64_emit_b_cond_placeholder(text, 1); // b.ne fallback
        z_aarch64_emit_load_x_imm(text, 0, 31, macho_sret_slot_offset(fun));
        if (fun->raises) z_aarch64_emit_movz_w(text, 1, 0);
        macho_emit_epilogue(text, frame_size, restore_process_args);
        z_aarch64_patch_cond19(text, fallback_patch, text->len);
        if (rescue_fallback->kind == IR_VALUE_LOCAL) {
          if (rescue_fallback->local_index >= fun->local_len) {
            return macho_diag_at(diag, "direct AArch64 Mach-O record return rescue fallback local is out of range", rescue_fallback->line, rescue_fallback->column, "invalid fallback local");
          }
          macho_emit_record_copy_to(text, fun, UINT_MAX, rescue_fallback->local_index, frame_size);
        } else if (rescue_fallback->kind == IR_VALUE_CALL) {
          if (!macho_emit_record_call_with_dest(text, fun, -1, rescue_fallback, frame_size, 0, ctx, diag)) return false;
        } else {
          return macho_diag_at(diag, "direct AArch64 Mach-O record return rescue fallback must be a record local or call", rescue_fallback->line, rescue_fallback->column, "unsupported rescue fallback");
        }
        z_aarch64_emit_load_x_imm(text, 0, 31, macho_sret_slot_offset(fun));
        if (fun->raises) z_aarch64_emit_movz_w(text, 1, 0);
        macho_emit_epilogue(text, frame_size, restore_process_args);
        return true;
      }
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!macho_emit_record_call_with_dest(text, fun, -1, instr->value, frame_size, 0, ctx, diag)) return false;
      } else if (instr->value->kind == IR_VALUE_LOCAL) {
        macho_emit_record_copy_to(text, fun, UINT_MAX, instr->value->local_index, frame_size);
      } else {
        return macho_diag_at(diag, "direct AArch64 Mach-O record return must be a record local or call", instr->line, instr->column, "unsupported record return");
      }
      // Return the sret pointer in x0 (AAPCS indirect-result convention also defines x0 = result on
      // return from a record-returning function); reload it from the saved slot.
      z_aarch64_emit_load_x_imm(text, 0, 31, macho_sret_slot_offset(fun));
      // A raising record-returning function uses x1 as the error tag carrier; clear it on
      // a successful return so callers see tag=0. (RAISE writes the error code to x1 directly.)
      if (fun->raises) z_aarch64_emit_movz_w(text, 1, 0);
      macho_emit_epilogue(text, frame_size, restore_process_args);
      return true;
    }
    if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
      // Span return: ptr in x0, len in x1. A span-returning call already lands both there; any other
      // byte view is materialized ptr-then-len. The length stays a 32-bit element count.
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!macho_emit_call_to_reg(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
      } else {
        if (!macho_emit_byte_view_ptr(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
        if (!macho_emit_byte_view_len(text, fun, instr->value, 1, frame_size, ctx, diag)) return false;
      }
      macho_emit_epilogue(text, frame_size, restore_process_args);
      return true;
    }
    if (instr->value && macho_type_is_float(instr->value->type)) {
      if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
      // A raising function returns its float result in v0; the value GPR x0 is otherwise unused for
      // a float payload, so clear it to flag "no error" (the high-32 tag reads as 0). Integer
      // payloads instead carry the tag in x0 directly via their value width.
      if (fun->raises) z_aarch64_emit_movz_x(text, 0, 0);
    } else if (instr->value && !macho_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) {
      return false;
    }
    if (fun->raises && !instr->value) z_aarch64_emit_movz_x(text, 0, 0);
    macho_emit_epilogue(text, frame_size, restore_process_args);
    return true;
  }
  if (instr->kind == IR_INSTR_RAISE) {
    if (!macho_function_propagates_to_process_exit(fun)) return macho_diag_at(diag, "direct AArch64 Mach-O raise requires a fallible function context", instr->line, instr->column, "non-fallible context");
    // A raising record-returning function carries the error tag in the low 32 bits of x1
    // (x0 is reserved for the sret pointer per AAPCS). Every other raising function packs the tag
    // into the high 32 bits of x0 via the packed-tag scheme.
    if (fun->return_type == IR_TYPE_RECORD) {
      z_aarch64_emit_movz_w(text, 1, instr->error_code ? instr->error_code : IR_ERROR_UNKNOWN);
      // x0 still holds the sret pointer (it was loaded at the prologue or preserved); reload it
      // here to be safe so the caller sees a valid pointer alongside the non-zero tag.
      z_aarch64_emit_load_x_imm(text, 0, 31, macho_sret_slot_offset(fun));
    } else {
      macho_emit_packed_error_reg(text, 0, instr->error_code ? instr->error_code : IR_ERROR_UNKNOWN);
    }
    macho_emit_epilogue(text, frame_size, restore_process_args);
    return true;
  }
  if (instr->kind == IR_INSTR_IF) {
    if (!macho_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
    size_t false_patch = z_aarch64_emit_cbz_w_placeholder(text, 0);
    if (!macho_emit_instrs(text, fun, instr->then_instrs, instr->then_len, frame_size, restore_process_args, ctx, diag)) return false;
    if (instr->else_len > 0) {
      size_t end_patch = z_aarch64_emit_b_placeholder(text);
      z_aarch64_patch_cond19(text, false_patch, text->len);
      if (!macho_emit_instrs(text, fun, instr->else_instrs, instr->else_len, frame_size, restore_process_args, ctx, diag)) return false;
      z_aarch64_patch_branch26(text, end_patch, text->len);
    } else {
      z_aarch64_patch_cond19(text, false_patch, text->len);
    }
    return true;
  }
  if (instr->kind == IR_INSTR_WHILE) {
    size_t loop_start = text->len;
    if (!macho_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
    size_t false_patch = z_aarch64_emit_cbz_w_placeholder(text, 0);
    if (!macho_emit_instrs(text, fun, instr->then_instrs, instr->then_len, frame_size, restore_process_args, ctx, diag)) return false;
    size_t loop_patch = z_aarch64_emit_b_placeholder(text);
    z_aarch64_patch_branch26(text, loop_patch, loop_start);
    z_aarch64_patch_cond19(text, false_patch, text->len);
    return true;
  }
  char actual[64];
  snprintf(actual, sizeof(actual), "unsupported instruction kind %d", instr ? (int)instr->kind : -1);
  return macho_diag_at(diag, "direct AArch64 Mach-O instruction kind is unsupported", instr->line, instr->column, actual);
}

static bool macho_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, unsigned frame_size, bool restore_process_args, MachOEmitContext *ctx, ZDiag *diag) {
  for (size_t i = 0; i < len; i++) {
    if (!macho_emit_instr(text, fun, &instrs[i], frame_size, restore_process_args, ctx, diag)) return false;
  }
  return true;
}

static bool macho_validate_function(const IrFunction *fun, ZDiag *diag) {
  uint32_t ignored = 0;
  if (macho_is_literal_return_function(fun, &ignored, NULL)) return true;
  unsigned abi_slots = 0;
  for (size_t i = 0; i < fun->param_count; i++) {
    abi_slots += macho_abi_slots_for_param(&fun->locals[i]);
    if (abi_slots > 8) return macho_diag_at(diag, "direct AArch64 Mach-O object backend supports at most eight ABI argument slots", fun->line, fun->column, fun->name);
  }
  // Returns: Void, primitive integer, float, a record (via the x8 sret pointer), or a span (ptr in
  // x0, len in x1). A raising function can return a record — the record flows via sret as
  // usual, and the error tag rides in the low 32 bits of x1 (free for record-returning calls).
  // A span-returning raising function is still rejected since x1 IS the span len carrier.
  if (fun->return_type != IR_TYPE_VOID && fun->return_type != IR_TYPE_RECORD && fun->return_type != IR_TYPE_BYTE_VIEW &&
      !macho_type_is_scalar(fun->return_type) && !macho_type_is_float(fun->return_type)) {
    return macho_diag_at(diag, "direct AArch64 Mach-O object backend currently supports only Void, primitive integer, float, record, and span returns", fun->line, fun->column, fun->name);
  }
  if (fun->raises && fun->return_type == IR_TYPE_BYTE_VIEW) {
    return macho_diag_at(diag, "direct AArch64 Mach-O object backend cannot return a span from a raising function", fun->line, fun->column, fun->name);
  }
  for (size_t i = 0; i < fun->local_len; i++) {
    if (fun->locals[i].type == IR_TYPE_BYTE_VIEW) {
      continue;
    }
    if (fun->locals[i].is_array && (fun->locals[i].element_type == IR_TYPE_U8 || fun->locals[i].element_type == IR_TYPE_BOOL ||
                                    fun->locals[i].element_type == IR_TYPE_U32 || fun->locals[i].element_type == IR_TYPE_I32 || fun->locals[i].element_type == IR_TYPE_USIZE)) continue;
    if (fun->locals[i].is_record) continue;
    if (fun->locals[i].type == IR_TYPE_ALLOC || fun->locals[i].type == IR_TYPE_MAYBE_BYTE_VIEW || fun->locals[i].type == IR_TYPE_MAYBE_SCALAR) continue;
    if (fun->locals[i].type == IR_TYPE_VEC) continue;
    if (fun->locals[i].is_array || (!macho_type_is_scalar(fun->locals[i].type) && !macho_type_is_float(fun->locals[i].type))) {
      return macho_diag_at(diag, "direct AArch64 Mach-O object backend currently supports only primitive scalar locals", fun->locals[i].line, fun->locals[i].column, fun->locals[i].name);
    }
  }
  return true;
}

static bool macho_emit_function_text(ZBuf *text, const IrFunction *fun, MachOEmitContext *ctx, ZDiag *diag) {
  uint32_t literal = 0;
  if (macho_is_literal_return_function(fun, &literal, NULL)) {
    z_aarch64_emit_literal_return(text, literal);
    return true;
  }

  unsigned frame_size = macho_frame_size(fun);
  bool seed_process_args = ctx && ctx->seed_main_process_args && macho_is_main_function(fun);
  if (seed_process_args) z_aarch64_emit_stp_x20_x21_sp_pre16(text);
  z_aarch64_emit_stp_x29_x30_sp_pre16(text);
  z_aarch64_emit_mov_x29_sp(text);
  if (frame_size > 0) z_aarch64_emit_sub_sp_imm(text, frame_size);
  if (seed_process_args) {
    z_aarch64_emit_mov_x(text, 20, 0);
    z_aarch64_emit_mov_x(text, 21, 1);
  }
  // A record-returning function gets the destination address in x8 (the AAPCS indirect-result
  // register, not an argument register). Save it so it survives the body's calls; field stores and
  // the return reload it from this slot.
  if (macho_returns_record(fun)) z_aarch64_emit_store_x_imm(text, 8, 31, macho_sret_slot_offset(fun));
  unsigned abi_slot = 0;
  unsigned fp_abi_slot = 0;
  for (size_t i = 0; i < fun->param_count; i++) {
    const IrLocal *local = &fun->locals[i];
    if (macho_type_is_float(local->type)) {
      // Float params arrive in the AAPCS FP bank (V0,V1,..), counted independently of X0-X7.
      macho_emit_store_local_v(text, fun, fp_abi_slot, (unsigned)i, 0, macho_type_is_f64(local->type), frame_size);
      fp_abi_slot++;
      continue;
    }
    unsigned slots = macho_abi_slots_for_param(local);
    if (abi_slot + slots > 8) return macho_diag_at(diag, "direct AArch64 Mach-O function has too many ABI argument slots", fun->line, fun->column, fun->name);
    if (local->type == IR_TYPE_BYTE_VIEW) {
      macho_emit_store_local_x(text, fun, abi_slot, (unsigned)i, 0, frame_size);
      macho_emit_store_local_x(text, fun, abi_slot + 1, (unsigned)i, 8, frame_size);
    } else if (local->is_record && local->is_ref) {
      // ref<Record> / mutref<Record> param. AAPCS passes a pointer in one int reg;
      // store it in the first 8 bytes of the local's slot. Field load/store go through the
      // pointer (no inline copy — that's what differentiates ref-record from by-value record).
      macho_emit_store_local_x(text, fun, abi_slot, (unsigned)i, 0, frame_size);
    } else if (local->is_record) {
      // Record param: AAPCS passes a pointer in one int reg; copy the pointed-to bytes into the
      // local's slot so the body sees value semantics.
      macho_emit_copy_record_param(text, fun, (unsigned)i, abi_slot, frame_size);
    } else if (macho_type_is_scalar64(local->type)) {
      macho_emit_store_local_x(text, fun, abi_slot, (unsigned)i, 0, frame_size);
    } else {
      macho_emit_store_local_w(text, fun, abi_slot, (unsigned)i, 0, frame_size);
    }
    abi_slot += slots;
  }
  if (!macho_emit_instrs(text, fun, fun->instrs, fun->instr_len, frame_size, seed_process_args, ctx, diag)) return false;
  if (fun->instr_len == 0 || (fun->instrs[fun->instr_len - 1].kind != IR_INSTR_RETURN && fun->instrs[fun->instr_len - 1].kind != IR_INSTR_RAISE)) macho_emit_epilogue(text, frame_size, seed_process_args);
  return true;
}

static unsigned macho_rodata_base_offset(const IrProgram *program) {
  if (!program || program->data_segment_len == 0) return 0;
  unsigned base = program->data_segments[0].offset;
  for (size_t i = 1; i < program->data_segment_len; i++) {
    if (program->data_segments[i].offset < base) base = program->data_segments[i].offset;
  }
  return base;
}

static void macho_append_rodata(ZBuf *rodata, const IrProgram *program, unsigned base_offset) {
  for (size_t i = 0; program && i < program->data_segment_len; i++) {
    const IrDataSegment *segment = &program->data_segments[i];
    macho_pad_to(rodata, segment->offset - base_offset);
    append_bytes(rodata, (const char *)segment->bytes, segment->len);
  }
}

typedef struct {
  ZBuf text, rodata, relocs, strings;
  size_t *offsets;
  uint32_t *function_string_offsets, runtime_string_offsets[MACHO_RUNTIME_HELPER_COUNT];
  uint32_t math_string_offsets[Z_MACHO_MATH_COUNT];
  uint32_t libc_string_offsets[Z_MACHO_LIBC_COUNT];
  ZMachOSymbol *symbols;
  size_t symbol_len;
  uint32_t symbol_count, rodata_string_offset;
  bool has_rodata;
  unsigned rodata_base_offset;
  MachOEmitContext ctx;
} MachOObjectBuild;

static bool macho_validate_object_program(const IrProgram *program, ZDiag *diag) {
  if (!program) return macho_diag(diag, "direct Mach-O backend received no program");
  if (!program->mir_valid) {
    bool ok = macho_diag_at(diag, program->mir_message[0] ? program->mir_message : "direct backend lowering failed", program->mir_line, program->mir_column, program->mir_actual);
    z_diag_set_backend_blocker(diag, &program->backend_blocker);
    return ok;
  }
  if (program->function_len == 0) return macho_diag_at(diag, "direct AArch64 Mach-O object backend requires at least one exported function", 1, 1, "empty program");
  bool has_export = false;
  for (size_t i = 0; i < program->function_len; i++) {
    if (program->functions[i].is_exported) has_export = true;
    if (!macho_validate_function(&program->functions[i], diag)) return false;
  }
  if (!has_export) return macho_diag_at(diag, "direct AArch64 Mach-O object backend requires at least one exported function", 1, 1, "no exported function");
  return true;
}

static void macho_object_build_free(MachOObjectBuild *build) {
  if (!build) return;
  free(build->symbols);
  z_macho_emit_context_free(&build->ctx);
  free(build->function_string_offsets);
  free(build->offsets);
  zbuf_free(&build->strings);
  zbuf_free(&build->relocs);
  zbuf_free(&build->rodata);
  zbuf_free(&build->text);
}

static bool macho_object_build_init(MachOObjectBuild *build, const IrProgram *program, ZDiag *diag) {
  memset(build, 0, sizeof(*build));
  zbuf_init(&build->text);
  zbuf_init(&build->rodata);
  zbuf_init(&build->relocs);
  zbuf_init(&build->strings);
  build->has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  build->rodata_base_offset = macho_rodata_base_offset(program);
  if (build->has_rodata) macho_append_rodata(&build->rodata, program, build->rodata_base_offset);
  build->offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  build->function_string_offsets = z_checked_calloc(program->function_len, sizeof(uint32_t));
  if (!build->offsets || !build->function_string_offsets) {
    macho_object_build_free(build);
    return macho_diag(diag, build->offsets ? "out of memory while emitting Mach-O symbols" : "out of memory while emitting Mach-O object");
  }
  append_u8(&build->strings, 0);
  build->ctx = (MachOEmitContext){
    .program = program,
    .function_offsets = build->offsets,
    .function_count = program->function_len,
    .rodata_base_offset = build->rodata_base_offset,
    .pie_relative_data = true,
    .seed_main_process_args = true
  };
  return true;
}

static bool macho_object_emit_functions(MachOObjectBuild *build, const IrProgram *program, ZDiag *diag) {
  for (size_t i = 0; i < program->function_len; i++) {
    const IrFunction *fun = &program->functions[i];
    macho_pad_to(&build->text, macho_align(build->text.len, 4));
    build->offsets[i] = build->text.len;
    if (!macho_emit_function_text(&build->text, fun, &build->ctx, diag)) return false;
    build->function_string_offsets[i] = (uint32_t)build->strings.len;
    zbuf_append_char(&build->strings, '_');
    zbuf_append(&build->strings, fun->name ? fun->name : "zero_fn");
    append_u8(&build->strings, 0);
  }
  return true;
}

static void macho_object_append_relocations(MachOObjectBuild *build, const IrProgram *program) {
  z_macho_append_call_relocations(&build->relocs, &build->ctx);
  if (build->has_rodata) z_macho_append_data_relocations(&build->relocs, &build->ctx, (unsigned)program->function_len);
  uint32_t next_symbol = (uint32_t)program->function_len + (build->has_rodata ? 1u : 0u);
  for (unsigned helper = 0; helper < MACHO_RUNTIME_HELPER_COUNT; helper++) {
    MachORuntimeHelper runtime_helper = (MachORuntimeHelper)helper;
    if (z_macho_runtime_patch_count(&build->ctx, runtime_helper) == 0) continue;
    z_macho_append_runtime_relocations(&build->relocs, &build->ctx, runtime_helper, next_symbol++);
  }
  // libm externals follow the runtime helpers in symbol-table order. The same Z_MACHO_MATH_* order
  // is reused by the strtab/nlist construction below, keeping symbol indices consistent.
  for (unsigned m = 0; m < Z_MACHO_MATH_COUNT; m++) {
    MachOMathSymbol symbol = (MachOMathSymbol)m;
    if (!z_macho_math_symbol_used(&build->ctx, symbol)) continue;
    z_macho_append_math_call_relocations(&build->relocs, &build->ctx, symbol, next_symbol++);
  }
  // libSystem (libc) externals follow the libm symbols in symbol-table order. The same
  // Z_MACHO_LIBC_* order is reused by the strtab/nlist construction below, keeping indices aligned.
  for (unsigned c = 0; c < Z_MACHO_LIBC_COUNT; c++) {
    MachOLibcSymbol symbol = (MachOLibcSymbol)c;
    if (!z_macho_libc_symbol_used(&build->ctx, symbol)) continue;
    z_macho_append_libc_call_relocations(&build->relocs, &build->ctx, symbol, next_symbol++);
  }
  build->symbol_count = next_symbol;
}

static void macho_object_append_symbol_strings(MachOObjectBuild *build) {
  if (build->has_rodata) {
    build->rodata_string_offset = (uint32_t)build->strings.len;
    zbuf_append(&build->strings, "l_.zero_rodata");
    append_u8(&build->strings, 0);
  }
  for (unsigned helper = 0; helper < MACHO_RUNTIME_HELPER_COUNT; helper++) {
    MachORuntimeHelper runtime_helper = (MachORuntimeHelper)helper;
    if (z_macho_runtime_patch_count(&build->ctx, runtime_helper) == 0) continue;
    build->runtime_string_offsets[helper] = (uint32_t)build->strings.len;
    zbuf_append(&build->strings, z_macho_runtime_helper_symbol(runtime_helper));
    append_u8(&build->strings, 0);
  }
  for (unsigned m = 0; m < Z_MACHO_MATH_COUNT; m++) {
    MachOMathSymbol symbol = (MachOMathSymbol)m;
    if (!z_macho_math_symbol_used(&build->ctx, symbol)) continue;
    build->math_string_offsets[m] = (uint32_t)build->strings.len;
    zbuf_append(&build->strings, z_macho_math_symbol_name(symbol));
    append_u8(&build->strings, 0);
  }
  for (unsigned c = 0; c < Z_MACHO_LIBC_COUNT; c++) {
    MachOLibcSymbol symbol = (MachOLibcSymbol)c;
    if (!z_macho_libc_symbol_used(&build->ctx, symbol)) continue;
    build->libc_string_offsets[c] = (uint32_t)build->strings.len;
    zbuf_append(&build->strings, z_macho_libc_symbol_name(symbol));
    append_u8(&build->strings, 0);
  }
}

static bool macho_object_build_symbols(MachOObjectBuild *build, const IrProgram *program, ZDiag *diag) {
  build->symbols = z_checked_calloc(build->symbol_count, sizeof(ZMachOSymbol));
  if (!build->symbols) return macho_diag(diag, "out of memory while emitting Mach-O symbols");
  for (size_t i = 0; i < program->function_len; i++) {
    build->symbols[build->symbol_len++] = (ZMachOSymbol){
      .string_offset = build->function_string_offsets[i],
      .type = program->functions[i].is_exported ? 0x0f : 0x0e,
      .section = 1,
      .value = build->offsets[i]
    };
  }
  if (build->has_rodata) {
    build->symbols[build->symbol_len++] = (ZMachOSymbol){
      .string_offset = build->rodata_string_offset,
      .type = 0x0e,
      .section = 2,
      .value = (uint32_t)macho_align(build->text.len, 8)
    };
  }
  for (unsigned helper = 0; helper < MACHO_RUNTIME_HELPER_COUNT; helper++) {
    MachORuntimeHelper runtime_helper = (MachORuntimeHelper)helper;
    if (z_macho_runtime_patch_count(&build->ctx, runtime_helper) == 0) continue;
    build->symbols[build->symbol_len++] = (ZMachOSymbol){ .string_offset = build->runtime_string_offsets[helper], .type = 0x01 };
  }
  // libm externals (sqrtf/expf/...), emitted in symbol-index order to match the relocations.
  for (unsigned m = 0; m < Z_MACHO_MATH_COUNT; m++) {
    MachOMathSymbol symbol = (MachOMathSymbol)m;
    if (!z_macho_math_symbol_used(&build->ctx, symbol)) continue;
    build->symbols[build->symbol_len++] = (ZMachOSymbol){ .string_offset = build->math_string_offsets[m], .type = 0x01 };
  }
  // libSystem externals (mmap/munmap/open/lseek/close), in symbol-index order after the libm set.
  for (unsigned c = 0; c < Z_MACHO_LIBC_COUNT; c++) {
    MachOLibcSymbol symbol = (MachOLibcSymbol)c;
    if (!z_macho_libc_symbol_used(&build->ctx, symbol)) continue;
    build->symbols[build->symbol_len++] = (ZMachOSymbol){ .string_offset = build->libc_string_offsets[c], .type = 0x01 };
  }
  return true;
}

static void macho_object_write(const MachOObjectBuild *build, ZBuf *out) {
  ZMachOObjectImage image = {
    .text = &build->text,
    .rodata = build->has_rodata ? &build->rodata : NULL,
    .relocs = &build->relocs,
    .strings = &build->strings,
    .symbols = build->symbols,
    .symbol_len = build->symbol_len,
    .text_reloc_count = (uint32_t)z_macho_text_relocation_count(&build->ctx)
  };
  z_macho_write_object64(out, &image);
}

bool z_emit_macho64_object_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!out) return macho_diag(diag, "direct Mach-O backend received no output buffer");
  if (!macho_validate_object_program(program, diag)) return false;
  MachOObjectBuild build;
  if (!macho_object_build_init(&build, program, diag)) return false;
  bool ok = macho_object_emit_functions(&build, program, diag);
  if (ok) {
    macho_object_append_relocations(&build, program);
    macho_object_append_symbol_strings(&build);
    ok = macho_object_build_symbols(&build, program, diag);
  }
  if (ok) macho_object_write(&build, out);
  macho_object_build_free(&build);
  return ok;
}

static const IrFunction *macho_find_executable_main(const IrProgram *program, ZDiag *diag, unsigned *out_index) {
  const IrFunction *fun = NULL;
  unsigned index = 0;
  for (size_t i = 0; program && i < program->function_len; i++) {
    if (macho_is_main_function(&program->functions[i])) {
      if (fun) {
        macho_diag_at(diag, "direct AArch64 Mach-O executable backend requires exactly one exported main function", program->functions[i].line, program->functions[i].column, program->functions[i].name);
        return NULL;
      }
      fun = &program->functions[i];
      index = (unsigned)i;
    }
  }
  if (!fun) {
    macho_diag_at(diag, "direct AArch64 Mach-O executable backend requires an exported main function", 1, 1, "missing main");
    return NULL;
  }
  if (fun->param_count != 0) {
    macho_diag_at(diag, "direct AArch64 Mach-O executable main must not take parameters", fun->line, fun->column, fun->name);
    return NULL;
  }
  if (fun->return_type != IR_TYPE_VOID && !macho_type_is_scalar32(fun->return_type)) {
    macho_diag_at(diag, "direct AArch64 Mach-O executable main must return Void or a 32-bit-or-smaller scalar", fun->line, fun->column, fun->name);
    return NULL;
  }
  if (out_index) *out_index = index;
  return fun;
}

static size_t macho_emit_exe_start_stub(ZBuf *text) {
  z_aarch64_emit_mov_x(text, 20, 0);
  z_aarch64_emit_mov_x(text, 21, 1);
  size_t patch = z_aarch64_emit_b_placeholder(text); // tail-call main so it returns to dyld's LC_MAIN trampoline
  return patch;
}

static size_t macho_emit_exe_world_write(ZBuf *text) {
  size_t offset = text->len;
  z_aarch64_emit_movz_x(text, 16, 0x02000004u); // Darwin SYS_write(fd=x0, buf=x1, len=x2)
  z_aarch64_emit_svc(text, 0x80);
  z_aarch64_emit_movz_w(text, 0, 0);   // report success to the checked std.io shim
  z_aarch64_emit_ret(text);
  return offset;
}

typedef struct {
  ZBuf text, rodata, rebase;
  size_t *offsets, start_call_patch;
  ZMachOExecutableLayout layout;
  MachOEmitContext ctx;
  unsigned main_index, rodata_base_offset;
  bool has_rodata;
} MachOExeBuild;

static bool macho_validate_exe_program(const IrProgram *program, unsigned *main_index, ZDiag *diag) {
  if (!program->mir_valid) {
    bool ok = macho_diag_at(diag, program->mir_message[0] ? program->mir_message : "direct backend lowering failed", program->mir_line, program->mir_column, program->mir_actual);
    z_diag_set_backend_blocker(diag, &program->backend_blocker);
    return ok;
  }
  if (!macho_find_executable_main(program, diag, main_index)) return false;
  for (size_t i = 0; i < program->function_len; i++) if (!macho_validate_function(&program->functions[i], diag)) return false;
  return true;
}

static void macho_exe_build_free(MachOExeBuild *build) {
  if (!build) return;
  z_macho_emit_context_free(&build->ctx);
  free(build->offsets);
  zbuf_free(&build->rebase); zbuf_free(&build->rodata); zbuf_free(&build->text);
}

static bool macho_exe_build_init(MachOExeBuild *build, const IrProgram *program, unsigned main_index, ZDiag *diag) {
  memset(build, 0, sizeof(*build));
  zbuf_init(&build->text); zbuf_init(&build->rodata); zbuf_init(&build->rebase);
  build->main_index = main_index;
  build->has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  build->rodata_base_offset = macho_rodata_base_offset(program);
  if (build->has_rodata) macho_append_rodata(&build->rodata, program, build->rodata_base_offset);
  build->offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  if (!build->offsets) {
    macho_exe_build_free(build);
    return macho_diag(diag, "out of memory while emitting Mach-O executable");
  }
  build->ctx = (MachOEmitContext){
    .program = program,
    .function_offsets = build->offsets,
    .function_count = program->function_len,
    .rodata_base_offset = build->rodata_base_offset,
    .pie_relative_data = true
  };
  build->start_call_patch = macho_emit_exe_start_stub(&build->text);
  macho_pad_to(&build->text, macho_align(build->text.len, 16));
  return true;
}

static bool macho_exe_emit_functions(MachOExeBuild *build, const IrProgram *program, ZDiag *diag) {
  for (size_t i = 0; i < program->function_len; i++) {
    macho_pad_to(&build->text, macho_align(build->text.len, 4));
    build->offsets[i] = build->text.len;
    if (!macho_emit_function_text(&build->text, &program->functions[i], &build->ctx, diag)) return false;
  }
  return true;
}

static bool macho_exe_validate_runtime(const MachOExeBuild *build, ZDiag *diag) {
  return !z_macho_has_unsupported_exe_runtime_patches(&build->ctx) || macho_diag_at(diag, "direct AArch64 Mach-O executable runtime helpers require object emission and an explicit runtime link step", 1, 1, "use --emit obj and link zero_runtime.c");
}

static void macho_exe_patch_branches(MachOExeBuild *build) {
  size_t world_write_offset = 0;
  if (z_macho_runtime_patch_count(&build->ctx, MACHO_RUNTIME_WORLD_WRITE) > 0) {
    macho_pad_to(&build->text, macho_align(build->text.len, 4));
    world_write_offset = macho_emit_exe_world_write(&build->text);
  }
  z_aarch64_patch_branch26(&build->text, build->start_call_patch, build->offsets[build->main_index]);
  for (size_t i = 0; i < build->ctx.call_patch_len; i++) {
    const MachOCallPatch *patch = &build->ctx.call_patches[i];
    z_aarch64_patch_branch26(&build->text, patch->patch_offset, build->offsets[patch->callee_index]);
  }
  const MachOPatchList *world_write_patches = z_macho_runtime_patch_list(&build->ctx, MACHO_RUNTIME_WORLD_WRITE);
  for (size_t i = 0; world_write_patches && i < world_write_patches->len; i++) {
    z_aarch64_patch_branch26(&build->text, world_write_patches->items[i].patch_offset, world_write_offset);
  }
}

static void macho_exe_patch_data(MachOExeBuild *build, const char *code_signature_id) {
  z_macho_compute_executable64_layout(&build->layout, &build->text, build->has_rodata ? &build->rodata : NULL, &build->rebase, code_signature_id);
  for (size_t i = 0; i < build->ctx.data_patch_len; i++) {
    const MachODataPatch *patch = &build->ctx.data_patches[i];
    uint64_t addr = build->layout.base_addr + build->layout.rodata_offset + (patch->data_offset - build->rodata_base_offset);
    z_aarch64_patch_adrp_add(&build->text, patch->patch_offset, build->layout.base_addr + build->layout.text_offset + patch->patch_offset, addr);
  }
  z_macho_compute_executable64_layout(&build->layout, &build->text, build->has_rodata ? &build->rodata : NULL, &build->rebase, code_signature_id);
}

static void macho_exe_write(const MachOExeBuild *build, ZBuf *out, const char *code_signature_id) {
  ZMachOExecutableImage image = { .text = &build->text, .rodata = build->has_rodata ? &build->rodata : NULL, .rebase = &build->rebase, .layout = build->layout, .code_signature_id = code_signature_id };
  z_macho_write_executable64(out, &image);
}

bool z_emit_macho64_exe_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!program || !out) return macho_diag(diag, "direct Mach-O executable backend received no program");
  unsigned main_index = 0;
  if (!macho_validate_exe_program(program, &main_index, diag)) return false;
  MachOExeBuild build;
  if (!macho_exe_build_init(&build, program, main_index, diag)) return false;
  bool ok = macho_exe_emit_functions(&build, program, diag) && macho_exe_validate_runtime(&build, diag);
  if (ok) {
    const char *code_signature_id = "zero-direct";
    macho_exe_patch_branches(&build);
    macho_exe_patch_data(&build, code_signature_id);
    macho_exe_write(&build, out, code_signature_id);
  }
  macho_exe_build_free(&build);
  return ok;
}
