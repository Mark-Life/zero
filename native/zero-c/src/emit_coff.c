#include "zero.h"
#include "coff_emit_state.h"
#include "coff_format.h"
#include "x64_emit.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool coff_diag(ZDiag *diag, const char *message) {
  if (diag) {
    diag->code = 4004;
    diag->line = 1;
    diag->column = 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct MIR subset");
    snprintf(diag->actual, sizeof(diag->actual), "unsupported feature");
    snprintf(diag->help, sizeof(diag->help), "reduce the program to primitive direct-backend constructs or choose a supported direct target");
  }
  return false;
}

static bool coff_diag_at(ZDiag *diag, const char *message, int line, int column, const char *actual) {
  if (diag) {
    diag->code = 4004;
    diag->line = line > 0 ? line : 1;
    diag->column = column > 0 ? column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct COFF x64 object MVP subset");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "unsupported feature");
    snprintf(diag->help, sizeof(diag->help), "reduce the program to primitive direct-backend constructs or choose a supported direct target");
  }
  return false;
}

static bool coff_type_is_scalar32(IrTypeKind type) { return type == IR_TYPE_BOOL || type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_I32 || type == IR_TYPE_U32 || type == IR_TYPE_USIZE; }

// 64-bit integer scalars (i64/u64). Locals of this width are stored/loaded via the
// `wide=true` rbp-disp helpers so we keep the full 64 bits across spills.
static bool coff_type_is_scalar64(IrTypeKind type) { return type == IR_TYPE_I64 || type == IR_TYPE_U64; }

static bool coff_type_is_f64(IrTypeKind type) { return type == IR_TYPE_F64; }
static bool coff_type_is_float(IrTypeKind type) { return type == IR_TYPE_F32 || type == IR_TYPE_F64; }
static bool coff_type_is_i64(IrTypeKind type) { return type == IR_TYPE_I64 || type == IR_TYPE_U64; }

// Typed-span element size for bytesAs*<T> reinterprets. Matches the macho_x64
// machx64_type_byte_size table: byte (u8/i8/Bool) = 1, u16 = 2, 64-bit (i64/u64/f64) = 8, else 4.
static unsigned coff_elem_byte_size(IrTypeKind element_type) {
  if (element_type == IR_TYPE_U8 || element_type == IR_TYPE_I8 || element_type == IR_TYPE_BOOL) return 1;
  if (element_type == IR_TYPE_U16) return 2;
  if (coff_type_is_i64(element_type) || coff_type_is_f64(element_type)) return 8;
  return 4;
}

// log2(coff_elem_byte_size). Only sizes 1/2/4/8 occur, so 0/1/2/3.
static unsigned coff_elem_byte_size_log2(IrTypeKind element_type) {
  unsigned size = coff_elem_byte_size(element_type);
  return size == 1 ? 0u : size == 2 ? 1u : size == 4 ? 2u : 3u;
}

static void coff_emit_cast_normalize_rax(ZBuf *text, IrTypeKind target) {
  switch (target) {
    case IR_TYPE_BOOL:
    case IR_TYPE_U8:
      z_x64_emit_and_reg_u32(text, 0, 0xff, false);
      return;
    case IR_TYPE_U16:
      z_x64_emit_and_reg_u32(text, 0, 0xffff, false);
      return;
    case IR_TYPE_I32:
    case IR_TYPE_U32:
    case IR_TYPE_USIZE:
      z_x64_emit_mov_reg_from_reg(text, 0, 0, false);
      return;
    default:
      return;
  }
}

static unsigned coff_local_offset(const IrFunction *fun, unsigned local_index) {
  if (fun && local_index < fun->local_len && fun->locals[local_index].frame_offset > 0) return fun->locals[local_index].frame_offset;
  return (local_index + 1) * 8;
}

static unsigned coff_local_slot_offset(const IrFunction *fun, unsigned local_index, unsigned slot_offset) { unsigned offset = coff_local_offset(fun, local_index); return offset >= slot_offset ? offset - slot_offset : offset; }

// Record/aggregate ABI. Win64 reserves rcx (param_regs[0]) for a caller-allocated sret
// buffer pointer when a function returns a record. The callee saves it into a fixed frame slot
// just past the regular locals so a later RETURN can hand it back in rax. The slot stays 16-byte
// aligned via the +16 in coff_emit_function_text.
static bool coff_returns_record(const IrFunction *fun) {
  return fun && fun->return_type == IR_TYPE_RECORD;
}

static unsigned coff_base_stack_size(const IrFunction *fun) {
  return (unsigned)z_coff_align(fun ? fun->frame_bytes : 0, 16);
}

static unsigned coff_sret_slot_offset(const IrFunction *fun) {
  return coff_base_stack_size(fun) + 8u;
}

// lea reg, [rbp - frame_offset(local)] — used to pass record args by pointer / build sret targets.
static void coff_emit_lea_local_addr_reg(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned reg) {
  z_x64_emit_rbp_disp_reg(text, 0x8d, reg, coff_local_offset(fun, local_index), true);
}

// Copy a record slot from src to dest (or through the saved sret pointer if dest_index == UINT_MAX),
// 8 bytes at a time then a 4-byte tail. The destination address goes into r11 (volatile, no SysV
// rdi available on Win64), the source into r10. rax is scratch.
static void coff_emit_record_copy_to(ZBuf *text, const IrFunction *fun, unsigned dest_index, unsigned src_index) {
  unsigned size = src_index < fun->local_len ? fun->locals[src_index].byte_size : 0;
  if (dest_index == UINT_MAX) {
    z_x64_emit_rbp_disp_reg(text, 0x8b, 11, coff_sret_slot_offset(fun), true); // mov r11, [rbp - sret]
  } else {
    coff_emit_lea_local_addr_reg(text, fun, dest_index, 11);
  }
  coff_emit_lea_local_addr_reg(text, fun, src_index, 10);
  unsigned k = 0;
  while (k + 8 <= size) {
    z_x64_emit_load_reg_ptr_reg_disp(text, 0, 10, k, true);          // mov rax, [r10 + k]
    z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, k, 0, true);    // mov [r11 + k], rax
    k += 8;
  }
  if (k + 4 <= size) {
    z_x64_emit_load_reg_ptr_reg_disp(text, 0, 10, k, false);         // mov eax, [r10 + k]
    z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, k, 0, false);   // mov [r11 + k], eax
  }
}

// Copy a record param (passed by pointer in ptr_reg) into its inline frame slot. coff_local_offset
// is the rbp-relative magnitude of the local's *start*; field k lives at rbp - (offset - k).
static void coff_emit_copy_record_param(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned ptr_reg) {
  unsigned size = local_index < fun->local_len ? fun->locals[local_index].byte_size : 0;
  unsigned frame_off = coff_local_offset(fun, local_index);
  unsigned k = 0;
  while (k + 8 <= size) {
    z_x64_emit_load_reg_ptr_reg_disp(text, 0, ptr_reg, k, true);    // mov rax, [ptr_reg + k]
    z_x64_emit_rbp_disp_reg(text, 0x89, 0, frame_off - k, true);
    k += 8;
  }
  if (k + 4 <= size) {
    z_x64_emit_load_reg_ptr_reg_disp(text, 0, ptr_reg, k, false);   // mov eax, [ptr_reg + k]
    z_x64_emit_rbp_disp_reg(text, 0x89, 0, frame_off - k, false);
  }
}

static void coff_emit_load_local_eax(ZBuf *text, const IrFunction *fun, unsigned local_index) { z_x64_emit_rbp_disp_reg(text, 0x8b, 0, coff_local_offset(fun, local_index), false); }

static void coff_emit_load_local_slot_rax(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned slot_offset) { z_x64_emit_rbp_disp_reg(text, 0x8b, 0, coff_local_slot_offset(fun, local_index, slot_offset), true); }

static void coff_emit_load_local_slot_eax(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned slot_offset) { z_x64_emit_rbp_disp_reg(text, 0x8b, 0, coff_local_slot_offset(fun, local_index, slot_offset), false); }

static void coff_emit_load_local_slot_reg(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned slot_offset, unsigned reg, bool wide) { z_x64_emit_rbp_disp_reg(text, 0x8b, reg, coff_local_slot_offset(fun, local_index, slot_offset), wide); }

static void coff_emit_store_local_from_reg(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned reg) { z_x64_emit_rbp_disp_reg(text, 0x89, reg, coff_local_offset(fun, local_index), false); }

// Float scalar load/store on xmm0. coff_local_offset is the downward magnitude
// (positive); z_x64_emit_movs_xmm_rbp_disp takes a raw signed disp, so negate.
static void coff_emit_load_local_xmm0(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  bool is64 = fun && local_index < fun->local_len && coff_type_is_f64(fun->locals[local_index].type);
  z_x64_emit_movs_xmm_rbp_disp(text, 0, -(int32_t)coff_local_offset(fun, local_index), is64, true);
}
static void coff_emit_store_local_xmm0(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  bool is64 = fun && local_index < fun->local_len && coff_type_is_f64(fun->locals[local_index].type);
  z_x64_emit_movs_xmm_rbp_disp(text, 0, -(int32_t)coff_local_offset(fun, local_index), is64, false);
}
static void coff_emit_store_local_xmm(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned xmm) {
  bool is64 = fun && local_index < fun->local_len && coff_type_is_f64(fun->locals[local_index].type);
  z_x64_emit_movs_xmm_rbp_disp(text, xmm, -(int32_t)coff_local_offset(fun, local_index), is64, false);
}

static void coff_emit_store_local_slot_from_reg(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned reg, unsigned slot_offset, bool wide) { z_x64_emit_rbp_disp_reg(text, 0x89, reg, coff_local_slot_offset(fun, local_index, slot_offset), wide); }

static void coff_emit_load_field_eax(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned field_offset, IrTypeKind type) {
  unsigned offset = coff_local_slot_offset(fun, local_index, field_offset);
  if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
    z_x64_append_u8(text, 0x0f);
    z_x64_emit_rbp_disp_reg(text, 0xb6, 0, offset, false);
  } else {
    z_x64_emit_rbp_disp_reg(text, 0x8b, 0, offset, false);
  }
}

static void coff_emit_store_field_from_eax(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned field_offset, IrTypeKind type) {
  unsigned offset = coff_local_slot_offset(fun, local_index, field_offset);
  if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
    z_x64_emit_rbp_disp_reg(text, 0x88, 0, offset, false);
  } else {
    z_x64_emit_rbp_disp_reg(text, 0x89, 0, offset, false);
  }
}

static void coff_emit_array_base_rdx(ZBuf *text, const IrFunction *fun, unsigned local_index) { z_x64_emit_rbp_disp_reg(text, 0x8d, 2, coff_local_offset(fun, local_index), true); }

static void coff_emit_u8_array_bounds_check(ZBuf *text, const IrLocal *local) {
  z_x64_append_u8(text, 0x3d);
  z_x64_append_u32(text, local ? local->array_len : 0);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x82);
  z_x64_emit_ud2(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
}

static void coff_emit_epilogue(ZBuf *text) { z_x64_emit_epilogue(text); }

// Fallible ABI. Mirrors machx64_emit_error_condition_from_rax: copy rax → rcx, shr
// rcx by 32 to surface the packed tag in the low 32 bits, then `test ecx, ecx` so the next
// JZ/JNZ branches on tag == 0. Leaves rax untouched so the success-path payload survives.
static void coff_emit_error_condition_from_rax(ZBuf *text) {
  z_x64_emit_mov_rcx_from_rax(text, true);
  z_x64_emit_shr_rcx_imm8(text, 32);
  z_x64_emit_test_ecx_ecx(text);
}

// COFF main is `export c fn main` and cannot itself raise (ABI001), so the only callers of a
// raising helper are other raising functions. Mirrors macho's variant minus the propagate-to-
// process-exit case for an int32-returning main (COFF main exits via the start stub).
static bool coff_function_raises(const IrFunction *fun) { return fun && fun->raises; }

static unsigned coff_setcc_opcode(IrCompareOp op) {
  switch (op) {
    case IR_CMP_EQ: return 0x94;
    case IR_CMP_NE: return 0x95;
    case IR_CMP_LT: return 0x9c;
    case IR_CMP_LE: return 0x9e;
    case IR_CMP_GT: return 0x9f;
    case IR_CMP_GE: return 0x9d;
  }
  return 0x94;
}

// UCOMIS sets EFLAGS like an unsigned compare; unordered (NaN) gives ZF=PF=CF=1,
// so float compares use the unsigned SETcc forms below.
static unsigned coff_xmm_setcc_opcode(IrCompareOp op) {
  switch (op) {
    case IR_CMP_EQ: return 0x94; // SETE
    case IR_CMP_NE: return 0x95; // SETNE
    case IR_CMP_LT: return 0x92; // SETB
    case IR_CMP_LE: return 0x96; // SETBE
    case IR_CMP_GT: return 0x97; // SETA  (false on NaN via CF=1)
    case IR_CMP_GE: return 0x93; // SETAE
  }
  return 0x94;
}

// ==,!=,<,<= need a PF fixup so NaN reads false (true only for !=); >,>= do not.
static bool coff_xmm_compare_needs_parity_fixup(IrCompareOp op) {
  return op == IR_CMP_EQ || op == IR_CMP_NE || op == IR_CMP_LT || op == IR_CMP_LE;
}

static void coff_emit_xmm_compare_to_bool(ZBuf *text, IrCompareOp op, bool is64) {
  z_x64_emit_ucomis(text, 0, 1, is64);
  if (coff_xmm_compare_needs_parity_fixup(op)) {
    bool is_ne = op == IR_CMP_NE;
    z_x64_emit_setcc_al(text, (uint8_t)coff_xmm_setcc_opcode(op));
    z_x64_emit_setcc_cl(text, is_ne ? 0x9a : 0x9b); // SETP for !=, SETNP otherwise
    if (is_ne) z_x64_emit_or_al_cl(text);
    else z_x64_emit_and_al_cl(text);
    z_x64_emit_movzx_eax_al(text);
  } else {
    z_x64_emit_setcc_al_to_bool(text, coff_xmm_setcc_opcode(op));
  }
}

static bool coff_const_u32_value(const IrValue *value, unsigned *out) {
  if (!value || value->kind != IR_VALUE_INT || value->int_value > UINT32_MAX) return false;
  if (out) *out = (unsigned)value->int_value;
  return true;
}

static bool coff_readonly_data_byte(const IrProgram *program, unsigned offset, unsigned char *out) {
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

static bool coff_byte_view_const_len(const IrValue *view, unsigned *out) {
  if (!view) return false;
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (out) *out = view->data_len;
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned base_len = 0;
    if (!coff_byte_view_const_len(view->left, &base_len)) return false;
    unsigned start = 0;
    unsigned end = base_len;
    if (view->index && !coff_const_u32_value(view->index, &start)) return false;
    if (view->right && !coff_const_u32_value(view->right, &end)) return false;
    if (start > end || end > base_len) return false;
    if (out) *out = end - start;
    return true;
  }
  // Reinterpreted element count = underlying byte length / sizeof(target element).
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    unsigned base_len = 0;
    if (!coff_byte_view_const_len(view->left, &base_len)) return false;
    if (out) *out = base_len / coff_elem_byte_size(view->element_type);
    return true;
  }
  return false;
}

static bool coff_byte_view_const_byte(const IrProgram *program, const IrValue *view, unsigned index, unsigned char *out) {
  if (!view) return false;
  if (view->kind == IR_VALUE_STRING_LITERAL) {
    if (index >= view->data_len) return false;
    return coff_readonly_data_byte(program, view->data_offset + index, out);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned len = 0;
    unsigned start = 0;
    if (!coff_byte_view_const_len(view, &len) || index >= len) return false;
    if (view->index && !coff_const_u32_value(view->index, &start)) return false;
    return coff_byte_view_const_byte(program, view->left, start + index, out);
  }
  return false;
}

static bool coff_emit_rodata_ptr_rax(ZBuf *text, unsigned data_offset, CoffEmitContext *ctx, const IrValue *value, ZDiag *diag) {
  uint64_t addend = data_offset - (ctx ? ctx->rodata_base_offset : 0);
  size_t patch = z_x64_emit_mov_rax_u64_patchable(text, addend);
  return z_coff_record_rodata_patch(ctx, patch, data_offset, value, diag);
}

static bool coff_emit_byte_view_ptr(ZBuf *text, const IrFunction *fun, const IrValue *view, CoffEmitContext *ctx, ZDiag *diag);
static bool coff_emit_byte_view_len(ZBuf *text, const IrFunction *fun, const IrValue *view, CoffEmitContext *ctx, ZDiag *diag);
static bool coff_emit_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag);
// Forward decl — std.fs.munmap(&mut m) emits inline as an IR_VALUE_FS_MUNMAP value.
static bool coff_emit_fs_munmap_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag);

static bool coff_emit_byte_view_len(ZBuf *text, const IrFunction *fun, const IrValue *view, CoffEmitContext *ctx, ZDiag *diag) {
  unsigned len = 0;
  if (coff_byte_view_const_len(view, &len)) {
    z_x64_emit_mov_eax_u32(text, len);
    return true;
  }
  // Typed-span length = underlying byte length >> log2(sizeof(T)). Mirrors macho_x64
  // (z_x64_emit_shr_reg_imm8 on rax). A 1-byte element (u8/i8/Bool) needs no shift.
  if (view && view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    if (!coff_emit_byte_view_len(text, fun, view->left, ctx, diag)) return false;
    unsigned shift = coff_elem_byte_size_log2(view->element_type);
    if (shift > 0) z_x64_emit_shr_reg_imm8(text, 0, shift, true);
    return true;
  }
  if (view && view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    coff_emit_load_local_slot_eax(text, fun, view->local_index, 8);
    return true;
  }
  // Span field length lives 8 bytes past the field's ptr (64-bit slot, but COFF len
  // accesses stay 32-bit per the rest of this emitter).
  if (view && view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    coff_emit_load_local_slot_eax(text, fun, view->local_index, view->field_offset + 8);
    return true;
  }
  // A span-returning call leaves ptr in rax and len in rdx (Win64 second-return). The
  // CALL emit path already lands both registers, so just re-evaluate and copy edx → eax.
  if (view && view->kind == IR_VALUE_CALL && view->type == IR_TYPE_BYTE_VIEW) {
    if (!coff_emit_value(text, fun, view, ctx, diag)) return false;
    z_x64_emit_mov_reg_from_reg(text, 0, 2, false); // mov eax, edx (len)
    return true;
  }
  if (view && view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    coff_emit_load_local_slot_eax(text, fun, view->local_index, 16);
    return true;
  }
  if (view && view->kind == IR_VALUE_BYTE_SLICE && view->index && view->right) {
    unsigned start = 0;
    unsigned end = 0;
    if (coff_const_u32_value(view->index, &start) && coff_const_u32_value(view->right, &end) && start <= end) {
      z_x64_emit_mov_eax_u32(text, end - start);
      return true;
    }
  }
  (void)ctx;
  return coff_diag_at(diag, "direct COFF byte-view length currently requires a literal, constant slice, or byte-view local", view ? view->line : 1, view ? view->column : 1, "unsupported byte view length");
}

static bool coff_emit_byte_view_ptr(ZBuf *text, const IrFunction *fun, const IrValue *view, CoffEmitContext *ctx, ZDiag *diag) {
  if (!view) return coff_diag_at(diag, "direct COFF byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    coff_emit_load_local_slot_rax(text, fun, view->local_index, 0);
    return true;
  }
  // Span field ptr lives at field_offset within the record local.
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    coff_emit_load_local_slot_rax(text, fun, view->local_index, view->field_offset);
    return true;
  }
  // A span-returning call lands ptr in rax and len in rdx; just emit the call.
  if (view->kind == IR_VALUE_CALL && view->type == IR_TYPE_BYTE_VIEW) {
    return coff_emit_value(text, fun, view, ctx, diag);
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    coff_emit_load_local_slot_rax(text, fun, view->local_index, 8);
    return true;
  }
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    // Any value-typed array binds as a typed span — ptr is LEA of &arr[0]. Element scaling
    // for INDEX_LOAD/STORE and BYTE_SLICE rides on the carrying IR_VALUE's element_type.
    if (!local->is_array) return coff_diag_at(diag, "direct COFF byte-view array source must be a fixed array local", view->line, view->column, "unsupported array view");
    z_x64_emit_rbp_disp_reg(text, 0x8d, 0, coff_local_offset(fun, view->array_index), true);
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) {
    return coff_emit_rodata_ptr_rax(text, view->data_offset, ctx, view, diag);
  }
  // A reinterpret keeps the same base pointer (zero-copy); only the element count changes.
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    return coff_emit_byte_view_ptr(text, fun, view->left, ctx, diag);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    if (!coff_const_u32_value(view->index, &start)) return coff_diag_at(diag, "direct COFF byte slice currently requires a constant start", view->line, view->column, "unsupported byte slice");
    if (!coff_emit_byte_view_ptr(text, fun, view->left, ctx, diag)) return false;
    // Slice bounds are element indices; the byte offset is start * sizeof(T). For u8 this is start.
    unsigned byte_start = start * coff_elem_byte_size(view->element_type);
    if (byte_start > 0) z_x64_emit_add_rax_u32(text, byte_start, true);
    return true;
  }
  return coff_diag_at(diag, "direct COFF value is not a supported byte view", view->line, view->column, "unsupported byte view");
}

// Forward decl: call a record-returning function and direct its sret pointer to a local
// (dest_local >= 0) or to the caller's own sret pointer slot (dest_local < 0).
static bool coff_emit_record_call_with_dest(ZBuf *text, const IrFunction *fun, int dest_local, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag);

static bool coff_emit_local_value(ZBuf *text, const IrFunction *fun, const IrValue *value, ZDiag *diag) {
  if (value->local_index >= fun->local_len) return coff_diag_at(diag, "direct COFF local index is out of range", value->line, value->column, "invalid local");
  IrTypeKind ltype = fun->locals[value->local_index].type;
  if (ltype == IR_TYPE_BYTE_VIEW) return coff_diag_at(diag, "direct COFF byte-view local cannot be used as a scalar", value->line, value->column, "byte-view local");
  if (coff_type_is_float(ltype)) coff_emit_load_local_xmm0(text, fun, value->local_index);
  // i64/u64 locals load via 64-bit `mov rax, [rbp+disp]` to preserve all 64 bits.
  else if (coff_type_is_scalar64(ltype)) coff_emit_load_local_slot_rax(text, fun, value->local_index, 0);
  else coff_emit_load_local_eax(text, fun, value->local_index);
  return true;
}

static bool coff_emit_binary_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (coff_type_is_float(value->type)) {
    bool is64 = coff_type_is_f64(value->type);
    if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
    z_x64_emit_xmm_push(text, 0);
    if (!coff_emit_value(text, fun, value->right, ctx, diag)) return false;
    z_x64_emit_movaps(text, 1, 0);
    z_x64_emit_xmm_pop(text, 0);
    if (value->binary_op == IR_BIN_ADD) z_x64_emit_sse_add(text, 0, 1, is64);
    else if (value->binary_op == IR_BIN_SUB) z_x64_emit_sse_sub(text, 0, 1, is64);
    else if (value->binary_op == IR_BIN_MUL) z_x64_emit_sse_mul(text, 0, 1, is64);
    else if (value->binary_op == IR_BIN_DIV) z_x64_emit_sse_div(text, 0, 1, is64);
    else return coff_diag_at(diag, "direct COFF float binary operator is unsupported", value->line, value->column, "unsupported operator");
    return true;
  }
  if (value->binary_op != IR_BIN_ADD && value->binary_op != IR_BIN_SUB && value->binary_op != IR_BIN_MUL) return coff_diag_at(diag, "direct COFF binary operator is unsupported", value->line, value->column, "unsupported operator");
  if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!coff_emit_value(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_mov_rcx_from_rax(text, false);
  z_x64_emit_pop_rax(text);
  if (value->binary_op == IR_BIN_ADD) z_x64_emit_add_rax_rcx(text, false);
  else if (value->binary_op == IR_BIN_SUB) z_x64_emit_sub_rax_rcx(text, false);
  else z_x64_emit_imul_rax_rcx(text, false);
  return true;
}

static bool coff_emit_compare_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return coff_diag_at(diag, "direct COFF comparison requires two operands", value->line, value->column, "invalid comparison");
  if (coff_type_is_float(value->left->type)) {
    bool is64 = coff_type_is_f64(value->left->type);
    if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
    z_x64_emit_xmm_push(text, 0);
    if (!coff_emit_value(text, fun, value->right, ctx, diag)) return false;
    z_x64_emit_movaps(text, 1, 0);
    z_x64_emit_xmm_pop(text, 0);
    coff_emit_xmm_compare_to_bool(text, value->compare_op, is64);
    return true;
  }
  if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!coff_emit_value(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_mov_rcx_from_rax(text, false);
  z_x64_emit_pop_rax(text);
  z_x64_emit_cmp_rax_rcx_to_bool(text, coff_setcc_opcode(value->compare_op), false);
  return true;
}

static bool coff_emit_call_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  // A record-returning call routes through the sret machinery — caller-provided
  // dest pointer in rcx, the rest of the args shifted by one register. Used only when
  // a record-returning call appears as an inline expression (e.g. an argument to another call).
  // Here we don't have a destination, so reject — record-returning calls only bind into a local
  // or into a return slot via the specialized paths below.
  if (value->type == IR_TYPE_RECORD) {
    return coff_diag_at(diag, "direct COFF record-returning call requires a record-typed binding or return", value->line, value->column, "unbound record call");
  }
  static const unsigned param_regs[] = {1, 2, 8, 9};
  // A record arg consumes one int slot (passed by pointer). Span args still consume 2.
  size_t int_slots = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    if (arg && arg->type == IR_TYPE_BYTE_VIEW) int_slots += 2u;
    else int_slots += 1u;
  }
  if (int_slots > 4) return coff_diag_at(diag, "direct COFF call supports at most four integer arguments", value->line, value->column, "too many arguments");
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    if (arg && arg->type == IR_TYPE_BYTE_VIEW) {
      if (!coff_emit_byte_view_ptr(text, fun, arg, ctx, diag)) return false;
      z_x64_emit_push_rax(text);
      if (!coff_emit_byte_view_len(text, fun, arg, ctx, diag)) return false;
      z_x64_emit_push_rax(text);
    } else if (arg && arg->type == IR_TYPE_RECORD) {
      // Record arg: pass by pointer (caller's spilled local stays valid for the call duration).
      if (arg->kind != IR_VALUE_LOCAL) return coff_diag_at(diag, "direct COFF record argument must be a local", value->line, value->column, "non-local record arg");
      coff_emit_lea_local_addr_reg(text, fun, arg->local_index, 0);
      z_x64_emit_push_rax(text);
    } else if (arg && coff_type_is_float(arg->type)) {
      // Win64 float arg: the value lands in xmm0; spill it. On Win64 a float consumes the same
      // unified slot as an int (xmm0↔rcx, xmm1↔rdx, …), so it pops into xmm at the slot index.
      if (!coff_emit_value(text, fun, arg, ctx, diag)) return false;
      z_x64_emit_xmm_push(text, 0);
    } else {
      if (!coff_emit_value(text, fun, value->args[i], ctx, diag)) return false;
      z_x64_emit_push_rax(text);
    }
  }
  size_t int_remaining = int_slots;
  for (size_t i = value->arg_len; i > 0; i--) {
    const IrValue *arg = value->args[i - 1];
    if (arg && arg->type == IR_TYPE_BYTE_VIEW) {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    } else if (arg && coff_type_is_float(arg->type)) {
      // Unified slot index = the reserved GPR position; route into the matching xmm register
      // (xmm0..3) so the callee prologue, which reads float params from xmm[slot], agrees.
      z_x64_emit_xmm_pop(text, (unsigned)(--int_remaining));
    } else {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    }
  }
  z_x64_emit_sub_rsp(text, 32);
  size_t patch = z_x64_emit_call32_placeholder(text);
  z_x64_emit_add_rsp(text, 32);
  return z_coff_record_call_patch(ctx, patch, value->callee_index, value, diag);
}

// Call a record-returning function under the Win64 sret ABI. The caller-allocated buffer
// pointer must arrive in rcx (param0), so int_count starts at 1 and rcx is set LAST — after the
// rest of the arg-marshaling pushes/pops are done — pointing at dest_local (own local) or the
// caller's saved sret slot (dest_local < 0). The callee returns the sret pointer back in rax.
static bool coff_emit_record_call_with_dest(ZBuf *text, const IrFunction *fun, int dest_local, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  static const unsigned param_regs[] = {1, 2, 8, 9};
  const IrFunction *callee = ctx && ctx->program && value->callee_index < ctx->program->function_len ? &ctx->program->functions[value->callee_index] : NULL;
  if (!callee) return coff_diag_at(diag, "direct COFF call target is unavailable", value->line, value->column, "invalid callee");
  size_t int_count = 1; // rcx = sret dest
  for (size_t i = 0; i < value->arg_len; i++) {
    IrTypeKind ptype = i < callee->param_count ? callee->locals[i].type : value->args[i]->type;
    if (ptype == IR_TYPE_BYTE_VIEW) int_count += 2u;
    else int_count += 1u;
  }
  if (int_count > 4) return coff_diag_at(diag, "direct COFF record-returning call supports at most three integer arguments (sret in rcx consumes the fourth)", value->line, value->column, "too many integer arguments");
  for (size_t i = 0; i < value->arg_len; i++) {
    IrTypeKind ptype = i < callee->param_count ? callee->locals[i].type : value->args[i]->type;
    if (ptype == IR_TYPE_BYTE_VIEW) {
      if (!coff_emit_byte_view_ptr(text, fun, value->args[i], ctx, diag)) return false;
      z_x64_emit_push_rax(text);
      if (!coff_emit_byte_view_len(text, fun, value->args[i], ctx, diag)) return false;
      z_x64_emit_push_rax(text);
    } else if (ptype == IR_TYPE_RECORD) {
      if (value->args[i]->kind != IR_VALUE_LOCAL) return coff_diag_at(diag, "direct COFF record argument must be a local", value->line, value->column, "non-local record arg");
      coff_emit_lea_local_addr_reg(text, fun, value->args[i]->local_index, 0);
      z_x64_emit_push_rax(text);
    } else if (coff_type_is_float(ptype)) {
      // Win64 float arg: spill xmm0; the unified slot is shared with the GPR bank (sret in rcx
      // already consumed slot 0, so int_count started at 1).
      if (!coff_emit_value(text, fun, value->args[i], ctx, diag)) return false;
      z_x64_emit_xmm_push(text, 0);
    } else {
      if (!coff_emit_value(text, fun, value->args[i], ctx, diag)) return false;
      z_x64_emit_push_rax(text);
    }
  }
  size_t int_remaining = int_count;
  for (size_t i = value->arg_len; i > 0; i--) {
    IrTypeKind ptype = (i - 1) < callee->param_count ? callee->locals[i - 1].type : value->args[i - 1]->type;
    if (ptype == IR_TYPE_BYTE_VIEW) {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    } else if (coff_type_is_float(ptype)) {
      z_x64_emit_xmm_pop(text, (unsigned)(--int_remaining));
    } else {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    }
  }
  // rcx (param0) = destination sret pointer — set LAST so the pop sequence above doesn't clobber it.
  if (dest_local >= 0) {
    coff_emit_lea_local_addr_reg(text, fun, (unsigned)dest_local, 1);
  } else {
    z_x64_emit_rbp_disp_reg(text, 0x8b, 1, coff_sret_slot_offset(fun), true); // mov rcx, [rbp - sret]
  }
  z_x64_emit_sub_rsp(text, 32);
  size_t patch = z_x64_emit_call32_placeholder(text);
  z_x64_emit_add_rsp(text, 32);
  return z_coff_record_call_patch(ctx, patch, value->callee_index, value, diag);
}

static bool coff_emit_vec_push_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_VEC) return coff_diag_at(diag, "direct COFF Vec push requires a Vec local", value->line, value->column, "invalid Vec local");
  coff_emit_load_local_slot_eax(text, fun, value->local_index, 8);
  coff_emit_load_local_slot_reg(text, fun, value->local_index, 12, 1, false);
  z_x64_emit_cmp_rax_rcx(text, false);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x82);
  z_x64_emit_mov_eax_u32(text, 0);
  size_t end_patch = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, ok_patch, text->len);
  z_x64_emit_push_rax(text);
  if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_pop_reg64(text, 1);
  coff_emit_load_local_slot_reg(text, fun, value->local_index, 0, 2, true);
  z_x64_emit_add_rdx_rcx(text, true);
  z_x64_emit_store_ptr_reg8_from_reg(text, 2, 0);
  z_x64_emit_mov_eax_from_ecx(text);
  z_x64_emit_add_reg_i8(text, 0, 1, false);
  coff_emit_store_local_slot_from_reg(text, fun, value->local_index, 0, 8, false);
  z_x64_emit_mov_eax_u32(text, 1);
  z_x64_patch_rel32(text, end_patch, text->len);
  return true;
}

static bool coff_emit_byte_view_index_load_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  unsigned const_index = 0;
  unsigned char byte = 0;
  if (coff_const_u32_value(value->index, &const_index) && coff_byte_view_const_byte(ctx ? ctx->program : NULL, value->left, const_index, &byte)) {
    z_x64_emit_mov_eax_u32(text, byte);
    return true;
  }
  if (!value->index || !coff_emit_value(text, fun, value->index, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!coff_emit_byte_view_len(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_mov_rcx_from_rax(text, false);
  z_x64_emit_pop_rax(text);
  z_x64_emit_cmp_rax_rcx(text, false);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x82);
  z_x64_emit_ud2(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
  z_x64_emit_push_rax(text);
  if (!coff_emit_byte_view_ptr(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_pop_reg64(text, 1);
  z_x64_emit_add_rax_rcx(text, true);
  z_x64_emit_movzx_reg32_ptr_reg_u8(text, 0, 0);
  return true;
}

// std.codec.read{I32,U32,U16,I64,U64,F32,F64}Le — read a 2/4/8-byte little-endian scalar
// at a byte offset into a Span<u8>. x86-64 is little-endian and tolerates unaligned loads, so the
// raw load at ptr+offset yields the value; signed and unsigned same-width forms share the load.
// Bounds-check that offset + (size - 1) < len with a 64-bit unsigned compare, which is equivalent
// to offset + size <= len and rejects offset overflow. Mirrors machx64_emit_byte_le_read_value.
static bool coff_emit_byte_le_read_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  bool is_float = value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE;
  bool is_f64 = is_float && coff_type_is_f64(value->type);
  if (!value->left) return coff_diag_at(diag, is_float ? "direct COFF readF*Le requires a byte view" : "direct COFF read*Le requires a byte view", value->line, value->column, "missing byte view");
  if (!value->index) return coff_diag_at(diag, is_float ? "direct COFF readF*Le requires an offset" : "direct COFF read*Le requires an offset", value->line, value->column, "missing offset");
  unsigned scalar_size = is_float ? (is_f64 ? 8u : 4u) : coff_elem_byte_size(value->type);
  if (!coff_emit_value(text, fun, value->index, ctx, diag)) return false; // rax = offset
  z_x64_emit_push_rax(text); // preserve true offset across the len evaluation
  if (!coff_emit_byte_view_len(text, fun, value->left, ctx, diag)) return false; // rax = len
  z_x64_emit_mov_rcx_from_rax(text, true); // rcx = len
  z_x64_emit_pop_rax(text); // rax = offset
  z_x64_emit_push_rax(text); // re-save the true offset for the address computation
  z_x64_emit_add_rax_u32(text, scalar_size - 1u, true); // rax = offset + (size - 1)
  z_x64_emit_cmp_rax_rcx(text, true);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x82); // JB: offset+(size-1) < len
  z_x64_emit_ud2(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
  if (!coff_emit_byte_view_ptr(text, fun, value->left, ctx, diag)) return false; // rax = ptr
  z_x64_emit_pop_reg64(text, 1); // rcx = offset
  z_x64_emit_add_rax_rcx(text, true); // rax = ptr + offset
  if (is_float) z_x64_emit_movs_xmm_ptr_reg(text, 0, 0, is_f64, true); // movss/movsd xmm0, [rax]
  else if (scalar_size == 8) z_x64_emit_load_reg_ptr_reg(text, 0, 0, true); // mov rax, [rax]
  else if (scalar_size == 2) z_x64_emit_movzx_reg32_ptr_reg_disp_u16(text, 0, 0, 0); // movzx eax, word [rax]
  else z_x64_emit_load_reg_ptr_reg(text, 0, 0, false); // mov eax, [rax]
  return true;
}

static bool coff_emit_byte_copy_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return coff_diag_at(diag, "direct COFF byte copy requires source and destination byte views", value->line, value->column, "missing byte view");
  z_x64_emit_push_reg64(text, 7); z_x64_emit_push_reg64(text, 6);
  if (!coff_emit_byte_view_ptr(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!coff_emit_byte_view_len(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!coff_emit_byte_view_ptr(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!coff_emit_byte_view_len(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_pop_reg64(text, 7);
  z_x64_emit_pop_reg64(text, 1);
  z_x64_emit_pop_reg64(text, 6);
  z_x64_emit_byte_copy_min_loop(text);
  z_x64_emit_pop_reg64(text, 6); z_x64_emit_pop_reg64(text, 7);
  return true;
}

static bool coff_emit_byte_fill_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return coff_diag_at(diag, "direct COFF byte fill requires a fill byte and destination byte view", value->line, value->column, "missing byte fill input");
  if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text); z_x64_emit_push_reg64(text, 7);
  if (!coff_emit_byte_view_ptr(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!coff_emit_byte_view_len(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_mov_rdx_from_rax(text);
  z_x64_emit_pop_reg64(text, 7); z_x64_emit_pop_reg64(text, 11);
  z_x64_emit_pop_reg64(text, 9); z_x64_emit_push_reg64(text, 11);
  z_x64_emit_byte_fill_loop(text); z_x64_emit_pop_reg64(text, 7);
  return true;
}

static bool coff_emit_byte_view_eq_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return coff_diag_at(diag, "direct COFF byte-view equality requires two byte views", value->line, value->column, "missing byte view");
  if (!coff_emit_byte_view_len(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!coff_emit_byte_view_len(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_pop_reg64(text, 1);
  z_x64_emit_cmp_reg_reg(text, 1, 0, false);
  size_t same_len = z_x64_emit_jcc32_placeholder(text, 0x84);
  z_x64_emit_mov_eax_u32(text, 0);
  size_t end = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, same_len, text->len);
  z_x64_emit_mov_reg_from_rax(text, 10, true);
  if (!coff_emit_byte_view_ptr(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_mov_reg_from_rax(text, 8, true);
  if (!coff_emit_byte_view_ptr(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_mov_r9_from_rax(text);
  z_x64_emit_byte_eq_loop(text);
  z_x64_patch_rel32(text, end, text->len);
  return true;
}

// Compute the element address (span.ptr + index * sizeof(T)) of a typed-span local into
// rax, with a bounds check (index < span.len traps via ud2). The index is evaluated first and saved
// on the stack so the len/ptr loads — which clobber rax — don't lose it. The len slot on COFF is
// 32-bit (the 64-bit widening is a Tier A feature; COFF is build-only and stays narrow), so the
// cmp/JA pair runs at 32-bit width. ptr is loaded as 64-bit; the scaled LEA folds index*sizeof(T)
// into the address.
static bool coff_emit_span_index_addr(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, CoffEmitContext *ctx, ZDiag *diag) {
  const IrLocal *local = &fun->locals[local_index];
  if (!index || !coff_emit_value(text, fun, index, ctx, diag)) return false; // eax = index
  z_x64_emit_push_rax(text);                                                  // [rsp] = index
  coff_emit_load_local_slot_eax(text, fun, local_index, 8);                   // eax = len
  z_x64_emit_pop_reg64(text, 1);                                              // rcx = index (low 32 in ecx)
  z_x64_emit_cmp_rax_rcx(text, false);                                        // cmp eax(len), ecx(index)
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x87);                 // JA: index < len when len > index
  z_x64_emit_ud2(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
  coff_emit_load_local_slot_rax(text, fun, local_index, 0);                   // rax = ptr (64-bit)
  z_x64_emit_lea_base_index_scale_disp_reg(text, 0, 0, 1, coff_elem_byte_size(local->element_type), 0);
  return true;
}

static bool coff_emit_index_load_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (value->array_index >= fun->local_len) return coff_diag_at(diag, "direct COFF indexed load array is out of range", value->line, value->column, "invalid array local");
  const IrLocal *local = &fun->locals[value->array_index];
  // Typed-span element read. After span_index_addr puts the element address in rax, dispatch
  // on element width: u8/Bool zero-extend, i8 sign-extend, 8-byte via 64-bit mov, float via movss/movsd
  // into xmm0, else 32-bit mov. Mirrors machx64_emit_index_load_value's BYTE_VIEW branch.
  if (local->type == IR_TYPE_BYTE_VIEW) {
    if (!coff_emit_span_index_addr(text, fun, value->array_index, value->index, ctx, diag)) return false;
    IrTypeKind elem = local->element_type;
    if (elem == IR_TYPE_U8 || elem == IR_TYPE_BOOL) z_x64_emit_movzx_reg32_ptr_reg_u8(text, 0, 0);
    else if (elem == IR_TYPE_I8) z_x64_emit_movsx_reg32_ptr_reg_i8(text, 0, 0);
    else if (coff_type_is_float(elem)) z_x64_emit_movs_xmm_ptr_reg(text, 0, 0, coff_type_is_f64(elem), true);
    else if (coff_type_is_i64(elem)) z_x64_emit_load_reg_ptr_reg(text, 0, 0, true);
    else z_x64_emit_load_reg_ptr_reg(text, 0, 0, false);
    return true;
  }
  unsigned const_index = 0;
  if (local->is_array && local->element_type != IR_TYPE_U8 && coff_const_u32_value(value->index, &const_index) && const_index < local->array_len) {
    coff_emit_load_local_slot_eax(text, fun, value->array_index, const_index * 4u);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
    if (!value->index || !coff_emit_value(text, fun, value->index, ctx, diag)) return false;
    coff_emit_u8_array_bounds_check(text, local);
    z_x64_emit_push_rax(text);
    coff_emit_array_base_rdx(text, fun, value->array_index);
    z_x64_emit_pop_reg64(text, 1);
    z_x64_emit_shl_rcx_imm8(text, 2);
    z_x64_emit_add_rdx_rcx(text, true);
    z_x64_emit_load_reg_ptr_reg(text, 0, 2, false);
    return true;
  }
  if (!local->is_array || local->element_type != IR_TYPE_U8) return coff_diag_at(diag, "direct COFF indexed load requires [N]u8 or integer arrays", value->line, value->column, "unsupported array local");
  if (!value->index || !coff_emit_value(text, fun, value->index, ctx, diag)) return false;
  coff_emit_u8_array_bounds_check(text, local);
  z_x64_emit_push_rax(text);
  coff_emit_array_base_rdx(text, fun, value->array_index);
  z_x64_emit_pop_reg64(text, 1);
  z_x64_emit_add_rdx_rcx(text, true);
  z_x64_emit_movzx_reg32_ptr_reg_u8(text, 0, 2);
  return true;
}

static bool coff_emit_field_load_value(ZBuf *text, const IrFunction *fun, const IrValue *value, ZDiag *diag) {
  if (value->local_index >= fun->local_len) return coff_diag_at(diag, "direct COFF field load record is out of range", value->line, value->column, "invalid record local");
  if (!fun->locals[value->local_index].is_record) return coff_diag_at(diag, "direct COFF field load requires record local", value->line, value->column, "non-record local");
  coff_emit_load_field_eax(text, fun, value->local_index, value->field_offset, value->type); return true;
}

// libm on COFF: msvcrt.dll resolves the symbol through the .idata import directory on the exe path
// (REL32 against an undefined external on the obj path). Win64 ABI requires a 32-byte shadow space
// around the call. isNaNf stays inline (UCOMIS+SETP) since NaN is the only x where x!=x.
static CoffMathSymbol coff_math_symbol_for_value(IrValueKind kind) {
  switch (kind) {
    case IR_VALUE_MATH_EXPF: return COFF_MATH_EXPF;
    case IR_VALUE_MATH_COSF: return COFF_MATH_COSF;
    case IR_VALUE_MATH_SINF: return COFF_MATH_SINF;
    case IR_VALUE_MATH_POWF: return COFF_MATH_POWF;
    case IR_VALUE_MATH_FABSF: return COFF_MATH_FABSF;
    case IR_VALUE_MATH_FLOORF: return COFF_MATH_FLOORF;
    default: return COFF_MATH_SQRTF;
  }
}

static bool coff_emit_math_call(ZBuf *text, CoffEmitContext *ctx, const IrValue *value, ZDiag *diag) {
  z_x64_emit_sub_rsp(text, 32);
  size_t patch = z_x64_emit_call_rip32_placeholder(text);
  z_x64_emit_add_rsp(text, 32);
  return z_coff_record_math_patch(ctx, patch, coff_math_symbol_for_value(value->kind), value, diag);
}

static bool coff_emit_math_unary_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value->left) return coff_diag_at(diag, "direct COFF math call requires an argument", value->line, value->column, "missing argument");
  if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
  return coff_emit_math_call(text, ctx, value, diag);
}

static bool coff_emit_math_powf_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return coff_diag_at(diag, "direct COFF powf requires two arguments", value->line, value->column, "missing argument");
  if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_xmm_push(text, 0);
  if (!coff_emit_value(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_movaps(text, 1, 0);
  z_x64_emit_xmm_pop(text, 0);
  return coff_emit_math_call(text, ctx, value, diag);
}

// `check <fallible>` — evaluate the fallible value (which lands payload in rax/xmm0
// and the error tag packed in rax[63:32]), inspect the tag, propagate on error (only valid in
// a raising context — COFF main can't raise so the caller is always a raising helper),
// otherwise drop the tag from rax's low 32 bits. Mirrors machx64_emit_check_value.
static bool coff_emit_check_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value->left || (value->left->type != IR_TYPE_I64 && !coff_type_is_float(value->left->type))) {
    return coff_diag_at(diag, "direct COFF check requires a fallible call result", value->line, value->column, "non-fallible value");
  }
  if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
  coff_emit_error_condition_from_rax(text);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  if (!coff_function_raises(fun)) return coff_diag_at(diag, "direct COFF check requires a fallible function context", value->line, value->column, "non-fallible context");
  // Tag is already packed in rax[63:32]; just jump to the epilogue (return_type stays packed).
  z_x64_emit_epilogue(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
  // OK path: an int payload shares rax's low 32 bits with the (now-zero) tag, so a 32-bit
  // self-move drops the tag bits. A float payload sits in xmm0 and needs no fixup.
  if (!coff_type_is_i64(value->type) && !coff_type_is_float(value->type)) z_x64_emit_mov_reg_from_reg(text, 0, 0, false);
  return true;
}

// `<fallible> rescue <fallback>` — evaluate the fallible, on tag != 0 evaluate the
// fallback expression (whose type matches `value->type`, i.e. int → rax, float → xmm0),
// otherwise drop the tag from rax. Mirrors machx64_emit_rescue_value.
static bool coff_emit_rescue_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right || (value->left->type != IR_TYPE_I64 && !coff_type_is_float(value->left->type))) {
    return coff_diag_at(diag, "direct COFF rescue requires a fallible call and fallback", value->line, value->column, "unsupported rescue");
  }
  if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
  coff_emit_error_condition_from_rax(text);
  size_t success_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  if (!coff_emit_value(text, fun, value->right, ctx, diag)) return false;
  size_t end_patch = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, success_patch, text->len);
  if (!coff_type_is_i64(value->type) && !coff_type_is_float(value->type)) z_x64_emit_mov_reg_from_reg(text, 0, 0, false);
  z_x64_patch_rel32(text, end_patch, text->len);
  return true;
}

static bool coff_emit_math_isnanf_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value->left) return coff_diag_at(diag, "direct COFF isNaNf requires an argument", value->line, value->column, "missing argument");
  if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_ucomis(text, 0, 0, coff_type_is_f64(value->left->type));
  z_x64_emit_setcc_al_to_bool(text, 0x9a);
  (void)ctx; return true;
}

static bool coff_emit_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (!value) return coff_diag_at(diag, "direct COFF expression is missing", 1, 1, "missing expression");
  switch (value->kind) {
    case IR_VALUE_FLOAT: {
      // Materialize the IEEE bit pattern in a GPR, then MOVD/MOVQ it into xmm0.
      bool is64 = coff_type_is_f64(value->type);
      if (is64) z_x64_emit_mov_rax_u64(text, (uint64_t)value->int_value);
      else z_x64_emit_mov_eax_u32(text, (uint32_t)value->int_value);
      z_x64_emit_movd_xmm_from_gpr(text, 0, 0, is64);
      return true;
    }
    case IR_VALUE_BOOL:
    case IR_VALUE_INT:
      z_x64_emit_mov_eax_u32(text, (uint32_t)value->int_value);
      return true;
    case IR_VALUE_LOCAL: return coff_emit_local_value(text, fun, value, diag);
    case IR_VALUE_CAST: {
      IrTypeKind src = value->left ? value->left->type : IR_TYPE_I32;
      IrTypeKind dst = value->type;
      if (!coff_emit_value(text, fun, value->left, ctx, diag)) return false;
      bool src_float = coff_type_is_float(src);
      bool dst_float = coff_type_is_float(dst);
      if (src_float && dst_float) {
        if (src != dst) z_x64_emit_cvts2s(text, 0, 0, coff_type_is_f64(src));
      } else if (!src_float && dst_float) {
        z_x64_emit_cvtsi2s(text, 0, 0, coff_type_is_f64(dst), false);
      } else if (src_float && !dst_float) {
        z_x64_emit_cvtts2si(text, 0, 0, coff_type_is_f64(src), false);
      } else {
        coff_emit_cast_normalize_rax(text, dst);
      }
      return true;
    }
    case IR_VALUE_BINARY: return coff_emit_binary_value(text, fun, value, ctx, diag);
    case IR_VALUE_COMPARE: return coff_emit_compare_value(text, fun, value, ctx, diag);
    case IR_VALUE_CALL: return coff_emit_call_value(text, fun, value, ctx, diag);
    case IR_VALUE_VEC_LEN:
    case IR_VALUE_VEC_CAPACITY:
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_VEC) return coff_diag_at(diag, "direct COFF Vec helper requires a Vec local", value->line, value->column, "invalid Vec local");
      coff_emit_load_local_slot_eax(text, fun, value->local_index, value->kind == IR_VALUE_VEC_LEN ? 8 : 12);
      return true;
    case IR_VALUE_VEC_PUSH: return coff_emit_vec_push_value(text, fun, value, ctx, diag);
    case IR_VALUE_MAYBE_HAS:
      // Accept either a Maybe<MutSpan<u8>> or a Maybe<scalar> local; both store has@0 u32.
      if (value->local_index >= fun->local_len ||
          (fun->locals[value->local_index].type != IR_TYPE_MAYBE_BYTE_VIEW && fun->locals[value->local_index].type != IR_TYPE_MAYBE_SCALAR)) {
        return coff_diag_at(diag, "direct COFF maybe helper requires a Maybe local", value->line, value->column, "invalid maybe local");
      }
      coff_emit_load_local_slot_eax(text, fun, value->local_index, 0);
      return true;
    case IR_VALUE_MAYBE_VALUE:
      // Maybe<scalar> payload at @8 (loaded full-width). Maybe<MutSpan<u8>> is read through
      // coff_emit_byte_view_ptr/_len when MAYBE_VALUE appears as a span source.
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_MAYBE_SCALAR) {
        return coff_diag_at(diag, "direct COFF maybe scalar value requires a Maybe scalar local", value->line, value->column, "invalid maybe value");
      }
      coff_emit_load_local_slot_rax(text, fun, value->local_index, 8);
      return true;
    case IR_VALUE_FS_HOST:
      // std.fs.host() is a stateless capability token; mirrors ELF/macho — return 0.
      z_x64_emit_xor_eax_eax(text);
      return true;
    case IR_VALUE_FS_MUNMAP: return coff_emit_fs_munmap_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_VIEW_LEN: return coff_emit_byte_view_len(text, fun, value->left, ctx, diag);
    case IR_VALUE_BYTE_COPY: return coff_emit_byte_copy_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_FILL: return coff_emit_byte_fill_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_VIEW_EQ: return coff_emit_byte_view_eq_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_VIEW_INDEX_LOAD: return coff_emit_byte_view_index_load_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_VIEW_READ_INT_LE: case IR_VALUE_BYTE_VIEW_READ_FLOAT_LE: return coff_emit_byte_le_read_value(text, fun, value, ctx, diag);
    case IR_VALUE_INDEX_LOAD: return coff_emit_index_load_value(text, fun, value, ctx, diag);
    case IR_VALUE_FIELD_LOAD: return coff_emit_field_load_value(text, fun, value, diag);
    case IR_VALUE_MATH_SQRTF: case IR_VALUE_MATH_EXPF: case IR_VALUE_MATH_COSF: case IR_VALUE_MATH_SINF:
    case IR_VALUE_MATH_FABSF: case IR_VALUE_MATH_FLOORF:
      return coff_emit_math_unary_value(text, fun, value, ctx, diag);
    case IR_VALUE_MATH_POWF: return coff_emit_math_powf_value(text, fun, value, ctx, diag);
    case IR_VALUE_MATH_ISNANF: return coff_emit_math_isnanf_value(text, fun, value, ctx, diag);
    case IR_VALUE_CHECK: return coff_emit_check_value(text, fun, value, ctx, diag);
    case IR_VALUE_RESCUE: return coff_emit_rescue_value(text, fun, value, ctx, diag);
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported value kind %d", value ? (int)value->kind : -1);
      return coff_diag_at(diag, "direct COFF value kind is unsupported", value->line, value->column, actual);
    }
  }
}

static bool coff_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, CoffEmitContext *ctx, ZDiag *diag);

static bool coff_emit_world_write(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  if (!instr || !instr->value) return coff_diag_at(diag, "direct COFF World write requires bytes", instr ? instr->line : 1, instr ? instr->column : 1, "missing byte view");
  if (!coff_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_push_rax(text); // preserve ptr while computing len
  if (!coff_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_mov_reg_from_rax(text, 8, false);
  z_x64_emit_pop_reg64(text, 2);
  z_x64_emit_mov_reg_u32(text, 1, instr->field_offset == 2 ? 2u : 1u); // ecx = fd
  z_x64_emit_sub_rsp(text, 32);
  size_t patch = z_x64_emit_call32_placeholder(text);
  z_x64_emit_add_rsp(text, 32);
  z_x64_emit_test_rax_rax(text, false);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  z_x64_emit_ud2(text); // ud2 on runtime write failure
  z_x64_patch_rel32(text, ok_patch, text->len);
  return z_coff_record_instr_runtime_patch(ctx, COFF_RUNTIME_WORLD_WRITE, patch, instr, diag);
}

static bool coff_emit_local_set_byte_view(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  // A span-returning call lands ptr in rax and len in rdx (Win64 second-return). Capture
  // both before evaluating anything else; falling back to byte_view_ptr/_len would re-evaluate.
  if (instr->value && instr->value->kind == IR_VALUE_CALL && instr->value->type == IR_TYPE_BYTE_VIEW) {
    if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
    coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
    coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 2, 8, false);
    return true;
  }
  if (!coff_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
  if (!coff_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 8, false);
  return true;
}
// Maybe<MutSpan<u8>> layout: {has@0 u32, ptr@8 i64, len@16 u64}. A failure path zeros every
// slot so a downstream `.has` check reads 0. Mirrors machx64_emit_maybe_clear.
static void coff_emit_maybe_clear(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  z_x64_emit_xor_eax_eax(text);
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false);
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 16, true);
}

// Maybe<scalar> layout: {has@0 u32, value@8 i64}. Failure path zeros both slots.
static void coff_emit_maybe_scalar_clear(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  z_x64_emit_xor_eax_eax(text);
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false);
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);
}

// Success path for a Maybe<scalar>: rax already holds the payload value. Push/pop preserves
// the payload across the has store.
static void coff_emit_maybe_scalar_store_rax(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  z_x64_emit_push_rax(text);
  z_x64_emit_mov_eax_u32(text, 1);
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false);
  z_x64_emit_pop_rax(text);
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);
}

// Emit a Win64 indirect kernel32 call through the .idata IAT (`ff 15 disp32`), with the
// mandatory 32-byte shadow space. The disp32 patch is recorded against the kernel32 import slot for
// later resolution (exe path: import_patches; obj path: external symbol REL32 reloc).
static bool coff_emit_kernel32_call(ZBuf *text, CoffEmitContext *ctx, unsigned import_index, const IrValue *value, ZDiag *diag) {
  z_x64_emit_sub_rsp(text, 32);
  size_t patch = z_x64_emit_call_rip32_placeholder(text);
  z_x64_emit_add_rsp(text, 32);
  return z_coff_record_mmap_patch(ctx, patch, import_index, value, diag);
}

// `std.mem.pageAlloc` allocBytes — request a kernel-zeroed anonymous region via kernel32
// VirtualAlloc(NULL, size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE). The size value is evaluated then
// spilled across the call, the result populates the Maybe<MutSpan<u8>> dest local: on success has=1
// @0, ptr@8, len@16; on failure (return == NULL) the Maybe is cleared. Win64 reg conv: rcx/rdx/r8/r9.
static bool coff_emit_anon_alloc_to_local(ZBuf *text, const IrFunction *fun, const IrValue *size, unsigned local_index, CoffEmitContext *ctx, ZDiag *diag) {
  if (!size) return coff_diag_at(diag, "direct COFF page allocation requires a byte length", 1, 1, "missing length");
  if (!coff_emit_value(text, fun, size, ctx, diag)) return false;            // rax = len
  z_x64_emit_push_rax(text);                                                  // [rsp] = len (survives the call)
  z_x64_emit_mov_rdx_from_rax(text);                                          // rdx = dwSize = len
  z_x64_emit_xor_reg_reg(text, 1, true);                                      // rcx = lpAddress = NULL
  z_x64_emit_mov_reg_u32(text, 8, 0x3000);                                    // r8 = MEM_COMMIT|MEM_RESERVE
  z_x64_emit_mov_reg_u32(text, 9, 0x04);                                      // r9 = PAGE_READWRITE
  if (!coff_emit_kernel32_call(text, ctx, Z_COFF_IMPORT_VIRTUAL_ALLOC, size, diag)) return false;
  z_x64_emit_test_rax_rax(text, true);
  size_t fail = z_x64_emit_jcc32_placeholder(text, 0x84);                     // ZF==1 (NULL) → failure
  z_x64_emit_push_rax(text);                                                  // [rsp+0]=addr, [rsp+8]=len
  z_x64_emit_mov_eax_u32(text, 1);
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false);   // has = 1
  z_x64_emit_pop_rax(text);                                                   // rax = addr
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);    // ptr
  z_x64_emit_pop_rax(text);                                                   // rax = len
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 16, true);   // len (64-bit byte count)
  size_t end = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, fail, text->len);
  z_x64_emit_add_rsp(text, 8);                                                // drop the pushed len before clearing
  coff_emit_maybe_clear(text, fun, local_index);
  z_x64_patch_rel32(text, end, text->len);
  return true;
}

// `let m = std.fs.mmap(fs, path)` -> Maybe<owned<Mapping>>. Plausible Win64 sequence:
//   CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
//   GetFileSizeEx(handle, &size_out)
//   CreateFileMappingA(handle, NULL, PAGE_READONLY, 0, 0, NULL)
//   MapViewOfFile(mapping_handle, FILE_MAP_READ, 0, 0, 0)
//   CloseHandle(mapping_handle); CloseHandle(file_handle)
// For build-only verification, any failure clears the Maybe. The path arg is the underlying span ptr
// (CreateFileA wants a NUL-terminated C string; literals carry a trailing NUL, which is good enough).
// The size slot is parked in a stack scratch area inside the per-call frame.
static bool coff_emit_fs_mmap_to_local(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned local_index, CoffEmitContext *ctx, ZDiag *diag) {
  const IrValue *path = instr->value->left;
  if (!path) return coff_diag_at(diag, "direct COFF std.fs.mmap requires a path", instr->line, instr->column, "missing path");
  // Reserve a 16-byte stack scratch slot for the GetFileSizeEx output (8 bytes), pushed before any
  // calls and popped before joining the end. The maybe-clear failure path drops this slot.
  z_x64_emit_sub_rsp(text, 16);
  // CreateFileA(path, GENERIC_READ=0x80000000, FILE_SHARE_READ=1, NULL, OPEN_EXISTING=3,
  //              FILE_ATTRIBUTE_NORMAL=0x80, NULL)
  if (!coff_emit_byte_view_ptr(text, fun, path, ctx, diag)) return false;     // rax = path ptr
  z_x64_emit_mov_rcx_from_rax(text, true);                                    // rcx = lpFileName
  z_x64_emit_mov_reg_u32(text, 2, 0x80000000u);                               // edx = GENERIC_READ
  z_x64_emit_mov_reg_u32(text, 8, 1);                                         // r8 = FILE_SHARE_READ
  z_x64_emit_xor_reg_reg(text, 9, true);                                      // r9 = NULL (lpSecurityAttributes)
  // The remaining 3 stack args go above the shadow space within the kernel32 call. The shadow
  // space is alloc'd by coff_emit_kernel32_call (sub rsp, 32), so we reserve room here too.
  z_x64_emit_sub_rsp(text, 32);                                               // shadow space for the prep stores
  z_x64_emit_mov_rsp_offset_u32(text, 32, 3, true);                           // [rsp+32] = OPEN_EXISTING (qword)
  z_x64_emit_mov_rsp_offset_u32(text, 40, 0x80, true);                        // [rsp+40] = FILE_ATTRIBUTE_NORMAL
  z_x64_emit_mov_rsp_offset_u32(text, 48, 0, true);                           // [rsp+48] = hTemplateFile=NULL
  z_x64_emit_add_rsp(text, 32);                                               // unwind that prep
  if (!coff_emit_kernel32_call(text, ctx, Z_COFF_IMPORT_CREATE_FILE_A, path, diag)) return false;
  // Test for INVALID_HANDLE_VALUE (-1). cmp rax, -1 → JE fail.
  z_x64_emit_mov_rcx_from_rax(text, true);                                    // save handle in rcx
  z_x64_emit_mov_rax_u64(text, 0xffffffffffffffffull);
  z_x64_emit_cmp_rax_rcx(text, true);
  size_t fail_open = z_x64_emit_jcc32_placeholder(text, 0x84);                // JE: handle == -1 → failure
  // We have a valid file handle in rcx. Stash it on the scratch slot to survive the next calls.
  z_x64_emit_store_rsp_offset_reg(text, 1, 0, true);                          // [rsp+0] = file_handle
  // GetFileSizeEx(handle, &size_out) — pSize at [rsp+8].
  z_x64_emit_lea_rsp_offset_reg(text, 2, 8);                                  // rdx = &size_out
  // rcx is already file_handle.
  if (!coff_emit_kernel32_call(text, ctx, Z_COFF_IMPORT_GET_FILE_SIZE_EX, path, diag)) return false;
  // CreateFileMappingA(handle, NULL, PAGE_READONLY=2, 0, 0, NULL).
  z_x64_emit_load_rsp_offset_reg(text, 1, 0, true);                           // rcx = file_handle
  z_x64_emit_xor_reg_reg(text, 2, true);                                      // rdx = NULL
  z_x64_emit_mov_reg_u32(text, 8, 2);                                         // r8 = PAGE_READONLY
  z_x64_emit_xor_reg_reg(text, 9, true);                                      // r9 = 0 (high size)
  if (!coff_emit_kernel32_call(text, ctx, Z_COFF_IMPORT_CREATE_FILE_MAPPING_A, path, diag)) return false;
  z_x64_emit_test_rax_rax(text, true);
  size_t fail_map = z_x64_emit_jcc32_placeholder(text, 0x84);                 // ZF==1 → CreateFileMappingA failed
  // MapViewOfFile(mapping_handle, FILE_MAP_READ=4, 0, 0, 0). Save mapping handle first.
  z_x64_emit_mov_rcx_from_rax(text, true);                                    // rcx = mapping_handle
  z_x64_emit_mov_reg_u32(text, 2, 4);                                         // edx = FILE_MAP_READ
  z_x64_emit_xor_reg_reg(text, 8, true);                                      // r8 = 0
  z_x64_emit_xor_reg_reg(text, 9, true);                                      // r9 = 0
  if (!coff_emit_kernel32_call(text, ctx, Z_COFF_IMPORT_MAP_VIEW_OF_FILE, path, diag)) return false;
  z_x64_emit_test_rax_rax(text, true);
  size_t fail_view = z_x64_emit_jcc32_placeholder(text, 0x84);                // view == NULL → failure
  // Success: write Maybe.has=1, ptr=rax, len=[rsp+8]. CloseHandle(file_handle) is omitted for the
  // build-only smoke (handle leak doesn't affect linkage); the kernel keeps the view valid.
  z_x64_emit_push_rax(text);                                                  // save view ptr ([rsp+0]=view, file_handle still at [rsp+8])
  z_x64_emit_mov_eax_u32(text, 1);
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false);   // has=1
  z_x64_emit_pop_rax(text);                                                   // rax = view ptr
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);    // ptr
  z_x64_emit_load_rsp_offset_reg(text, 0, 8, true);                           // rax = size (from scratch slot)
  coff_emit_store_local_slot_from_reg(text, fun, local_index, 0, 16, true);   // len
  z_x64_emit_add_rsp(text, 16);                                               // drop scratch (file_handle + size)
  size_t end = z_x64_emit_jmp32_placeholder(text, 0xe9);
  // Failure landings — all unwind the 16-byte scratch slot before clearing the Maybe.
  z_x64_patch_rel32(text, fail_open, text->len);
  z_x64_patch_rel32(text, fail_map, text->len);
  z_x64_patch_rel32(text, fail_view, text->len);
  z_x64_emit_add_rsp(text, 16);
  coff_emit_maybe_clear(text, fun, local_index);
  z_x64_patch_rel32(text, end, text->len);
  return true;
}

// FixedBufAlloc/PageAlloc init. PageAlloc has no pre-reserved buffer (each allocBytes is a
// fresh VirtualAlloc), so we zero the slot to keep it free of stale ptr/len. FixedBufAlloc seeds
// ptr@0 from backing.ptr, capacity@8 from backing.len, position@12 = 0.
static bool coff_emit_local_set_alloc(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  if (instr->value && instr->value->kind == IR_VALUE_PAGE_ALLOC) {
    z_x64_emit_xor_eax_eax(text);
    coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
    return true;
  }
  if (!instr->value || instr->value->kind != IR_VALUE_FIXED_BUF_ALLOC) return coff_diag_at(diag, "direct COFF FixedBufAlloc local requires std.mem.fixedBufAlloc", instr->line, instr->column, "unsupported allocator initializer");
  if (!coff_emit_byte_view_ptr(text, fun, instr->value->left, ctx, diag)) return false;
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
  if (!coff_emit_byte_view_len(text, fun, instr->value->left, ctx, diag)) return false;
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 8, false);
  z_x64_emit_mov_eax_u32(text, 0);
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 12, false);
  return true;
}
static bool coff_emit_local_set_vec(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  if (!instr->value || instr->value->kind != IR_VALUE_VEC_INIT) return coff_diag_at(diag, "direct COFF Vec local requires std.mem.vec", instr->line, instr->column, "unsupported Vec initializer");
  if (!coff_emit_byte_view_ptr(text, fun, instr->value->left, ctx, diag)) return false;
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
  z_x64_emit_mov_eax_u32(text, 0);
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 8, false);
  if (!coff_emit_byte_view_len(text, fun, instr->value->left, ctx, diag)) return false;
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 12, false);
  return true;
}

// FixedBufAlloc bump allocator. Mirrors machx64_emit_alloc_bytes_to_local. On success the
// Maybe<MutSpan<u8>> dest gets has=1, ptr=alloc.ptr+pos, len=n; on overflow the Maybe is cleared.
static bool coff_emit_alloc_bytes_to_local(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  const IrValue *value = instr->value;
  if (!value || value->kind != IR_VALUE_ALLOC_BYTES || value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_ALLOC) return coff_diag_at(diag, "direct COFF allocation source is invalid", instr->line, instr->column, "invalid allocation");
  if (fun->locals[value->local_index].is_page_alloc) {
    // PageAlloc: each allocation is a fresh kernel32 VirtualAlloc region.
    return coff_emit_anon_alloc_to_local(text, fun, value->left, instr->local_index, ctx, diag);
  }
  if (!coff_emit_value(text, fun, instr->value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  coff_emit_load_local_slot_eax(text, fun, instr->value->local_index, 12);
  coff_emit_load_local_slot_reg(text, fun, instr->value->local_index, 8, 1, false);
  z_x64_emit_pop_rax(text);
  z_x64_emit_mov_reg_from_reg(text, 2, 0, false);
  z_x64_emit_add_rax_rcx(text, false);
  z_x64_emit_cmp_rax_rcx(text, false);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x86);
  z_x64_emit_mov_eax_u32(text, 0);
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, false);
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 8, true);
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 16, false);
  size_t end_patch = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, ok_patch, text->len);
  z_x64_emit_mov_eax_u32(text, 1);
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, false);
  coff_emit_load_local_slot_reg(text, fun, instr->value->local_index, 0, 2, true);
  z_x64_emit_add_reg_reg(text, 2, 2, true);
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 2, 8, true);
  z_x64_emit_mov_reg_from_reg(text, 0, 2, false);
  coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 16, false);
  z_x64_emit_mov_eax_from_ecx(text);
  coff_emit_store_local_slot_from_reg(text, fun, instr->value->local_index, 0, 12, false);
  z_x64_patch_rel32(text, end_patch, text->len);
  return true;
}

static bool coff_emit_local_set_maybe_byte_view(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  // Route on the initializer's IR kind. ALLOC_BYTES drives FixedBufAlloc bump or PageAlloc
  // mmap (each per its is_page_alloc tag). FS_MMAP drives the kernel32 file-mapping sequence.
  if (instr->value && instr->value->kind == IR_VALUE_ALLOC_BYTES) return coff_emit_alloc_bytes_to_local(text, fun, instr, ctx, diag);
  if (instr->value && instr->value->kind == IR_VALUE_FS_MMAP) return coff_emit_fs_mmap_to_local(text, fun, instr, instr->local_index, ctx, diag);
  return coff_diag_at(diag, "direct COFF Maybe<byte-view> initializer is unsupported", instr->line, instr->column, "unsupported maybe-byte-view initializer");
}

// Maybe<scalar> local. MAYBE_SCALAR_LITERAL is a constant {has = data_len != 0, value =
// int_value} pair; the plain fallible-scalar-call path saves the packed rax, inspects the tag,
// branches to clear-or-store. Mirrors machx64_emit_maybe_scalar_local_set.
static bool coff_emit_local_set_maybe_scalar(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  if (!instr->value) return coff_diag_at(diag, "direct COFF Maybe scalar initializer is missing", instr->line, instr->column, "missing maybe value");
  if (instr->value->kind == IR_VALUE_MAYBE_SCALAR_LITERAL) {
    z_x64_emit_mov_eax_u32(text, instr->value->data_len ? 1u : 0u);
    coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, false);
    z_x64_emit_mov_eax_u32(text, (uint32_t)instr->value->int_value);
    coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 8, true);
    return true;
  }
  if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  coff_emit_error_condition_from_rax(text);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  z_x64_emit_pop_rax(text);
  coff_emit_maybe_scalar_clear(text, fun, instr->local_index);
  size_t end = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, ok_patch, text->len);
  z_x64_emit_pop_rax(text);
  coff_emit_maybe_scalar_store_rax(text, fun, instr->local_index);
  z_x64_patch_rel32(text, end, text->len);
  return true;
}

// std.fs.munmap(&mut m) — release a Mapping via kernel32 UnmapViewOfFile(lpBaseAddress).
// Loads mapping.ptr (slot 0, 64-bit) into rcx and emits the indirect kernel32 call. Result is Void;
// nothing is produced into rax.
static bool coff_emit_fs_munmap_value(ZBuf *text, const IrFunction *fun, const IrValue *value, CoffEmitContext *ctx, ZDiag *diag) {
  if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_BYTE_VIEW) {
    return coff_diag_at(diag, "direct COFF std.fs.munmap requires a Mapping local", value->line, value->column, "invalid Mapping");
  }
  coff_emit_load_local_slot_rax(text, fun, value->local_index, 0); // rax = addr
  z_x64_emit_mov_rcx_from_rax(text, true);                          // rcx = lpBaseAddress
  return coff_emit_kernel32_call(text, ctx, Z_COFF_IMPORT_UNMAP_VIEW_OF_FILE, value, diag);
}

static bool coff_emit_local_set_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  if (instr->local_index >= fun->local_len) return coff_diag_at(diag, "direct COFF local store is out of range", instr->line, instr->column, "invalid local");
  const IrLocal *local = &fun->locals[instr->local_index];
  IrTypeKind ltype = local->type;
  // Record local — bind via sret (no copy) when the source is a record-returning call, or
  // byte-copy when the source is another record local. Shape literals are already handled by the
  // upstream lowering which emits a sequence of FIELD_STORE instructions.
  if (local->is_record) {
    if (instr->value && instr->value->kind == IR_VALUE_CALL) {
      return coff_emit_record_call_with_dest(text, fun, (int)instr->local_index, instr->value, ctx, diag);
    }
    if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
      coff_emit_record_copy_to(text, fun, instr->local_index, instr->value->local_index);
      return true;
    }
    return coff_diag_at(diag, "direct COFF record local assignment requires a record value", instr->line, instr->column, "unsupported record set");
  }
  switch (ltype) {
    case IR_TYPE_BYTE_VIEW: return coff_emit_local_set_byte_view(text, fun, instr, ctx, diag);
    case IR_TYPE_ALLOC: return coff_emit_local_set_alloc(text, fun, instr, ctx, diag);
    case IR_TYPE_VEC: return coff_emit_local_set_vec(text, fun, instr, ctx, diag);
    case IR_TYPE_MAYBE_BYTE_VIEW: return coff_emit_local_set_maybe_byte_view(text, fun, instr, ctx, diag);
    // Maybe<scalar> locals — MAYBE_SCALAR_LITERAL or fallible-scalar-call result.
    case IR_TYPE_MAYBE_SCALAR: return coff_emit_local_set_maybe_scalar(text, fun, instr, ctx, diag);
    default:
      if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
      if (coff_type_is_float(ltype)) coff_emit_store_local_xmm0(text, fun, instr->local_index);
      // i64/u64 locals store via the 64-bit `mov [rbp+disp], rax` form
      // (coff_emit_store_local_slot_from_reg with wide=true) so the upper 32 bits land in the slot.
      else if (coff_type_is_scalar64(ltype)) coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
      else coff_emit_store_local_from_reg(text, fun, instr->local_index, 0);
      return true;
  }
}
static bool coff_emit_field_store_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  // local_index == UINT_MAX targets the record being returned, written through the saved
  // sret pointer (the `return <shape literal>` lowering stores its fields this way).
  if (instr->local_index == UINT_MAX) {
    IrTypeKind value_type = instr->value ? instr->value->type : IR_TYPE_I32;
    unsigned slot = coff_sret_slot_offset(fun);
    if (value_type == IR_TYPE_BYTE_VIEW) {
      // Span field via sret: ptr at field_offset, len at field_offset+8. A span-returning call lands
      // ptr in rax + len in rdx; any other byte view materializes ptr-then-len. r11 holds the
      // reloaded sret pointer (the saved rcx may have been clobbered between calls).
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
        z_x64_emit_rbp_disp_reg(text, 0x8b, 11, slot, true);
        z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, instr->field_offset, 0, true);    // mov [r11+off], rax (ptr)
        z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, instr->field_offset + 8u, 2, true); // mov [r11+off+8], rdx (len)
        return true;
      }
      if (!coff_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
      z_x64_emit_push_rax(text);
      if (!coff_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
      z_x64_emit_mov_reg_from_reg(text, 2, 0, true);   // mov rdx, rax (len)
      z_x64_emit_pop_reg64(text, 0);                   // pop rax (ptr)
      z_x64_emit_rbp_disp_reg(text, 0x8b, 11, slot, true);
      z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, instr->field_offset, 0, true);
      z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, instr->field_offset + 8u, 2, true);
      return true;
    }
    // Primitive (or float) field via sret. Materialize the value, then store through saved sret ptr in r11.
    if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
    z_x64_emit_rbp_disp_reg(text, 0x8b, 11, slot, true);
    if (coff_type_is_float(value_type)) {
      // movss/sd [r11 + off], xmm0. movs_xmm_ptr_reg has no displacement form; advance r11 if needed.
      if (instr->field_offset > 0) z_x64_emit_add_reg_u32(text, 11, instr->field_offset, true);
      z_x64_emit_movs_xmm_ptr_reg(text, 0, 11, coff_type_is_f64(value_type), false);
    } else if (value_type == IR_TYPE_U8 || value_type == IR_TYPE_BOOL) {
      z_x64_emit_store_ptr_reg_disp_from_reg8(text, 11, instr->field_offset, 0);
    } else {
      bool wide = coff_type_is_i64(value_type);
      z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, instr->field_offset, 0, wide);
    }
    return true;
  }
  if (instr->local_index >= fun->local_len) return coff_diag_at(diag, "direct COFF field store record is out of range", instr->line, instr->column, "invalid record local");
  if (!fun->locals[instr->local_index].is_record) return coff_diag_at(diag, "direct COFF field store requires record local", instr->line, instr->column, "non-record local");
  IrTypeKind value_type = instr->value ? instr->value->type : IR_TYPE_I32;
  // Byte-view field store into a record local — span fat-pointer ptr@field_offset, len@+8.
  if (value_type == IR_TYPE_BYTE_VIEW) {
    if (instr->value->kind == IR_VALUE_CALL) {
      if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
      coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, instr->field_offset, true);
      coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 2, instr->field_offset + 8u, true);
      return true;
    }
    if (!coff_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
    coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, instr->field_offset, true);
    if (!coff_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
    coff_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, instr->field_offset + 8u, true);
    return true;
  }
  if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
  coff_emit_store_field_from_eax(text, fun, instr->local_index, instr->field_offset, value_type);
  return true;
}

static bool coff_emit_index_store_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  if (instr->array_index >= fun->local_len) return coff_diag_at(diag, "direct COFF indexed store array is out of range", instr->line, instr->column, "invalid array local");
  const IrLocal *local = &fun->locals[instr->array_index];
  // Typed-span element store. Materialize the value first and spill it before computing the
  // address, since the address machinery clobbers rax/rcx. Float values store via movss/movsd; u8/i8/
  // Bool write the low byte; 8-byte via 64-bit mov; else 32-bit mov. Mirrors machx64_emit_index_store_instr.
  if (local->type == IR_TYPE_BYTE_VIEW) {
    IrTypeKind elem = local->element_type;
    if (coff_type_is_float(elem)) {
      bool is64 = coff_type_is_f64(elem);
      if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
      z_x64_emit_xmm_push(text, 0);
      if (!coff_emit_span_index_addr(text, fun, instr->array_index, instr->index, ctx, diag)) return false;
      z_x64_emit_xmm_pop(text, 0);
      z_x64_emit_movs_xmm_ptr_reg(text, 0, 0, is64, false);
      return true;
    }
    if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
    z_x64_emit_push_rax(text);
    if (!coff_emit_span_index_addr(text, fun, instr->array_index, instr->index, ctx, diag)) return false;
    z_x64_emit_pop_reg64(text, 1); // value -> rcx; element address is in rax
    if (elem == IR_TYPE_U8 || elem == IR_TYPE_BOOL || elem == IR_TYPE_I8) z_x64_emit_store_ptr_reg8_from_reg(text, 0, 1);
    else if (coff_type_is_i64(elem)) z_x64_emit_store_ptr_reg_from_reg(text, 0, 1, true);
    else z_x64_emit_store_ptr_reg_from_reg(text, 0, 1, false);
    return true;
  }
  unsigned const_index = 0;
  if (local->is_array && local->element_type != IR_TYPE_U8 && coff_const_u32_value(instr->index, &const_index) && const_index < local->array_len) {
    if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
    coff_emit_store_local_slot_from_reg(text, fun, instr->array_index, 0, const_index * 4u, false);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
    if (!instr->index || !coff_emit_value(text, fun, instr->index, ctx, diag)) return false;
    coff_emit_u8_array_bounds_check(text, local);
    z_x64_emit_push_rax(text);
    if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
    z_x64_emit_pop_reg64(text, 1);
    coff_emit_array_base_rdx(text, fun, instr->array_index);
    z_x64_emit_shl_rcx_imm8(text, 2);
    z_x64_emit_add_rdx_rcx(text, true);
    z_x64_emit_store_ptr_reg_from_reg(text, 2, 0, false);
    return true;
  }
  if (!local->is_array || local->element_type != IR_TYPE_U8) return coff_diag_at(diag, "direct COFF indexed store requires [N]u8 or integer arrays", instr->line, instr->column, "unsupported array local");
  if (!instr->index || !coff_emit_value(text, fun, instr->index, ctx, diag)) return false;
  coff_emit_u8_array_bounds_check(text, local);
  z_x64_emit_push_rax(text);
  if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_pop_reg64(text, 1);
  coff_emit_array_base_rdx(text, fun, instr->array_index);
  z_x64_emit_add_rdx_rcx(text, true);
  z_x64_emit_store_ptr_reg8_from_reg(text, 2, 0);
  return true;
}

static bool coff_emit_if_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_test_rax_rax(text, false);
  size_t false_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  if (!coff_emit_instrs(text, fun, instr->then_instrs, instr->then_len, ctx, diag)) return false;
  if (instr->else_len > 0) {
    size_t end_patch = z_x64_emit_jmp32_placeholder(text, 0xe9);
    z_x64_patch_rel32(text, false_patch, text->len);
    if (!coff_emit_instrs(text, fun, instr->else_instrs, instr->else_len, ctx, diag)) return false;
    z_x64_patch_rel32(text, end_patch, text->len);
  } else z_x64_patch_rel32(text, false_patch, text->len);
  return true;
}

static bool coff_emit_while_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  size_t loop_start = text->len;
  if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_test_rax_rax(text, false);
  size_t false_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  if (!coff_emit_instrs(text, fun, instr->then_instrs, instr->then_len, ctx, diag)) return false;
  size_t loop_patch = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, loop_patch, loop_start);
  z_x64_patch_rel32(text, false_patch, text->len);
  return true;
}

static bool coff_emit_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, CoffEmitContext *ctx, ZDiag *diag) {
  switch (instr->kind) {
    case IR_INSTR_WORLD_WRITE: return coff_emit_world_write(text, fun, instr, ctx, diag);
    case IR_INSTR_LOCAL_SET: return coff_emit_local_set_instr(text, fun, instr, ctx, diag);
    case IR_INSTR_FIELD_STORE: return coff_emit_field_store_instr(text, fun, instr, ctx, diag);
    case IR_INSTR_INDEX_STORE: return coff_emit_index_store_instr(text, fun, instr, ctx, diag);
    case IR_INSTR_EXPR: return !instr->value || coff_emit_value(text, fun, instr->value, ctx, diag);
    case IR_INSTR_RETURN:
      // Record return — write through caller's sret pointer. A record-returning call
      // routes its sret target through ours (no temp local); a record local is byte-copied; a
      // shape literal had its fields already written through sret via FIELD_STORE UINT_MAX.
      if (coff_returns_record(fun)) {
        if (instr->value && instr->value->kind == IR_VALUE_CALL) {
          if (!coff_emit_record_call_with_dest(text, fun, -1, instr->value, ctx, diag)) return false;
          // Reload sret pointer into rax (the callee already wrote it but our spilled sret slot is
          // the authoritative source — mirrors macho_x64's pattern).
          z_x64_emit_rbp_disp_reg(text, 0x8b, 0, coff_sret_slot_offset(fun), true);
          coff_emit_epilogue(text);
          return true;
        }
        if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
          coff_emit_record_copy_to(text, fun, UINT_MAX, instr->value->local_index);
        }
        // For shape literals, FIELD_STORE UINT_MAX already wrote the fields. Hand back sret ptr.
        z_x64_emit_rbp_disp_reg(text, 0x8b, 0, coff_sret_slot_offset(fun), true);
        coff_emit_epilogue(text);
        return true;
      }
      // Span return — ptr in rax, len in rdx (Win64 second-return). A span-returning call
      // already lands both; any other byte view materializes ptr-then-len.
      if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
        if (instr->value->kind == IR_VALUE_CALL) {
          if (!coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
        } else {
          if (!coff_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
          z_x64_emit_push_rax(text);
          if (!coff_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
          z_x64_emit_mov_reg_from_reg(text, 2, 0, true); // mov rdx, rax (len)
          z_x64_emit_pop_reg64(text, 0);                  // pop rax (ptr)
        }
        coff_emit_epilogue(text);
        return true;
      }
      if (instr->value && !coff_emit_value(text, fun, instr->value, ctx, diag)) return false;
      // In a raising function, the success-path return packs the payload into rax's
      // low 32 bits with a zero tag in the upper 32. For int payloads, a 32-bit self-move zero-
      // extends; for void, just xor rax. Float payloads keep their xmm0 result and the int rax
      // (carrying junk in the upper) is cleared by mov eax, eax. Mirrors macho_x64's path.
      if (coff_function_raises(fun) && !instr->value) z_x64_emit_xor_rax_rax(text);
      else if (coff_function_raises(fun) && instr->value && !coff_type_is_i64(instr->value->type)) z_x64_emit_mov_reg_from_reg(text, 0, 0, false);
      coff_emit_epilogue(text);
      return true;
    case IR_INSTR_RAISE:
      if (!coff_function_raises(fun)) return coff_diag_at(diag, "direct COFF raise requires a fallible function context", instr->line, instr->column, "non-fallible context");
      // Pack the error code into rax[63:32]; the caller's CHECK extracts via shr rcx, 32.
      z_x64_emit_mov_rax_u64(text, ((uint64_t)(instr->error_code ? instr->error_code : IR_ERROR_UNKNOWN)) << 32);
      coff_emit_epilogue(text);
      return true;
    case IR_INSTR_IF: return coff_emit_if_instr(text, fun, instr, ctx, diag);
    case IR_INSTR_WHILE: return coff_emit_while_instr(text, fun, instr, ctx, diag);
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported instruction kind %d", instr ? (int)instr->kind : -1);
      return coff_diag_at(diag, "direct COFF instruction kind is unsupported", instr->line, instr->column, actual);
    }
  }
}

static bool coff_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, CoffEmitContext *ctx, ZDiag *diag) {
  for (size_t i = 0; i < len; i++) {
    if (!coff_emit_instr(text, fun, &instrs[i], ctx, diag)) return false;
  }
  return true;
}

static bool coff_validate_function(const IrFunction *fun, ZDiag *diag) {
  if (fun->param_count > 8) return coff_diag_at(diag, "direct COFF object backend supports at most eight integer parameters", fun->line, fun->column, fun->name);
  // i64/u64 returns piggyback on rax just like the SysV emitters.
  // Also accept RECORD returns (via Win64 sret in rcx) and BYTE_VIEW returns (ptr in rax,
  // len in rdx). A raising record-returning fn is rejected: the macho_x64 sibling pre-empts that
  // by routing the tag through rdx, but rdx is the second-return register on Win64 too — a span
  // return is fine, a record return packing tag in high 32 of rax would collide with the sret-ptr
  // return convention.
  if (fun->return_type != IR_TYPE_VOID && fun->return_type != IR_TYPE_RECORD && fun->return_type != IR_TYPE_BYTE_VIEW
      && !coff_type_is_scalar32(fun->return_type) && !coff_type_is_scalar64(fun->return_type) && !coff_type_is_float(fun->return_type)) {
    return coff_diag_at(diag, "direct COFF object backend currently supports only Void, numeric, record, and span returns", fun->line, fun->column, fun->name);
  }
  if (fun->raises && fun->return_type == IR_TYPE_RECORD) {
    return coff_diag_at(diag, "direct COFF object backend cannot return a record from a raising function", fun->line, fun->column, fun->name);
  }
  if (fun->raises && fun->return_type == IR_TYPE_BYTE_VIEW) {
    return coff_diag_at(diag, "direct COFF object backend cannot return a span from a raising function", fun->line, fun->column, fun->name);
  }
  // With a record return, rcx is consumed by sret — only 3 user int slots remain
  // (rdx/r8/r9). Re-walk params and count int slots (records count as 1 ptr slot, spans as 2).
  if (coff_returns_record(fun)) {
    size_t int_slots = 1; // sret in rcx
    for (size_t i = 0; i < fun->param_count; i++) {
      IrTypeKind t = fun->locals[i].type;
      if (coff_type_is_float(t)) continue;
      if (t == IR_TYPE_BYTE_VIEW) int_slots += 2u;
      else int_slots += 1u;
    }
    if (int_slots > 4) return coff_diag_at(diag, "direct COFF record-returning function has too many integer parameter slots (sret in rcx consumes one)", fun->line, fun->column, fun->name);
  }
  for (size_t i = 0; i < fun->local_len; i++) {
    if (fun->locals[i].type == IR_TYPE_BYTE_VIEW) {
      // Typed-span locals (Span<T>/MutSpan<T>) for T in {u8, i8, i32, u32, i64, u64, f32,
      // f64, bool} ride on IR_TYPE_BYTE_VIEW with element_type carrying T. The 16-byte layout
      // (ptr@0, 64-bit len@8) is element-type-agnostic; the dispatch on width happens in load/store.
      continue;
    }
    if (fun->locals[i].is_array && (fun->locals[i].element_type == IR_TYPE_U8 || fun->locals[i].element_type == IR_TYPE_U32 || fun->locals[i].element_type == IR_TYPE_I32 || fun->locals[i].element_type == IR_TYPE_USIZE)) continue;
    if (fun->locals[i].is_record) continue;
    if (fun->locals[i].type == IR_TYPE_ALLOC || fun->locals[i].type == IR_TYPE_MAYBE_BYTE_VIEW) continue;
    if (fun->locals[i].type == IR_TYPE_VEC) continue;
    if (!fun->locals[i].is_array && coff_type_is_float(fun->locals[i].type)) continue;
    // Scalar i64/u64 locals (no array form yet — codec LE reads land into a flat scalar).
    if (!fun->locals[i].is_array && coff_type_is_scalar64(fun->locals[i].type)) continue;
    if (fun->locals[i].is_array || !coff_type_is_scalar32(fun->locals[i].type)) {
      return coff_diag_at(diag, "direct COFF object backend currently supports only primitive scalar locals", fun->locals[i].line, fun->locals[i].column, fun->locals[i].name);
    }
  }
  return true;
}

static bool coff_emit_function_text(ZBuf *text, const IrFunction *fun, CoffEmitContext *ctx, ZDiag *diag) {
  static const unsigned param_regs[] = {1, 2, 8, 9};
  // Reserve an extra 16-byte slot when the function returns a record, so the prologue
  // can park the caller's sret pointer (rcx) at [rbp - sret_slot_offset]. The slot stays 16-aligned.
  unsigned base_frame = coff_base_stack_size(fun);
  unsigned frame_size = base_frame + (coff_returns_record(fun) ? 16u : 0u);
  z_x64_emit_prologue(text, frame_size);
  size_t abi_slot = 0;   // Win64 GPR bank index (param_regs)
  size_t float_idx = 0;  // SSE bank index (xmm0..3); on Win64 floats consume the SAME slot as ints
                         //  (rcx ↔ xmm0, rdx ↔ xmm1, etc.), so abi_slot and float_idx march together
                         //  for non-record returns. With a record return, sret eats param_regs[0]/xmm0.
  if (coff_returns_record(fun)) {
    // Save the caller-supplied sret pointer; param_regs[0] (rcx) is now consumed.
    z_x64_emit_rbp_disp_reg(text, 0x89, 1, coff_sret_slot_offset(fun), true);
    abi_slot = 1;
    float_idx = 1;
  }
  for (size_t i = 0; i < fun->param_count; i++) {
    if (abi_slot < 4) {
      if (coff_type_is_float(fun->locals[i].type)) {
        coff_emit_store_local_xmm(text, fun, (unsigned)i, (unsigned)float_idx);
        abi_slot++; float_idx++;
        continue;
      }
      if (fun->locals[i].type == IR_TYPE_RECORD) {
        // By-value record arg passed by pointer in one GPR slot. Copy the pointed-to bytes
        // into the local's inline frame slot for value semantics.
        coff_emit_copy_record_param(text, fun, (unsigned)i, param_regs[abi_slot]);
        abi_slot++; float_idx++;
        continue;
      }
      coff_emit_store_local_from_reg(text, fun, (unsigned)i, param_regs[abi_slot]);
      abi_slot++; float_idx++;
    } else {
      // Params 5+ arrive on the stack (above the shadow space + return address). Always 8-byte.
      z_x64_emit_load_rbp_positive_reg(text, 0, 48u + (unsigned)(i - 4u) * 8u, false);
      coff_emit_store_local_from_reg(text, fun, (unsigned)i, 0);
    }
  }
  if (!coff_emit_instrs(text, fun, fun->instrs, fun->instr_len, ctx, diag)) return false;
  // RAISE also emits an epilogue, so don't double-emit when it tails the function.
  if (fun->instr_len == 0 || (fun->instrs[fun->instr_len - 1].kind != IR_INSTR_RETURN && fun->instrs[fun->instr_len - 1].kind != IR_INSTR_RAISE)) coff_emit_epilogue(text);
  return true;
}

static unsigned coff_rodata_base_offset(const IrProgram *program) {
  if (!program || program->data_segment_len == 0) return 0;
  unsigned base = program->data_segments[0].offset;
  for (size_t i = 1; i < program->data_segment_len; i++) {
    if (program->data_segments[i].offset < base) base = program->data_segments[i].offset;
  }
  return base;
}

static void coff_append_rodata(ZBuf *rodata, const IrProgram *program, unsigned base_offset) {
  for (size_t i = 0; program && i < program->data_segment_len; i++) {
    const IrDataSegment *segment = &program->data_segments[i];
    while (rodata->len < segment->offset - base_offset) z_coff_append_u8(rodata, 0);
    z_coff_append_bytes(rodata, (const char *)segment->bytes, segment->len);
  }
}

bool z_emit_coff_x64_object_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!program || !out) return coff_diag(diag, "direct COFF backend received no program");
  if (!program->mir_valid) {
    bool ok = coff_diag_at(diag, program->mir_message[0] ? program->mir_message : "direct backend lowering failed", program->mir_line, program->mir_column, program->mir_actual);
    z_diag_set_backend_blocker(diag, &program->backend_blocker);
    return ok;
  }
  if (program->function_len == 0) return coff_diag_at(diag, "direct COFF object backend requires at least one exported function", 1, 1, "empty program");
  bool has_export = false;
  for (size_t i = 0; i < program->function_len; i++) {
    if (program->functions[i].is_exported) has_export = true;
    if (!coff_validate_function(&program->functions[i], diag)) return false;
  }
  if (!has_export) return coff_diag_at(diag, "direct COFF object backend requires at least one exported function", 1, 1, "no exported function");

  ZBuf text;
  ZBuf rodata;
  ZBuf relocs;
  zbuf_init(&text);
  zbuf_init(&rodata);
  zbuf_init(&relocs);
  bool has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  unsigned rodata_base_offset = coff_rodata_base_offset(program);
  if (has_rodata) coff_append_rodata(&rodata, program, rodata_base_offset);
  size_t *offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  if (!offsets) {
    zbuf_free(&relocs);
    zbuf_free(&rodata);
    zbuf_free(&text);
    return coff_diag(diag, "out of memory while emitting COFF object");
  }
  CoffEmitContext ctx = {
    .program = program,
    .function_offsets = offsets,
    .function_count = program->function_len,
    .rodata_base_offset = rodata_base_offset
  };
  for (size_t i = 0; i < program->function_len; i++) {
    while (text.len % 16 != 0) z_x64_append_u8(&text, 0x90);
    offsets[i] = text.len;
    if (!coff_emit_function_text(&text, &program->functions[i], &ctx, diag)) {
      z_coff_emit_context_free(&ctx);
      free(offsets);
      zbuf_free(&relocs);
      zbuf_free(&rodata);
      zbuf_free(&text);
      return false;
    }
  }

  unsigned section_symbol_count = has_rodata ? 2u : 1u;
  bool has_world_write = z_coff_runtime_patch_count(&ctx, COFF_RUNTIME_WORLD_WRITE) > 0;
  size_t math_symbol_count = z_coff_math_used_symbol_count(&ctx);
  // kernel32 mmap/munmap/CreateFileA/.../VirtualAlloc — one external symbol per used import.
  size_t mmap_symbol_count = z_coff_mmap_used_symbol_count(&ctx);
  size_t symbol_len = program->function_len + (has_world_write ? 1u : 0u) + math_symbol_count + mmap_symbol_count;
  ZCoffSymbol *symbols = z_checked_calloc(symbol_len, sizeof(ZCoffSymbol));
  if (!symbols) {
    z_coff_emit_context_free(&ctx);
    free(offsets);
    zbuf_free(&relocs);
    zbuf_free(&rodata);
    zbuf_free(&text);
    return coff_diag(diag, "out of memory while emitting COFF object");
  }
  z_coff_patch_call_patches(&text, &ctx);
  z_coff_append_call_relocations(&relocs, &ctx, section_symbol_count);
  z_coff_append_rodata_relocations(&relocs, &ctx, 1u);
  uint32_t world_write_symbol_index = section_symbol_count + (uint32_t)program->function_len;
  z_coff_append_runtime_relocations(&relocs, &ctx, COFF_RUNTIME_WORLD_WRITE, world_write_symbol_index);
  uint32_t math_symbol_base = world_write_symbol_index + (has_world_write ? 1u : 0u);
  z_coff_append_math_relocations(&relocs, &ctx, math_symbol_base);
  uint32_t mmap_symbol_base = math_symbol_base + (uint32_t)math_symbol_count;
  z_coff_append_mmap_relocations(&relocs, &ctx, mmap_symbol_base);

  for (size_t i = 0; i < program->function_len; i++) {
    symbols[i] = (ZCoffSymbol){
      .name = program->functions[i].name ? program->functions[i].name : "zero_fn",
      .value = (uint32_t)offsets[i],
      .section_number = 1,
      .type = 0x20,
      .storage_class = program->functions[i].is_exported ? Z_COFF_SYMBOL_EXTERNAL : Z_COFF_SYMBOL_STATIC
    };
  }
  if (has_world_write) {
    symbols[program->function_len] = (ZCoffSymbol){
      .name = z_coff_runtime_helper_symbol(COFF_RUNTIME_WORLD_WRITE),
      .section_number = 0,
      .type = 0x20,
      .storage_class = Z_COFF_SYMBOL_EXTERNAL
    };
  }
  size_t math_slot = program->function_len + (has_world_write ? 1u : 0u);
  for (unsigned s = 0; s < COFF_MATH_COUNT; s++) {
    if (!z_coff_math_symbol_used(&ctx, (CoffMathSymbol)s)) continue;
    symbols[math_slot++] = (ZCoffSymbol){
      .name = z_coff_math_symbol_name((CoffMathSymbol)s),
      .section_number = 0,
      .type = 0x20,
      .storage_class = Z_COFF_SYMBOL_EXTERNAL
    };
  }
  // kernel32 mmap externals — emitted in the same monotonic order as
  // z_coff_append_mmap_relocations resolves their per-symbol slot.
  static const unsigned mmap_ordered_imports[] = {
    Z_COFF_IMPORT_CREATE_FILE_A, Z_COFF_IMPORT_GET_FILE_SIZE_EX, Z_COFF_IMPORT_CREATE_FILE_MAPPING_A,
    Z_COFF_IMPORT_MAP_VIEW_OF_FILE, Z_COFF_IMPORT_UNMAP_VIEW_OF_FILE, Z_COFF_IMPORT_CLOSE_HANDLE,
    Z_COFF_IMPORT_VIRTUAL_ALLOC,
  };
  size_t mmap_slot = math_slot;
  for (unsigned i = 0; i < sizeof(mmap_ordered_imports) / sizeof(mmap_ordered_imports[0]); i++) {
    if (!z_coff_mmap_import_used(&ctx, mmap_ordered_imports[i])) continue;
    symbols[mmap_slot++] = (ZCoffSymbol){
      .name = z_coff_mmap_import_name(mmap_ordered_imports[i]),
      .section_number = 0,
      .type = 0x20,
      .storage_class = Z_COFF_SYMBOL_EXTERNAL
    };
  }
  ZCoffObjectImage image = {
    .machine = Z_COFF_MACHINE_AMD64,
    .text = &text,
    .rodata = has_rodata ? &rodata : NULL,
    .text_relocs = &relocs,
    .text_reloc_count = (uint16_t)z_coff_text_relocation_count(&ctx),
    .symbols = symbols,
    .symbol_len = symbol_len
  };
  z_coff_write_object(out, &image);

  z_coff_emit_context_free(&ctx);
  free(offsets);
  free(symbols);
  zbuf_free(&relocs);
  zbuf_free(&rodata);
  zbuf_free(&text);
  return true;
}

static const IrFunction *coff_find_executable_main(const IrProgram *program, ZDiag *diag, unsigned *out_index) {
  const IrFunction *fun = NULL;
  unsigned index = 0;
  for (size_t i = 0; program && i < program->function_len; i++) {
    if (program->functions[i].is_exported && strcmp(program->functions[i].name, "main") == 0) {
      if (fun) {
        coff_diag_at(diag, "direct COFF x64 executable backend requires exactly one exported main function", program->functions[i].line, program->functions[i].column, program->functions[i].name);
        return NULL;
      }
      fun = &program->functions[i];
      index = (unsigned)i;
    }
  }
  if (!fun) {
    coff_diag_at(diag, "direct COFF x64 executable backend requires an exported main function", 1, 1, "missing main");
    return NULL;
  }
  if (fun->param_count != 0) {
    coff_diag_at(diag, "direct COFF x64 executable main must not take parameters", fun->line, fun->column, fun->name);
    return NULL;
  }
  if (fun->return_type != IR_TYPE_VOID && !coff_type_is_scalar32(fun->return_type)) {
    coff_diag_at(diag, "direct COFF x64 executable main must return Void or a 32-bit-or-smaller scalar", fun->line, fun->column, fun->name);
    return NULL;
  }
  if (out_index) *out_index = index;
  return fun;
}

typedef struct {
  ZCoffImportPatch *items;
  size_t len;
  size_t cap;
} CoffImportPatchList;

static void coff_import_patches_push(CoffImportPatchList *list, ZCoffImportPatch patch) {
  if (list->len == list->cap) {
    list->cap = z_grow_capacity(list->cap, list->len + 1, 8);
    list->items = z_checked_reallocarray(list->items, list->cap, sizeof(ZCoffImportPatch));
  }
  list->items[list->len++] = patch;
}

static void coff_emit_import_call(ZBuf *text, CoffImportPatchList *patches, unsigned import_index) {
  size_t patch = z_x64_emit_call_rip32_placeholder(text);
  if (patches) coff_import_patches_push(patches, (ZCoffImportPatch){.patch_offset = patch, .import_index = import_index});
}

static size_t coff_emit_exe_start_stub(ZBuf *text, CoffImportPatchList *import_patches) {
  z_x64_emit_sub_rsp(text, 40);
  size_t main_patch = z_x64_emit_call32_placeholder(text);
  z_x64_emit_mov_rcx_from_rax(text, false);
  coff_emit_import_call(text, import_patches, Z_COFF_IMPORT_EXIT_PROCESS);
  z_x64_append_u8(text, 0xcc);
  return main_patch;
}

static size_t coff_emit_exe_world_write(ZBuf *text, CoffImportPatchList *import_patches) {
  size_t offset = text->len;
  z_x64_emit_sub_rsp(text, 72);
  z_x64_emit_store_rsp_offset_reg(text, 2, 40, true);
  z_x64_emit_store_rsp_offset_reg(text, 8, 48, true);
  z_x64_emit_mov_rsp_offset_u32(text, 56, 0, false); // DWORD bytes_written = 0
  z_x64_emit_cmp_reg_i8(text, 1, 2, false); // cmp ecx, 2
  z_x64_emit_mov_reg_u32(text, 1, 0xfffffff5u); // STD_OUTPUT_HANDLE
  z_x64_append_u8(text, 0x75);
  z_x64_append_u8(text, 0x05); // jne after stderr handle
  z_x64_emit_mov_reg_u32(text, 1, 0xfffffff4u); // STD_ERROR_HANDLE
  coff_emit_import_call(text, import_patches, Z_COFF_IMPORT_GET_STD_HANDLE);
  z_x64_emit_mov_rcx_from_rax(text, true);
  z_x64_emit_load_rsp_offset_reg(text, 2, 40, true);
  z_x64_emit_load_rsp_offset_reg(text, 8, 48, true);
  z_x64_emit_lea_rsp_offset_reg(text, 9, 56);
  z_x64_emit_mov_rsp_offset_u32(text, 32, 0, true); // lpOverlapped = NULL
  coff_emit_import_call(text, import_patches, Z_COFF_IMPORT_WRITE_FILE);
  z_x64_emit_xor_eax_eax(text);
  z_x64_emit_add_rsp(text, 72);
  z_x64_append_u8(text, 0xc3);
  return offset;
}

bool z_emit_coff_x64_exe_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!program || !out) return coff_diag(diag, "direct COFF executable backend received no program");
  if (!program->mir_valid) {
    bool ok = coff_diag_at(diag, program->mir_message[0] ? program->mir_message : "direct backend lowering failed", program->mir_line, program->mir_column, program->mir_actual);
    z_diag_set_backend_blocker(diag, &program->backend_blocker);
    return ok;
  }
  unsigned main_index = 0;
  if (!coff_find_executable_main(program, diag, &main_index)) return false;
  for (size_t i = 0; i < program->function_len; i++) {
    if (!coff_validate_function(&program->functions[i], diag)) return false;
  }

  ZBuf text;
  ZBuf rdata;
  zbuf_init(&text);
  zbuf_init(&rdata);
  bool has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  unsigned rodata_base_offset = coff_rodata_base_offset(program);
  if (has_rodata) coff_append_rodata(&rdata, program, rodata_base_offset);

  size_t *offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  if (!offsets) {
    zbuf_free(&rdata);
    zbuf_free(&text);
    return coff_diag(diag, "out of memory while emitting COFF executable");
  }

  CoffEmitContext ctx = {
    .program = program,
    .function_offsets = offsets,
    .function_count = program->function_len,
    .rodata_base_offset = rodata_base_offset
  };
  CoffImportPatchList import_patches = {0};
  size_t start_main_patch = coff_emit_exe_start_stub(&text, &import_patches);
  while (text.len % 16 != 0) z_x64_append_u8(&text, 0x90);
  for (size_t i = 0; i < program->function_len; i++) {
    while (text.len % 16 != 0) z_x64_append_u8(&text, 0x90);
    offsets[i] = text.len;
    if (!coff_emit_function_text(&text, &program->functions[i], &ctx, diag)) {
      free(import_patches.items);
      z_coff_emit_context_free(&ctx);
      free(offsets);
      zbuf_free(&rdata);
      zbuf_free(&text);
      return false;
    }
  }
  size_t world_write_offset = 0;
  if (z_coff_runtime_patch_count(&ctx, COFF_RUNTIME_WORLD_WRITE) > 0) {
    while (text.len % 16 != 0) z_x64_append_u8(&text, 0x90);
    world_write_offset = coff_emit_exe_world_write(&text, &import_patches);
  }

  // Math call sites (recorded via z_coff_record_math_patch from coff_emit_value's
  // IR_VALUE_MATH_* lowering) are turned into import patches against the msvcrt.dll IAT entries.
  for (size_t i = 0; i < z_coff_math_patch_count(&ctx); i++) {
    const CoffMathPatch *patch = &ctx.math_patches[i];
    coff_import_patches_push(&import_patches, (ZCoffImportPatch){
      .patch_offset = patch->patch_offset,
      .import_index = z_coff_math_symbol_import_index(patch->symbol),
    });
  }
  // kernel32 mmap/munmap/CreateFileA/.../VirtualAlloc call sites (recorded via
  // z_coff_record_mmap_patch from the mmap helpers) are likewise resolved through the .idata
  // import directory against their KERNEL32.dll IAT slots.
  for (size_t i = 0; i < z_coff_mmap_patch_count(&ctx); i++) {
    const CoffMmapPatch *patch = &ctx.mmap_patches[i];
    coff_import_patches_push(&import_patches, (ZCoffImportPatch){
      .patch_offset = patch->patch_offset,
      .import_index = patch->import_index,
    });
  }

  z_x64_patch_rel32(&text, start_main_patch, offsets[main_index]);
  z_coff_patch_call_patches(&text, &ctx);
  z_coff_patch_runtime_patches(&text, &ctx, COFF_RUNTIME_WORLD_WRITE, world_write_offset);

  ZCoffExecutableImage image = {
    .machine = Z_COFF_MACHINE_AMD64,
    .image_base = 0x140000000ull,
    .section_alignment = 0x1000,
    .file_alignment = 0x200,
    .text = &text,
    .rdata = &rdata,
    .rodata_base_offset = rodata_base_offset,
    .rodata_patches = ctx.rodata_patches,
    .rodata_patch_len = ctx.rodata_patch_len,
    .import_patches = import_patches.items,
    .import_patch_len = import_patches.len,
  };
  z_coff_write_pe64_executable(out, &image);

  free(import_patches.items);
  z_coff_emit_context_free(&ctx);
  free(offsets);
  zbuf_free(&rdata);
  zbuf_free(&text);
  return true;
}
