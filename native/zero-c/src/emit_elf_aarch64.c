#include "zero.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Direct AArch64 ELF backend. Two paths share the codegen helpers, mirroring emit_elf64.c's
// object/direct-exe split:
//   * z_emit_elf_aarch64_object_from_ir — a relocatable ELF object (the obj+link path, for libm).
//   * z_emit_elf_aarch64_exe_from_ir   — a self-contained executable using raw Linux `svc` syscalls
//     for I/O and exit, needing no relocations (the runtime gate this host exercises in docker).
// The arm64 instruction ENCODING is identical to the committed Mach-O arm64 backend; the leaf
// encoders below are ported from emit_macho64.c under the `a64_` prefix. What differs from Mach-O is
// the container (ELF), the OS interface (Linux `svc #0`, not libSystem), and string addressing
// (absolute from the fixed non-PIE load base, so the direct-exe path needs no relocations). The
// backend is HONEST: it emits correct code for the constructs it supports and bails CGEN004 with a
// precise message for everything else (floating point, libm, typed spans, aggregates, mmap, …),
// never silently miscompiling — exactly like emit_macho64.c / emit_elf64.c.

static void a64_append_u8(ZBuf *buf, unsigned value) {
  zbuf_append_char(buf, (char)(value & 0xff));
}

static void a64_append_u16(ZBuf *buf, uint16_t value) {
  a64_append_u8(buf, value);
  a64_append_u8(buf, value >> 8);
}

static void a64_append_u32(ZBuf *buf, uint32_t value) {
  a64_append_u8(buf, value);
  a64_append_u8(buf, value >> 8);
  a64_append_u8(buf, value >> 16);
  a64_append_u8(buf, value >> 24);
}

static void a64_append_u64(ZBuf *buf, uint64_t value) {
  a64_append_u32(buf, (uint32_t)value);
  a64_append_u32(buf, (uint32_t)(value >> 32));
}

static void a64_append_bytes(ZBuf *buf, const unsigned char *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) a64_append_u8(buf, bytes[i]);
}

static void a64_append_zeros(ZBuf *buf, size_t len) {
  for (size_t i = 0; i < len; i++) a64_append_u8(buf, 0);
}

static size_t a64_align(size_t value, size_t alignment) {
  size_t remainder = alignment ? value % alignment : 0;
  return remainder == 0 ? value : value + (alignment - remainder);
}

static void a64_pad_to(ZBuf *buf, size_t offset) {
  while (buf->len < offset) a64_append_u8(buf, 0);
}

// One 32-bit AArch64 instruction word, little-endian.
static void a64_emit_word(ZBuf *text, uint32_t word) {
  a64_append_u32(text, word);
}

static bool a64_diag(ZDiag *diag, const char *message, int line, int column, const char *actual) {
  if (diag) {
    diag->code = 4004;
    diag->line = line > 0 ? line : 1;
    diag->column = column > 0 ? column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct AArch64 ELF supported subset");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "unsupported construct");
    snprintf(diag->help, sizeof(diag->help), "use a supported direct target, or restrict this program to the integer/control-flow/world.write subset the AArch64 ELF backend currently implements");
  }
  return false;
}

static bool a64_diag_at(ZDiag *diag, const char *message, int line, int column, const char *actual) {
  return a64_diag(diag, message, line, column, actual);
}

// ---------------------------------------------------------------------------------------------
// Type predicates (ported from emit_macho64.c). The integer register file is 32-bit (Wd) for
// everything that fits in 32 bits and 64-bit (Xd) for i64/u64. usize is 64-bit on this target but
// the IR carries it as a scalar that the existing helpers address with the 32-bit form when used as
// an index/length, matching the Mach-O backend.
static bool a64_type_is_float(IrTypeKind type) {
  return type == IR_TYPE_F32 || type == IR_TYPE_F64;
}

static bool a64_type_is_f64(IrTypeKind type) {
  return type == IR_TYPE_F64;
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
  return type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_U32 || type == IR_TYPE_USIZE || type == IR_TYPE_U64;
}

// ---------------------------------------------------------------------------------------------
// AArch64 leaf encoders (ported byte-identical from emit_macho64.c). Each emits one 32-bit word.

static void a64_emit_movz_w(ZBuf *text, unsigned reg, uint32_t literal) {
  a64_emit_word(text, 0x52800000u | ((literal & 0xffffu) << 5) | (reg & 31u));
  if (literal > 0xffffu) {
    a64_emit_word(text, 0x72a00000u | (((literal >> 16) & 0xffffu) << 5) | (reg & 31u));
  }
}

static void a64_emit_movz_x(ZBuf *text, unsigned reg, uint32_t literal) {
  a64_emit_word(text, 0xd2800000u | ((literal & 0xffffu) << 5) | (reg & 31u));
  if (literal > 0xffffu) {
    a64_emit_word(text, 0xf2a00000u | (((literal >> 16) & 0xffffu) << 5) | (reg & 31u));
  }
}

// MOVN Xd, #imm16 — move the bitwise-NOT of the (zero-extended) immediate. MOVN reg, #0 materializes
// the all-ones value -1, used for the mmap fd argument (fd = -1 for an anonymous mapping).
static void a64_emit_movn_x(ZBuf *text, unsigned reg, uint32_t literal) {
  a64_emit_word(text, 0x92800000u | ((literal & 0xffffu) << 5) | (reg & 31u));
}

// Emit a raw Linux syscall: the syscall number goes in x8 (movz), then `svc #0`. The kernel returns
// the result (or a negative errno) in x0 and clobbers x0..x5 + x8 (the AArch64 Linux syscall ABI), so
// callers that need a value to survive a syscall must spill it to a frame scratch slot — exactly the
// discipline the integer left-operand spill already uses across a `bl`. (Unlike the BSD `svc #0x80`
// macOS uses, Linux takes the bare syscall number in x8.)
static void a64_emit_svc(ZBuf *text, unsigned syscall_number) {
  a64_emit_movz_x(text, 8, syscall_number);
  a64_emit_word(text, 0xd4000001u); // svc #0
}

// MOVZ/MOVK run for an arbitrary 64-bit immediate (used for u64/i64 literals and absolute string
// addresses on the direct-exe path).
static void a64_emit_mov_imm64(ZBuf *text, unsigned reg, uint64_t value) {
  bool emitted = false;
  for (unsigned shift = 0; shift < 4; shift++) {
    uint32_t chunk = (uint32_t)((value >> (shift * 16)) & 0xffffu);
    if (chunk == 0) continue;
    if (!emitted) {
      a64_emit_word(text, 0xd2800000u | ((uint32_t)shift << 21) | (chunk << 5) | (reg & 31u)); // movz
      emitted = true;
    } else {
      a64_emit_word(text, 0xf2800000u | ((uint32_t)shift << 21) | (chunk << 5) | (reg & 31u)); // movk
    }
  }
  if (!emitted) a64_emit_word(text, 0xd2800000u | (reg & 31u)); // movz reg, #0
}

// Fixed-width MOVZ/MOVK run that always emits all four 16-bit chunks. Used to materialize a string
// address as a relocation/patch target whose final value is unknown at emit time: the slot is a
// constant 16 bytes (4 words) so it can be patched in place after the rodata address is known.
static void a64_emit_mov_imm64_fixed(ZBuf *text, unsigned reg, uint64_t value) {
  a64_emit_word(text, 0xd2800000u | (0u << 21) | ((uint32_t)(value & 0xffffu) << 5) | (reg & 31u));        // movz
  a64_emit_word(text, 0xf2800000u | (1u << 21) | ((uint32_t)((value >> 16) & 0xffffu) << 5) | (reg & 31u)); // movk lsl 16
  a64_emit_word(text, 0xf2800000u | (2u << 21) | ((uint32_t)((value >> 32) & 0xffffu) << 5) | (reg & 31u)); // movk lsl 32
  a64_emit_word(text, 0xf2800000u | (3u << 21) | ((uint32_t)((value >> 48) & 0xffffu) << 5) | (reg & 31u)); // movk lsl 48
}

// Patch a 4-word fixed MOVZ/MOVK run in place with a now-known 64-bit value (string address on the
// direct-exe path). The register field of the existing words is preserved.
static void a64_patch_mov_imm64_fixed(ZBuf *text, size_t patch_offset, uint64_t value) {
  for (unsigned chunk = 0; chunk < 4; chunk++) {
    size_t off = patch_offset + chunk * 4u;
    uint32_t word = ((unsigned char)text->data[off]) |
                    ((uint32_t)(unsigned char)text->data[off + 1] << 8) |
                    ((uint32_t)(unsigned char)text->data[off + 2] << 16) |
                    ((uint32_t)(unsigned char)text->data[off + 3] << 24);
    word = (word & 0xffe0001fu) | ((uint32_t)((value >> (chunk * 16)) & 0xffffu) << 5);
    text->data[off + 0] = (char)(word & 0xff);
    text->data[off + 1] = (char)((word >> 8) & 0xff);
    text->data[off + 2] = (char)((word >> 16) & 0xff);
    text->data[off + 3] = (char)((word >> 24) & 0xff);
  }
}

static void a64_emit_mov_w(ZBuf *text, unsigned dst, unsigned src) {
  a64_emit_word(text, 0x2a0003e0u | ((src & 31u) << 16) | (dst & 31u));
}

static void a64_emit_mov_x(ZBuf *text, unsigned dst, unsigned src) {
  a64_emit_word(text, 0xaa0003e0u | ((src & 31u) << 16) | (dst & 31u));
}

// add/sub sp, sp, #imm — `base` carries the opcode (0x910003ff add, 0xd10003ff sub).
static void a64_emit_add_sp_imm(ZBuf *text, uint32_t base, unsigned imm) {
  a64_emit_word(text, base | ((imm & 0xfffu) << 10));
}

// SDIV/UDIV (data-processing 2-source): bit 31 selects width (0=Wd 32-bit, 1=Xd 64-bit), bit 10
// selects signed(1)/unsigned(0). AArch64 divide-by-zero yields 0 (no trap) and INT_MIN/-1 yields
// INT_MIN — the architectural results; the language treats these as UB, so matching the hardware is
// consistent with the ELF x86 path's idiv/div (which faults on /0, also UB).
static void a64_emit_div(ZBuf *text, unsigned dst, unsigned lhs, unsigned rhs, bool wide, bool is_unsigned) {
  uint32_t op = is_unsigned ? 0x1ac00800u : 0x1ac00c00u;
  if (wide) op |= (1u << 31);
  a64_emit_word(text, op | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
}

// MSUB Xd, Xn, Xm, Xa = Xa - Xn*Xm (bit 31 selects width). Used for modulo: rem = a - (a/b)*b,
// so MSUB dst, quotient, divisor, dividend after the matching SDIV/UDIV.
static void a64_emit_msub(ZBuf *text, unsigned dst, unsigned n, unsigned m, unsigned a, bool wide) {
  uint32_t op = 0x1b008000u;
  if (wide) op |= (1u << 31);
  a64_emit_word(text, op | ((m & 31u) << 16) | ((a & 31u) << 10) | ((n & 31u) << 5) | (dst & 31u));
}

// Integer ADD/SUB/MUL/DIV/MOD into a GPR. `wide` selects the 64-bit (Xd) forms for i64/u64 results,
// the 32-bit (Wd) forms otherwise. DIV/MOD additionally honour `is_unsigned` (UDIV vs SDIV),
// mirroring the ELF/Mach-O backends' signed/unsigned division selection off the result type. MOD
// computes the remainder with a quotient in x10 (free here — operands are x8/x9, result in `dst`)
// followed by MSUB. Bitwise ops still bail (no consumer in the self-host subset).
static bool a64_emit_binary_int(ZBuf *text, IrBinaryOp op, unsigned dst, unsigned lhs, unsigned rhs, bool wide, bool is_unsigned, ZDiag *diag, const IrValue *value) {
  if (op == IR_BIN_ADD) {
    uint32_t base = wide ? 0x8b000000u : 0x0b000000u;
    a64_emit_word(text, base | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
    return true;
  }
  if (op == IR_BIN_SUB) {
    uint32_t base = wide ? 0xcb000000u : 0x4b000000u;
    a64_emit_word(text, base | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
    return true;
  }
  if (op == IR_BIN_MUL) {
    uint32_t base = wide ? 0x9b000000u : 0x1b000000u;
    a64_emit_word(text, base | ((rhs & 31u) << 16) | (31u << 10) | ((lhs & 31u) << 5) | (dst & 31u));
    return true;
  }
  if (op == IR_BIN_DIV) {
    a64_emit_div(text, dst, lhs, rhs, wide, is_unsigned);
    return true;
  }
  if (op == IR_BIN_MOD) {
    a64_emit_div(text, 10, lhs, rhs, wide, is_unsigned);
    a64_emit_msub(text, dst, 10, rhs, lhs, wide);
    return true;
  }
  // Bitwise operators have no consumer in the self-host subset: bail honestly rather than miscompile.
  return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support this binary operator", value ? value->line : 1, value ? value->column : 1, "unsupported operator");
}

static void a64_emit_cmp_w(ZBuf *text, unsigned lhs, unsigned rhs) {
  a64_emit_word(text, 0x6b00001fu | ((rhs & 31u) << 16) | ((lhs & 31u) << 5));
}

static void a64_emit_cmp_x(ZBuf *text, unsigned lhs, unsigned rhs) {
  a64_emit_word(text, 0xeb00001fu | ((rhs & 31u) << 16) | ((lhs & 31u) << 5));
}

// Condition codes after a CMP. EQ/NE are sign-agnostic; ordering comparisons pick signed
// (LT/LE/GT/GE) or unsigned (LO/LS/HI/HS) codes off the operand type, mirroring how the ELF/x86
// backend selects signed vs unsigned setcc, and the Mach-O backend its b.cond codes.
static unsigned a64_cond_for_compare(IrCompareOp op, bool is_unsigned) {
  switch (op) {
    case IR_CMP_EQ: return 0;  // EQ
    case IR_CMP_NE: return 1;  // NE
    case IR_CMP_LT: return is_unsigned ? 3u : 11u;   // LO : LT
    case IR_CMP_LE: return is_unsigned ? 9u : 13u;   // LS : LE
    case IR_CMP_GT: return is_unsigned ? 8u : 12u;   // HI : GT
    case IR_CMP_GE: return is_unsigned ? 2u : 10u;   // HS : GE
  }
  return 0;
}

static unsigned a64_invert_cond(unsigned cond) {
  return cond ^ 1u;
}

// Local frame addressing. Locals live at the top of the frame: a local with frame_offset F sits at
// [sp, frame_size - F]. Falls back to slot indexing for locals the IR did not assign an offset.
static unsigned a64_local_slot_offset(const IrFunction *fun, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  if (fun && local_index < fun->local_len && fun->locals[local_index].frame_offset > 0 && frame_size >= fun->locals[local_index].frame_offset) {
    return frame_size - fun->locals[local_index].frame_offset + slot_offset;
  }
  return local_index * 8u + slot_offset;
}

static void a64_emit_load_local_w(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = a64_local_slot_offset(fun, local_index, slot_offset, frame_size);
  a64_emit_word(text, 0xb9400000u | ((offset / 4u) << 10) | (31u << 5) | (reg & 31u));
}

static void a64_emit_load_local_x(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = a64_local_slot_offset(fun, local_index, slot_offset, frame_size);
  a64_emit_word(text, 0xf9400000u | ((offset / 8u) << 10) | (31u << 5) | (reg & 31u));
}

static void a64_emit_store_local_w(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = a64_local_slot_offset(fun, local_index, slot_offset, frame_size);
  a64_emit_word(text, 0xb9000000u | ((offset / 4u) << 10) | (31u << 5) | (reg & 31u));
}

static void a64_emit_store_local_x(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = a64_local_slot_offset(fun, local_index, slot_offset, frame_size);
  a64_emit_word(text, 0xf9000000u | ((offset / 8u) << 10) | (31u << 5) | (reg & 31u));
}

// Byte-granular local load (LDRB): the offset is in bytes (scale 1), used for a u8/Bool record field
// whose offset is not 4-byte aligned.
static void a64_emit_load_local_b(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = a64_local_slot_offset(fun, local_index, slot_offset, frame_size);
  a64_emit_word(text, 0x39400000u | ((offset & 0xfffu) << 10) | (31u << 5) | (reg & 31u));
}

// Byte-granular local store (STRB): the byte sibling of a64_emit_load_local_b, used to write a
// u8/Bool record field at its (possibly unaligned) field offset.
static void a64_emit_store_local_b(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = a64_local_slot_offset(fun, local_index, slot_offset, frame_size);
  a64_emit_word(text, 0x39000000u | ((offset & 0xfffu) << 10) | (31u << 5) | (reg & 31u));
}

// STR/LDR (GPR, immediate byte offset off an arbitrary base register) — used for the integer
// left-operand spill slots, which are addressed off sp (x31) at a fixed offset.
static void a64_emit_str_x_disp(ZBuf *text, unsigned src, unsigned base, unsigned byte_offset) {
  a64_emit_word(text, 0xf9000000u | (((byte_offset / 8u) & 0xfffu) << 10) | ((base & 31u) << 5) | (src & 31u));
}

static void a64_emit_ldr_x_disp(ZBuf *text, unsigned dst, unsigned base, unsigned byte_offset) {
  a64_emit_word(text, 0xf9400000u | (((byte_offset / 8u) & 0xfffu) << 10) | ((base & 31u) << 5) | (dst & 31u));
}

// STR (GPR/FP, immediate offset off an arbitrary base register) — write a record field through a
// pointer held in a base register (e.g. the caller's sret pointer in x8). The byte offset is scaled
// by the access size, as the unsigned-offset encodings require (record fields are naturally aligned
// to their width). Used for sret field stores and record-copy through a pointer (the aggregate ABI).
// Byte-identical to emit_macho64.c's macho_emit_str_{b,w,x}_disp / _ldr_w_disp / _str_v_disp.
static void a64_emit_str_b_disp(ZBuf *text, unsigned src, unsigned base, unsigned byte_offset) {
  a64_emit_word(text, 0x39000000u | ((byte_offset & 0xfffu) << 10) | ((base & 31u) << 5) | (src & 31u));
}

static void a64_emit_str_w_disp(ZBuf *text, unsigned src, unsigned base, unsigned byte_offset) {
  a64_emit_word(text, 0xb9000000u | (((byte_offset / 4u) & 0xfffu) << 10) | ((base & 31u) << 5) | (src & 31u));
}

static void a64_emit_ldr_w_disp(ZBuf *text, unsigned dst, unsigned base, unsigned byte_offset) {
  a64_emit_word(text, 0xb9400000u | (((byte_offset / 4u) & 0xfffu) << 10) | ((base & 31u) << 5) | (dst & 31u));
}

static void a64_emit_str_v_disp(ZBuf *text, unsigned vreg, unsigned base, unsigned byte_offset, bool is64) {
  uint32_t op = is64 ? 0xfd000000u : 0xbd000000u;
  unsigned scaled = is64 ? (byte_offset / 8u) : (byte_offset / 4u);
  a64_emit_word(text, op | ((scaled & 0xfffu) << 10) | ((base & 31u) << 5) | (vreg & 31u));
}

// Small load/store leaves used by typed-span / array element access and the codec LE reads. Each
// addresses an element via a base register holding the already-computed element address (#0 offset),
// or, for the codec, ptr+offset. AArch64 is little-endian, so a plain integer/FP load at the byte
// offset already yields the little-endian-encoded value.

// LDRB Wt, [Xn] — load one byte, zero-extend to 32 bits (u8 span/array elements).
static void a64_emit_ldrb_w(ZBuf *text, unsigned dst, unsigned base) {
  a64_emit_word(text, 0x39400000u | ((base & 31u) << 5) | (dst & 31u));
}

// LDRSB Wt, [Xn] — load one byte, sign-extend to 32 bits (i8 span elements).
static void a64_emit_ldrsb_w(ZBuf *text, unsigned dst, unsigned base) {
  a64_emit_word(text, 0x39c00000u | ((base & 31u) << 5) | (dst & 31u));
}

// LDR/STR Wt, [Xn] (#0 offset) — 32-bit element (i32/u32) load/store + the codec 32-bit read.
static void a64_emit_ldr_w(ZBuf *text, unsigned dst, unsigned base) {
  a64_emit_word(text, 0xb9400000u | ((base & 31u) << 5) | (dst & 31u));
}

static void a64_emit_str_w(ZBuf *text, unsigned src, unsigned base) {
  a64_emit_word(text, 0xb9000000u | ((base & 31u) << 5) | (src & 31u));
}

// STR Xt, [Xn] (#0 offset) — 8-byte element (i64/u64/usize) store. The LDR x form is a64_emit_ldr_x_disp
// at offset 0.
static void a64_emit_str_x(ZBuf *text, unsigned src, unsigned base) {
  a64_emit_word(text, 0xf9000000u | ((base & 31u) << 5) | (src & 31u));
}

// STRB Wt, [Xn] (#0 offset) — store the low byte (u8/i8 span/array elements; the in-register value
// is already zero/sign-extended).
static void a64_emit_strb_w(ZBuf *text, unsigned src, unsigned base) {
  a64_emit_word(text, 0x39000000u | ((base & 31u) << 5) | (src & 31u));
}

// LSR Wd, Wn, #shift (UBFM alias) — logical right shift by a constant, used to turn a byte length
// into an element count after a typed reinterpret (count = byteLen >> log2(elemSize)).
static void a64_emit_lsr_w_imm(ZBuf *text, unsigned dst, unsigned src, unsigned shift) {
  a64_emit_word(text, 0x53007c00u | ((shift & 31u) << 16) | ((src & 31u) << 5) | (dst & 31u));
}

// ADD Xd, Xn, Xm and ADD Xd, Xn, Xm, LSL #shift — pointer + index (optionally scaled by the element
// size log2). The plain form (lsl #0) is the 1-byte element case.
static void a64_emit_add_x_reg(ZBuf *text, unsigned dst, unsigned lhs, unsigned rhs) {
  a64_emit_word(text, 0x8b000000u | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
}

static void a64_emit_add_x_reg_lsl(ZBuf *text, unsigned dst, unsigned lhs, unsigned rhs, unsigned shift) {
  a64_emit_word(text, 0x8b000000u | ((rhs & 31u) << 16) | ((shift & 0x3fu) << 10) | ((lhs & 31u) << 5) | (dst & 31u));
}

// ADD Xd, Xn, #imm12 / ADD Wd, Wn, #imm12 / SUB Wd, Wn, #imm12 — small constant adjustments (scaled
// byte-slice start, codec offset+size, sliced length).
static void a64_emit_add_x_imm(ZBuf *text, unsigned dst, unsigned src, unsigned imm) {
  a64_emit_word(text, 0x91000000u | ((imm & 0xfffu) << 10) | ((src & 31u) << 5) | (dst & 31u));
}

static void a64_emit_add_w_imm(ZBuf *text, unsigned dst, unsigned src, unsigned imm) {
  a64_emit_word(text, 0x11000000u | ((imm & 0xfffu) << 10) | ((src & 31u) << 5) | (dst & 31u));
}

static void a64_emit_sub_w_imm(ZBuf *text, unsigned dst, unsigned src, unsigned imm) {
  a64_emit_word(text, 0x51000000u | ((imm & 0xfffu) << 10) | ((src & 31u) << 5) | (dst & 31u));
}

// ADD Xd, sp, #imm12 — materialize the address of a frame-local fixed array (its base address feeds
// element indexing and span-over-array).
static void a64_emit_add_x_sp_imm(ZBuf *text, unsigned dst, unsigned imm) {
  a64_emit_word(text, 0x910003e0u | ((imm & 0xfffu) << 10) | (dst & 31u));
}

static size_t a64_emit_bl_placeholder(ZBuf *text) {
  size_t patch = text->len;
  a64_emit_word(text, 0x94000000u);
  return patch;
}

static size_t a64_emit_b_placeholder(ZBuf *text) {
  size_t patch = text->len;
  a64_emit_word(text, 0x14000000u);
  return patch;
}

static size_t a64_emit_b_cond_placeholder(ZBuf *text, unsigned cond) {
  size_t patch = text->len;
  a64_emit_word(text, 0x54000000u | (cond & 15u));
  return patch;
}

static size_t a64_emit_cbz_w_placeholder(ZBuf *text, unsigned reg) {
  size_t patch = text->len;
  a64_emit_word(text, 0x34000000u | (reg & 31u));
  return patch;
}

// Patch a B / BL (26-bit signed word offset).
static void a64_patch_branch26(ZBuf *text, size_t patch_offset, size_t target_offset) {
  uint32_t old_instr = ((unsigned char)text->data[patch_offset]) |
                       ((uint32_t)(unsigned char)text->data[patch_offset + 1] << 8) |
                       ((uint32_t)(unsigned char)text->data[patch_offset + 2] << 16) |
                       ((uint32_t)(unsigned char)text->data[patch_offset + 3] << 24);
  int64_t delta = (int64_t)target_offset - (int64_t)patch_offset;
  int64_t words = delta / 4;
  uint32_t instr = (old_instr & 0xfc000000u) | ((uint32_t)words & 0x03ffffffu);
  text->data[patch_offset + 0] = (char)(instr & 0xff);
  text->data[patch_offset + 1] = (char)((instr >> 8) & 0xff);
  text->data[patch_offset + 2] = (char)((instr >> 16) & 0xff);
  text->data[patch_offset + 3] = (char)((instr >> 24) & 0xff);
}

// Patch a conditional branch / CBZ (19-bit signed word offset).
static void a64_patch_cond19(ZBuf *text, size_t patch_offset, size_t target_offset) {
  uint32_t instr = ((unsigned char)text->data[patch_offset]) |
                   ((uint32_t)(unsigned char)text->data[patch_offset + 1] << 8) |
                   ((uint32_t)(unsigned char)text->data[patch_offset + 2] << 16) |
                   ((uint32_t)(unsigned char)text->data[patch_offset + 3] << 24);
  int64_t delta = (int64_t)target_offset - (int64_t)patch_offset;
  int64_t words = delta / 4;
  instr = (instr & 0xff00001fu) | (((uint32_t)words & 0x7ffffu) << 5);
  text->data[patch_offset + 0] = (char)(instr & 0xff);
  text->data[patch_offset + 1] = (char)((instr >> 8) & 0xff);
  text->data[patch_offset + 2] = (char)((instr >> 16) & 0xff);
  text->data[patch_offset + 3] = (char)((instr >> 24) & 0xff);
}

// ---------------------------------------------------------------------------------------------
// AArch64 scalar floating-point leaf encoders (AAPCS FP ABI: args/return in v0..v7, s_ for f32, d_
// for f64). Ported byte-identical from emit_macho64.c — arm64 is arm64, the encodings do not depend
// on the container. The v-register file is independent of x0..x30, so FP temporaries (v8/v9) never
// clobber the integer scratch registers (x8/x9) used alongside them. IEEE 754 strict, no FMA
// contraction (each fadd/fmul rounds separately), matching the x86-64 ELF backend's scalar FP.

// LDR/STR (scalar FP, unsigned offset): bit 30 selects width (0=s 32-bit, 1=d 64-bit), bit 22
// selects load(1)/store(0). The byte offset is scaled by the access size, like the GPR forms.
static void a64_emit_ldr_v_local(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned slot_offset, unsigned frame_size, bool is64) {
  unsigned offset = a64_local_slot_offset(fun, local_index, slot_offset, frame_size);
  uint32_t base = is64 ? 0xfd400000u : 0xbd400000u;
  unsigned scaled = is64 ? (offset / 8u) : (offset / 4u);
  a64_emit_word(text, base | ((scaled & 0xfffu) << 10) | (31u << 5) | (vreg & 31u));
}

static void a64_emit_str_v_local(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned slot_offset, unsigned frame_size, bool is64) {
  unsigned offset = a64_local_slot_offset(fun, local_index, slot_offset, frame_size);
  uint32_t base = is64 ? 0xfd000000u : 0xbd000000u;
  unsigned scaled = is64 ? (offset / 8u) : (offset / 4u);
  a64_emit_word(text, base | ((scaled & 0xfffu) << 10) | (31u << 5) | (vreg & 31u));
}

// Spill/reload an FP register to a fixed [sp, #off] slot. sp is constant for the body, so the slot
// is stable across nested evaluation (the float-binary/compare/call-arg left-spill discipline)
// without disturbing the sp-relative local addressing. The byte offset is scaled by the access size.
static void a64_emit_str_v_sp(ZBuf *text, unsigned vreg, unsigned byte_offset, bool is64) {
  uint32_t base = is64 ? 0xfd000000u : 0xbd000000u;
  unsigned scaled = is64 ? (byte_offset / 8u) : (byte_offset / 4u);
  a64_emit_word(text, base | ((scaled & 0xfffu) << 10) | (31u << 5) | (vreg & 31u));
}

static void a64_emit_ldr_v_sp(ZBuf *text, unsigned vreg, unsigned byte_offset, bool is64) {
  uint32_t base = is64 ? 0xfd400000u : 0xbd400000u;
  unsigned scaled = is64 ? (byte_offset / 8u) : (byte_offset / 4u);
  a64_emit_word(text, base | ((scaled & 0xfffu) << 10) | (31u << 5) | (vreg & 31u));
}

// LDR/STR (scalar FP, base only, #0 offset): ldr/str s_/d_, [xbase]. Used when the element address
// is already computed in a base register (float span index load/store, codec readF*Le).
static void a64_emit_ldr_v_base(ZBuf *text, unsigned vreg, unsigned base, bool is64) {
  uint32_t op = is64 ? 0xfd400000u : 0xbd400000u;
  a64_emit_word(text, op | ((base & 31u) << 5) | (vreg & 31u));
}

static void a64_emit_str_v_base(ZBuf *text, unsigned vreg, unsigned base, bool is64) {
  uint32_t op = is64 ? 0xfd000000u : 0xbd000000u;
  a64_emit_word(text, op | ((base & 31u) << 5) | (vreg & 31u));
}

// LDR/STR (scalar FP, register offset, lsl by access-size log2): ldr s_, [xbase, xindex, lsl #2] /
// ldr d_, [xbase, xindex, lsl #3]. Used for fixed [N]f32 / [N]f64 array element access.
static void a64_emit_ldr_v_reg_lsl(ZBuf *text, unsigned vreg, unsigned base, unsigned index, bool is64) {
  uint32_t opc = is64 ? 0xfc607800u : 0xbc607800u;
  a64_emit_word(text, opc | ((index & 31u) << 16) | ((base & 31u) << 5) | (vreg & 31u));
}

static void a64_emit_str_v_reg_lsl(ZBuf *text, unsigned vreg, unsigned base, unsigned index, bool is64) {
  uint32_t opc = is64 ? 0xfc207800u : 0xbc207800u;
  a64_emit_word(text, opc | ((index & 31u) << 16) | ((base & 31u) << 5) | (vreg & 31u));
}

// Byte width and log2(width) of a span/array element type. Only the element kinds a byte view or
// fixed array can hold reach here; the 1-byte forms (u8/i8) have log2 0, so index scaling is a no-op.
static unsigned a64_elem_byte_size(IrTypeKind type) {
  switch (type) {
    case IR_TYPE_U8:
    case IR_TYPE_I8: return 1;
    case IR_TYPE_I32:
    case IR_TYPE_U32: return 4;
    case IR_TYPE_F32: return 4;
    case IR_TYPE_I64:
    case IR_TYPE_U64: return 8;
    case IR_TYPE_F64: return 8;
    case IR_TYPE_USIZE: return 8;
    default: return 0;
  }
}

static unsigned a64_elem_log2(IrTypeKind type) {
  unsigned size = a64_elem_byte_size(type);
  if (size == 8) return 3;
  if (size == 4) return 2;
  return 0;
}

// FMOV v_, v_ (register move within the FP file): bit 22 selects s/d.
static void a64_emit_fmov_v(ZBuf *text, unsigned dst, unsigned src, bool is64) {
  uint32_t base = is64 ? 0x1e604000u : 0x1e204000u;
  a64_emit_word(text, base | ((src & 31u) << 5) | (dst & 31u));
}

// FMOV v_, w_/x_ : copy the raw bits of an integer register into an FP register (no conversion).
// Used to materialize a float constant (its IEEE bit pattern is built in a GPR, then moved across).
static void a64_emit_fmov_v_from_gpr(ZBuf *text, unsigned vreg, unsigned gpr, bool is64) {
  uint32_t base = is64 ? 0x9e670000u : 0x1e270000u;
  a64_emit_word(text, base | ((gpr & 31u) << 5) | (vreg & 31u));
}

// FADD/FSUB/FMUL/FDIV (scalar): bit 22 selects s/d; the opcode field selects the operation. Integer
// division/modulo bail (P6); FP division is a single instruction with no trap, so it lands here.
static bool a64_emit_float_arith(ZBuf *text, IrBinaryOp op, unsigned dst, unsigned lhs, unsigned rhs, bool is64) {
  uint32_t opcode;
  switch (op) {
    case IR_BIN_ADD: opcode = 0x1e202800u; break;
    case IR_BIN_SUB: opcode = 0x1e203800u; break;
    case IR_BIN_MUL: opcode = 0x1e200800u; break;
    case IR_BIN_DIV: opcode = 0x1e201800u; break;
    default: return false;
  }
  if (is64) opcode |= (1u << 22);
  a64_emit_word(text, opcode | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
  return true;
}

// FCMP v_, v_ : sets NZCV. IEEE unordered (NaN) yields N=0 Z=0 C=1 V=1, so the carry/overflow flags
// drive the IEEE-correct condition codes in a64_float_cond_for_compare.
static void a64_emit_fcmp(ZBuf *text, unsigned lhs, unsigned rhs, bool is64) {
  uint32_t base = is64 ? 0x1e602000u : 0x1e202000u;
  a64_emit_word(text, base | ((rhs & 31u) << 16) | ((lhs & 31u) << 5));
}

// CSET w_, <cond> : materialize a boolean from NZCV. The CSINC encoding carries the inverted
// condition (the assembler alias does the same: `cset w, EQ` encodes `csinc w, wzr, wzr, NE`).
static void a64_emit_cset(ZBuf *text, unsigned gpr, unsigned cond) {
  a64_emit_word(text, 0x1a9f07e0u | (((cond ^ 1u) & 15u) << 12) | (gpr & 31u));
}

// FCVT s<->d : narrow/widen between f32 and f64.
static void a64_emit_fcvt(ZBuf *text, unsigned dst, unsigned src, bool dst_is64) {
  uint32_t base = dst_is64 ? 0x1e22c000u : 0x1e624000u; // fcvt d,s : fcvt s,d
  a64_emit_word(text, base | ((src & 31u) << 5) | (dst & 31u));
}

// FCVTZS w_, v_ : float -> i32, round toward zero (matches the `as i32` truncation contract).
static void a64_emit_fcvtzs_w(ZBuf *text, unsigned gpr, unsigned vreg, bool src_is64) {
  uint32_t base = src_is64 ? 0x1e780000u : 0x1e380000u;
  a64_emit_word(text, base | ((vreg & 31u) << 5) | (gpr & 31u));
}

// SCVTF v_, w_ : signed i32 -> float. UCVTF v_, x_ : unsigned u64 -> float.
static void a64_emit_scvtf_from_w(ZBuf *text, unsigned vreg, unsigned gpr, bool dst_is64) {
  uint32_t base = dst_is64 ? 0x1e620000u : 0x1e220000u;
  a64_emit_word(text, base | ((gpr & 31u) << 5) | (vreg & 31u));
}

static void a64_emit_ucvtf_from_x(ZBuf *text, unsigned vreg, unsigned gpr, bool dst_is64) {
  uint32_t base = dst_is64 ? 0x9e630000u : 0x9e230000u;
  a64_emit_word(text, base | ((gpr & 31u) << 5) | (vreg & 31u));
}

// Condition codes after an FCMP. The IEEE-correct codes make every ordering against a NaN false and
// `!=` true: EQ/NE are direct; LT=MI, LE=LS, GT=GT, GE=GE use the C/V flags so an unordered compare
// (NaN) takes the false branch for </<=/>/>= and the true branch for !=. Mirrors emit_macho64.c.
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

// ---------------------------------------------------------------------------------------------
// Emit context. Modeled on emit_elf64.c's ElfEmitContext: same-file call patches + string-literal
// (rodata) address patches. P1's direct-exe path is self-contained, so no R_AARCH64 relocations are
// emitted; string addresses are absolute from the fixed load base and patched in place once the
// rodata address is known (later phases add the object-path reloc writers).

typedef struct {
  size_t patch_offset;
  unsigned callee_index;
} Aarch64CallPatch;

typedef struct {
  size_t patch_offset;  // direct-exe: start of the 4-word MOVZ/MOVK run; object: the ADRP word
  unsigned data_offset; // rodata byte offset of the string literal
} Aarch64RodataPatch;

// libm is reached through bare ELF symbols (sqrtf, expf, …, powf) bound by the host linker via
// R_AARCH64_CALL26 relocations in the object path. A single symbol-keyed table (one enum + names
// array + one patch array) records each `bl` site, mirroring emit_macho64.c's MachOMathSymbol table
// — cleaner than emit_elf64.c's seven parallel arrays while emitting the same per-symbol relocations.
typedef enum {
  A64_MATH_SQRTF = 0,
  A64_MATH_EXPF,
  A64_MATH_COSF,
  A64_MATH_SINF,
  A64_MATH_POWF,
  A64_MATH_FABSF,
  A64_MATH_FLOORF,
  A64_MATH_SYMBOL_COUNT
} Aarch64MathSymbol;

static const char *const a64_math_symbol_names[A64_MATH_SYMBOL_COUNT] = {
  "sqrtf", "expf", "cosf", "sinf", "powf", "fabsf", "floorf"
};

typedef struct {
  size_t patch_offset;        // the `bl` site that branches to the libm symbol
  Aarch64MathSymbol symbol;
} Aarch64MathCallPatch;

typedef struct {
  const IrProgram *ir;
  size_t *function_offsets;
  size_t function_count;
  unsigned rodata_base_offset;
  uint64_t rodata_addr;     // runtime address of rodata_base_offset (direct-exe), set late
  bool world_write_used;    // both paths emit an inline svc-write helper if true
  bool emit_rodata_relocations; // object path: address rodata via ADRP+ADD + R_AARCH64 relocs
  Aarch64CallPatch *call_patches;
  size_t call_patch_len;
  size_t call_patch_cap;
  Aarch64RodataPatch *rodata_patches;
  size_t rodata_patch_len;
  size_t rodata_patch_cap;
  Aarch64MathCallPatch *math_call_patches;
  size_t math_call_patch_len;
  size_t math_call_patch_cap;
} Aarch64EmitContext;

static Aarch64MathSymbol a64_math_symbol_for_value(IrValueKind kind) {
  switch (kind) {
    case IR_VALUE_MATH_SQRTF: return A64_MATH_SQRTF;
    case IR_VALUE_MATH_EXPF: return A64_MATH_EXPF;
    case IR_VALUE_MATH_COSF: return A64_MATH_COSF;
    case IR_VALUE_MATH_SINF: return A64_MATH_SINF;
    case IR_VALUE_MATH_POWF: return A64_MATH_POWF;
    case IR_VALUE_MATH_FABSF: return A64_MATH_FABSF;
    case IR_VALUE_MATH_FLOORF: return A64_MATH_FLOORF;
    default: return A64_MATH_SQRTF;
  }
}

static bool a64_record_math_call_patch(Aarch64EmitContext *ctx, size_t patch_offset, Aarch64MathSymbol symbol, const IrValue *value, ZDiag *diag) {
  if (!ctx) return a64_diag_at(diag, "direct AArch64 ELF math relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->math_call_patch_len == ctx->math_call_patch_cap) {
    ctx->math_call_patch_cap = z_grow_capacity(ctx->math_call_patch_cap, ctx->math_call_patch_len + 1, 8);
    ctx->math_call_patches = z_checked_reallocarray(ctx->math_call_patches, ctx->math_call_patch_cap, sizeof(Aarch64MathCallPatch));
  }
  ctx->math_call_patches[ctx->math_call_patch_len++] = (Aarch64MathCallPatch){.patch_offset = patch_offset, .symbol = symbol};
  return true;
}

static bool a64_math_symbol_used(const Aarch64EmitContext *ctx, Aarch64MathSymbol symbol) {
  for (size_t i = 0; ctx && i < ctx->math_call_patch_len; i++) {
    if (ctx->math_call_patches[i].symbol == symbol) return true;
  }
  return false;
}

static bool a64_record_call_patch(Aarch64EmitContext *ctx, size_t patch_offset, unsigned callee_index, const IrValue *value, ZDiag *diag) {
  if (!ctx || callee_index >= ctx->function_count) {
    return a64_diag_at(diag, "direct AArch64 ELF call target is out of range", value ? value->line : 1, value ? value->column : 1, "invalid callee");
  }
  if (ctx->call_patch_len == ctx->call_patch_cap) {
    ctx->call_patch_cap = z_grow_capacity(ctx->call_patch_cap, ctx->call_patch_len + 1, 8);
    ctx->call_patches = z_checked_reallocarray(ctx->call_patches, ctx->call_patch_cap, sizeof(Aarch64CallPatch));
  }
  ctx->call_patches[ctx->call_patch_len++] = (Aarch64CallPatch){.patch_offset = patch_offset, .callee_index = callee_index};
  return true;
}

static bool a64_record_rodata_patch(Aarch64EmitContext *ctx, size_t patch_offset, unsigned data_offset, const IrValue *value, ZDiag *diag) {
  if (!ctx) return a64_diag_at(diag, "direct AArch64 ELF readonly-data patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->rodata_patch_len == ctx->rodata_patch_cap) {
    ctx->rodata_patch_cap = z_grow_capacity(ctx->rodata_patch_cap, ctx->rodata_patch_len + 1, 8);
    ctx->rodata_patches = z_checked_reallocarray(ctx->rodata_patches, ctx->rodata_patch_cap, sizeof(Aarch64RodataPatch));
  }
  ctx->rodata_patches[ctx->rodata_patch_len++] = (Aarch64RodataPatch){.patch_offset = patch_offset, .data_offset = data_offset};
  return true;
}

// world.out.write call sites are recorded as patches whose callee_index is the sentinel value
// `function_count` (one past the last real function); the direct-exe path resolves them to the
// inline svc-write helper rather than a function offset.
static bool a64_record_world_write_patch(Aarch64EmitContext *ctx, size_t patch_offset, const IrInstr *instr, ZDiag *diag) {
  if (!ctx) return a64_diag_at(diag, "direct AArch64 ELF world-write patch requires an emit context", instr ? instr->line : 1, instr ? instr->column : 1, "missing context");
  if (ctx->call_patch_len == ctx->call_patch_cap) {
    ctx->call_patch_cap = z_grow_capacity(ctx->call_patch_cap, ctx->call_patch_len + 1, 8);
    ctx->call_patches = z_checked_reallocarray(ctx->call_patches, ctx->call_patch_cap, sizeof(Aarch64CallPatch));
  }
  ctx->call_patches[ctx->call_patch_len++] = (Aarch64CallPatch){.patch_offset = patch_offset, .callee_index = (unsigned)ctx->function_count};
  return true;
}

static void a64_patch_call_patches(ZBuf *text, const Aarch64EmitContext *ctx) {
  for (size_t i = 0; ctx && i < ctx->call_patch_len; i++) {
    if (ctx->call_patches[i].callee_index >= ctx->function_count) continue; // world-write sentinel, resolved separately
    a64_patch_branch26(text, ctx->call_patches[i].patch_offset, ctx->function_offsets[ctx->call_patches[i].callee_index]);
  }
}

static void a64_patch_rodata_patches(ZBuf *text, const Aarch64EmitContext *ctx) {
  for (size_t i = 0; ctx && i < ctx->rodata_patch_len; i++) {
    const Aarch64RodataPatch *patch = &ctx->rodata_patches[i];
    uint64_t addr = ctx->rodata_addr + (patch->data_offset - ctx->rodata_base_offset);
    a64_patch_mov_imm64_fixed(text, patch->patch_offset, addr);
  }
}

// ADRP xreg, #0 : materialize the 4 KiB page base of a PC-relative symbol (immhi/immlo filled by the
// R_AARCH64_ADR_PREL_PG_HI21 relocation). ADD xreg, xreg, #0 : add the within-page offset (filled by
// R_AARCH64_ADD_ABS_LO12_NC). The pair is the standard PIC small-model address materialization the
// host linker resolves; the object is relocatable, not fixed-base, so it must use this form (not an
// absolute immediate).
static void a64_emit_adrp(ZBuf *text, unsigned reg) {
  a64_emit_word(text, 0x90000000u | (reg & 31u));
}

static void a64_emit_add_imm_lo12_placeholder(ZBuf *text, unsigned reg) {
  a64_emit_word(text, 0x91000000u | ((reg & 31u) << 5) | (reg & 31u));
}

// Materialize the address of a rodata string literal into a register. The object path is relocatable,
// so it emits ADRP+ADD and records two R_AARCH64 relocations (page-high + page-low) against the
// .rodata section symbol. The direct-exe path loads at a fixed base, so the address is a constant once
// the rodata offset is laid out: a fixed 4-word MOVZ/MOVK run, patched in place after the layout is
// final (no relocations needed for ET_EXEC at a fixed base). This is the AArch64 analog of
// emit_elf64.c's elf_emit_rodata_ptr_rax (reloc in the object, absolute imm64 in the exe).
static bool a64_emit_rodata_ptr(ZBuf *text, unsigned reg, unsigned data_offset, Aarch64EmitContext *ctx, const IrValue *value, ZDiag *diag) {
  size_t patch_offset = text->len;
  if (ctx && ctx->emit_rodata_relocations) {
    a64_emit_adrp(text, reg);
    a64_emit_add_imm_lo12_placeholder(text, reg);
    return a64_record_rodata_patch(ctx, patch_offset, data_offset, value, diag);
  }
  a64_emit_mov_imm64_fixed(text, reg, 0);
  return a64_record_rodata_patch(ctx, patch_offset, data_offset, value, diag);
}

// ---------------------------------------------------------------------------------------------
// Byte-view (string/span) pointer + length. P4 supports every byte-view shape the typed-span / codec
// fixtures use: a string literal (rodata pointer + constant length), a BYTE_VIEW local (ptr@slot+0,
// len@slot+8 as a 32-bit element count), a span field of a record, a constant or runtime byte slice,
// a typed reinterpret (same pointer, length divided by the new element size), and a span over a fixed
// array (the array's base address). Span PARAMS are the aggregate-ABI phase (P5); a Mapping's 64-bit
// length is P7 (the is_mapping flag) — P4 keeps the 32-bit element-count convention everywhere.

static bool a64_emit_value_to_reg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag);

// depth is the integer binary/compare nesting level: a byte-view sub-expression evaluated while
// `depth` outer left operands are spilled in frame slots must use slots at or above `depth`, so depth
// threads through every sub-value evaluation (these helpers forward it unchanged; only a binary/
// compare consumes a slot and passes depth+1 to its operands). The non-depth wrappers below enter at
// depth 0 for instruction-level callers. Mirrors emit_macho64.c's _depth byte-view helpers.
static bool a64_emit_byte_view_ptr_depth(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag);
static bool a64_emit_byte_view_len_depth(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag);
static bool a64_emit_span_index_addr_depth(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, IrTypeKind element_type, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag);

static bool a64_const_u32_value(const IrValue *value, unsigned *out) {
  if (!value || value->kind != IR_VALUE_INT || value->int_value > UINT32_MAX) return false;
  if (out) *out = (unsigned)value->int_value;
  return true;
}

// Constant element count of a byte view whose length is statically known: a literal/array view, a
// constant-bounded slice, or a reinterpret of a constant-length view (count = base byte length /
// element size). Returns false for runtime-length views.
static bool a64_byte_view_const_len(const IrValue *view, unsigned *out) {
  if (!view) return false;
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (out) *out = view->data_len;
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned base_len = 0;
    if (!a64_byte_view_const_len(view->left, &base_len)) return false;
    unsigned start = 0;
    unsigned end = base_len;
    if (view->index && !a64_const_u32_value(view->index, &start)) return false;
    if (view->right && !a64_const_u32_value(view->right, &end)) return false;
    if (start > end || end > base_len) return false;
    if (out) *out = end - start;
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) {
    unsigned base_len = 0;
    if (!a64_byte_view_const_len(view->left, &base_len)) return false;
    unsigned size = a64_elem_byte_size(view->element_type);
    if (size == 0) return false;
    if (out) *out = base_len / size;
    return true;
  }
  return false;
}

// Read a single byte of the program's readonly data at `offset` (for compile-time constant u8
// byte-view index loads). Mirrors emit_macho64.c's macho_readonly_data_byte.
static bool a64_readonly_data_byte(const IrProgram *program, unsigned offset, unsigned char *out) {
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

// Constant byte at `index` of a constant byte view (a string literal or a constant-bounded slice of
// one). Lets a `view[const]` u8 read fold to a movz at compile time.
static bool a64_byte_view_const_byte(const IrProgram *program, const IrValue *view, unsigned index, unsigned char *out) {
  if (!view) return false;
  if (view->kind == IR_VALUE_STRING_LITERAL) {
    if (index >= view->data_len) return false;
    return a64_readonly_data_byte(program, view->data_offset + index, out);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned len = 0;
    unsigned start = 0;
    if (!a64_byte_view_const_len(view, &len) || index >= len) return false;
    if (view->index && !a64_const_u32_value(view->index, &start)) return false;
    return a64_byte_view_const_byte(program, view->left, start + index, out);
  }
  return false;
}

static bool a64_emit_byte_view_ptr(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  return a64_emit_byte_view_ptr_depth(text, fun, view, reg, frame_size, 0, ctx, diag);
}

static bool a64_emit_byte_view_ptr_depth(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (!view) return a64_diag_at(diag, "direct AArch64 ELF byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    a64_emit_load_local_x(text, fun, reg, view->local_index, 0, frame_size); // ptr @ slot+0
    return true;
  }
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field: the 8-byte pointer lives at the field offset within the record local.
    a64_emit_load_local_x(text, fun, reg, view->local_index, view->field_offset, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    // Maybe<MutSpan<u8>>.value / Maybe<owned<Mapping>>.value: the 8-byte pointer lives at slot+8.
    a64_emit_load_local_x(text, fun, reg, view->local_index, 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    // A span over a fixed array yields the array base address; the element type drives later index
    // scaling. Any primitive element type a span can hold is accepted (matching the ELF/macho backends).
    IrTypeKind elem = local->element_type;
    bool ok = local->is_array && (elem == IR_TYPE_U8 || elem == IR_TYPE_I8 || elem == IR_TYPE_I32 || elem == IR_TYPE_U32 ||
              elem == IR_TYPE_I64 || elem == IR_TYPE_U64 || elem == IR_TYPE_USIZE || elem == IR_TYPE_F32 || elem == IR_TYPE_F64);
    if (!ok) return a64_diag_at(diag, "direct AArch64 ELF byte-view array element type is unsupported", view->line, view->column, "unsupported array view");
    a64_emit_add_x_sp_imm(text, reg, a64_local_slot_offset(fun, view->array_index, 0, frame_size));
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) {
    return a64_emit_rodata_ptr(text, reg, view->data_offset, ctx, view, diag);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    if (!a64_emit_byte_view_ptr_depth(text, fun, view->left, reg, frame_size, depth, ctx, diag)) return false;
    if (!view->index) return true;
    // The slice start is an element COUNT; the pointer must advance by start*sizeof(element). u8/i8
    // (size 1) need no scaling; wider element types shift the start by log2(size). Mirrors ELF's
    // element-byte-size scaling of the slice start.
    unsigned log2 = a64_elem_log2(view->element_type);
    if (a64_const_u32_value(view->index, &start)) {
      unsigned byte_off = start << log2;
      if (byte_off > 4095) return a64_diag_at(diag, "direct AArch64 ELF byte slice constant start is too large", view->line, view->column, "unsupported byte slice");
      if (byte_off > 0) a64_emit_add_x_imm(text, reg, reg, byte_off);
      return true;
    }
    unsigned tmp = reg == 8 ? 9 : 8;
    if (!a64_emit_value_to_reg_depth(text, fun, view->index, tmp, frame_size, depth, ctx, diag)) return false;
    a64_emit_add_x_reg_lsl(text, reg, reg, tmp, log2);
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) {
    // A typed reinterpret keeps the same base pointer; only the length (element count) changes.
    return a64_emit_byte_view_ptr_depth(text, fun, view->left, reg, frame_size, depth, ctx, diag);
  }
  return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support this byte-view source (span parameters land in a later phase)", view->line, view->column, "unsupported byte view");
}

static bool a64_emit_byte_view_len(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  return a64_emit_byte_view_len_depth(text, fun, view, reg, frame_size, 0, ctx, diag);
}

static bool a64_emit_byte_view_len_depth(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (!view) return a64_diag_at(diag, "direct AArch64 ELF byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (view->data_len > 65535) return a64_diag_at(diag, "direct AArch64 ELF byte-view length is too large for the current subset", view->line, view->column, "large byte view");
    a64_emit_movz_w(text, reg, view->data_len);
    return true;
  }
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    // A Mapping length is a 64-bit byte count (files may exceed 4 GiB); ordinary spans keep a 32-bit
    // element count.
    if (fun->locals[view->local_index].is_mapping) a64_emit_load_local_x(text, fun, reg, view->local_index, 8, frame_size);
    else a64_emit_load_local_w(text, fun, reg, view->local_index, 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field length lives 8 bytes past the field's ptr, as a 32-bit element count.
    a64_emit_load_local_w(text, fun, reg, view->local_index, view->field_offset + 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    // Maybe<owned<Mapping>>.value carries a 64-bit byte length at slot+16; other Maybe spans keep a
    // 32-bit count.
    if (fun->locals[view->local_index].is_mapping) a64_emit_load_local_x(text, fun, reg, view->local_index, 16, frame_size);
    else a64_emit_load_local_w(text, fun, reg, view->local_index, 16, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    unsigned end = 0;
    if ((!view->index || a64_const_u32_value(view->index, &start)) &&
        a64_const_u32_value(view->right, &end) && end >= start && end - start <= 65535) {
      a64_emit_movz_w(text, reg, end - start);
      return true;
    }
    if ((!view->index || a64_const_u32_value(view->index, &start)) && view->right) {
      if (!a64_emit_value_to_reg_depth(text, fun, view->right, reg, frame_size, depth, ctx, diag)) return false;
      if (start > 0) a64_emit_sub_w_imm(text, reg, reg, start);
      return true;
    }
    if (view->index && view->right) {
      unsigned tmp = reg == 8 ? 9 : 8;
      if (!a64_emit_value_to_reg_depth(text, fun, view->right, reg, frame_size, depth, ctx, diag)) return false;
      if (!a64_emit_value_to_reg_depth(text, fun, view->index, tmp, frame_size, depth, ctx, diag)) return false;
      a64_emit_binary_int(text, IR_BIN_SUB, reg, reg, tmp, false, false, diag, view);
      return true;
    }
  }
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    if (!a64_emit_byte_view_len_depth(text, fun, view->left, reg, frame_size, depth, ctx, diag)) return false;
    // Element count = underlying byte length >> log2(element size). A 1-byte element (u8/i8) needs no
    // shift: the byte length already is the count.
    unsigned shift = a64_elem_log2(view->element_type);
    if (shift > 0) a64_emit_lsr_w_imm(text, reg, reg, shift);
    return true;
  }
  return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support this byte-view length source (span parameters land in a later phase)", view->line, view->column, "unsupported byte-view length");
}

// Compute the address of element `index` of a span (byte-view) local into x9, bounds-checked against
// the span's runtime length (idx < len, else brk #0 — the analog of the array out-of-range trap).
// `element_type` sets the element size used to scale the index. The index is materialized into w8
// first, so callers must not rely on x8 surviving this call. x9 holds the element address on return.
// Ported byte-identical from emit_macho64.c's macho_emit_span_index_addr.
static bool a64_emit_span_index_addr(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, IrTypeKind element_type, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  return a64_emit_span_index_addr_depth(text, fun, local_index, index, element_type, frame_size, 0, ctx, diag);
}

static bool a64_emit_span_index_addr_depth(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, IrTypeKind element_type, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (!index || !a64_emit_value_to_reg_depth(text, fun, index, 8, frame_size, depth, ctx, diag)) return false;
  a64_emit_load_local_w(text, fun, 9, local_index, 8, frame_size); // len @ slot+8 (32-bit count)
  a64_emit_cmp_w(text, 8, 9);
  size_t ok_patch = a64_emit_b_cond_placeholder(text, 3); // unsigned lower (idx < len)
  a64_emit_word(text, 0xd4200000u); // brk #0 on an out-of-range index
  a64_patch_cond19(text, ok_patch, text->len);
  a64_emit_load_local_x(text, fun, 9, local_index, 0, frame_size); // ptr @ slot+0
  unsigned shift = a64_elem_log2(element_type);
  if (shift > 0) a64_emit_add_x_reg_lsl(text, 9, 9, 8, shift);
  else a64_emit_add_x_reg(text, 9, 9, 8);
  return true;
}

// ---------------------------------------------------------------------------------------------
// FP scratch slots. A float binary/compare/call-arg spills its left/arg operand to a fixed frame
// slot across the sibling's evaluation (which may nest deeper or, in a later phase, call libm — both
// of which the frame slot survives, unlike a caller-saved register or an sp-push that would corrupt
// the sp-relative local addressing). One 16-byte slot per FP nesting level, indexed by FP depth,
// independent of the integer depth counter (the two register files never overlap). Ported from
// emit_macho64.c's macho_value_fp_depth / macho_fp_scratch_*.

static unsigned a64_fp_scratch_slot_offset(unsigned depth) {
  return depth * 16u; // FP scratch lives at the bottom of the frame, [0, fp_scratch_bytes)
}

// Maximum FP binary/compare nesting depth in a value (1 for a leaf binary). Drives how many FP
// scratch slots the frame must reserve.
static unsigned a64_value_fp_depth(const IrValue *value) {
  if (!value) return 0;
  if (value->kind == IR_VALUE_BINARY && a64_type_is_float(value->type)) {
    unsigned l = a64_value_fp_depth(value->left);
    unsigned r = a64_value_fp_depth(value->right);
    return 1 + (l > r ? l : r);
  }
  if (value->kind == IR_VALUE_COMPARE && value->left && a64_type_is_float(value->left->type)) {
    unsigned l = a64_value_fp_depth(value->left);
    unsigned r = a64_value_fp_depth(value->right);
    return 1 + (l > r ? l : r);
  }
  unsigned best = 0;
  const IrValue *kids[3] = {value->left, value->right, value->index};
  for (unsigned i = 0; i < 3; i++) {
    unsigned d = a64_value_fp_depth(kids[i]);
    if (d > best) best = d;
  }
  for (size_t i = 0; i < value->arg_len; i++) {
    unsigned d = a64_value_fp_depth(value->args[i]);
    if (d > best) best = d;
  }
  return best;
}

static unsigned a64_instrs_fp_depth(const IrInstr *instrs, size_t len);

static unsigned a64_instr_fp_depth(const IrInstr *instr) {
  if (!instr) return 0;
  unsigned best = a64_value_fp_depth(instr->value);
  unsigned t = a64_instrs_fp_depth(instr->then_instrs, instr->then_len);
  if (t > best) best = t;
  unsigned e = a64_instrs_fp_depth(instr->else_instrs, instr->else_len);
  if (e > best) best = e;
  return best;
}

static unsigned a64_instrs_fp_depth(const IrInstr *instrs, size_t len) {
  unsigned best = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned d = a64_instr_fp_depth(&instrs[i]);
    if (d > best) best = d;
  }
  return best;
}

static unsigned a64_fp_scratch_bytes(const IrFunction *fun) {
  if (!fun) return 0;
  return a64_instrs_fp_depth(fun->instrs, fun->instr_len) * 16u;
}

// ---------------------------------------------------------------------------------------------
// Integer scratch slots. The integer binary/compare path spills its left operand to a fixed frame
// slot across the right operand's evaluation (the right may clobber x8 via a call), mirroring the
// Mach-O backend's discipline. One 16-byte slot per nesting level. The integer scratch region sits
// just above the FP scratch region (which occupies [0, fp_scratch_bytes)), so the slot offset adds
// fp_scratch_bytes — exactly macho's frame = fp + int layout.

static unsigned a64_value_int_depth(const IrValue *value) {
  if (!value) return 0;
  // std.fs.mmap holds fd, size, and addr across the openat/lseek/mmap/close syscalls; it needs two
  // 16-byte scratch slots (fd+size in one, addr in the other), so it contributes depth 2.
  if (value->kind == IR_VALUE_FS_MMAP) {
    unsigned l = a64_value_int_depth(value->left);
    unsigned r = a64_value_int_depth(value->right);
    return 2 + (l > r ? l : r);
  }
  bool spills = (value->kind == IR_VALUE_BINARY && value->binary_op != IR_BIN_AND && value->binary_op != IR_BIN_OR &&
                 !a64_type_is_float(value->type)) ||
                (value->kind == IR_VALUE_COMPARE && value->left && !a64_type_is_float(value->left->type)) ||
                value->kind == IR_VALUE_BYTE_VIEW_EQ || // bridges len/ptr sub-evaluations via one slot
                value->kind == IR_VALUE_BYTE_COPY ||    // bridges src/dst ptr+len sub-evaluations via one slot
                value->kind == IR_VALUE_BYTE_FILL ||    // bridges dst ptr+len sub-evaluations via one slot
                value->kind == IR_VALUE_ALLOC_BYTES;    // page-alloc allocBytes spills the size across the mmap syscall
  if (spills) {
    unsigned l = a64_value_int_depth(value->left);
    unsigned r = a64_value_int_depth(value->right);
    return 1 + (l > r ? l : r);
  }
  unsigned best = 0;
  const IrValue *kids[3] = {value->left, value->right, value->index};
  for (unsigned i = 0; i < 3; i++) {
    unsigned d = a64_value_int_depth(kids[i]);
    if (d > best) best = d;
  }
  for (size_t i = 0; i < value->arg_len; i++) {
    unsigned d = a64_value_int_depth(value->args[i]);
    if (d > best) best = d;
  }
  return best;
}

static unsigned a64_instrs_int_depth(const IrInstr *instrs, size_t len);

static unsigned a64_instr_int_depth(const IrInstr *instr) {
  if (!instr) return 0;
  unsigned best = a64_value_int_depth(instr->value);
  unsigned t = a64_instrs_int_depth(instr->then_instrs, instr->then_len);
  if (t > best) best = t;
  unsigned e = a64_instrs_int_depth(instr->else_instrs, instr->else_len);
  if (e > best) best = e;
  return best;
}

static unsigned a64_instrs_int_depth(const IrInstr *instrs, size_t len) {
  unsigned best = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned d = a64_instr_int_depth(&instrs[i]);
    if (d > best) best = d;
  }
  return best;
}

static unsigned a64_int_scratch_bytes(const IrFunction *fun) {
  if (!fun) return 0;
  return a64_instrs_int_depth(fun->instrs, fun->instr_len) * 16u;
}

static unsigned a64_int_scratch_slot_offset(const IrFunction *fun, unsigned depth) {
  return a64_fp_scratch_bytes(fun) + depth * 16u; // integer slots sit just above the FP scratch
}

// ---------------------------------------------------------------------------------------------
// sret (record return) frame slot. A record-returning function receives its destination buffer
// pointer in x8 (the AAPCS indirect-result register, which is NOT an argument register, unlike ELF's
// rdi-sret) and must keep it live across the body — calls clobber x8 — so it is spilled to a reserved
// frame slot in the prologue and reloaded at each field store / return. The slot sits just above the
// FP and integer scratch regions; the frame is enlarged by 16 bytes so it never overlaps the locals
// (addressed from the top of the frame). Ported from emit_macho64.c's macho_sret_* helpers.

static bool a64_returns_record(const IrFunction *fun) {
  return fun && fun->return_type == IR_TYPE_RECORD;
}

static unsigned a64_sret_reserved_bytes(const IrFunction *fun) {
  return a64_returns_record(fun) ? 16u : 0u;
}

static unsigned a64_sret_slot_offset(const IrFunction *fun) {
  return a64_fp_scratch_bytes(fun) + a64_int_scratch_bytes(fun);
}

// ---------------------------------------------------------------------------------------------
// Expression codegen. Materializes an integer value into register `reg`. `depth` is the integer
// binary/compare nesting level (forwarded unchanged except where a binary/compare consumes a slot
// and evaluates its operands at depth+1).

static bool a64_emit_value_to_reg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag);
// FP value dispatch (defined below): the integer path's float COMPARE / CAST(f->i) / isNaNf hooks
// and the FP instruction hooks route float sub-expressions through it.
static bool a64_emit_float_value_to_vreg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag);
static bool a64_emit_float_value_to_vreg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag);
// The float `check` arm runs the epilogue to propagate an error tag; the epilogue is defined with the
// instruction codegen below.
static void a64_emit_epilogue(ZBuf *text, unsigned frame_size, bool restore_process_args);
// OS-interface lowerings (raw `svc` syscalls): anonymous + file mmap, std.args.get. Defined with the
// instruction codegen below, used by the LOCAL_SET cases that populate allocator / Maybe / Mapping
// locals.
static bool a64_emit_anon_mmap_to_local(ZBuf *text, const IrFunction *fun, const IrValue *size, const IrLocal *local, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag);
static bool a64_emit_mmap_file_addr_size(ZBuf *text, const IrFunction *fun, const IrValue *path, unsigned depth, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag);
static bool a64_emit_args_get_to_local(ZBuf *text, const IrFunction *fun, const IrValue *value, const IrLocal *local, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag);
// The single call-argument marshaller (record->pointer / span->two-reg / scalar-float->value) is
// defined with the aggregate-ABI helpers below; the regular call path and the record-call path share it.
static bool a64_emit_marshal_call_arg(ZBuf *text, const IrFunction *fun, const IrValue *arg, unsigned *int_arg, unsigned *fp_arg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag);

static bool a64_emit_value_to_reg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  return a64_emit_value_to_reg_depth(text, fun, value, reg, frame_size, 0, ctx, diag);
}

static bool a64_emit_call_to_reg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (value->arg_len > 8) return a64_diag_at(diag, "direct AArch64 ELF call supports at most eight arguments", value->line, value->column, "too many arguments");
  // A record-returning call must receive its destination in x8; it is always routed through
  // a64_emit_record_call_with_dest (a record bind or return), never read as a plain scalar value. A
  // span-returning call leaves ptr in x0 / len in x1, which span-context callers consume directly.
  if (value->type == IR_TYPE_RECORD) {
    return a64_diag_at(diag, "direct AArch64 ELF record-returning call must bind to a record local or be returned", value->line, value->column, "unsupported record call position");
  }
  // Integer and FP arguments fill their own register banks (x0..x7 and v0..v7), each consumed by its
  // own counter (the AArch64 AAPCS); record args pass a pointer, span args pass (ptr,len) in two int
  // regs. Scratch lives in x8/x9 and v8/v9, none of which is an argument register, so earlier arguments
  // survive. An argument that is itself a spilling binary/compare must use a frame scratch slot at this
  // call's depth, not slot 0, lest it clobber an outer expression's spilled operand.
  unsigned int_arg = 0;
  unsigned fp_arg = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    if (!a64_emit_marshal_call_arg(text, fun, value->args[i], &int_arg, &fp_arg, frame_size, depth, ctx, diag)) return false;
  }
  size_t patch = a64_emit_bl_placeholder(text);
  if (!a64_record_call_patch(ctx, patch, value->callee_index, value, diag)) return false;
  if (a64_type_is_float(value->type)) {
    if (reg != 0) a64_emit_fmov_v(text, reg, 0, a64_type_is_f64(value->type)); // result in v0/s0/d0
  } else if (value->type != IR_TYPE_BYTE_VIEW && reg != 0) {
    // A span result rides in x0/x1 and the caller consumes it directly (it never asks for a single
    // scalar reg); a scalar/integer result is moved from x0/w0 into the requested register.
    if (a64_type_is_scalar64(value->type)) a64_emit_mov_x(text, reg, 0);
    else a64_emit_mov_w(text, reg, 0);
  }
  return true;
}

static bool a64_emit_value_to_reg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (!value) return a64_diag_at(diag, "direct AArch64 ELF expression is missing", 1, 1, "missing expression");
  switch (value->kind) {
    case IR_VALUE_BOOL:
    case IR_VALUE_INT:
      if (a64_type_is_scalar64(value->type)) a64_emit_mov_imm64(text, reg, (uint64_t)value->int_value);
      else a64_emit_movz_w(text, reg, (uint32_t)value->int_value);
      return true;
    case IR_VALUE_LOCAL:
      if (value->local_index >= fun->local_len) return a64_diag_at(diag, "direct AArch64 ELF local index is out of range", value->line, value->column, "invalid local");
      if (fun->locals[value->local_index].type == IR_TYPE_BYTE_VIEW) {
        return a64_diag_at(diag, "direct AArch64 ELF byte-view local cannot be used as a scalar", value->line, value->column, "byte-view local");
      }
      if (!a64_type_is_scalar(fun->locals[value->local_index].type)) {
        return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support loading this local kind (float/aggregate land in later phases)", value->line, value->column, "unsupported local load");
      }
      if (a64_type_is_scalar64(fun->locals[value->local_index].type)) a64_emit_load_local_x(text, fun, reg, value->local_index, 0, frame_size);
      else a64_emit_load_local_w(text, fun, reg, value->local_index, 0, frame_size);
      return true;
    case IR_VALUE_BINARY:
      if (value->binary_op == IR_BIN_AND) {
        if (!a64_emit_value_to_reg_depth(text, fun, value->left, reg, frame_size, depth, ctx, diag)) return false;
        size_t left_false = a64_emit_cbz_w_placeholder(text, reg);
        if (!a64_emit_value_to_reg_depth(text, fun, value->right, reg, frame_size, depth, ctx, diag)) return false;
        size_t right_false = a64_emit_cbz_w_placeholder(text, reg);
        a64_emit_movz_w(text, reg, 1);
        size_t end_patch = a64_emit_b_placeholder(text);
        a64_patch_cond19(text, left_false, text->len);
        a64_patch_cond19(text, right_false, text->len);
        a64_emit_movz_w(text, reg, 0);
        a64_patch_branch26(text, end_patch, text->len);
        return true;
      }
      if (value->binary_op == IR_BIN_OR) {
        if (!a64_emit_value_to_reg_depth(text, fun, value->left, reg, frame_size, depth, ctx, diag)) return false;
        size_t eval_right = a64_emit_cbz_w_placeholder(text, reg);
        a64_emit_movz_w(text, reg, 1);
        size_t left_true_end = a64_emit_b_placeholder(text);
        a64_patch_cond19(text, eval_right, text->len);
        if (!a64_emit_value_to_reg_depth(text, fun, value->right, reg, frame_size, depth, ctx, diag)) return false;
        size_t right_false = a64_emit_cbz_w_placeholder(text, reg);
        a64_emit_movz_w(text, reg, 1);
        size_t right_true_end = a64_emit_b_placeholder(text);
        a64_patch_cond19(text, right_false, text->len);
        a64_emit_movz_w(text, reg, 0);
        a64_patch_branch26(text, left_true_end, text->len);
        a64_patch_branch26(text, right_true_end, text->len);
        return true;
      }
      {
        // Spill the left operand to this depth's frame scratch slot across the right's evaluation
        // (the right may clobber x8 via a call). Width and signedness for DIV/MOD come off the result
        // type; bitwise ops still bail inside a64_emit_binary_int.
        unsigned slot = a64_int_scratch_slot_offset(fun, depth);
        bool wide = a64_type_is_scalar64(value->type);
        bool is_unsigned = a64_type_is_unsigned(value->type);
        if (!a64_emit_value_to_reg_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
        a64_emit_str_x_disp(text, 8, 31, slot);
        if (!a64_emit_value_to_reg_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
        a64_emit_ldr_x_disp(text, 8, 31, slot);
        if (!a64_emit_binary_int(text, value->binary_op, reg, 8, 9, wide, is_unsigned, diag, value)) return false;
      }
      return true;
    case IR_VALUE_COMPARE: {
      if (!value->left || !value->right) {
        return a64_diag_at(diag, "direct AArch64 ELF comparison requires two operands", value->line, value->column, "invalid comparison");
      }
      if (a64_type_is_float(value->left->type)) {
        // A float comparison's result is an integer boolean; its operands are floats. Spill the left
        // operand to an FP scratch slot across the right's evaluation, then FCMP and materialize the
        // IEEE-correct boolean with CSET. FP scratch is indexed by FP depth, independent of the
        // integer scratch counter, so depth 0 is correct (a float compare never nests under an
        // integer binary's spilled operand in the same register file).
        bool is64 = a64_type_is_f64(value->left->type);
        unsigned slot = a64_fp_scratch_slot_offset(0);
        if (!a64_emit_float_value_to_vreg_depth(text, fun, value->left, 8, frame_size, 1, ctx, diag)) return false;
        a64_emit_str_v_sp(text, 8, slot, is64);
        if (!a64_emit_float_value_to_vreg_depth(text, fun, value->right, 9, frame_size, 1, ctx, diag)) return false;
        a64_emit_ldr_v_sp(text, 8, slot, is64);
        a64_emit_fcmp(text, 8, 9, is64);
        a64_emit_cset(text, reg, a64_float_cond_for_compare(value->compare_op));
        return true;
      }
      unsigned slot = a64_int_scratch_slot_offset(fun, depth);
      bool wide = a64_type_is_scalar64(value->left->type);
      bool is_unsigned = a64_type_is_unsigned(value->left->type);
      if (!a64_emit_value_to_reg_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_str_x_disp(text, 8, 31, slot);
      if (!a64_emit_value_to_reg_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_ldr_x_disp(text, 8, 31, slot);
      if (wide) a64_emit_cmp_x(text, 8, 9);
      else a64_emit_cmp_w(text, 8, 9);
      a64_emit_movz_w(text, reg, 0);
      size_t false_patch = a64_emit_b_cond_placeholder(text, a64_invert_cond(a64_cond_for_compare(value->compare_op, is_unsigned)));
      a64_emit_movz_w(text, reg, 1);
      a64_patch_cond19(text, false_patch, text->len);
      return true;
    }
    case IR_VALUE_CAST: {
      // Casts only reach the IR when floats are involved. A float-typed result is produced in the FP
      // path (a64_emit_float_value_to_vreg); here we handle only conversions whose result is an
      // integer, i.e. float -> i32 via FCVTZS (round toward zero).
      if (!value->left) return a64_diag_at(diag, "direct AArch64 ELF cast missing operand", value->line, value->column, "missing cast operand");
      if (a64_type_is_float(value->type) || !a64_type_is_float(value->left->type)) {
        return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support this cast", value->line, value->column, "unsupported cast");
      }
      if (!a64_emit_float_value_to_vreg_depth(text, fun, value->left, 8, frame_size, 0, ctx, diag)) return false;
      a64_emit_fcvtzs_w(text, reg, 8, a64_type_is_f64(value->left->type));
      return true;
    }
    case IR_VALUE_MATH_ISNANF: {
      // NaN is the only value where x != x. FCMP sets V=1 (overflow) on an unordered compare, so
      // `cset reg, VS` yields 1 for NaN and 0 otherwise — inline, no libm call.
      if (!a64_emit_float_value_to_vreg_depth(text, fun, value->left, 8, frame_size, 0, ctx, diag)) return false;
      a64_emit_fcmp(text, 8, 8, a64_type_is_f64(value->left ? value->left->type : IR_TYPE_F32));
      a64_emit_cset(text, reg, 6); // VS (overflow set = unordered)
      return true;
    }
    case IR_VALUE_CALL:
      return a64_emit_call_to_reg_depth(text, fun, value, reg, frame_size, depth, ctx, diag);
    case IR_VALUE_FIELD_LOAD:
      // A scalar field of a record local: load by the field's width at the field offset.
      if (value->local_index >= fun->local_len) return a64_diag_at(diag, "direct AArch64 ELF field load record is out of range", value->line, value->column, "invalid record local");
      if (!fun->locals[value->local_index].is_record) return a64_diag_at(diag, "direct AArch64 ELF field load requires a record local", value->line, value->column, "non-record local");
      if (value->type == IR_TYPE_U8 || value->type == IR_TYPE_BOOL) a64_emit_load_local_b(text, fun, reg, value->local_index, value->field_offset, frame_size);
      else if (a64_type_is_scalar64(value->type)) a64_emit_load_local_x(text, fun, reg, value->local_index, value->field_offset, frame_size);
      else a64_emit_load_local_w(text, fun, reg, value->local_index, value->field_offset, frame_size);
      return true;
    case IR_VALUE_BYTE_VIEW_LEN:
      // std.mem.len(span): the element count, materialized via the byte-view length helper.
      return a64_emit_byte_view_len_depth(text, fun, value->left, reg, frame_size, depth, ctx, diag);
    case IR_VALUE_BYTE_VIEW_EQ: {
      // std.mem.eqlBytes(a, b): 1 if both views have the same length and bytes, else 0. Mirrors the
      // ELF backend's length-then-bytewise compare. Locals are sp-relative, so the values that must
      // outlive a sibling's evaluation are spilled to the integer scratch slot rather than pushed:
      // pointers are settled into stable scratch first, then the length last, so the loop's invariants
      // (both pointers in x10/x11, length in x12) survive without any further byte-view evaluation.
      // Sub-views are evaluated at depth+1 so a runtime-slice index never reuses our slot.
      if (!value->left || !value->right) return a64_diag_at(diag, "direct AArch64 ELF byte-view equality requires two byte views", value->line, value->column, "missing byte view");
      unsigned slot = a64_int_scratch_slot_offset(fun, depth);
      // Settle both pointers first (left in x10, right in x11).
      if (!a64_emit_byte_view_ptr_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_str_x_disp(text, 8, 31, slot);
      if (!a64_emit_byte_view_ptr_depth(text, fun, value->right, 11, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_ldr_x_disp(text, 10, 31, slot);
      // Compare lengths (left spilled across right). On mismatch the result is 0.
      if (!a64_emit_byte_view_len_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_str_x_disp(text, 8, 31, slot);
      if (!a64_emit_byte_view_len_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_ldr_x_disp(text, 8, 31, slot);
      a64_emit_cmp_w(text, 8, 9);
      size_t len_mismatch = a64_emit_b_cond_placeholder(text, 1); // NE
      a64_emit_mov_w(text, 12, 8); // length (counted-down loop bound) in x12
      // Byte loop: i in x13 from 0; compare left[i] vs right[i]; any mismatch -> 0, else fall to 1.
      a64_emit_movz_x(text, 13, 0);
      size_t loop = text->len;
      a64_emit_cmp_x(text, 13, 12);
      size_t loop_done = a64_emit_b_cond_placeholder(text, 2); // HS (i >= len): all matched
      a64_emit_add_x_reg(text, 14, 10, 13);
      a64_emit_ldrb_w(text, 14, 14);
      a64_emit_add_x_reg(text, 15, 11, 13);
      a64_emit_ldrb_w(text, 15, 15);
      a64_emit_cmp_w(text, 14, 15);
      size_t byte_mismatch = a64_emit_b_cond_placeholder(text, 1); // NE
      a64_emit_add_x_imm(text, 13, 13, 1);
      size_t back = a64_emit_b_placeholder(text);
      a64_patch_branch26(text, back, loop);
      // false: lengths differ or a byte differed.
      a64_patch_cond19(text, len_mismatch, text->len);
      a64_patch_cond19(text, byte_mismatch, text->len);
      a64_emit_movz_w(text, reg, 0);
      size_t end = a64_emit_b_placeholder(text);
      // true: every byte matched.
      a64_patch_cond19(text, loop_done, text->len);
      a64_emit_movz_w(text, reg, 1);
      a64_patch_branch26(text, end, text->len);
      return true;
    }
    case IR_VALUE_BYTE_COPY: {
      // std.mem.copy(dst, src): copy min(src.len, dst.len) bytes and return the count. value->left is
      // the source view, value->right the destination. Like byte-view equality, locals are sp-relative
      // so the pointers and length are settled into stable registers (src in x10, dst in x11, count in
      // x12) via the integer scratch slot before the byte loop; the loop body touches only x13/x14.
      // Sub-views are evaluated at depth+1 so a runtime slice never reuses our slot.
      if (!value->left || !value->right) return a64_diag_at(diag, "direct AArch64 ELF byte copy requires source and destination byte views", value->line, value->column, "missing byte view");
      unsigned slot = a64_int_scratch_slot_offset(fun, depth);
      // Settle source pointer (x10) and destination pointer (x11).
      if (!a64_emit_byte_view_ptr_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_str_x_disp(text, 8, 31, slot);
      if (!a64_emit_byte_view_ptr_depth(text, fun, value->right, 11, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_ldr_x_disp(text, 10, 31, slot);
      // count = min(src.len, dst.len) into x12 (src.len spilled across dst.len's evaluation).
      if (!a64_emit_byte_view_len_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_str_x_disp(text, 8, 31, slot);
      if (!a64_emit_byte_view_len_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_ldr_x_disp(text, 8, 31, slot);
      a64_emit_mov_w(text, 12, 8);
      a64_emit_cmp_w(text, 9, 12);
      size_t keep_src_len = a64_emit_b_cond_placeholder(text, 2); // HS: dst.len >= src.len, keep src.len
      a64_emit_mov_w(text, 12, 9);                                // else count = dst.len
      a64_patch_cond19(text, keep_src_len, text->len);
      // Byte loop: i in x13 from 0; while i < count copy src[i] -> dst[i].
      a64_emit_movz_x(text, 13, 0);
      size_t loop = text->len;
      a64_emit_cmp_x(text, 13, 12);
      size_t loop_done = a64_emit_b_cond_placeholder(text, 2); // HS (i >= count)
      a64_emit_add_x_reg(text, 14, 10, 13);
      a64_emit_ldrb_w(text, 14, 14);
      a64_emit_add_x_reg(text, 15, 11, 13);
      a64_emit_strb_w(text, 14, 15);
      a64_emit_add_x_imm(text, 13, 13, 1);
      size_t back = a64_emit_b_placeholder(text);
      a64_patch_branch26(text, back, loop);
      a64_patch_cond19(text, loop_done, text->len);
      a64_emit_mov_w(text, reg, 12); // result = bytes copied
      return true;
    }
    case IR_VALUE_BYTE_FILL: {
      // std.mem.fill(dst, byte): write `byte` across the whole destination view and return its length.
      // value->left is the u8 fill value, value->right the destination. The fill byte and the length
      // are settled into stable registers (fill in x14, length in x12) via the scratch slot, then the
      // loop body touches only x13.
      if (!value->left || !value->right) return a64_diag_at(diag, "direct AArch64 ELF byte fill requires a destination byte view", value->line, value->column, "missing byte view");
      unsigned slot = a64_int_scratch_slot_offset(fun, depth);
      // Destination pointer in x11.
      if (!a64_emit_byte_view_ptr_depth(text, fun, value->right, 11, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_str_x_disp(text, 11, 31, slot);
      // Fill value (low byte) in x14; reload the pointer afterwards in case its evaluation clobbered x11.
      if (!a64_emit_value_to_reg_depth(text, fun, value->left, 14, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_ldr_x_disp(text, 11, 31, slot);
      // Length into x12 (the pointer is held in our slot across this evaluation).
      if (!a64_emit_byte_view_len_depth(text, fun, value->right, 12, frame_size, depth + 1, ctx, diag)) return false;
      // Byte loop: i in x13 from 0; while i < len store the fill byte to dst[i].
      a64_emit_movz_x(text, 13, 0);
      size_t loop = text->len;
      a64_emit_cmp_x(text, 13, 12);
      size_t loop_done = a64_emit_b_cond_placeholder(text, 2); // HS (i >= len)
      a64_emit_add_x_reg(text, 15, 11, 13);
      a64_emit_strb_w(text, 14, 15);
      a64_emit_add_x_imm(text, 13, 13, 1);
      size_t back = a64_emit_b_placeholder(text);
      a64_patch_branch26(text, back, loop);
      a64_patch_cond19(text, loop_done, text->len);
      a64_emit_mov_w(text, reg, 12); // result = bytes filled (destination length)
      return true;
    }
    case IR_VALUE_BYTE_VIEW_INDEX_LOAD: {
      // u8 byte-view element read (the byte-granular index path). A constant index into a constant
      // view folds at compile time; otherwise bounds-check idx < len at runtime, then ldrb.
      unsigned const_index = 0;
      unsigned char byte = 0;
      if (a64_const_u32_value(value->index, &const_index) &&
          a64_byte_view_const_byte(ctx ? ctx->ir : NULL, value->left, const_index, &byte)) {
        a64_emit_movz_w(text, reg, byte);
        return true;
      }
      if (!value->index || !a64_emit_value_to_reg_depth(text, fun, value->index, 8, frame_size, depth, ctx, diag)) return false;
      if (!a64_emit_byte_view_len_depth(text, fun, value->left, 9, frame_size, depth, ctx, diag)) return false;
      a64_emit_cmp_w(text, 8, 9);
      size_t ok_patch = a64_emit_b_cond_placeholder(text, 3); // unsigned lower
      a64_emit_word(text, 0xd4200000u); // brk #0
      a64_patch_cond19(text, ok_patch, text->len);
      if (!a64_emit_byte_view_ptr_depth(text, fun, value->left, 9, frame_size, depth, ctx, diag)) return false;
      a64_emit_add_x_reg(text, 9, 9, 8);
      a64_emit_ldrb_w(text, reg, 9);
      return true;
    }
    case IR_VALUE_BYTE_VIEW_READ_INT_LE: {
      // std.codec.readI32Le / readU32Le: read 4 little-endian bytes at a byte offset. AArch64 is
      // little-endian, so a 32-bit load at ptr+offset already yields the value. Bounds-check that
      // offset+4 fits within the span length before loading.
      if (!value->left) return a64_diag_at(diag, "direct AArch64 ELF readI32Le/readU32Le requires a byte view", value->line, value->column, "missing byte view");
      if (!value->index) return a64_diag_at(diag, "direct AArch64 ELF readI32Le/readU32Le requires an offset", value->line, value->column, "missing offset");
      if (!a64_emit_value_to_reg_depth(text, fun, value->index, 8, frame_size, depth, ctx, diag)) return false;
      a64_emit_add_w_imm(text, 10, 8, 4); // offset + 4
      if (!a64_emit_byte_view_len_depth(text, fun, value->left, 9, frame_size, depth, ctx, diag)) return false;
      a64_emit_cmp_w(text, 10, 9);
      size_t ok_patch = a64_emit_b_cond_placeholder(text, 9); // unsigned lower or same
      a64_emit_word(text, 0xd4200000u); // brk #0
      a64_patch_cond19(text, ok_patch, text->len);
      if (!a64_emit_byte_view_ptr_depth(text, fun, value->left, 9, frame_size, depth, ctx, diag)) return false;
      a64_emit_add_x_reg(text, 9, 9, 8);
      a64_emit_ldr_w(text, reg, 9);
      return true;
    }
    case IR_VALUE_INDEX_LOAD: {
      if (value->array_index >= fun->local_len) return a64_diag_at(diag, "direct AArch64 ELF indexed load array is out of range", value->line, value->column, "invalid array local");
      const IrLocal *local = &fun->locals[value->array_index];
      if (local->type == IR_TYPE_BYTE_VIEW) {
        // Typed-span element read: bounds-check against the runtime length, compute ptr+idx*size, then
        // load by element width (u8 zero-extends, i8 sign-extends, i32/u32 via w, 8-byte via x).
        if (!a64_emit_span_index_addr_depth(text, fun, value->array_index, value->index, local->element_type, frame_size, depth, ctx, diag)) return false;
        if (local->element_type == IR_TYPE_U8) a64_emit_ldrb_w(text, reg, 9);
        else if (local->element_type == IR_TYPE_I8) a64_emit_ldrsb_w(text, reg, 9);
        else if (a64_elem_byte_size(local->element_type) == 8) a64_emit_ldr_x_disp(text, reg, 9, 0);
        else a64_emit_ldr_w(text, reg, 9);
        return true;
      }
      // Fixed integer array element read. A constant index folds to a direct frame load; a runtime
      // index bounds-checks against the array length then loads at base + idx*4.
      unsigned const_index = 0;
      if (local->is_array && local->element_type != IR_TYPE_U8 && a64_const_u32_value(value->index, &const_index) && const_index < local->array_len) {
        a64_emit_load_local_w(text, fun, reg, value->array_index, const_index * 4u, frame_size);
        return true;
      }
      if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
        if (!value->index || !a64_emit_value_to_reg_depth(text, fun, value->index, 8, frame_size, depth, ctx, diag)) return false;
        a64_emit_movz_w(text, 9, local->array_len);
        a64_emit_cmp_w(text, 8, 9);
        size_t ok_patch = a64_emit_b_cond_placeholder(text, 3); // unsigned lower
        a64_emit_word(text, 0xd4200000u); // brk #0
        a64_patch_cond19(text, ok_patch, text->len);
        a64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, value->array_index, 0, frame_size));
        a64_emit_add_x_reg_lsl(text, 9, 9, 8, 2);
        a64_emit_ldr_w(text, reg, 9);
        return true;
      }
      if (!local->is_array || local->element_type != IR_TYPE_U8) return a64_diag_at(diag, "direct AArch64 ELF indexed load requires [N]u8 or integer arrays", value->line, value->column, "unsupported array local");
      if (!value->index || !a64_emit_value_to_reg_depth(text, fun, value->index, 8, frame_size, depth, ctx, diag)) return false;
      a64_emit_movz_w(text, 9, local->array_len);
      a64_emit_cmp_w(text, 8, 9);
      size_t ok_patch = a64_emit_b_cond_placeholder(text, 3); // unsigned lower
      a64_emit_word(text, 0xd4200000u); // brk #0
      a64_patch_cond19(text, ok_patch, text->len);
      a64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, value->array_index, 0, frame_size));
      a64_emit_add_x_reg(text, 9, 9, 8);
      a64_emit_ldrb_w(text, reg, 9);
      return true;
    }
    case IR_VALUE_ARGS_LEN:
      // argc is seeded into the callee-saved x20 by the prologue (from x0). std.args.len reports it.
      a64_emit_mov_w(text, reg, 20);
      return true;
    case IR_VALUE_FS_HOST:
      // std.fs.host() yields the host filesystem handle, an i32 token that carries no state on a hosted
      // target (the OS-interface lowerings issue syscalls directly). Mirrors the ELF/macho backends,
      // which return 0.
      a64_emit_movz_w(text, reg, 0);
      return true;
    case IR_VALUE_FS_MUNMAP: {
      // std.fs.munmap(&mut m): release a mapping via the munmap syscall (addr in x0, len in x1, number
      // 215 in x8). The Mapping local stores ptr@0 and len@8 both 64-bit (matching the file-mmap store),
      // so both load with the x form. Result type is Void; nothing is produced into `reg`.
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_BYTE_VIEW) {
        return a64_diag_at(diag, "direct AArch64 ELF std.fs.munmap requires a Mapping local", value->line, value->column, "invalid Mapping");
      }
      a64_emit_load_local_x(text, fun, 0, value->local_index, 0, frame_size); // addr
      a64_emit_load_local_x(text, fun, 1, value->local_index, 8, frame_size); // len
      a64_emit_svc(text, 215); // munmap
      return true;
    }
    case IR_VALUE_MAYBE_HAS:
      if (value->local_index >= fun->local_len ||
          (fun->locals[value->local_index].type != IR_TYPE_MAYBE_BYTE_VIEW && fun->locals[value->local_index].type != IR_TYPE_MAYBE_SCALAR)) {
        return a64_diag_at(diag, "direct AArch64 ELF maybe helper requires a Maybe local", value->line, value->column, "invalid maybe local");
      }
      a64_emit_load_local_w(text, fun, reg, value->local_index, 0, frame_size); // has @ slot+0
      return true;
    case IR_VALUE_MAYBE_VALUE:
      // The scalar payload of a Maybe<scalar> lives at slot+8 (a byte-view Maybe's value is handled by
      // the byte-view ptr/len paths, not here).
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_MAYBE_SCALAR) {
        return a64_diag_at(diag, "direct AArch64 ELF maybe scalar value requires a Maybe scalar local", value->line, value->column, "invalid maybe value");
      }
      a64_emit_load_local_x(text, fun, reg, value->local_index, 8, frame_size);
      return true;
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported value kind %d", (int)value->kind);
      return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support this expression kind", value->line, value->column, actual);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Floating-point expression codegen. Materializes an f32/f64 value into NEON register `vreg` (s_ for
// f32, d_ for f64). The FP sibling of a64_emit_value_to_reg_depth; integer operands inside it (a
// cast source, an array index) call back into the integer emitter — the x* and v* register files
// never overlap, so interleaving is safe. `depth` is the FP binary/compare nesting level. Ported from
// emit_macho64.c's macho_emit_float_value_to_vreg_depth; covers FLOAT const, LOCAL, CAST, BINARY
// (fadd/fsub/fmul/fdiv), float-returning CALL, libm, `check`, float typed-span / array INDEX_LOAD,
// and the codec readF*Le. Record/span fields land in the aggregate-ABI phase.

// Load an f32/f64 element of a span (byte-view) local or a fixed [N]f32 / [N]f64 array into `vreg`.
// Span elements bounds-check at runtime via a64_emit_span_index_addr; array elements bounds-check
// against the array length (a constant index folds to a direct frame load). Ported from
// emit_macho64.c's macho_emit_float_index_load.
static bool a64_emit_float_index_load(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (value->array_index >= fun->local_len) return a64_diag_at(diag, "direct AArch64 ELF indexed load array is out of range", value->line, value->column, "invalid array local");
  const IrLocal *local = &fun->locals[value->array_index];
  if (local->type == IR_TYPE_BYTE_VIEW) {
    if (!a64_type_is_float(local->element_type)) return a64_diag_at(diag, "direct AArch64 ELF float indexed load requires a float span", value->line, value->column, "unsupported span element");
    if (!a64_emit_span_index_addr(text, fun, value->array_index, value->index, local->element_type, frame_size, ctx, diag)) return false;
    a64_emit_ldr_v_base(text, vreg, 9, a64_type_is_f64(local->element_type));
    return true;
  }
  if (!local->is_array || !a64_type_is_float(local->element_type)) {
    return a64_diag_at(diag, "direct AArch64 ELF float indexed load requires a float array", value->line, value->column, "unsupported array local");
  }
  bool is64 = a64_type_is_f64(local->element_type);
  unsigned const_index = 0;
  if (a64_const_u32_value(value->index, &const_index) && const_index < local->array_len) {
    a64_emit_ldr_v_local(text, fun, vreg, value->array_index, const_index * (is64 ? 8u : 4u), frame_size, is64);
    return true;
  }
  if (!value->index || !a64_emit_value_to_reg(text, fun, value->index, 8, frame_size, ctx, diag)) return false;
  a64_emit_movz_w(text, 9, local->array_len);
  a64_emit_cmp_w(text, 8, 9);
  size_t ok_patch = a64_emit_b_cond_placeholder(text, 3); // unsigned lower
  a64_emit_word(text, 0xd4200000u); // brk #0
  a64_patch_cond19(text, ok_patch, text->len);
  a64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, value->array_index, 0, frame_size));
  a64_emit_ldr_v_reg_lsl(text, vreg, 9, 8, is64);
  return true;
}

// Store `vreg` into an f32/f64 element of a fixed [N]f32 / [N]f64 array local (the span store path is
// handled inline in IR_INSTR_INDEX_STORE via a64_emit_span_index_addr). Same bounds check + scaled
// register offset as the float load. Ported from emit_macho64.c's macho_emit_float_index_store.
static bool a64_emit_float_index_store(ZBuf *text, const IrFunction *fun, const IrInstr *instr, const IrLocal *local, unsigned vreg, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  bool is64 = a64_type_is_f64(local->element_type);
  unsigned const_index = 0;
  if (a64_const_u32_value(instr->index, &const_index) && const_index < local->array_len) {
    a64_emit_str_v_local(text, fun, vreg, instr->array_index, const_index * (is64 ? 8u : 4u), frame_size, is64);
    return true;
  }
  if (!instr->index || !a64_emit_value_to_reg(text, fun, instr->index, 8, frame_size, ctx, diag)) return false;
  a64_emit_movz_w(text, 9, local->array_len);
  a64_emit_cmp_w(text, 8, 9);
  size_t ok_patch = a64_emit_b_cond_placeholder(text, 3); // unsigned lower
  a64_emit_word(text, 0xd4200000u); // brk #0
  a64_patch_cond19(text, ok_patch, text->len);
  a64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, instr->array_index, 0, frame_size));
  a64_emit_str_v_reg_lsl(text, vreg, 9, 8, is64);
  return true;
}

static bool a64_emit_float_value_to_vreg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  return a64_emit_float_value_to_vreg_depth(text, fun, value, vreg, frame_size, 0, ctx, diag);
}

static bool a64_emit_float_value_to_vreg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (!value) return a64_diag_at(diag, "direct AArch64 ELF float expression is missing", 1, 1, "missing expression");
  bool is64 = a64_type_is_f64(value->type);
  switch (value->kind) {
    case IR_VALUE_FLOAT: {
      // Materialize the IEEE bit pattern in an integer scratch register, then FMOV into the FP
      // register. f32 patterns fit in w8 (movz/movk); f64 patterns need the full x8 sequence.
      if (is64) {
        a64_emit_mov_imm64(text, 8, (uint64_t)value->int_value);
        a64_emit_fmov_v_from_gpr(text, vreg, 8, true);
      } else {
        a64_emit_movz_w(text, 8, (uint32_t)value->int_value);
        a64_emit_fmov_v_from_gpr(text, vreg, 8, false);
      }
      return true;
    }
    case IR_VALUE_LOCAL:
      if (value->local_index >= fun->local_len) return a64_diag_at(diag, "direct AArch64 ELF local index is out of range", value->line, value->column, "invalid local");
      if (!a64_type_is_float(fun->locals[value->local_index].type)) {
        return a64_diag_at(diag, "direct AArch64 ELF float load requires a float local", value->line, value->column, "non-float local");
      }
      a64_emit_ldr_v_local(text, fun, vreg, value->local_index, 0, frame_size, is64);
      return true;
    case IR_VALUE_CAST: {
      if (!value->left) return a64_diag_at(diag, "direct AArch64 ELF cast missing operand", value->line, value->column, "missing cast operand");
      IrTypeKind src = value->left->type;
      if (a64_type_is_float(src)) {
        // float -> float: widen/narrow when the widths differ, otherwise a plain load.
        if (!a64_emit_float_value_to_vreg_depth(text, fun, value->left, vreg, frame_size, depth, ctx, diag)) return false;
        if (a64_type_is_f64(src) != is64) a64_emit_fcvt(text, vreg, vreg, is64);
        return true;
      }
      // integer -> float: signed i32 via SCVTF from w_, unsigned u64 via UCVTF from x_.
      if (!a64_emit_value_to_reg(text, fun, value->left, 8, frame_size, ctx, diag)) return false;
      if (a64_type_is_scalar64(src) && a64_type_is_unsigned(src)) a64_emit_ucvtf_from_x(text, vreg, 8, is64);
      else a64_emit_scvtf_from_w(text, vreg, 8, is64);
      return true;
    }
    case IR_VALUE_BINARY: {
      if (value->binary_op != IR_BIN_ADD && value->binary_op != IR_BIN_SUB && value->binary_op != IR_BIN_MUL && value->binary_op != IR_BIN_DIV) {
        return a64_diag_at(diag, "direct AArch64 ELF float binary operator is unsupported", value->line, value->column, "unsupported operator");
      }
      // Evaluate the left operand, spill it to this depth's FP scratch slot, evaluate the right
      // (which may nest deeper or use the v8/v9 scratch — the frame slot survives), then reload and
      // combine. fadd/fsub/fmul/fdiv are all single instructions with no trap.
      unsigned slot = a64_fp_scratch_slot_offset(depth);
      if (!a64_emit_float_value_to_vreg_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_str_v_sp(text, 8, slot, is64);
      if (!a64_emit_float_value_to_vreg_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_ldr_v_sp(text, 8, slot, is64);
      if (!a64_emit_float_arith(text, value->binary_op, vreg, 8, 9, is64)) {
        return a64_diag_at(diag, "direct AArch64 ELF float binary operator is unsupported", value->line, value->column, "unsupported operator");
      }
      return true;
    }
    case IR_VALUE_CALL:
      // A float-returning Zero function: argument marshaling and the v0 result move are handled by
      // a64_emit_call_to_reg_depth, which keys the result bank off the value's float type. Thread the
      // FP nesting depth so a float argument that is itself a spilling binary uses a deeper scratch slot.
      return a64_emit_call_to_reg_depth(text, fun, value, vreg, frame_size, depth, ctx, diag);
    case IR_VALUE_CHECK: {
      // `check <float fallible call>`: the callee leaves its f32/f64 result in v0 and an error tag in
      // x1 (the fallible ABI — the AAPCS analog of ELF's rdx tag). Evaluate into v0, test the tag, and
      // on error propagate by returning (the tag already in x1) from this raising context, mirroring
      // ELF's check. On success the value sits in v0; move it to vreg if needed.
      if (!value->left) return a64_diag_at(diag, "direct AArch64 ELF check requires a fallible call result", value->line, value->column, "non-fallible value");
      if (!a64_emit_float_value_to_vreg_depth(text, fun, value->left, 0, frame_size, depth, ctx, diag)) return false;
      size_t ok = a64_emit_cbz_w_placeholder(text, 1); // x1 == 0 -> no error (tested via its low word)
      bool seed = fun->is_exported && fun->name && strcmp(fun->name, "main") == 0;
      a64_emit_epilogue(text, frame_size, seed); // error: x1 already holds the tag; propagate it
      a64_patch_cond19(text, ok, text->len);
      if (vreg != 0) a64_emit_fmov_v(text, vreg, 0, is64);
      return true;
    }
    case IR_VALUE_MATH_SQRTF:
    case IR_VALUE_MATH_EXPF:
    case IR_VALUE_MATH_COSF:
    case IR_VALUE_MATH_SINF:
    case IR_VALUE_MATH_FABSF:
    case IR_VALUE_MATH_FLOORF: {
      // Single-arg libm call: argument in s0, result in s0 (AAPCS FP ABI). All std.math libm helpers
      // operate on f32. The `bl` placeholder is resolved by the host linker via an R_AARCH64_CALL26
      // relocation against the bare ELF symbol (sqrtf/expf/…); the FP scratch survives the call (it is
      // frame memory, not a caller-saved register). Only the object path can bind these externals, so
      // a libm program is routed through obj+link by the dispatch (ir_needs_zero_runtime_object).
      if (!a64_emit_float_value_to_vreg_depth(text, fun, value->left, 0, frame_size, depth, ctx, diag)) return false;
      size_t patch = a64_emit_bl_placeholder(text);
      if (!a64_record_math_call_patch(ctx, patch, a64_math_symbol_for_value(value->kind), value, diag)) return false;
      if (vreg != 0) a64_emit_fmov_v(text, vreg, 0, false);
      return true;
    }
    case IR_VALUE_MATH_POWF: {
      // Two-arg libm call: arg0 in s0, arg1 in s1. Compute arg0 into s0, spill it to the depth scratch
      // slot, compute arg1 into s1 (its evaluation may use the FP scratch or call libm), then reload
      // arg0 into s0 just before the call.
      unsigned slot = a64_fp_scratch_slot_offset(depth);
      if (!a64_emit_float_value_to_vreg_depth(text, fun, value->left, 0, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_str_v_sp(text, 0, slot, false);
      if (!a64_emit_float_value_to_vreg_depth(text, fun, value->right, 1, frame_size, depth + 1, ctx, diag)) return false;
      a64_emit_ldr_v_sp(text, 0, slot, false);
      size_t patch = a64_emit_bl_placeholder(text);
      if (!a64_record_math_call_patch(ctx, patch, A64_MATH_POWF, value, diag)) return false;
      if (vreg != 0) a64_emit_fmov_v(text, vreg, 0, false);
      return true;
    }
    case IR_VALUE_INDEX_LOAD:
      return a64_emit_float_index_load(text, fun, value, vreg, frame_size, ctx, diag);
    case IR_VALUE_BYTE_VIEW_READ_FLOAT_LE: {
      // std.codec.readF32Le / readF64Le: 4 or 8 little-endian bytes at a byte offset. AArch64 is
      // little-endian, so an FP load at ptr+offset yields the value; bounds-check offset+size first.
      if (!value->left) return a64_diag_at(diag, "direct AArch64 ELF readF*Le requires a byte view", value->line, value->column, "missing byte view");
      if (!value->index) return a64_diag_at(diag, "direct AArch64 ELF readF*Le requires an offset", value->line, value->column, "missing offset");
      unsigned scalar_size = is64 ? 8u : 4u;
      if (!a64_emit_value_to_reg(text, fun, value->index, 8, frame_size, ctx, diag)) return false;
      a64_emit_add_w_imm(text, 10, 8, scalar_size); // offset + size
      if (!a64_emit_byte_view_len(text, fun, value->left, 9, frame_size, ctx, diag)) return false;
      a64_emit_cmp_w(text, 10, 9);
      size_t ok_patch = a64_emit_b_cond_placeholder(text, 9); // unsigned lower or same
      a64_emit_word(text, 0xd4200000u); // brk #0
      a64_patch_cond19(text, ok_patch, text->len);
      if (!a64_emit_byte_view_ptr(text, fun, value->left, 9, frame_size, ctx, diag)) return false;
      a64_emit_add_x_reg(text, 9, 9, 8);
      a64_emit_ldr_v_base(text, vreg, 9, is64);
      return true;
    }
    case IR_VALUE_FIELD_LOAD:
      // A float-typed field of a record local: load by width at the field offset (the FP sibling of
      // the integer FIELD_LOAD). Mirrors emit_macho64.c's macho_emit_ldr_v_field.
      if (value->local_index >= fun->local_len) return a64_diag_at(diag, "direct AArch64 ELF field load record is out of range", value->line, value->column, "invalid record local");
      if (!fun->locals[value->local_index].is_record) return a64_diag_at(diag, "direct AArch64 ELF field load requires a record local", value->line, value->column, "non-record local");
      a64_emit_ldr_v_local(text, fun, vreg, value->local_index, value->field_offset, frame_size, is64);
      return true;
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported float value kind %d", value ? (int)value->kind : -1);
      return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support this float value kind", value->line, value->column, actual);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Aggregate ABI (sret via x8, span/record params + returns, record copy, span-in-record). Ported
// from emit_macho64.c's P4 layer under the `a64_` prefix — arm64 is arm64, so the encodings are
// byte-identical; only the diagnostic strings change. The AArch64 AAPCS differs from ELF/x86 in two
// ways the macho campaign already settled: the indirect-result pointer rides in x8 (not an argument
// register, so the 8-int arg cap is unaffected — unlike ELF's rdi-sret 5-int cap), and a record/span
// argument or parameter uses the integer register bank (records by pointer, spans as a (ptr,len)
// pair). The integer left-spill discipline (per-depth frame slots) is already in place from P1/P4
// and wraps any spilling op, so call args evaluated at a given nesting depth take deeper slots.

// Store a scalar value into a field of a record local at its field offset, by the field's width
// (u8/Bool via STRB, i64/u64/usize via STR x, else STR w). The byte-store sibling of the FIELD_LOAD
// path. Used when building a record local (e.g. a record argument materialized into a temp).
static void a64_emit_store_field(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned field_offset, IrTypeKind type, unsigned frame_size) {
  if (type == IR_TYPE_U8 || type == IR_TYPE_I8 || type == IR_TYPE_BOOL) {
    a64_emit_store_local_b(text, fun, reg, local_index, field_offset, frame_size);
  } else if (a64_type_is_scalar64(type)) {
    a64_emit_store_local_x(text, fun, reg, local_index, field_offset, frame_size);
  } else {
    a64_emit_store_local_w(text, fun, reg, local_index, field_offset, frame_size);
  }
}

// Store an f32/f64 value into a record local's float field (a thin wrapper over the FP local store at
// the field offset).
static void a64_emit_str_v_field(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned field_offset, unsigned frame_size, bool is64) {
  a64_emit_str_v_local(text, fun, vreg, local_index, field_offset, frame_size, is64);
}

// ADD reg, sp, #localoff — materialize the address of a frame-local into a GPR. Used to pass a record
// by pointer and to set up source/destination pointers for a record copy. Locals live at the top of
// the frame, addressed exactly as the load/store helpers do.
static void a64_emit_lea_local_addr(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned reg, unsigned frame_size) {
  a64_emit_add_x_sp_imm(text, reg, a64_local_slot_offset(fun, local_index, 0, frame_size));
}

// memcpy a record value between inline frame slots: copy the source local's bytes to a destination —
// another local's slot, or the caller's sret buffer when dest_index == UINT_MAX. x8/x9 are scratch;
// 8-byte chunks then a 4-byte tail (records are 8- or 4-aligned, so the tail is exact). Span fields
// ride along as raw 16 bytes (ptr then len), preserving the view. Mirrors macho_emit_record_copy_to.
static void a64_emit_record_copy_to(ZBuf *text, const IrFunction *fun, unsigned dest_index, unsigned src_index, unsigned frame_size) {
  unsigned size = src_index < fun->local_len ? fun->locals[src_index].byte_size : 0;
  if (dest_index == UINT_MAX) {
    a64_emit_ldr_x_disp(text, 8, 31, a64_sret_slot_offset(fun)); // x8 = caller's sret pointer
  } else {
    a64_emit_lea_local_addr(text, fun, dest_index, 8, frame_size); // x8 = &dest
  }
  a64_emit_lea_local_addr(text, fun, src_index, 9, frame_size); // x9 = &src
  unsigned k = 0;
  while (k + 8 <= size) {
    a64_emit_ldr_x_disp(text, 10, 9, k);
    a64_emit_str_x_disp(text, 10, 8, k);
    k += 8;
  }
  if (k + 4 <= size) {
    a64_emit_ldr_w_disp(text, 10, 9, k);
    a64_emit_str_w_disp(text, 10, 8, k);
  }
}

// Copy a record param (passed by pointer in ptr_reg = x0..x7) into its inline frame slot, preserving
// value semantics. x9/x10 are scratch; 8-byte chunks then a 4-byte tail. Mirrors macho_emit_copy_record_param.
static void a64_emit_copy_record_param(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned ptr_reg, unsigned frame_size) {
  unsigned size = local_index < fun->local_len ? fun->locals[local_index].byte_size : 0;
  a64_emit_lea_local_addr(text, fun, local_index, 9, frame_size); // x9 = &slot
  unsigned k = 0;
  while (k + 8 <= size) {
    a64_emit_ldr_x_disp(text, 10, ptr_reg, k);
    a64_emit_str_x_disp(text, 10, 9, k);
    k += 8;
  }
  if (k + 4 <= size) {
    a64_emit_ldr_w_disp(text, 10, ptr_reg, k);
    a64_emit_str_w_disp(text, 10, 9, k);
  }
}

// Marshal one call argument into its destination register bank. Records pass a pointer to the
// argument's slot (record args are pre-materialized into locals, so each is an addressable local);
// spans pass (ptr,len) in two consecutive int regs; scalars/floats go through the value emitters.
// `int_arg`/`fp_arg` are advanced past the registers consumed. Scratch (x8/x9, v8/v9) never overlaps
// an arg reg, so already-marshalled arguments survive. Mirrors macho_emit_marshal_call_arg.
static bool a64_emit_marshal_call_arg(ZBuf *text, const IrFunction *fun, const IrValue *arg, unsigned *int_arg, unsigned *fp_arg, unsigned frame_size, unsigned depth, Aarch64EmitContext *ctx, ZDiag *diag) {
  IrTypeKind atype = arg ? arg->type : IR_TYPE_I32;
  if (a64_type_is_float(atype)) {
    if (*fp_arg > 7) return a64_diag_at(diag, "direct AArch64 ELF call supports at most eight floating-point arguments", arg ? arg->line : 1, arg ? arg->column : 1, "too many arguments");
    if (!a64_emit_float_value_to_vreg_depth(text, fun, arg, *fp_arg, frame_size, depth, ctx, diag)) return false;
    (*fp_arg)++;
    return true;
  }
  if (atype == IR_TYPE_BYTE_VIEW) {
    if (*int_arg > 6) return a64_diag_at(diag, "direct AArch64 ELF call supports at most eight integer arguments", arg ? arg->line : 1, arg ? arg->column : 1, "too many arguments");
    if (!a64_emit_byte_view_ptr_depth(text, fun, arg, *int_arg, frame_size, depth, ctx, diag)) return false;
    (*int_arg)++;
    if (!a64_emit_byte_view_len_depth(text, fun, arg, *int_arg, frame_size, depth, ctx, diag)) return false;
    (*int_arg)++;
    return true;
  }
  if (atype == IR_TYPE_RECORD) {
    if (*int_arg > 7) return a64_diag_at(diag, "direct AArch64 ELF call supports at most eight integer arguments", arg ? arg->line : 1, arg ? arg->column : 1, "too many arguments");
    if (!arg || arg->kind != IR_VALUE_LOCAL || arg->local_index >= fun->local_len) {
      return a64_diag_at(diag, "direct AArch64 ELF record argument here must be a record local (a record call/literal argument is only supported in straight-line statements, not loop conditions or short-circuit operands)", arg ? arg->line : 1, arg ? arg->column : 1, "non-local record arg");
    }
    a64_emit_lea_local_addr(text, fun, arg->local_index, *int_arg, frame_size);
    (*int_arg)++;
    return true;
  }
  if (*int_arg > 7) return a64_diag_at(diag, "direct AArch64 ELF call supports at most eight integer arguments", arg ? arg->line : 1, arg ? arg->column : 1, "too many arguments");
  if (!a64_emit_value_to_reg_depth(text, fun, arg, *int_arg, frame_size, depth, ctx, diag)) return false;
  (*int_arg)++;
  return true;
}

// A record-returning call written into a destination buffer: marshal the call's own arguments into
// x0.., then set the destination address in x8 (the AAPCS indirect-result register), then bl.
// `dest_local` >= 0 is a record local's slot (`let x = f()`); `dest_local` < 0 is the current
// function's own sret buffer (`return f()`), so the callee writes straight through to our caller's
// storage. x8 is set last because the argument emitters use it as scratch. Mirrors
// macho_emit_record_call_with_dest.
static bool a64_emit_record_call_with_dest(ZBuf *text, const IrFunction *fun, int dest_local, const IrValue *value, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (value->arg_len > 8) return a64_diag_at(diag, "direct AArch64 ELF call supports at most eight arguments", value->line, value->column, "too many arguments");
  unsigned int_arg = 0;
  unsigned fp_arg = 0;
  // Record calls are only emitted from straight-line statements (a record bind or return), never as a
  // nested operand, so their arguments evaluate at depth 0.
  for (size_t i = 0; i < value->arg_len; i++) {
    if (!a64_emit_marshal_call_arg(text, fun, value->args[i], &int_arg, &fp_arg, frame_size, 0, ctx, diag)) return false;
  }
  if (dest_local >= 0) a64_emit_lea_local_addr(text, fun, (unsigned)dest_local, 8, frame_size);
  else a64_emit_ldr_x_disp(text, 8, 31, a64_sret_slot_offset(fun)); // x8 = caller's sret pointer
  size_t patch = a64_emit_bl_placeholder(text);
  return a64_record_call_patch(ctx, patch, value->callee_index, value, diag);
}

// Store one field of the record being returned, written through the caller's sret pointer (saved in
// the frame). x8 is reloaded from the slot after each value is materialized so an intervening call
// cannot strand it. Span fields store ptr@offset and the 32-bit count len@offset+8. Mirrors
// macho_emit_sret_field_store.
static bool a64_emit_sret_field_store(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  unsigned slot = a64_sret_slot_offset(fun);
  IrTypeKind vt = instr->value ? instr->value->type : IR_TYPE_I32;
  unsigned fo = instr->field_offset;
  if (vt == IR_TYPE_BYTE_VIEW) {
    if (instr->value && instr->value->kind == IR_VALUE_CALL) {
      // A span-returning call leaves ptr in x0, len in x1; capture both before reloading x8.
      if (!a64_emit_call_to_reg_depth(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
      a64_emit_mov_x(text, 9, 1); // preserve len; x1 is otherwise clobbered by the x8 reload path
      a64_emit_ldr_x_disp(text, 8, 31, slot);
      a64_emit_str_x_disp(text, 0, 8, fo);
      a64_emit_str_w_disp(text, 9, 8, fo + 8);
    } else {
      if (!a64_emit_byte_view_ptr(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
      a64_emit_ldr_x_disp(text, 8, 31, slot);
      a64_emit_str_x_disp(text, 9, 8, fo);
      if (!a64_emit_byte_view_len(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
      a64_emit_ldr_x_disp(text, 8, 31, slot);
      a64_emit_str_w_disp(text, 9, 8, fo + 8);
    }
    return true;
  }
  if (a64_type_is_float(vt)) {
    bool is64 = a64_type_is_f64(vt);
    if (!a64_emit_float_value_to_vreg(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
    a64_emit_ldr_x_disp(text, 8, 31, slot);
    a64_emit_str_v_disp(text, 9, 8, fo, is64);
    return true;
  }
  if (!a64_emit_value_to_reg(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
  a64_emit_ldr_x_disp(text, 8, 31, slot);
  if (vt == IR_TYPE_U8 || vt == IR_TYPE_I8 || vt == IR_TYPE_BOOL) a64_emit_str_b_disp(text, 9, 8, fo);
  else if (a64_type_is_scalar64(vt)) a64_emit_str_x_disp(text, 9, 8, fo);
  else a64_emit_str_w_disp(text, 9, 8, fo);
  return true;
}

// ---------------------------------------------------------------------------------------------
// world.out.write / world.err.write via a raw Linux `svc` write syscall (number 64 in x8). The
// helper takes fd in x0, buffer in x1, length in x2 and returns 0 on success (a partial/short write
// is treated as success here, matching the checked-std.io shim's expectation). On the direct-exe
// path it is emitted once at the end of the text and called with `bl`. A failing `check
// world.out.write(...)` traps with brk #0, mirroring the Mach-O direct-exe path.

static size_t a64_emit_exe_world_write(ZBuf *text) {
  size_t offset = text->len;
  a64_emit_movz_x(text, 8, 64);   // Linux SYS_write
  a64_emit_word(text, 0xd4000001u); // svc #0  -> x0 = bytes written or -errno
  a64_emit_movz_w(text, 0, 0);    // report success to the checked std.io shim
  a64_emit_word(text, 0xd65f03c0u); // ret
  return offset;
}

// fd/buf/len -> x0/x1/x2, then bl the shared svc-write helper; brk on a failed `check`. field_offset
// == 2 selects stderr (fd 2), else stdout (fd 1) — the same convention the other backends use.
static bool a64_emit_world_write(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (!instr || !instr->value) return a64_diag_at(diag, "direct AArch64 ELF world write requires bytes", instr ? instr->line : 1, instr ? instr->column : 1, "missing byte view");
  if (!a64_emit_byte_view_ptr(text, fun, instr->value, 1, frame_size, ctx, diag)) return false;
  if (!a64_emit_byte_view_len(text, fun, instr->value, 2, frame_size, ctx, diag)) return false;
  a64_emit_movz_w(text, 0, instr->field_offset == 2 ? 2u : 1u); // fd
  ctx->world_write_used = true;
  size_t patch = a64_emit_bl_placeholder(text);
  if (!a64_record_world_write_patch(ctx, patch, instr, diag)) return false;
  size_t ok_patch = a64_emit_cbz_w_placeholder(text, 0);
  a64_emit_word(text, 0xd4200000u); // brk #0 on a failed checked write
  a64_patch_cond19(text, ok_patch, text->len);
  return true;
}

// ---------------------------------------------------------------------------------------------
// OS-interface lowerings via raw Linux `svc` syscalls. These mirror emit_elf64.c's syscall sequences
// (open->lseek->mmap->close for a file, MAP_ANON mmap for pageAlloc) and emit_macho64.c's arm64
// register/Maybe shapes — but where macho issues `bl _libc` external calls (obj+link), the AArch64
// Linux backend issues the syscall directly (number in x8, args x0..x5, `svc #0`), so these stay on
// the self-contained direct-exe path. A syscall clobbers x0..x5 + x8, so any value that must outlive
// it is spilled to a frame scratch slot (the same discipline the integer left-operand spill uses).

// Anonymous std.mem.pageAlloc allocation: a fresh kernel-zeroed region via the mmap syscall (the Linux
// analog of macho's _mmap call). The size value is evaluated, spilled to the instruction's integer
// scratch slot so it survives the syscall, the arguments are set up, and `svc mmap` (222) is issued.
// The result populates the Maybe<MutSpan<u8>> dest local: on success has=1@0, ptr@8, len@16; on
// failure (MAP_FAILED, a negative return) the Maybe is cleared. Linux map flags differ from Darwin:
// MAP_ANON=0x20, MAP_PRIVATE=0x2 -> flags=0x22 (Darwin's MAP_ANON is 0x1000); PROT_READ|PROT_WRITE=0x3;
// fd=-1; offset=0. Kernel-zeroed pages give calloc semantics (the token-for-token bar depends on it).
static bool a64_emit_anon_mmap_to_local(ZBuf *text, const IrFunction *fun, const IrValue *size, const IrLocal *local, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (!size) return a64_diag_at(diag, "direct AArch64 ELF page allocation requires a byte length", local ? local->line : 1, local ? local->column : 1, "missing length");
  unsigned slot = a64_int_scratch_slot_offset(fun, 0);
  if (!a64_emit_value_to_reg_depth(text, fun, size, 9, frame_size, 0, ctx, diag)) return false;
  a64_emit_str_x_disp(text, 9, 31, slot); // preserve the length across the syscall
  a64_emit_mov_x(text, 1, 9);             // x1 = len
  a64_emit_movz_x(text, 0, 0);            // x0 = addr (NULL: kernel chooses)
  a64_emit_movz_x(text, 2, 3);            // x2 = prot = PROT_READ|PROT_WRITE
  a64_emit_movz_x(text, 3, 0x22);         // x3 = flags = MAP_ANON|MAP_PRIVATE (Linux)
  a64_emit_movn_x(text, 4, 0);            // x4 = fd = -1
  a64_emit_movz_x(text, 5, 0);            // x5 = offset = 0
  a64_emit_svc(text, 222);                // mmap
  // mmap returns MAP_FAILED (-1) on error; a valid user-space address is never negative.
  a64_emit_cmp_x(text, 0, 31);            // compare result against xzr
  size_t fail = a64_emit_b_cond_placeholder(text, 11); // LT -> failure
  a64_emit_movz_w(text, 9, 1);
  a64_emit_store_local_w(text, fun, 9, local->index, 0, frame_size);   // has = 1
  a64_emit_store_local_x(text, fun, 0, local->index, 8, frame_size);   // ptr
  a64_emit_ldr_x_disp(text, 9, 31, slot);
  a64_emit_store_local_w(text, fun, 9, local->index, 16, frame_size);  // len (element count = bytes)
  size_t end = a64_emit_b_placeholder(text);
  a64_patch_cond19(text, fail, text->len);
  a64_emit_movz_w(text, 9, 0);
  a64_emit_store_local_w(text, fun, 9, local->index, 0, frame_size);   // has = 0
  a64_emit_store_local_x(text, fun, 9, local->index, 8, frame_size);
  a64_emit_store_local_w(text, fun, 9, local->index, 16, frame_size);
  a64_patch_branch26(text, end, text->len);
  return true;
}

// Open + size + read-only mmap a file by path, leaving the mapping address in x0 and its byte length
// in x1 (the AArch64-Linux analog of emit_elf64.c's elf_emit_mmap_file_addr_size, which lands
// addr/size in rax/rdx). On any failure x0 is negative (and never a valid mapping). The syscall
// sequence:
//   fd   = openat(AT_FDCWD=-100, path, O_RDONLY=0)  ; negative fd -> open failure (returned straight)
//   size = lseek(fd, 0, SEEK_END=2)                 ; negative -> seek failure: close fd, return it
//   addr = mmap(NULL, size, PROT_READ=1, MAP_PRIVATE=2, fd, 0)
//   close(fd)                                       ; the fd is no longer needed once the region maps
// The fd is closed on the mmap-success AND mmap-failure (MAP_FAILED) AND lseek-failure paths so no
// descriptor leaks across repeated maps (the mmap-munmap-loop runs 1000x). Locals are sp-relative, so
// values that must outlive a syscall (fd, size, addr) are spilled to integer scratch slots: slot+0
// holds fd, slot+8 holds size, a second slot (slot2+0) holds addr across the close. The size is a
// 64-bit byte count (files may exceed 4 GiB), stored/reloaded with the full-width x form. The caller
// reserves two scratch levels (FS_MMAP contributes depth 2 in a64_value_int_depth).
static bool a64_emit_mmap_file_addr_size(ZBuf *text, const IrFunction *fun, const IrValue *path, unsigned depth, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  unsigned slot = a64_int_scratch_slot_offset(fun, depth);
  unsigned slot2 = a64_int_scratch_slot_offset(fun, depth + 1);
  // openat(AT_FDCWD, path, O_RDONLY): dirfd in x0, path in x1, flags in x2 (mode x3 unused for RDONLY).
  // Materialize the path pointer first (it may use scratch registers), then set the constant args, so
  // none of x0/x2/x3 are clobbered by the path evaluation.
  if (!a64_emit_byte_view_ptr_depth(text, fun, path, 1, frame_size, depth + 2, ctx, diag)) return false; // x1 = path
  a64_emit_movn_x(text, 0, 99);  // x0 = AT_FDCWD = -100 (~99)
  a64_emit_movz_x(text, 2, 0);   // x2 = O_RDONLY = 0
  a64_emit_movz_x(text, 3, 0);   // x3 = mode = 0
  a64_emit_svc(text, 56);        // openat
  a64_emit_cmp_x(text, 0, 31);
  size_t open_fail = a64_emit_b_cond_placeholder(text, 11); // LT: negative fd -> open failure
  a64_emit_str_x_disp(text, 0, 31, slot); // save fd
  // lseek(fd, 0, SEEK_END) -> file size
  a64_emit_movz_x(text, 1, 0);   // offset = 0
  a64_emit_movz_x(text, 2, 2);   // SEEK_END = 2
  a64_emit_svc(text, 62);        // lseek
  a64_emit_cmp_x(text, 0, 31);
  size_t seek_fail = a64_emit_b_cond_placeholder(text, 11); // LT: lseek error
  a64_emit_str_x_disp(text, 0, 31, slot + 8); // save size (64-bit byte count)
  // mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)
  a64_emit_movz_x(text, 0, 0);             // addr = NULL (kernel chooses)
  a64_emit_ldr_x_disp(text, 1, 31, slot + 8); // len = size
  a64_emit_movz_x(text, 2, 1);             // prot = PROT_READ
  a64_emit_movz_x(text, 3, 2);             // flags = MAP_PRIVATE
  a64_emit_ldr_x_disp(text, 4, 31, slot);  // fd
  a64_emit_movz_x(text, 5, 0);             // offset = 0
  a64_emit_svc(text, 222);                 // mmap
  // Close the fd (no longer needed) on both the success and MAP_FAILED paths; preserve addr across the
  // close, then hand addr back in x0 and size in x1. A MAP_FAILED (negative) addr flows through.
  a64_emit_str_x_disp(text, 0, 31, slot2); // save addr
  a64_emit_ldr_x_disp(text, 0, 31, slot);  // x0 = fd
  a64_emit_svc(text, 57);                  // close
  a64_emit_ldr_x_disp(text, 0, 31, slot2); // x0 = addr (result)
  a64_emit_ldr_x_disp(text, 1, 31, slot + 8); // x1 = size (result)
  size_t done = a64_emit_b_placeholder(text);
  // Seek failure: close the fd and return the negative lseek result in x0.
  a64_patch_cond19(text, seek_fail, text->len);
  a64_emit_str_x_disp(text, 0, 31, slot2); // preserve the negative result across the close
  a64_emit_ldr_x_disp(text, 0, 31, slot);  // x0 = fd
  a64_emit_svc(text, 57);                  // close
  a64_emit_ldr_x_disp(text, 0, 31, slot2); // x0 = negative result
  // open_fail lands here with x0 already holding the negative open result.
  a64_patch_cond19(text, open_fail, text->len);
  a64_patch_branch26(text, done, text->len);
  return true;
}

// std.args.get(i): populate the Maybe<Span<u8>> dest local from the seeded argv. argc lives in x20 and
// argv (a NULL-terminated char** ) in x21 (seeded by the prologue from the kernel-provided argc/argv).
// An out-of-range index clears the Maybe; otherwise the i-th argv pointer is taken and its NUL-
// terminated length measured into a 32-bit count. Ported byte-for-byte from emit_macho64.c's
// macho_emit_args_get_to_local (arm64 is arm64). has@0, ptr@8, len@16.
static bool a64_emit_args_get_to_local(ZBuf *text, const IrFunction *fun, const IrValue *value, const IrLocal *local, unsigned frame_size, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (!value || !value->left) return a64_diag_at(diag, "direct AArch64 ELF std.args.get requires an index", value ? value->line : 1, value ? value->column : 1, "missing index");
  if (!a64_emit_value_to_reg_depth(text, fun, value->left, 10, frame_size, 0, ctx, diag)) return false;
  a64_emit_cmp_w(text, 10, 20);
  size_t in_range = a64_emit_b_cond_placeholder(text, 3); // unsigned lower: idx < argc
  a64_emit_movz_w(text, 8, 0);
  a64_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);  // has = 0
  a64_emit_store_local_x(text, fun, 8, local->index, 8, frame_size);
  a64_emit_store_local_w(text, fun, 8, local->index, 16, frame_size);
  size_t end_patch = a64_emit_b_placeholder(text);
  a64_patch_cond19(text, in_range, text->len);

  a64_emit_add_x_reg_lsl(text, 12, 21, 10, 3); // &argv[idx] = argv + idx*8
  a64_emit_ldr_x_disp(text, 12, 12, 0);        // x12 = argv[idx]
  a64_emit_movz_w(text, 10, 0);                // i = 0
  size_t loop_start = text->len;
  a64_emit_add_x_reg(text, 13, 12, 10);        // &arg[i]
  a64_emit_ldrb_w(text, 14, 13);               // arg[i]
  size_t done_patch = a64_emit_cbz_w_placeholder(text, 14); // NUL -> end of string
  a64_emit_add_w_imm(text, 10, 10, 1);         // i++
  size_t loop_patch = a64_emit_b_placeholder(text);
  a64_patch_branch26(text, loop_patch, loop_start);
  a64_patch_cond19(text, done_patch, text->len);

  a64_emit_movz_w(text, 8, 1);
  a64_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);  // has = 1
  a64_emit_store_local_x(text, fun, 12, local->index, 8, frame_size); // ptr
  a64_emit_store_local_w(text, fun, 10, local->index, 16, frame_size); // len (32-bit count)
  a64_patch_branch26(text, end_patch, text->len);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Instruction codegen.

static bool a64_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, unsigned frame_size, bool restore_process_args, Aarch64EmitContext *ctx, ZDiag *diag);

static void a64_emit_epilogue(ZBuf *text, unsigned frame_size, bool restore_process_args) {
  if (frame_size > 0) a64_emit_add_sp_imm(text, 0x910003ffu, frame_size); // add sp, sp, #frame_size
  a64_emit_word(text, 0xa8c17bfdu); // ldp x29, x30, [sp], #16
  if (restore_process_args) a64_emit_word(text, 0xa8c157f4u); // ldp x20, x21, [sp], #16
  a64_emit_word(text, 0xd65f03c0u); // ret
}

static bool a64_emit_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, bool restore_process_args, Aarch64EmitContext *ctx, ZDiag *diag) {
  if (instr->kind == IR_INSTR_WORLD_WRITE) {
    return a64_emit_world_write(text, fun, instr, frame_size, ctx, diag);
  }
  if (instr->kind == IR_INSTR_LOCAL_SET) {
    if (instr->local_index >= fun->local_len) return a64_diag_at(diag, "direct AArch64 ELF local store is out of range", instr->line, instr->column, "invalid local");
    const IrLocal *local = &fun->locals[instr->local_index];
    if (local->is_record) {
      // `let q = f()` — bind a record-returning call straight into q's slot via sret (no copy). The
      // callee receives q's address in x8 and writes its result through it.
      if (instr->value && instr->value->kind == IR_VALUE_CALL) {
        return a64_emit_record_call_with_dest(text, fun, (int)local->index, instr->value, frame_size, ctx, diag);
      }
      // `let q = p` / `q = p` — record-to-record value copy (span fields ride along as raw bytes).
      if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
        a64_emit_record_copy_to(text, fun, local->index, instr->value->local_index, frame_size);
        return true;
      }
      return a64_diag_at(diag, "direct AArch64 ELF record local assignment requires a record value", instr->line, instr->column, "unsupported record set");
    }
    if (local->type == IR_TYPE_BYTE_VIEW) {
      // A span local (`let s: Span<T> = arr` / a reinterpret / a slice / a span-returning call): store
      // the pointer at slot+0 and the 32-bit element count at slot+8 (the P4 convention). A Mapping
      // local keeps a 64-bit length (is_mapping).
      // `let m: owned<Mapping> = check std.fs.mmapOrRaise(fs, path)` -> unwrap into a Mapping local
      // (ptr@0, len@8). The helper lands addr in x0 and the 64-bit byte length in x1; a negative addr is
      // the failure case. Mirroring the `check world.out.write` discipline (which traps rather than
      // threading an error tag — that machinery does not exist on this backend), a failed required
      // mapping traps with brk #0. The length is stored full-width (files may exceed 4 GiB).
      if (instr->value && instr->value->kind == IR_VALUE_CHECK && instr->value->left && instr->value->left->kind == IR_VALUE_FS_MMAP) {
        if (!a64_emit_mmap_file_addr_size(text, fun, instr->value->left->left, 0, frame_size, ctx, diag)) return false;
        a64_emit_cmp_x(text, 0, 31);
        size_t ok = a64_emit_b_cond_placeholder(text, 10); // GE: addr >= 0 -> mapped
        a64_emit_word(text, 0xd4200000u); // brk #0 on a failed required mapping
        a64_patch_cond19(text, ok, text->len);
        a64_emit_store_local_x(text, fun, 0, instr->local_index, 0, frame_size); // ptr
        a64_emit_store_local_x(text, fun, 1, instr->local_index, 8, frame_size); // len (64-bit byte count)
        return true;
      }
      if (instr->value && instr->value->kind == IR_VALUE_CALL) {
        // A span-returning call leaves ptr in x0 and len in x1; store both straight into the slot.
        if (!a64_emit_call_to_reg_depth(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
        a64_emit_store_local_x(text, fun, 0, instr->local_index, 0, frame_size);
        a64_emit_store_local_w(text, fun, 1, instr->local_index, 8, frame_size);
        return true;
      }
      if (!a64_emit_byte_view_ptr(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      a64_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
      if (!a64_emit_byte_view_len(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      // A Mapping target keeps its 64-bit byte length; ordinary span locals store a 32-bit count.
      if (local->is_mapping) a64_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
      else a64_emit_store_local_w(text, fun, 8, instr->local_index, 8, frame_size);
      return true;
    }
    if (local->type == IR_TYPE_ALLOC) {
      if (instr->value && instr->value->kind == IR_VALUE_PAGE_ALLOC) {
        // A PageAlloc carries no pre-reserved buffer — each std.mem.allocBytes performs a fresh
        // anonymous mmap (see a64_emit_anon_mmap_to_local). Zero the allocator slot so it holds no stale
        // pointer/length; mirrors the ELF64 page-alloc init.
        a64_emit_movz_x(text, 8, 0);
        a64_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
        a64_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
        return true;
      }
      return a64_diag_at(diag, "direct AArch64 ELF backend supports only std.mem.pageAlloc allocators", instr->line, instr->column, "unsupported allocator initializer");
    }
    if (local->type == IR_TYPE_MAYBE_BYTE_VIEW) {
      if (instr->value && instr->value->kind == IR_VALUE_ARGS_GET) {
        return a64_emit_args_get_to_local(text, fun, instr->value, local, frame_size, ctx, diag);
      }
      if (instr->value && instr->value->kind == IR_VALUE_FS_MMAP) {
        // `let m = std.fs.mmap(fs, path)` -> Maybe<owned<Mapping>> (has@0, ptr@8, len@16). The helper
        // lands addr in x0 and the 64-bit byte length in x1; a negative addr is the not-found/failure
        // path, which clears the Maybe. The length is stored full-width (files may exceed 4 GiB).
        if (!a64_emit_mmap_file_addr_size(text, fun, instr->value->left, 0, frame_size, ctx, diag)) return false;
        a64_emit_cmp_x(text, 0, 31);
        size_t fail = a64_emit_b_cond_placeholder(text, 11); // LT: MAP_FAILED / open failure
        a64_emit_mov_x(text, 9, 0); // preserve addr across the has store
        a64_emit_movz_w(text, 8, 1);
        a64_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);  // has = 1
        a64_emit_store_local_x(text, fun, 9, local->index, 8, frame_size);  // ptr
        a64_emit_store_local_x(text, fun, 1, local->index, 16, frame_size); // len (64-bit byte count)
        size_t end = a64_emit_b_placeholder(text);
        a64_patch_cond19(text, fail, text->len);
        a64_emit_movz_w(text, 8, 0);
        a64_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);  // has = 0
        a64_emit_store_local_x(text, fun, 8, local->index, 8, frame_size);
        a64_emit_store_local_x(text, fun, 8, local->index, 16, frame_size);
        a64_patch_branch26(text, end, text->len);
        return true;
      }
      // `let m = std.mem.allocBytes(alloc, n)` over a PageAlloc -> Maybe<MutSpan<u8>> via a fresh
      // kernel-zeroed mmap region (calloc semantics).
      if (!instr->value || instr->value->kind != IR_VALUE_ALLOC_BYTES || instr->value->local_index >= fun->local_len || fun->locals[instr->value->local_index].type != IR_TYPE_ALLOC) {
        return a64_diag_at(diag, "direct AArch64 ELF allocation source is invalid", instr->line, instr->column, "invalid allocation");
      }
      if (!fun->locals[instr->value->local_index].is_page_alloc) {
        return a64_diag_at(diag, "direct AArch64 ELF backend supports allocBytes only over a std.mem.pageAlloc allocator", instr->line, instr->column, "unsupported allocator");
      }
      return a64_emit_anon_mmap_to_local(text, fun, instr->value->left, local, frame_size, ctx, diag);
    }
    if (local->type == IR_TYPE_MAYBE_SCALAR) {
      if (!instr->value) return a64_diag_at(diag, "direct AArch64 ELF Maybe scalar initializer is missing", instr->line, instr->column, "missing maybe value");
      if (instr->value->kind == IR_VALUE_MAYBE_SCALAR_LITERAL) {
        a64_emit_movz_w(text, 8, instr->value->data_len ? 1u : 0u);
        a64_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size); // has
        // The payload is stored in the full 8-byte slot, so use the 64-bit immediate sequence to avoid
        // truncating a Maybe<u64>/Maybe<i64> value that exceeds 32 bits.
        a64_emit_mov_imm64(text, 8, (uint64_t)instr->value->int_value);
        a64_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size); // value
        return true;
      }
      return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support this Maybe scalar source", instr->line, instr->column, "unsupported maybe scalar set");
    }
    if (local->is_array || local->type == IR_TYPE_VEC) {
      return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support assigning this local kind (Vec lands in a later phase)", instr->line, instr->column, "unsupported local set");
    }
    // Float local: materialize the value into v8 and store by width. Integer locals go via x8.
    if (a64_type_is_float(local->type)) {
      bool is64 = a64_type_is_f64(local->type);
      if (!a64_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      a64_emit_str_v_local(text, fun, 8, instr->local_index, 0, frame_size, is64);
      return true;
    }
    if (!a64_emit_value_to_reg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
    if (a64_type_is_scalar64(local->type)) a64_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
    else a64_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
    return true;
  }
  if (instr->kind == IR_INSTR_FIELD_STORE) {
    // local_index == UINT_MAX targets the record being returned, written through the saved sret
    // pointer (`return <shape literal>` stores its fields this way); otherwise it writes a field of an
    // addressable record local (e.g. a record argument materialized into a temp).
    if (instr->local_index == UINT_MAX) {
      return a64_emit_sret_field_store(text, fun, instr, frame_size, ctx, diag);
    }
    if (instr->local_index >= fun->local_len) return a64_diag_at(diag, "direct AArch64 ELF field store record is out of range", instr->line, instr->column, "invalid record local");
    if (!fun->locals[instr->local_index].is_record) return a64_diag_at(diag, "direct AArch64 ELF field store requires a record local", instr->line, instr->column, "non-record local");
    if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
      // Span field: store ptr at the field offset and the 32-bit element-count len 8 bytes higher. A
      // span-returning call leaves ptr in x0 and len in x1; any other byte view is materialized
      // ptr-then-len.
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!a64_emit_call_to_reg_depth(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
        a64_emit_store_local_x(text, fun, 0, instr->local_index, instr->field_offset, frame_size);
        a64_emit_store_local_w(text, fun, 1, instr->local_index, instr->field_offset + 8, frame_size);
        return true;
      }
      if (!a64_emit_byte_view_ptr(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      a64_emit_store_local_x(text, fun, 8, instr->local_index, instr->field_offset, frame_size);
      if (!a64_emit_byte_view_len(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      a64_emit_store_local_w(text, fun, 8, instr->local_index, instr->field_offset + 8, frame_size);
      return true;
    }
    if (instr->value && a64_type_is_float(instr->value->type)) {
      bool is64 = a64_type_is_f64(instr->value->type);
      if (!a64_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      a64_emit_str_v_field(text, fun, 8, instr->local_index, instr->field_offset, frame_size, is64);
      return true;
    }
    if (!a64_emit_value_to_reg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
    a64_emit_store_field(text, fun, 8, instr->local_index, instr->field_offset, instr->value ? instr->value->type : IR_TYPE_I32, frame_size);
    return true;
  }
  if (instr->kind == IR_INSTR_INDEX_STORE) {
    if (instr->array_index >= fun->local_len) return a64_diag_at(diag, "direct AArch64 ELF indexed store array is out of range", instr->line, instr->column, "invalid array local");
    const IrLocal *local = &fun->locals[instr->array_index];
    unsigned const_index = 0;
    if (local->type == IR_TYPE_BYTE_VIEW) {
      // Typed-span element write: bounds-check at runtime, then store by element width. The value is
      // materialized BEFORE the address so the address scratch (x8/x9) survives — u8/i8 store the low
      // byte (the in-register value is sign/zero-extended), wider ints via w/x, floats via the FP file.
      if (a64_type_is_float(local->element_type)) {
        bool is64 = a64_type_is_f64(local->element_type);
        if (!a64_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
        if (!a64_emit_span_index_addr(text, fun, instr->array_index, instr->index, local->element_type, frame_size, ctx, diag)) return false;
        a64_emit_str_v_base(text, 8, 9, is64);
        return true;
      }
      if (!a64_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
      if (!a64_emit_span_index_addr(text, fun, instr->array_index, instr->index, local->element_type, frame_size, ctx, diag)) return false;
      if (local->element_type == IR_TYPE_U8 || local->element_type == IR_TYPE_I8) a64_emit_strb_w(text, 10, 9);
      else if (a64_elem_byte_size(local->element_type) == 8) a64_emit_str_x(text, 10, 9);
      else a64_emit_str_w(text, 10, 9);
      return true;
    }
    if (local->is_array && a64_type_is_float(local->element_type)) {
      if (!a64_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      return a64_emit_float_index_store(text, fun, instr, local, 8, frame_size, ctx, diag);
    }
    if (local->is_array && local->element_type != IR_TYPE_U8 && a64_const_u32_value(instr->index, &const_index) && const_index < local->array_len) {
      if (!a64_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
      a64_emit_store_local_w(text, fun, 10, instr->array_index, const_index * 4u, frame_size);
      return true;
    }
    if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
      if (!a64_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
      if (!instr->index || !a64_emit_value_to_reg(text, fun, instr->index, 8, frame_size, ctx, diag)) return false;
      a64_emit_movz_w(text, 9, local->array_len);
      a64_emit_cmp_w(text, 8, 9);
      size_t ok_patch = a64_emit_b_cond_placeholder(text, 3); // unsigned lower
      a64_emit_word(text, 0xd4200000u); // brk #0
      a64_patch_cond19(text, ok_patch, text->len);
      a64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, instr->array_index, 0, frame_size));
      a64_emit_add_x_reg_lsl(text, 9, 9, 8, 2);
      a64_emit_str_w(text, 10, 9);
      return true;
    }
    if (!local->is_array || local->element_type != IR_TYPE_U8) return a64_diag_at(diag, "direct AArch64 ELF indexed store requires [N]u8 or integer arrays", instr->line, instr->column, "unsupported array local");
    if (!a64_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
    if (!instr->index || !a64_emit_value_to_reg(text, fun, instr->index, 8, frame_size, ctx, diag)) return false;
    a64_emit_movz_w(text, 9, local->array_len);
    a64_emit_cmp_w(text, 8, 9);
    size_t ok_patch = a64_emit_b_cond_placeholder(text, 3); // unsigned lower
    a64_emit_word(text, 0xd4200000u); // brk #0
    a64_patch_cond19(text, ok_patch, text->len);
    a64_emit_add_x_sp_imm(text, 9, a64_local_slot_offset(fun, instr->array_index, 0, frame_size));
    a64_emit_add_x_reg(text, 9, 9, 8);
    a64_emit_strb_w(text, 10, 9);
    return true;
  }
  if (instr->kind == IR_INSTR_EXPR) {
    if (!instr->value) return true;
    if (a64_type_is_float(instr->value->type)) return a64_emit_float_value_to_vreg(text, fun, instr->value, 0, frame_size, ctx, diag);
    return a64_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag);
  }
  if (instr->kind == IR_INSTR_RETURN) {
    if (fun->return_type == IR_TYPE_RECORD) {
      // `return f()` — the callee writes straight through our sret pointer (x8) and hands it back in
      // x0; nothing more to do. `return p` — copy a record local/param through the sret pointer. For a
      // `return <shape literal>` the fields were already stored through sret. Either way, leave the
      // sret pointer in x0 as the result (the AAPCS indirect-result convention).
      if (instr->value && instr->value->kind == IR_VALUE_CALL) {
        if (!a64_emit_record_call_with_dest(text, fun, -1, instr->value, frame_size, ctx, diag)) return false;
        a64_emit_epilogue(text, frame_size, restore_process_args);
        return true;
      }
      if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
        a64_emit_record_copy_to(text, fun, UINT_MAX, instr->value->local_index, frame_size);
      }
      a64_emit_ldr_x_disp(text, 0, 31, a64_sret_slot_offset(fun));
      a64_emit_epilogue(text, frame_size, restore_process_args);
      return true;
    }
    if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
      // Span return: ptr in x0, len in x1. A span-returning call already lands both there; any other
      // byte view is materialized ptr-then-len. The length stays a 32-bit element count.
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!a64_emit_call_to_reg_depth(text, fun, instr->value, 0, frame_size, 0, ctx, diag)) return false;
      } else {
        if (!a64_emit_byte_view_ptr(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
        if (!a64_emit_byte_view_len(text, fun, instr->value, 1, frame_size, ctx, diag)) return false;
      }
      a64_emit_epilogue(text, frame_size, restore_process_args);
      return true;
    }
    if (instr->value) {
      // FP results return in v0 (s0/d0); integer results in x0/w0.
      if (a64_type_is_float(instr->value->type)) {
        if (!a64_emit_float_value_to_vreg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
      } else if (!a64_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) {
        return false;
      }
    }
    // A raising function reports "no error" with x1 = 0 alongside its value (the AAPCS analog of the
    // ELF rdx tag); the caller's `check` tests x1. A float result rides in v0 with the tag still in
    // x1, so the tag is set for float returns too. Skip for span returns (handled above — x1 carries
    // the length there; raising span returns are not part of the ABI).
    if (fun->raises) {
      a64_emit_movz_x(text, 1, 0);
    }
    a64_emit_epilogue(text, frame_size, restore_process_args);
    return true;
  }
  if (instr->kind == IR_INSTR_IF) {
    if (!a64_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
    size_t false_patch = a64_emit_cbz_w_placeholder(text, 0);
    if (!a64_emit_instrs(text, fun, instr->then_instrs, instr->then_len, frame_size, restore_process_args, ctx, diag)) return false;
    if (instr->else_len > 0) {
      size_t end_patch = a64_emit_b_placeholder(text);
      a64_patch_cond19(text, false_patch, text->len);
      if (!a64_emit_instrs(text, fun, instr->else_instrs, instr->else_len, frame_size, restore_process_args, ctx, diag)) return false;
      a64_patch_branch26(text, end_patch, text->len);
    } else {
      a64_patch_cond19(text, false_patch, text->len);
    }
    return true;
  }
  if (instr->kind == IR_INSTR_WHILE) {
    size_t loop_start = text->len;
    if (!a64_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
    size_t false_patch = a64_emit_cbz_w_placeholder(text, 0);
    if (!a64_emit_instrs(text, fun, instr->then_instrs, instr->then_len, frame_size, restore_process_args, ctx, diag)) return false;
    size_t loop_patch = a64_emit_b_placeholder(text);
    a64_patch_branch26(text, loop_patch, loop_start);
    a64_patch_cond19(text, false_patch, text->len);
    return true;
  }
  char actual[64];
  snprintf(actual, sizeof(actual), "unsupported instruction kind %d", (int)instr->kind);
  return a64_diag_at(diag, "direct AArch64 ELF backend does not yet support this instruction kind", instr->line, instr->column, actual);
}

static bool a64_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, unsigned frame_size, bool restore_process_args, Aarch64EmitContext *ctx, ZDiag *diag) {
  for (size_t i = 0; i < len; i++) {
    if (!a64_emit_instr(text, fun, &instrs[i], frame_size, restore_process_args, ctx, diag)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Function shape validation + prologue.

static unsigned a64_frame_size(const IrFunction *fun) {
  return (unsigned)a64_align(fun ? (fun->frame_bytes ? fun->frame_bytes : fun->local_len * 8) : 0, 16);
}

static bool a64_is_literal_return_function(const IrFunction *fun, uint32_t *out) {
  if (!fun || fun->local_len != 0 || fun->instr_len != 1 || fun->param_count != 0) return false;
  if (fun->return_type != IR_TYPE_U8 && fun->return_type != IR_TYPE_I32 && fun->return_type != IR_TYPE_U32 && fun->return_type != IR_TYPE_USIZE) return false;
  const IrInstr *instr = &fun->instrs[0];
  if (instr->kind != IR_INSTR_RETURN || !instr->value || instr->value->kind != IR_VALUE_INT || instr->value->int_value > 65535) return false;
  if (out) *out = (uint32_t)instr->value->int_value;
  return true;
}

// `movz w0, #literal; ret` — the canonical small-integer-return leaf the conformance assertion
// hardcodes (the `movz w0,#42; ret` byte sequence).
static void a64_emit_literal_return(ZBuf *text, uint32_t literal) {
  a64_emit_word(text, 0x52800000u | ((literal & 0xffffu) << 5)); // movz w0, #literal
  a64_emit_word(text, 0xd65f03c0u); // ret
}

static bool a64_validate_function(const IrFunction *fun, ZDiag *diag) {
  uint32_t ignored = 0;
  if (a64_is_literal_return_function(fun, &ignored)) return true;
  if (fun->param_count > 8) return a64_diag_at(diag, "direct AArch64 ELF backend supports at most eight parameters", fun->line, fun->column, fun->name);
  // Returns: Void, primitive integer, float, a record (via the x8 sret pointer), or a span (ptr in x0,
  // len in x1). A raising function cannot return a record — the error tag would collide with the
  // sret/len register convention — but no consumer needs that, so it is rejected like ELF/macho.
  if (fun->return_type == IR_TYPE_RECORD) {
    if (fun->raises) return a64_diag_at(diag, "direct AArch64 ELF backend cannot return a record from a raising function", fun->line, fun->column, fun->name);
  } else if (fun->return_type != IR_TYPE_VOID && fun->return_type != IR_TYPE_BYTE_VIEW && !a64_type_is_scalar(fun->return_type) && !a64_type_is_float(fun->return_type)) {
    return a64_diag_at(diag, "direct AArch64 ELF backend currently supports only Void, primitive integer, float, record, and span returns", fun->line, fun->column, fun->name);
  }
  // Parameters may be primitive scalars, spans (`(ptr,len)` in two int regs), or records (a pointer in
  // one int reg, copied into the slot for value semantics). Locals may additionally be fixed primitive
  // arrays, byte-view (span) locals, records, page allocators (IR_TYPE_ALLOC), and Maybe locals
  // (Maybe<MutSpan<u8>> from allocBytes/std.args.get, Maybe<owned<Mapping>> from std.fs.mmap, and
  // Maybe<scalar>). Vec locals still bail.
  for (size_t i = 0; i < fun->local_len; i++) {
    const IrLocal *local = &fun->locals[i];
    if (local->is_param && local->type != IR_TYPE_BYTE_VIEW && !local->is_record &&
        !a64_type_is_scalar(local->type) && !a64_type_is_float(local->type)) {
      return a64_diag_at(diag, "direct AArch64 ELF backend currently supports only primitive scalar, span, and record parameters", local->line, local->column, local->name);
    }
    if (local->type == IR_TYPE_BYTE_VIEW) {
      continue; // a span local or span param (the param's prologue spills (ptr,len) from two int regs)
    }
    if (local->is_record) {
      continue; // a record local/param (sret-bound, copied, or copied-in by pointer)
    }
    if (local->type == IR_TYPE_ALLOC || local->type == IR_TYPE_MAYBE_BYTE_VIEW || local->type == IR_TYPE_MAYBE_SCALAR) {
      continue; // a page allocator or a Maybe (allocBytes / std.args.get / std.fs.mmap / Maybe<scalar>)
    }
    if (local->is_array && (local->element_type == IR_TYPE_U8 || local->element_type == IR_TYPE_I8 ||
        local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_I64 ||
        local->element_type == IR_TYPE_U64 || local->element_type == IR_TYPE_USIZE || a64_type_is_float(local->element_type))) {
      continue;
    }
    if (local->is_array || local->type == IR_TYPE_VEC ||
        (!a64_type_is_scalar(local->type) && !a64_type_is_float(local->type))) {
      return a64_diag_at(diag, "direct AArch64 ELF backend currently supports only primitive scalar, fixed primitive array, span, record, allocator, and Maybe locals (Vec lands in a later phase)", local->line, local->column, local->name);
    }
  }
  return true;
}

static bool a64_emit_function_text(ZBuf *text, const IrFunction *fun, bool seed_main_process_args, Aarch64EmitContext *ctx, ZDiag *diag) {
  uint32_t literal = 0;
  if (a64_is_literal_return_function(fun, &literal)) {
    a64_emit_literal_return(text, literal);
    return true;
  }

  // The FP scratch region sits at the bottom of the frame ([0, fp_scratch_bytes)), the integer
  // binary/compare scratch just above it, then a 16-byte sret slot (for a record-returning function),
  // then the locals at the top. The single frame_size keeps local addressing, both scratch regions,
  // and the sret slot consistent for the body and the matching epilogue.
  unsigned frame_size = a64_frame_size(fun) + a64_fp_scratch_bytes(fun) + a64_int_scratch_bytes(fun) + a64_sret_reserved_bytes(fun);
  // main reads argc/argv from the callee-saved x20/x21 (the ARGS_LEN/ARGS_GET lowerings), so its
  // prologue seeds x20<-x0, x21<-x1 from the C-ABI argument registers. This works under BOTH entry
  // paths because each delivers argc in x0 and argv in x1 before `bl main`: the direct-exe `_start`
  // stub (a64_emit_exe_start_stub) reads them off the kernel stack into x0/x1, and on the obj+link
  // path the musl/glibc crt0 calls main(argc, argv) per the C ABI (x0=argc, x1=argv). Mirrors
  // emit_elf64.c, whose object path also sets seed_main_process_args so its main seeds rdi/rsi into
  // its callee-saved arg registers regardless of which _start invoked it. x20/x21 are callee-saved,
  // so they are stp-saved here and ldp-restored in the epilogue (the obj+link caller relies on that).
  bool seed_process_args = seed_main_process_args && fun->is_exported && fun->name && strcmp(fun->name, "main") == 0;
  if (seed_process_args) a64_emit_word(text, 0xa9bf57f4u); // stp x20, x21, [sp, #-16]!
  a64_emit_word(text, 0xa9bf7bfdu); // stp x29, x30, [sp, #-16]!
  a64_emit_word(text, 0x910003fdu); // mov x29, sp
  if (frame_size > 0) a64_emit_add_sp_imm(text, 0xd10003ffu, frame_size); // sub sp, sp, #frame_size
  if (seed_process_args) {
    a64_emit_mov_x(text, 20, 0); // x20 = argc
    a64_emit_mov_x(text, 21, 1); // x21 = argv
  }
  // A record-returning function gets the destination address in x8 (the AAPCS indirect-result
  // register, not an argument register). Save it so it survives the body's calls; field stores and the
  // return reload it from this slot.
  if (a64_returns_record(fun)) a64_emit_str_x_disp(text, 8, 31, a64_sret_slot_offset(fun));
  // AAPCS passes integer and FP parameters in separate register banks (x0..x7 vs v0..v7), each
  // consumed by its own counter. A span (byte-view) param arrives as (ptr,len) in two int regs; a
  // record param arrives as a pointer in one int reg and is copied into its inline slot for value
  // semantics. Spill each incoming argument register into its slot.
  unsigned int_arg = 0;
  unsigned fp_arg = 0;
  for (size_t i = 0; i < fun->param_count; i++) {
    IrTypeKind ptype = fun->locals[i].type;
    if (a64_type_is_float(ptype)) {
      a64_emit_str_v_local(text, fun, fp_arg, (unsigned)i, 0, frame_size, a64_type_is_f64(ptype));
      fp_arg++;
    } else if (ptype == IR_TYPE_BYTE_VIEW) {
      a64_emit_store_local_x(text, fun, int_arg, (unsigned)i, 0, frame_size); // ptr @ slot+0
      int_arg++;
      a64_emit_store_local_w(text, fun, int_arg, (unsigned)i, 8, frame_size); // len @ slot+8 (element count)
      int_arg++;
    } else if (fun->locals[i].is_record) {
      a64_emit_copy_record_param(text, fun, (unsigned)i, int_arg, frame_size);
      int_arg++;
    } else if (a64_type_is_scalar64(ptype)) {
      a64_emit_store_local_x(text, fun, int_arg, (unsigned)i, 0, frame_size);
      int_arg++;
    } else {
      a64_emit_store_local_w(text, fun, int_arg, (unsigned)i, 0, frame_size);
      int_arg++;
    }
  }
  if (!a64_emit_instrs(text, fun, fun->instrs, fun->instr_len, frame_size, seed_process_args, ctx, diag)) return false;
  if (fun->instr_len == 0 || fun->instrs[fun->instr_len - 1].kind != IR_INSTR_RETURN) a64_emit_epilogue(text, frame_size, seed_process_args);
  return true;
}

// ---------------------------------------------------------------------------------------------
// rodata layout (ported from emit_elf64.c).

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
    a64_pad_to(rodata, segment->offset - base_offset);
    a64_append_bytes(rodata, segment->bytes, segment->len);
  }
}

static void a64_append_symbol(ZBuf *symtab, uint32_t name, unsigned char info, uint16_t shndx, uint64_t value, uint64_t size) {
  a64_append_u32(symtab, name);
  a64_append_u8(symtab, info);
  a64_append_u8(symtab, 0);
  a64_append_u16(symtab, shndx);
  a64_append_u64(symtab, value);
  a64_append_u64(symtab, size);
}

// One Elf64_Rela entry: r_offset (the patched .text byte), r_info (sym index << 32 | type), and the
// addend. AArch64 reloc types (verified against elf.h): R_AARCH64_ADR_PREL_PG_HI21=275 and
// R_AARCH64_ADD_ABS_LO12_NC=277 (the ADRP+ADD rodata pair, addend = the in-section byte offset),
// R_AARCH64_CALL26=283 (the libm `bl`, addend 0). Mirrors emit_elf64.c's elf_append_rela.
static void a64_append_rela(ZBuf *rela, uint64_t offset, uint32_t sym, uint32_t type, int64_t addend) {
  a64_append_u64(rela, offset);
  a64_append_u64(rela, ((uint64_t)sym << 32) | type);
  a64_append_u64(rela, (uint64_t)addend);
}

static void a64_append_section_header(ZBuf *out, uint32_t name, uint32_t type, uint64_t flags, uint64_t offset, uint64_t size, uint32_t link, uint32_t info, uint64_t align, uint64_t entsize) {
  a64_append_u32(out, name);
  a64_append_u32(out, type);
  a64_append_u64(out, flags);
  a64_append_u64(out, 0);
  a64_append_u64(out, offset);
  a64_append_u64(out, size);
  a64_append_u32(out, link);
  a64_append_u32(out, info);
  a64_append_u64(out, align);
  a64_append_u64(out, entsize);
}

static const IrFunction *a64_find_main(const IrProgram *ir, unsigned *out_index, ZDiag *diag) {
  const IrFunction *fun = NULL;
  unsigned index = 0;
  for (size_t i = 0; ir && i < ir->function_len; i++) {
    if (ir->functions[i].is_exported && strcmp(ir->functions[i].name, "main") == 0) {
      if (fun) {
        a64_diag_at(diag, "direct AArch64 ELF executable backend requires exactly one exported main function", ir->functions[i].line, ir->functions[i].column, ir->functions[i].name);
        return NULL;
      }
      fun = &ir->functions[i];
      index = (unsigned)i;
    }
  }
  if (!fun) {
    a64_diag_at(diag, "direct AArch64 ELF executable backend requires an exported main function", 1, 1, "missing main");
    return NULL;
  }
  if (out_index) *out_index = index;
  return fun;
}

static void a64_free_ctx(Aarch64EmitContext *ctx) {
  free(ctx->call_patches);
  free(ctx->rodata_patches);
  free(ctx->math_call_patches);
}

// =============================================================================================
// Object path — relocatable ELF (machine 183). It shares the codegen helpers with the direct-exe
// path and emits a real relocatable object: `.text` + (optional) `.rodata` + `.rela.text` +
// `.symtab` + `.strtab` + `.shstrtab`. This is the path libm programs take (forced by the obj+link
// dispatch), linked by `zig cc -target aarch64-linux-musl`. The relocation discipline mirrors
// emit_elf64.c's object path, with AArch64 reloc types:
//   - libm `bl` sites          -> R_AARCH64_CALL26 (283) against an undefined external symbol
//                                 (sqrtf/expf/…); the host linker binds them from libm.
//   - string-literal addresses -> R_AARCH64_ADR_PREL_PG_HI21 (275) + R_AARCH64_ADD_ABS_LO12_NC (277)
//                                 against the .rodata section symbol (the ADRP+ADD pair).
//   - same-file Zero->Zero `bl` and the inline svc-write helper -> resolved IN PLACE (PC-relative
//                                 within .text), exactly like emit_elf64.c's elf_patch_call_patches;
//                                 no relocation is emitted for intra-.text calls.
// world.write is the same inline raw `svc` write helper the direct-exe path uses (a self-contained
// syscall, no libc symbol), appended after the function bodies and called via an in-place `bl`.

static void a64_object_free(Aarch64EmitContext *ctx, size_t *fo, size_t *fs, uint32_t *sn,
                            ZBuf *text, ZBuf *rodata, ZBuf *rela, ZBuf *strtab, ZBuf *symtab, ZBuf *shstrtab) {
  a64_free_ctx(ctx);
  free(fo);
  free(fs);
  free(sn);
  zbuf_free(text);
  zbuf_free(rodata);
  zbuf_free(rela);
  zbuf_free(strtab);
  zbuf_free(symtab);
  zbuf_free(shstrtab);
}

bool z_emit_elf_aarch64_object_from_ir(const IrProgram *ir, ZBuf *out, ZDiag *diag) {
  if (!ir) return a64_diag_at(diag, "direct AArch64 ELF object backend requires MIR", 1, 1, "missing MIR");
  if (!ir->mir_valid) {
    bool ok = a64_diag_at(diag, ir->mir_message[0] ? ir->mir_message : "direct backend lowering failed", ir->mir_line, ir->mir_column, ir->mir_actual);
    z_diag_set_backend_blocker(diag, &ir->backend_blocker);
    return ok;
  }
  bool has_export = false;
  for (size_t i = 0; i < ir->function_len; i++) {
    if (ir->functions[i].is_exported) has_export = true;
    if (!a64_validate_function(&ir->functions[i], diag)) return false;
  }
  if (!has_export) return a64_diag_at(diag, "direct AArch64 ELF object backend requires at least one exported function", 1, 1, "no exported function");

  ZBuf text;
  ZBuf rodata;
  ZBuf rela_text;
  ZBuf strtab;
  ZBuf symtab;
  ZBuf shstrtab;
  zbuf_init(&text);
  zbuf_init(&rodata);
  zbuf_init(&rela_text);
  zbuf_init(&strtab);
  zbuf_init(&symtab);
  zbuf_init(&shstrtab);
  a64_append_u8(&strtab, 0);
  a64_append_zeros(&symtab, 24);
  bool has_rodata = ir->readonly_data_bytes > 0 || ir->data_segment_len > 0;
  unsigned rodata_base_offset = a64_rodata_base_offset(ir);
  if (has_rodata) a64_append_rodata(&rodata, ir, rodata_base_offset);

  size_t *function_offsets = z_checked_calloc(ir->function_len, sizeof(size_t));
  size_t *function_sizes = z_checked_calloc(ir->function_len, sizeof(size_t));
  uint32_t *symbol_names = z_checked_calloc(ir->function_len, sizeof(uint32_t));
  if (!function_offsets || !function_sizes || !symbol_names) {
    free(function_offsets);
    free(function_sizes);
    free(symbol_names);
    zbuf_free(&text);
    zbuf_free(&rodata);
    zbuf_free(&rela_text);
    zbuf_free(&strtab);
    zbuf_free(&symtab);
    zbuf_free(&shstrtab);
    return a64_diag_at(diag, "direct AArch64 ELF object backend ran out of memory", 1, 1, "allocation failed");
  }

  // emit_rodata_relocations: address string literals via ADRP+ADD + R_AARCH64 relocs (the object is
  // relocatable, not fixed-base). Same-file calls + the world-write helper are still resolved in
  // place after the layout is known.
  Aarch64EmitContext ctx = {
    .ir = ir,
    .function_offsets = function_offsets,
    .function_count = ir->function_len,
    .rodata_base_offset = rodata_base_offset,
    .emit_rodata_relocations = true
  };

  for (size_t i = 0; i < ir->function_len; i++) {
    a64_pad_to(&text, a64_align(text.len, 4));
    function_offsets[i] = text.len;
    // seed_main_process_args = true: this object is linked against crt0 (the obj+link path), which
    // calls main(argc, argv) via the C ABI (x0=argc, x1=argv). main's prologue must copy x0/x1 into
    // the callee-saved x20/x21 that std.args.{len,get} read; without this seed x20/x21 hold garbage
    // under crt0 and the first argv access faults. Matches emit_elf64.c's object path (seed = true).
    if (!a64_emit_function_text(&text, &ir->functions[i], true, &ctx, diag)) {
      a64_object_free(&ctx, function_offsets, function_sizes, symbol_names, &text, &rodata, &rela_text, &strtab, &symtab, &shstrtab);
      return false;
    }
    function_sizes[i] = text.len - function_offsets[i];
    symbol_names[i] = (uint32_t)strtab.len;
    zbuf_append(&strtab, ir->functions[i].name ? ir->functions[i].name : "zero_fn");
    a64_append_u8(&strtab, 0);
  }

  // The inline svc-write helper goes after the function bodies; world.write call sites (the sentinel
  // callee_index == function_count) branch to it. Same-file calls and the helper are intra-.text, so
  // they are patched in place — no relocations.
  size_t world_write_offset = 0;
  if (ctx.world_write_used) {
    a64_pad_to(&text, a64_align(text.len, 4));
    world_write_offset = a64_emit_exe_world_write(&text);
  }
  a64_patch_call_patches(&text, &ctx);
  for (size_t i = 0; i < ctx.call_patch_len; i++) {
    if (ctx.call_patches[i].callee_index >= ctx.function_count) {
      a64_patch_branch26(&text, ctx.call_patches[i].patch_offset, world_write_offset);
    }
  }

  // Undefined external libm symbols (bare ELF names) are appended to .strtab after the function
  // names; their .symtab indices follow the function symbols (and the optional .rodata section
  // symbol). Record the strtab name offset for each used math symbol.
  uint32_t math_symbol_names[A64_MATH_SYMBOL_COUNT];
  for (int s = 0; s < A64_MATH_SYMBOL_COUNT; s++) {
    math_symbol_names[s] = 0;
    if (a64_math_symbol_used(&ctx, (Aarch64MathSymbol)s)) {
      math_symbol_names[s] = (uint32_t)strtab.len;
      zbuf_append(&strtab, a64_math_symbol_names[s]);
      a64_append_u8(&strtab, 0);
    }
  }

  // Symbol table layout: index 0 = null (already written); index 1 = the .rodata section symbol when
  // present (the rodata relocs reference it); then the function symbols; then the undefined math
  // externals. Compute each math symbol's final index in this order so the relocs name it correctly.
  const uint32_t function_symbol_base = has_rodata ? 2u : 1u;
  uint32_t next_external_symbol = function_symbol_base + (uint32_t)ir->function_len;
  uint32_t math_symbol_index[A64_MATH_SYMBOL_COUNT];
  for (int s = 0; s < A64_MATH_SYMBOL_COUNT; s++) {
    math_symbol_index[s] = 0;
    if (a64_math_symbol_used(&ctx, (Aarch64MathSymbol)s)) math_symbol_index[s] = next_external_symbol++;
  }

  // .rela.text — string-literal ADRP+ADD pair (against the .rodata section symbol, index 1) followed
  // by libm CALL26 sites (against the external symbols). The ADRP/ADD addend is the byte offset of
  // the literal within .rodata; CALL26 needs no addend.
  for (size_t i = 0; i < ctx.rodata_patch_len; i++) {
    int64_t addend = (int64_t)ctx.rodata_patches[i].data_offset - (int64_t)ctx.rodata_base_offset;
    a64_append_rela(&rela_text, ctx.rodata_patches[i].patch_offset, 1, 275, addend);     // R_AARCH64_ADR_PREL_PG_HI21 (ADRP word)
    a64_append_rela(&rela_text, ctx.rodata_patches[i].patch_offset + 4, 1, 277, addend); // R_AARCH64_ADD_ABS_LO12_NC (ADD word)
  }
  for (size_t i = 0; i < ctx.math_call_patch_len; i++) {
    a64_append_rela(&rela_text, ctx.math_call_patches[i].patch_offset, math_symbol_index[ctx.math_call_patches[i].symbol], 283, 0); // R_AARCH64_CALL26
  }

  // Symbols: optional .rodata section symbol (STT_SECTION, shndx = .rodata index), then every defined
  // function as GLOBAL|FUNC (0x12, shndx = .text index) — keeping non-exported functions global too,
  // because the .symtab sh_info below declares the first global at the function base; a local past
  // sh_info is rejected by the linker (the emit_elf64.c lesson). Finally the undefined math externals
  // (0x12, shndx 0). The .text/.rodata indices depend on whether .rodata exists.
  const uint16_t text_shndx = 1;
  const uint16_t rodata_shndx = 2;
  if (has_rodata) a64_append_symbol(&symtab, 0, 0x03, rodata_shndx, 0, 0); // STT_SECTION for .rodata
  for (size_t i = 0; i < ir->function_len; i++) {
    a64_append_symbol(&symtab, symbol_names[i], 0x12, text_shndx, function_offsets[i], function_sizes[i]);
  }
  for (int s = 0; s < A64_MATH_SYMBOL_COUNT; s++) {
    if (a64_math_symbol_used(&ctx, (Aarch64MathSymbol)s)) a64_append_symbol(&symtab, math_symbol_names[s], 0x12, 0, 0, 0);
  }

  a64_append_u8(&shstrtab, 0);
  uint32_t sh_name_text = (uint32_t)shstrtab.len;
  zbuf_append(&shstrtab, ".text");
  a64_append_u8(&shstrtab, 0);
  uint32_t sh_name_rodata = 0;
  uint32_t sh_name_rela_text = 0;
  if (has_rodata) {
    sh_name_rodata = (uint32_t)shstrtab.len;
    zbuf_append(&shstrtab, ".rodata");
    a64_append_u8(&shstrtab, 0);
  }
  if (rela_text.len > 0) {
    sh_name_rela_text = (uint32_t)shstrtab.len;
    zbuf_append(&shstrtab, ".rela.text");
    a64_append_u8(&shstrtab, 0);
  }
  uint32_t sh_name_symtab = (uint32_t)shstrtab.len;
  zbuf_append(&shstrtab, ".symtab");
  a64_append_u8(&shstrtab, 0);
  uint32_t sh_name_strtab = (uint32_t)shstrtab.len;
  zbuf_append(&shstrtab, ".strtab");
  a64_append_u8(&shstrtab, 0);
  uint32_t sh_name_shstrtab = (uint32_t)shstrtab.len;
  zbuf_append(&shstrtab, ".shstrtab");
  a64_append_u8(&shstrtab, 0);

  const size_t ehdr_size = 64;
  const size_t shnum = 5 + (has_rodata ? 1 : 0) + (rela_text.len > 0 ? 1 : 0);
  const uint16_t symtab_shndx = (uint16_t)(2 + (has_rodata ? 1 : 0) + (rela_text.len > 0 ? 1 : 0));
  const uint16_t strtab_shndx = (uint16_t)(symtab_shndx + 1);
  const uint16_t shstrtab_shndx = (uint16_t)(strtab_shndx + 1);
  size_t text_offset = a64_align(ehdr_size, 4);
  size_t rodata_offset = has_rodata ? a64_align(text_offset + text.len, 8) : 0;
  size_t rela_text_offset = rela_text.len > 0 ? a64_align((has_rodata ? rodata_offset + rodata.len : text_offset + text.len), 8) : 0;
  size_t symtab_offset = a64_align((rela_text.len > 0 ? rela_text_offset + rela_text.len : (has_rodata ? rodata_offset + rodata.len : text_offset + text.len)), 8);
  size_t strtab_offset = symtab_offset + symtab.len;
  size_t shstrtab_offset = strtab_offset + strtab.len;
  size_t shoff = a64_align(shstrtab_offset + shstrtab.len, 8);

  zbuf_init(out);
  const unsigned char ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  a64_append_bytes(out, ident, sizeof(ident));
  a64_append_u16(out, 1);   // ET_REL
  a64_append_u16(out, 183); // EM_AARCH64
  a64_append_u32(out, 1);
  a64_append_u64(out, 0);
  a64_append_u64(out, 0);
  a64_append_u64(out, shoff);
  a64_append_u32(out, 0);
  a64_append_u16(out, 64);
  a64_append_u16(out, 0);
  a64_append_u16(out, 0);
  a64_append_u16(out, 64);
  a64_append_u16(out, (uint16_t)shnum);
  a64_append_u16(out, shstrtab_shndx);

  a64_pad_to(out, text_offset);
  a64_append_bytes(out, (const unsigned char *)text.data, text.len);
  if (has_rodata) {
    a64_pad_to(out, rodata_offset);
    a64_append_bytes(out, (const unsigned char *)rodata.data, rodata.len);
  }
  if (rela_text.len > 0) {
    a64_pad_to(out, rela_text_offset);
    a64_append_bytes(out, (const unsigned char *)rela_text.data, rela_text.len);
  }
  a64_pad_to(out, symtab_offset);
  a64_append_bytes(out, (const unsigned char *)symtab.data, symtab.len);
  a64_pad_to(out, strtab_offset);
  a64_append_bytes(out, (const unsigned char *)strtab.data, strtab.len);
  a64_pad_to(out, shstrtab_offset);
  a64_append_bytes(out, (const unsigned char *)shstrtab.data, shstrtab.len);
  a64_pad_to(out, shoff);

  // sh_info of .symtab is the index of the first global symbol = the function-symbol base (the linker
  // requires all locals to precede this index). sh_info of .rela.text is the section it relocates
  // (.text = index 1).
  a64_append_section_header(out, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  a64_append_section_header(out, sh_name_text, 1, 0x6, text_offset, text.len, 0, 0, 4, 0);
  if (has_rodata) a64_append_section_header(out, sh_name_rodata, 1, 0x2, rodata_offset, rodata.len, 0, 0, 8, 0);
  if (rela_text.len > 0) a64_append_section_header(out, sh_name_rela_text, 4, 0, rela_text_offset, rela_text.len, symtab_shndx, text_shndx, 8, 24);
  a64_append_section_header(out, sh_name_symtab, 2, 0, symtab_offset, symtab.len, strtab_shndx, function_symbol_base, 8, 24);
  a64_append_section_header(out, sh_name_strtab, 3, 0, strtab_offset, strtab.len, 0, 0, 1, 0);
  a64_append_section_header(out, sh_name_shstrtab, 3, 0, shstrtab_offset, shstrtab.len, 0, 0, 1, 0);

  a64_object_free(&ctx, function_offsets, function_sizes, symbol_names, &text, &rodata, &rela_text, &strtab, &symtab, &shstrtab);
  return true;
}

// =============================================================================================
// Direct-exe path — a self-contained ELF executable (machine 183) using raw Linux syscalls. Single
// PT_LOAD segment (r-x) holding the start stub, function bodies, the inline svc-write helper, and
// the rodata, all at the fixed base 0x400000 (non-PIE, so string addresses are absolute constants —
// no relocations needed). Mirrors emit_elf64.c's z_emit_elf64_exe_from_ir container.

static size_t a64_emit_exe_start_stub(ZBuf *text) {
  // At the ELF entry point the kernel leaves sp pointing at [argc, argv[0], argv[1], …, NULL, envp…].
  // Pass argc in x0 and argv (the char**) in x1 to main; main's prologue copies them into the callee-
  // saved x20/x21, where std.args.{len,get} read them. (main is exported as an arg-less `c fun` in the
  // self-host subset, so these registers are simply ignored when the program does not use std.args.)
  a64_emit_ldr_x_disp(text, 0, 31, 0); // x0 = argc = [sp]
  a64_emit_add_x_imm(text, 1, 31, 8);  // x1 = argv = sp + 8
  size_t patch = a64_emit_bl_placeholder(text); // bl main
  // exit(status in w0): SYS_exit = 93 in x8, svc #0. The conformance assertion hardcodes these.
  a64_emit_movz_x(text, 8, 93);
  a64_emit_word(text, 0xd4000001u); // svc #0
  return patch;
}

bool z_emit_elf_aarch64_exe_from_ir(const IrProgram *ir, ZBuf *out, ZDiag *diag) {
  if (!ir) return a64_diag_at(diag, "direct AArch64 ELF executable backend requires MIR", 1, 1, "missing MIR");
  if (!ir->mir_valid) {
    bool ok = a64_diag_at(diag, ir->mir_message[0] ? ir->mir_message : "direct backend lowering failed", ir->mir_line, ir->mir_column, ir->mir_actual);
    z_diag_set_backend_blocker(diag, &ir->backend_blocker);
    return ok;
  }
  unsigned main_index = 0;
  if (!a64_find_main(ir, &main_index, diag)) return false;
  for (size_t i = 0; i < ir->function_len; i++) {
    if (!a64_validate_function(&ir->functions[i], diag)) return false;
  }

  const uint64_t base_addr = 0x400000;
  const size_t ehdr_size = 64;
  const size_t phdr_size = 56;
  const size_t text_offset = ehdr_size + phdr_size;
  const uint64_t entry_addr = base_addr + text_offset;

  ZBuf text;
  ZBuf rodata;
  zbuf_init(&text);
  zbuf_init(&rodata);
  bool has_rodata = ir->readonly_data_bytes > 0 || ir->data_segment_len > 0;
  unsigned rodata_base_offset = a64_rodata_base_offset(ir);
  if (has_rodata) a64_append_rodata(&rodata, ir, rodata_base_offset);

  size_t *function_offsets = z_checked_calloc(ir->function_len, sizeof(size_t));
  if (!function_offsets) {
    zbuf_free(&text);
    zbuf_free(&rodata);
    return a64_diag_at(diag, "direct AArch64 ELF executable backend ran out of memory", 1, 1, "allocation failed");
  }
  Aarch64EmitContext ctx = {
    .ir = ir,
    .function_offsets = function_offsets,
    .function_count = ir->function_len,
    .rodata_base_offset = rodata_base_offset
  };

  size_t start_call_patch = a64_emit_exe_start_stub(&text);
  a64_pad_to(&text, a64_align(text.len, 16));
  for (size_t i = 0; i < ir->function_len; i++) {
    a64_pad_to(&text, a64_align(text.len, 4));
    function_offsets[i] = text.len;
    if (!a64_emit_function_text(&text, &ir->functions[i], true, &ctx, diag)) {
      a64_free_ctx(&ctx);
      free(function_offsets);
      zbuf_free(&text);
      zbuf_free(&rodata);
      return false;
    }
  }

  size_t world_write_offset = 0;
  if (ctx.world_write_used) {
    a64_pad_to(&text, a64_align(text.len, 4));
    world_write_offset = a64_emit_exe_world_write(&text);
  }
  a64_patch_branch26(&text, start_call_patch, function_offsets[main_index]);
  a64_patch_call_patches(&text, &ctx);
  // world.write call sites carry the sentinel callee_index == function_count; resolve them to the
  // inline svc-write helper (a64_patch_call_patches skipped them).
  for (size_t i = 0; i < ctx.call_patch_len; i++) {
    if (ctx.call_patches[i].callee_index >= ctx.function_count) {
      a64_patch_branch26(&text, ctx.call_patches[i].patch_offset, world_write_offset);
    }
  }

  size_t rodata_offset = has_rodata ? a64_align(text_offset + text.len, 8) : 0;
  ctx.rodata_addr = has_rodata ? base_addr + rodata_offset : 0;
  a64_patch_rodata_patches(&text, &ctx);
  uint64_t file_size = has_rodata ? rodata_offset + rodata.len : text_offset + text.len;

  zbuf_init(out);
  const unsigned char ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  a64_append_bytes(out, ident, sizeof(ident));
  a64_append_u16(out, 2);   // ET_EXEC
  a64_append_u16(out, 183); // EM_AARCH64
  a64_append_u32(out, 1);
  a64_append_u64(out, entry_addr);
  a64_append_u64(out, ehdr_size); // e_phoff
  a64_append_u64(out, 0);
  a64_append_u32(out, 0);
  a64_append_u16(out, 64);  // e_ehsize
  a64_append_u16(out, 56);  // e_phentsize
  a64_append_u16(out, 1);   // e_phnum
  a64_append_u16(out, 0);
  a64_append_u16(out, 0);
  a64_append_u16(out, 0);

  a64_append_u32(out, 1);   // PT_LOAD
  a64_append_u32(out, 5);   // r-x
  a64_append_u64(out, 0);
  a64_append_u64(out, base_addr);
  a64_append_u64(out, base_addr);
  a64_append_u64(out, file_size);
  a64_append_u64(out, file_size);
  a64_append_u64(out, 0x1000);

  a64_pad_to(out, text_offset);
  a64_append_bytes(out, (const unsigned char *)text.data, text.len);
  if (has_rodata) {
    a64_pad_to(out, rodata_offset);
    a64_append_bytes(out, (const unsigned char *)rodata.data, rodata.len);
  }

  a64_free_ctx(&ctx);
  free(function_offsets);
  zbuf_free(&text);
  zbuf_free(&rodata);
  return true;
}
