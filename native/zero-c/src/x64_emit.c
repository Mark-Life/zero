#include "x64_emit.h"

#include <stdlib.h>

void z_x64_append_u8(ZBuf *buf, unsigned value) {
  zbuf_append_char(buf, (char)(value & 0xffu));
}

void z_x64_append_u32(ZBuf *buf, uint32_t value) {
  z_x64_append_u8(buf, value);
  z_x64_append_u8(buf, value >> 8);
  z_x64_append_u8(buf, value >> 16);
  z_x64_append_u8(buf, value >> 24);
}

void z_x64_append_u64(ZBuf *buf, uint64_t value) {
  z_x64_append_u32(buf, (uint32_t)value);
  z_x64_append_u32(buf, (uint32_t)(value >> 32));
}

static void z_x64_emit_wide_prefix(ZBuf *buf, bool wide) {
  if (wide) z_x64_append_u8(buf, 0x48);
}

static void z_x64_require_reg(unsigned reg) {
  if (reg >= 16) abort();
}

static void z_x64_require_sib_index(unsigned reg) {
  z_x64_require_reg(reg);
  // SIB index field 4 without REX.X encodes no index, so rsp cannot be used here.
  if (reg == 4) abort();
}

void z_x64_patch_u32(ZBuf *buf, size_t offset, uint32_t value) {
  buf->data[offset + 0] = (char)(value & 0xffu);
  buf->data[offset + 1] = (char)((value >> 8) & 0xffu);
  buf->data[offset + 2] = (char)((value >> 16) & 0xffu);
  buf->data[offset + 3] = (char)((value >> 24) & 0xffu);
}

void z_x64_patch_rel32(ZBuf *buf, size_t patch_offset, size_t target_offset) {
  int64_t rel = (int64_t)target_offset - (int64_t)(patch_offset + 4);
  z_x64_patch_u32(buf, patch_offset, (uint32_t)(int32_t)rel);
}

size_t z_x64_emit_jmp32_placeholder(ZBuf *buf, unsigned opcode) {
  z_x64_append_u8(buf, opcode);
  size_t patch = buf->len;
  z_x64_append_u32(buf, 0);
  return patch;
}

size_t z_x64_emit_call32_placeholder(ZBuf *buf) {
  return z_x64_emit_jmp32_placeholder(buf, 0xe8);
}

size_t z_x64_emit_call_rip32_placeholder(ZBuf *buf) {
  z_x64_append_u8(buf, 0xff);
  z_x64_append_u8(buf, 0x15);
  size_t patch = buf->len;
  z_x64_append_u32(buf, 0);
  return patch;
}

size_t z_x64_emit_jcc32_placeholder(ZBuf *buf, unsigned condition) {
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, condition);
  size_t patch = buf->len;
  z_x64_append_u32(buf, 0);
  return patch;
}

void z_x64_emit_rbp_disp_reg(ZBuf *buf, unsigned opcode, unsigned reg, unsigned offset, bool wide) {
  if (wide || reg >= 8) {
    unsigned rex = wide ? 0x48 : 0x40;
    if (reg >= 8) rex |= 0x04;
    z_x64_append_u8(buf, rex);
  }
  z_x64_append_u8(buf, opcode);
  unsigned reg_low = reg & 7u;
  if (offset <= 127) {
    z_x64_append_u8(buf, 0x40 | (reg_low << 3) | 0x05);
    z_x64_append_u8(buf, (unsigned char)(-(int)offset));
  } else {
    z_x64_append_u8(buf, 0x80 | (reg_low << 3) | 0x05);
    z_x64_append_u32(buf, (uint32_t)(-(int32_t)offset));
  }
}

void z_x64_emit_load_rbp_positive_reg(ZBuf *buf, unsigned reg, unsigned offset, bool wide) {
  if (wide || reg >= 8) {
    unsigned rex = wide ? 0x48 : 0x40;
    if (reg >= 8) rex |= 0x04;
    z_x64_append_u8(buf, rex);
  }
  z_x64_append_u8(buf, 0x8b);
  unsigned reg_low = reg & 7u;
  if (offset <= 127) {
    z_x64_append_u8(buf, 0x40 | (reg_low << 3) | 0x05);
    z_x64_append_u8(buf, (unsigned char)offset);
  } else {
    z_x64_append_u8(buf, 0x80 | (reg_low << 3) | 0x05);
    z_x64_append_u32(buf, offset);
  }
}

static void z_x64_emit_rsp_offset_reg(ZBuf *buf, unsigned opcode, unsigned reg, unsigned offset, bool wide) {
  if (wide || reg >= 8) {
    unsigned rex = wide ? 0x48 : 0x40;
    if (reg >= 8) rex |= 0x04;
    z_x64_append_u8(buf, rex);
  }
  z_x64_append_u8(buf, opcode);
  unsigned reg_low = reg & 7u;
  if (offset == 0) {
    z_x64_append_u8(buf, (reg_low << 3) | 0x04);
    z_x64_append_u8(buf, 0x24);
  } else if (offset <= 127) {
    z_x64_append_u8(buf, 0x40 | (reg_low << 3) | 0x04);
    z_x64_append_u8(buf, 0x24);
    z_x64_append_u8(buf, offset);
  } else {
    z_x64_append_u8(buf, 0x80 | (reg_low << 3) | 0x04);
    z_x64_append_u8(buf, 0x24);
    z_x64_append_u32(buf, offset);
  }
}

void z_x64_emit_load_rsp_offset_reg(ZBuf *buf, unsigned reg, unsigned offset, bool wide) {
  z_x64_emit_rsp_offset_reg(buf, 0x8b, reg, offset, wide);
}

void z_x64_emit_store_rsp_offset_reg(ZBuf *buf, unsigned reg, unsigned offset, bool wide) {
  z_x64_emit_rsp_offset_reg(buf, 0x89, reg, offset, wide);
}

void z_x64_emit_lea_rsp_offset_reg(ZBuf *buf, unsigned reg, unsigned offset) {
  z_x64_emit_rsp_offset_reg(buf, 0x8d, reg, offset, true);
}

void z_x64_emit_mov_rsp_offset_u32(ZBuf *buf, unsigned offset, uint32_t value, bool wide) {
  if (wide) z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0xc7);
  if (offset == 0) {
    z_x64_append_u8(buf, 0x04);
    z_x64_append_u8(buf, 0x24);
  } else if (offset <= 127) {
    z_x64_append_u8(buf, 0x44);
    z_x64_append_u8(buf, 0x24);
    z_x64_append_u8(buf, offset);
  } else {
    z_x64_append_u8(buf, 0x84);
    z_x64_append_u8(buf, 0x24);
    z_x64_append_u32(buf, offset);
  }
  z_x64_append_u32(buf, value);
}

void z_x64_emit_inc_rsp_offset64(ZBuf *buf, unsigned offset) {
  z_x64_emit_rsp_offset_reg(buf, 0xff, 0, offset, true);
}

void z_x64_emit_add_rax_rsp_offset(ZBuf *buf, unsigned offset) {
  z_x64_emit_rsp_offset_reg(buf, 0x03, 0, offset, true);
}

void z_x64_emit_cmp_rax_rsp_offset(ZBuf *buf, unsigned offset) {
  z_x64_emit_rsp_offset_reg(buf, 0x3b, 0, offset, true);
}

void z_x64_emit_cmp_reg_reg(ZBuf *buf, unsigned lhs, unsigned rhs, bool wide) {
  z_x64_require_reg(lhs);
  z_x64_require_reg(rhs);
  unsigned rex = wide ? 0x48 : 0x40;
  if (rhs >= 8) rex |= 0x04;
  if (lhs >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x39);
  z_x64_append_u8(buf, 0xc0 | ((rhs & 7u) << 3) | (lhs & 7u));
}

void z_x64_emit_mov_reg_from_rax(ZBuf *buf, unsigned reg, bool wide) {
  z_x64_require_reg(reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xc0 | (reg & 7u));
}

void z_x64_emit_mov_reg_from_reg(ZBuf *buf, unsigned dst_reg, unsigned src_reg, bool wide) {
  z_x64_require_reg(dst_reg);
  z_x64_require_reg(src_reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (src_reg >= 8) rex |= 0x04;
  if (dst_reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xc0 | ((src_reg & 7u) << 3) | (dst_reg & 7u));
}

void z_x64_emit_cdqe(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x98);
}

void z_x64_emit_mov_reg_u32(ZBuf *buf, unsigned reg, uint32_t value) {
  z_x64_require_reg(reg);
  if (reg >= 8) z_x64_append_u8(buf, 0x41);
  z_x64_append_u8(buf, 0xb8 + (reg & 7u));
  z_x64_append_u32(buf, value);
}

void z_x64_emit_mov_reg_i32(ZBuf *buf, unsigned reg, int32_t value) {
  z_x64_require_reg(reg);
  unsigned rex = 0x48;
  if (reg >= 8) rex |= 0x01;
  z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0xc7);
  z_x64_append_u8(buf, 0xc0 | (reg & 7u));
  z_x64_append_u32(buf, (uint32_t)value);
}

void z_x64_emit_mov_reg_u64(ZBuf *buf, unsigned reg, uint64_t value) {
  z_x64_require_reg(reg);
  unsigned rex = 0x48;
  if (reg >= 8) rex |= 0x01;
  z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0xb8 + (reg & 7u));
  z_x64_append_u64(buf, value);
}

void z_x64_emit_xor_reg_reg(ZBuf *buf, unsigned reg, bool wide) {
  z_x64_require_reg(reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x05;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x31);
  z_x64_append_u8(buf, 0xc0 | ((reg & 7u) << 3) | (reg & 7u));
}

static void z_x64_emit_reg_reg_op(ZBuf *buf, unsigned opcode, unsigned dst_reg, unsigned src_reg, bool wide) {
  z_x64_require_reg(dst_reg);
  z_x64_require_reg(src_reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (src_reg >= 8) rex |= 0x04;
  if (dst_reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, opcode);
  z_x64_append_u8(buf, 0xc0 | ((src_reg & 7u) << 3) | (dst_reg & 7u));
}

void z_x64_emit_add_reg_reg(ZBuf *buf, unsigned dst_reg, unsigned src_reg, bool wide) {
  z_x64_emit_reg_reg_op(buf, 0x01, dst_reg, src_reg, wide);
}

void z_x64_emit_sub_reg_reg(ZBuf *buf, unsigned dst_reg, unsigned src_reg, bool wide) {
  z_x64_emit_reg_reg_op(buf, 0x29, dst_reg, src_reg, wide);
}

void z_x64_emit_xor_reg_from_reg(ZBuf *buf, unsigned dst_reg, unsigned src_reg, bool wide) {
  z_x64_emit_reg_reg_op(buf, 0x31, dst_reg, src_reg, wide);
}

static void z_x64_emit_reg_i8_op(ZBuf *buf, unsigned modrm_op, unsigned reg, int8_t value, bool wide) {
  if (modrm_op > 7) abort();
  z_x64_require_reg(reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x83);
  z_x64_append_u8(buf, 0xc0 | ((modrm_op & 7u) << 3) | (reg & 7u));
  z_x64_append_u8(buf, (uint8_t)value);
}

void z_x64_emit_add_reg_i8(ZBuf *buf, unsigned reg, int8_t value, bool wide) {
  z_x64_emit_reg_i8_op(buf, 0, reg, value, wide);
}

void z_x64_emit_and_reg_i8(ZBuf *buf, unsigned reg, int8_t value, bool wide) {
  z_x64_emit_reg_i8_op(buf, 4, reg, value, wide);
}

void z_x64_emit_cmp_reg_i8(ZBuf *buf, unsigned reg, int8_t value, bool wide) {
  z_x64_emit_reg_i8_op(buf, 7, reg, value, wide);
}

void z_x64_emit_and_reg_u32(ZBuf *buf, unsigned reg, uint32_t value, bool wide) {
  z_x64_require_reg(reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x81);
  z_x64_append_u8(buf, 0xe0 | (reg & 7u));
  z_x64_append_u32(buf, value);
}

void z_x64_emit_neg_reg(ZBuf *buf, unsigned reg, bool wide) {
  z_x64_require_reg(reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0xf7);
  z_x64_append_u8(buf, 0xd8 | (reg & 7u));
}

void z_x64_emit_shr_reg_one(ZBuf *buf, unsigned reg, bool wide) {
  z_x64_require_reg(reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0xd1);
  z_x64_append_u8(buf, 0xe8 | (reg & 7u));
}

static void z_x64_emit_shift_reg_imm8(ZBuf *buf, unsigned modrm_op, unsigned reg, unsigned amount, bool wide) {
  if (modrm_op > 7 || amount > 0xff) abort();
  z_x64_require_reg(reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0xc1);
  z_x64_append_u8(buf, 0xc0 | ((modrm_op & 7u) << 3) | (reg & 7u));
  z_x64_append_u8(buf, amount);
}

void z_x64_emit_shl_reg_imm8(ZBuf *buf, unsigned reg, unsigned amount, bool wide) {
  z_x64_emit_shift_reg_imm8(buf, 4, reg, amount, wide);
}

void z_x64_emit_shr_reg_imm8(ZBuf *buf, unsigned reg, unsigned amount, bool wide) {
  z_x64_emit_shift_reg_imm8(buf, 5, reg, amount, wide);
}

void z_x64_emit_imul_reg_i32(ZBuf *buf, unsigned reg, int32_t value, bool wide) {
  z_x64_require_reg(reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x05;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x69);
  z_x64_append_u8(buf, 0xc0 | ((reg & 7u) << 3) | (reg & 7u));
  z_x64_append_u32(buf, (uint32_t)value);
}

void z_x64_emit_add_rax_u32(ZBuf *buf, uint32_t value, bool wide) {
  z_x64_emit_wide_prefix(buf, wide);
  z_x64_append_u8(buf, 0x05);
  z_x64_append_u32(buf, value);
}

void z_x64_emit_sub_rax_u32(ZBuf *buf, uint32_t value, bool wide) {
  z_x64_emit_wide_prefix(buf, wide);
  z_x64_append_u8(buf, 0x2d);
  z_x64_append_u32(buf, value);
}

// Add a u32 immediate to any register (REX.B handles r8-r15). ADD r/m, imm32 = 0x81 /0.
void z_x64_emit_add_reg_u32(ZBuf *buf, unsigned reg, uint32_t value, bool wide) {
  z_x64_require_reg(reg);
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x81);
  z_x64_append_u8(buf, 0xc0 | (reg & 7u));
  z_x64_append_u32(buf, value);
}

static unsigned z_x64_scale_bits(unsigned scale) {
  switch (scale) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
  }
  abort();
}

static void z_x64_emit_base_index_scale_disp_op(ZBuf *buf, unsigned opcode, unsigned reg, unsigned base_reg, unsigned index_reg, unsigned scale, unsigned disp, bool wide, bool reg_is_byte) {
  z_x64_require_reg(reg);
  z_x64_require_reg(base_reg);
  z_x64_require_sib_index(index_reg);
  bool force_rex = !wide && reg_is_byte && reg >= 4 && reg < 8;
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x04;
  if (index_reg >= 8) rex |= 0x02;
  if (base_reg >= 8) rex |= 0x01;
  if (rex != 0x40 || force_rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, opcode);
  bool needs_base_disp = (base_reg & 7u) == 5;
  unsigned mod = 0x80;
  if (disp == 0 && !needs_base_disp) mod = 0x00;
  else if (disp <= 127) mod = 0x40;
  z_x64_append_u8(buf, mod | ((reg & 7u) << 3) | 0x04);
  z_x64_append_u8(buf, (z_x64_scale_bits(scale) << 6) | ((index_reg & 7u) << 3) | (base_reg & 7u));
  if (mod == 0x40) z_x64_append_u8(buf, disp);
  else if (mod == 0x80) z_x64_append_u32(buf, disp);
}

static void z_x64_emit_base_index_reg(ZBuf *buf, unsigned opcode, unsigned reg, unsigned base_reg, unsigned index_reg, bool wide, bool reg_is_byte) {
  z_x64_emit_base_index_scale_disp_op(buf, opcode, reg, base_reg, index_reg, 1, 0, wide, reg_is_byte);
}

void z_x64_emit_load_reg8_base_index(ZBuf *buf, unsigned dst_reg, unsigned base_reg, unsigned index_reg) {
  z_x64_emit_base_index_reg(buf, 0x8a, dst_reg, base_reg, index_reg, false, true);
}

void z_x64_emit_movzx_reg32_base_index_u8(ZBuf *buf, unsigned dst_reg, unsigned base_reg, unsigned index_reg) {
  z_x64_require_reg(dst_reg);
  z_x64_require_reg(base_reg);
  z_x64_require_sib_index(index_reg);
  unsigned rex = 0x40;
  if (dst_reg >= 8) rex |= 0x04;
  if (index_reg >= 8) rex |= 0x02;
  if (base_reg >= 8) rex |= 0x01;
  if (rex != 0x40) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0xb6);
  unsigned mod = (base_reg & 7u) == 5 ? 0x40 : 0x00;
  z_x64_append_u8(buf, mod | ((dst_reg & 7u) << 3) | 0x04);
  z_x64_append_u8(buf, ((index_reg & 7u) << 3) | (base_reg & 7u));
  if (mod == 0x40) z_x64_append_u8(buf, 0);
}

void z_x64_emit_store_base_index_reg8(ZBuf *buf, unsigned base_reg, unsigned index_reg, unsigned src_reg) {
  z_x64_emit_base_index_reg(buf, 0x88, src_reg, base_reg, index_reg, false, true);
}

void z_x64_emit_cmp_base_index_reg8(ZBuf *buf, unsigned base_reg, unsigned index_reg, unsigned reg) {
  z_x64_emit_base_index_reg(buf, 0x38, reg, base_reg, index_reg, false, true);
}

void z_x64_emit_cmp_base_index_u8(ZBuf *buf, unsigned base_reg, unsigned index_reg, unsigned value) {
  if (value > 0xff) abort();
  z_x64_emit_base_index_reg(buf, 0x80, 7, base_reg, index_reg, false, false);
  z_x64_append_u8(buf, value);
}

void z_x64_emit_byte_copy_min_loop(ZBuf *buf) {
  // Inputs: rsi=source ptr, rcx=source len, rdi=destination ptr, rax=destination len. Output: rax=copied len.
  z_x64_emit_cmp_rax_rcx(buf, true);
  size_t keep_dst_len = z_x64_emit_jcc32_placeholder(buf, 0x86);
  z_x64_emit_mov_rax_from_rcx(buf);
  z_x64_patch_rel32(buf, keep_dst_len, buf->len);
  z_x64_emit_mov_rdx_from_rax(buf);
  z_x64_emit_xor_r8d_r8d(buf);
  size_t loop = buf->len;
  z_x64_emit_cmp_reg_reg(buf, 2, 8, true);
  size_t done = z_x64_emit_jcc32_placeholder(buf, 0x86);
  z_x64_emit_load_reg8_base_index(buf, 10, 6, 8);
  z_x64_emit_store_base_index_reg8(buf, 7, 8, 10);
  z_x64_emit_inc_r8(buf);
  size_t back = z_x64_emit_jmp32_placeholder(buf, 0xe9);
  z_x64_patch_rel32(buf, back, loop);
  z_x64_patch_rel32(buf, done, buf->len);
  z_x64_emit_mov_rax_from_rdx(buf);
}

void z_x64_emit_byte_fill_loop(ZBuf *buf) {
  // Inputs: rdi=destination ptr, rdx=len, r9b=fill byte. Output: rax=len.
  z_x64_emit_xor_r8d_r8d(buf);
  size_t loop = buf->len;
  z_x64_emit_cmp_reg_reg(buf, 2, 8, true);
  size_t done = z_x64_emit_jcc32_placeholder(buf, 0x86);
  z_x64_emit_store_base_index_reg8(buf, 7, 8, 9);
  z_x64_emit_inc_r8(buf);
  size_t back = z_x64_emit_jmp32_placeholder(buf, 0xe9);
  z_x64_patch_rel32(buf, back, loop);
  z_x64_patch_rel32(buf, done, buf->len);
  z_x64_emit_mov_rax_from_rdx(buf);
}

void z_x64_emit_byte_eq_loop(ZBuf *buf) {
  // Inputs: r8=left ptr, r9=right ptr, r10=len. Output: eax=Bool.
  z_x64_emit_xor_ecx_ecx(buf);
  size_t loop = buf->len;
  z_x64_emit_cmp_reg_reg(buf, 1, 10, true);
  size_t equal = z_x64_emit_jcc32_placeholder(buf, 0x83);
  z_x64_emit_load_reg8_base_index(buf, 0, 8, 1);
  z_x64_emit_cmp_base_index_reg8(buf, 9, 1, 0);
  size_t mismatch = z_x64_emit_jcc32_placeholder(buf, 0x85);
  z_x64_emit_inc_rcx(buf);
  size_t back = z_x64_emit_jmp32_placeholder(buf, 0xe9);
  z_x64_patch_rel32(buf, back, loop);
  z_x64_patch_rel32(buf, mismatch, buf->len);
  z_x64_emit_mov_eax_u32(buf, 0);
  size_t after_false = z_x64_emit_jmp32_placeholder(buf, 0xe9);
  z_x64_patch_rel32(buf, equal, buf->len);
  z_x64_emit_mov_eax_u32(buf, 1);
  z_x64_patch_rel32(buf, after_false, buf->len);
}

void z_x64_emit_load_base_index_scale_disp_reg(ZBuf *buf, unsigned dst_reg, unsigned base_reg, unsigned index_reg, unsigned scale, unsigned disp, bool wide) {
  z_x64_emit_base_index_scale_disp_op(buf, 0x8b, dst_reg, base_reg, index_reg, scale, disp, wide, false);
}

void z_x64_emit_lea_base_index_scale_disp_reg(ZBuf *buf, unsigned dst_reg, unsigned base_reg, unsigned index_reg, unsigned scale, unsigned disp) {
  z_x64_emit_base_index_scale_disp_op(buf, 0x8d, dst_reg, base_reg, index_reg, scale, disp, true, false);
}

void z_x64_emit_xor_r8d_r8d(ZBuf *buf) {
  z_x64_append_u8(buf, 0x45);
  z_x64_append_u8(buf, 0x31);
  z_x64_append_u8(buf, 0xc0);
}

void z_x64_emit_push_reg64(ZBuf *buf, unsigned reg) {
  if (reg >= 8) z_x64_append_u8(buf, 0x41);
  z_x64_append_u8(buf, 0x50 + (reg & 7u));
}

void z_x64_emit_pop_reg64(ZBuf *buf, unsigned reg) {
  if (reg >= 8) z_x64_append_u8(buf, 0x41);
  z_x64_append_u8(buf, 0x58 + (reg & 7u));
}

void z_x64_emit_push_rax(ZBuf *buf) {
  z_x64_emit_push_reg64(buf, 0);
}

void z_x64_emit_pop_rax(ZBuf *buf) {
  z_x64_emit_pop_reg64(buf, 0);
}

void z_x64_emit_mov_rcx_from_rax(ZBuf *buf, bool wide) {
  z_x64_emit_wide_prefix(buf, wide);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xc1);
}

void z_x64_emit_mov_r9_from_rax(ZBuf *buf) {
  z_x64_append_u8(buf, 0x49);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xc1);
}

void z_x64_emit_mov_rax_from_rcx(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xc8);
}

void z_x64_emit_mov_rdx_from_rax(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xc2);
}

void z_x64_emit_mov_rdi_from_rax(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xc7);
}

void z_x64_emit_mov_rsi_from_rax(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xc6);
}

void z_x64_emit_mov_rsi_from_rsp(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xe6);
}

void z_x64_emit_mov_rax_from_rdx(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xd0);
}

void z_x64_emit_mov_rax_from_rdi(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xf8);
}

void z_x64_emit_mov_eax_from_ecx(ZBuf *buf) {
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xc8);
}

size_t z_x64_emit_mov_rax_u64_patchable(ZBuf *buf, uint64_t value) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0xb8);
  size_t patch = buf->len;
  z_x64_append_u64(buf, value);
  return patch;
}

void z_x64_emit_mov_rax_u64(ZBuf *buf, uint64_t value) {
  (void)z_x64_emit_mov_rax_u64_patchable(buf, value);
}

void z_x64_emit_xor_eax_eax(ZBuf *buf) {
  z_x64_append_u8(buf, 0x31);
  z_x64_append_u8(buf, 0xc0);
}

void z_x64_emit_xor_ecx_ecx(ZBuf *buf) {
  z_x64_append_u8(buf, 0x31);
  z_x64_append_u8(buf, 0xc9);
}

void z_x64_emit_xor_rdi_rdi(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x31);
  z_x64_append_u8(buf, 0xff);
}

void z_x64_emit_xor_rax_rax(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_emit_xor_eax_eax(buf);
}

void z_x64_emit_inc_ecx(ZBuf *buf) {
  z_x64_append_u8(buf, 0xff);
  z_x64_append_u8(buf, 0xc1);
}

void z_x64_emit_inc_rcx(ZBuf *buf) {
  z_x64_append_u8(buf, 0x48);
  z_x64_emit_inc_ecx(buf);
}

void z_x64_emit_inc_r8(ZBuf *buf) {
  z_x64_append_u8(buf, 0x49);
  z_x64_append_u8(buf, 0xff);
  z_x64_append_u8(buf, 0xc0);
}

void z_x64_emit_dec_r8d(ZBuf *buf) {
  z_x64_append_u8(buf, 0x41);
  z_x64_append_u8(buf, 0xff);
  z_x64_append_u8(buf, 0xc8);
}

void z_x64_emit_add_rax_rcx(ZBuf *buf, bool wide) {
  z_x64_emit_add_reg_reg(buf, 0, 1, wide);
}

void z_x64_emit_sub_rax_rcx(ZBuf *buf, bool wide) {
  z_x64_emit_sub_reg_reg(buf, 0, 1, wide);
}

void z_x64_emit_imul_rax_rcx(ZBuf *buf, bool wide) {
  z_x64_emit_wide_prefix(buf, wide);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0xaf);
  z_x64_append_u8(buf, 0xc1);
}

void z_x64_emit_and_rax_rcx(ZBuf *buf, bool wide) {
  z_x64_emit_wide_prefix(buf, wide);
  z_x64_append_u8(buf, 0x21);
  z_x64_append_u8(buf, 0xc8);
}

void z_x64_emit_or_rax_rcx(ZBuf *buf, bool wide) {
  z_x64_emit_wide_prefix(buf, wide);
  z_x64_append_u8(buf, 0x09);
  z_x64_append_u8(buf, 0xc8);
}

void z_x64_emit_add_rdx_rcx(ZBuf *buf, bool wide) {
  z_x64_emit_add_reg_reg(buf, 2, 1, wide);
}

void z_x64_emit_shl_rcx_imm8(ZBuf *buf, unsigned amount) {
  z_x64_emit_shl_reg_imm8(buf, 1, amount, true);
}

void z_x64_emit_shr_rcx_imm8(ZBuf *buf, unsigned amount) {
  z_x64_emit_shr_reg_imm8(buf, 1, amount, true);
}

static void z_x64_emit_ptr_reg_disp_op_ex(ZBuf *buf, unsigned escape, unsigned opcode, unsigned reg, unsigned base_reg, unsigned disp, bool wide, bool reg_is_byte, bool force_disp32) {
  z_x64_require_reg(reg);
  z_x64_require_reg(base_reg);
  bool force_rex = !wide && reg_is_byte && reg >= 4 && reg < 8;
  unsigned rex = wide ? 0x48 : 0x40;
  if (reg >= 8) rex |= 0x04;
  if (base_reg >= 8) rex |= 0x01;
  if (rex != 0x40 || force_rex) z_x64_append_u8(buf, rex);
  if (escape) z_x64_append_u8(buf, escape);
  z_x64_append_u8(buf, opcode);
  bool sib = (base_reg & 7u) == 4;
  bool needs_base_disp = (base_reg & 7u) == 5;
  unsigned mod = 0x80;
  if (!force_disp32) {
    if (disp == 0 && !needs_base_disp) mod = 0x00;
    else if (disp <= 127) mod = 0x40;
  }
  z_x64_append_u8(buf, mod | ((reg & 7u) << 3) | (sib ? 0x04 : (base_reg & 7u)));
  if (sib) z_x64_append_u8(buf, 0x20 | (base_reg & 7u));
  if (mod == 0x40) z_x64_append_u8(buf, disp);
  else if (mod == 0x80) z_x64_append_u32(buf, disp);
}

static void z_x64_emit_ptr_reg_disp_op(ZBuf *buf, unsigned escape, unsigned opcode, unsigned reg, unsigned base_reg, unsigned disp, bool wide, bool reg_is_byte) {
  z_x64_emit_ptr_reg_disp_op_ex(buf, escape, opcode, reg, base_reg, disp, wide, reg_is_byte, false);
}

void z_x64_emit_movzx_reg32_ptr_reg_u8(ZBuf *buf, unsigned dst_reg, unsigned base_reg) { z_x64_emit_ptr_reg_disp_op(buf, 0x0f, 0xb6, dst_reg, base_reg, 0, false, false); }

void z_x64_emit_movzx_reg32_ptr_reg_disp_u16(ZBuf *buf, unsigned dst_reg, unsigned base_reg, unsigned disp) { z_x64_emit_ptr_reg_disp_op(buf, 0x0f, 0xb7, dst_reg, base_reg, disp, false, false); }

void z_x64_emit_load_reg_ptr_reg(ZBuf *buf, unsigned dst_reg, unsigned base_reg, bool wide) { z_x64_emit_ptr_reg_disp_op(buf, 0, 0x8b, dst_reg, base_reg, 0, wide, false); }

// Same as z_x64_emit_load_reg_ptr_reg but always encodes a mod=10 disp32 displacement (even when
// disp would fit in a byte or be zero). Useful when matching exact byte output of legacy raw
// sequences or when the caller wants a fixed-size 7-byte form for later patching.
void z_x64_emit_load_reg_ptr_reg_disp(ZBuf *buf, unsigned dst_reg, unsigned base_reg, unsigned disp, bool wide) { z_x64_emit_ptr_reg_disp_op_ex(buf, 0, 0x8b, dst_reg, base_reg, disp, wide, false, true); }

// Sign-extending byte load: MOVSX r32, byte ptr [base] (0F BE /r). The signed-byte counterpart
// of the zero-extending MOVZX above; used for i8 typed-span element reads.
void z_x64_emit_movsx_reg32_ptr_reg_i8(ZBuf *buf, unsigned dst_reg, unsigned base_reg) { z_x64_emit_ptr_reg_disp_op(buf, 0x0f, 0xbe, dst_reg, base_reg, 0, false, false); }

void z_x64_emit_mov_ptr_reg_disp_u8(ZBuf *buf, unsigned base_reg, unsigned disp, unsigned value) {
  if (value > 0xff) abort();
  z_x64_emit_ptr_reg_disp_op(buf, 0, 0xc6, 0, base_reg, disp, false, false);
  z_x64_append_u8(buf, value);
}

void z_x64_emit_store_ptr_reg8_from_reg(ZBuf *buf, unsigned base_reg, unsigned src_reg) { z_x64_emit_ptr_reg_disp_op(buf, 0, 0x88, src_reg, base_reg, 0, false, true); }

void z_x64_emit_store_ptr_reg_from_reg(ZBuf *buf, unsigned base_reg, unsigned src_reg, bool wide) { z_x64_emit_ptr_reg_disp_op(buf, 0, 0x89, src_reg, base_reg, 0, wide, false); }

// Same as z_x64_emit_store_ptr_reg_from_reg but always encodes a mod=10 disp32 displacement (even
// when disp would fit in a byte or be zero). Useful when matching exact byte output of legacy raw
// sequences or when the caller wants a fixed-size 7-byte form for later patching.
void z_x64_emit_store_ptr_reg_disp_from_reg(ZBuf *buf, unsigned base_reg, unsigned disp, unsigned src_reg, bool wide) { z_x64_emit_ptr_reg_disp_op_ex(buf, 0, 0x89, src_reg, base_reg, disp, wide, false, true); }

void z_x64_emit_cmp_reg_ptr_reg(ZBuf *buf, unsigned lhs_reg, unsigned base_reg, bool wide) { z_x64_emit_ptr_reg_disp_op(buf, 0, 0x3b, lhs_reg, base_reg, 0, wide, false); }

void z_x64_emit_div_rax_rcx(ZBuf *buf, bool wide, bool uns, bool keep_remainder) {
  if (uns) {
    z_x64_emit_wide_prefix(buf, wide);
    z_x64_append_u8(buf, 0x31);
    z_x64_append_u8(buf, 0xd2);
    z_x64_emit_wide_prefix(buf, wide);
    z_x64_append_u8(buf, 0xf7);
    z_x64_append_u8(buf, 0xf1);
  } else {
    z_x64_emit_wide_prefix(buf, wide);
    z_x64_append_u8(buf, 0x99);
    z_x64_emit_wide_prefix(buf, wide);
    z_x64_append_u8(buf, 0xf7);
    z_x64_append_u8(buf, 0xf9);
  }
  if (keep_remainder) {
    z_x64_emit_wide_prefix(buf, wide);
    z_x64_append_u8(buf, 0x89);
    z_x64_append_u8(buf, 0xd0);
  }
}

void z_x64_emit_test_reg_reg(ZBuf *buf, unsigned reg, bool wide) {
  z_x64_emit_reg_reg_op(buf, 0x85, reg, reg, wide);
}

void z_x64_emit_test_rax_rax(ZBuf *buf, bool wide) {
  z_x64_emit_test_reg_reg(buf, 0, wide);
}

void z_x64_emit_test_ecx_ecx(ZBuf *buf) {
  z_x64_emit_test_reg_reg(buf, 1, false);
}

void z_x64_emit_cmp_rax_rcx(ZBuf *buf, bool wide) {
  z_x64_emit_cmp_reg_reg(buf, 0, 1, wide);
}

void z_x64_emit_setcc_al_to_bool(ZBuf *buf, unsigned setcc_opcode) {
  if (setcc_opcode < 0x90 || setcc_opcode > 0x9f) abort();
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, setcc_opcode);
  z_x64_append_u8(buf, 0xc0);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0xb6);
  z_x64_append_u8(buf, 0xc0);
}

// SETcc AL: 0F <cc> C0 (reg field 0, rm = AL = 000). No trailing MOVZX, unlike
// z_x64_emit_setcc_al_to_bool — use this when a parity fixup must run before the widen.
void z_x64_emit_setcc_al(ZBuf *buf, uint8_t cc_opcode) {
  if (cc_opcode < 0x90 || cc_opcode > 0x9f) abort();
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, cc_opcode);
  z_x64_append_u8(buf, 0xc0);
}

// MOVZX eax, al: 0F B6 C0. Zero-extends the bool byte to a full 32-bit value.
void z_x64_emit_movzx_eax_al(ZBuf *buf) {
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0xb6);
  z_x64_append_u8(buf, 0xc0);
}

// MOVSX eax, al (0F BE C0): sign-extend the low byte across the 32-bit register. The signed-byte
// counterpart of z_x64_emit_movzx_eax_al; used to normalize a value cast to i8.
void z_x64_emit_movsx_eax_al(ZBuf *buf) {
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0xbe);
  z_x64_append_u8(buf, 0xc0);
}

void z_x64_emit_cmp_rax_rcx_to_bool(ZBuf *buf, unsigned setcc_opcode, bool wide) {
  z_x64_emit_cmp_rax_rcx(buf, wide);
  z_x64_emit_setcc_al_to_bool(buf, setcc_opcode);
}

void z_x64_emit_bool_from_nonnegative_rax(ZBuf *buf) {
  z_x64_emit_test_rax_rax(buf, true);
  z_x64_emit_setcc_al_to_bool(buf, 0x99);
}

// MOVD/MOVQ xmm <- gpr (raw bits): 66 [REX.W] 0F 6E /r.
void z_x64_emit_movd_xmm_from_gpr(ZBuf *buf, unsigned xmm, unsigned gpr, bool is64) {
  z_x64_require_reg(xmm);
  z_x64_require_reg(gpr);
  z_x64_append_u8(buf, 0x66);
  unsigned rex = 0;
  if (is64) rex |= 0x48;
  if (xmm >= 8) rex |= 0x44;
  if (gpr >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x6e);
  z_x64_append_u8(buf, 0xc0 | ((xmm & 7u) << 3) | (gpr & 7u));
}

// SSE scalar arithmetic: ADDSS/SD (58), SUBSS/SD (5C), MULSS/SD (59), DIVSS/SD (5E).
// F32 uses the F3 prefix; F64 uses F2. dst is the destination XMM, src the source XMM.
static void z_x64_emit_sse_arith(ZBuf *buf, unsigned opcode, unsigned dst, unsigned src, bool is64) {
  z_x64_require_reg(dst);
  z_x64_require_reg(src);
  z_x64_append_u8(buf, is64 ? 0xf2 : 0xf3);
  unsigned rex = 0;
  if (dst >= 8) rex |= 0x44;
  if (src >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, opcode);
  z_x64_append_u8(buf, 0xc0 | ((dst & 7u) << 3) | (src & 7u));
}

void z_x64_emit_sse_add(ZBuf *buf, unsigned dst, unsigned src, bool is64) {
  z_x64_emit_sse_arith(buf, 0x58, dst, src, is64);
}

void z_x64_emit_sse_sub(ZBuf *buf, unsigned dst, unsigned src, bool is64) {
  z_x64_emit_sse_arith(buf, 0x5c, dst, src, is64);
}

void z_x64_emit_sse_mul(ZBuf *buf, unsigned dst, unsigned src, bool is64) {
  z_x64_emit_sse_arith(buf, 0x59, dst, src, is64);
}

void z_x64_emit_sse_div(ZBuf *buf, unsigned dst, unsigned src, bool is64) {
  z_x64_emit_sse_arith(buf, 0x5e, dst, src, is64);
}

// UCOMISS (0F 2E /r) / UCOMISD (66 0F 2E /r): IEEE 754 unordered compare into EFLAGS.
void z_x64_emit_ucomis(ZBuf *buf, unsigned lhs, unsigned rhs, bool is64) {
  z_x64_require_reg(lhs);
  z_x64_require_reg(rhs);
  if (is64) z_x64_append_u8(buf, 0x66);
  unsigned rex = 0;
  if (lhs >= 8) rex |= 0x44;
  if (rhs >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x2e);
  z_x64_append_u8(buf, 0xc0 | ((lhs & 7u) << 3) | (rhs & 7u));
}

// CVTSI2SS (F3 [REX.W] 0F 2A /r) / CVTSI2SD (F2 ...): int -> float. src_is64 sets REX.W.
void z_x64_emit_cvtsi2s(ZBuf *buf, unsigned xmm, unsigned gpr, bool dst_is64, bool src_is64) {
  z_x64_require_reg(xmm);
  z_x64_require_reg(gpr);
  z_x64_append_u8(buf, dst_is64 ? 0xf2 : 0xf3);
  unsigned rex = 0;
  if (src_is64) rex |= 0x48;
  if (xmm >= 8) rex |= 0x44;
  if (gpr >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x2a);
  z_x64_append_u8(buf, 0xc0 | ((xmm & 7u) << 3) | (gpr & 7u));
}

// CVTTSS2SI (F3 [REX.W] 0F 2C /r) / CVTTSD2SI (F2 ...): float -> int, truncating. dst_is64 sets REX.W.
void z_x64_emit_cvtts2si(ZBuf *buf, unsigned gpr, unsigned xmm, bool src_is64, bool dst_is64) {
  z_x64_require_reg(gpr);
  z_x64_require_reg(xmm);
  z_x64_append_u8(buf, src_is64 ? 0xf2 : 0xf3);
  unsigned rex = 0;
  if (dst_is64) rex |= 0x48;
  if (gpr >= 8) rex |= 0x44;
  if (xmm >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x2c);
  z_x64_append_u8(buf, 0xc0 | ((gpr & 7u) << 3) | (xmm & 7u));
}

// CVTSS2SD (F3 0F 5A /r) / CVTSD2SS (F2 0F 5A /r): float<->float. src_is64 picks the source width.
void z_x64_emit_cvts2s(ZBuf *buf, unsigned dst, unsigned src, bool src_is64) {
  z_x64_require_reg(dst);
  z_x64_require_reg(src);
  z_x64_append_u8(buf, src_is64 ? 0xf2 : 0xf3);
  unsigned rex = 0;
  if (dst >= 8) rex |= 0x44;
  if (src >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x5a);
  z_x64_append_u8(buf, 0xc0 | ((dst & 7u) << 3) | (src & 7u));
}

// MOVAPS xmm, xmm (0F 28 /r): copy a full XMM register. No mandatory prefix.
void z_x64_emit_movaps(ZBuf *buf, unsigned dst, unsigned src) {
  z_x64_require_reg(dst);
  z_x64_require_reg(src);
  unsigned rex = 0;
  if (dst >= 8) rex |= 0x44;
  if (src >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x28);
  z_x64_append_u8(buf, 0xc0 | ((dst & 7u) << 3) | (src & 7u));
}

// MOVSS/MOVSD xmm,[rbp+disp] (load, opcode 0x10) or [rbp+disp],xmm (store, opcode 0x11).
// base = RBP (101) always uses a displacement; disp8 when it fits a signed byte, else disp32.
void z_x64_emit_movs_xmm_rbp_disp(ZBuf *buf, unsigned xmm, int32_t disp, bool is64, bool is_load) {
  z_x64_require_reg(xmm);
  z_x64_append_u8(buf, is64 ? 0xf2 : 0xf3);
  unsigned rex = 0;
  if (xmm >= 8) rex |= 0x44;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, is_load ? 0x10 : 0x11);
  unsigned reg_low = xmm & 7u;
  if (disp >= -128 && disp <= 127) {
    z_x64_append_u8(buf, 0x40 | (reg_low << 3) | 0x05);
    z_x64_append_u8(buf, (uint8_t)(int8_t)disp);
  } else {
    z_x64_append_u8(buf, 0x80 | (reg_low << 3) | 0x05);
    z_x64_append_u32(buf, (uint32_t)disp);
  }
}

// Scalar SSE load/store through a register-held address: MOVSS/MOVSD xmm,[base] (is_load) or
// [base],xmm. F3 prefix for f32, F2 for f64; opcode 10 loads, 11 stores. A no-displacement
// [base] operand uses mod=00 — except rbp/r13 (rm=101) which require an explicit disp8=0, and
// rsp/r12 (rm=100) which require a SIB byte. Used for typed-span float element loads/stores.
void z_x64_emit_movs_xmm_ptr_reg(ZBuf *buf, unsigned xmm, unsigned base_reg, bool is64, bool is_load) {
  z_x64_require_reg(xmm);
  z_x64_require_reg(base_reg);
  z_x64_append_u8(buf, is64 ? 0xf2 : 0xf3);
  unsigned rex = 0;
  if (xmm >= 8) rex |= 0x44;
  if (base_reg >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, is_load ? 0x10 : 0x11);
  unsigned reg_low = xmm & 7u;
  unsigned base_low = base_reg & 7u;
  bool sib = base_low == 4;
  bool needs_disp = base_low == 5;
  unsigned mod = needs_disp ? 0x40 : 0x00;
  z_x64_append_u8(buf, mod | (reg_low << 3) | (sib ? 0x04 : base_low));
  if (sib) z_x64_append_u8(buf, 0x20 | base_low);
  if (needs_disp) z_x64_append_u8(buf, 0x00);
}

// MOVSS/MOVSD xmm,[base+disp] or [base+disp],xmm. F3/F2 prefix; opcode 10/11 load/store.
// Same encoding shape as the disp=0 variant above, but always emits a disp8 or disp32 so the
// field offset within a ref<Record> deref'd base survives. SIB (rsp/r12) and rbp/r13 base
// quirks are handled exactly as the disp=0 variant: SIB byte for rsp-family, mod=01/10 for
// non-zero disp.
void z_x64_emit_movs_xmm_ptr_reg_disp(ZBuf *buf, unsigned xmm, unsigned base_reg, unsigned disp, bool is64, bool is_load) {
  z_x64_require_reg(xmm);
  z_x64_require_reg(base_reg);
  z_x64_append_u8(buf, is64 ? 0xf2 : 0xf3);
  unsigned rex = 0;
  if (xmm >= 8) rex |= 0x44;
  if (base_reg >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, is_load ? 0x10 : 0x11);
  unsigned reg_low = xmm & 7u;
  unsigned base_low = base_reg & 7u;
  bool sib = base_low == 4;
  unsigned mod = disp <= 127 ? 0x40 : 0x80;
  z_x64_append_u8(buf, mod | (reg_low << 3) | (sib ? 0x04 : base_low));
  if (sib) z_x64_append_u8(buf, 0x20 | base_low);
  if (mod == 0x40) z_x64_append_u8(buf, (uint8_t)disp);
  else z_x64_append_u32(buf, disp);
}

// MOVZX r32, byte ptr [base+disp] — zero-extend a byte load through a register-held base into
// the low 32 bits of a 32-bit destination (high 32 cleared automatically by the 32-bit form).
// Used for u8/Bool field reads through a ref<Record>'s deref'd pointer.
void z_x64_emit_movzx_reg32_ptr_reg_disp_u8(ZBuf *buf, unsigned dst_reg, unsigned base_reg, unsigned disp) {
  z_x64_require_reg(dst_reg);
  z_x64_require_reg(base_reg);
  unsigned rex = 0;
  if (dst_reg >= 8) rex |= 0x44;
  if (base_reg >= 8) rex |= 0x41;
  if (rex) z_x64_append_u8(buf, rex);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0xb6);
  unsigned reg_low = dst_reg & 7u;
  unsigned base_low = base_reg & 7u;
  bool sib = base_low == 4;
  unsigned mod = disp <= 127 ? 0x40 : 0x80;
  z_x64_append_u8(buf, mod | (reg_low << 3) | (sib ? 0x04 : base_low));
  if (sib) z_x64_append_u8(buf, 0x20 | base_low);
  if (mod == 0x40) z_x64_append_u8(buf, (uint8_t)disp);
  else z_x64_append_u32(buf, disp);
}

// MOV byte ptr [base+disp], reg8 — store the low byte of a GPR through a register-held base.
// Used for u8/Bool field writes through a mutref<Record>'s deref'd pointer.
void z_x64_emit_store_ptr_reg_disp_from_reg8(ZBuf *buf, unsigned base_reg, unsigned disp, unsigned src_reg) {
  z_x64_require_reg(src_reg);
  z_x64_require_reg(base_reg);
  unsigned rex = 0;
  if (src_reg >= 8) rex |= 0x44;
  if (base_reg >= 8) rex |= 0x41;
  // Force REX for spl/bpl/sil/dil (regs 4-7) byte access, just like z_x64_emit_ptr_reg_disp_op_ex.
  bool force_rex = src_reg >= 4 && src_reg < 8;
  if (rex || force_rex) z_x64_append_u8(buf, rex ? rex : 0x40);
  z_x64_append_u8(buf, 0x88);
  unsigned reg_low = src_reg & 7u;
  unsigned base_low = base_reg & 7u;
  bool sib = base_low == 4;
  unsigned mod = disp <= 127 ? 0x40 : 0x80;
  z_x64_append_u8(buf, mod | (reg_low << 3) | (sib ? 0x04 : base_low));
  if (sib) z_x64_append_u8(buf, 0x20 | base_low);
  if (mod == 0x40) z_x64_append_u8(buf, (uint8_t)disp);
  else z_x64_append_u32(buf, disp);
}

// Spill an XMM: sub rsp,8 then MOVSD [rsp],xmm (F2 0F 11 /r, SIB base=rsp).
void z_x64_emit_xmm_push(ZBuf *buf, unsigned xmm) {
  z_x64_require_reg(xmm);
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x83);
  z_x64_append_u8(buf, 0xec);
  z_x64_append_u8(buf, 0x08);
  z_x64_append_u8(buf, 0xf2);
  if (xmm >= 8) z_x64_append_u8(buf, 0x44);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x11);
  z_x64_append_u8(buf, 0x04 | ((xmm & 7u) << 3));
  z_x64_append_u8(buf, 0x24);
}

// Reload an XMM: MOVSD xmm,[rsp] (F2 0F 10 /r, SIB base=rsp) then add rsp,8.
void z_x64_emit_xmm_pop(ZBuf *buf, unsigned xmm) {
  z_x64_require_reg(xmm);
  z_x64_append_u8(buf, 0xf2);
  if (xmm >= 8) z_x64_append_u8(buf, 0x44);
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x10);
  z_x64_append_u8(buf, 0x04 | ((xmm & 7u) << 3));
  z_x64_append_u8(buf, 0x24);
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x83);
  z_x64_append_u8(buf, 0xc4);
  z_x64_append_u8(buf, 0x08);
}

// SETcc CL: 0F <cc> C1 (reg field 0, rm = CL = 001).
void z_x64_emit_setcc_cl(ZBuf *buf, uint8_t cc_opcode) {
  if (cc_opcode < 0x90 || cc_opcode > 0x9f) abort();
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, cc_opcode);
  z_x64_append_u8(buf, 0xc1);
}

// AND al, cl: 20 C8.
void z_x64_emit_and_al_cl(ZBuf *buf) {
  z_x64_append_u8(buf, 0x20);
  z_x64_append_u8(buf, 0xc8);
}

// OR al, cl: 08 C8.
void z_x64_emit_or_al_cl(ZBuf *buf) {
  z_x64_append_u8(buf, 0x08);
  z_x64_append_u8(buf, 0xc8);
}

void z_x64_emit_prologue(ZBuf *buf, unsigned stack_size) {
  z_x64_append_u8(buf, 0x55);
  z_x64_append_u8(buf, 0x48);
  z_x64_append_u8(buf, 0x89);
  z_x64_append_u8(buf, 0xe5);
  z_x64_emit_sub_rsp(buf, stack_size);
}

void z_x64_emit_epilogue(ZBuf *buf) {
  z_x64_append_u8(buf, 0xc9);
  z_x64_append_u8(buf, 0xc3);
}

void z_x64_emit_mov_eax_u32(ZBuf *buf, uint32_t value) {
  z_x64_append_u8(buf, 0xb8);
  z_x64_append_u32(buf, value);
}

void z_x64_emit_ud2(ZBuf *buf) {
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x0b);
}

void z_x64_emit_syscall(ZBuf *buf) {
  z_x64_append_u8(buf, 0x0f);
  z_x64_append_u8(buf, 0x05);
}

void z_x64_emit_sub_rsp(ZBuf *buf, unsigned amount) {
  if (amount == 0) return;
  z_x64_append_u8(buf, 0x48);
  if (amount <= 127) {
    z_x64_append_u8(buf, 0x83);
    z_x64_append_u8(buf, 0xec);
    z_x64_append_u8(buf, amount);
  } else {
    z_x64_append_u8(buf, 0x81);
    z_x64_append_u8(buf, 0xec);
    z_x64_append_u32(buf, amount);
  }
}

void z_x64_emit_add_rsp(ZBuf *buf, unsigned amount) {
  if (amount == 0) return;
  z_x64_append_u8(buf, 0x48);
  if (amount <= 127) {
    z_x64_append_u8(buf, 0x83);
    z_x64_append_u8(buf, 0xc4);
    z_x64_append_u8(buf, amount);
  } else {
    z_x64_append_u8(buf, 0x81);
    z_x64_append_u8(buf, 0xc4);
    z_x64_append_u32(buf, amount);
  }
}
