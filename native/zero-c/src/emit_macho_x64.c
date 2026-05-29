#include "zero.h"
#include "macho_emit_state.h"
#include "macho_format.h"
#include "x64_emit.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool machx64_diag(ZDiag *diag, const char *message) {
  if (diag) {
    diag->code = 4004;
    diag->line = 1;
    diag->column = 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct x86_64 Mach-O subset");
    snprintf(diag->actual, sizeof(diag->actual), "unsupported feature");
    snprintf(diag->help, sizeof(diag->help), "reduce the program to primitive direct-backend constructs or choose a supported direct target");
  }
  return false;
}

static bool machx64_diag_at(ZDiag *diag, const char *message, int line, int column, const char *actual) {
  if (diag) {
    diag->code = 4004;
    diag->line = line > 0 ? line : 1;
    diag->column = column > 0 ? column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct x86_64 Mach-O subset");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "unsupported feature");
    snprintf(diag->help, sizeof(diag->help), "reduce the program to primitive direct-backend constructs or choose a supported direct target");
  }
  return false;
}

static bool machx64_type_is_scalar32(IrTypeKind type) {
  return type == IR_TYPE_BOOL || type == IR_TYPE_U8 || type == IR_TYPE_I8 || type == IR_TYPE_U16 || type == IR_TYPE_I32 || type == IR_TYPE_U32 || type == IR_TYPE_USIZE;
}

static bool machx64_type_is_i64(IrTypeKind type) {
  return type == IR_TYPE_I64 || type == IR_TYPE_U64;
}

static bool machx64_type_is_supported_scalar(IrTypeKind type) {
  return machx64_type_is_scalar32(type) || machx64_type_is_i64(type);
}

static bool machx64_type_is_f64(IrTypeKind type) {
  return type == IR_TYPE_F64;
}

static bool machx64_type_is_float(IrTypeKind type) {
  return type == IR_TYPE_F32 || type == IR_TYPE_F64;
}

// Byte size of a typed-span / array element. 1-byte elements (u8/i8/Bool) need no index scaling;
// 2-byte (u16) by 2; 8-byte elements (i64/u64/f64) scale by 8; everything else (i32/u32/usize/f32)
// by 4. u16 is currently exercised only as a codec read result width.
static unsigned machx64_type_byte_size(IrTypeKind type) {
  if (type == IR_TYPE_U8 || type == IR_TYPE_I8 || type == IR_TYPE_BOOL) return 1;
  if (type == IR_TYPE_U16) return 2;
  if (machx64_type_is_i64(type) || machx64_type_is_f64(type)) return 8;
  return 4;
}

static bool machx64_type_is_unsigned(IrTypeKind type) {
  return type == IR_TYPE_BOOL || type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_U32 || type == IR_TYPE_USIZE || type == IR_TYPE_U64;
}

static bool machx64_is_main_function(const IrFunction *fun) {
  return fun && fun->is_exported && fun->name && fun->name[0] == 'm' && fun->name[1] == 'a' && fun->name[2] == 'i' && fun->name[3] == 'n' && fun->name[4] == '\0';
}

static bool machx64_function_propagates_to_process_exit(const IrFunction *fun) {
  return fun && (fun->raises || (machx64_is_main_function(fun) && fun->return_type == IR_TYPE_I32 && fun->value_return_type == IR_TYPE_VOID));
}

// Main seeds argc/argv/envp from the System V argument registers (rdi/rsi/rdx) into the
// callee-saved trio (r14/r15/r13) so subsequent std.args.{len,get} reads can recover them after
// arbitrary intervening calls. Only the exe-build context flips seed_main_process_args; the obj
// build leaves it false and falls back to reading argc/argv from a musl-style [r15] argv vector.
// Mirrors elf_function_seeds_process_args.
static bool machx64_function_seeds_process_args(const IrFunction *fun, const MachOEmitContext *ctx) {
  return ctx && ctx->seed_main_process_args && fun && fun->is_exported && fun->name && strcmp(fun->name, "main") == 0;
}

// Epilogue wrapper: when main seeded r13/r14/r15, restore the caller's saved values from the
// extra slots just past the regular frame before emitting the bare leave/ret. Every callsite that
// previously called z_x64_emit_epilogue(text) directly now goes through this so the restore stays
// in lockstep with the prologue's save. Mirrors elf_emit_epilogue.
static void machx64_emit_epilogue(ZBuf *code, const IrFunction *fun, const MachOEmitContext *ctx);

static size_t machx64_align(size_t value, size_t alignment) {
  size_t remainder = alignment ? value % alignment : 0;
  return remainder == 0 ? value : value + (alignment - remainder);
}

static void machx64_pad_to(ZBuf *buf, size_t offset) {
  while (buf->len < offset) z_x64_append_u8(buf, 0x90);
}

static void machx64_append_u8(ZBuf *buf, unsigned value) {
  zbuf_append_char(buf, (char)(value & 0xffu));
}

static void machx64_append_bytes(ZBuf *buf, const char *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) machx64_append_u8(buf, (unsigned char)bytes[i]);
}

static unsigned machx64_local_offset(const IrFunction *fun, unsigned local_index) {
  if (fun && local_index < fun->local_len && fun->locals[local_index].frame_offset > 0) return fun->locals[local_index].frame_offset;
  return (local_index + 1) * 8;
}

static unsigned machx64_local_slot_offset(const IrFunction *fun, unsigned local_index, unsigned slot_offset) {
  unsigned offset = machx64_local_offset(fun, local_index);
  return offset >= slot_offset ? offset - slot_offset : offset;
}

// Records use the System V x86-64 sret ABI: a caller-allocated buffer pointer arrives in rdi and the
// callee must save it into a fixed frame slot so a later RETURN can hand it back in rax. The slot
// lives just past the regular locals; the prologue reserves +16 bytes (kept 16-aligned).
static bool machx64_returns_record(const IrFunction *fun) {
  return fun && fun->return_type == IR_TYPE_RECORD;
}

static unsigned machx64_base_stack_size(const IrFunction *fun) {
  return (unsigned)machx64_align(fun ? fun->frame_bytes : 0, 16);
}

static unsigned machx64_sret_slot_offset(const IrFunction *fun) {
  return machx64_base_stack_size(fun) + 8u;
}

// lea reg, [rbp - frame_offset(local)] — used to pass record args by pointer / build sret targets.
static void machx64_emit_lea_local_addr_reg(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned reg) {
  z_x64_emit_rbp_disp_reg(text, 0x8d, reg, machx64_local_offset(fun, local_index), true);
}

static void machx64_emit_lea_local_addr_rax(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  machx64_emit_lea_local_addr_reg(text, fun, local_index, 0);
}

// Copy a record slot from src to dest (or through the saved sret pointer if dest_index == UINT_MAX),
// 8 bytes at a time then a 4-byte tail. rax is scratch; the helper touches rdi, rsi, rax.
static void machx64_emit_record_copy_to(ZBuf *text, const IrFunction *fun, unsigned dest_index, unsigned src_index) {
  unsigned size = src_index < fun->local_len ? fun->locals[src_index].byte_size : 0;
  if (dest_index == UINT_MAX) {
    z_x64_emit_rbp_disp_reg(text, 0x8b, 7, machx64_sret_slot_offset(fun), true); // mov rdi, [rbp - sret]
  } else {
    machx64_emit_lea_local_addr_reg(text, fun, dest_index, 7);
  }
  machx64_emit_lea_local_addr_reg(text, fun, src_index, 6);
  unsigned k = 0;
  while (k + 8 <= size) {
    z_x64_emit_load_reg_ptr_reg_disp(text, 0, 6, k, true);          // mov rax, [rsi + k]
    z_x64_emit_store_ptr_reg_disp_from_reg(text, 7, k, 0, true);    // mov [rdi + k], rax
    k += 8;
  }
  if (k + 4 <= size) {
    z_x64_emit_load_reg_ptr_reg_disp(text, 0, 6, k, false);         // mov eax, [rsi + k]
    z_x64_emit_store_ptr_reg_disp_from_reg(text, 7, k, 0, false);   // mov [rdi + k], eax
  }
}

// Copy a record param (passed by pointer in ptr_reg) into its inline frame slot. machx64_local_offset
// is the rbp-relative magnitude of the local's *start*; field k lives at rbp - (offset - k).
static void machx64_emit_copy_record_param(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned ptr_reg) {
  unsigned size = local_index < fun->local_len ? fun->locals[local_index].byte_size : 0;
  unsigned frame_off = machx64_local_offset(fun, local_index);
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

// Load the 8-byte pointer stashed in slot 0 of a ref<Record>/mutref<Record> local into
// `dst_reg`. The prologue stored the caller-supplied address there (no inline byte copy); every
// downstream field load/store rebases off the returned register at the field's offset. Use BEFORE
// any [dst_reg + disp] ldr/str whose destination value reg might collide with `dst_reg`.
static void machx64_emit_load_ref_record_ptr(ZBuf *text, const IrFunction *fun, unsigned dst_reg, unsigned local_index) {
  z_x64_emit_rbp_disp_reg(text, 0x8b, dst_reg, machx64_local_slot_offset(fun, local_index, 0), true);
}

static void machx64_emit_load_local_eax(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  bool wide = fun && local_index < fun->local_len && machx64_type_is_i64(fun->locals[local_index].type);
  z_x64_emit_rbp_disp_reg(text, 0x8b, 0, machx64_local_offset(fun, local_index), wide);
}

static void machx64_emit_load_local_slot_rax(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned slot_offset) {
  z_x64_emit_rbp_disp_reg(text, 0x8b, 0, machx64_local_slot_offset(fun, local_index, slot_offset), true);
}

static void machx64_emit_load_local_slot_eax(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned slot_offset) {
  z_x64_emit_rbp_disp_reg(text, 0x8b, 0, machx64_local_slot_offset(fun, local_index, slot_offset), false);
}

static void machx64_emit_store_local_from_reg(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned reg) {
  bool wide = fun && local_index < fun->local_len && machx64_type_is_i64(fun->locals[local_index].type);
  z_x64_emit_rbp_disp_reg(text, 0x89, reg, machx64_local_offset(fun, local_index), wide);
}

static void machx64_emit_store_local_slot_from_reg(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned reg, unsigned slot_offset, bool wide) {
  z_x64_emit_rbp_disp_reg(text, 0x89, reg, machx64_local_slot_offset(fun, local_index, slot_offset), wide);
}

// Maybe<MutSpan<u8>> layout: {has@0 u32, ptr@8 i64, len@16 u64}. A failure path clears every slot
// so a downstream `.has` check reads 0. Mirrors elf64's elf_emit_maybe_clear.
static void machx64_emit_maybe_clear(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  z_x64_emit_xor_eax_eax(text);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 16, true);
}

// Maybe<scalar> layout: {has@0 u32, value@8 i64}. Failure path zeros both slots.
static void machx64_emit_maybe_scalar_clear(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  z_x64_emit_xor_eax_eax(text);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);
}

// Success path for a Maybe<scalar>: rax already holds the payload value. Store 1 at has@0, then the
// (saved) payload at value@8. Stack push/pop preserves the payload across the has store.
static void machx64_emit_maybe_scalar_store_rax(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  z_x64_emit_push_rax(text);
  z_x64_emit_mov_eax_u32(text, 1);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false);
  z_x64_emit_pop_rax(text);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);
}

// rax = NUL-terminated string ptr → ecx = strlen(rax). Inline scan: rdx tracks the base
// pointer (preserved across the loop), rcx is the running byte count; the loop bumps rcx until the
// indexed byte is zero. Mirrors elf_emit_strlen_rax_to_ecx; same opcode shape since both use the
// SysV x64 register layout. rax is unchanged on exit.
static void machx64_emit_strlen_rax_to_ecx(ZBuf *text) {
  z_x64_emit_mov_rdx_from_rax(text);
  z_x64_emit_xor_ecx_ecx(text);
  size_t loop = text->len;
  z_x64_emit_cmp_base_index_u8(text, 2, 1, 0);
  size_t done = z_x64_emit_jcc32_placeholder(text, 0x84);
  z_x64_emit_inc_ecx(text);
  size_t back = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, back, loop);
  z_x64_patch_rel32(text, done, text->len);
}

// Forward-declared at the top of the file. The args-seeded main reserved 32 extra bytes
// past base_stack_size for the saved r13/r14/r15 trio; reload them in reverse store order before
// the bare leave/ret. Non-seeded callers (any function but main, or any obj-build emission) reuse
// the unwrapped epilogue.
static void machx64_emit_epilogue(ZBuf *code, const IrFunction *fun, const MachOEmitContext *ctx) {
  if (machx64_function_seeds_process_args(fun, ctx)) {
    unsigned base = machx64_base_stack_size(fun);
    z_x64_emit_rbp_disp_reg(code, 0x8b, 13, base + 8, true);
    z_x64_emit_rbp_disp_reg(code, 0x8b, 14, base + 16, true);
    z_x64_emit_rbp_disp_reg(code, 0x8b, 15, base + 24, true);
  }
  z_x64_emit_epilogue(code);
}

static void machx64_emit_load_field_eax(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned field_offset, IrTypeKind type) {
  // ref-record field load — deref the stashed ptr into r11 first, then mov from
  // [r11 + field_offset] into eax/rax. r11 is caller-saved and avoided by surrounding code
  // (the local byte_view_eq stash uses r10 / r8 — keeping r11 distinct avoids clobber).
  if (local_index < fun->local_len && fun->locals[local_index].is_ref) {
    machx64_emit_load_ref_record_ptr(text, fun, 11, local_index);
    if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
      z_x64_emit_movzx_reg32_ptr_reg_disp_u8(text, 0, 11, field_offset);
    } else {
      bool wide = machx64_type_is_i64(type);
      z_x64_emit_load_reg_ptr_reg_disp(text, 0, 11, field_offset, wide);
    }
    return;
  }
  unsigned offset = machx64_local_slot_offset(fun, local_index, field_offset);
  if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
    z_x64_append_u8(text, 0x0f);
    z_x64_emit_rbp_disp_reg(text, 0xb6, 0, offset, false);
  } else if (machx64_type_is_i64(type)) {
    z_x64_emit_rbp_disp_reg(text, 0x8b, 0, offset, true);
  } else {
    z_x64_emit_rbp_disp_reg(text, 0x8b, 0, offset, false);
  }
}

static void machx64_emit_store_field_from_eax(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned field_offset, IrTypeKind type) {
  // mutref-record field store — deref ptr into r11 (value being stored is in rax/al),
  // then mov [r11 + field_offset], (e|r|al)ax. Mutability is enforced by the FIELD_STORE handler.
  if (local_index < fun->local_len && fun->locals[local_index].is_ref) {
    machx64_emit_load_ref_record_ptr(text, fun, 11, local_index);
    if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
      z_x64_emit_store_ptr_reg_disp_from_reg8(text, 11, field_offset, 0);
    } else {
      bool wide = machx64_type_is_i64(type);
      z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, field_offset, 0, wide);
    }
    return;
  }
  unsigned offset = machx64_local_slot_offset(fun, local_index, field_offset);
  if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
    z_x64_emit_rbp_disp_reg(text, 0x88, 0, offset, false);
  } else if (machx64_type_is_i64(type)) {
    z_x64_emit_rbp_disp_reg(text, 0x89, 0, offset, true);
  } else {
    z_x64_emit_rbp_disp_reg(text, 0x89, 0, offset, false);
  }
}

// Float scalar load/store on xmm0. machx64_local_offset gives the rbp-relative
// downward magnitude, but z_x64_emit_movs_xmm_rbp_disp wants a raw signed disp, so negate.
static void machx64_emit_load_local_xmm0(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  bool is64 = fun && local_index < fun->local_len && machx64_type_is_f64(fun->locals[local_index].type);
  z_x64_emit_movs_xmm_rbp_disp(text, 0, -(int32_t)machx64_local_offset(fun, local_index), is64, true);
}

static void machx64_emit_store_local_xmm0(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  bool is64 = fun && local_index < fun->local_len && machx64_type_is_f64(fun->locals[local_index].type);
  z_x64_emit_movs_xmm_rbp_disp(text, 0, -(int32_t)machx64_local_offset(fun, local_index), is64, false);
}

static void machx64_emit_load_field_xmm0(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned field_offset, IrTypeKind type) {
  // ref-record FP field load — deref the stashed ptr into r11, advance by field_offset
  // (movs_xmm_ptr_reg has no disp form), then movss/movsd xmm0, [r11].
  if (local_index < fun->local_len && fun->locals[local_index].is_ref) {
    machx64_emit_load_ref_record_ptr(text, fun, 11, local_index);
    if (field_offset > 0) z_x64_emit_add_reg_u32(text, 11, field_offset, true);
    z_x64_emit_movs_xmm_ptr_reg(text, 0, 11, machx64_type_is_f64(type), true);
    return;
  }
  z_x64_emit_movs_xmm_rbp_disp(text, 0, -(int32_t)machx64_local_slot_offset(fun, local_index, field_offset), machx64_type_is_f64(type), true);
}

static void machx64_emit_store_field_xmm0(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned field_offset, IrTypeKind type) {
  // mutref-record FP field store — deref ptr into r11 + offset, then movss/movsd
  // [r11], xmm0. Mutability is enforced by the FIELD_STORE handler before this is called.
  if (local_index < fun->local_len && fun->locals[local_index].is_ref) {
    machx64_emit_load_ref_record_ptr(text, fun, 11, local_index);
    if (field_offset > 0) z_x64_emit_add_reg_u32(text, 11, field_offset, true);
    z_x64_emit_movs_xmm_ptr_reg(text, 0, 11, machx64_type_is_f64(type), false);
    return;
  }
  z_x64_emit_movs_xmm_rbp_disp(text, 0, -(int32_t)machx64_local_slot_offset(fun, local_index, field_offset), machx64_type_is_f64(type), false);
}

static void machx64_emit_array_base_rdx(ZBuf *text, const IrFunction *fun, unsigned local_index) {
  z_x64_emit_rbp_disp_reg(text, 0x8d, 2, machx64_local_offset(fun, local_index), true);
}

static void machx64_emit_bounds_check(ZBuf *text, const IrLocal *local) {
  z_x64_append_u8(text, 0x3d);
  z_x64_append_u32(text, local ? local->array_len : 0);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x82);
  z_x64_emit_ud2(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
}

static unsigned machx64_setcc_opcode(IrCompareOp op, bool uns) {
  switch (op) {
    case IR_CMP_EQ: return 0x94;
    case IR_CMP_NE: return 0x95;
    case IR_CMP_LT: return uns ? 0x92 : 0x9c;
    case IR_CMP_LE: return uns ? 0x96 : 0x9e;
    case IR_CMP_GT: return uns ? 0x97 : 0x9f;
    case IR_CMP_GE: return uns ? 0x93 : 0x9d;
  }
  return 0x94;
}

// IR_CMP_* -> primary unsigned SETcc after UCOMISS/UCOMISD. UCOMIS sets EFLAGS
// like an unsigned compare; unordered (NaN) gives ZF=PF=CF=1.
static unsigned machx64_xmm_setcc_opcode(IrCompareOp op) {
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
static bool machx64_xmm_compare_needs_parity_fixup(IrCompareOp op) {
  return op == IR_CMP_EQ || op == IR_CMP_NE || op == IR_CMP_LT || op == IR_CMP_LE;
}

static void machx64_emit_xmm_compare_to_bool(ZBuf *text, IrCompareOp op, bool is64) {
  z_x64_emit_ucomis(text, 0, 1, is64);
  if (machx64_xmm_compare_needs_parity_fixup(op)) {
    bool is_ne = op == IR_CMP_NE;
    z_x64_emit_setcc_al(text, machx64_xmm_setcc_opcode(op));
    z_x64_emit_setcc_cl(text, is_ne ? 0x9a : 0x9b); // SETP for !=, SETNP otherwise
    if (is_ne) z_x64_emit_or_al_cl(text);
    else z_x64_emit_and_al_cl(text);
    z_x64_emit_movzx_eax_al(text);
  } else {
    z_x64_emit_setcc_al_to_bool(text, machx64_xmm_setcc_opcode(op));
  }
}

static bool machx64_const_u32_value(const IrValue *value, unsigned *out) {
  if (!value || value->kind != IR_VALUE_INT || value->int_value > UINT32_MAX) return false;
  if (out) *out = (unsigned)value->int_value;
  return true;
}

static bool machx64_readonly_data_byte(const IrProgram *program, unsigned offset, unsigned char *out) {
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

static void machx64_emit_error_condition_from_rax(ZBuf *text) {
  z_x64_emit_mov_rcx_from_rax(text, true);
  z_x64_emit_shr_rcx_imm8(text, 32);
  z_x64_emit_test_ecx_ecx(text);
}

static void machx64_emit_packed_error_rax(ZBuf *text, unsigned code_value) {
  z_x64_emit_mov_rax_u64(text, ((uint64_t)code_value) << 32);
}

static bool machx64_byte_view_const_len(const IrValue *view, unsigned *out) {
  if (!view) return false;
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (out) *out = view->data_len;
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned base_len = 0;
    if (!machx64_byte_view_const_len(view->left, &base_len)) return false;
    unsigned start = 0;
    unsigned end = base_len;
    if (view->index && !machx64_const_u32_value(view->index, &start)) return false;
    if (view->right && !machx64_const_u32_value(view->right, &end)) return false;
    if (start > end || end > base_len) return false;
    if (out) *out = end - start;
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    unsigned base_len = 0;
    if (!machx64_byte_view_const_len(view->left, &base_len)) return false;
    // Reinterpreted element count = underlying byte length / sizeof(target element).
    if (out) *out = base_len / machx64_type_byte_size(view->element_type);
    return true;
  }
  return false;
}

static bool machx64_byte_view_const_byte(const IrProgram *program, const IrValue *view, unsigned index, unsigned char *out) {
  if (!view) return false;
  if (view->kind == IR_VALUE_STRING_LITERAL) {
    if (index >= view->data_len) return false;
    return machx64_readonly_data_byte(program, view->data_offset + index, out);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned len = 0;
    unsigned start = 0;
    if (!machx64_byte_view_const_len(view, &len) || index >= len) return false;
    if (view->index && !machx64_const_u32_value(view->index, &start)) return false;
    return machx64_byte_view_const_byte(program, view->left, start + index, out);
  }
  return false;
}

static size_t machx64_emit_lea_rax_rip_placeholder(ZBuf *text) {
  z_x64_append_u8(text, 0x48);
  z_x64_append_u8(text, 0x8d);
  z_x64_append_u8(text, 0x05);
  size_t patch = text->len;
  z_x64_append_u32(text, 0);
  return patch;
}

static bool machx64_emit_rodata_ptr_rax(ZBuf *text, unsigned data_offset, MachOEmitContext *ctx, const IrValue *value, ZDiag *diag) {
  size_t patch = machx64_emit_lea_rax_rip_placeholder(text);
  return z_macho_record_data_patch(ctx, patch, data_offset, value, diag);
}

static bool machx64_emit_byte_view_ptr(ZBuf *text, const IrFunction *fun, const IrValue *view, MachOEmitContext *ctx, ZDiag *diag);
static bool machx64_emit_byte_view_len(ZBuf *text, const IrFunction *fun, const IrValue *view, MachOEmitContext *ctx, ZDiag *diag);
static bool machx64_emit_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag);

static bool machx64_emit_byte_view_len(ZBuf *text, const IrFunction *fun, const IrValue *view, MachOEmitContext *ctx, ZDiag *diag) {
  unsigned len = 0;
  if (machx64_byte_view_const_len(view, &len)) {
    z_x64_emit_mov_eax_u32(text, len);
    return true;
  }
  if (view && view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    // Reinterpreted element count = underlying byte length >> log2(sizeof(T)). A 1-byte element (i8)
    // needs no shift: the byte length already is the element count.
    if (!machx64_emit_byte_view_len(text, fun, view->left, ctx, diag)) return false;
    unsigned size = machx64_type_byte_size(view->element_type);
    if (size > 1) z_x64_emit_shr_reg_imm8(text, 0, size == 8 ? 3 : 2, true);
    return true;
  }
  if (view && view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    machx64_emit_load_local_slot_rax(text, fun, view->local_index, 8);
    return true;
  }
  if (view && view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field length lives 8 bytes past the field's ptr, as a 64-bit byte/element count.
    // ref-record — deref stashed ptr into r11, then load len at [r11 + field_offset + 8].
    if (fun->locals[view->local_index].is_ref) {
      machx64_emit_load_ref_record_ptr(text, fun, 11, view->local_index);
      z_x64_emit_load_reg_ptr_reg_disp(text, 0, 11, view->field_offset + 8u, true);
      return true;
    }
    machx64_emit_load_local_slot_rax(text, fun, view->local_index, view->field_offset + 8);
    return true;
  }
  if (view && view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    // first.value as a span: Maybe<MutSpan<u8>> stores len at slot +16 (64-bit byte count).
    machx64_emit_load_local_slot_rax(text, fun, view->local_index, 16);
    return true;
  }
  if (view && view->kind == IR_VALUE_BYTE_SLICE && view->right) {
    unsigned start = 0;
    unsigned end = 0;
    bool const_start = !view->index || machx64_const_u32_value(view->index, &start);
    if (const_start && machx64_const_u32_value(view->right, &end) && start <= end) {
      z_x64_emit_mov_eax_u32(text, end - start);
      return true;
    }
    if (const_start) {
      // const start (possibly 0) + dynamic end: eval end into rax, subtract const start.
      if (!machx64_emit_value(text, fun, view->right, ctx, diag)) return false;
      if (start > 0) z_x64_emit_sub_rax_u32(text, start, true);
      return true;
    }
    if (view->index) {
      // dynamic start + dynamic end: eval start, push, eval end, pop start into rcx, end - start.
      if (!machx64_emit_value(text, fun, view->index, ctx, diag)) return false;
      z_x64_emit_push_rax(text);
      if (!machx64_emit_value(text, fun, view->right, ctx, diag)) return false;
      z_x64_emit_pop_reg64(text, 1);
      z_x64_emit_sub_reg_reg(text, 0, 1, true);
      return true;
    }
  }
  (void)ctx;
  return machx64_diag_at(diag, "direct x86_64 Mach-O byte-view length currently requires a literal, constant slice, or byte-view local", view ? view->line : 1, view ? view->column : 1, "unsupported byte view length");
}

static bool machx64_emit_byte_view_ptr(ZBuf *text, const IrFunction *fun, const IrValue *view, MachOEmitContext *ctx, ZDiag *diag) {
  if (!view) return machx64_diag_at(diag, "direct x86_64 Mach-O byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    machx64_emit_load_local_slot_rax(text, fun, view->local_index, 0);
    return true;
  }
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field: the 8-byte pointer lives at the field offset within the record local.
    // ref-record — deref stashed ptr into r11, then load span ptr at [r11 + field_offset].
    if (fun->locals[view->local_index].is_ref) {
      machx64_emit_load_ref_record_ptr(text, fun, 11, view->local_index);
      z_x64_emit_load_reg_ptr_reg_disp(text, 0, 11, view->field_offset, true);
      return true;
    }
    machx64_emit_load_local_slot_rax(text, fun, view->local_index, view->field_offset);
    return true;
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    // first.value as a span: Maybe<MutSpan<u8>> stores ptr at slot +8.
    machx64_emit_load_local_slot_rax(text, fun, view->local_index, 8);
    return true;
  }
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    // Any value-typed array binds as a typed span — ptr = LEA of &arr[0]. Element scaling
    // for INDEX_LOAD/STORE / BYTE_SLICE rides on the carrying IR_VALUE's element_type.
    if (!local->is_array) return machx64_diag_at(diag, "direct x86_64 Mach-O byte-view array source must be a fixed array local", view->line, view->column, "unsupported array view");
    z_x64_emit_rbp_disp_reg(text, 0x8d, 0, machx64_local_offset(fun, view->array_index), true);
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) return machx64_emit_rodata_ptr_rax(text, view->data_offset, ctx, view, diag);
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    // A reinterpret keeps the same base pointer (zero-copy); only the element count changes.
    return machx64_emit_byte_view_ptr(text, fun, view->left, ctx, diag);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    unsigned elem_size = machx64_type_byte_size(view->element_type);
    if (!view->index || machx64_const_u32_value(view->index, &start)) {
      if (!machx64_emit_byte_view_ptr(text, fun, view->left, ctx, diag)) return false;
      // Slice bounds are element indices, so the byte offset is start * sizeof(T). For u8 this is start.
      unsigned byte_start = start * elem_size;
      if (byte_start > 0) z_x64_emit_add_rax_u32(text, byte_start, true);
      return true;
    }
    // dynamic start: compute base ptr → rax, push, eval index → rax, scale by sizeof(T), pop base into rcx, add.
    if (!machx64_emit_byte_view_ptr(text, fun, view->left, ctx, diag)) return false;
    z_x64_emit_push_rax(text);
    if (!machx64_emit_value(text, fun, view->index, ctx, diag)) return false;
    if (elem_size == 8 || elem_size == 4) z_x64_emit_shl_reg_imm8(text, 0, elem_size == 8 ? 3 : 2, true);
    z_x64_emit_pop_reg64(text, 1);
    z_x64_emit_add_reg_reg(text, 0, 1, true);
    return true;
  }
  return machx64_diag_at(diag, "direct x86_64 Mach-O value is not a supported byte view", view->line, view->column, "unsupported byte view");
}

static void machx64_emit_cast_normalize_rax(ZBuf *text, IrTypeKind source, IrTypeKind target) {
  switch (target) {
    case IR_TYPE_BOOL:
    case IR_TYPE_U8:
      z_x64_emit_and_reg_u32(text, 0, 0xff, false);
      return;
    case IR_TYPE_I8:
      // Sign-extend the low byte so a signed-byte value carries the correct sign in arithmetic.
      z_x64_emit_movsx_eax_al(text);
      return;
    case IR_TYPE_U16:
      z_x64_emit_and_reg_u32(text, 0, 0xffff, false);
      return;
    case IR_TYPE_I32:
    case IR_TYPE_U32:
    case IR_TYPE_USIZE:
      z_x64_emit_mov_reg_from_reg(text, 0, 0, false);
      return;
    case IR_TYPE_I64:
      if (source == IR_TYPE_I32) z_x64_emit_cdqe(text);
      return;
    case IR_TYPE_U64:
      return;
    default:
      return;
  }
}

static bool machx64_emit_local_value(ZBuf *text, const IrFunction *fun, const IrValue *value, ZDiag *diag) {
  if (value->local_index >= fun->local_len) return machx64_diag_at(diag, "direct x86_64 Mach-O local index is out of range", value->line, value->column, "invalid local");
  if (fun->locals[value->local_index].type == IR_TYPE_BYTE_VIEW) return machx64_diag_at(diag, "direct x86_64 Mach-O byte-view local cannot be used as a scalar", value->line, value->column, "byte-view local");
  if (machx64_type_is_float(fun->locals[value->local_index].type)) machx64_emit_load_local_xmm0(text, fun, value->local_index);
  else machx64_emit_load_local_eax(text, fun, value->local_index);
  return true;
}

static bool machx64_emit_binary_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (machx64_type_is_float(value->type)) {
    bool is64 = machx64_type_is_f64(value->type);
    if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
    z_x64_emit_xmm_push(text, 0);
    if (!machx64_emit_value(text, fun, value->right, ctx, diag)) return false;
    z_x64_emit_movaps(text, 1, 0);
    z_x64_emit_xmm_pop(text, 0);
    if (value->binary_op == IR_BIN_ADD) z_x64_emit_sse_add(text, 0, 1, is64);
    else if (value->binary_op == IR_BIN_SUB) z_x64_emit_sse_sub(text, 0, 1, is64);
    else if (value->binary_op == IR_BIN_MUL) z_x64_emit_sse_mul(text, 0, 1, is64);
    else if (value->binary_op == IR_BIN_DIV) z_x64_emit_sse_div(text, 0, 1, is64);
    else return machx64_diag_at(diag, "direct x86_64 Mach-O float binary operator is unsupported", value->line, value->column, "unsupported operator");
    return true;
  }
  if (value->binary_op != IR_BIN_ADD && value->binary_op != IR_BIN_SUB && value->binary_op != IR_BIN_MUL &&
      value->binary_op != IR_BIN_DIV && value->binary_op != IR_BIN_MOD && value->binary_op != IR_BIN_AND &&
      value->binary_op != IR_BIN_OR) {
    return machx64_diag_at(diag, "direct x86_64 Mach-O binary operator is unsupported", value->line, value->column, "unsupported operator");
  }
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_value(text, fun, value->right, ctx, diag)) return false;
  bool wide = machx64_type_is_i64(value->type);
  z_x64_emit_mov_rcx_from_rax(text, wide);
  z_x64_emit_pop_rax(text);
  if (value->binary_op == IR_BIN_ADD) z_x64_emit_add_rax_rcx(text, wide);
  else if (value->binary_op == IR_BIN_SUB) z_x64_emit_sub_rax_rcx(text, wide);
  else if (value->binary_op == IR_BIN_MUL) z_x64_emit_imul_rax_rcx(text, wide);
  else if (value->binary_op == IR_BIN_AND) z_x64_emit_and_rax_rcx(text, wide);
  else if (value->binary_op == IR_BIN_OR) z_x64_emit_or_rax_rcx(text, wide);
  else z_x64_emit_div_rax_rcx(text, wide, machx64_type_is_unsigned(value->type), value->binary_op == IR_BIN_MOD);
  return true;
}

static bool machx64_emit_compare_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return machx64_diag_at(diag, "direct x86_64 Mach-O comparison requires two operands", value->line, value->column, "invalid comparison");
  if (value->left->type != value->right->type) return machx64_diag_at(diag, "direct x86_64 Mach-O comparison operands must have the same type", value->line, value->column, "mismatched comparison");
  if (machx64_type_is_float(value->left->type)) {
    bool is64 = machx64_type_is_f64(value->left->type);
    if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
    z_x64_emit_xmm_push(text, 0);
    if (!machx64_emit_value(text, fun, value->right, ctx, diag)) return false;
    z_x64_emit_movaps(text, 1, 0);
    z_x64_emit_xmm_pop(text, 0);
    machx64_emit_xmm_compare_to_bool(text, value->compare_op, is64);
    return true;
  }
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_value(text, fun, value->right, ctx, diag)) return false;
  bool wide = machx64_type_is_i64(value->left ? value->left->type : value->type);
  z_x64_emit_mov_rcx_from_rax(text, wide);
  z_x64_emit_pop_rax(text);
  z_x64_emit_cmp_rax_rcx_to_bool(text, machx64_setcc_opcode(value->compare_op, machx64_type_is_unsigned(value->left ? value->left->type : value->type)), wide);
  return true;
}

static bool machx64_emit_check_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || (value->left->type != IR_TYPE_I64 && !machx64_type_is_float(value->left->type))) {
    return machx64_diag_at(diag, "direct x86_64 Mach-O check requires a fallible call result", value->line, value->column, "non-fallible value");
  }
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  machx64_emit_error_condition_from_rax(text);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  if (!machx64_function_propagates_to_process_exit(fun)) z_x64_emit_mov_eax_u32(text, 1);
  machx64_emit_epilogue(text, fun, ctx);
  z_x64_patch_rel32(text, ok_patch, text->len);
  // OK path: an int payload shares rax's low 32 bits with the tag, so drop the tag with a
  // 32-bit self-move. A float payload already sits untouched in xmm0 — leave rax alone.
  if (!machx64_type_is_i64(value->type) && !machx64_type_is_float(value->type)) z_x64_emit_mov_reg_from_reg(text, 0, 0, false);
  return true;
}

static bool machx64_emit_rescue_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right || (value->left->type != IR_TYPE_I64 && !machx64_type_is_float(value->left->type))) {
    return machx64_diag_at(diag, "direct x86_64 Mach-O rescue requires a fallible call and fallback", value->line, value->column, "unsupported rescue");
  }
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  machx64_emit_error_condition_from_rax(text);
  size_t success_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  if (!machx64_emit_value(text, fun, value->right, ctx, diag)) return false;
  size_t end_patch = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, success_patch, text->len);
  // Success: drop the tag from an int payload's rax; a float payload is already in xmm0.
  if (!machx64_type_is_i64(value->type) && !machx64_type_is_float(value->type)) z_x64_emit_mov_reg_from_reg(text, 0, 0, false);
  z_x64_patch_rel32(text, end_patch, text->len);
  return true;
}

// Forward declaration: call a record-returning function and direct its sret pointer to a local
// (dest_local >= 0) or to the caller's own sret pointer slot (dest_local < 0).
static bool machx64_emit_record_call_with_dest(ZBuf *text, const IrFunction *fun, int dest_local, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag);

static bool machx64_emit_call_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  static const unsigned param_regs[] = {7, 6, 2, 1, 8, 9};
  // System V: integer/pointer args fill param_regs, float args fill xmm0..7, counted
  // independently. Push all args left-to-right, then pop right-to-left into each bank.
  size_t int_slots = 0, float_slots = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    if (arg && machx64_type_is_float(arg->type)) float_slots++;
    else if (arg && arg->type == IR_TYPE_BYTE_VIEW) int_slots += 2u;
    else int_slots++;
  }
  if (int_slots > 6) return machx64_diag_at(diag, "direct x86_64 Mach-O call supports at most six integer ABI argument slots", value->line, value->column, "too many arguments");
  if (float_slots > 8) return machx64_diag_at(diag, "direct x86_64 Mach-O call supports at most eight float arguments", value->line, value->column, "too many float arguments");
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    if (arg && arg->type == IR_TYPE_BYTE_VIEW) {
      if (!machx64_emit_byte_view_ptr(text, fun, arg, ctx, diag)) return false;
      z_x64_emit_push_rax(text);
      if (!machx64_emit_byte_view_len(text, fun, arg, ctx, diag)) return false;
      z_x64_emit_push_rax(text);
    } else if (arg && arg->type == IR_TYPE_RECORD) {
      // Record arg: pass by pointer (caller spill stays valid for the call).
      if (arg->kind != IR_VALUE_LOCAL) return machx64_diag_at(diag, "direct x86_64 Mach-O record argument must be a local", value->line, value->column, "non-local record arg");
      machx64_emit_lea_local_addr_rax(text, fun, arg->local_index);
      z_x64_emit_push_rax(text);
    } else if (arg && machx64_type_is_float(arg->type)) {
      if (!machx64_emit_value(text, fun, arg, ctx, diag)) return false;
      z_x64_emit_xmm_push(text, 0);
    } else {
      if (!machx64_emit_value(text, fun, arg, ctx, diag)) return false;
      z_x64_emit_push_rax(text);
    }
  }
  size_t int_remaining = int_slots, float_remaining = float_slots;
  for (size_t i = value->arg_len; i > 0; i--) {
    const IrValue *arg = value->args[i - 1];
    if (arg && arg->type == IR_TYPE_BYTE_VIEW) {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    } else if (arg && arg->type == IR_TYPE_RECORD) {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    } else if (arg && machx64_type_is_float(arg->type)) {
      z_x64_emit_xmm_pop(text, (unsigned)(--float_remaining));
    } else {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    }
  }
  size_t patch = z_x64_emit_call32_placeholder(text);
  return z_macho_record_call_patch(ctx, patch, value->callee_index, value, diag);
}

// Call a record-returning function under the System V sret ABI. The caller-allocated buffer pointer
// must arrive in rdi (param0), so int_count starts at 1 and rdi is set LAST — after the rest of the
// arg-marshaling pushes/pops are done — pointing at dest_local (own local) or the caller's saved
// sret slot (dest_local < 0). The callee returns the sret pointer back in rax.
static bool machx64_emit_record_call_with_dest(ZBuf *text, const IrFunction *fun, int dest_local, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  static const unsigned param_regs[] = {7, 6, 2, 1, 8, 9};
  const IrFunction *callee = ctx && ctx->program && value->callee_index < ctx->program->function_len ? &ctx->program->functions[value->callee_index] : NULL;
  if (!callee) return machx64_diag_at(diag, "direct x86_64 Mach-O call target is unavailable", value->line, value->column, "invalid callee");
  size_t int_count = 1, float_count = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    IrTypeKind ptype = i < callee->param_count ? callee->locals[i].type : value->args[i]->type;
    if (machx64_type_is_float(ptype)) float_count++;
    else if (ptype == IR_TYPE_BYTE_VIEW) int_count += 2u;
    else int_count++;
  }
  if (int_count > 6) return machx64_diag_at(diag, "direct x86_64 Mach-O record-returning call supports at most five integer arguments", value->line, value->column, "too many integer arguments");
  if (float_count > 8) return machx64_diag_at(diag, "direct x86_64 Mach-O call supports at most eight float arguments", value->line, value->column, "too many float arguments");
  for (size_t i = 0; i < value->arg_len; i++) {
    IrTypeKind ptype = i < callee->param_count ? callee->locals[i].type : value->args[i]->type;
    if (ptype == IR_TYPE_BYTE_VIEW) {
      if (!machx64_emit_byte_view_ptr(text, fun, value->args[i], ctx, diag)) return false;
      z_x64_emit_push_rax(text);
      if (!machx64_emit_byte_view_len(text, fun, value->args[i], ctx, diag)) return false;
      z_x64_emit_push_rax(text);
    } else if (ptype == IR_TYPE_RECORD) {
      if (value->args[i]->kind != IR_VALUE_LOCAL) return machx64_diag_at(diag, "direct x86_64 Mach-O record argument must be a local", value->line, value->column, "non-local record arg");
      machx64_emit_lea_local_addr_rax(text, fun, value->args[i]->local_index);
      z_x64_emit_push_rax(text);
    } else if (machx64_type_is_float(ptype)) {
      if (!machx64_emit_value(text, fun, value->args[i], ctx, diag)) return false;
      z_x64_emit_xmm_push(text, 0);
    } else {
      if (!machx64_emit_value(text, fun, value->args[i], ctx, diag)) return false;
      z_x64_emit_push_rax(text);
    }
  }
  size_t int_remaining = int_count, float_remaining = float_count;
  for (size_t i = value->arg_len; i > 0; i--) {
    IrTypeKind ptype = (i - 1) < callee->param_count ? callee->locals[i - 1].type : value->args[i - 1]->type;
    if (ptype == IR_TYPE_BYTE_VIEW) {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    } else if (ptype == IR_TYPE_RECORD) {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    } else if (machx64_type_is_float(ptype)) {
      z_x64_emit_xmm_pop(text, (unsigned)(--float_remaining));
    } else {
      z_x64_emit_pop_reg64(text, param_regs[--int_remaining]);
    }
  }
  // rdi (param0) = destination sret pointer — set LAST so the pop sequence above doesn't clobber it.
  if (dest_local >= 0) {
    machx64_emit_lea_local_addr_reg(text, fun, (unsigned)dest_local, 7);
  } else {
    z_x64_emit_rbp_disp_reg(text, 0x8b, 7, machx64_sret_slot_offset(fun), true);
  }
  size_t patch = z_x64_emit_call32_placeholder(text);
  return z_macho_record_call_patch(ctx, patch, value->callee_index, value, diag);
}

// `let q = check f()` where f returns a record and raises. Issue the record-returning call
// (writes the record via sret into q's slot AND writes the tag to rdx: 0 on success, error code on
// failure). Then test edx: on zero fall through; on non-zero propagate. The record buffer is
// undefined on failure — the caller's CHECK semantics ensure it isn't read. Mirrors
// machx64_emit_check_value combined with machx64_emit_record_call_with_dest.
static bool machx64_emit_local_set_record_check(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  const IrValue *check = instr->value;
  if (!check->left || check->left->kind != IR_VALUE_CALL || check->left->type != IR_TYPE_RECORD) {
    return machx64_diag_at(diag, "direct x86_64 Mach-O record check requires a record-returning fallible call", check->line, check->column, "unsupported record check");
  }
  if (!machx64_emit_record_call_with_dest(text, fun, (int)fun->locals[instr->local_index].index, check->left, ctx, diag)) return false;
  // Test the tag in rdx (low 32 bits carry the error code). On zero fall through.
  z_x64_emit_test_reg_reg(text, 2, false); // test edx, edx
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x84); // JZ ok
  if (machx64_function_propagates_to_process_exit(fun)) {
    // Propagation by caller-return shape:
    //  - We return a record AND raise: rdx already carries the tag; reload rax from the sret slot
    //    so the caller sees its valid pointer alongside non-zero rdx.
    //  - We are hosted-main (Void return mapped to I32 exit code): main has no sret; route the
    //    tag from rdx into rax (low 32) so the OS reads the error code as the exit code.
    //  - We are a non-record raising fn: the packed-tag ABI puts the tag in rax's HIGH 32 bits.
    //    Compose rax = (rdx << 32): shl rdx, 32; mov rax, rdx.
    if (fun->return_type == IR_TYPE_RECORD) {
      z_x64_emit_rbp_disp_reg(text, 0x8b, 0, machx64_sret_slot_offset(fun), true); // mov rax, [rbp - sret]
    } else if (machx64_is_main_function(fun)) {
      z_x64_emit_mov_reg_from_reg(text, 0, 2, false); // mov eax, edx (low 32 → exit code)
    } else {
      z_x64_emit_shl_reg_imm8(text, 2, 32, true);     // shl rdx, 32
      z_x64_emit_mov_reg_from_reg(text, 0, 2, true);  // mov rax, rdx (packed-tag in high 32)
    }
  } else {
    // Non-propagating context shouldn't reach here (CHECK is illegal in a non-fallible context).
    z_x64_emit_mov_eax_u32(text, 1);
  }
  machx64_emit_epilogue(text, fun, ctx);
  z_x64_patch_rel32(text, ok_patch, text->len);
  return true;
}

// `let q = f() rescue r0` where f returns a record and raises. Issue the call with q as
// sret target; on success (rdx == 0) the record is already in q. On failure, materialize the
// fallback record into q — either by record_copy_to (fallback is a record local) or another
// record-returning call (fallback is itself a call). Mirrors machx64_emit_rescue_value combined
// with machx64_emit_record_call_with_dest.
static bool machx64_emit_local_set_record_rescue(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  const IrValue *rescue = instr->value;
  const IrLocal *dest = &fun->locals[instr->local_index];
  if (!rescue->left || rescue->left->kind != IR_VALUE_CALL || rescue->left->type != IR_TYPE_RECORD) {
    return machx64_diag_at(diag, "direct x86_64 Mach-O record rescue requires a record-returning fallible call", rescue->line, rescue->column, "unsupported record rescue");
  }
  if (!rescue->right || (rescue->right->kind != IR_VALUE_LOCAL && rescue->right->kind != IR_VALUE_CALL)) {
    return machx64_diag_at(diag, "direct x86_64 Mach-O record rescue fallback must be a record local or call", rescue->line, rescue->column, "unsupported rescue fallback");
  }
  if (!machx64_emit_record_call_with_dest(text, fun, (int)dest->index, rescue->left, ctx, diag)) return false;
  z_x64_emit_test_reg_reg(text, 2, false); // test edx, edx
  size_t fallback_patch = z_x64_emit_jcc32_placeholder(text, 0x85); // JNZ fallback
  size_t end_patch = z_x64_emit_jmp32_placeholder(text, 0xe9); // success: skip fallback
  z_x64_patch_rel32(text, fallback_patch, text->len);
  if (rescue->right->kind == IR_VALUE_LOCAL) {
    if (rescue->right->local_index >= fun->local_len) {
      return machx64_diag_at(diag, "direct x86_64 Mach-O record rescue fallback local is out of range", rescue->right->line, rescue->right->column, "invalid fallback local");
    }
    machx64_emit_record_copy_to(text, fun, dest->index, rescue->right->local_index);
  } else {
    if (!machx64_emit_record_call_with_dest(text, fun, (int)dest->index, rescue->right, ctx, diag)) return false;
  }
  z_x64_patch_rel32(text, end_patch, text->len);
  return true;
}

static bool machx64_emit_byte_view_index_load_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  unsigned const_index = 0;
  unsigned char byte = 0;
  if (machx64_const_u32_value(value->index, &const_index) && machx64_byte_view_const_byte(ctx ? ctx->program : NULL, value->left, const_index, &byte)) {
    z_x64_emit_mov_eax_u32(text, byte);
    return true;
  }
  if (!value->index || !machx64_emit_value(text, fun, value->index, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_byte_view_len(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_mov_rcx_from_rax(text, true);
  z_x64_emit_pop_rax(text);
  z_x64_emit_cmp_rax_rcx(text, true);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x82);
  z_x64_emit_ud2(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
  z_x64_emit_push_rax(text);
  if (!machx64_emit_byte_view_ptr(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_pop_reg64(text, 1);
  z_x64_emit_add_rax_rcx(text, true);
  z_x64_emit_movzx_reg32_ptr_reg_u8(text, 0, 0);
  return true;
}

static bool machx64_emit_byte_le_read_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  // std.codec.readU16Le / readI32Le / readU32Le / readI64Le / readU64Le / readF32Le / readF64Le:
  // read a 2/4/8-byte little-endian scalar at a byte offset. x86-64 is little-endian and tolerates
  // unaligned loads, so a load at ptr+offset already yields the value; same-width signed/unsigned
  // share the raw load. Bounds-check that offset+size fits the span length first: with the 64-bit
  // add and unsigned compare, offset+(size-1) < len is equivalent to offset+size <= len and rejects
  // offset overflow.
  bool is_float = value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE;
  bool is_f64 = is_float && machx64_type_is_f64(value->type);
  if (!value->left) return machx64_diag_at(diag, is_float ? "direct x86_64 Mach-O readF*Le requires a byte view" : "direct x86_64 Mach-O read*Le requires a byte view", value->line, value->column, "missing byte view");
  if (!value->index) return machx64_diag_at(diag, is_float ? "direct x86_64 Mach-O readF*Le requires an offset" : "direct x86_64 Mach-O read*Le requires an offset", value->line, value->column, "missing offset");
  unsigned scalar_size = is_float ? (is_f64 ? 8u : 4u) : machx64_type_byte_size(value->type);
  if (!machx64_emit_value(text, fun, value->index, ctx, diag)) return false; // rax = offset
  z_x64_emit_push_rax(text); // preserve true offset across the length evaluation
  if (!machx64_emit_byte_view_len(text, fun, value->left, ctx, diag)) return false; // rax = len
  z_x64_emit_mov_rcx_from_rax(text, true); // rcx = len
  z_x64_emit_pop_rax(text); // rax = offset
  z_x64_emit_push_rax(text); // re-save the true offset for the address computation
  z_x64_emit_add_rax_u32(text, scalar_size - 1u, true); // rax = offset + (size - 1)
  z_x64_emit_cmp_rax_rcx(text, true);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x82); // JB: offset+(size-1) < len
  z_x64_emit_ud2(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
  if (!machx64_emit_byte_view_ptr(text, fun, value->left, ctx, diag)) return false; // rax = ptr
  z_x64_emit_pop_reg64(text, 1); // rcx = offset
  z_x64_emit_add_rax_rcx(text, true); // rax = ptr + offset
  if (is_float) z_x64_emit_movs_xmm_ptr_reg(text, 0, 0, is_f64, true); // movss/movsd xmm0, [rax]
  else if (scalar_size == 8) z_x64_emit_load_reg_ptr_reg(text, 0, 0, true); // mov rax, [rax]
  else if (scalar_size == 2) z_x64_emit_movzx_reg32_ptr_reg_disp_u16(text, 0, 0, 0); // movzx eax, word [rax]
  else z_x64_emit_load_reg_ptr_reg(text, 0, 0, false); // mov eax, [rax]
  return true;
}

static bool machx64_emit_byte_copy_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return machx64_diag_at(diag, "direct x86_64 Mach-O byte copy requires source and destination byte views", value->line, value->column, "missing byte view");
  if (!machx64_emit_byte_view_ptr(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_byte_view_len(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_byte_view_ptr(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_byte_view_len(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_pop_reg64(text, 7);
  z_x64_emit_pop_reg64(text, 1);
  z_x64_emit_pop_reg64(text, 6);
  z_x64_emit_byte_copy_min_loop(text);
  return true;
}

static bool machx64_emit_byte_fill_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return machx64_diag_at(diag, "direct x86_64 Mach-O byte fill requires a fill byte and destination byte view", value->line, value->column, "missing byte fill input");
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_byte_view_ptr(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_byte_view_len(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_mov_rdx_from_rax(text);
  z_x64_emit_pop_reg64(text, 7);
  z_x64_emit_pop_reg64(text, 9);
  z_x64_emit_byte_fill_loop(text);
  return true;
}

static bool machx64_emit_byte_view_eq_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return machx64_diag_at(diag, "direct x86_64 Mach-O byte-view equality requires two byte views", value->line, value->column, "missing byte view");
  if (!machx64_emit_byte_view_len(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_byte_view_len(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_pop_reg64(text, 1);
  z_x64_emit_cmp_reg_reg(text, 1, 0, false);
  size_t same_len = z_x64_emit_jcc32_placeholder(text, 0x84);
  z_x64_emit_mov_eax_u32(text, 0);
  size_t end = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, same_len, text->len);
  z_x64_emit_mov_reg_from_rax(text, 10, true);
  if (!machx64_emit_byte_view_ptr(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_mov_reg_from_rax(text, 8, true);
  if (!machx64_emit_byte_view_ptr(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_mov_r9_from_rax(text);
  z_x64_emit_byte_eq_loop(text);
  z_x64_patch_rel32(text, end, text->len);
  return true;
}

// Compute the element address (span.ptr + index * sizeof(T)) of a typed-span local into rax, with a
// bounds check (index < span.len traps via ud2). The index is materialized into rax then moved to
// rcx for the scaling LEA; span.len is the runtime element count at slot +8, span.ptr at slot +0.
static bool machx64_emit_span_index_addr(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, MachOEmitContext *ctx, ZDiag *diag) {
  const IrLocal *local = &fun->locals[local_index];
  if (!index || !machx64_emit_value(text, fun, index, ctx, diag)) return false;
  z_x64_emit_mov_rcx_from_rax(text, true);
  machx64_emit_load_local_slot_rax(text, fun, local_index, 8);
  z_x64_emit_cmp_reg_reg(text, 1, 0, true); // cmp rcx(index), rax(len); JB taken when index < len
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x82);
  z_x64_emit_ud2(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
  machx64_emit_load_local_slot_rax(text, fun, local_index, 0);
  z_x64_emit_lea_base_index_scale_disp_reg(text, 0, 0, 1, machx64_type_byte_size(local->element_type), 0);
  return true;
}

static bool machx64_emit_index_load_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (value->array_index >= fun->local_len) return machx64_diag_at(diag, "direct x86_64 Mach-O indexed load array is out of range", value->line, value->column, "invalid array local");
  const IrLocal *local = &fun->locals[value->array_index];
  if (local->type == IR_TYPE_BYTE_VIEW) {
    // Typed-span element read: element-scaled address (rax) then a width-appropriate load. u8/Bool
    // zero-extend (movzbl), i8 sign-extend (movsbl), 8-byte via 64-bit mov, float via movss/movsd
    // into xmm0 (the value's type is f32/f64 so callers read xmm0), else 32-bit mov.
    if (!machx64_emit_span_index_addr(text, fun, value->array_index, value->index, ctx, diag)) return false;
    IrTypeKind elem = local->element_type;
    if (elem == IR_TYPE_U8 || elem == IR_TYPE_BOOL) z_x64_emit_movzx_reg32_ptr_reg_u8(text, 0, 0);
    else if (elem == IR_TYPE_I8) z_x64_emit_movsx_reg32_ptr_reg_i8(text, 0, 0);
    else if (machx64_type_is_float(elem)) z_x64_emit_movs_xmm_ptr_reg(text, 0, 0, machx64_type_is_f64(elem), true);
    else if (machx64_type_is_i64(elem)) z_x64_emit_load_reg_ptr_reg(text, 0, 0, true);
    else z_x64_emit_load_reg_ptr_reg(text, 0, 0, false);
    return true;
  }
  bool byte_array = local->element_type == IR_TYPE_U8 || local->element_type == IR_TYPE_BOOL;
  unsigned const_index = 0;
  if (local->is_array && !byte_array && machx64_const_u32_value(value->index, &const_index) && const_index < local->array_len) {
    machx64_emit_load_local_slot_eax(text, fun, value->array_index, const_index * 4u);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
    if (!value->index || !machx64_emit_value(text, fun, value->index, ctx, diag)) return false;
    machx64_emit_bounds_check(text, local);
    z_x64_emit_push_rax(text);
    machx64_emit_array_base_rdx(text, fun, value->array_index);
    z_x64_emit_pop_reg64(text, 1);
    z_x64_emit_shl_rcx_imm8(text, 2);
    z_x64_emit_add_rdx_rcx(text, true);
    z_x64_emit_load_reg_ptr_reg(text, 0, 2, false);
    return true;
  }
  if (!local->is_array || !byte_array) return machx64_diag_at(diag, "direct x86_64 Mach-O indexed load requires [N]u8, [N]Bool, or integer arrays", value->line, value->column, "unsupported array local");
  if (!value->index || !machx64_emit_value(text, fun, value->index, ctx, diag)) return false;
  machx64_emit_bounds_check(text, local);
  z_x64_emit_push_rax(text);
  machx64_emit_array_base_rdx(text, fun, value->array_index);
  z_x64_emit_pop_reg64(text, 1);
  z_x64_emit_add_rdx_rcx(text, true);
  z_x64_emit_movzx_reg32_ptr_reg_u8(text, 0, 2);
  return true;
}

static bool machx64_emit_field_load_value(ZBuf *text, const IrFunction *fun, const IrValue *value, ZDiag *diag) {
  if (value->local_index >= fun->local_len) return machx64_diag_at(diag, "direct x86_64 Mach-O field load record is out of range", value->line, value->column, "invalid record local");
  if (!fun->locals[value->local_index].is_record) return machx64_diag_at(diag, "direct x86_64 Mach-O field load requires record local", value->line, value->column, "non-record local");
  if (machx64_type_is_float(value->type)) machx64_emit_load_field_xmm0(text, fun, value->local_index, value->field_offset, value->type);
  else machx64_emit_load_field_eax(text, fun, value->local_index, value->field_offset, value->type);
  return true;
}

// Single-arg libm call (System V FP ABI): argument in xmm0, result in xmm0. All std.math libm
// helpers operate on f32. The undefined external + X86_64_RELOC_BRANCH relocation are resolved by
// the host link step against libSystem (-lm). Shares the arch-neutral Mach-O math machinery.
static bool machx64_emit_math_unary_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left) return machx64_diag_at(diag, "direct x86_64 Mach-O math call requires an argument", value->line, value->column, "missing argument");
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  size_t patch = z_x64_emit_call32_placeholder(text);
  return z_macho_record_math_call_patch(ctx, patch, z_macho_math_symbol_for_value(value->kind), value, diag);
}

// Two-arg libm call (powf): arg0 in xmm0, arg1 in xmm1. Evaluate arg0 into xmm0 and spill it across
// arg1's evaluation (which itself may call libm), then place arg1 in xmm1 and reload arg0 into xmm0.
static bool machx64_emit_math_powf_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left || !value->right) return machx64_diag_at(diag, "direct x86_64 Mach-O powf requires two arguments", value->line, value->column, "missing argument");
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_xmm_push(text, 0);
  if (!machx64_emit_value(text, fun, value->right, ctx, diag)) return false;
  z_x64_emit_movaps(text, 1, 0);
  z_x64_emit_xmm_pop(text, 0);
  size_t patch = z_x64_emit_call32_placeholder(text);
  return z_macho_record_math_call_patch(ctx, patch, Z_MACHO_MATH_POWF, value, diag);
}

// isNaNf is inline (no libm symbol): NaN is the only value where x != x. UCOMIS sets PF=1 on an
// unordered compare, so SETP yields 1 exactly for NaN. Result goes to eax as a Bool.
static bool machx64_emit_math_isnanf_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value->left) return machx64_diag_at(diag, "direct x86_64 Mach-O isNaNf requires an argument", value->line, value->column, "missing argument");
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_ucomis(text, 0, 0, machx64_type_is_f64(value->left->type));
  z_x64_emit_setcc_al_to_bool(text, 0x9a); // SETP
  return true;
}

static bool machx64_emit_value(ZBuf *text, const IrFunction *fun, const IrValue *value, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value) return machx64_diag_at(diag, "direct x86_64 Mach-O expression is missing", 1, 1, "missing expression");
  switch (value->kind) {
    case IR_VALUE_FLOAT: {
      // Materialize the IEEE bit pattern in a GPR, then MOVD/MOVQ it into xmm0.
      bool is64 = machx64_type_is_f64(value->type);
      if (is64) z_x64_emit_mov_rax_u64(text, (uint64_t)value->int_value);
      else z_x64_emit_mov_eax_u32(text, (uint32_t)value->int_value);
      z_x64_emit_movd_xmm_from_gpr(text, 0, 0, is64);
      return true;
    }
    case IR_VALUE_BOOL:
    case IR_VALUE_INT:
      if (machx64_type_is_i64(value->type)) z_x64_emit_mov_rax_u64(text, (uint64_t)value->int_value);
      else z_x64_emit_mov_eax_u32(text, (uint32_t)value->int_value);
      return true;
    case IR_VALUE_LOCAL: return machx64_emit_local_value(text, fun, value, diag);
    case IR_VALUE_CAST: {
      IrTypeKind src = value->left ? value->left->type : IR_TYPE_UNSUPPORTED;
      IrTypeKind dst = value->type;
      if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
      bool src_float = machx64_type_is_float(src);
      bool dst_float = machx64_type_is_float(dst);
      if (src_float && dst_float) {
        if (src != dst) z_x64_emit_cvts2s(text, 0, 0, machx64_type_is_f64(src)); // float<->float
      } else if (!src_float && dst_float) {
        z_x64_emit_cvtsi2s(text, 0, 0, machx64_type_is_f64(dst), machx64_type_is_i64(src)); // int->float
      } else if (src_float && !dst_float) {
        z_x64_emit_cvtts2si(text, 0, 0, machx64_type_is_f64(src), machx64_type_is_i64(dst)); // float->int (trunc)
      } else {
        machx64_emit_cast_normalize_rax(text, src, dst);
      }
      return true;
    }
    case IR_VALUE_BINARY: return machx64_emit_binary_value(text, fun, value, ctx, diag);
    case IR_VALUE_COMPARE: return machx64_emit_compare_value(text, fun, value, ctx, diag);
    case IR_VALUE_CHECK: return machx64_emit_check_value(text, fun, value, ctx, diag);
    case IR_VALUE_RESCUE: return machx64_emit_rescue_value(text, fun, value, ctx, diag);
    case IR_VALUE_CALL: return machx64_emit_call_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_VIEW_LEN: return machx64_emit_byte_view_len(text, fun, value->left, ctx, diag);
    case IR_VALUE_BYTE_COPY: return machx64_emit_byte_copy_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_FILL: return machx64_emit_byte_fill_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_VIEW_EQ: return machx64_emit_byte_view_eq_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_VIEW_INDEX_LOAD: return machx64_emit_byte_view_index_load_value(text, fun, value, ctx, diag);
    case IR_VALUE_BYTE_VIEW_READ_INT_LE: case IR_VALUE_BYTE_VIEW_READ_FLOAT_LE: return machx64_emit_byte_le_read_value(text, fun, value, ctx, diag);
    case IR_VALUE_INDEX_LOAD: return machx64_emit_index_load_value(text, fun, value, ctx, diag);
    case IR_VALUE_FIELD_LOAD: return machx64_emit_field_load_value(text, fun, value, diag);
    case IR_VALUE_MAYBE_HAS:
      if (value->local_index >= fun->local_len ||
          (fun->locals[value->local_index].type != IR_TYPE_MAYBE_BYTE_VIEW && fun->locals[value->local_index].type != IR_TYPE_MAYBE_SCALAR)) {
        return machx64_diag_at(diag, "direct x86_64 Mach-O maybe helper requires a Maybe local", value->line, value->column, "invalid maybe local");
      }
      machx64_emit_load_local_slot_eax(text, fun, value->local_index, 0);
      return true;
    case IR_VALUE_MAYBE_VALUE:
      // Scalar payload at @8. Byte-view ptr@8/len@16 are read through machx64_emit_byte_view_ptr/_len
      // when MAYBE_VALUE appears as a span source (Maybe<MutSpan<u8>>); this path covers the scalar
      // case (Maybe<i32> etc.).
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_MAYBE_SCALAR) return machx64_diag_at(diag, "direct x86_64 Mach-O maybe scalar value requires a Maybe scalar local", value->line, value->column, "invalid maybe value");
      machx64_emit_load_local_slot_rax(text, fun, value->local_index, 8);
      return true;
    case IR_VALUE_MATH_SQRTF: case IR_VALUE_MATH_EXPF: case IR_VALUE_MATH_COSF: case IR_VALUE_MATH_SINF:
    case IR_VALUE_MATH_FABSF: case IR_VALUE_MATH_FLOORF:
      return machx64_emit_math_unary_value(text, fun, value, ctx, diag);
    case IR_VALUE_MATH_POWF: return machx64_emit_math_powf_value(text, fun, value, ctx, diag);
    case IR_VALUE_MATH_ISNANF: return machx64_emit_math_isnanf_value(text, fun, value, ctx, diag);
    case IR_VALUE_FS_HOST:
      // std.fs.host() is a stateless capability token; mirrors the ELF/macho64 backends which
      // both return 0 (the OS-interface lowerings call libSystem/syscalls directly).
      z_x64_emit_xor_eax_eax(text);
      return true;
    case IR_VALUE_ARGS_LEN:
      // std.args.len(). With process-args seeding (the direct-exe path), argc was stashed
      // into r14 by the prologue: push r14; pop rax recovers it. Without seeding (the obj+link
      // path), argc lives at [r15] under a musl-style argv vector layout; the load mirrors elf64's
      // fallback. The value type is usize; on x86_64 usize is 64-bit, so we load full-width.
      if (ctx && ctx->seed_main_process_args) {
        z_x64_emit_push_reg64(text, 14);
        z_x64_emit_pop_reg64(text, 0);
        return true;
      }
      z_x64_emit_load_reg_ptr_reg(text, 0, 15, true);
      return true;
    case IR_VALUE_FS_MUNMAP: {
      // std.fs.munmap(&mut m): release a mapping via libSystem _munmap(addr, len). The Mapping
      // local stores ptr@0 (64-bit) and len@8 (64-bit byte count, matching the byte-view store);
      // len is loaded full-width into rsi so munmap(addr, full_size) handles >4 GiB mappings.
      // Result type is Void; nothing is produced into rax.
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_BYTE_VIEW) {
        return machx64_diag_at(diag, "direct x86_64 Mach-O std.fs.munmap requires a Mapping local", value->line, value->column, "invalid Mapping");
      }
      machx64_emit_load_local_slot_rax(text, fun, value->local_index, 0); // addr
      z_x64_emit_mov_rdi_from_rax(text);                                  // rdi = addr
      machx64_emit_load_local_slot_rax(text, fun, value->local_index, 8); // rax = len (64-bit)
      z_x64_emit_mov_rsi_from_rax(text);                                  // rsi = len
      size_t patch = z_x64_emit_call32_placeholder(text);
      if (!z_macho_record_libc_call_patch(ctx, patch, Z_MACHO_LIBC_MUNMAP, value, diag)) return false;
      return true;
    }
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported value kind %d", (int)value->kind);
      return machx64_diag_at(diag, "direct x86_64 Mach-O value kind is unsupported", value->line, value->column, actual);
    }
  }
}

static bool machx64_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, MachOEmitContext *ctx, ZDiag *diag);

static bool machx64_emit_world_write(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  if (!instr || !instr->value) return machx64_diag_at(diag, "direct x86_64 Mach-O World write requires bytes", instr ? instr->line : 1, instr ? instr->column : 1, "missing byte view");
  if (!machx64_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_push_rax(text);
  if (!machx64_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_mov_rdx_from_rax(text);
  z_x64_emit_pop_reg64(text, 6);
  z_x64_emit_mov_reg_u32(text, 7, instr->field_offset == 2 ? 2u : 1u);
  size_t patch = z_x64_emit_call32_placeholder(text);
  z_x64_emit_test_rax_rax(text, false);
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  z_x64_emit_ud2(text);
  z_x64_patch_rel32(text, ok_patch, text->len);
  return z_macho_record_instr_runtime_patch(ctx, MACHO_RUNTIME_WORLD_WRITE, patch, instr, diag);
}

static bool machx64_emit_local_set_byte_view(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  // A span-returning call leaves ptr in rax and len in rdx (SysV second-return). Capture both before
  // evaluating anything else; falling back to byte_view_ptr/_len would re-evaluate the call.
  if (instr->value && instr->value->kind == IR_VALUE_CALL) {
    if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
    machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
    machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 2, 8, true);
    return true;
  }
  if (!machx64_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
  machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
  if (!machx64_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
  machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 8, true);
  return true;
}

// Open a file by path, measure its size, and map it read-only via libSystem. On return rax holds
// the mapping address and rdx the 64-bit byte length; a negative rax signals the open/lseek/mmap
// failure path (the fd is closed first on the lseek-failure leg, the open-failure leg already has
// the stack balanced). Mirrors the ELF raw-syscall file-mmap sequence (elf_emit_mmap_file_addr_size),
// but resolves the libSystem externals through the host link step. The size is held in a 64-bit
// register and stored back into the Mapping byte-view as a 64-bit count by the caller (the
// Mapping/span len slot is 64-bit-wide), so files >4 GiB map correctly. Uses libc-style call convention
// (rdi/rsi/rdx/rcx/r8/r9 for args; r10 is the syscall-only quirk).
static bool machx64_emit_mmap_file_addr_size(ZBuf *text, const IrFunction *fun, const IrValue *path, MachOEmitContext *ctx, ZDiag *diag) {
  // _open(path, O_RDONLY=0)
  if (!machx64_emit_byte_view_ptr(text, fun, path, ctx, diag)) return false; // rax = path ptr
  z_x64_emit_mov_rdi_from_rax(text);                                          // rdi = path
  z_x64_emit_xor_reg_reg(text, 6, true);                                      // rsi = 0 (flags = O_RDONLY)
  size_t open_patch = z_x64_emit_call32_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, open_patch, Z_MACHO_LIBC_OPEN, path, diag)) return false;
  z_x64_emit_test_rax_rax(text, true);
  size_t open_fail = z_x64_emit_jcc32_placeholder(text, 0x88);                // JS: negative fd → open failure
  z_x64_emit_push_rax(text);                                                  // [rsp+0] = fd
  z_x64_emit_mov_rdi_from_rax(text);                                          // rdi = fd
  z_x64_emit_xor_reg_reg(text, 6, true);                                      // rsi = 0 (offset)
  z_x64_emit_mov_reg_u32(text, 2, 2);                                         // rdx = SEEK_END
  size_t seek_patch = z_x64_emit_call32_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, seek_patch, Z_MACHO_LIBC_LSEEK, path, diag)) return false;
  z_x64_emit_test_rax_rax(text, true);
  size_t seek_fail = z_x64_emit_jcc32_placeholder(text, 0x88);
  z_x64_emit_push_rax(text);                                                  // [rsp+0] = size, [rsp+8] = fd
  // _mmap(NULL, size, PROT_READ=1, MAP_PRIVATE=2, fd, 0)
  z_x64_emit_mov_rsi_from_rax(text);                                          // rsi = len = size
  z_x64_emit_xor_reg_reg(text, 7, true);                                      // rdi = NULL
  z_x64_emit_mov_reg_u32(text, 2, 1);                                         // rdx = PROT_READ
  z_x64_emit_mov_reg_u32(text, 1, 2);                                         // rcx = MAP_PRIVATE
  z_x64_emit_load_rsp_offset_reg(text, 8, 8, true);                           // r8 = fd
  z_x64_emit_xor_reg_reg(text, 9, true);                                      // r9 = 0 (offset)
  size_t mmap_patch = z_x64_emit_call32_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, mmap_patch, Z_MACHO_LIBC_MMAP, path, diag)) return false; // rax = addr or -1
  // Close the fd on both the success and MAP_FAILED paths; preserve addr across the close, then
  // recover addr in rax and size in rdx. A MAP_FAILED (negative) addr flows through unchanged.
  z_x64_emit_push_rax(text);                                                  // [rsp+0]=addr, [rsp+8]=size, [rsp+16]=fd
  z_x64_emit_load_rsp_offset_reg(text, 7, 16, true);                          // rdi = fd
  size_t close_patch = z_x64_emit_call32_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, close_patch, Z_MACHO_LIBC_CLOSE, path, diag)) return false;
  z_x64_emit_pop_rax(text);                                                   // rax = addr; [rsp+0]=size, [rsp+8]=fd
  z_x64_emit_pop_reg64(text, 2);                                              // rdx = size; [rsp+0]=fd
  z_x64_emit_add_rsp(text, 8);                                                // drop fd
  size_t done = z_x64_emit_jmp32_placeholder(text, 0xe9);
  // Seek failure: stack holds [rsp+0]=fd, rax = negative lseek result. Close the fd, preserve the
  // negative result across the close, then return it in rax.
  z_x64_patch_rel32(text, seek_fail, text->len);
  z_x64_emit_load_rsp_offset_reg(text, 7, 0, true);                           // rdi = fd
  z_x64_emit_push_rax(text);                                                  // preserve negative result; [rsp+0]=neg, [rsp+8]=fd
  size_t seek_close_patch = z_x64_emit_call32_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, seek_close_patch, Z_MACHO_LIBC_CLOSE, path, diag)) return false;
  z_x64_emit_pop_rax(text);                                                   // rax = negative result; [rsp+0]=fd
  z_x64_emit_add_rsp(text, 8);                                                // drop fd
  size_t seek_done = z_x64_emit_jmp32_placeholder(text, 0xe9);
  // open_fail lands here with rax already holding the negative open result (stack balanced).
  z_x64_patch_rel32(text, open_fail, text->len);
  z_x64_patch_rel32(text, done, text->len);
  z_x64_patch_rel32(text, seek_done, text->len);
  return true;
}

// Anonymous std.mem.pageAlloc allocation: a fresh kernel-zeroed region via libSystem _mmap (the
// macOS analog of ELF's MAP_ANON mmap, calloc semantics). The size value is evaluated then spilled
// so it survives the call, the SysV libc args are set up, and `call _mmap` is recorded as an
// external relocation. The result populates the Maybe<MutSpan<u8>> dest local: on success has=1@0,
// ptr@8, len@16; on failure (MAP_FAILED, a negative return) the Maybe is cleared. Darwin map flags
// differ from Linux: MAP_ANON=0x1000 (Linux 0x20) and MAP_PRIVATE=0x2 so flags=0x1002;
// PROT_READ|PROT_WRITE=0x3; fd=-1.
static bool machx64_emit_anon_mmap_to_local(ZBuf *text, const IrFunction *fun, const IrValue *size, unsigned local_index, MachOEmitContext *ctx, ZDiag *diag) {
  if (!size) return machx64_diag_at(diag, "direct x86_64 Mach-O page allocation requires a byte length", 1, 1, "missing length");
  if (!machx64_emit_value(text, fun, size, ctx, diag)) return false;          // rax = len
  z_x64_emit_push_rax(text);                                                  // [rsp+0] = len (survives the call)
  z_x64_emit_mov_rsi_from_rax(text);                                          // rsi = len
  z_x64_emit_xor_reg_reg(text, 7, true);                                      // rdi = NULL
  z_x64_emit_mov_reg_u32(text, 2, 3);                                         // rdx = PROT_READ|PROT_WRITE
  z_x64_emit_mov_reg_u32(text, 1, 0x1002);                                    // rcx = MAP_ANON|MAP_PRIVATE (Darwin)
  z_x64_emit_mov_reg_u64(text, 8, 0xffffffffffffffffULL);                     // r8 = fd = -1
  z_x64_emit_xor_reg_reg(text, 9, true);                                      // r9 = 0 (offset)
  size_t patch = z_x64_emit_call32_placeholder(text);
  if (!z_macho_record_libc_call_patch(ctx, patch, Z_MACHO_LIBC_MMAP, size, diag)) return false;
  z_x64_emit_test_rax_rax(text, true);
  size_t fail = z_x64_emit_jcc32_placeholder(text, 0x88);                     // negative → failure
  z_x64_emit_push_rax(text);                                                  // [rsp+0]=addr, [rsp+8]=len
  z_x64_emit_mov_eax_u32(text, 1);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false); // has = 1
  z_x64_emit_pop_rax(text);                                                   // rax = addr; [rsp+0]=len
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true); // ptr
  z_x64_emit_pop_rax(text);                                                   // rax = len
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 16, true); // len (64-bit byte count)
  size_t end = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, fail, text->len);
  z_x64_emit_add_rsp(text, 8);                                                // drop the pushed len before clearing
  machx64_emit_maybe_clear(text, fun, local_index);
  z_x64_patch_rel32(text, end, text->len);
  return true;
}

// `let arg = std.args.get(i)` -> Maybe<MutSpan<u8>> {has@0, ptr@8, len@16 byte count}.
// Bounds-check the index against argc (r14 when seeded, [r15] under the musl-style argv layout
// otherwise), load argv[i] into rax via [r15 + i*8], measure its NUL-terminated length, and write
// the Maybe out-of-band on the failure path via machx64_emit_maybe_clear. Mirrors
// elf_emit_args_get_to_local — same SysV-style register layout (r14=argc, r15=argv), same
// opcode shapes.
static bool machx64_emit_args_get_to_local(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned local_index, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value || !value->left) return machx64_diag_at(diag, "direct x86_64 Mach-O std.args.get requires an index", value ? value->line : 1, value ? value->column : 1, "missing index");
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  if (ctx && ctx->seed_main_process_args) {
    z_x64_emit_push_reg64(text, 14);                       // push r14 (argc)
    z_x64_emit_pop_reg64(text, 1);                         // rcx = argc
    z_x64_emit_cmp_rax_rcx(text, true);                    // cmp rax(index), rcx(argc)
  } else {
    z_x64_emit_cmp_reg_ptr_reg(text, 0, 15, true);         // cmp rax, [r15] (argc word)
  }
  size_t in_range = z_x64_emit_jcc32_placeholder(text, 0x82); // JB: index < argc
  machx64_emit_maybe_clear(text, fun, local_index);
  size_t end = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, in_range, text->len);
  if (ctx && ctx->seed_main_process_args) {
    // r15 = argv (a pointer-array, not preceded by argc), so argv[i] = [r15 + i*8].
    z_x64_emit_load_base_index_scale_disp_reg(text, 0, 15, 0, 8, 0, true);
  } else {
    // Musl-style argv vector at [r15]: word 0 is argc, words 1..N are argv pointers; argv[i] = [r15 + i*8 + 8].
    z_x64_emit_load_base_index_scale_disp_reg(text, 0, 15, 0, 8, 8, true);
  }
  z_x64_emit_push_rax(text);                               // preserve ptr across strlen
  machx64_emit_strlen_rax_to_ecx(text);                    // ecx = strlen(rax); rax preserved by mov rdx,rax
  z_x64_emit_mov_eax_u32(text, 1);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false); // has = 1
  z_x64_emit_pop_rax(text);                                // rax = argv[i] (ptr)
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);  // ptr
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 1, 16, true); // len = rcx (64-bit byte count)
  z_x64_patch_rel32(text, end, text->len);
  return true;
}

// `let m = std.fs.mmap(fs, path)` -> Maybe<owned<Mapping>> (has@0, ptr@8, len@16 byte count). The
// helper lands addr in rax and the 64-bit byte length in rdx; a negative addr is the
// not-found/failure path, which clears the Maybe. Mirrors elf_emit_fs_mmap_to_local.
static bool machx64_emit_fs_mmap_to_local(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned local_index, MachOEmitContext *ctx, ZDiag *diag) {
  if (!machx64_emit_mmap_file_addr_size(text, fun, instr->value->left, ctx, diag)) return false;
  z_x64_emit_test_rax_rax(text, true);
  size_t fail = z_x64_emit_jcc32_placeholder(text, 0x88);                     // MAP_FAILED / open failure
  z_x64_emit_push_rax(text);                                                  // [rsp+0]=addr (rdx=len survives the has store)
  z_x64_emit_mov_eax_u32(text, 1);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false); // has = 1
  z_x64_emit_pop_rax(text);                                                   // rax = addr
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true); // ptr
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 2, 16, true); // len (64-bit byte count, from rdx)
  size_t end = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, fail, text->len);
  machx64_emit_maybe_clear(text, fun, local_index);
  z_x64_patch_rel32(text, end, text->len);
  return true;
}

// FixedBufAlloc layout: {ptr@0 i64, capacity@8 u32, position@12 u32}. Init seeds ptr and capacity
// from the backing buffer (Span<u8>) and zeroes position. PageAlloc carries no pre-reserved buffer
// (each allocBytes is a fresh anon mmap via machx64_emit_anon_mmap_to_local); zero the slot so it
// holds no stale pointer/length.
static bool machx64_emit_alloc_local_set(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->value && instr->value->kind == IR_VALUE_PAGE_ALLOC) {
    z_x64_emit_xor_eax_eax(text);
    machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
    return true;
  }
  if (!instr->value || instr->value->kind != IR_VALUE_FIXED_BUF_ALLOC) return machx64_diag_at(diag, "direct x86_64 Mach-O FixedBufAlloc local requires std.mem.fixedBufAlloc", instr->line, instr->column, "unsupported allocator initializer");
  if (!machx64_emit_byte_view_ptr(text, fun, instr->value->left, ctx, diag)) return false;
  machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, true);
  if (!machx64_emit_byte_view_len(text, fun, instr->value->left, ctx, diag)) return false;
  machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 8, false);
  z_x64_emit_mov_eax_u32(text, 0);
  machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 12, false);
  return true;
}

// `let r = std.mem.allocBytes(alloc, n)` -> Maybe<MutSpan<u8>> {has@0, ptr@8, len@16}. FixedBufAlloc
// bump with overflow detection — mirrors the AArch64 Mach-O host (emit_macho64.c). PageAlloc
// (anonymous mmap) is rejected here.
//
// Plan: stash all the inputs we need in stack slots before checking, then either commit (success
// branch) or clear (failure branch). After the OK branch we still need (position, n, new_position)
// to write out the four destinations (Maybe.has, .ptr, .len, alloc.position), so they're stashed
// on the call-stack via push.
//
// Stack layout entering the OK branch (after the prologue pushes):
//   [rsp+16] = position  (32-bit, but pushed 64-bit)
//   [rsp+8]  = n         (32-bit, pushed 64-bit)
//   [rsp+0]  = new_pos   (32-bit, pushed 64-bit; == position + n)
// The failure branch pops 3 slots and clears the Maybe.
static bool machx64_emit_alloc_bytes_to_local(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  const IrValue *value = instr->value;
  if (!value || value->kind != IR_VALUE_ALLOC_BYTES || value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_ALLOC) return machx64_diag_at(diag, "direct x86_64 Mach-O allocation source is invalid", instr->line, instr->column, "invalid allocation");
  if (fun->locals[value->local_index].is_page_alloc) {
    // PageAlloc: each allocation is a fresh kernel-zeroed anonymous _mmap region (calloc semantics),
    // rather than a bump out of a pre-reserved buffer.
    return machx64_emit_anon_mmap_to_local(text, fun, value->left, instr->local_index, ctx, diag);
  }
  unsigned alloc_index = value->local_index;
  unsigned local_index = instr->local_index;
  // Step 1: rax = n. Spill to rcx via push, then load position into rax→push.
  if (!machx64_emit_value(text, fun, value->left, ctx, diag)) return false;
  z_x64_emit_mov_rcx_from_rax(text, true);                                              // rcx = n (saved)
  machx64_emit_load_local_slot_eax(text, fun, alloc_index, 12);                        // rax = position (32-bit)
  z_x64_emit_push_rax(text);                                                            // [rsp+16] = position
  z_x64_emit_mov_reg_from_reg(text, 0, 1, true);                                        // rax = n (from rcx)
  z_x64_emit_push_rax(text);                                                            // [rsp+8] = n
  // Compute new_position = position + n; cmp with capacity.
  // rax already = n; add position from spill: load via [rsp+8]? push rcx separately easier.
  // Approach: rax = position (load fresh) then add n (still in rcx).
  machx64_emit_load_local_slot_eax(text, fun, alloc_index, 12);                        // rax = position
  z_x64_emit_add_reg_reg(text, 0, 1, false);                                            // rax = position + n  (32-bit)
  z_x64_emit_push_rax(text);                                                            // [rsp+0] = new_position
  machx64_emit_load_local_slot_eax(text, fun, alloc_index, 8);                         // rax = capacity (32-bit)
  z_x64_emit_mov_rcx_from_rax(text, false);                                             // rcx = capacity
  z_x64_emit_pop_rax(text);                                                             // rax = new_position
  z_x64_emit_push_rax(text);                                                            // restore stack: [rsp+0]=new_pos
  z_x64_emit_cmp_reg_reg(text, 0, 1, false);                                            // cmp new_pos, capacity
  size_t fail_patch = z_x64_emit_jcc32_placeholder(text, 0x87);                         // JA: new_pos > capacity → fail
  // Success branch. Stack: [rsp+0]=new_pos, [rsp+8]=n, [rsp+16]=position.
  z_x64_emit_pop_rax(text);                                                              // rax = new_position
  machx64_emit_store_local_slot_from_reg(text, fun, alloc_index, 0, 12, false);        // alloc.position = new_pos (32-bit)
  z_x64_emit_pop_rax(text);                                                              // rax = n
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 16, true);         // Maybe.len = n (64-bit byte count)
  z_x64_emit_pop_rax(text);                                                              // rax = position
  z_x64_emit_mov_rcx_from_rax(text, true);                                               // rcx = position (zero-extended via 32-bit mov above? no, the above was 32-bit move. need wide.)
  // The pop_rax above is a 64-bit pop into rax — value is the 64-bit pushed value, low 32 bits = pos.
  // For pointer arithmetic we want the unsigned 32-bit position zero-extended; mov ecx, eax suffices.
  z_x64_emit_mov_reg_from_reg(text, 1, 0, false);                                        // rcx = position (zero-extended)
  machx64_emit_load_local_slot_rax(text, fun, alloc_index, 0);                          // rax = alloc.ptr (64-bit)
  z_x64_emit_add_reg_reg(text, 0, 1, true);                                              // rax = alloc.ptr + position
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 8, true);            // Maybe.ptr = rax (64-bit)
  z_x64_emit_mov_eax_u32(text, 1);
  machx64_emit_store_local_slot_from_reg(text, fun, local_index, 0, 0, false);          // Maybe.has = 1
  size_t end_patch = z_x64_emit_jmp32_placeholder(text, 0xe9);
  // Failure branch: pop the three stashed values, clear the Maybe (has=0, ptr=0, len=0).
  z_x64_patch_rel32(text, fail_patch, text->len);
  z_x64_emit_pop_rax(text);                                                              // discard new_pos
  z_x64_emit_pop_rax(text);                                                              // discard n
  z_x64_emit_pop_rax(text);                                                              // discard position
  machx64_emit_maybe_clear(text, fun, local_index);
  z_x64_patch_rel32(text, end_patch, text->len);
  return true;
}

static bool machx64_emit_maybe_byte_view_local_set(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->value && instr->value->kind == IR_VALUE_ALLOC_BYTES) return machx64_emit_alloc_bytes_to_local(text, fun, instr, ctx, diag);
  if (instr->value && instr->value->kind == IR_VALUE_FS_MMAP) return machx64_emit_fs_mmap_to_local(text, fun, instr, instr->local_index, ctx, diag);
  // std.args.get(i). ENV_GET / FS_READ_ALL / FS_TEMP_NAME remain out-of-scope for macho_x64.
  if (instr->value && instr->value->kind == IR_VALUE_ARGS_GET) return machx64_emit_args_get_to_local(text, fun, instr->value, instr->local_index, ctx, diag);
  return machx64_diag_at(diag, "direct x86_64 Mach-O Maybe<byte-view> initializer is unsupported", instr->line, instr->column, "unsupported maybe-byte-view initializer");
}

static bool machx64_emit_maybe_scalar_local_set(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  if (!instr->value) return machx64_diag_at(diag, "direct x86_64 Mach-O Maybe scalar initializer is missing", instr->line, instr->column, "missing maybe value");
  if (instr->value->kind == IR_VALUE_MAYBE_SCALAR_LITERAL) {
    // {has = data_len != 0, value = int_value}. Mirrors elf64's MAYBE_SCALAR_LITERAL path.
    z_x64_emit_mov_eax_u32(text, instr->value->data_len ? 1u : 0u);
    machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 0, false);
    z_x64_emit_mov_eax_u32(text, (uint32_t)instr->value->int_value);
    machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, 8, true);
    return true;
  }
  // Plain fallible-scalar-call path: evaluate the value (lands in rax with the packed error tag in
  // the high 32 bits), branch on the tag — success writes has=1+value, error clears the Maybe.
  // The tag check mirrors machx64_emit_error_condition_from_rax: shift rax >> 32 into rcx and test
  // for zero. The full 64-bit rax is preserved on the stack while the high-32 word is examined.
  if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_push_rax(text);                                                            // save the packed value
  machx64_emit_error_condition_from_rax(text);                                          // rcx = tag, test ecx,ecx (clobbers rcx)
  size_t ok_patch = z_x64_emit_jcc32_placeholder(text, 0x84);                           // JZ: tag==0 → success
  z_x64_emit_pop_rax(text);                                                              // discard saved value (error path)
  machx64_emit_maybe_scalar_clear(text, fun, instr->local_index);
  size_t end = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, ok_patch, text->len);
  z_x64_emit_pop_rax(text);                                                              // restore packed value (tag was 0; full 64-bit is the payload)
  machx64_emit_maybe_scalar_store_rax(text, fun, instr->local_index);
  z_x64_patch_rel32(text, end, text->len);
  return true;
}

static bool machx64_emit_local_set_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->local_index >= fun->local_len) return machx64_diag_at(diag, "direct x86_64 Mach-O local store is out of range", instr->line, instr->column, "invalid local");
  const IrLocal *local = &fun->locals[instr->local_index];
  if (local->is_record) {
    // `let q = check f()` and `let q = f() rescue r0` — fallible record-returning calls.
    if (instr->value && instr->value->kind == IR_VALUE_CHECK) {
      return machx64_emit_local_set_record_check(text, fun, instr, ctx, diag);
    }
    if (instr->value && instr->value->kind == IR_VALUE_RESCUE) {
      return machx64_emit_local_set_record_rescue(text, fun, instr, ctx, diag);
    }
    // `let q = f()` — bind a record-returning call straight into q's slot via sret (no copy).
    if (instr->value && instr->value->kind == IR_VALUE_CALL) {
      return machx64_emit_record_call_with_dest(text, fun, (int)instr->local_index, instr->value, ctx, diag);
    }
    // `let q = p` / `q = p` — record-to-record value copy.
    if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
      machx64_emit_record_copy_to(text, fun, instr->local_index, instr->value->local_index);
      return true;
    }
    return machx64_diag_at(diag, "direct x86_64 Mach-O record local assignment requires a record value", instr->line, instr->column, "unsupported record set");
  }
  if (local->type == IR_TYPE_BYTE_VIEW) return machx64_emit_local_set_byte_view(text, fun, instr, ctx, diag);
  if (local->type == IR_TYPE_ALLOC) return machx64_emit_alloc_local_set(text, fun, instr, ctx, diag);
  if (local->type == IR_TYPE_MAYBE_BYTE_VIEW) return machx64_emit_maybe_byte_view_local_set(text, fun, instr, ctx, diag);
  if (local->type == IR_TYPE_MAYBE_SCALAR) return machx64_emit_maybe_scalar_local_set(text, fun, instr, ctx, diag);
  if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
  if (machx64_type_is_float(local->type)) machx64_emit_store_local_xmm0(text, fun, instr->local_index);
  else machx64_emit_store_local_from_reg(text, fun, instr->local_index, 0);
  return true;
}

static bool machx64_emit_field_store_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  // local_index == UINT_MAX targets the record being returned, written through the saved sret
  // pointer (the `return <shape literal>` lowering stores its fields this way).
  if (instr->local_index == UINT_MAX) {
    IrTypeKind value_type = instr->value ? instr->value->type : IR_TYPE_I32;
    unsigned slot = machx64_sret_slot_offset(fun);
    if (value_type == IR_TYPE_BYTE_VIEW) {
      // Span field via sret: ptr at field_offset, len at field_offset+8. A span-returning call lands
      // ptr in rax + len in rdx; any other byte view materializes ptr-then-len.
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
        z_x64_emit_rbp_disp_reg(text, 0x8b, 7, slot, true);
        z_x64_emit_store_ptr_reg_disp_from_reg(text, 7, instr->field_offset, 0, true);     // mov [rdi + off], rax (ptr)
        z_x64_emit_store_ptr_reg_disp_from_reg(text, 7, instr->field_offset + 8u, 2, true); // mov [rdi + off+8], rdx (len)
        return true;
      }
      if (!machx64_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
      z_x64_emit_push_rax(text);
      if (!machx64_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
      z_x64_emit_mov_reg_from_reg(text, 2, 0, true);   // mov rdx, rax (len)
      z_x64_emit_pop_reg64(text, 0);                   // pop rax (ptr)
      z_x64_emit_rbp_disp_reg(text, 0x8b, 7, slot, true);
      z_x64_emit_store_ptr_reg_disp_from_reg(text, 7, instr->field_offset, 0, true);
      z_x64_emit_store_ptr_reg_disp_from_reg(text, 7, instr->field_offset + 8u, 2, true);
      return true;
    }
    // Primitive (or float) field via sret. Materialize the value, then mov through saved sret ptr.
    if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
    z_x64_emit_rbp_disp_reg(text, 0x8b, 7, slot, true);
    if (machx64_type_is_float(value_type)) {
      // movss/sd [rdi + off], xmm0, honoring the field offset via the displacement form.
      bool is64 = machx64_type_is_f64(value_type);
      z_x64_emit_movs_xmm_ptr_reg_disp(text, 0, 7, instr->field_offset, is64, false);
    } else {
      bool wide = machx64_type_is_i64(value_type);
      z_x64_emit_store_ptr_reg_disp_from_reg(text, 7, instr->field_offset, 0, wide);
    }
    return true;
  }
  if (instr->local_index >= fun->local_len) return machx64_diag_at(diag, "direct x86_64 Mach-O field store record is out of range", instr->line, instr->column, "invalid record local");
  if (!fun->locals[instr->local_index].is_record) return machx64_diag_at(diag, "direct x86_64 Mach-O field store requires record local", instr->line, instr->column, "non-record local");
  // Field store through a ref<Record> is rejected — `set p.x …` requires mutref<Record>.
  if (fun->locals[instr->local_index].is_ref && !fun->locals[instr->local_index].is_mutable) {
    return machx64_diag_at(diag, "direct x86_64 Mach-O field store through ref<Record> requires mutref", instr->line, instr->column, fun->locals[instr->local_index].name ? fun->locals[instr->local_index].name : "ref-record");
  }
  IrTypeKind value_type = instr->value ? instr->value->type : IR_TYPE_I32;
  bool target_is_ref = fun->locals[instr->local_index].is_ref;
  if (value_type == IR_TYPE_BYTE_VIEW) {
    // Span field: store ptr at the field offset and the 64-bit byte/element-count len 8 bytes
    // higher. A span-returning call leaves ptr in rax and len in rdx; any other byte view
    // materializes ptr-then-len. When target is mutref<Record>, deref the stashed ptr
    // into r11 before each store and address through it.
    if (instr->value->kind == IR_VALUE_CALL) {
      if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
      if (target_is_ref) {
        machx64_emit_load_ref_record_ptr(text, fun, 11, instr->local_index);
        z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, instr->field_offset, 0, true);     // mov [r11+off], rax (ptr)
        z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, instr->field_offset + 8u, 2, true); // mov [r11+off+8], rdx (len)
      } else {
        machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, instr->field_offset, true);
        machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 2, instr->field_offset + 8u, true);
      }
      return true;
    }
    if (!machx64_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
    if (target_is_ref) {
      machx64_emit_load_ref_record_ptr(text, fun, 11, instr->local_index);
      z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, instr->field_offset, 0, true);
    } else {
      machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, instr->field_offset, true);
    }
    if (!machx64_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
    if (target_is_ref) {
      machx64_emit_load_ref_record_ptr(text, fun, 11, instr->local_index);
      z_x64_emit_store_ptr_reg_disp_from_reg(text, 11, instr->field_offset + 8u, 0, true);
    } else {
      machx64_emit_store_local_slot_from_reg(text, fun, instr->local_index, 0, instr->field_offset + 8u, true);
    }
    return true;
  }
  if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
  if (machx64_type_is_float(value_type)) machx64_emit_store_field_xmm0(text, fun, instr->local_index, instr->field_offset, value_type);
  else machx64_emit_store_field_from_eax(text, fun, instr->local_index, instr->field_offset, value_type);
  return true;
}

static bool machx64_emit_index_store_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->array_index >= fun->local_len) return machx64_diag_at(diag, "direct x86_64 Mach-O indexed store array is out of range", instr->line, instr->column, "invalid array local");
  const IrLocal *local = &fun->locals[instr->array_index];
  if (local->type == IR_TYPE_BYTE_VIEW) {
    // Typed-span element store. The value is materialized and spilled before the address is computed,
    // since computing the address clobbers rax/rcx (and may evaluate the index). Float values store
    // via movss/movsd; u8/i8/Bool write the low byte; 8-byte via 64-bit mov; else 32-bit mov.
    IrTypeKind elem = local->element_type;
    if (machx64_type_is_float(elem)) {
      bool is64 = machx64_type_is_f64(elem);
      if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
      z_x64_emit_xmm_push(text, 0);
      if (!machx64_emit_span_index_addr(text, fun, instr->array_index, instr->index, ctx, diag)) return false;
      z_x64_emit_xmm_pop(text, 0);
      z_x64_emit_movs_xmm_ptr_reg(text, 0, 0, is64, false);
      return true;
    }
    if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
    z_x64_emit_push_rax(text);
    if (!machx64_emit_span_index_addr(text, fun, instr->array_index, instr->index, ctx, diag)) return false;
    z_x64_emit_pop_reg64(text, 1); // value -> rcx; element address is in rax
    if (elem == IR_TYPE_U8 || elem == IR_TYPE_BOOL || elem == IR_TYPE_I8) z_x64_emit_store_ptr_reg8_from_reg(text, 0, 1);
    else if (machx64_type_is_i64(elem)) z_x64_emit_store_ptr_reg_from_reg(text, 0, 1, true);
    else z_x64_emit_store_ptr_reg_from_reg(text, 0, 1, false);
    return true;
  }
  bool byte_array = local->element_type == IR_TYPE_U8 || local->element_type == IR_TYPE_BOOL;
  unsigned const_index = 0;
  if (local->is_array && !byte_array && machx64_const_u32_value(instr->index, &const_index) && const_index < local->array_len) {
    if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
    machx64_emit_store_local_slot_from_reg(text, fun, instr->array_index, 0, const_index * 4u, false);
    return true;
  }
  if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
    if (!instr->index || !machx64_emit_value(text, fun, instr->index, ctx, diag)) return false;
    machx64_emit_bounds_check(text, local);
    z_x64_emit_push_rax(text);
    if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
    z_x64_emit_pop_reg64(text, 1);
    machx64_emit_array_base_rdx(text, fun, instr->array_index);
    z_x64_emit_shl_rcx_imm8(text, 2);
    z_x64_emit_add_rdx_rcx(text, true);
    z_x64_emit_store_ptr_reg_from_reg(text, 2, 0, false);
    return true;
  }
  if (!local->is_array || !byte_array) return machx64_diag_at(diag, "direct x86_64 Mach-O indexed store requires [N]u8, [N]Bool, or integer arrays", instr->line, instr->column, "unsupported array local");
  if (!instr->index || !machx64_emit_value(text, fun, instr->index, ctx, diag)) return false;
  machx64_emit_bounds_check(text, local);
  z_x64_emit_push_rax(text);
  if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_pop_reg64(text, 1);
  machx64_emit_array_base_rdx(text, fun, instr->array_index);
  z_x64_emit_add_rdx_rcx(text, true);
  z_x64_emit_store_ptr_reg8_from_reg(text, 2, 0);
  return true;
}

static bool machx64_emit_if_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_test_rax_rax(text, false);
  size_t false_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  if (!machx64_emit_instrs(text, fun, instr->then_instrs, instr->then_len, ctx, diag)) return false;
  if (instr->else_len > 0) {
    size_t end_patch = z_x64_emit_jmp32_placeholder(text, 0xe9);
    z_x64_patch_rel32(text, false_patch, text->len);
    if (!machx64_emit_instrs(text, fun, instr->else_instrs, instr->else_len, ctx, diag)) return false;
    z_x64_patch_rel32(text, end_patch, text->len);
  } else {
    z_x64_patch_rel32(text, false_patch, text->len);
  }
  return true;
}

static bool machx64_emit_while_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  size_t loop_start = text->len;
  if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
  z_x64_emit_test_rax_rax(text, false);
  size_t false_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  if (!machx64_emit_instrs(text, fun, instr->then_instrs, instr->then_len, ctx, diag)) return false;
  size_t loop_patch = z_x64_emit_jmp32_placeholder(text, 0xe9);
  z_x64_patch_rel32(text, loop_patch, loop_start);
  z_x64_patch_rel32(text, false_patch, text->len);
  return true;
}

static bool machx64_emit_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, MachOEmitContext *ctx, ZDiag *diag) {
  switch (instr->kind) {
    case IR_INSTR_WORLD_WRITE: return machx64_emit_world_write(text, fun, instr, ctx, diag);
    case IR_INSTR_LOCAL_SET: return machx64_emit_local_set_instr(text, fun, instr, ctx, diag);
    case IR_INSTR_FIELD_STORE: return machx64_emit_field_store_instr(text, fun, instr, ctx, diag);
    case IR_INSTR_INDEX_STORE: return machx64_emit_index_store_instr(text, fun, instr, ctx, diag);
    case IR_INSTR_EXPR: return !instr->value || machx64_emit_value(text, fun, instr->value, ctx, diag);
    case IR_INSTR_RETURN:
      if (fun->return_type == IR_TYPE_RECORD) {
        // Record return through the caller's sret pointer (rdi spilled at the prologue). A
        // record-returning call passes our sret pointer straight through; a record local is copied
        // byte-by-byte into the caller's buffer. `ret check f()` and `ret f() rescue r0`
        // route the inner call's sret straight into our own sret target (no temp local), with the
        // CHECK/RESCUE tag testing the second-return register rdx between the call and the return.
        const IrValue *call_for_check = NULL;
        const IrValue *rescue_fallback = NULL;
        if (instr->value && instr->value->kind == IR_VALUE_CHECK && instr->value->left && instr->value->left->kind == IR_VALUE_CALL) {
          call_for_check = instr->value->left;
        } else if (instr->value && instr->value->kind == IR_VALUE_RESCUE && instr->value->left && instr->value->left->kind == IR_VALUE_CALL) {
          call_for_check = instr->value->left;
          rescue_fallback = instr->value->right;
        }
        if (call_for_check) {
          if (!machx64_emit_record_call_with_dest(text, fun, -1, call_for_check, ctx, diag)) return false;
          if (!rescue_fallback) {
            // CHECK path: rdx carries inner call's tag. Reload rax (sret pointer) and epilogue —
            // on success rdx is 0 (set by inner callee), on failure rdx propagates.
            z_x64_emit_rbp_disp_reg(text, 0x8b, 0, machx64_sret_slot_offset(fun), true);
            machx64_emit_epilogue(text, fun, ctx);
            return true;
          }
          // RESCUE path: on success (rdx==0), reload rax and return. On failure, materialize
          // fallback into our sret target, clear rdx (we're succeeding now), then return.
          z_x64_emit_test_reg_reg(text, 2, false); // test edx, edx
          size_t fallback_patch = z_x64_emit_jcc32_placeholder(text, 0x85); // JNZ fallback
          z_x64_emit_rbp_disp_reg(text, 0x8b, 0, machx64_sret_slot_offset(fun), true);
          if (fun->raises) z_x64_emit_xor_reg_reg(text, 2, false); // xor edx, edx
          machx64_emit_epilogue(text, fun, ctx);
          z_x64_patch_rel32(text, fallback_patch, text->len);
          if (rescue_fallback->kind == IR_VALUE_LOCAL) {
            if (rescue_fallback->local_index >= fun->local_len) {
              return machx64_diag_at(diag, "direct x86_64 Mach-O record return rescue fallback local is out of range", rescue_fallback->line, rescue_fallback->column, "invalid fallback local");
            }
            machx64_emit_record_copy_to(text, fun, UINT_MAX, rescue_fallback->local_index);
          } else if (rescue_fallback->kind == IR_VALUE_CALL) {
            if (!machx64_emit_record_call_with_dest(text, fun, -1, rescue_fallback, ctx, diag)) return false;
          } else {
            return machx64_diag_at(diag, "direct x86_64 Mach-O record return rescue fallback must be a record local or call", rescue_fallback->line, rescue_fallback->column, "unsupported rescue fallback");
          }
          z_x64_emit_rbp_disp_reg(text, 0x8b, 0, machx64_sret_slot_offset(fun), true);
          if (fun->raises) z_x64_emit_xor_reg_reg(text, 2, false); // xor edx, edx
          machx64_emit_epilogue(text, fun, ctx);
          return true;
        }
        // `return f()` — the callee writes through our sret pointer and returns it in rax.
        if (instr->value && instr->value->kind == IR_VALUE_CALL) {
          if (!machx64_emit_record_call_with_dest(text, fun, -1, instr->value, ctx, diag)) return false;
          // A raising record-returning function uses rdx as the error tag carrier; clear it
          // on a successful return so callers see tag=0. (RAISE writes the error code to rdx.)
          if (fun->raises) z_x64_emit_xor_reg_reg(text, 2, false); // xor edx, edx
          machx64_emit_epilogue(text, fun, ctx);
          return true;
        }
        // `return p` — copy a record local/param through the sret pointer.
        if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
          machx64_emit_record_copy_to(text, fun, UINT_MAX, instr->value->local_index);
        }
        // `return <shape literal>` had its fields already stored through sret (FIELD_STORE
        // UINT_MAX). Hand the sret pointer back in rax either way.
        z_x64_emit_rbp_disp_reg(text, 0x8b, 0, machx64_sret_slot_offset(fun), true);
        if (fun->raises) z_x64_emit_xor_reg_reg(text, 2, false); // xor edx, edx
        machx64_emit_epilogue(text, fun, ctx);
        return true;
      }
      if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
        // Span return: ptr in rax, len in rdx. A span-returning call already lands both there;
        // any other byte view is materialized ptr-then-len.
        if (instr->value->kind == IR_VALUE_CALL) {
          if (!machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
        } else {
          if (!machx64_emit_byte_view_ptr(text, fun, instr->value, ctx, diag)) return false;
          z_x64_emit_push_rax(text);
          if (!machx64_emit_byte_view_len(text, fun, instr->value, ctx, diag)) return false;
          z_x64_emit_mov_reg_from_reg(text, 2, 0, true);   // mov rdx, rax (len)
          z_x64_emit_pop_reg64(text, 0);                   // pop rax (ptr)
        }
        machx64_emit_epilogue(text, fun, ctx);
        return true;
      }
      if (instr->value && !machx64_emit_value(text, fun, instr->value, ctx, diag)) return false;
      if (fun->raises && !instr->value) z_x64_emit_xor_rax_rax(text);
      else if (fun->raises && instr->value && !machx64_type_is_i64(instr->value->type)) z_x64_emit_mov_reg_from_reg(text, 0, 0, false);
      machx64_emit_epilogue(text, fun, ctx);
      return true;
    case IR_INSTR_RAISE:
      if (!machx64_function_propagates_to_process_exit(fun)) return machx64_diag_at(diag, "direct x86_64 Mach-O raise requires a fallible function context", instr->line, instr->column, "non-fallible context");
      // A raising record-returning function carries the error tag in the low 32 bits of rdx
      // (rax is reserved for the sret pointer per System V x64). Every other raising function packs
      // the tag into the high 32 bits of rax via the packed-tag scheme.
      if (fun->return_type == IR_TYPE_RECORD) {
        z_x64_emit_mov_reg_u32(text, 2, instr->error_code ? instr->error_code : IR_ERROR_UNKNOWN); // mov edx, code
        // rax still must carry the sret pointer; reload it from the saved slot to be safe.
        z_x64_emit_rbp_disp_reg(text, 0x8b, 0, machx64_sret_slot_offset(fun), true);
      } else {
        machx64_emit_packed_error_rax(text, instr->error_code ? instr->error_code : IR_ERROR_UNKNOWN);
      }
      machx64_emit_epilogue(text, fun, ctx);
      return true;
    case IR_INSTR_IF: return machx64_emit_if_instr(text, fun, instr, ctx, diag);
    case IR_INSTR_WHILE: return machx64_emit_while_instr(text, fun, instr, ctx, diag);
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported instruction kind %d", instr ? (int)instr->kind : -1);
      return machx64_diag_at(diag, "direct x86_64 Mach-O instruction kind is unsupported", instr ? instr->line : 1, instr ? instr->column : 1, actual);
    }
  }
}

static bool machx64_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, MachOEmitContext *ctx, ZDiag *diag) {
  for (size_t i = 0; i < len; i++) {
    if (!machx64_emit_instr(text, fun, &instrs[i], ctx, diag)) return false;
  }
  return true;
}

static bool machx64_validate_function(const IrFunction *fun, ZDiag *diag) {
  // System V x64 splits int and float into separate register pools: floats go into XMM0-7, not GPRs.
  // A record param consumes one GPR slot (passed by pointer); a record return reserves param_regs[0]
  // for the caller-allocated sret pointer (rdi).
  size_t abi_slots = 0;
  for (size_t i = 0; i < fun->param_count; i++) {
    IrTypeKind t = fun->locals[i].type;
    if (machx64_type_is_float(t)) continue;
    if (t == IR_TYPE_BYTE_VIEW) abi_slots += 2u;
    else abi_slots += 1u;
  }
  if (abi_slots > 6) return machx64_diag_at(diag, "direct x86_64 Mach-O object backend supports at most six ABI parameter slots", fun->line, fun->column, fun->name);
  // Returns: Void, primitive integer, float, a record (via rdi sret pointer), or a span (ptr in rax,
  // len in rdx). A raising function can return a record — the record flows via sret as usual,
  // and the error tag rides in the low 32 bits of rdx (free for record-returning calls). A span-
  // returning raising function is still rejected since rdx IS the span len carrier.
  if (fun->return_type != IR_TYPE_VOID && fun->return_type != IR_TYPE_RECORD && fun->return_type != IR_TYPE_BYTE_VIEW &&
      !machx64_type_is_supported_scalar(fun->return_type) && !machx64_type_is_float(fun->return_type)) {
    return machx64_diag_at(diag, "direct x86_64 Mach-O object backend currently supports only Void, numeric, record, and span returns", fun->line, fun->column, fun->name);
  }
  if (fun->raises && fun->return_type == IR_TYPE_BYTE_VIEW) {
    return machx64_diag_at(diag, "direct x86_64 Mach-O object backend cannot return a span from a raising function", fun->line, fun->column, fun->name);
  }
  for (size_t i = 0; i < fun->local_len; i++) {
    if (fun->locals[i].type == IR_TYPE_BYTE_VIEW) continue;
    // FixedBufAlloc + Maybe<byte-view> + Maybe<scalar> locals — laid out and updated by the
    // alloc/maybe emit helpers. PageAlloc (the same IR_TYPE_ALLOC but is_page_alloc=true) routes
    // through the emit-helper diagnostic; the slot reserve itself works the same.
    if (fun->locals[i].type == IR_TYPE_ALLOC || fun->locals[i].type == IR_TYPE_MAYBE_BYTE_VIEW || fun->locals[i].type == IR_TYPE_MAYBE_SCALAR) continue;
    if (fun->locals[i].is_array && (fun->locals[i].element_type == IR_TYPE_BOOL || fun->locals[i].element_type == IR_TYPE_U8 || fun->locals[i].element_type == IR_TYPE_U32 || fun->locals[i].element_type == IR_TYPE_I32 || fun->locals[i].element_type == IR_TYPE_USIZE)) continue;
    if (fun->locals[i].is_record) continue;
    if (!fun->locals[i].is_array && machx64_type_is_float(fun->locals[i].type)) continue;
    if (fun->locals[i].is_array || !machx64_type_is_supported_scalar(fun->locals[i].type)) {
      return machx64_diag_at(diag, "direct x86_64 Mach-O object backend currently supports only primitive scalar locals", fun->locals[i].line, fun->locals[i].column, fun->locals[i].name);
    }
  }
  return true;
}

static bool machx64_emit_function_text(ZBuf *text, const IrFunction *fun, MachOEmitContext *ctx, ZDiag *diag) {
  static const unsigned param_regs[] = {7, 6, 2, 1, 8, 9};
  // Reserve an extra 16-byte slot when the function returns a record, so the prologue can park the
  // caller's sret pointer (rdi) at [rbp - sret_slot_offset]. The slot stays 16-byte aligned. For
  // main when seeding process args we reserve 32 bytes past base for the caller-saved r13/r14/r15
  // trio; main never returns a record (validator rejects), so the two paths don't overlap.
  bool seed_process_args = machx64_function_seeds_process_args(fun, ctx);
  unsigned base_frame = machx64_base_stack_size(fun);
  unsigned frame_size = base_frame +
                        (machx64_returns_record(fun) ? 16u : 0u) +
                        (seed_process_args ? 32u : 0u);
  z_x64_emit_prologue(text, frame_size);
  if (seed_process_args) {
    // Spill the caller's r13/r14/r15 just past the locals, then shuffle the System V argument
    // registers (rdi=argc, rsi=argv, rdx=envp) into those callee-saved registers so the body
    // (std.args.{len,get}) can recover them after arbitrary intervening calls. push/pop is the
    // shortest reg→reg move that survives because rdi/rsi/rdx are caller-saved.
    z_x64_emit_rbp_disp_reg(text, 0x89, 13, base_frame + 8, true);
    z_x64_emit_rbp_disp_reg(text, 0x89, 14, base_frame + 16, true);
    z_x64_emit_rbp_disp_reg(text, 0x89, 15, base_frame + 24, true);
    z_x64_emit_push_reg64(text, 7);   // push rdi (argc)
    z_x64_emit_pop_reg64(text, 14);   // r14 = argc
    z_x64_emit_push_reg64(text, 6);   // push rsi (argv)
    z_x64_emit_pop_reg64(text, 15);   // r15 = argv
    z_x64_emit_push_reg64(text, 2);   // push rdx (envp)
    z_x64_emit_pop_reg64(text, 13);   // r13 = envp
  }
  size_t abi_slot = 0;   // GPR bank index (param_regs)
  size_t float_idx = 0;  // SSE bank index (xmm0..7), counted independently
  if (machx64_returns_record(fun)) {
    // Save the caller-supplied sret pointer; param_regs[0] (rdi) is now consumed.
    z_x64_emit_rbp_disp_reg(text, 0x89, 7, machx64_sret_slot_offset(fun), true);
    abi_slot = 1;
  }
  for (size_t i = 0; i < fun->param_count; i++) {
    if (machx64_type_is_float(fun->locals[i].type)) {
      // System V: a float param arrives in an XMM register; spill it to its home slot.
      z_x64_emit_movs_xmm_rbp_disp(text, (unsigned)float_idx++, -(int32_t)machx64_local_offset(fun, (unsigned)i), machx64_type_is_f64(fun->locals[i].type), false);
      continue;
    }
    if (fun->locals[i].type == IR_TYPE_RECORD) {
      // Record arg: passed by pointer in one GPR slot.
      if (abi_slot >= 6) return machx64_diag_at(diag, "direct x86_64 Mach-O function has too many ABI argument slots", fun->line, fun->column, fun->name);
      if (fun->locals[i].is_ref) {
        // ref<Record> / mutref<Record> param — stash the pointer in slot 0 of the local
        // frame slot. Field load/store dereference through it; no inline byte copy (that's the
        // only behavioral difference from the by-value record param case below).
        machx64_emit_store_local_slot_from_reg(text, fun, (unsigned)i, param_regs[abi_slot], 0, true);
      } else {
        // By-value record arg: copy the pointed-to bytes into the inline frame for value semantics.
        machx64_emit_copy_record_param(text, fun, (unsigned)i, param_regs[abi_slot]);
      }
      abi_slot += 1;
      continue;
    }
    if (fun->locals[i].type == IR_TYPE_BYTE_VIEW) {
      machx64_emit_store_local_slot_from_reg(text, fun, (unsigned)i, param_regs[abi_slot++], 0, true);
      machx64_emit_store_local_slot_from_reg(text, fun, (unsigned)i, param_regs[abi_slot++], 8, true);
      continue;
    }
    machx64_emit_store_local_from_reg(text, fun, (unsigned)i, param_regs[abi_slot++]);
  }
  if (!machx64_emit_instrs(text, fun, fun->instrs, fun->instr_len, ctx, diag)) return false;
  if (fun->instr_len == 0 || (fun->instrs[fun->instr_len - 1].kind != IR_INSTR_RETURN && fun->instrs[fun->instr_len - 1].kind != IR_INSTR_RAISE)) machx64_emit_epilogue(text, fun, ctx);
  return true;
}

static unsigned machx64_rodata_base_offset(const IrProgram *program) {
  if (!program || program->data_segment_len == 0) return 0;
  unsigned base = program->data_segments[0].offset;
  for (size_t i = 1; i < program->data_segment_len; i++) {
    if (program->data_segments[i].offset < base) base = program->data_segments[i].offset;
  }
  return base;
}

static void machx64_append_rodata(ZBuf *rodata, const IrProgram *program, unsigned base_offset) {
  for (size_t i = 0; program && i < program->data_segment_len; i++) {
    const IrDataSegment *segment = &program->data_segments[i];
    while (rodata->len < segment->offset - base_offset) machx64_append_u8(rodata, 0);
    machx64_append_bytes(rodata, (const char *)segment->bytes, segment->len);
  }
}

static void machx64_patch_object_data_refs(ZBuf *text, const MachOEmitContext *ctx) {
  uint32_t const_addr = (uint32_t)machx64_align(text ? text->len : 0, 8);
  for (size_t i = 0; ctx && i < ctx->data_patch_len; i++) {
    const MachODataPatch *patch = &ctx->data_patches[i];
    int64_t displacement = (int64_t)const_addr + (int64_t)(patch->data_offset - ctx->rodata_base_offset) - (int64_t)(patch->patch_offset + 4u);
    z_x64_patch_u32(text, patch->patch_offset, (uint32_t)(int32_t)displacement);
  }
}

static void machx64_append_reloc(ZBuf *relocs, uint32_t address, uint32_t symbol_or_section, bool pcrel, unsigned length, bool external, unsigned type) {
  uint32_t reloc_info = (symbol_or_section & 0x00ffffffu) |
                        ((pcrel ? 1u : 0u) << 24) |
                        ((length & 3u) << 25) |
                        ((external ? 1u : 0u) << 27) |
                        ((type & 15u) << 28);
  z_macho_append_u32(relocs, address);
  z_macho_append_u32(relocs, reloc_info);
}

static void machx64_append_data_relocations(ZBuf *relocs, const MachOEmitContext *ctx) {
  for (size_t i = 0; ctx && i < ctx->data_patch_len; i++) {
    machx64_append_reloc(relocs, (uint32_t)ctx->data_patches[i].patch_offset, 2u, true, 2, false, 1);
  }
}

static size_t machx64_text_relocation_count(const MachOEmitContext *ctx) {
  if (!ctx) return 0;
  size_t count = ctx->call_patch_len + ctx->data_patch_len;
  for (unsigned i = 0; i < MACHO_RUNTIME_HELPER_COUNT; i++) count += ctx->runtime_patches[i].len;
  count += z_macho_math_call_patch_count(ctx);
  count += z_macho_libc_call_patch_count(ctx);
  return count;
}

typedef struct {
  ZBuf text;
  ZBuf rodata;
  ZBuf relocs;
  ZBuf strings;
  size_t *offsets;
  uint32_t *function_string_offsets;
  uint32_t runtime_string_offsets[MACHO_RUNTIME_HELPER_COUNT];
  uint32_t math_string_offsets[Z_MACHO_MATH_COUNT];
  uint32_t libc_string_offsets[Z_MACHO_LIBC_COUNT];
  uint32_t rodata_string_offset;
  ZMachOSymbol *symbols;
  size_t symbol_len;
  uint32_t symbol_count;
  MachOEmitContext ctx;
  unsigned rodata_base_offset;
  bool has_rodata;
} MachX64ObjectBuild;

static void machx64_object_build_free(MachX64ObjectBuild *build) {
  if (!build) return;
  z_macho_emit_context_free(&build->ctx);
  free(build->symbols);
  free(build->function_string_offsets);
  free(build->offsets);
  zbuf_free(&build->strings);
  zbuf_free(&build->relocs);
  zbuf_free(&build->rodata);
  zbuf_free(&build->text);
}

static bool machx64_validate_object_program(const IrProgram *program, ZDiag *diag) {
  if (!program) return machx64_diag(diag, "direct x86_64 Mach-O backend received no program");
  if (!program->mir_valid) {
    bool ok = machx64_diag_at(diag, program->mir_message[0] ? program->mir_message : "direct backend lowering failed", program->mir_line, program->mir_column, program->mir_actual);
    z_diag_set_backend_blocker(diag, &program->backend_blocker);
    return ok;
  }
  if (program->function_len == 0) return machx64_diag_at(diag, "direct x86_64 Mach-O object backend requires at least one exported function", 1, 1, "empty program");
  bool has_export = false;
  for (size_t i = 0; i < program->function_len; i++) {
    if (program->functions[i].is_exported) has_export = true;
    if (!machx64_validate_function(&program->functions[i], diag)) return false;
  }
  if (!has_export) return machx64_diag_at(diag, "direct x86_64 Mach-O object backend requires at least one exported function", 1, 1, "no exported function");
  return true;
}

static bool machx64_object_build_init(MachX64ObjectBuild *build, const IrProgram *program, ZDiag *diag) {
  memset(build, 0, sizeof(*build));
  zbuf_init(&build->text);
  zbuf_init(&build->rodata);
  zbuf_init(&build->relocs);
  zbuf_init(&build->strings);
  machx64_append_u8(&build->strings, 0);
  build->has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  build->rodata_base_offset = machx64_rodata_base_offset(program);
  if (build->has_rodata) machx64_append_rodata(&build->rodata, program, build->rodata_base_offset);
  build->offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  build->function_string_offsets = z_checked_calloc(program->function_len, sizeof(uint32_t));
  if (!build->offsets || !build->function_string_offsets) {
    machx64_object_build_free(build);
    return machx64_diag(diag, "out of memory while emitting x86_64 Mach-O object");
  }
  build->ctx = (MachOEmitContext){
    .program = program,
    .function_offsets = build->offsets,
    .function_count = program->function_len,
    .rodata_base_offset = build->rodata_base_offset,
    // obj+link route — macOS crt0 calls main(argc, argv, envp, apple) under the SysV
    // C-ABI, same as the direct-exe LC_MAIN path. Seed argc/argv/envp from rdi/rsi/rdx into
    // r14/r15/r13 in main's prologue so std.args.{len,get} can recover them; mirrors the obj-
    // path seeding in the elf64 sibling.
    .seed_main_process_args = true
  };
  return true;
}

static bool machx64_object_emit_functions(MachX64ObjectBuild *build, const IrProgram *program, ZDiag *diag) {
  for (size_t i = 0; i < program->function_len; i++) {
    machx64_pad_to(&build->text, machx64_align(build->text.len, 16));
    build->offsets[i] = build->text.len;
    if (!machx64_emit_function_text(&build->text, &program->functions[i], &build->ctx, diag)) return false;
    build->function_string_offsets[i] = (uint32_t)build->strings.len;
    zbuf_append_char(&build->strings, '_');
    zbuf_append(&build->strings, program->functions[i].name ? program->functions[i].name : "zero_fn");
    machx64_append_u8(&build->strings, 0);
  }
  return true;
}

static void machx64_object_append_relocations(MachX64ObjectBuild *build, const IrProgram *program) {
  machx64_patch_object_data_refs(&build->text, &build->ctx);
  z_macho_append_call_relocations(&build->relocs, &build->ctx);
  if (build->has_rodata) machx64_append_data_relocations(&build->relocs, &build->ctx);
  uint32_t next_symbol = (uint32_t)program->function_len + (build->has_rodata ? 1u : 0u);
  for (unsigned helper = 0; helper < MACHO_RUNTIME_HELPER_COUNT; helper++) {
    MachORuntimeHelper runtime_helper = (MachORuntimeHelper)helper;
    if (z_macho_runtime_patch_count(&build->ctx, runtime_helper) == 0) continue;
    z_macho_append_runtime_relocations(&build->relocs, &build->ctx, runtime_helper, next_symbol++);
  }
  // libm externals follow the runtime helpers in symbol-table order; the same Z_MACHO_MATH_* order
  // is reused by the strtab/nlist construction below to keep symbol indices consistent.
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

static void machx64_object_append_symbol_strings(MachX64ObjectBuild *build) {
  if (build->has_rodata) {
    build->rodata_string_offset = (uint32_t)build->strings.len;
    zbuf_append(&build->strings, "l_.zero_rodata");
    machx64_append_u8(&build->strings, 0);
  }
  for (unsigned helper = 0; helper < MACHO_RUNTIME_HELPER_COUNT; helper++) {
    MachORuntimeHelper runtime_helper = (MachORuntimeHelper)helper;
    if (z_macho_runtime_patch_count(&build->ctx, runtime_helper) == 0) continue;
    build->runtime_string_offsets[helper] = (uint32_t)build->strings.len;
    zbuf_append(&build->strings, z_macho_runtime_helper_symbol(runtime_helper));
    machx64_append_u8(&build->strings, 0);
  }
  for (unsigned m = 0; m < Z_MACHO_MATH_COUNT; m++) {
    MachOMathSymbol symbol = (MachOMathSymbol)m;
    if (!z_macho_math_symbol_used(&build->ctx, symbol)) continue;
    build->math_string_offsets[m] = (uint32_t)build->strings.len;
    zbuf_append(&build->strings, z_macho_math_symbol_name(symbol));
    machx64_append_u8(&build->strings, 0);
  }
  for (unsigned c = 0; c < Z_MACHO_LIBC_COUNT; c++) {
    MachOLibcSymbol symbol = (MachOLibcSymbol)c;
    if (!z_macho_libc_symbol_used(&build->ctx, symbol)) continue;
    build->libc_string_offsets[c] = (uint32_t)build->strings.len;
    zbuf_append(&build->strings, z_macho_libc_symbol_name(symbol));
    machx64_append_u8(&build->strings, 0);
  }
}

static bool machx64_object_build_symbols(MachX64ObjectBuild *build, const IrProgram *program, ZDiag *diag) {
  build->symbols = z_checked_calloc(build->symbol_count, sizeof(ZMachOSymbol));
  if (!build->symbols) return machx64_diag(diag, "out of memory while emitting x86_64 Mach-O symbols");
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
      .value = (uint32_t)machx64_align(build->text.len, 8)
    };
  }
  for (unsigned helper = 0; helper < MACHO_RUNTIME_HELPER_COUNT; helper++) {
    MachORuntimeHelper runtime_helper = (MachORuntimeHelper)helper;
    if (z_macho_runtime_patch_count(&build->ctx, runtime_helper) == 0) continue;
    build->symbols[build->symbol_len++] = (ZMachOSymbol){.string_offset = build->runtime_string_offsets[helper], .type = 0x01};
  }
  // libm externals (sqrtf/expf/...), emitted in symbol-index order to match the relocations.
  for (unsigned m = 0; m < Z_MACHO_MATH_COUNT; m++) {
    MachOMathSymbol symbol = (MachOMathSymbol)m;
    if (!z_macho_math_symbol_used(&build->ctx, symbol)) continue;
    build->symbols[build->symbol_len++] = (ZMachOSymbol){.string_offset = build->math_string_offsets[m], .type = 0x01};
  }
  // libSystem externals (mmap/munmap/open/lseek/close), in symbol-index order after the libm set.
  for (unsigned c = 0; c < Z_MACHO_LIBC_COUNT; c++) {
    MachOLibcSymbol symbol = (MachOLibcSymbol)c;
    if (!z_macho_libc_symbol_used(&build->ctx, symbol)) continue;
    build->symbols[build->symbol_len++] = (ZMachOSymbol){.string_offset = build->libc_string_offsets[c], .type = 0x01};
  }
  return true;
}

static void machx64_object_write(const MachX64ObjectBuild *build, ZBuf *out) {
  ZMachOObjectImage image = {
    .cpu = Z_MACHO_CPU_X86_64,
    .text = &build->text,
    .rodata = build->has_rodata ? &build->rodata : NULL,
    .relocs = &build->relocs,
    .strings = &build->strings,
    .symbols = build->symbols,
    .symbol_len = build->symbol_len,
    .text_reloc_count = (uint32_t)machx64_text_relocation_count(&build->ctx)
  };
  z_macho_write_object64(out, &image);
}

bool z_emit_macho_x64_object_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!out) return machx64_diag(diag, "direct x86_64 Mach-O backend received no output buffer");
  if (!machx64_validate_object_program(program, diag)) return false;
  MachX64ObjectBuild build;
  if (!machx64_object_build_init(&build, program, diag)) return false;
  bool ok = machx64_object_emit_functions(&build, program, diag);
  if (ok) {
    machx64_object_append_relocations(&build, program);
    machx64_object_append_symbol_strings(&build);
    ok = machx64_object_build_symbols(&build, program, diag);
  }
  if (ok) machx64_object_write(&build, out);
  machx64_object_build_free(&build);
  return ok;
}

static const IrFunction *machx64_find_executable_main(const IrProgram *program, ZDiag *diag, unsigned *out_index) {
  const IrFunction *fun = NULL;
  unsigned index = 0;
  for (size_t i = 0; program && i < program->function_len; i++) {
    if (program->functions[i].is_exported && program->functions[i].name && strcmp(program->functions[i].name, "main") == 0) {
      if (fun) {
        machx64_diag_at(diag, "direct x86_64 Mach-O executable backend requires exactly one exported main function", program->functions[i].line, program->functions[i].column, program->functions[i].name);
        return NULL;
      }
      fun = &program->functions[i];
      index = (unsigned)i;
    }
  }
  if (!fun) {
    machx64_diag_at(diag, "direct x86_64 Mach-O executable backend requires an exported main function", 1, 1, "missing main");
    return NULL;
  }
  if (fun->param_count != 0) {
    machx64_diag_at(diag, "direct x86_64 Mach-O executable main must not take parameters", fun->line, fun->column, fun->name);
    return NULL;
  }
  if (fun->return_type != IR_TYPE_VOID && !machx64_type_is_scalar32(fun->return_type)) {
    machx64_diag_at(diag, "direct x86_64 Mach-O executable main must return Void or a 32-bit-or-smaller scalar", fun->line, fun->column, fun->name);
    return NULL;
  }
  if (out_index) *out_index = index;
  return fun;
}

typedef struct {
  ZBuf text;
  ZBuf rodata;
  ZBuf rebase;
  size_t *offsets;
  size_t start_call_patch;
  ZMachOExecutableLayout layout;
  MachOEmitContext ctx;
  unsigned main_index;
  unsigned rodata_base_offset;
  bool has_rodata;
} MachX64ExeBuild;

static size_t machx64_emit_exe_start_stub(ZBuf *text) {
  z_x64_emit_sub_rsp(text, 8);
  size_t patch = z_x64_emit_call32_placeholder(text);
  z_x64_emit_add_rsp(text, 8);
  machx64_emit_error_condition_from_rax(text);
  size_t success_patch = z_x64_emit_jcc32_placeholder(text, 0x84);
  z_x64_emit_mov_eax_u32(text, 1);
  z_x64_append_u8(text, 0xc3);
  z_x64_patch_rel32(text, success_patch, text->len);
  z_x64_append_u8(text, 0xc3);
  return patch;
}

static size_t machx64_emit_exe_world_write(ZBuf *text) {
  size_t offset = text->len;
  z_x64_emit_mov_eax_u32(text, 0x02000004u);
  z_x64_emit_syscall(text);
  z_x64_emit_xor_eax_eax(text);
  z_x64_append_u8(text, 0xc3);
  return offset;
}

static bool machx64_validate_exe_program(const IrProgram *program, unsigned *main_index, ZDiag *diag) {
  if (!program) return machx64_diag(diag, "direct x86_64 Mach-O executable backend received no program");
  if (!program->mir_valid) {
    bool ok = machx64_diag_at(diag, program->mir_message[0] ? program->mir_message : "direct backend lowering failed", program->mir_line, program->mir_column, program->mir_actual);
    z_diag_set_backend_blocker(diag, &program->backend_blocker);
    return ok;
  }
  if (!machx64_find_executable_main(program, diag, main_index)) return false;
  for (size_t i = 0; i < program->function_len; i++) if (!machx64_validate_function(&program->functions[i], diag)) return false;
  return true;
}

static void machx64_exe_build_free(MachX64ExeBuild *build) {
  if (!build) return;
  z_macho_emit_context_free(&build->ctx);
  free(build->offsets);
  zbuf_free(&build->rebase);
  zbuf_free(&build->rodata);
  zbuf_free(&build->text);
}

static bool machx64_exe_build_init(MachX64ExeBuild *build, const IrProgram *program, unsigned main_index, ZDiag *diag) {
  memset(build, 0, sizeof(*build));
  zbuf_init(&build->text);
  zbuf_init(&build->rodata);
  zbuf_init(&build->rebase);
  build->main_index = main_index;
  build->has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  build->rodata_base_offset = machx64_rodata_base_offset(program);
  if (build->has_rodata) machx64_append_rodata(&build->rodata, program, build->rodata_base_offset);
  build->offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  if (!build->offsets) {
    machx64_exe_build_free(build);
    return machx64_diag(diag, "out of memory while emitting x86_64 Mach-O executable");
  }
  build->ctx = (MachOEmitContext){
    .program = program,
    .function_offsets = build->offsets,
    .function_count = program->function_len,
    .rodata_base_offset = build->rodata_base_offset,
    // dyld invokes LC_MAIN's entry point with the SysV C-ABI shape (rdi=argc, rsi=argv,
    // rdx=envp, rcx=apple). The start stub below only does `sub rsp, 8; call <main>` — neither
    // instruction touches rdi/rsi/rdx — so main's prologue still sees the live argument registers
    // and can seed r14/r15/r13 from them for the std.args.{len,get} body lowerings.
    .seed_main_process_args = true
  };
  build->start_call_patch = machx64_emit_exe_start_stub(&build->text);
  machx64_pad_to(&build->text, machx64_align(build->text.len, 16));
  return true;
}

static bool machx64_exe_emit_functions(MachX64ExeBuild *build, const IrProgram *program, ZDiag *diag) {
  for (size_t i = 0; i < program->function_len; i++) {
    machx64_pad_to(&build->text, machx64_align(build->text.len, 16));
    build->offsets[i] = build->text.len;
    if (!machx64_emit_function_text(&build->text, &program->functions[i], &build->ctx, diag)) return false;
  }
  return true;
}

static bool machx64_exe_validate_runtime(const MachX64ExeBuild *build, ZDiag *diag) {
  return !z_macho_has_unsupported_exe_runtime_patches(&build->ctx) || machx64_diag_at(diag, "direct x86_64 Mach-O executable runtime helpers require object emission and an explicit runtime link step", 1, 1, "use --emit obj and link zero_runtime.c");
}

static void machx64_exe_patch_branches(MachX64ExeBuild *build) {
  size_t world_write_offset = 0;
  if (z_macho_runtime_patch_count(&build->ctx, MACHO_RUNTIME_WORLD_WRITE) > 0) {
    machx64_pad_to(&build->text, machx64_align(build->text.len, 16));
    world_write_offset = machx64_emit_exe_world_write(&build->text);
  }
  z_x64_patch_rel32(&build->text, build->start_call_patch, build->offsets[build->main_index]);
  for (size_t i = 0; i < build->ctx.call_patch_len; i++) {
    const MachOCallPatch *patch = &build->ctx.call_patches[i];
    z_x64_patch_rel32(&build->text, patch->patch_offset, build->offsets[patch->callee_index]);
  }
  const MachOPatchList *world_write_patches = z_macho_runtime_patch_list(&build->ctx, MACHO_RUNTIME_WORLD_WRITE);
  for (size_t i = 0; world_write_patches && i < world_write_patches->len; i++) {
    z_x64_patch_rel32(&build->text, world_write_patches->items[i].patch_offset, world_write_offset);
  }
}

static void machx64_exe_patch_data(MachX64ExeBuild *build, const char *code_signature_id) {
  z_macho_compute_executable64_layout(&build->layout, &build->text, build->has_rodata ? &build->rodata : NULL, &build->rebase, code_signature_id);
  for (size_t i = 0; i < build->ctx.data_patch_len; i++) {
    const MachODataPatch *patch = &build->ctx.data_patches[i];
    uint64_t target = build->layout.base_addr + build->layout.rodata_offset + (patch->data_offset - build->rodata_base_offset);
    uint64_t source_next = build->layout.base_addr + build->layout.text_offset + patch->patch_offset + 4u;
    int64_t displacement = (int64_t)target - (int64_t)source_next;
    z_x64_patch_u32(&build->text, patch->patch_offset, (uint32_t)(int32_t)displacement);
  }
  z_macho_compute_executable64_layout(&build->layout, &build->text, build->has_rodata ? &build->rodata : NULL, &build->rebase, code_signature_id);
}

static void machx64_exe_write(const MachX64ExeBuild *build, ZBuf *out, const char *code_signature_id) {
  ZMachOExecutableImage image = {
    .cpu = Z_MACHO_CPU_X86_64,
    .text = &build->text,
    .rodata = build->has_rodata ? &build->rodata : NULL,
    .rebase = &build->rebase,
    .layout = build->layout,
    .code_signature_id = code_signature_id
  };
  z_macho_write_executable64(out, &image);
}

bool z_emit_macho_x64_exe_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!program || !out) return machx64_diag(diag, "direct x86_64 Mach-O executable backend received no program");
  unsigned main_index = 0;
  if (!machx64_validate_exe_program(program, &main_index, diag)) return false;
  MachX64ExeBuild build;
  if (!machx64_exe_build_init(&build, program, main_index, diag)) return false;
  bool ok = machx64_exe_emit_functions(&build, program, diag) && machx64_exe_validate_runtime(&build, diag);
  if (ok) {
    const char *code_signature_id = "zero-direct-x64";
    machx64_exe_patch_branches(&build);
    machx64_exe_patch_data(&build, code_signature_id);
    machx64_exe_write(&build, out, code_signature_id);
  }
  machx64_exe_build_free(&build);
  return ok;
}
