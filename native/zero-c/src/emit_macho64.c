#include "zero.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void append_u8(ZBuf *buf, unsigned value) {
  zbuf_append_char(buf, (char)(value & 0xffu));
}

static void append_u16le(ZBuf *buf, uint16_t value) {
  append_u8(buf, value);
  append_u8(buf, value >> 8);
}

static void append_u32le(ZBuf *buf, uint32_t value) {
  append_u8(buf, value);
  append_u8(buf, value >> 8);
  append_u8(buf, value >> 16);
  append_u8(buf, value >> 24);
}

static void append_u64le(ZBuf *buf, uint64_t value) {
  append_u32le(buf, (uint32_t)(value & 0xffffffffu));
  append_u32le(buf, (uint32_t)(value >> 32));
}

static void patch_bytes(ZBuf *buf, size_t offset, const unsigned char *bytes, size_t len) {
  if (!buf || !bytes || offset + len > buf->len) return;
  for (size_t i = 0; i < len; i++) buf->data[offset + i] = (char)bytes[i];
}

static void patch_u64le(ZBuf *buf, size_t offset, uint64_t value) {
  for (unsigned i = 0; i < 8; i++) buf->data[offset + i] = (char)((value >> (i * 8)) & 0xffu);
}

static void append_bytes(ZBuf *buf, const char *bytes, size_t len);
static size_t macho_align(size_t value, size_t alignment);

static void append_u8be(ZBuf *buf, unsigned value) {
  append_u8(buf, value);
}

static void append_u32be(ZBuf *buf, uint32_t value) {
  append_u8(buf, value >> 24);
  append_u8(buf, value >> 16);
  append_u8(buf, value >> 8);
  append_u8(buf, value);
}

static void append_u64be(ZBuf *buf, uint64_t value) {
  append_u32be(buf, (uint32_t)(value >> 32));
  append_u32be(buf, (uint32_t)(value & 0xffffffffu));
}

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  unsigned char data[64];
  size_t datalen;
} MachOSha256;

static uint32_t macho_sha_rotr(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32 - bits));
}

static void macho_sha256_transform(MachOSha256 *ctx, const unsigned char data[64]) {
  static const uint32_t k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
  };
  uint32_t m[64];
  for (unsigned i = 0; i < 16; i++) {
    m[i] = ((uint32_t)data[i * 4] << 24) |
           ((uint32_t)data[i * 4 + 1] << 16) |
           ((uint32_t)data[i * 4 + 2] << 8) |
           ((uint32_t)data[i * 4 + 3]);
  }
  for (unsigned i = 16; i < 64; i++) {
    uint32_t s0 = macho_sha_rotr(m[i - 15], 7) ^ macho_sha_rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
    uint32_t s1 = macho_sha_rotr(m[i - 2], 17) ^ macho_sha_rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
    m[i] = m[i - 16] + s0 + m[i - 7] + s1;
  }

  uint32_t a = ctx->state[0];
  uint32_t b = ctx->state[1];
  uint32_t c = ctx->state[2];
  uint32_t d = ctx->state[3];
  uint32_t e = ctx->state[4];
  uint32_t f = ctx->state[5];
  uint32_t g = ctx->state[6];
  uint32_t h = ctx->state[7];
  for (unsigned i = 0; i < 64; i++) {
    uint32_t s1 = macho_sha_rotr(e, 6) ^ macho_sha_rotr(e, 11) ^ macho_sha_rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + s1 + ch + k[i] + m[i];
    uint32_t s0 = macho_sha_rotr(a, 2) ^ macho_sha_rotr(a, 13) ^ macho_sha_rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

static void macho_sha256_init(MachOSha256 *ctx) {
  ctx->datalen = 0;
  ctx->bitlen = 0;
  ctx->state[0] = 0x6a09e667u;
  ctx->state[1] = 0xbb67ae85u;
  ctx->state[2] = 0x3c6ef372u;
  ctx->state[3] = 0xa54ff53au;
  ctx->state[4] = 0x510e527fu;
  ctx->state[5] = 0x9b05688cu;
  ctx->state[6] = 0x1f83d9abu;
  ctx->state[7] = 0x5be0cd19u;
}

static void macho_sha256_update(MachOSha256 *ctx, const unsigned char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    ctx->data[ctx->datalen++] = data[i];
    if (ctx->datalen == 64) {
      macho_sha256_transform(ctx, ctx->data);
      ctx->bitlen += 512;
      ctx->datalen = 0;
    }
  }
}

static void macho_sha256_final(MachOSha256 *ctx, unsigned char hash[32]) {
  size_t i = ctx->datalen;
  ctx->data[i++] = 0x80;
  if (i > 56) {
    while (i < 64) ctx->data[i++] = 0;
    macho_sha256_transform(ctx, ctx->data);
    i = 0;
  }
  while (i < 56) ctx->data[i++] = 0;
  ctx->bitlen += ctx->datalen * 8;
  for (unsigned j = 0; j < 8; j++) ctx->data[63 - j] = (unsigned char)(ctx->bitlen >> (j * 8));
  macho_sha256_transform(ctx, ctx->data);
  for (unsigned j = 0; j < 8; j++) {
    hash[j * 4] = (unsigned char)(ctx->state[j] >> 24);
    hash[j * 4 + 1] = (unsigned char)(ctx->state[j] >> 16);
    hash[j * 4 + 2] = (unsigned char)(ctx->state[j] >> 8);
    hash[j * 4 + 3] = (unsigned char)ctx->state[j];
  }
}

static void macho_sha256_hash(const unsigned char *data, size_t len, unsigned char hash[32]) {
  MachOSha256 ctx;
  macho_sha256_init(&ctx);
  macho_sha256_update(&ctx, data, len);
  macho_sha256_final(&ctx, hash);
}

static void macho_append_code_signature(ZBuf *sig, const unsigned char *code, size_t code_len, const char *identifier) {
  const uint32_t page_log = 12;
  const size_t page_size = 1u << page_log;
  const uint32_t hash_size = 32;
  const uint32_t nslots = (uint32_t)((code_len + page_size - 1) / page_size);
  const uint32_t cd_header_size = 88;
  const uint32_t ident_offset = cd_header_size;
  const uint32_t ident_len = (uint32_t)strlen(identifier) + 1;
  const uint32_t hash_offset = (uint32_t)macho_align(ident_offset + ident_len, 4);
  const uint32_t cd_length = hash_offset + nslots * hash_size;
  const uint32_t cd_offset = 20;
  const uint32_t super_length = cd_offset + cd_length;

  zbuf_init(sig);
  append_u32be(sig, 0xfade0cc0u);      // CSMAGIC_EMBEDDED_SIGNATURE
  append_u32be(sig, super_length);
  append_u32be(sig, 1);
  append_u32be(sig, 0);                // CSSLOT_CODEDIRECTORY
  append_u32be(sig, cd_offset);

  append_u32be(sig, 0xfade0c02u);      // CSMAGIC_CODEDIRECTORY
  append_u32be(sig, cd_length);
  append_u32be(sig, 0x00020400u);
  append_u32be(sig, 0x00000002u);      // ad-hoc
  append_u32be(sig, hash_offset);
  append_u32be(sig, ident_offset);
  append_u32be(sig, 0);
  append_u32be(sig, nslots);
  append_u32be(sig, (uint32_t)code_len);
  append_u8be(sig, hash_size);
  append_u8be(sig, 2);                 // SHA-256
  append_u8be(sig, 0);
  append_u8be(sig, page_log);
  append_u32be(sig, 0);
  append_u32be(sig, 0);                // scatterOffset
  append_u32be(sig, 0);                // teamOffset
  append_u32be(sig, 0);                // spare3
  append_u64be(sig, code_len);
  append_u64be(sig, 0);                // execSegBase
  append_u64be(sig, code_len);         // execSegLimit
  append_u64be(sig, 0);                // execSegFlags
  append_bytes(sig, identifier, ident_len);
  while (sig->len < cd_offset + hash_offset) append_u8(sig, 0);
  for (uint32_t slot = 0; slot < nslots; slot++) {
    size_t offset = (size_t)slot * page_size;
    size_t len = code_len - offset;
    if (len > page_size) len = page_size;
    unsigned char hash[32];
    macho_sha256_hash(code + offset, len, hash);
    append_bytes(sig, (const char *)hash, sizeof(hash));
  }
}

static void append_bytes(ZBuf *buf, const char *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) append_u8(buf, (unsigned char)bytes[i]);
}

static void append_fixed(ZBuf *buf, const char *text, size_t width) {
  size_t len = text ? strlen(text) : 0;
  if (len > width) len = width;
  for (size_t i = 0; i < len; i++) append_u8(buf, (unsigned char)text[i]);
  for (size_t i = len; i < width; i++) append_u8(buf, 0);
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

static void macho_emit_aarch64_literal_return(ZBuf *text, uint32_t literal) {
  append_u32le(text, 0x52800000u | ((literal & 0xffffu) << 5)); // movz w0, #literal
  append_u32le(text, 0xd65f03c0u); // ret
}

typedef struct {
  size_t patch_offset;
  unsigned callee_index;
  int line;
  int column;
} MachOCallPatch;

typedef struct {
  size_t patch_offset;
  unsigned data_offset;
} MachODataPatch;

typedef struct {
  size_t patch_offset;
} MachOWorldWritePatch;

typedef struct {
  size_t patch_offset;
} MachORuntimeJsonParseBytesPatch;

typedef struct {
  size_t patch_offset;
} MachORuntimeHttpFetchPatch;

typedef struct {
  size_t patch_offset;
} MachORuntimeHttpResultPatch;

// The libm symbols std.math lowers to. The order is the symbol-table order; one undefined
// external is emitted per symbol that is actually called, each resolved by the host link step
// (zig cc) against libSystem. Underscore-prefixed Mach-O symbol names.
typedef enum {
  MACHO_MATH_SQRTF = 0,
  MACHO_MATH_EXPF,
  MACHO_MATH_COSF,
  MACHO_MATH_SINF,
  MACHO_MATH_POWF,
  MACHO_MATH_FABSF,
  MACHO_MATH_FLOORF,
  MACHO_MATH_SYMBOL_COUNT
} MachOMathSymbol;

static const char *const macho_math_symbol_names[MACHO_MATH_SYMBOL_COUNT] = {
  "_sqrtf", "_expf", "_cosf", "_sinf", "_powf", "_fabsf", "_floorf"
};

static MachOMathSymbol macho_math_symbol_for_value(IrValueKind kind) {
  switch (kind) {
    case IR_VALUE_MATH_SQRTF: return MACHO_MATH_SQRTF;
    case IR_VALUE_MATH_EXPF: return MACHO_MATH_EXPF;
    case IR_VALUE_MATH_COSF: return MACHO_MATH_COSF;
    case IR_VALUE_MATH_SINF: return MACHO_MATH_SINF;
    case IR_VALUE_MATH_POWF: return MACHO_MATH_POWF;
    case IR_VALUE_MATH_FABSF: return MACHO_MATH_FABSF;
    case IR_VALUE_MATH_FLOORF: return MACHO_MATH_FLOORF;
    default: return MACHO_MATH_SQRTF;
  }
}

// One recorded `bl <libm>` call site. `symbol` selects which libm external the BRANCH26
// relocation binds to.
typedef struct {
  size_t patch_offset;
  MachOMathSymbol symbol;
} MachOMathCallPatch;

// The libSystem (libc) symbols the OS-interface lowerings call. Like the libm set, the order is
// the symbol-table order and one undefined external is emitted per symbol actually called, resolved
// by the host link step (zig cc) against libSystem. Args/return are integer (x0..x7/x0), so these
// are strictly simpler than the libm FP calls. Anonymous std.mem.pageAlloc uses `_mmap`; the file
// mmap path (std.fs.mmap/mmapOrRaise) uses `_open`/`_lseek`/`_mmap`/`_close` and the owned<Mapping>
// drop (std.fs.munmap) uses `_munmap`.
typedef enum {
  MACHO_LIBC_MMAP = 0,
  MACHO_LIBC_OPEN,
  MACHO_LIBC_LSEEK,
  MACHO_LIBC_CLOSE,
  MACHO_LIBC_MUNMAP,
  MACHO_LIBC_SYMBOL_COUNT
} MachOLibcSymbol;

static const char *const macho_libc_symbol_names[MACHO_LIBC_SYMBOL_COUNT] = {
  "_mmap",
  "_open",
  "_lseek",
  "_close",
  "_munmap"
};

// One recorded `bl <libc>` call site. `symbol` selects which libSystem external the BRANCH26
// relocation binds to (same external-call shape as the libm sites).
typedef struct {
  size_t patch_offset;
  MachOLibcSymbol symbol;
} MachOLibcCallPatch;

typedef struct {
  const IrProgram *program;
  size_t *function_offsets;
  size_t function_count;
  MachOCallPatch *call_patches;
  size_t call_patch_len;
  size_t call_patch_cap;
  MachODataPatch *data_patches;
  size_t data_patch_len;
  size_t data_patch_cap;
  MachOWorldWritePatch *world_write_patches;
  size_t world_write_patch_len;
  size_t world_write_patch_cap;
  MachORuntimeJsonParseBytesPatch *runtime_json_parse_bytes_patches;
  size_t runtime_json_parse_bytes_patch_len;
  size_t runtime_json_parse_bytes_patch_cap;
  MachORuntimeHttpFetchPatch *runtime_http_fetch_patches;
  size_t runtime_http_fetch_patch_len;
  size_t runtime_http_fetch_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_result_ok_patches;
  size_t runtime_http_result_ok_patch_len;
  size_t runtime_http_result_ok_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_result_status_patches;
  size_t runtime_http_result_status_patch_len;
  size_t runtime_http_result_status_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_result_body_len_patches;
  size_t runtime_http_result_body_len_patch_len;
  size_t runtime_http_result_body_len_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_result_error_patches;
  size_t runtime_http_result_error_patch_len;
  size_t runtime_http_result_error_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_response_len_patches;
  size_t runtime_http_response_len_patch_len;
  size_t runtime_http_response_len_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_response_headers_len_patches;
  size_t runtime_http_response_headers_len_patch_len;
  size_t runtime_http_response_headers_len_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_response_body_offset_patches;
  size_t runtime_http_response_body_offset_patch_len;
  size_t runtime_http_response_body_offset_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_header_value_patches;
  size_t runtime_http_header_value_patch_len;
  size_t runtime_http_header_value_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_header_found_patches;
  size_t runtime_http_header_found_patch_len;
  size_t runtime_http_header_found_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_header_offset_patches;
  size_t runtime_http_header_offset_patch_len;
  size_t runtime_http_header_offset_patch_cap;
  MachORuntimeHttpResultPatch *runtime_http_header_len_patches;
  size_t runtime_http_header_len_patch_len;
  size_t runtime_http_header_len_patch_cap;
  MachOMathCallPatch *math_call_patches;
  size_t math_call_patch_len;
  size_t math_call_patch_cap;
  MachOLibcCallPatch *libc_call_patches;
  size_t libc_call_patch_len;
  size_t libc_call_patch_cap;
  unsigned rodata_base_offset;
  bool pie_relative_data;
  bool seed_main_process_args;
} MachOEmitContext;

static size_t macho_align(size_t value, size_t alignment) {
  size_t remainder = alignment ? value % alignment : 0;
  return remainder == 0 ? value : value + (alignment - remainder);
}

static void macho_pad_to(ZBuf *buf, size_t offset) {
  while (buf->len < offset) append_u8(buf, 0);
}

static void macho_append_uleb128(ZBuf *buf, uint64_t value) {
  do {
    unsigned byte = (unsigned)(value & 0x7fu);
    value >>= 7;
    if (value) byte |= 0x80u;
    append_u8(buf, byte);
  } while (value);
}

static bool macho_type_is_scalar32(IrTypeKind type) {
  return type == IR_TYPE_BOOL || type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_I32 || type == IR_TYPE_U32 || type == IR_TYPE_USIZE;
}

static bool macho_type_is_scalar64(IrTypeKind type) {
  return type == IR_TYPE_I64 || type == IR_TYPE_U64;
}

static bool macho_type_is_scalar(IrTypeKind type) {
  return macho_type_is_scalar32(type) || macho_type_is_scalar64(type);
}

static unsigned macho_slot_offset(unsigned local_index) {
  return local_index * 8;
}

static void macho_emit_add_sp_imm(ZBuf *text, uint32_t base, unsigned imm) {
  append_u32le(text, base | ((imm & 0xfffu) << 10));
}

static void macho_emit_add_x_sp_imm(ZBuf *text, unsigned dst, unsigned imm) {
  append_u32le(text, 0x910003e0u | ((imm & 0xfffu) << 10) | (dst & 31u));
}

static void macho_emit_nop(ZBuf *text) {
  append_u32le(text, 0xd503201fu);
}

static void macho_emit_movz_w(ZBuf *text, unsigned reg, uint32_t literal) {
  append_u32le(text, 0x52800000u | ((literal & 0xffffu) << 5) | (reg & 31u));
  if (literal > 0xffffu) {
    append_u32le(text, 0x72a00000u | (((literal >> 16) & 0xffffu) << 5) | (reg & 31u));
  }
}

static void macho_emit_movz_x(ZBuf *text, unsigned reg, uint32_t literal) {
  append_u32le(text, 0xd2800000u | ((literal & 0xffffu) << 5) | (reg & 31u));
  if (literal > 0xffffu) {
    append_u32le(text, 0xf2a00000u | (((literal >> 16) & 0xffffu) << 5) | (reg & 31u));
  }
}

// MOVZ/MOVK sequence for an arbitrary 64-bit immediate (used for f64 bit patterns). Emits MOVZ
// for the lowest non-zero chunk and MOVK for each remaining non-zero 16-bit chunk; falls back to
// MOVZ #0 when the value is zero.
static void macho_emit_mov_imm64(ZBuf *text, unsigned reg, uint64_t value) {
  bool emitted = false;
  for (unsigned shift = 0; shift < 4; shift++) {
    uint32_t chunk = (uint32_t)((value >> (shift * 16)) & 0xffffu);
    if (chunk == 0) continue;
    if (!emitted) {
      append_u32le(text, 0xd2800000u | ((uint32_t)shift << 21) | (chunk << 5) | (reg & 31u)); // movz
      emitted = true;
    } else {
      append_u32le(text, 0xf2800000u | ((uint32_t)shift << 21) | (chunk << 5) | (reg & 31u)); // movk
    }
  }
  if (!emitted) append_u32le(text, 0xd2800000u | (reg & 31u)); // movz reg, #0
}

// MOVN Xd, #imm16 — load the bitwise-NOT of a 16-bit immediate. `macho_emit_movn_x(reg, 0)`
// materializes -1 (all ones) in one instruction, used for the mmap fd argument.
static void macho_emit_movn_x(ZBuf *text, unsigned reg, uint32_t imm16) {
  append_u32le(text, 0x92800000u | ((imm16 & 0xffffu) << 5) | (reg & 31u));
}

static void macho_emit_mov_w(ZBuf *text, unsigned dst, unsigned src) {
  append_u32le(text, 0x2a0003e0u | ((src & 31u) << 16) | (dst & 31u));
}

static void macho_emit_mov_x(ZBuf *text, unsigned dst, unsigned src) {
  append_u32le(text, 0xaa0003e0u | ((src & 31u) << 16) | (dst & 31u));
}

static void macho_emit_add_x_imm(ZBuf *text, unsigned dst, unsigned src, unsigned imm) {
  append_u32le(text, 0x91000000u | ((imm & 0xfffu) << 10) | ((src & 31u) << 5) | (dst & 31u));
}

static void macho_emit_add_w_imm(ZBuf *text, unsigned dst, unsigned src, unsigned imm) {
  append_u32le(text, 0x11000000u | ((imm & 0xfffu) << 10) | ((src & 31u) << 5) | (dst & 31u));
}

static void macho_emit_sub_w_imm(ZBuf *text, unsigned dst, unsigned src, unsigned imm) {
  append_u32le(text, 0x51000000u | ((imm & 0xfffu) << 10) | ((src & 31u) << 5) | (dst & 31u));
}

static unsigned macho_local_slot_offset(const IrFunction *fun, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  if (fun && local_index < fun->local_len && fun->locals[local_index].frame_offset > 0 && frame_size >= fun->locals[local_index].frame_offset) {
    return frame_size - fun->locals[local_index].frame_offset + slot_offset;
  }
  return macho_slot_offset(local_index) + slot_offset;
}

static void macho_emit_load_local_w(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  append_u32le(text, 0xb9400000u | ((offset / 4u) << 10) | (31u << 5) | (reg & 31u));
}

static void macho_emit_load_local_x(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  append_u32le(text, 0xf9400000u | ((offset / 8u) << 10) | (31u << 5) | (reg & 31u));
}

static void macho_emit_store_local_w(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  append_u32le(text, 0xb9000000u | ((offset / 4u) << 10) | (31u << 5) | (reg & 31u));
}

static void macho_emit_store_local_x(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  append_u32le(text, 0xf9000000u | ((offset / 8u) << 10) | (31u << 5) | (reg & 31u));
}

static void macho_emit_load_local_b(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  append_u32le(text, 0x39400000u | ((offset & 0xfffu) << 10) | (31u << 5) | (reg & 31u));
}

static void macho_emit_store_local_b(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned slot_offset, unsigned frame_size) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  append_u32le(text, 0x39000000u | ((offset & 0xfffu) << 10) | (31u << 5) | (reg & 31u));
}

static void macho_emit_load_field(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned field_offset, IrTypeKind type, unsigned frame_size) {
  if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
    macho_emit_load_local_b(text, fun, reg, local_index, field_offset, frame_size);
  } else {
    macho_emit_load_local_w(text, fun, reg, local_index, field_offset, frame_size);
  }
}

static void macho_emit_store_field(ZBuf *text, const IrFunction *fun, unsigned reg, unsigned local_index, unsigned field_offset, IrTypeKind type, unsigned frame_size) {
  if (type == IR_TYPE_U8 || type == IR_TYPE_BOOL) {
    macho_emit_store_local_b(text, fun, reg, local_index, field_offset, frame_size);
  } else {
    macho_emit_store_local_w(text, fun, reg, local_index, field_offset, frame_size);
  }
}

// SDIV/UDIV (data-processing 2-source): bit 31 selects width (0=Wd 32-bit, 1=Xd 64-bit), bit 10
// selects signed(1)/unsigned(0). AArch64 divide-by-zero yields 0 (no trap) and INT_MIN/-1 yields
// INT_MIN — the architectural results; the language treats these as UB, so matching the hardware
// is consistent with the ELF path's idiv/div (which faults on /0, also UB).
static void macho_emit_div(ZBuf *text, unsigned dst, unsigned lhs, unsigned rhs, bool wide, bool is_unsigned) {
  uint32_t op = is_unsigned ? 0x1ac00800u : 0x1ac00c00u;
  if (wide) op |= (1u << 31);
  append_u32le(text, op | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
}

// MSUB Xd, Xn, Xm, Xa = Xa - Xn*Xm (bit 31 selects width). Used for modulo: rem = a - (a/b)*b,
// so MSUB dst, quotient, divisor, dividend after the matching SDIV/UDIV.
static void macho_emit_msub(ZBuf *text, unsigned dst, unsigned n, unsigned m, unsigned a, bool wide) {
  uint32_t op = 0x1b008000u;
  if (wide) op |= (1u << 31);
  append_u32le(text, op | ((m & 31u) << 16) | ((a & 31u) << 10) | ((n & 31u) << 5) | (dst & 31u));
}

// Integer ADD/SUB/MUL/DIV/MOD into a GPR. `wide` selects the 64-bit (Xd) forms for i64/u64
// results; the 32-bit (Wd) forms otherwise. DIV/MOD additionally honour `is_unsigned` (UDIV vs
// SDIV), mirroring the ELF backend's signed/unsigned division selection off the result type.
// MOD computes the remainder with a quotient in x10 (free here — operands are x8/x9, result in
// `dst`) followed by MSUB.
static void macho_emit_binary_int(ZBuf *text, IrBinaryOp op, unsigned dst, unsigned lhs, unsigned rhs, bool wide, bool is_unsigned) {
  if (op == IR_BIN_ADD) {
    uint32_t base = wide ? 0x8b000000u : 0x0b000000u;
    append_u32le(text, base | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
  } else if (op == IR_BIN_SUB) {
    uint32_t base = wide ? 0xcb000000u : 0x4b000000u;
    append_u32le(text, base | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
  } else if (op == IR_BIN_MUL) {
    uint32_t base = wide ? 0x9b000000u : 0x1b000000u;
    append_u32le(text, base | ((rhs & 31u) << 16) | (31u << 10) | ((lhs & 31u) << 5) | (dst & 31u));
  } else if (op == IR_BIN_DIV) {
    macho_emit_div(text, dst, lhs, rhs, wide, is_unsigned);
  } else if (op == IR_BIN_MOD) {
    macho_emit_div(text, 10, lhs, rhs, wide, is_unsigned);
    macho_emit_msub(text, dst, 10, rhs, lhs, wide);
  }
}

static void macho_emit_cmp_w(ZBuf *text, unsigned lhs, unsigned rhs) {
  append_u32le(text, 0x6b00001fu | ((rhs & 31u) << 16) | ((lhs & 31u) << 5));
}

static void macho_emit_cmp_x(ZBuf *text, unsigned lhs, unsigned rhs) {
  append_u32le(text, 0xeb00001fu | ((rhs & 31u) << 16) | ((lhs & 31u) << 5));
}

static void macho_emit_ldrb_w(ZBuf *text, unsigned dst, unsigned base) {
  append_u32le(text, 0x39400000u | ((base & 31u) << 5) | (dst & 31u));
}

// LDRSB Wt, [Xn] — load one byte and sign-extend it to 32 bits (the signed-byte counterpart of
// LDRB's zero extension), used when reading i8 span elements.
static void macho_emit_ldrsb_w(ZBuf *text, unsigned dst, unsigned base) {
  append_u32le(text, 0x39c00000u | ((base & 31u) << 5) | (dst & 31u));
}

static void macho_emit_ldr_w(ZBuf *text, unsigned dst, unsigned base) {
  append_u32le(text, 0xb9400000u | ((base & 31u) << 5) | (dst & 31u));
}

static void macho_emit_str_w(ZBuf *text, unsigned src, unsigned base) {
  append_u32le(text, 0xb9000000u | ((base & 31u) << 5) | (src & 31u));
}

static void macho_emit_ldr_x_imm(ZBuf *text, unsigned dst, unsigned base, unsigned byte_offset) {
  append_u32le(text, 0xf9400000u | (((byte_offset / 8u) & 0xfffu) << 10) | ((base & 31u) << 5) | (dst & 31u));
}

static void macho_emit_str_x(ZBuf *text, unsigned src, unsigned base) {
  append_u32le(text, 0xf9000000u | ((base & 31u) << 5) | (src & 31u));
}

static void macho_emit_strb_w(ZBuf *text, unsigned src, unsigned base) {
  append_u32le(text, 0x39000000u | ((base & 31u) << 5) | (src & 31u));
}

// LSR Wd, Wn, #shift (UBFM alias) — logical right shift by a constant, used to turn a byte length
// into an element count after a typed reinterpret (count = byteLen >> log2(elemSize)).
static void macho_emit_lsr_w_imm(ZBuf *text, unsigned dst, unsigned src, unsigned shift) {
  append_u32le(text, 0x53007c00u | ((shift & 31u) << 16) | ((src & 31u) << 5) | (dst & 31u));
}

static void macho_emit_add_x_reg(ZBuf *text, unsigned dst, unsigned lhs, unsigned rhs) {
  append_u32le(text, 0x8b000000u | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
}

static void macho_emit_add_x_reg_lsl(ZBuf *text, unsigned dst, unsigned lhs, unsigned rhs, unsigned shift) {
  append_u32le(text, 0x8b000000u | ((rhs & 31u) << 16) | ((shift & 0x3fu) << 10) | ((lhs & 31u) << 5) | (dst & 31u));
}

// AArch64 scalar floating-point (AAPCS FP ABI: args/return in v0..v7, s_ for f32, d_ for f64).
// IEEE 754 strict; no flush-to-zero. The v-register file is independent of x0..x30, so FP
// temporaries (v8/v9) never clobber the integer scratch registers (x8/x9) used alongside them.
static bool macho_type_is_f64(IrTypeKind type) {
  return type == IR_TYPE_F64;
}

static bool macho_type_is_float(IrTypeKind type) {
  return type == IR_TYPE_F32 || type == IR_TYPE_F64;
}

static bool macho_type_is_unsigned(IrTypeKind type) {
  return type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_U32 || type == IR_TYPE_USIZE || type == IR_TYPE_U64;
}

// LDR/STR (scalar FP, unsigned offset): bit 30 selects width (0=s 32-bit, 1=d 64-bit), bit 22
// selects load(1)/store(0). The byte offset is scaled by the access size, like the GPR forms.
static void macho_emit_ldr_v_local(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned slot_offset, unsigned frame_size, bool is64) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  uint32_t base = is64 ? 0xfd400000u : 0xbd400000u;
  unsigned scaled = is64 ? (offset / 8u) : (offset / 4u);
  append_u32le(text, base | ((scaled & 0xfffu) << 10) | (31u << 5) | (vreg & 31u));
}

static void macho_emit_str_v_local(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned slot_offset, unsigned frame_size, bool is64) {
  unsigned offset = macho_local_slot_offset(fun, local_index, slot_offset, frame_size);
  uint32_t base = is64 ? 0xfd000000u : 0xbd000000u;
  unsigned scaled = is64 ? (offset / 8u) : (offset / 4u);
  append_u32le(text, base | ((scaled & 0xfffu) << 10) | (31u << 5) | (vreg & 31u));
}

// Record-field FP load/store: the field byte offset is the slot offset within the record local.
static void macho_emit_ldr_v_field(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned field_offset, unsigned frame_size, bool is64) {
  macho_emit_ldr_v_local(text, fun, vreg, local_index, field_offset, frame_size, is64);
}

static void macho_emit_str_v_field(ZBuf *text, const IrFunction *fun, unsigned vreg, unsigned local_index, unsigned field_offset, unsigned frame_size, bool is64) {
  macho_emit_str_v_local(text, fun, vreg, local_index, field_offset, frame_size, is64);
}

// LDR/STR (scalar FP, base only, #0 offset): ldr s_/d_, [xbase]. Used when the element address is
// already computed in a base register (span index load/store, codec readF*Le).
static void macho_emit_ldr_v_base(ZBuf *text, unsigned vreg, unsigned base, bool is64) {
  uint32_t op = is64 ? 0xfd400000u : 0xbd400000u;
  append_u32le(text, op | ((base & 31u) << 5) | (vreg & 31u));
}

static void macho_emit_str_v_base(ZBuf *text, unsigned vreg, unsigned base, bool is64) {
  uint32_t op = is64 ? 0xfd000000u : 0xbd000000u;
  append_u32le(text, op | ((base & 31u) << 5) | (vreg & 31u));
}

// LDR/STR (scalar FP, register offset, lsl by access-size log2): ldr s_, [xbase, xindex, lsl #2].
static void macho_emit_ldr_v_reg_lsl(ZBuf *text, unsigned vreg, unsigned base, unsigned index, bool is64) {
  uint32_t opc = is64 ? 0xfc607800u : 0xbc607800u;
  append_u32le(text, opc | ((index & 31u) << 16) | ((base & 31u) << 5) | (vreg & 31u));
}

static void macho_emit_str_v_reg_lsl(ZBuf *text, unsigned vreg, unsigned base, unsigned index, bool is64) {
  uint32_t opc = is64 ? 0xfc207800u : 0xbc207800u;
  append_u32le(text, opc | ((index & 31u) << 16) | ((base & 31u) << 5) | (vreg & 31u));
}

// STR (GPR/FP, immediate offset off an arbitrary base register) — write a record field through a
// pointer held in a base register, e.g. the caller's sret pointer. The byte offset is scaled by
// the access size, as the load/store unsigned-offset encodings require (fields are naturally
// aligned to their width). Used for sret field stores and record-copy through a pointer.
static void macho_emit_str_b_disp(ZBuf *text, unsigned src, unsigned base, unsigned byte_offset) {
  append_u32le(text, 0x39000000u | ((byte_offset & 0xfffu) << 10) | ((base & 31u) << 5) | (src & 31u));
}

static void macho_emit_str_w_disp(ZBuf *text, unsigned src, unsigned base, unsigned byte_offset) {
  append_u32le(text, 0xb9000000u | (((byte_offset / 4u) & 0xfffu) << 10) | ((base & 31u) << 5) | (src & 31u));
}

static void macho_emit_str_x_disp(ZBuf *text, unsigned src, unsigned base, unsigned byte_offset) {
  append_u32le(text, 0xf9000000u | (((byte_offset / 8u) & 0xfffu) << 10) | ((base & 31u) << 5) | (src & 31u));
}

static void macho_emit_ldr_w_disp(ZBuf *text, unsigned dst, unsigned base, unsigned byte_offset) {
  append_u32le(text, 0xb9400000u | (((byte_offset / 4u) & 0xfffu) << 10) | ((base & 31u) << 5) | (dst & 31u));
}

static void macho_emit_str_v_disp(ZBuf *text, unsigned vreg, unsigned base, unsigned byte_offset, bool is64) {
  uint32_t op = is64 ? 0xfd000000u : 0xbd000000u;
  unsigned scaled = is64 ? (byte_offset / 8u) : (byte_offset / 4u);
  append_u32le(text, op | ((scaled & 0xfffu) << 10) | ((base & 31u) << 5) | (vreg & 31u));
}

// FMOV v_, v_ (register move within the FP file): bit 22 selects s/d.
static void macho_emit_fmov_v(ZBuf *text, unsigned dst, unsigned src, bool is64) {
  uint32_t base = is64 ? 0x1e604000u : 0x1e204000u;
  append_u32le(text, base | ((src & 31u) << 5) | (dst & 31u));
}

// FMOV v_, w_/x_ : copy the raw bits of an integer register into an FP register (no conversion).
static void macho_emit_fmov_v_from_gpr(ZBuf *text, unsigned vreg, unsigned gpr, bool is64) {
  uint32_t base = is64 ? 0x9e670000u : 0x1e270000u;
  append_u32le(text, base | ((gpr & 31u) << 5) | (vreg & 31u));
}

// FADD/FSUB/FMUL/FDIV (scalar): bit 22 selects s/d; opcode field selects the operation.
static bool macho_emit_float_arith(ZBuf *text, IrBinaryOp op, unsigned dst, unsigned lhs, unsigned rhs, bool is64) {
  uint32_t opcode;
  switch (op) {
    case IR_BIN_ADD: opcode = 0x1e202800u; break;
    case IR_BIN_SUB: opcode = 0x1e203800u; break;
    case IR_BIN_MUL: opcode = 0x1e200800u; break;
    case IR_BIN_DIV: opcode = 0x1e201800u; break;
    default: return false;
  }
  if (is64) opcode |= (1u << 22);
  append_u32le(text, opcode | ((rhs & 31u) << 16) | ((lhs & 31u) << 5) | (dst & 31u));
  return true;
}

// FCMP v_, v_ : sets NZCV. IEEE unordered (NaN) yields N=0 Z=0 C=1 V=1, so the carry/overflow
// flags drive the IEEE-correct condition codes used below.
static void macho_emit_fcmp(ZBuf *text, unsigned lhs, unsigned rhs, bool is64) {
  uint32_t base = is64 ? 0x1e602000u : 0x1e202000u;
  append_u32le(text, base | ((rhs & 31u) << 16) | ((lhs & 31u) << 5));
}

// Spill/reload an FP register to a fixed [sp, #off] slot. sp is constant for the body, so the
// slot is stable across nested evaluation (including libm calls) without disturbing local
// addressing. The byte offset is scaled by the access size, like the other FP load/stores.
static void macho_emit_str_v_sp(ZBuf *text, unsigned vreg, unsigned byte_offset, bool is64) {
  uint32_t base = is64 ? 0xfd000000u : 0xbd000000u;
  unsigned scaled = is64 ? (byte_offset / 8u) : (byte_offset / 4u);
  append_u32le(text, base | ((scaled & 0xfffu) << 10) | (31u << 5) | (vreg & 31u));
}

static void macho_emit_ldr_v_sp(ZBuf *text, unsigned vreg, unsigned byte_offset, bool is64) {
  uint32_t base = is64 ? 0xfd400000u : 0xbd400000u;
  unsigned scaled = is64 ? (byte_offset / 8u) : (byte_offset / 4u);
  append_u32le(text, base | ((scaled & 0xfffu) << 10) | (31u << 5) | (vreg & 31u));
}

// FCVT s<->d : narrow/widen between f32 and f64.
static void macho_emit_fcvt(ZBuf *text, unsigned dst, unsigned src, bool dst_is64) {
  uint32_t base = dst_is64 ? 0x1e22c000u : 0x1e624000u; // fcvt d,s : fcvt s,d
  append_u32le(text, base | ((src & 31u) << 5) | (dst & 31u));
}

// FCVTZS w_, v_ : float -> i32, round toward zero (matches the `as i32` truncation contract).
static void macho_emit_fcvtzs_w(ZBuf *text, unsigned gpr, unsigned vreg, bool src_is64) {
  uint32_t base = src_is64 ? 0x1e780000u : 0x1e380000u;
  append_u32le(text, base | ((vreg & 31u) << 5) | (gpr & 31u));
}

// SCVTF v_, w_ : signed i32 -> float. UCVTF v_, x_ : unsigned u64 -> float.
static void macho_emit_scvtf_from_w(ZBuf *text, unsigned vreg, unsigned gpr, bool dst_is64) {
  uint32_t base = dst_is64 ? 0x1e620000u : 0x1e220000u;
  append_u32le(text, base | ((gpr & 31u) << 5) | (vreg & 31u));
}

static void macho_emit_ucvtf_from_x(ZBuf *text, unsigned vreg, unsigned gpr, bool dst_is64) {
  uint32_t base = dst_is64 ? 0x9e630000u : 0x9e230000u;
  append_u32le(text, base | ((gpr & 31u) << 5) | (vreg & 31u));
}

// CSET w_, <cond> : materialize a boolean from NZCV. The CSINC encoding carries the inverted
// condition (the assembler alias does the same: `cset w, EQ` encodes `csinc w, wzr, wzr, NE`).
static void macho_emit_cset(ZBuf *text, unsigned gpr, unsigned cond) {
  append_u32le(text, 0x1a9f07e0u | (((cond ^ 1u) & 15u) << 12) | (gpr & 31u));
}

static size_t macho_emit_bl_placeholder(ZBuf *text) {
  size_t patch = text->len;
  append_u32le(text, 0x94000000u);
  return patch;
}

static size_t macho_emit_b_placeholder(ZBuf *text) {
  size_t patch = text->len;
  append_u32le(text, 0x14000000u);
  return patch;
}

static size_t macho_emit_b_cond_placeholder(ZBuf *text, unsigned cond) {
  size_t patch = text->len;
  append_u32le(text, 0x54000000u | (cond & 15u));
  return patch;
}

static size_t macho_emit_cbz_w_placeholder(ZBuf *text, unsigned reg) {
  size_t patch = text->len;
  append_u32le(text, 0x34000000u | (reg & 31u));
  return patch;
}

static void macho_patch_branch26(ZBuf *text, size_t patch_offset, size_t target_offset) {
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

static void macho_patch_cond19(ZBuf *text, size_t patch_offset, size_t target_offset) {
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

static void macho_patch_adrp_add(ZBuf *text, size_t patch_offset, uint64_t instr_addr, uint64_t target_addr) {
  uint32_t adrp = ((unsigned char)text->data[patch_offset]) |
                  ((uint32_t)(unsigned char)text->data[patch_offset + 1] << 8) |
                  ((uint32_t)(unsigned char)text->data[patch_offset + 2] << 16) |
                  ((uint32_t)(unsigned char)text->data[patch_offset + 3] << 24);
  unsigned reg = adrp & 31u;
  int64_t instr_page = (int64_t)(instr_addr & ~0xfffull);
  int64_t target_page = (int64_t)(target_addr & ~0xfffull);
  int64_t pages = (target_page - instr_page) / 4096;
  uint32_t immlo = (uint32_t)pages & 0x3u;
  uint32_t immhi = ((uint32_t)pages >> 2) & 0x7ffffu;
  uint32_t patched_adrp = 0x90000000u | (immlo << 29) | (immhi << 5) | reg;
  uint32_t pageoff = (uint32_t)(target_addr & 0xfffu);
  uint32_t patched_add = 0x91000000u | ((pageoff & 0xfffu) << 10) | (reg << 5) | reg;
  patch_u64le(text, patch_offset, ((uint64_t)patched_add << 32) | patched_adrp);
}

static bool macho_record_call_patch(MachOEmitContext *ctx, size_t patch_offset, unsigned callee_index, const IrValue *value, ZDiag *diag) {
  if (!ctx || callee_index >= ctx->function_count) {
    return macho_diag_at(diag, "direct AArch64 Mach-O call target is out of range", value ? value->line : 1, value ? value->column : 1, "invalid callee");
  }
  if (ctx->call_patch_len == ctx->call_patch_cap) {
    ctx->call_patch_cap = z_grow_capacity(ctx->call_patch_cap, ctx->call_patch_len + 1, 8);
    ctx->call_patches = z_checked_reallocarray(ctx->call_patches, ctx->call_patch_cap, sizeof(MachOCallPatch));
  }
  ctx->call_patches[ctx->call_patch_len++] = (MachOCallPatch){.patch_offset = patch_offset, .callee_index = callee_index, .line = value ? value->line : 1, .column = value ? value->column : 1};
  return true;
}

static bool macho_record_data_patch(MachOEmitContext *ctx, size_t patch_offset, unsigned data_offset, const IrValue *value, ZDiag *diag) {
  if (!ctx) return macho_diag_at(diag, "direct AArch64 Mach-O data relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->data_patch_len == ctx->data_patch_cap) {
    ctx->data_patch_cap = z_grow_capacity(ctx->data_patch_cap, ctx->data_patch_len + 1, 8);
    ctx->data_patches = z_checked_reallocarray(ctx->data_patches, ctx->data_patch_cap, sizeof(MachODataPatch));
  }
  ctx->data_patches[ctx->data_patch_len++] = (MachODataPatch){.patch_offset = patch_offset, .data_offset = data_offset};
  return true;
}

static bool macho_record_world_write_patch(MachOEmitContext *ctx, size_t patch_offset, const IrInstr *instr, ZDiag *diag) {
  if (!ctx) return macho_diag_at(diag, "direct AArch64 Mach-O World write relocation requires an emit context", instr ? instr->line : 1, instr ? instr->column : 1, "missing context");
  if (ctx->world_write_patch_len == ctx->world_write_patch_cap) {
    ctx->world_write_patch_cap = z_grow_capacity(ctx->world_write_patch_cap, ctx->world_write_patch_len + 1, 4);
    ctx->world_write_patches = z_checked_reallocarray(ctx->world_write_patches, ctx->world_write_patch_cap, sizeof(MachOWorldWritePatch));
  }
  ctx->world_write_patches[ctx->world_write_patch_len++] = (MachOWorldWritePatch){.patch_offset = patch_offset};
  return true;
}

static bool macho_record_runtime_json_parse_bytes_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  if (!ctx) return macho_diag_at(diag, "direct AArch64 Mach-O JSON runtime relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->runtime_json_parse_bytes_patch_len == ctx->runtime_json_parse_bytes_patch_cap) {
    ctx->runtime_json_parse_bytes_patch_cap = z_grow_capacity(ctx->runtime_json_parse_bytes_patch_cap, ctx->runtime_json_parse_bytes_patch_len + 1, 4);
    ctx->runtime_json_parse_bytes_patches = z_checked_reallocarray(ctx->runtime_json_parse_bytes_patches, ctx->runtime_json_parse_bytes_patch_cap, sizeof(MachORuntimeJsonParseBytesPatch));
  }
  ctx->runtime_json_parse_bytes_patches[ctx->runtime_json_parse_bytes_patch_len++] = (MachORuntimeJsonParseBytesPatch){.patch_offset = patch_offset};
  return true;
}

static bool macho_record_runtime_http_fetch_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  if (!ctx) return macho_diag_at(diag, "direct AArch64 Mach-O HTTP runtime relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->runtime_http_fetch_patch_len == ctx->runtime_http_fetch_patch_cap) {
    ctx->runtime_http_fetch_patch_cap = z_grow_capacity(ctx->runtime_http_fetch_patch_cap, ctx->runtime_http_fetch_patch_len + 1, 4);
    ctx->runtime_http_fetch_patches = z_checked_reallocarray(ctx->runtime_http_fetch_patches, ctx->runtime_http_fetch_patch_cap, sizeof(MachORuntimeHttpFetchPatch));
  }
  ctx->runtime_http_fetch_patches[ctx->runtime_http_fetch_patch_len++] = (MachORuntimeHttpFetchPatch){.patch_offset = patch_offset};
  return true;
}

static bool macho_record_runtime_http_result_patch(MachORuntimeHttpResultPatch **items, size_t *len, size_t *cap, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  if (!items || !len || !cap) return macho_diag_at(diag, "direct AArch64 Mach-O HTTP result relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (*len == *cap) {
    *cap = z_grow_capacity(*cap, *len + 1, 4);
    *items = z_checked_reallocarray(*items, *cap, sizeof(MachORuntimeHttpResultPatch));
  }
  (*items)[(*len)++] = (MachORuntimeHttpResultPatch){.patch_offset = patch_offset};
  return true;
}

static bool macho_record_runtime_http_result_ok_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  return ctx && macho_record_runtime_http_result_patch(&ctx->runtime_http_result_ok_patches, &ctx->runtime_http_result_ok_patch_len, &ctx->runtime_http_result_ok_patch_cap, patch_offset, value, diag);
}

static bool macho_record_runtime_http_result_status_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  return ctx && macho_record_runtime_http_result_patch(&ctx->runtime_http_result_status_patches, &ctx->runtime_http_result_status_patch_len, &ctx->runtime_http_result_status_patch_cap, patch_offset, value, diag);
}

static bool macho_record_runtime_http_result_body_len_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  return ctx && macho_record_runtime_http_result_patch(&ctx->runtime_http_result_body_len_patches, &ctx->runtime_http_result_body_len_patch_len, &ctx->runtime_http_result_body_len_patch_cap, patch_offset, value, diag);
}

static bool macho_record_runtime_http_result_error_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  return ctx && macho_record_runtime_http_result_patch(&ctx->runtime_http_result_error_patches, &ctx->runtime_http_result_error_patch_len, &ctx->runtime_http_result_error_patch_cap, patch_offset, value, diag);
}

static bool macho_record_runtime_http_header_value_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  return ctx && macho_record_runtime_http_result_patch(&ctx->runtime_http_header_value_patches, &ctx->runtime_http_header_value_patch_len, &ctx->runtime_http_header_value_patch_cap, patch_offset, value, diag);
}

static bool macho_record_runtime_http_header_found_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  return ctx && macho_record_runtime_http_result_patch(&ctx->runtime_http_header_found_patches, &ctx->runtime_http_header_found_patch_len, &ctx->runtime_http_header_found_patch_cap, patch_offset, value, diag);
}

static bool macho_record_runtime_http_header_offset_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  return ctx && macho_record_runtime_http_result_patch(&ctx->runtime_http_header_offset_patches, &ctx->runtime_http_header_offset_patch_len, &ctx->runtime_http_header_offset_patch_cap, patch_offset, value, diag);
}

static bool macho_record_runtime_http_header_len_patch(MachOEmitContext *ctx, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  return ctx && macho_record_runtime_http_result_patch(&ctx->runtime_http_header_len_patches, &ctx->runtime_http_header_len_patch_len, &ctx->runtime_http_header_len_patch_cap, patch_offset, value, diag);
}

static bool macho_record_math_call_patch(MachOEmitContext *ctx, size_t patch_offset, MachOMathSymbol symbol, const IrValue *value, ZDiag *diag) {
  if (!ctx) return macho_diag_at(diag, "direct AArch64 Mach-O math relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->math_call_patch_len == ctx->math_call_patch_cap) {
    ctx->math_call_patch_cap = z_grow_capacity(ctx->math_call_patch_cap, ctx->math_call_patch_len + 1, 4);
    ctx->math_call_patches = z_checked_reallocarray(ctx->math_call_patches, ctx->math_call_patch_cap, sizeof(MachOMathCallPatch));
  }
  ctx->math_call_patches[ctx->math_call_patch_len++] = (MachOMathCallPatch){.patch_offset = patch_offset, .symbol = symbol};
  return true;
}

// True when at least one call site targets the given libm symbol. Drives which undefined
// externals (and their relocations) are emitted into the object's symbol table.
static bool macho_math_symbol_used(const MachOEmitContext *ctx, MachOMathSymbol symbol) {
  for (size_t i = 0; ctx && i < ctx->math_call_patch_len; i++) {
    if (ctx->math_call_patches[i].symbol == symbol) return true;
  }
  return false;
}

static void macho_append_call_relocations(ZBuf *relocs, const MachOEmitContext *ctx) {
  for (size_t i = 0; ctx && i < ctx->call_patch_len; i++) {
    const MachOCallPatch *patch = &ctx->call_patches[i];
    uint32_t reloc_info = (patch->callee_index & 0x00ffffffu) |
                          (1u << 24) |  // r_pcrel
                          (2u << 25) |  // r_length: 4 bytes
                          (1u << 27) |  // r_extern: symbol table index
                          (2u << 28);   // ARM64_RELOC_BRANCH26
    append_u32le(relocs, (uint32_t)patch->patch_offset);
    append_u32le(relocs, reloc_info);
  }
}

static void macho_append_world_write_relocations(ZBuf *relocs, const MachOEmitContext *ctx, unsigned symbol_index) {
  for (size_t i = 0; ctx && i < ctx->world_write_patch_len; i++) {
    const MachOWorldWritePatch *patch = &ctx->world_write_patches[i];
    uint32_t reloc_info = (symbol_index & 0x00ffffffu) |
                          (1u << 24) |  // r_pcrel
                          (2u << 25) |  // r_length: 4 bytes
                          (1u << 27) |  // r_extern: symbol table index
                          (2u << 28);   // ARM64_RELOC_BRANCH26
    append_u32le(relocs, (uint32_t)patch->patch_offset);
    append_u32le(relocs, reloc_info);
  }
}

static void macho_append_runtime_json_parse_bytes_relocations(ZBuf *relocs, const MachOEmitContext *ctx, unsigned symbol_index) {
  for (size_t i = 0; ctx && i < ctx->runtime_json_parse_bytes_patch_len; i++) {
    const MachORuntimeJsonParseBytesPatch *patch = &ctx->runtime_json_parse_bytes_patches[i];
    uint32_t reloc_info = (symbol_index & 0x00ffffffu) |
                          (1u << 24) |  // r_pcrel
                          (2u << 25) |  // r_length: 4 bytes
                          (1u << 27) |  // r_extern: symbol table index
                          (2u << 28);   // ARM64_RELOC_BRANCH26
    append_u32le(relocs, (uint32_t)patch->patch_offset);
    append_u32le(relocs, reloc_info);
  }
}

static void macho_append_runtime_http_fetch_relocations(ZBuf *relocs, const MachOEmitContext *ctx, unsigned symbol_index) {
  for (size_t i = 0; ctx && i < ctx->runtime_http_fetch_patch_len; i++) {
    const MachORuntimeHttpFetchPatch *patch = &ctx->runtime_http_fetch_patches[i];
    uint32_t reloc_info = (symbol_index & 0x00ffffffu) |
                          (1u << 24) |
                          (2u << 25) |
                          (1u << 27) |
                          (2u << 28);
    append_u32le(relocs, (uint32_t)patch->patch_offset);
    append_u32le(relocs, reloc_info);
  }
}

static void macho_append_runtime_http_result_relocations(ZBuf *relocs, const MachORuntimeHttpResultPatch *patches, size_t patch_len, unsigned symbol_index) {
  for (size_t i = 0; i < patch_len; i++) {
    const MachORuntimeHttpResultPatch *patch = &patches[i];
    uint32_t reloc_info = (symbol_index & 0x00ffffffu) |
                          (1u << 24) |
                          (2u << 25) |
                          (1u << 27) |
                          (2u << 28);
    append_u32le(relocs, (uint32_t)patch->patch_offset);
    append_u32le(relocs, reloc_info);
  }
}

// One BRANCH26 external relocation per `bl <libm>` site that targets `symbol`. Same external
// call-relocation shape as the world-write/http runtime symbols.
static void macho_append_math_call_relocations(ZBuf *relocs, const MachOEmitContext *ctx, MachOMathSymbol symbol, unsigned symbol_index) {
  for (size_t i = 0; ctx && i < ctx->math_call_patch_len; i++) {
    const MachOMathCallPatch *patch = &ctx->math_call_patches[i];
    if (patch->symbol != symbol) continue;
    uint32_t reloc_info = (symbol_index & 0x00ffffffu) |
                          (1u << 24) |  // r_pcrel
                          (2u << 25) |  // r_length: 4 bytes
                          (1u << 27) |  // r_extern: symbol table index
                          (2u << 28);   // ARM64_RELOC_BRANCH26
    append_u32le(relocs, (uint32_t)patch->patch_offset);
    append_u32le(relocs, reloc_info);
  }
}

static bool macho_record_libc_call_patch(MachOEmitContext *ctx, size_t patch_offset, MachOLibcSymbol symbol, const IrValue *value, ZDiag *diag) {
  if (!ctx) return macho_diag_at(diag, "direct AArch64 Mach-O libc relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->libc_call_patch_len == ctx->libc_call_patch_cap) {
    ctx->libc_call_patch_cap = z_grow_capacity(ctx->libc_call_patch_cap, ctx->libc_call_patch_len + 1, 4);
    ctx->libc_call_patches = z_checked_reallocarray(ctx->libc_call_patches, ctx->libc_call_patch_cap, sizeof(MachOLibcCallPatch));
  }
  ctx->libc_call_patches[ctx->libc_call_patch_len++] = (MachOLibcCallPatch){.patch_offset = patch_offset, .symbol = symbol};
  return true;
}

// True when at least one call site targets the given libSystem symbol. Drives which undefined
// externals (and their relocations) are emitted into the object's symbol table.
static bool macho_libc_symbol_used(const MachOEmitContext *ctx, MachOLibcSymbol symbol) {
  for (size_t i = 0; ctx && i < ctx->libc_call_patch_len; i++) {
    if (ctx->libc_call_patches[i].symbol == symbol) return true;
  }
  return false;
}

// One BRANCH26 external relocation per `bl <libc>` site that targets `symbol`. Same external
// call-relocation shape as the libm and world-write/http runtime symbols.
static void macho_append_libc_call_relocations(ZBuf *relocs, const MachOEmitContext *ctx, MachOLibcSymbol symbol, unsigned symbol_index) {
  for (size_t i = 0; ctx && i < ctx->libc_call_patch_len; i++) {
    const MachOLibcCallPatch *patch = &ctx->libc_call_patches[i];
    if (patch->symbol != symbol) continue;
    uint32_t reloc_info = (symbol_index & 0x00ffffffu) |
                          (1u << 24) |  // r_pcrel
                          (2u << 25) |  // r_length: 4 bytes
                          (1u << 27) |  // r_extern: symbol table index
                          (2u << 28);   // ARM64_RELOC_BRANCH26
    append_u32le(relocs, (uint32_t)patch->patch_offset);
    append_u32le(relocs, reloc_info);
  }
}

static size_t macho_data_relocation_count(const MachOEmitContext *ctx) {
  if (!ctx) return 0;
  if (!ctx->pie_relative_data) return ctx->data_patch_len;
  size_t count = ctx->data_patch_len * 2;
  for (size_t i = 0; i < ctx->data_patch_len; i++) {
    const MachODataPatch *patch = &ctx->data_patches[i];
    if (patch->data_offset != ctx->rodata_base_offset) count += 2;
  }
  return count;
}

static void macho_append_reloc(ZBuf *relocs, uint32_t address, uint32_t symbol_or_addend, bool pcrel, unsigned length, bool external, unsigned type) {
  uint32_t reloc_info = (symbol_or_addend & 0x00ffffffu) |
                        ((pcrel ? 1u : 0u) << 24) |
                        ((length & 3u) << 25) |
                        ((external ? 1u : 0u) << 27) |
                        ((type & 15u) << 28);
  append_u32le(relocs, address);
  append_u32le(relocs, reloc_info);
}

static void macho_append_data_relocations(ZBuf *relocs, const MachOEmitContext *ctx, unsigned data_symbol_index) {
  for (size_t i = 0; ctx && i < ctx->data_patch_len; i++) {
    const MachODataPatch *patch = &ctx->data_patches[i];
    if (ctx->pie_relative_data) {
      uint32_t addend = patch->data_offset - ctx->rodata_base_offset;
      if (addend != 0) macho_append_reloc(relocs, (uint32_t)patch->patch_offset + 4u, addend, false, 2, false, 10); // ARM64_RELOC_ADDEND
      macho_append_reloc(relocs, (uint32_t)patch->patch_offset + 4u, data_symbol_index, false, 2, true, 4);          // ARM64_RELOC_PAGEOFF12
      if (addend != 0) macho_append_reloc(relocs, (uint32_t)patch->patch_offset, addend, false, 2, false, 10);      // ARM64_RELOC_ADDEND
      macho_append_reloc(relocs, (uint32_t)patch->patch_offset, data_symbol_index, true, 2, true, 3);               // ARM64_RELOC_PAGE21
    } else {
      macho_append_reloc(relocs, (uint32_t)patch->patch_offset, data_symbol_index, false, 3, true, 0);              // ARM64_RELOC_UNSIGNED
    }
  }
}

static bool macho_const_u32_value(const IrValue *value, unsigned *out) {
  if (!value || value->kind != IR_VALUE_INT || value->int_value > UINT32_MAX) return false;
  if (out) *out = (unsigned)value->int_value;
  return true;
}

// Condition codes after a CMP. EQ/NE are sign-agnostic; the ordering comparisons pick signed
// (LT/LE/GT/GE) or unsigned (LO/LS/HI/HS) codes off the operand type, mirroring how the ELF
// backend selects signed vs unsigned setcc opcodes.
static unsigned macho_cond_for_compare(IrCompareOp op, bool is_unsigned) {
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

static unsigned macho_invert_cond(unsigned cond) {
  return cond ^ 1u;
}

// Condition codes for FP compares after FCMP. Chosen so every ordering involving NaN is false,
// matching IEEE 754 (and Zero's float-nan-compare contract): NaN sets C=1,V=1,N=0,Z=0.
//   ==: EQ (Z)        <:  MI (N)        >:  GT (Z=0 & N==V)
//   !=: NE (Z=0)      <=: LS (C=0|Z)    >=: GE (N==V)
// != is the only one true on NaN; the rest read false because N stays clear / C stays set.
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

// Byte width and log2(width) of a span/array element type. Only the element kinds a byte view can
// hold reach here; the 1-byte forms (u8/i8) have log2 0, so their index scaling is a no-op.
static unsigned macho_elem_byte_size(IrTypeKind type) {
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

static unsigned macho_elem_log2(IrTypeKind type) {
  unsigned size = macho_elem_byte_size(type);
  if (size == 8) return 3;
  if (size == 4) return 2;
  return 0;
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
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) {
    // Reinterpreting a constant-length byte view yields a constant element count: the underlying
    // byte length divided by the new element size.
    unsigned base_len = 0;
    if (!macho_byte_view_const_len(view->left, &base_len)) return false;
    unsigned size = macho_elem_byte_size(view->element_type);
    if (size == 0) return false;
    if (out) *out = base_len / size;
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
    append_u32le(text, 0x90000000u | (reg & 31u));                         // adrp xreg, target@page
    append_u32le(text, 0x91000000u | ((reg & 31u) << 5) | (reg & 31u));     // add xreg, xreg, target@pageoff
    return macho_record_data_patch(ctx, patch_offset, data_offset, value, diag);
  }
  while (((text->len + 8) % 8) != 0) macho_emit_nop(text);
  append_u32le(text, 0x58000000u | (2u << 5) | (reg & 31u)); // ldr xreg, .+8
  append_u32le(text, 0x14000003u); // b .+12, over the relocated literal
  size_t patch_offset = text->len;
  append_u64le(text, data_offset - (ctx ? ctx->rodata_base_offset : 0));
  return macho_record_data_patch(ctx, patch_offset, data_offset, value, diag);
}

static bool macho_emit_byte_view_ptr(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_byte_view_len(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_value_to_reg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_float_value_to_vreg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_float_value_to_vreg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag);
static unsigned macho_fp_scratch_slot_offset(unsigned frame_size, unsigned depth);
static unsigned macho_int_scratch_slot_offset(const IrFunction *fun, unsigned depth);
static bool macho_emit_marshal_call_arg(ZBuf *text, const IrFunction *fun, const IrValue *arg, unsigned *int_arg, unsigned *fp_arg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_call_to_reg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag);
static void macho_emit_epilogue(ZBuf *text, unsigned frame_size, bool restore_process_args);

static bool macho_emit_json_parse_bytes_call(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!macho_emit_byte_view_ptr(text, fun, value->left, 0, frame_size, ctx, diag)) return false;
  if (!macho_emit_byte_view_len(text, fun, value->left, 1, frame_size, ctx, diag)) return false;
  size_t patch = macho_emit_bl_placeholder(text);
  return macho_record_runtime_json_parse_bytes_patch(ctx, patch, value, diag);
}

static bool macho_emit_value_to_reg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_byte_view_len_depth(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_byte_view_ptr_depth(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag);
static bool macho_emit_span_index_addr_depth(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, IrTypeKind element_type, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag);

// depth is the integer binary/compare nesting level: a sub-expression evaluated while `depth` outer
// left operands are spilled in frame slots must use slots at or above `depth`, so depth threads
// through every sub-value evaluation (these helpers and the value emitter forward it unchanged; only
// a binary/compare consumes a slot and passes depth+1 to its operands). The non-depth wrappers below
// enter at depth 0 for instruction-level callers.
static bool macho_emit_byte_view_len(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  return macho_emit_byte_view_len_depth(text, fun, view, reg, frame_size, 0, ctx, diag);
}

static bool macho_emit_byte_view_len_depth(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag) {
  if (!view) return macho_diag_at(diag, "direct AArch64 Mach-O byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (view->data_len > 65535) return macho_diag_at(diag, "direct AArch64 Mach-O byte-view length is too large for the current MVP", view->line, view->column, "large byte view");
    macho_emit_movz_w(text, reg, view->data_len);
    return true;
  }
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    // A Mapping length is a 64-bit byte count (files may exceed 4 GiB); ordinary spans keep a 32-bit
    // element count.
    if (fun->locals[view->local_index].is_mapping) macho_emit_load_local_x(text, fun, reg, view->local_index, 8, frame_size);
    else macho_emit_load_local_w(text, fun, reg, view->local_index, 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field length lives 8 bytes past the field's ptr, as a 32-bit element count.
    macho_emit_load_local_w(text, fun, reg, view->local_index, view->field_offset + 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    // Maybe<owned<Mapping>>.value carries a 64-bit byte length; other Maybe spans keep 32 bits.
    if (fun->locals[view->local_index].is_mapping) macho_emit_load_local_x(text, fun, reg, view->local_index, 16, frame_size);
    else macho_emit_load_local_w(text, fun, reg, view->local_index, 16, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    unsigned end = 0;
    if ((!view->index || macho_const_u32_value(view->index, &start)) &&
        macho_const_u32_value(view->right, &end) && end >= start && end - start <= 65535) {
      macho_emit_movz_w(text, reg, end - start);
      return true;
    }
    if ((!view->index || macho_const_u32_value(view->index, &start)) && view->right) {
      if (!macho_emit_value_to_reg_depth(text, fun, view->right, reg, frame_size, depth, ctx, diag)) return false;
      if (start > 0) macho_emit_sub_w_imm(text, reg, reg, start);
      return true;
    }
    if (view->index && view->right) {
      unsigned tmp = reg == 8 ? 9 : 8;
      if (!macho_emit_value_to_reg_depth(text, fun, view->right, reg, frame_size, depth, ctx, diag)) return false;
      if (!macho_emit_value_to_reg_depth(text, fun, view->index, tmp, frame_size, depth, ctx, diag)) return false;
      macho_emit_binary_int(text, IR_BIN_SUB, reg, reg, tmp, false, false);
      return true;
    }
  }
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET && view->left) {
    if (!macho_emit_byte_view_len_depth(text, fun, view->left, reg, frame_size, depth, ctx, diag)) return false;
    // Element count = underlying byte length >> log2(element size). A 1-byte element (u8/i8) needs
    // no shift: the byte length already is the count.
    unsigned shift = macho_elem_log2(view->element_type);
    if (shift > 0) macho_emit_lsr_w_imm(text, reg, reg, shift);
    return true;
  }
  (void)ctx;
  return macho_diag_at(diag, "direct AArch64 Mach-O byte-view length currently requires a literal, constant slice, or byte-view local", view->line, view->column, "unsupported byte view length");
}

static bool macho_emit_byte_view_ptr(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  return macho_emit_byte_view_ptr_depth(text, fun, view, reg, frame_size, 0, ctx, diag);
}

static bool macho_emit_byte_view_ptr_depth(ZBuf *text, const IrFunction *fun, const IrValue *view, unsigned reg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag) {
  if (!view) return macho_diag_at(diag, "direct AArch64 Mach-O byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) {
    macho_emit_load_local_x(text, fun, reg, view->local_index, 0, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW && view->local_index < fun->local_len && fun->locals[view->local_index].is_record) {
    // Span field: the 8-byte pointer lives at the field offset within the record local.
    macho_emit_load_local_x(text, fun, reg, view->local_index, view->field_offset, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_MAYBE_VALUE && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
    macho_emit_load_local_x(text, fun, reg, view->local_index, 8, frame_size);
    return true;
  }
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    // A span over a fixed array yields the array base address; the element type drives later index
    // scaling. Any primitive element type a span can hold is accepted (matching the ELF backend).
    IrTypeKind elem = local->element_type;
    bool ok = local->is_array && (elem == IR_TYPE_U8 || elem == IR_TYPE_I8 || elem == IR_TYPE_I32 || elem == IR_TYPE_U32 ||
              elem == IR_TYPE_I64 || elem == IR_TYPE_U64 || elem == IR_TYPE_USIZE || elem == IR_TYPE_F32 || elem == IR_TYPE_F64);
    if (!ok) return macho_diag_at(diag, "direct AArch64 Mach-O byte-view array element type is unsupported", view->line, view->column, "unsupported array view");
    macho_emit_add_x_sp_imm(text, reg, macho_local_slot_offset(fun, view->array_index, 0, frame_size));
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) {
    return macho_emit_rodata_ptr_literal(text, reg, view->data_offset, ctx, view, diag);
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    if (!macho_emit_byte_view_ptr_depth(text, fun, view->left, reg, frame_size, depth, ctx, diag)) return false;
    if (!view->index) return true;
    // The slice start is an element COUNT; the pointer must advance by start*sizeof(element). u8/i8
    // (size 1) need no scaling; wider element types shift the start by log2(size). Mirrors ELF's
    // elf_type_byte_size scaling of the slice start.
    unsigned log2 = macho_elem_log2(view->element_type);
    if (macho_const_u32_value(view->index, &start)) {
      unsigned byte_off = start << log2;
      if (byte_off > 4095) return macho_diag_at(diag, "direct AArch64 Mach-O byte slice constant start is too large", view->line, view->column, "unsupported byte slice");
      if (byte_off > 0) macho_emit_add_x_imm(text, reg, reg, byte_off);
      return true;
    }
    unsigned tmp = reg == 8 ? 9 : 8;
    if (!macho_emit_value_to_reg_depth(text, fun, view->index, tmp, frame_size, depth, ctx, diag)) return false;
    macho_emit_add_x_reg_lsl(text, reg, reg, tmp, log2);
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) {
    // A typed reinterpret keeps the same base pointer; only the length (element count) changes.
    return macho_emit_byte_view_ptr_depth(text, fun, view->left, reg, frame_size, depth, ctx, diag);
  }
  return macho_diag_at(diag, "direct AArch64 Mach-O value is not a supported byte view", view->line, view->column, "unsupported byte view");
}

// Compute the address of element `index` of a span (byte-view) local into x9, bounds-checked
// against the span's runtime length (idx < len, else brk #0 — the macho analog of the array
// out-of-range trap). `element_type` sets the element size used to scale the index. The index is
// materialized into w8 first, so callers must not rely on x8 surviving this call. x9 holds the
// element address on return.
static bool macho_emit_span_index_addr(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, IrTypeKind element_type, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  return macho_emit_span_index_addr_depth(text, fun, local_index, index, element_type, frame_size, 0, ctx, diag);
}

static bool macho_emit_span_index_addr_depth(ZBuf *text, const IrFunction *fun, unsigned local_index, const IrValue *index, IrTypeKind element_type, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag) {
  if (!index || !macho_emit_value_to_reg_depth(text, fun, index, 8, frame_size, depth, ctx, diag)) return false;
  macho_emit_load_local_w(text, fun, 9, local_index, 8, frame_size); // len @ slot+8
  macho_emit_cmp_w(text, 8, 9);
  size_t ok_patch = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
  append_u32le(text, 0xd4200000u); // brk #0
  macho_patch_cond19(text, ok_patch, text->len);
  macho_emit_load_local_x(text, fun, 9, local_index, 0, frame_size); // ptr @ slot+0
  unsigned shift = macho_elem_log2(element_type);
  if (shift > 0) macho_emit_add_x_reg_lsl(text, 9, 9, 8, shift);
  else macho_emit_add_x_reg(text, 9, 9, 8);
  return true;
}

static bool macho_emit_call_to_reg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  return macho_emit_call_to_reg_depth(text, fun, value, reg, frame_size, 0, ctx, diag);
}

static bool macho_emit_call_to_reg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag) {
  if (value->arg_len > 8) return macho_diag_at(diag, "direct AArch64 Mach-O call supports at most eight arguments", value->line, value->column, "too many arguments");
  // A record-returning call must receive its destination in x8; it is always routed through
  // macho_emit_record_call_with_dest (a record bind or return), never read as a plain scalar value.
  if (value->type == IR_TYPE_RECORD) return macho_diag_at(diag, "direct AArch64 Mach-O record-returning call must bind to a record local or be returned", value->line, value->column, "unsupported record call position");
  // Integer and FP arguments fill their own register banks (x0..x7 and v0..v7). Each argument
  // expression is materialized directly into its destination register; record args pass a pointer
  // and span args pass (ptr,len) in two int regs. Scratch lives in x8/x9 and v8/v9, none of which
  // is an argument register, so earlier arguments survive. `depth` is the nesting level at which this
  // call is evaluated: an argument that is itself a spilling binary/compare must use a frame scratch
  // slot at this depth (not slot 0), or it would clobber an outer expression's spilled operand.
  unsigned int_arg = 0;
  unsigned fp_arg = 0;
  for (size_t i = 0; i < value->arg_len; i++) {
    if (!macho_emit_marshal_call_arg(text, fun, value->args[i], &int_arg, &fp_arg, frame_size, depth, ctx, diag)) return false;
  }
  size_t patch = macho_emit_bl_placeholder(text);
  if (!macho_record_call_patch(ctx, patch, value->callee_index, value, diag)) return false;
  if (macho_type_is_float(value->type)) {
    if (reg != 0) macho_emit_fmov_v(text, reg, 0, macho_type_is_f64(value->type));
  } else if (reg != 0) {
    if (macho_type_is_scalar64(value->type)) macho_emit_mov_x(text, reg, 0);
    else macho_emit_mov_w(text, reg, 0);
  }
  return true;
}

static bool macho_emit_value_to_reg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  return macho_emit_value_to_reg_depth(text, fun, value, reg, frame_size, 0, ctx, diag);
}

static bool macho_emit_value_to_reg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned reg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value) return macho_diag_at(diag, "direct AArch64 Mach-O expression is missing", 1, 1, "missing expression");
  switch (value->kind) {
    case IR_VALUE_BOOL:
    case IR_VALUE_INT:
      // 64-bit literals need the full MOVZ/MOVK immediate sequence — u64/i64 constants such as the
      // PRNG multiplier do not fit in 32 bits, so the truncating movz_x form would corrupt them.
      if (macho_type_is_scalar64(value->type)) macho_emit_mov_imm64(text, reg, (uint64_t)value->int_value);
      else macho_emit_movz_w(text, reg, (uint32_t)value->int_value);
      return true;
    case IR_VALUE_LOCAL:
      if (value->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O local index is out of range", value->line, value->column, "invalid local");
      if (fun->locals[value->local_index].type == IR_TYPE_BYTE_VIEW) {
        return macho_diag_at(diag, "direct AArch64 Mach-O byte-view local cannot be used as a scalar", value->line, value->column, "byte-view local");
      }
      if (macho_type_is_scalar64(fun->locals[value->local_index].type)) macho_emit_load_local_x(text, fun, reg, value->local_index, 0, frame_size);
      else macho_emit_load_local_w(text, fun, reg, value->local_index, 0, frame_size);
      return true;
    case IR_VALUE_BINARY:
      if (value->binary_op == IR_BIN_AND) {
        if (!macho_emit_value_to_reg_depth(text, fun, value->left, reg, frame_size, depth, ctx, diag)) return false;
        size_t left_false = macho_emit_cbz_w_placeholder(text, reg);
        if (!macho_emit_value_to_reg_depth(text, fun, value->right, reg, frame_size, depth, ctx, diag)) return false;
        size_t right_false = macho_emit_cbz_w_placeholder(text, reg);
        macho_emit_movz_w(text, reg, 1);
        size_t end_patch = macho_emit_b_placeholder(text);
        macho_patch_cond19(text, left_false, text->len);
        macho_patch_cond19(text, right_false, text->len);
        macho_emit_movz_w(text, reg, 0);
        macho_patch_branch26(text, end_patch, text->len);
        return true;
      }
      if (value->binary_op == IR_BIN_OR) {
        if (!macho_emit_value_to_reg_depth(text, fun, value->left, reg, frame_size, depth, ctx, diag)) return false;
        size_t eval_right = macho_emit_cbz_w_placeholder(text, reg);
        macho_emit_movz_w(text, reg, 1);
        size_t left_true_end = macho_emit_b_placeholder(text);
        macho_patch_cond19(text, eval_right, text->len);
        if (!macho_emit_value_to_reg_depth(text, fun, value->right, reg, frame_size, depth, ctx, diag)) return false;
        size_t right_false = macho_emit_cbz_w_placeholder(text, reg);
        macho_emit_movz_w(text, reg, 1);
        size_t right_true_end = macho_emit_b_placeholder(text);
        macho_patch_cond19(text, right_false, text->len);
        macho_emit_movz_w(text, reg, 0);
        macho_patch_branch26(text, left_true_end, text->len);
        macho_patch_branch26(text, right_true_end, text->len);
        return true;
      }
      if (value->binary_op != IR_BIN_ADD && value->binary_op != IR_BIN_SUB && value->binary_op != IR_BIN_MUL &&
          value->binary_op != IR_BIN_DIV && value->binary_op != IR_BIN_MOD) return macho_diag_at(diag, "direct AArch64 Mach-O binary operator is unsupported", value->line, value->column, "unsupported operator");
      // Spill the left operand to a frame scratch slot across the right's evaluation: the right may
      // clobber x8 (a span/array index uses it, a call trashes the caller-saved set), so x8 cannot be
      // relied on to survive. One slot per nesting level keeps nested binaries independent. The op
      // width (32- vs 64-bit) and DIV/MOD signedness come from the result type, mirroring ELF.
      {
        unsigned slot = macho_int_scratch_slot_offset(fun, depth);
        bool wide = macho_type_is_scalar64(value->type);
        bool is_unsigned = macho_type_is_unsigned(value->type);
        if (!macho_emit_value_to_reg_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
        macho_emit_str_x_disp(text, 8, 31, slot);
        if (!macho_emit_value_to_reg_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
        macho_emit_ldr_x_imm(text, 8, 31, slot);
        macho_emit_binary_int(text, value->binary_op, reg, 8, 9, wide, is_unsigned);
      }
      return true;
    case IR_VALUE_COMPARE: {
      if (!value->left || !value->right) {
        return macho_diag_at(diag, "direct AArch64 Mach-O comparison requires two operands", value->line, value->column, "invalid comparison");
      }
      if (macho_type_is_float(value->left->type)) {
        bool is64 = macho_type_is_f64(value->left->type);
        // A float comparison's result is an integer boolean; its operands are floats. Spill the left
        // operand to an FP scratch slot across the right's evaluation, then compare and materialize
        // the IEEE-correct boolean with CSET. FP scratch is indexed by FP depth, independent of the
        // integer scratch counter, so depth 0 here is correct (a float compare never nests under an
        // integer binary's spilled operand in the same register file).
        unsigned slot = macho_fp_scratch_slot_offset(frame_size, 0);
        if (!macho_emit_float_value_to_vreg_depth(text, fun, value->left, 8, frame_size, 1, ctx, diag)) return false;
        macho_emit_str_v_sp(text, 8, slot, is64);
        if (!macho_emit_float_value_to_vreg_depth(text, fun, value->right, 9, frame_size, 1, ctx, diag)) return false;
        macho_emit_ldr_v_sp(text, 8, slot, is64);
        macho_emit_fcmp(text, 8, 9, is64);
        macho_emit_cset(text, reg, macho_float_cond_for_compare(value->compare_op));
        return true;
      }
      // Integer compare: same left-spill discipline as the integer binary path. The compare width
      // (CMP Xn vs CMP Wn) and the signed/unsigned condition codes come from the operand type,
      // mirroring ELF's wide/unsigned setcc selection.
      {
        unsigned slot = macho_int_scratch_slot_offset(fun, depth);
        bool wide = macho_type_is_scalar64(value->left->type);
        bool is_unsigned = macho_type_is_unsigned(value->left->type);
        if (!macho_emit_value_to_reg_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
        macho_emit_str_x_disp(text, 8, 31, slot);
        if (!macho_emit_value_to_reg_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
        macho_emit_ldr_x_imm(text, 8, 31, slot);
        if (wide) macho_emit_cmp_x(text, 8, 9);
        else macho_emit_cmp_w(text, 8, 9);
        macho_emit_movz_w(text, reg, 0);
        size_t false_patch = macho_emit_b_cond_placeholder(text, macho_invert_cond(macho_cond_for_compare(value->compare_op, is_unsigned)));
        macho_emit_movz_w(text, reg, 1);
        macho_patch_cond19(text, false_patch, text->len);
      }
      return true;
    }
    case IR_VALUE_CALL:
      return macho_emit_call_to_reg_depth(text, fun, value, reg, frame_size, depth, ctx, diag);
    case IR_VALUE_JSON_PARSE_BYTES:
      if (!macho_emit_json_parse_bytes_call(text, fun, value, frame_size, ctx, diag)) return false;
      if (reg != 0) macho_emit_mov_x(text, reg, 0);
      return true;
    case IR_VALUE_JSON_VALIDATE_BYTES:
      if (!macho_emit_json_parse_bytes_call(text, fun, value, frame_size, ctx, diag)) return false;
      macho_emit_cmp_x(text, 0, 31);
      macho_emit_movz_w(text, reg, 0);
      {
        size_t invalid = macho_emit_b_cond_placeholder(text, 11); // signed less than
        macho_emit_movz_w(text, reg, 1);
        macho_patch_cond19(text, invalid, text->len);
      }
      return true;
    case IR_VALUE_JSON_STREAM_TOKENS_BYTES:
      if (!macho_emit_json_parse_bytes_call(text, fun, value, frame_size, ctx, diag)) return false;
      macho_emit_cmp_x(text, 0, 31);
      {
        size_t ok = macho_emit_b_cond_placeholder(text, 10); // signed greater or equal
        if (reg != 0) macho_emit_mov_x(text, reg, 31);
        else macho_emit_mov_x(text, 0, 31);
        size_t done = macho_emit_b_placeholder(text);
        macho_patch_cond19(text, ok, text->len);
        if (reg != 0) macho_emit_mov_x(text, reg, 0);
        macho_patch_branch26(text, done, text->len);
      }
      return true;
    case IR_VALUE_HTTP_FETCH: {
      if (!macho_emit_byte_view_ptr(text, fun, value->left, 0, frame_size, ctx, diag)) return false;
      if (!macho_emit_byte_view_len(text, fun, value->left, 1, frame_size, ctx, diag)) return false;
      if (!macho_emit_byte_view_ptr(text, fun, value->right, 2, frame_size, ctx, diag)) return false;
      if (!macho_emit_byte_view_len(text, fun, value->right, 3, frame_size, ctx, diag)) return false;
      if (!macho_emit_value_to_reg(text, fun, value->index, 4, frame_size, ctx, diag)) return false;
      size_t patch = macho_emit_bl_placeholder(text);
      if (!macho_record_runtime_http_fetch_patch(ctx, patch, value, diag)) return false;
      if (reg != 0) macho_emit_mov_x(text, reg, 0);
      return true;
    }
    case IR_VALUE_HTTP_RESULT_OK:
    case IR_VALUE_HTTP_RESULT_STATUS:
    case IR_VALUE_HTTP_RESULT_BODY_LEN:
    case IR_VALUE_HTTP_RESULT_ERROR:
    case IR_VALUE_HTTP_HEADER_FOUND:
    case IR_VALUE_HTTP_HEADER_OFFSET:
    case IR_VALUE_HTTP_HEADER_LEN: {
      if (!macho_emit_value_to_reg(text, fun, value->left, 0, frame_size, ctx, diag)) return false;
      size_t patch = macho_emit_bl_placeholder(text);
      if (value->kind == IR_VALUE_HTTP_RESULT_OK) {
        if (!macho_record_runtime_http_result_ok_patch(ctx, patch, value, diag)) return false;
      } else if (value->kind == IR_VALUE_HTTP_RESULT_STATUS) {
        if (!macho_record_runtime_http_result_status_patch(ctx, patch, value, diag)) return false;
      } else if (value->kind == IR_VALUE_HTTP_RESULT_BODY_LEN) {
        if (!macho_record_runtime_http_result_body_len_patch(ctx, patch, value, diag)) return false;
      } else if (value->kind == IR_VALUE_HTTP_RESULT_ERROR) {
        if (!macho_record_runtime_http_result_error_patch(ctx, patch, value, diag)) return false;
      } else if (value->kind == IR_VALUE_HTTP_HEADER_FOUND) {
        if (!macho_record_runtime_http_header_found_patch(ctx, patch, value, diag)) return false;
      } else if (value->kind == IR_VALUE_HTTP_HEADER_OFFSET) {
        if (!macho_record_runtime_http_header_offset_patch(ctx, patch, value, diag)) return false;
      } else if (!macho_record_runtime_http_header_len_patch(ctx, patch, value, diag)) {
        return false;
      }
      if (reg != 0) macho_emit_mov_w(text, reg, 0);
      return true;
    }
    case IR_VALUE_HTTP_RESPONSE_LEN:
    case IR_VALUE_HTTP_RESPONSE_HEADERS_LEN:
    case IR_VALUE_HTTP_RESPONSE_BODY_OFFSET: {
      if (!macho_emit_byte_view_ptr(text, fun, value->left, 0, frame_size, ctx, diag)) return false;
      if (!macho_emit_byte_view_len(text, fun, value->left, 1, frame_size, ctx, diag)) return false;
      size_t patch = macho_emit_bl_placeholder(text);
      if (value->kind == IR_VALUE_HTTP_RESPONSE_LEN) {
        if (!macho_record_runtime_http_result_patch(&ctx->runtime_http_response_len_patches, &ctx->runtime_http_response_len_patch_len, &ctx->runtime_http_response_len_patch_cap, patch, value, diag)) return false;
      } else if (value->kind == IR_VALUE_HTTP_RESPONSE_HEADERS_LEN) {
        if (!macho_record_runtime_http_result_patch(&ctx->runtime_http_response_headers_len_patches, &ctx->runtime_http_response_headers_len_patch_len, &ctx->runtime_http_response_headers_len_patch_cap, patch, value, diag)) return false;
      } else if (!macho_record_runtime_http_result_patch(&ctx->runtime_http_response_body_offset_patches, &ctx->runtime_http_response_body_offset_patch_len, &ctx->runtime_http_response_body_offset_patch_cap, patch, value, diag)) {
        return false;
      }
      if (reg != 0) macho_emit_mov_w(text, reg, 0);
      return true;
    }
    case IR_VALUE_HTTP_HEADER_VALUE: {
      if (!macho_emit_byte_view_ptr(text, fun, value->left, 0, frame_size, ctx, diag)) return false;
      if (!macho_emit_byte_view_len(text, fun, value->left, 1, frame_size, ctx, diag)) return false;
      if (!macho_emit_byte_view_ptr(text, fun, value->right, 2, frame_size, ctx, diag)) return false;
      if (!macho_emit_byte_view_len(text, fun, value->right, 3, frame_size, ctx, diag)) return false;
      size_t patch = macho_emit_bl_placeholder(text);
      if (!macho_record_runtime_http_header_value_patch(ctx, patch, value, diag)) return false;
      if (reg != 0) macho_emit_mov_x(text, reg, 0);
      return true;
    }
    case IR_VALUE_VEC_LEN:
    case IR_VALUE_VEC_CAPACITY:
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_VEC) return macho_diag_at(diag, "direct AArch64 Mach-O Vec helper requires a Vec local", value->line, value->column, "invalid Vec local");
      macho_emit_load_local_w(text, fun, reg, value->local_index, value->kind == IR_VALUE_VEC_LEN ? 8 : 12, frame_size);
      return true;
    case IR_VALUE_VEC_PUSH: {
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_VEC) return macho_diag_at(diag, "direct AArch64 Mach-O Vec push requires a Vec local", value->line, value->column, "invalid Vec local");
      macho_emit_load_local_w(text, fun, 8, value->local_index, 8, frame_size);
      macho_emit_load_local_w(text, fun, 9, value->local_index, 12, frame_size);
      macho_emit_cmp_w(text, 8, 9);
      size_t ok_patch = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
      macho_emit_movz_w(text, reg, 0);
      size_t end_patch = macho_emit_b_placeholder(text);
      macho_patch_cond19(text, ok_patch, text->len);
      macho_emit_store_local_w(text, fun, 8, value->local_index, 8, frame_size);
      macho_emit_load_local_x(text, fun, 9, value->local_index, 0, frame_size);
      macho_emit_add_x_reg(text, 9, 9, 8);
      if (!macho_emit_value_to_reg(text, fun, value->left, 10, frame_size, ctx, diag)) return false;
      macho_emit_strb_w(text, 10, 9);
      macho_emit_add_w_imm(text, 8, 8, 1);
      macho_emit_store_local_w(text, fun, 8, value->local_index, 8, frame_size);
      macho_emit_movz_w(text, reg, 1);
      macho_patch_branch26(text, end_patch, text->len);
      return true;
    }
    case IR_VALUE_ARGS_LEN:
      macho_emit_mov_w(text, reg, 20);
      return true;
    case IR_VALUE_FS_HOST:
      // std.fs.host() yields the host filesystem handle, an i32 token that carries no state on a
      // hosted target (the OS-interface lowerings call libSystem directly). Mirrors the ELF backend,
      // which returns 0 in rax.
      macho_emit_movz_w(text, reg, 0);
      return true;
    case IR_VALUE_FS_MUNMAP: {
      // std.fs.munmap(&mut m): release a mapping via libSystem _munmap(addr, len). The Mapping local
      // stores ptr@0 (full 64-bit) and len@8 (64-bit byte count, matching the file-mmap store), so
      // both are loaded with the x form. Result type is Void; nothing is produced into `reg`.
      if (value->local_index >= fun->local_len || fun->locals[value->local_index].type != IR_TYPE_BYTE_VIEW) {
        return macho_diag_at(diag, "direct AArch64 Mach-O std.fs.munmap requires a Mapping local", value->line, value->column, "invalid Mapping");
      }
      macho_emit_load_local_x(text, fun, 0, value->local_index, 0, frame_size); // addr
      macho_emit_load_local_x(text, fun, 1, value->local_index, 8, frame_size); // len
      size_t patch = macho_emit_bl_placeholder(text);
      if (!macho_record_libc_call_patch(ctx, patch, MACHO_LIBC_MUNMAP, value, diag)) return false;
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
      return macho_emit_byte_view_len_depth(text, fun, value->left, reg, frame_size, depth, ctx, diag);
    case IR_VALUE_BYTE_VIEW_EQ: {
      // std.mem.eqlBytes(a, b): 1 if both views have the same length and bytes, else 0. Mirrors the
      // ELF backend's length-then-bytewise compare. macho addresses locals via sp, so the values
      // that must outlive a sibling's evaluation are spilled to the integer scratch slot rather than
      // pushed: pointers are settled into stable scratch first, then the length last, so the loop's
      // invariants (both pointers in x10/x11, length in x12) survive without any further byte-view
      // evaluation. Sub-views are evaluated at depth+1 so a runtime-slice index never reuses our slot.
      if (!value->left || !value->right) return macho_diag_at(diag, "direct AArch64 Mach-O byte-view equality requires two byte views", value->line, value->column, "missing byte view");
      unsigned slot = macho_int_scratch_slot_offset(fun, depth);
      // Settle both pointers first (left in x10, right in x11).
      if (!macho_emit_byte_view_ptr_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_str_x_disp(text, 8, 31, slot);
      if (!macho_emit_byte_view_ptr_depth(text, fun, value->right, 11, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_ldr_x_imm(text, 10, 31, slot);
      // Compare lengths (left spilled across right). On mismatch the result is 0.
      if (!macho_emit_byte_view_len_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_str_x_disp(text, 8, 31, slot);
      if (!macho_emit_byte_view_len_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_ldr_x_imm(text, 8, 31, slot);
      macho_emit_cmp_w(text, 8, 9);
      size_t len_mismatch = macho_emit_b_cond_placeholder(text, 1); // NE
      macho_emit_mov_w(text, 12, 8); // length (counted-down loop bound) in x12
      // Byte loop: i in x13 from 0; compare left[i] vs right[i]; any mismatch -> 0, else fall to 1.
      macho_emit_movz_x(text, 13, 0);
      size_t loop = text->len;
      macho_emit_cmp_x(text, 13, 12);
      size_t loop_done = macho_emit_b_cond_placeholder(text, 2); // HS (i >= len): all matched
      macho_emit_add_x_reg(text, 14, 10, 13);
      macho_emit_ldrb_w(text, 14, 14);
      macho_emit_add_x_reg(text, 15, 11, 13);
      macho_emit_ldrb_w(text, 15, 15);
      macho_emit_cmp_w(text, 14, 15);
      size_t byte_mismatch = macho_emit_b_cond_placeholder(text, 1); // NE
      macho_emit_add_x_imm(text, 13, 13, 1);
      size_t back = macho_emit_b_placeholder(text);
      macho_patch_branch26(text, back, loop);
      // false: lengths differ or a byte differed.
      macho_patch_cond19(text, len_mismatch, text->len);
      macho_patch_cond19(text, byte_mismatch, text->len);
      macho_emit_movz_w(text, reg, 0);
      size_t end = macho_emit_b_placeholder(text);
      // true: every byte matched.
      macho_patch_cond19(text, loop_done, text->len);
      macho_emit_movz_w(text, reg, 1);
      macho_patch_branch26(text, end, text->len);
      return true;
    }
    case IR_VALUE_BYTE_COPY: {
      // std.mem.copy(dst, src): copy min(src.len, dst.len) bytes and return the count. value->left is
      // the source view, value->right the destination. Like byte-view equality, macho cannot push
      // sp-relative temporaries, so the pointers and length are settled into stable registers (src in
      // x10, dst in x11, count in x12) via the integer scratch slot before the byte loop; the loop
      // body touches only x13/x14. Sub-views are evaluated at depth+1 so a runtime slice never reuses
      // our slot.
      if (!value->left || !value->right) return macho_diag_at(diag, "direct AArch64 Mach-O byte copy requires source and destination byte views", value->line, value->column, "missing byte view");
      unsigned slot = macho_int_scratch_slot_offset(fun, depth);
      // Settle source pointer (x10) and destination pointer (x11).
      if (!macho_emit_byte_view_ptr_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_str_x_disp(text, 8, 31, slot);
      if (!macho_emit_byte_view_ptr_depth(text, fun, value->right, 11, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_ldr_x_imm(text, 10, 31, slot);
      // count = min(src.len, dst.len) into x12 (src.len spilled across dst.len's evaluation).
      if (!macho_emit_byte_view_len_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_str_x_disp(text, 8, 31, slot);
      if (!macho_emit_byte_view_len_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_ldr_x_imm(text, 8, 31, slot);
      macho_emit_mov_w(text, 12, 8);
      macho_emit_cmp_w(text, 9, 12);
      size_t keep_src_len = macho_emit_b_cond_placeholder(text, 2); // HS: dst.len >= src.len, keep src.len
      macho_emit_mov_w(text, 12, 9);                                // else count = dst.len
      macho_patch_cond19(text, keep_src_len, text->len);
      // Byte loop: i in x13 from 0; while i < count copy src[i] -> dst[i].
      macho_emit_movz_x(text, 13, 0);
      size_t loop = text->len;
      macho_emit_cmp_x(text, 13, 12);
      size_t loop_done = macho_emit_b_cond_placeholder(text, 2); // HS (i >= count)
      macho_emit_add_x_reg(text, 14, 10, 13);
      macho_emit_ldrb_w(text, 14, 14);
      macho_emit_add_x_reg(text, 15, 11, 13);
      macho_emit_strb_w(text, 14, 15);
      macho_emit_add_x_imm(text, 13, 13, 1);
      size_t back = macho_emit_b_placeholder(text);
      macho_patch_branch26(text, back, loop);
      macho_patch_cond19(text, loop_done, text->len);
      macho_emit_mov_w(text, reg, 12); // result = bytes copied
      return true;
    }
    case IR_VALUE_BYTE_FILL: {
      // std.mem.fill(dst, byte): write `byte` across the whole destination view and return its
      // length. value->left is the u8 fill value, value->right the destination. The fill byte and the
      // length are settled into stable registers (fill in x14, length in x12) via the scratch slot,
      // then the loop body touches only x13.
      if (!value->left || !value->right) return macho_diag_at(diag, "direct AArch64 Mach-O byte fill requires a destination byte view", value->line, value->column, "missing byte view");
      unsigned slot = macho_int_scratch_slot_offset(fun, depth);
      // Destination pointer in x11.
      if (!macho_emit_byte_view_ptr_depth(text, fun, value->right, 11, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_str_x_disp(text, 11, 31, slot);
      // Fill value (low byte) in x14; reload the pointer afterwards in case its evaluation clobbered x11.
      if (!macho_emit_value_to_reg_depth(text, fun, value->left, 14, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_ldr_x_imm(text, 11, 31, slot);
      // Length into x12 (the pointer is held in our slot across this evaluation).
      if (!macho_emit_byte_view_len_depth(text, fun, value->right, 12, frame_size, depth + 1, ctx, diag)) return false;
      // Byte loop: i in x13 from 0; while i < len store the fill byte to dst[i].
      macho_emit_movz_x(text, 13, 0);
      size_t loop = text->len;
      macho_emit_cmp_x(text, 13, 12);
      size_t loop_done = macho_emit_b_cond_placeholder(text, 2); // HS (i >= len)
      macho_emit_add_x_reg(text, 15, 11, 13);
      macho_emit_strb_w(text, 14, 15);
      macho_emit_add_x_imm(text, 13, 13, 1);
      size_t back = macho_emit_b_placeholder(text);
      macho_patch_branch26(text, back, loop);
      macho_patch_cond19(text, loop_done, text->len);
      macho_emit_mov_w(text, reg, 12); // result = bytes filled (destination length)
      return true;
    }
    case IR_VALUE_BYTE_VIEW_INDEX_LOAD: {
      unsigned const_index = 0;
      unsigned char byte = 0;
      if (macho_const_u32_value(value->index, &const_index) &&
          macho_byte_view_const_byte(ctx ? ctx->program : NULL, value->left, const_index, &byte)) {
        macho_emit_movz_w(text, reg, byte);
        return true;
      }
      if (!value->index || !macho_emit_value_to_reg_depth(text, fun, value->index, 8, frame_size, depth, ctx, diag)) return false;
      if (!macho_emit_byte_view_len_depth(text, fun, value->left, 9, frame_size, depth, ctx, diag)) return false;
      macho_emit_cmp_w(text, 8, 9);
      size_t ok_patch = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
      append_u32le(text, 0xd4200000u); // brk #0
      macho_patch_cond19(text, ok_patch, text->len);
      if (!macho_emit_byte_view_ptr_depth(text, fun, value->left, 9, frame_size, depth, ctx, diag)) return false;
      macho_emit_add_x_reg(text, 9, 9, 8);
      macho_emit_ldrb_w(text, reg, 9);
      return true;
    }
    case IR_VALUE_BYTE_VIEW_READ_INT_LE: {
      // std.codec.readI32Le / readU32Le: read 4 little-endian bytes at a byte offset. AArch64 is
      // little-endian, so a 32-bit load at ptr+offset already yields the value. Bounds-check that
      // offset+4 fits within the span length before loading.
      if (!value->left) return macho_diag_at(diag, "direct AArch64 Mach-O readI32Le/readU32Le requires a byte view", value->line, value->column, "missing byte view");
      if (!value->index) return macho_diag_at(diag, "direct AArch64 Mach-O readI32Le/readU32Le requires an offset", value->line, value->column, "missing offset");
      if (!macho_emit_value_to_reg_depth(text, fun, value->index, 8, frame_size, depth, ctx, diag)) return false;
      macho_emit_add_w_imm(text, 10, 8, 4); // offset + 4
      if (!macho_emit_byte_view_len_depth(text, fun, value->left, 9, frame_size, depth, ctx, diag)) return false;
      macho_emit_cmp_w(text, 10, 9);
      size_t ok_patch = macho_emit_b_cond_placeholder(text, 9); // unsigned lower or same
      append_u32le(text, 0xd4200000u); // brk #0
      macho_patch_cond19(text, ok_patch, text->len);
      if (!macho_emit_byte_view_ptr_depth(text, fun, value->left, 9, frame_size, depth, ctx, diag)) return false;
      macho_emit_add_x_reg(text, 9, 9, 8);
      macho_emit_ldr_w(text, reg, 9);
      return true;
    }
    case IR_VALUE_INDEX_LOAD: {
      if (value->array_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O indexed load array is out of range", value->line, value->column, "invalid array local");
      const IrLocal *local = &fun->locals[value->array_index];
      if (local->type == IR_TYPE_BYTE_VIEW) {
        // Typed-span element read: bounds-check against the runtime length, compute ptr+idx*size,
        // then load by element width (u8 zero-extends, i8 sign-extends, i32/u32/usize via w/x).
        if (!macho_emit_span_index_addr_depth(text, fun, value->array_index, value->index, local->element_type, frame_size, depth, ctx, diag)) return false;
        if (local->element_type == IR_TYPE_U8) macho_emit_ldrb_w(text, reg, 9);
        else if (local->element_type == IR_TYPE_I8) macho_emit_ldrsb_w(text, reg, 9);
        else if (macho_elem_byte_size(local->element_type) == 8) macho_emit_ldr_x_imm(text, reg, 9, 0);
        else macho_emit_ldr_w(text, reg, 9);
        return true;
      }
      unsigned const_index = 0;
      if (local->is_array && local->element_type != IR_TYPE_U8 && macho_const_u32_value(value->index, &const_index) && const_index < local->array_len) {
        macho_emit_load_local_w(text, fun, reg, value->array_index, const_index * 4u, frame_size);
        return true;
      }
      if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
        if (!value->index || !macho_emit_value_to_reg_depth(text, fun, value->index, 8, frame_size, depth, ctx, diag)) return false;
        macho_emit_movz_w(text, 9, local->array_len);
        macho_emit_cmp_w(text, 8, 9);
        size_t ok_patch = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
        append_u32le(text, 0xd4200000u); // brk #0
        macho_patch_cond19(text, ok_patch, text->len);
        macho_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, value->array_index, 0, frame_size));
        macho_emit_add_x_reg_lsl(text, 9, 9, 8, 2);
        append_u32le(text, 0xb9400000u | (9u << 5) | (reg & 31u));
        return true;
      }
      if (!local->is_array || local->element_type != IR_TYPE_U8) return macho_diag_at(diag, "direct AArch64 Mach-O indexed load requires [N]u8 or integer arrays", value->line, value->column, "unsupported array local");
      if (!value->index || !macho_emit_value_to_reg_depth(text, fun, value->index, 8, frame_size, depth, ctx, diag)) return false;
      macho_emit_movz_w(text, 9, local->array_len);
      macho_emit_cmp_w(text, 8, 9);
      size_t ok_patch = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
      append_u32le(text, 0xd4200000u); // brk #0
      macho_patch_cond19(text, ok_patch, text->len);
      macho_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, value->array_index, 0, frame_size));
      macho_emit_add_x_reg(text, 9, 9, 8);
      macho_emit_ldrb_w(text, reg, 9);
      return true;
    }
    case IR_VALUE_FIELD_LOAD:
      if (value->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O field load record is out of range", value->line, value->column, "invalid record local");
      if (!fun->locals[value->local_index].is_record) return macho_diag_at(diag, "direct AArch64 Mach-O field load requires record local", value->line, value->column, "non-record local");
      macho_emit_load_field(text, fun, reg, value->local_index, value->field_offset, value->type, frame_size);
      return true;
    case IR_VALUE_CAST: {
      // Casts only reach the IR when floats are involved. A float-typed result is produced in
      // the FP path; here we only handle conversions whose result is an integer (float -> i32).
      if (!value->left) return macho_diag_at(diag, "direct AArch64 Mach-O cast missing operand", value->line, value->column, "missing cast operand");
      if (macho_type_is_float(value->type) || !macho_type_is_float(value->left->type)) {
        return macho_diag_at(diag, "direct AArch64 Mach-O integer cast is not a float conversion", value->line, value->column, "unsupported cast");
      }
      if (!macho_emit_float_value_to_vreg(text, fun, value->left, 8, frame_size, ctx, diag)) return false;
      macho_emit_fcvtzs_w(text, reg, 8, macho_type_is_f64(value->left->type));
      return true;
    }
    case IR_VALUE_MATH_ISNANF: {
      // NaN is the only value where x != x. FCMP sets V=1 on unordered, so `cset reg, VS` yields 1.
      if (!macho_emit_float_value_to_vreg(text, fun, value->left, 8, frame_size, ctx, diag)) return false;
      macho_emit_fcmp(text, 8, 8, macho_type_is_f64(value->left ? value->left->type : IR_TYPE_F32));
      macho_emit_cset(text, reg, 6); // VS (overflow set = unordered)
      return true;
    }
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported value kind %d", value ? (int)value->kind : -1);
      return macho_diag_at(diag, "direct AArch64 Mach-O value kind is unsupported", value->line, value->column, actual);
    }
  }
}

// Indexed load from a fixed [N]f32 / [N]f64 array local into `vreg`. Bounds-checked against the
// static array length (brk on out-of-range), then `ldr s_/d_, [base, index, lsl #log2]`.
static bool macho_emit_float_index_load(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (value->array_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O indexed load array is out of range", value->line, value->column, "invalid array local");
  const IrLocal *local = &fun->locals[value->array_index];
  if (local->type == IR_TYPE_BYTE_VIEW) {
    // Typed float-span element read: runtime bounds-check, then ldr s_/d_ at ptr+idx*size.
    if (!macho_type_is_float(local->element_type)) return macho_diag_at(diag, "direct AArch64 Mach-O float indexed load requires a float span", value->line, value->column, "unsupported span element");
    if (!macho_emit_span_index_addr(text, fun, value->array_index, value->index, local->element_type, frame_size, ctx, diag)) return false;
    macho_emit_ldr_v_base(text, vreg, 9, macho_type_is_f64(local->element_type));
    return true;
  }
  if (!local->is_array || !macho_type_is_float(local->element_type)) {
    return macho_diag_at(diag, "direct AArch64 Mach-O float indexed load requires a float array", value->line, value->column, "unsupported array local");
  }
  bool is64 = macho_type_is_f64(local->element_type);
  unsigned const_index = 0;
  if (macho_const_u32_value(value->index, &const_index) && const_index < local->array_len) {
    macho_emit_ldr_v_local(text, fun, vreg, value->array_index, const_index * (is64 ? 8u : 4u), frame_size, is64);
    return true;
  }
  if (!value->index || !macho_emit_value_to_reg(text, fun, value->index, 8, frame_size, ctx, diag)) return false;
  macho_emit_movz_w(text, 9, local->array_len);
  macho_emit_cmp_w(text, 8, 9);
  size_t ok_patch = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
  append_u32le(text, 0xd4200000u); // brk #0
  macho_patch_cond19(text, ok_patch, text->len);
  macho_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, value->array_index, 0, frame_size));
  macho_emit_ldr_v_reg_lsl(text, vreg, 9, 8, is64);
  return true;
}

// Indexed store of `vreg` into a fixed [N]f32 / [N]f64 array local. Same bounds check + scaled
// register offset as the float load.
static bool macho_emit_float_index_store(ZBuf *text, const IrFunction *fun, const IrInstr *instr, const IrLocal *local, unsigned vreg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  bool is64 = macho_type_is_f64(local->element_type);
  unsigned const_index = 0;
  if (macho_const_u32_value(instr->index, &const_index) && const_index < local->array_len) {
    macho_emit_str_v_local(text, fun, vreg, instr->array_index, const_index * (is64 ? 8u : 4u), frame_size, is64);
    return true;
  }
  if (!instr->index || !macho_emit_value_to_reg(text, fun, instr->index, 8, frame_size, ctx, diag)) return false;
  macho_emit_movz_w(text, 9, local->array_len);
  macho_emit_cmp_w(text, 8, 9);
  size_t ok_patch = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
  append_u32le(text, 0xd4200000u); // brk #0
  macho_patch_cond19(text, ok_patch, text->len);
  macho_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, instr->array_index, 0, frame_size));
  macho_emit_str_v_reg_lsl(text, vreg, 9, 8, is64);
  return true;
}

// FP scratch lives in fixed slots at the bottom of the frame (the locals sit above it via the
// enlarged frame size), one 16-byte slot per binary/compare nesting level. sp is constant for the
// whole body, so these offsets are stable; unlike a push/pop they do not disturb the sp-relative
// local addressing, and unlike a caller-saved register they survive a nested libm call.
static unsigned macho_fp_scratch_slot_offset(unsigned frame_size, unsigned depth) {
  (void)frame_size;
  return depth * 16u;
}

// Maximum FP binary/compare nesting depth in a value (1 for a leaf binary). Drives how many
// scratch slots the frame must reserve.
static unsigned macho_value_fp_depth(const IrValue *value) {
  if (!value) return 0;
  if (value->kind == IR_VALUE_BINARY && macho_type_is_float(value->type)) {
    unsigned l = macho_value_fp_depth(value->left);
    unsigned r = macho_value_fp_depth(value->right);
    return 1 + (l > r ? l : r);
  }
  if (value->kind == IR_VALUE_COMPARE && value->left && macho_type_is_float(value->left->type)) {
    unsigned l = macho_value_fp_depth(value->left);
    unsigned r = macho_value_fp_depth(value->right);
    return 1 + (l > r ? l : r);
  }
  unsigned best = 0;
  const IrValue *kids[3] = {value->left, value->right, value->index};
  for (unsigned i = 0; i < 3; i++) {
    unsigned d = macho_value_fp_depth(kids[i]);
    if (d > best) best = d;
  }
  for (size_t i = 0; i < value->arg_len; i++) {
    unsigned d = macho_value_fp_depth(value->args[i]);
    if (d > best) best = d;
  }
  return best;
}

static unsigned macho_instrs_fp_depth(const IrInstr *instrs, size_t len);

// Maximum FP scratch depth needed by a single instruction (including the values nested in its
// then/else/while bodies). The frame reserves this many 16-byte scratch slots.
static unsigned macho_instr_fp_depth(const IrInstr *instr) {
  if (!instr) return 0;
  unsigned best = macho_value_fp_depth(instr->value);
  unsigned t = macho_instrs_fp_depth(instr->then_instrs, instr->then_len);
  if (t > best) best = t;
  unsigned e = macho_instrs_fp_depth(instr->else_instrs, instr->else_len);
  if (e > best) best = e;
  return best;
}

static unsigned macho_instrs_fp_depth(const IrInstr *instrs, size_t len) {
  unsigned best = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned d = macho_instr_fp_depth(&instrs[i]);
    if (d > best) best = d;
  }
  return best;
}

// Total frame scratch bytes reserved for FP spills: one 16-byte slot per nesting level.
static unsigned macho_fp_scratch_bytes(const IrFunction *fun) {
  if (!fun) return 0;
  return macho_instrs_fp_depth(fun->instrs, fun->instr_len) * 16u;
}

static unsigned macho_instrs_int_depth(const IrInstr *instrs, size_t len);

// Maximum integer binary/compare nesting depth in a value (1 for a leaf binary). The integer binary
// and compare paths evaluate the left operand into a scratch register and then evaluate the right;
// because the right's evaluation can clobber that register (a span/array index uses x8, a call
// trashes the caller-saved set), the left value is spilled to a fixed frame slot across the right,
// exactly like the FP path. One 16-byte slot per nesting level. Short-circuit &&/|| do not spill
// (they evaluate into a single register without an outliving left), so they are not counted.
static unsigned macho_value_int_depth(const IrValue *value) {
  if (!value) return 0;
  // std.fs.mmap holds fd, size, and addr across the _open/_lseek/_mmap/_close calls; it needs two
  // 16-byte scratch slots (fd+size in one, addr in the other), so it contributes depth 2.
  if (value->kind == IR_VALUE_FS_MMAP) {
    unsigned l = macho_value_int_depth(value->left);
    unsigned r = macho_value_int_depth(value->right);
    return 2 + (l > r ? l : r);
  }
  bool spills = (value->kind == IR_VALUE_BINARY && value->binary_op != IR_BIN_AND && value->binary_op != IR_BIN_OR &&
                 !macho_type_is_float(value->type)) ||
                (value->kind == IR_VALUE_COMPARE && value->left && !macho_type_is_float(value->left->type)) ||
                value->kind == IR_VALUE_BYTE_VIEW_EQ || // bridges len/ptr sub-evaluations via one slot
                value->kind == IR_VALUE_BYTE_COPY ||    // bridges src/dst ptr+len sub-evaluations via one slot
                value->kind == IR_VALUE_BYTE_FILL ||    // bridges dst ptr+len sub-evaluations via one slot
                value->kind == IR_VALUE_ALLOC_BYTES; // page-alloc allocBytes spills the size across the _mmap call
  if (spills) {
    unsigned l = macho_value_int_depth(value->left);
    unsigned r = macho_value_int_depth(value->right);
    return 1 + (l > r ? l : r);
  }
  unsigned best = 0;
  const IrValue *kids[3] = {value->left, value->right, value->index};
  for (unsigned i = 0; i < 3; i++) {
    unsigned d = macho_value_int_depth(kids[i]);
    if (d > best) best = d;
  }
  for (size_t i = 0; i < value->arg_len; i++) {
    unsigned d = macho_value_int_depth(value->args[i]);
    if (d > best) best = d;
  }
  return best;
}

static unsigned macho_instr_int_depth(const IrInstr *instr) {
  if (!instr) return 0;
  unsigned best = macho_value_int_depth(instr->value);
  unsigned t = macho_instrs_int_depth(instr->then_instrs, instr->then_len);
  if (t > best) best = t;
  unsigned e = macho_instrs_int_depth(instr->else_instrs, instr->else_len);
  if (e > best) best = e;
  return best;
}

static unsigned macho_instrs_int_depth(const IrInstr *instrs, size_t len) {
  unsigned best = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned d = macho_instr_int_depth(&instrs[i]);
    if (d > best) best = d;
  }
  return best;
}

// Total frame scratch bytes reserved for integer binary/compare spills: one 16-byte slot per level.
static unsigned macho_int_scratch_bytes(const IrFunction *fun) {
  if (!fun) return 0;
  return macho_instrs_int_depth(fun->instrs, fun->instr_len) * 16u;
}

// Integer scratch slots sit just above the FP scratch region; sp is constant for the whole body so
// these offsets are stable. depth is the binary/compare nesting level (0 = outermost).
static unsigned macho_int_scratch_slot_offset(const IrFunction *fun, unsigned depth) {
  return macho_fp_scratch_bytes(fun) + depth * 16u;
}

// A record-returning function receives the destination address in x8 (the AAPCS indirect-result
// register) and must keep it live across the body — calls clobber x8 — so it is spilled to a
// reserved frame slot in the prologue and reloaded at each field store / return. The slot sits just
// above the FP and integer scratch regions; the frame is enlarged by 16 bytes (see
// macho_emit_function_text) so it never overlaps the locals (addressed from the top of the frame).
static bool macho_returns_record(const IrFunction *fun) {
  return fun && fun->return_type == IR_TYPE_RECORD;
}

static unsigned macho_sret_reserved_bytes(const IrFunction *fun) {
  return macho_returns_record(fun) ? 16u : 0u;
}

static unsigned macho_sret_slot_offset(const IrFunction *fun) {
  return macho_fp_scratch_bytes(fun) + macho_int_scratch_bytes(fun);
}

// Emit a floating-point value into NEON register `vreg` (s_ for f32, d_ for f64). Integer scratch
// lives in x8/x9 and never overlaps the FP file, so callers may freely interleave the two
// emitters. Single-arg libm calls take their argument in s0 and return in s0; powf takes s0/s1.
static bool macho_emit_float_value_to_vreg(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  return macho_emit_float_value_to_vreg_depth(text, fun, value, vreg, frame_size, 0, ctx, diag);
}

static bool macho_emit_float_value_to_vreg_depth(ZBuf *text, const IrFunction *fun, const IrValue *value, unsigned vreg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value) return macho_diag_at(diag, "direct AArch64 Mach-O float expression is missing", 1, 1, "missing expression");
  bool is64 = macho_type_is_f64(value->type);
  switch (value->kind) {
    case IR_VALUE_FLOAT: {
      // Materialize the IEEE bit pattern in an integer scratch register, then FMOV into the FP
      // register. f32 patterns fit in w8 (movz/movk); f64 patterns need the full x8 sequence.
      if (is64) {
        macho_emit_mov_imm64(text, 8, (uint64_t)value->int_value);
        macho_emit_fmov_v_from_gpr(text, vreg, 8, true);
      } else {
        macho_emit_movz_w(text, 8, (uint32_t)value->int_value);
        macho_emit_fmov_v_from_gpr(text, vreg, 8, false);
      }
      return true;
    }
    case IR_VALUE_LOCAL:
      if (value->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O local index is out of range", value->line, value->column, "invalid local");
      if (!macho_type_is_float(fun->locals[value->local_index].type)) {
        return macho_diag_at(diag, "direct AArch64 Mach-O float load requires a float local", value->line, value->column, "non-float local");
      }
      macho_emit_ldr_v_local(text, fun, vreg, value->local_index, 0, frame_size, is64);
      return true;
    case IR_VALUE_FIELD_LOAD:
      if (value->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O field load record is out of range", value->line, value->column, "invalid record local");
      if (!fun->locals[value->local_index].is_record) return macho_diag_at(diag, "direct AArch64 Mach-O field load requires record local", value->line, value->column, "non-record local");
      macho_emit_ldr_v_field(text, fun, vreg, value->local_index, value->field_offset, frame_size, is64);
      return true;
    case IR_VALUE_INDEX_LOAD:
      return macho_emit_float_index_load(text, fun, value, vreg, frame_size, ctx, diag);
    case IR_VALUE_CAST: {
      if (!value->left) return macho_diag_at(diag, "direct AArch64 Mach-O cast missing operand", value->line, value->column, "missing cast operand");
      IrTypeKind src = value->left->type;
      if (macho_type_is_float(src)) {
        // float -> float: widen/narrow when the widths differ, otherwise a plain load.
        if (!macho_emit_float_value_to_vreg_depth(text, fun, value->left, vreg, frame_size, depth, ctx, diag)) return false;
        if (macho_type_is_f64(src) != is64) macho_emit_fcvt(text, vreg, vreg, is64);
        return true;
      }
      // integer -> float: signed i32 via SCVTF from w_, unsigned u64 via UCVTF from x_.
      if (!macho_emit_value_to_reg(text, fun, value->left, 8, frame_size, ctx, diag)) return false;
      if (macho_type_is_scalar64(src) && macho_type_is_unsigned(src)) macho_emit_ucvtf_from_x(text, vreg, 8, is64);
      else macho_emit_scvtf_from_w(text, vreg, 8, is64);
      return true;
    }
    case IR_VALUE_BINARY: {
      if (value->binary_op != IR_BIN_ADD && value->binary_op != IR_BIN_SUB && value->binary_op != IR_BIN_MUL && value->binary_op != IR_BIN_DIV) {
        return macho_diag_at(diag, "direct AArch64 Mach-O float binary operator is unsupported", value->line, value->column, "unsupported operator");
      }
      // Evaluate the left operand, spill it to this depth's frame scratch slot, evaluate the
      // right (which may nest deeper, use the v8/v9 scratch, or even call libm — all of which the
      // frame slot survives), then reload and combine.
      unsigned slot = macho_fp_scratch_slot_offset(frame_size, depth);
      if (!macho_emit_float_value_to_vreg_depth(text, fun, value->left, 8, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_str_v_sp(text, 8, slot, is64);
      if (!macho_emit_float_value_to_vreg_depth(text, fun, value->right, 9, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_ldr_v_sp(text, 8, slot, is64);
      if (!macho_emit_float_arith(text, value->binary_op, vreg, 8, 9, is64)) {
        return macho_diag_at(diag, "direct AArch64 Mach-O float binary operator is unsupported", value->line, value->column, "unsupported operator");
      }
      return true;
    }
    case IR_VALUE_CALL:
      // A float-returning Zero function: argument marshaling and the v0 result move are handled
      // by macho_emit_call_to_reg, which keys the result bank off the value's float type. Thread the
      // FP nesting depth so a float argument that is itself a spilling binary uses a deeper scratch slot.
      return macho_emit_call_to_reg_depth(text, fun, value, vreg, frame_size, depth, ctx, diag);
    case IR_VALUE_CHECK: {
      // `check <float fallible call>`: the callee leaves its f32/f64 result in v0 and an error tag in
      // x1 (the macho fallible ABI). Evaluate into v0, test the tag, and on error propagate by
      // returning (the tag already in x1) from this raising context, mirroring ELF's check. The value
      // then sits in v0; move it to vreg if needed.
      if (!value->left) return macho_diag_at(diag, "direct AArch64 Mach-O check requires a fallible call result", value->line, value->column, "non-fallible value");
      if (!macho_emit_float_value_to_vreg_depth(text, fun, value->left, 0, frame_size, depth, ctx, diag)) return false;
      size_t ok = macho_emit_cbz_w_placeholder(text, 1); // x1 == 0 -> no error (tested via its low word)
      bool seed = ctx && ctx->seed_main_process_args && fun->is_exported && fun->name && strcmp(fun->name, "main") == 0;
      macho_emit_epilogue(text, frame_size, seed); // error: x1 already holds the tag; propagate it
      macho_patch_cond19(text, ok, text->len);
      if (vreg != 0) macho_emit_fmov_v(text, vreg, 0, is64);
      return true;
    }
    case IR_VALUE_MATH_SQRTF:
    case IR_VALUE_MATH_EXPF:
    case IR_VALUE_MATH_COSF:
    case IR_VALUE_MATH_SINF:
    case IR_VALUE_MATH_FABSF:
    case IR_VALUE_MATH_FLOORF: {
      // Single-arg libm call: argument in s0, result in s0 (AAPCS FP ABI). All std.math libm
      // helpers operate on f32.
      if (!macho_emit_float_value_to_vreg_depth(text, fun, value->left, 0, frame_size, depth, ctx, diag)) return false;
      size_t patch = macho_emit_bl_placeholder(text);
      if (!macho_record_math_call_patch(ctx, patch, macho_math_symbol_for_value(value->kind), value, diag)) return false;
      if (vreg != 0) macho_emit_fmov_v(text, vreg, 0, false);
      return true;
    }
    case IR_VALUE_MATH_POWF: {
      // Two-arg libm call: arg0 in s0, arg1 in s1. Compute arg0 into s0, spill it to the depth
      // scratch slot, compute arg1 into s1 (its evaluation may use the FP scratch or call libm),
      // then reload arg0 into s0 just before the call.
      unsigned slot = macho_fp_scratch_slot_offset(frame_size, depth);
      if (!macho_emit_float_value_to_vreg_depth(text, fun, value->left, 0, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_str_v_sp(text, 0, slot, false);
      if (!macho_emit_float_value_to_vreg_depth(text, fun, value->right, 1, frame_size, depth + 1, ctx, diag)) return false;
      macho_emit_ldr_v_sp(text, 0, slot, false);
      size_t patch = macho_emit_bl_placeholder(text);
      if (!macho_record_math_call_patch(ctx, patch, MACHO_MATH_POWF, value, diag)) return false;
      if (vreg != 0) macho_emit_fmov_v(text, vreg, 0, false);
      return true;
    }
    case IR_VALUE_BYTE_VIEW_READ_FLOAT_LE: {
      // std.codec.readF32Le / readF64Le: 4 or 8 little-endian bytes at a byte offset. AArch64 is
      // little-endian, so an FP load at ptr+offset yields the value; bounds-check offset+size first.
      if (!value->left) return macho_diag_at(diag, "direct AArch64 Mach-O readF*Le requires a byte view", value->line, value->column, "missing byte view");
      if (!value->index) return macho_diag_at(diag, "direct AArch64 Mach-O readF*Le requires an offset", value->line, value->column, "missing offset");
      unsigned scalar_size = is64 ? 8u : 4u;
      if (!macho_emit_value_to_reg(text, fun, value->index, 8, frame_size, ctx, diag)) return false;
      macho_emit_add_w_imm(text, 10, 8, scalar_size); // offset + size
      if (!macho_emit_byte_view_len(text, fun, value->left, 9, frame_size, ctx, diag)) return false;
      macho_emit_cmp_w(text, 10, 9);
      size_t ok_patch = macho_emit_b_cond_placeholder(text, 9); // unsigned lower or same
      append_u32le(text, 0xd4200000u); // brk #0
      macho_patch_cond19(text, ok_patch, text->len);
      if (!macho_emit_byte_view_ptr(text, fun, value->left, 9, frame_size, ctx, diag)) return false;
      macho_emit_add_x_reg(text, 9, 9, 8);
      macho_emit_ldr_v_base(text, vreg, 9, is64);
      return true;
    }
    default: {
      char actual[64];
      snprintf(actual, sizeof(actual), "unsupported float value kind %d", value ? (int)value->kind : -1);
      return macho_diag_at(diag, "direct AArch64 Mach-O float value kind is unsupported", value->line, value->column, actual);
    }
  }
}

static unsigned macho_frame_size(const IrFunction *fun) {
  return (unsigned)macho_align(fun ? (fun->frame_bytes ? fun->frame_bytes : fun->local_len * 8) : 0, 16);
}

static void macho_emit_epilogue(ZBuf *text, unsigned frame_size, bool restore_process_args) {
  if (frame_size > 0) macho_emit_add_sp_imm(text, 0x910003ffu, frame_size); // add sp, sp, #frame_size
  append_u32le(text, 0xa8c17bfdu); // ldp x29, x30, [sp], #16
  if (restore_process_args) append_u32le(text, 0xa8c157f4u); // ldp x20, x21, [sp], #16
  append_u32le(text, 0xd65f03c0u); // ret
}

// add reg, sp, #frame_offset(local) — the base address of an inline record/array local, into any
// GPR. Used to pass a record by pointer and to set up source/destination pointers for a record
// copy. Locals live at the top of the frame, addressed exactly as the load/store helpers do.
static void macho_emit_lea_local_addr(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned reg, unsigned frame_size) {
  macho_emit_add_x_sp_imm(text, reg, macho_local_slot_offset(fun, local_index, 0, frame_size));
}

// memcpy a record value between inline frame slots: copy the source local's bytes to a destination
// — another local's slot, or the caller's sret buffer when dest_index == UINT_MAX. x8/x9 are
// scratch; 8-byte chunks then a 4-byte tail (records are 8- or 4-aligned, so the tail is exact).
// Span fields ride along as raw 16 bytes (ptr then len), preserving the view.
static void macho_emit_record_copy_to(ZBuf *text, const IrFunction *fun, unsigned dest_index, unsigned src_index, unsigned frame_size) {
  unsigned size = src_index < fun->local_len ? fun->locals[src_index].byte_size : 0;
  if (dest_index == UINT_MAX) {
    macho_emit_ldr_x_imm(text, 8, 31, macho_sret_slot_offset(fun)); // x8 = caller's sret pointer
  } else {
    macho_emit_lea_local_addr(text, fun, dest_index, 8, frame_size); // x8 = &dest
  }
  macho_emit_lea_local_addr(text, fun, src_index, 9, frame_size); // x9 = &src
  unsigned k = 0;
  while (k + 8 <= size) {
    macho_emit_ldr_x_imm(text, 10, 9, k);
    macho_emit_str_x_disp(text, 10, 8, k);
    k += 8;
  }
  if (k + 4 <= size) {
    macho_emit_ldr_w_disp(text, 10, 9, k);
    macho_emit_str_w_disp(text, 10, 8, k);
  }
}

// Copy a record param (passed by pointer in ptr_reg = x0..x7) into its inline frame slot,
// preserving value semantics. x9/x10 are scratch; 8-byte chunks then a 4-byte tail.
static void macho_emit_copy_record_param(ZBuf *text, const IrFunction *fun, unsigned local_index, unsigned ptr_reg, unsigned frame_size) {
  unsigned size = local_index < fun->local_len ? fun->locals[local_index].byte_size : 0;
  macho_emit_lea_local_addr(text, fun, local_index, 9, frame_size); // x9 = &slot
  unsigned k = 0;
  while (k + 8 <= size) {
    macho_emit_ldr_x_imm(text, 10, ptr_reg, k);
    macho_emit_str_x_disp(text, 10, 9, k);
    k += 8;
  }
  if (k + 4 <= size) {
    macho_emit_ldr_w_disp(text, 10, ptr_reg, k);
    macho_emit_str_w_disp(text, 10, 9, k);
  }
}

// Marshal one call argument into its destination register bank. Records pass a pointer to the
// argument's slot (record args are pre-materialized into locals, so each is an addressable local);
// spans pass (ptr,len) in two consecutive int regs; scalars/floats go through the value emitters.
// `int_arg`/`fp_arg` are advanced past the registers consumed. Mirrors the AArch64 AAPCS the
// prologue spills from. x8/x9 scratch used inside the value emitters never overlaps an arg reg, so
// already-marshalled arguments survive.
static bool macho_emit_marshal_call_arg(ZBuf *text, const IrFunction *fun, const IrValue *arg, unsigned *int_arg, unsigned *fp_arg, unsigned frame_size, unsigned depth, MachOEmitContext *ctx, ZDiag *diag) {
  IrTypeKind atype = arg ? arg->type : IR_TYPE_I32;
  if (macho_type_is_float(atype)) {
    if (!macho_emit_float_value_to_vreg_depth(text, fun, arg, *fp_arg, frame_size, depth, ctx, diag)) return false;
    (*fp_arg)++;
    return true;
  }
  if (atype == IR_TYPE_BYTE_VIEW) {
    if (!macho_emit_byte_view_ptr_depth(text, fun, arg, *int_arg, frame_size, depth, ctx, diag)) return false;
    (*int_arg)++;
    if (!macho_emit_byte_view_len_depth(text, fun, arg, *int_arg, frame_size, depth, ctx, diag)) return false;
    (*int_arg)++;
    return true;
  }
  if (atype == IR_TYPE_RECORD) {
    if (!arg || arg->kind != IR_VALUE_LOCAL || arg->local_index >= fun->local_len) {
      return macho_diag_at(diag, "direct AArch64 Mach-O record argument here must be a record local (a record call/literal argument is only supported in straight-line statements, not loop conditions or short-circuit operands)", arg ? arg->line : 1, arg ? arg->column : 1, "non-local record arg");
    }
    macho_emit_lea_local_addr(text, fun, arg->local_index, *int_arg, frame_size);
    (*int_arg)++;
    return true;
  }
  if (!macho_emit_value_to_reg_depth(text, fun, arg, *int_arg, frame_size, depth, ctx, diag)) return false;
  (*int_arg)++;
  return true;
}

// A record-returning call written into a destination buffer: marshal the call's own arguments into
// x0.., then set the destination address in x8 (the AAPCS indirect-result register), then bl.
// `dest_local` >= 0 is a record local's slot (`let x = f()`); `dest_local` < 0 is the current
// function's own sret buffer (`return f()`), so the callee writes straight through to our caller's
// storage. x8 is set last because the argument emitters use it as scratch.
static bool macho_emit_record_call_with_dest(ZBuf *text, const IrFunction *fun, int dest_local, const IrValue *value, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (value->arg_len > 8) return macho_diag_at(diag, "direct AArch64 Mach-O call supports at most eight arguments", value->line, value->column, "too many arguments");
  unsigned int_arg = 0;
  unsigned fp_arg = 0;
  // Record calls are only emitted from straight-line statements (a record bind or return), never as a
  // nested operand, so their arguments evaluate at depth 0.
  for (size_t i = 0; i < value->arg_len; i++) {
    if (!macho_emit_marshal_call_arg(text, fun, value->args[i], &int_arg, &fp_arg, frame_size, 0, ctx, diag)) return false;
  }
  if (dest_local >= 0) macho_emit_lea_local_addr(text, fun, (unsigned)dest_local, 8, frame_size);
  else macho_emit_ldr_x_imm(text, 8, 31, macho_sret_slot_offset(fun)); // x8 = caller's sret pointer
  size_t patch = macho_emit_bl_placeholder(text);
  return macho_record_call_patch(ctx, patch, value->callee_index, value, diag);
}

// Store one field of the record being returned, written through the caller's sret pointer (saved in
// the frame). x8 is reloaded from the slot after each value is materialized so an intervening call
// cannot strand it. Span fields store ptr@offset and len@offset+8.
static bool macho_emit_sret_field_store(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  unsigned slot = macho_sret_slot_offset(fun);
  IrTypeKind vt = instr->value ? instr->value->type : IR_TYPE_I32;
  unsigned fo = instr->field_offset;
  if (vt == IR_TYPE_BYTE_VIEW) {
    if (instr->value && instr->value->kind == IR_VALUE_CALL) {
      // A span-returning call leaves ptr in x0, len in x1; capture both before reloading x8.
      if (!macho_emit_call_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
      macho_emit_mov_x(text, 9, 1); // preserve len; x1 is otherwise clobbered by the x8 reload path
      macho_emit_ldr_x_imm(text, 8, 31, slot);
      macho_emit_str_x_disp(text, 0, 8, fo);
      macho_emit_str_w_disp(text, 9, 8, fo + 8);
    } else {
      if (!macho_emit_byte_view_ptr(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
      macho_emit_ldr_x_imm(text, 8, 31, slot);
      macho_emit_str_x_disp(text, 9, 8, fo);
      if (!macho_emit_byte_view_len(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
      macho_emit_ldr_x_imm(text, 8, 31, slot);
      macho_emit_str_w_disp(text, 9, 8, fo + 8);
    }
    return true;
  }
  if (macho_type_is_float(vt)) {
    bool is64 = macho_type_is_f64(vt);
    if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
    macho_emit_ldr_x_imm(text, 8, 31, slot);
    macho_emit_str_v_disp(text, 9, 8, fo, is64);
    return true;
  }
  if (!macho_emit_value_to_reg(text, fun, instr->value, 9, frame_size, ctx, diag)) return false;
  macho_emit_ldr_x_imm(text, 8, 31, slot);
  if (vt == IR_TYPE_U8 || vt == IR_TYPE_I8 || vt == IR_TYPE_BOOL) macho_emit_str_b_disp(text, 9, 8, fo);
  else if (macho_type_is_scalar64(vt)) macho_emit_str_x_disp(text, 9, 8, fo);
  else macho_emit_str_w_disp(text, 9, 8, fo);
  return true;
}

static bool macho_emit_instrs(ZBuf *text, const IrFunction *fun, const IrInstr *instrs, size_t len, unsigned frame_size, bool restore_process_args, MachOEmitContext *ctx, ZDiag *diag);

static bool macho_emit_world_write(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!instr || !instr->value) return macho_diag_at(diag, "direct AArch64 Mach-O World write requires bytes", instr ? instr->line : 1, instr ? instr->column : 1, "missing byte view");
  if (!macho_emit_byte_view_ptr(text, fun, instr->value, 1, frame_size, ctx, diag)) return false;
  if (!macho_emit_byte_view_len(text, fun, instr->value, 2, frame_size, ctx, diag)) return false;
  macho_emit_movz_w(text, 0, instr->field_offset == 2 ? 2u : 1u);
  size_t patch = macho_emit_bl_placeholder(text);
  if (!macho_record_world_write_patch(ctx, patch, instr, diag)) return false;
  size_t ok_patch = macho_emit_cbz_w_placeholder(text, 0);
  append_u32le(text, 0xd4200000u); // brk #0 on runtime write failure
  macho_patch_cond19(text, ok_patch, text->len);
  return true;
}

static bool macho_emit_args_get_to_local(ZBuf *text, const IrFunction *fun, const IrValue *value, const IrLocal *local, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!value || !value->left) return macho_diag_at(diag, "direct AArch64 Mach-O std.args.get requires an index", value ? value->line : 1, value ? value->column : 1, "missing index");
  if (!macho_emit_value_to_reg(text, fun, value->left, 10, frame_size, ctx, diag)) return false;
  macho_emit_cmp_w(text, 10, 20);
  size_t in_range = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
  macho_emit_movz_w(text, 8, 0);
  macho_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);
  macho_emit_store_local_x(text, fun, 8, local->index, 8, frame_size);
  macho_emit_store_local_w(text, fun, 8, local->index, 16, frame_size);
  size_t end_patch = macho_emit_b_placeholder(text);
  macho_patch_cond19(text, in_range, text->len);

  macho_emit_add_x_reg_lsl(text, 12, 21, 10, 3);
  macho_emit_ldr_x_imm(text, 12, 12, 0);
  macho_emit_movz_w(text, 10, 0);
  size_t loop_start = text->len;
  macho_emit_add_x_reg(text, 13, 12, 10);
  macho_emit_ldrb_w(text, 14, 13);
  size_t done_patch = macho_emit_cbz_w_placeholder(text, 14);
  macho_emit_add_w_imm(text, 10, 10, 1);
  size_t loop_patch = macho_emit_b_placeholder(text);
  macho_patch_branch26(text, loop_patch, loop_start);
  macho_patch_cond19(text, done_patch, text->len);

  macho_emit_movz_w(text, 8, 1);
  macho_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);
  macho_emit_store_local_x(text, fun, 12, local->index, 8, frame_size);
  macho_emit_store_local_w(text, fun, 10, local->index, 16, frame_size);
  macho_patch_branch26(text, end_patch, text->len);
  return true;
}

// Open + size + read-only mmap a file by path, leaving the mapping address in x0 and its byte length
// in x1 (the Mach-O analog of ELF's elf_emit_mmap_file_addr_size, which lands addr/size in rax/rdx).
// On any failure x0 is negative (and never a valid mapping). The libSystem call sequence:
//   fd   = _open(path, O_RDONLY=0)        ; negative fd -> open failure (returned straight through)
//   size = _lseek(fd, 0, SEEK_END=2)      ; negative -> seek failure: close fd, return the negative
//   addr = _mmap(NULL, size, PROT_READ=1, MAP_PRIVATE=2, fd, 0)
//   _close(fd)                            ; the fd is no longer needed once the region is mapped
// The fd is closed on the mmap-success AND mmap-failure (MAP_FAILED) paths so no descriptor leaks
// across repeated maps. macho addresses locals via sp, so the values that must outlive a `bl` (fd,
// size, addr) are spilled to integer scratch slots rather than pushed: slot+0 holds fd, slot+8 holds
// size, and a second slot (slot2+0) holds addr across the close. The size is a 64-bit byte count
// (files may exceed 4 GiB), so it is stored and reloaded with the full-width x form. The caller
// reserves two scratch levels (FS_MMAP contributes depth 2 in macho_value_int_depth).
static bool macho_emit_mmap_file_addr_size(ZBuf *text, const IrFunction *fun, const IrValue *path, unsigned depth, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  unsigned slot = macho_int_scratch_slot_offset(fun, depth);
  unsigned slot2 = macho_int_scratch_slot_offset(fun, depth + 1);
  // _open(path, O_RDONLY)
  if (!macho_emit_byte_view_ptr_depth(text, fun, path, 0, frame_size, depth + 2, ctx, diag)) return false;
  macho_emit_movz_x(text, 1, 0); // O_RDONLY = 0
  size_t open_patch = macho_emit_bl_placeholder(text);
  if (!macho_record_libc_call_patch(ctx, open_patch, MACHO_LIBC_OPEN, path, diag)) return false;
  macho_emit_cmp_x(text, 0, 31);
  size_t open_fail = macho_emit_b_cond_placeholder(text, 11); // LT: negative fd -> open failure
  macho_emit_str_x_disp(text, 0, 31, slot); // save fd
  // _lseek(fd, 0, SEEK_END) -> file size
  macho_emit_movz_x(text, 1, 0); // offset = 0
  macho_emit_movz_x(text, 2, 2); // SEEK_END = 2
  size_t seek_patch = macho_emit_bl_placeholder(text);
  if (!macho_record_libc_call_patch(ctx, seek_patch, MACHO_LIBC_LSEEK, path, diag)) return false;
  macho_emit_cmp_x(text, 0, 31);
  size_t seek_fail = macho_emit_b_cond_placeholder(text, 11); // LT: lseek error
  macho_emit_str_x_disp(text, 0, 31, slot + 8); // save size (64-bit byte count)
  // _mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)
  macho_emit_movz_x(text, 0, 0);            // addr = NULL (kernel chooses)
  macho_emit_ldr_x_imm(text, 1, 31, slot + 8); // len = size
  macho_emit_movz_x(text, 2, 1);            // prot = PROT_READ
  macho_emit_movz_x(text, 3, 2);            // flags = MAP_PRIVATE
  macho_emit_ldr_x_imm(text, 4, 31, slot);  // fd
  macho_emit_movz_x(text, 5, 0);            // offset = 0
  size_t mmap_patch = macho_emit_bl_placeholder(text);
  if (!macho_record_libc_call_patch(ctx, mmap_patch, MACHO_LIBC_MMAP, path, diag)) return false;
  // Close the fd (no longer needed) on both the success and MAP_FAILED paths; preserve addr across
  // the close, then hand addr back in x0 and size in x1. A MAP_FAILED (negative) addr flows through.
  macho_emit_str_x_disp(text, 0, 31, slot2); // save addr
  macho_emit_ldr_x_imm(text, 0, 31, slot);   // x0 = fd
  size_t close_patch = macho_emit_bl_placeholder(text);
  if (!macho_record_libc_call_patch(ctx, close_patch, MACHO_LIBC_CLOSE, path, diag)) return false;
  macho_emit_ldr_x_imm(text, 0, 31, slot2);  // x0 = addr (result)
  macho_emit_ldr_x_imm(text, 1, 31, slot + 8); // x1 = size (result)
  size_t done = macho_emit_b_placeholder(text);
  // Seek failure: close the fd and return the negative lseek result in x0.
  macho_patch_cond19(text, seek_fail, text->len);
  macho_emit_str_x_disp(text, 0, 31, slot2); // preserve the negative result across the close
  macho_emit_ldr_x_imm(text, 0, 31, slot);   // x0 = fd
  size_t seek_fail_close = macho_emit_bl_placeholder(text);
  if (!macho_record_libc_call_patch(ctx, seek_fail_close, MACHO_LIBC_CLOSE, path, diag)) return false;
  macho_emit_ldr_x_imm(text, 0, 31, slot2);  // x0 = negative result
  // open_fail lands here with x0 already holding the negative open result.
  macho_patch_cond19(text, open_fail, text->len);
  macho_patch_branch26(text, done, text->len);
  return true;
}

// Anonymous std.mem.pageAlloc allocation: a fresh kernel-zeroed region via libSystem _mmap, the
// macOS analog of ELF's MAP_ANON mmap syscall (calloc semantics). The size value is evaluated, then
// spilled to the instruction's integer scratch slot so it survives the call (mmap clobbers x0..x7),
// the AAPCS integer arguments are set up, and `bl _mmap` is recorded as an external relocation. The
// result populates the Maybe<MutSpan<u8>> dest local: on success has=1@0, ptr@8, len@16; on failure
// (MAP_FAILED, i.e. a negative return) the Maybe is cleared. Darwin map flags differ from Linux:
// MAP_ANON=0x1000 (Linux 0x20) and MAP_PRIVATE=0x2, so flags=0x1002; PROT_READ|PROT_WRITE=0x3; fd=-1.
static bool macho_emit_anon_mmap_to_local(ZBuf *text, const IrFunction *fun, const IrValue *size, const IrLocal *local, unsigned frame_size, MachOEmitContext *ctx, ZDiag *diag) {
  if (!size) return macho_diag_at(diag, "direct AArch64 Mach-O page allocation requires a byte length", local ? local->line : 1, local ? local->column : 1, "missing length");
  unsigned slot = macho_int_scratch_slot_offset(fun, 0);
  if (!macho_emit_value_to_reg(text, fun, size, 9, frame_size, ctx, diag)) return false;
  macho_emit_str_x_disp(text, 9, 31, slot); // preserve the length across the call
  macho_emit_mov_x(text, 1, 9);             // x1 = len
  macho_emit_movz_x(text, 0, 0);            // x0 = addr (NULL: kernel chooses)
  macho_emit_movz_x(text, 2, 3);            // x2 = prot = PROT_READ|PROT_WRITE
  macho_emit_movz_x(text, 3, 0x1002);       // x3 = flags = MAP_ANON|MAP_PRIVATE (Darwin)
  macho_emit_movn_x(text, 4, 0);            // x4 = fd = -1
  macho_emit_movz_x(text, 5, 0);            // x5 = offset = 0
  size_t patch = macho_emit_bl_placeholder(text);
  if (!macho_record_libc_call_patch(ctx, patch, MACHO_LIBC_MMAP, size, diag)) return false;
  // mmap returns MAP_FAILED (-1) on error; a valid user-space address is never negative.
  macho_emit_cmp_x(text, 0, 31);            // compare result against xzr
  size_t fail = macho_emit_b_cond_placeholder(text, 11); // signed less than -> failure
  macho_emit_movz_w(text, 9, 1);
  macho_emit_store_local_w(text, fun, 9, local->index, 0, frame_size);   // has = 1
  macho_emit_store_local_x(text, fun, 0, local->index, 8, frame_size);   // ptr
  macho_emit_ldr_x_imm(text, 9, 31, slot);
  macho_emit_store_local_w(text, fun, 9, local->index, 16, frame_size);  // len (element count = bytes)
  size_t end = macho_emit_b_placeholder(text);
  macho_patch_cond19(text, fail, text->len);
  macho_emit_movz_w(text, 9, 0);
  macho_emit_store_local_w(text, fun, 9, local->index, 0, frame_size);   // has = 0
  macho_emit_store_local_x(text, fun, 9, local->index, 8, frame_size);
  macho_emit_store_local_w(text, fun, 9, local->index, 16, frame_size);
  macho_patch_branch26(text, end, text->len);
  return true;
}

static bool macho_emit_instr(ZBuf *text, const IrFunction *fun, const IrInstr *instr, unsigned frame_size, bool restore_process_args, MachOEmitContext *ctx, ZDiag *diag) {
  if (instr->kind == IR_INSTR_WORLD_WRITE) {
    return macho_emit_world_write(text, fun, instr, frame_size, ctx, diag);
  }
  if (instr->kind == IR_INSTR_LOCAL_SET) {
    if (instr->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O local store is out of range", instr->line, instr->column, "invalid local");
    if (fun->locals[instr->local_index].is_record) {
      const IrLocal *local = &fun->locals[instr->local_index];
      // `let q = f()` — bind a record-returning call straight into q's slot via sret (no copy).
      if (instr->value && instr->value->kind == IR_VALUE_CALL) {
        return macho_emit_record_call_with_dest(text, fun, (int)local->index, instr->value, frame_size, ctx, diag);
      }
      // `let q = p` / `q = p` — record-to-record value copy (span fields ride along as raw bytes).
      if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
        macho_emit_record_copy_to(text, fun, local->index, instr->value->local_index, frame_size);
        return true;
      }
      return macho_diag_at(diag, "direct AArch64 Mach-O record local assignment requires a record value", instr->line, instr->column, "unsupported record set");
    }
    if (fun->locals[instr->local_index].type == IR_TYPE_BYTE_VIEW) {
      const IrLocal *local = &fun->locals[instr->local_index];
      // `let m: owned<Mapping> = check std.fs.mmapOrRaise(fs, path)` -> unwrap into a Mapping local
      // (ptr@0, len@8). The helper lands addr in x0 and the 64-bit byte length in x1; a negative addr
      // is the raise/failure case. Mirroring the macho `check world.out.write` discipline (which traps
      // rather than threading an error-tag return — that machinery does not exist on this backend), a
      // failed mmap-or-raise traps with brk #0. The length is stored full-width (files may exceed 4
      // GiB); std.mem.len over the Mapping reads it back as a 64-bit count.
      if (instr->value && instr->value->kind == IR_VALUE_CHECK && instr->value->left && instr->value->left->kind == IR_VALUE_FS_MMAP) {
        if (!macho_emit_mmap_file_addr_size(text, fun, instr->value->left->left, 0, frame_size, ctx, diag)) return false;
        macho_emit_cmp_x(text, 0, 31);
        size_t ok = macho_emit_b_cond_placeholder(text, 10); // GE: addr >= 0 -> mapped
        append_u32le(text, 0xd4200000u); // brk #0 on a failed required mapping
        macho_patch_cond19(text, ok, text->len);
        macho_emit_store_local_x(text, fun, 0, local->index, 0, frame_size); // ptr
        macho_emit_store_local_x(text, fun, 1, local->index, 8, frame_size); // len (64-bit byte count)
        return true;
      }
      // A span-returning call leaves ptr in x0 and len in x1; store both straight into the slot.
      if (instr->value && instr->value->kind == IR_VALUE_CALL) {
        if (!macho_emit_call_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
        macho_emit_store_local_x(text, fun, 0, local->index, 0, frame_size);
        macho_emit_store_local_w(text, fun, 1, local->index, 8, frame_size);
        return true;
      }
      if (!macho_emit_byte_view_ptr(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
      if (!macho_emit_byte_view_len(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      // A Mapping target keeps its 64-bit byte length; ordinary span locals store a 32-bit count.
      if (local->is_mapping) macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
      else macho_emit_store_local_w(text, fun, 8, instr->local_index, 8, frame_size);
      return true;
    }
    if (fun->locals[instr->local_index].type == IR_TYPE_ALLOC) {
      if (instr->value && instr->value->kind == IR_VALUE_PAGE_ALLOC) {
        // A PageAlloc carries no pre-reserved buffer — each std.mem.allocBytes performs a fresh
        // anonymous _mmap (see macho_emit_anon_mmap_to_local). Zero the allocator slot so it holds
        // no stale pointer/length; mirrors the ELF64 page-alloc init.
        macho_emit_movz_x(text, 8, 0);
        macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
        macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
        return true;
      }
      if (!instr->value || instr->value->kind != IR_VALUE_FIXED_BUF_ALLOC) return macho_diag_at(diag, "direct AArch64 Mach-O FixedBufAlloc local requires std.mem.fixedBufAlloc", instr->line, instr->column, "unsupported allocator initializer");
      if (!macho_emit_byte_view_ptr(text, fun, instr->value->left, 8, frame_size, ctx, diag)) return false;
      macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
      if (!macho_emit_byte_view_len(text, fun, instr->value->left, 8, frame_size, ctx, diag)) return false;
      macho_emit_store_local_w(text, fun, 8, instr->local_index, 8, frame_size);
      macho_emit_movz_w(text, 8, 0);
      macho_emit_store_local_w(text, fun, 8, instr->local_index, 12, frame_size);
      return true;
    }
    if (fun->locals[instr->local_index].type == IR_TYPE_VEC) {
      if (!instr->value || instr->value->kind != IR_VALUE_VEC_INIT) return macho_diag_at(diag, "direct AArch64 Mach-O Vec local requires std.mem.vec", instr->line, instr->column, "unsupported Vec initializer");
      if (!macho_emit_byte_view_ptr(text, fun, instr->value->left, 8, frame_size, ctx, diag)) return false;
      macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
      macho_emit_movz_w(text, 8, 0);
      macho_emit_store_local_w(text, fun, 8, instr->local_index, 8, frame_size);
      if (!macho_emit_byte_view_len(text, fun, instr->value->left, 8, frame_size, ctx, diag)) return false;
      macho_emit_store_local_w(text, fun, 8, instr->local_index, 12, frame_size);
      return true;
    }
    if (fun->locals[instr->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) {
      if (instr->value && instr->value->kind == IR_VALUE_ARGS_GET) {
        return macho_emit_args_get_to_local(text, fun, instr->value, &fun->locals[instr->local_index], frame_size, ctx, diag);
      }
      if (instr->value && instr->value->kind == IR_VALUE_FS_MMAP) {
        // `let m = std.fs.mmap(fs, path)` -> Maybe<owned<Mapping>> (has@0, ptr@8, len@16). The helper
        // lands addr in x0 and the 64-bit byte length in x1; a negative addr is the not-found/failure
        // path, which clears the Maybe. The length is stored full-width (files may exceed 4 GiB).
        const IrLocal *local = &fun->locals[instr->local_index];
        if (!macho_emit_mmap_file_addr_size(text, fun, instr->value->left, 0, frame_size, ctx, diag)) return false;
        macho_emit_cmp_x(text, 0, 31);
        size_t fail = macho_emit_b_cond_placeholder(text, 11); // LT: MAP_FAILED / open failure
        macho_emit_mov_x(text, 9, 0); // preserve addr across the has store
        macho_emit_movz_w(text, 8, 1);
        macho_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);  // has = 1
        macho_emit_store_local_x(text, fun, 9, local->index, 8, frame_size);  // ptr
        macho_emit_store_local_x(text, fun, 1, local->index, 16, frame_size); // len (64-bit byte count)
        size_t end = macho_emit_b_placeholder(text);
        macho_patch_cond19(text, fail, text->len);
        macho_emit_movz_w(text, 8, 0);
        macho_emit_store_local_w(text, fun, 8, local->index, 0, frame_size);  // has = 0
        macho_emit_store_local_x(text, fun, 8, local->index, 8, frame_size);
        macho_emit_store_local_x(text, fun, 8, local->index, 16, frame_size);
        macho_patch_branch26(text, end, text->len);
        return true;
      }
      if (!instr->value || instr->value->kind != IR_VALUE_ALLOC_BYTES || instr->value->local_index >= fun->local_len || fun->locals[instr->value->local_index].type != IR_TYPE_ALLOC) return macho_diag_at(diag, "direct AArch64 Mach-O allocation source is invalid", instr->line, instr->column, "invalid allocation");
      if (fun->locals[instr->value->local_index].is_page_alloc) {
        // PageAlloc: each allocation is a fresh kernel-zeroed _mmap region (calloc semantics).
        return macho_emit_anon_mmap_to_local(text, fun, instr->value->left, &fun->locals[instr->local_index], frame_size, ctx, diag);
      }
      if (!macho_emit_value_to_reg(text, fun, instr->value->left, 10, frame_size, ctx, diag)) return false;
      macho_emit_load_local_w(text, fun, 8, instr->value->local_index, 12, frame_size);
      macho_emit_load_local_w(text, fun, 9, instr->value->local_index, 8, frame_size);
      macho_emit_add_w_imm(text, 11, 8, 0);
      macho_emit_binary_int(text, IR_BIN_ADD, 11, 11, 10, false, false);
      macho_emit_cmp_w(text, 11, 9);
      size_t ok_patch = macho_emit_b_cond_placeholder(text, 9); // unsigned lower or same
      macho_emit_movz_w(text, 8, 0);
      macho_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
      macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
      macho_emit_store_local_w(text, fun, 8, instr->local_index, 16, frame_size);
      size_t end_patch = macho_emit_b_placeholder(text);
      macho_patch_cond19(text, ok_patch, text->len);
      macho_emit_movz_w(text, 12, 1);
      macho_emit_store_local_w(text, fun, 12, instr->local_index, 0, frame_size);
      macho_emit_load_local_x(text, fun, 12, instr->value->local_index, 0, frame_size);
      macho_emit_add_x_reg(text, 12, 12, 8);
      macho_emit_store_local_x(text, fun, 12, instr->local_index, 8, frame_size);
      macho_emit_store_local_w(text, fun, 10, instr->local_index, 16, frame_size);
      macho_emit_store_local_w(text, fun, 11, instr->value->local_index, 12, frame_size);
      macho_patch_branch26(text, end_patch, text->len);
      return true;
    }
    if (fun->locals[instr->local_index].type == IR_TYPE_MAYBE_SCALAR) {
      if (!instr->value) return macho_diag_at(diag, "direct AArch64 Mach-O Maybe scalar initializer is missing", instr->line, instr->column, "missing maybe value");
      if (instr->value->kind == IR_VALUE_MAYBE_SCALAR_LITERAL) {
        macho_emit_movz_w(text, 8, instr->value->data_len ? 1u : 0u);
        macho_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
        // The payload is stored in the full 8-byte slot, so use the 64-bit immediate sequence to
        // avoid truncating a Maybe<u64>/Maybe<i64> value that exceeds 32 bits.
        macho_emit_mov_imm64(text, 8, (uint64_t)instr->value->int_value);
        macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
        return true;
      }
      if (instr->value->kind == IR_VALUE_JSON_PARSE_BYTES) {
        if (instr->value->local_index >= fun->local_len || fun->locals[instr->value->local_index].type != IR_TYPE_ALLOC) {
          return macho_diag_at(diag, "direct AArch64 Mach-O JSON parse allocator is invalid", instr->line, instr->column, "invalid allocator");
        }
        if (!macho_emit_value_to_reg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
        macho_emit_cmp_x(text, 8, 31);
        size_t fail = macho_emit_b_cond_placeholder(text, 11); // signed less than
        macho_emit_load_local_w(text, fun, 9, instr->value->local_index, 12, frame_size);
        macho_emit_mov_w(text, 10, 8);
        macho_emit_binary_int(text, IR_BIN_ADD, 11, 9, 10, false, false);
        macho_emit_load_local_w(text, fun, 12, instr->value->local_index, 8, frame_size);
        macho_emit_cmp_w(text, 11, 12);
        size_t overflow = macho_emit_b_cond_placeholder(text, 8); // unsigned higher
        macho_emit_movz_w(text, 9, 1);
        macho_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
        macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
        macho_emit_store_local_w(text, fun, 11, instr->value->local_index, 12, frame_size);
        size_t end = macho_emit_b_placeholder(text);
        macho_patch_cond19(text, fail, text->len);
        macho_patch_cond19(text, overflow, text->len);
        macho_emit_movz_w(text, 9, 0);
        macho_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
        macho_emit_store_local_x(text, fun, 9, instr->local_index, 8, frame_size);
        macho_patch_branch26(text, end, text->len);
        return true;
      }
      if (!macho_emit_value_to_reg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      macho_emit_cmp_x(text, 8, 31);
      size_t fail = macho_emit_b_cond_placeholder(text, 11); // signed less than
      macho_emit_movz_w(text, 9, 1);
      macho_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
      macho_emit_store_local_x(text, fun, 8, instr->local_index, 8, frame_size);
      size_t end = macho_emit_b_placeholder(text);
      macho_patch_cond19(text, fail, text->len);
      macho_emit_movz_w(text, 9, 0);
      macho_emit_store_local_w(text, fun, 9, instr->local_index, 0, frame_size);
      macho_emit_store_local_x(text, fun, 9, instr->local_index, 8, frame_size);
      macho_patch_branch26(text, end, text->len);
      return true;
    }
    if (macho_type_is_float(fun->locals[instr->local_index].type)) {
      bool is64 = macho_type_is_f64(fun->locals[instr->local_index].type);
      if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      macho_emit_str_v_local(text, fun, 8, instr->local_index, 0, frame_size, is64);
      return true;
    }
    if (!macho_emit_value_to_reg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
    if (macho_type_is_scalar64(fun->locals[instr->local_index].type)) macho_emit_store_local_x(text, fun, 8, instr->local_index, 0, frame_size);
    else macho_emit_store_local_w(text, fun, 8, instr->local_index, 0, frame_size);
    return true;
  }
  if (instr->kind == IR_INSTR_FIELD_STORE) {
    // local_index == UINT_MAX targets the record being returned, written through the saved sret
    // pointer (`return <shape literal>` stores its fields this way).
    if (instr->local_index == UINT_MAX) {
      return macho_emit_sret_field_store(text, fun, instr, frame_size, ctx, diag);
    }
    if (instr->local_index >= fun->local_len) return macho_diag_at(diag, "direct AArch64 Mach-O field store record is out of range", instr->line, instr->column, "invalid record local");
    if (!fun->locals[instr->local_index].is_record) return macho_diag_at(diag, "direct AArch64 Mach-O field store requires record local", instr->line, instr->column, "non-record local");
    if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
      // Span field: store ptr at the field offset and the 32-bit element-count len 8 bytes higher.
      // A span-returning call leaves ptr in x0 and len in x1; any other byte view is materialized
      // ptr-then-len.
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!macho_emit_call_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
        macho_emit_store_local_x(text, fun, 0, instr->local_index, instr->field_offset, frame_size);
        macho_emit_store_local_w(text, fun, 1, instr->local_index, instr->field_offset + 8, frame_size);
        return true;
      }
      if (!macho_emit_byte_view_ptr(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      macho_emit_store_local_x(text, fun, 8, instr->local_index, instr->field_offset, frame_size);
      if (!macho_emit_byte_view_len(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      macho_emit_store_local_w(text, fun, 8, instr->local_index, instr->field_offset + 8, frame_size);
      return true;
    }
    if (instr->value && macho_type_is_float(instr->value->type)) {
      bool is64 = macho_type_is_f64(instr->value->type);
      if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      macho_emit_str_v_field(text, fun, 8, instr->local_index, instr->field_offset, frame_size, is64);
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
      // Typed-span element write: bounds-check at runtime, then store by element width. u8/i8 store
      // the low byte (the in-register value is sign/zero-extended), wider ints via w/x, floats via
      // the FP file. The value is materialized before the address so the address scratch survives.
      if (macho_type_is_float(local->element_type)) {
        bool is64 = macho_type_is_f64(local->element_type);
        if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
        if (!macho_emit_span_index_addr(text, fun, instr->array_index, instr->index, local->element_type, frame_size, ctx, diag)) return false;
        macho_emit_str_v_base(text, 8, 9, is64);
        return true;
      }
      if (!macho_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
      if (!macho_emit_span_index_addr(text, fun, instr->array_index, instr->index, local->element_type, frame_size, ctx, diag)) return false;
      if (local->element_type == IR_TYPE_U8 || local->element_type == IR_TYPE_I8) macho_emit_strb_w(text, 10, 9);
      else if (macho_elem_byte_size(local->element_type) == 8) macho_emit_str_x(text, 10, 9);
      else macho_emit_str_w(text, 10, 9);
      return true;
    }
    if (local->is_array && macho_type_is_float(local->element_type)) {
      if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 8, frame_size, ctx, diag)) return false;
      return macho_emit_float_index_store(text, fun, instr, local, 8, frame_size, ctx, diag);
    }
    if (local->is_array && local->element_type != IR_TYPE_U8 && macho_const_u32_value(instr->index, &const_index) && const_index < local->array_len) {
      if (!macho_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
      macho_emit_store_local_w(text, fun, 10, instr->array_index, const_index * 4u, frame_size);
      return true;
    }
    if (local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE)) {
      if (!macho_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
      if (!instr->index || !macho_emit_value_to_reg(text, fun, instr->index, 8, frame_size, ctx, diag)) return false;
      macho_emit_movz_w(text, 9, local->array_len);
      macho_emit_cmp_w(text, 8, 9);
      size_t ok_patch = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
      append_u32le(text, 0xd4200000u); // brk #0
      macho_patch_cond19(text, ok_patch, text->len);
      macho_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, instr->array_index, 0, frame_size));
      macho_emit_add_x_reg_lsl(text, 9, 9, 8, 2);
      append_u32le(text, 0xb9000000u | (9u << 5) | (10u & 31u));
      return true;
    }
    if (!local->is_array || local->element_type != IR_TYPE_U8) return macho_diag_at(diag, "direct AArch64 Mach-O indexed store requires [N]u8 or integer arrays", instr->line, instr->column, "unsupported array local");
    if (!macho_emit_value_to_reg(text, fun, instr->value, 10, frame_size, ctx, diag)) return false;
    if (!instr->index || !macho_emit_value_to_reg(text, fun, instr->index, 8, frame_size, ctx, diag)) return false;
    macho_emit_movz_w(text, 9, local->array_len);
    macho_emit_cmp_w(text, 8, 9);
    size_t ok_patch = macho_emit_b_cond_placeholder(text, 3); // unsigned lower
    append_u32le(text, 0xd4200000u); // brk #0
    macho_patch_cond19(text, ok_patch, text->len);
    macho_emit_add_x_sp_imm(text, 9, macho_local_slot_offset(fun, instr->array_index, 0, frame_size));
    macho_emit_add_x_reg(text, 9, 9, 8);
    macho_emit_strb_w(text, 10, 9);
    return true;
  }
  if (instr->kind == IR_INSTR_EXPR) {
    if (!instr->value) return true;
    if (macho_type_is_float(instr->value->type)) return macho_emit_float_value_to_vreg(text, fun, instr->value, 0, frame_size, ctx, diag);
    return macho_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag);
  }
  if (instr->kind == IR_INSTR_RETURN) {
    if (fun->return_type == IR_TYPE_RECORD) {
      // `return f()` — the callee writes straight through our sret pointer (x8) and hands it back in
      // x0; nothing more to do. `return p` — copy a record local/param through the sret pointer. For
      // a `return <shape literal>` the fields were already stored through sret. Either way, leave the
      // sret pointer in x0 as the result (mirrors the AAPCS indirect-result convention).
      if (instr->value && instr->value->kind == IR_VALUE_CALL) {
        if (!macho_emit_record_call_with_dest(text, fun, -1, instr->value, frame_size, ctx, diag)) return false;
        macho_emit_epilogue(text, frame_size, restore_process_args);
        return true;
      }
      if (instr->value && instr->value->kind == IR_VALUE_LOCAL) {
        macho_emit_record_copy_to(text, fun, UINT_MAX, instr->value->local_index, frame_size);
      }
      macho_emit_ldr_x_imm(text, 0, 31, macho_sret_slot_offset(fun));
      macho_emit_epilogue(text, frame_size, restore_process_args);
      return true;
    }
    if (instr->value && instr->value->type == IR_TYPE_BYTE_VIEW) {
      // Span return: ptr in x0, len in x1. A span-returning call already lands both there; any other
      // byte view is materialized ptr-then-len. The length stays a 32-bit element count.
      if (instr->value->kind == IR_VALUE_CALL) {
        if (!macho_emit_call_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
      } else {
        if (!macho_emit_byte_view_ptr(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
        if (!macho_emit_byte_view_len(text, fun, instr->value, 1, frame_size, ctx, diag)) return false;
      }
      macho_emit_epilogue(text, frame_size, restore_process_args);
      return true;
    }
    if (instr->value) {
      // FP results return in v0 (s0/d0); integer results in x0/w0.
      if (macho_type_is_float(instr->value->type)) {
        if (!macho_emit_float_value_to_vreg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
      } else if (!macho_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) {
        return false;
      }
    }
    // A raising function returns its value alongside an error tag in x1 (the AAPCS analog of ELF's
    // rdx tag); a normal return reports "no error" with x1 = 0. The caller's `check` tests x1. Skip
    // for span returns (x1 carries the length there — raising span returns are not part of the ABI).
    if (fun->raises && (!instr->value || instr->value->type != IR_TYPE_BYTE_VIEW)) {
      macho_emit_movz_x(text, 1, 0);
    }
    macho_emit_epilogue(text, frame_size, restore_process_args);
    return true;
  }
  if (instr->kind == IR_INSTR_IF) {
    if (!macho_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
    size_t false_patch = macho_emit_cbz_w_placeholder(text, 0);
    if (!macho_emit_instrs(text, fun, instr->then_instrs, instr->then_len, frame_size, restore_process_args, ctx, diag)) return false;
    if (instr->else_len > 0) {
      size_t end_patch = macho_emit_b_placeholder(text);
      macho_patch_cond19(text, false_patch, text->len);
      if (!macho_emit_instrs(text, fun, instr->else_instrs, instr->else_len, frame_size, restore_process_args, ctx, diag)) return false;
      macho_patch_branch26(text, end_patch, text->len);
    } else {
      macho_patch_cond19(text, false_patch, text->len);
    }
    return true;
  }
  if (instr->kind == IR_INSTR_WHILE) {
    size_t loop_start = text->len;
    if (!macho_emit_value_to_reg(text, fun, instr->value, 0, frame_size, ctx, diag)) return false;
    size_t false_patch = macho_emit_cbz_w_placeholder(text, 0);
    if (!macho_emit_instrs(text, fun, instr->then_instrs, instr->then_len, frame_size, restore_process_args, ctx, diag)) return false;
    size_t loop_patch = macho_emit_b_placeholder(text);
    macho_patch_branch26(text, loop_patch, loop_start);
    macho_patch_cond19(text, false_patch, text->len);
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
  if (fun->param_count > 8) return macho_diag_at(diag, "direct AArch64 Mach-O object backend supports at most eight parameters", fun->line, fun->column, fun->name);
  // Returns: Void, primitive integer, float, a record (via the x8 sret pointer), or a span (ptr in
  // x0, len in x1). A raising function cannot return a record — the error tag would collide with the
  // sret/len register convention — but no consumer needs that, so it is rejected like ELF.
  if (fun->return_type == IR_TYPE_RECORD) {
    if (fun->raises) return macho_diag_at(diag, "direct AArch64 Mach-O object backend cannot return a record from a raising function", fun->line, fun->column, fun->name);
  } else if (fun->return_type != IR_TYPE_VOID && fun->return_type != IR_TYPE_BYTE_VIEW && !macho_type_is_scalar(fun->return_type) && !macho_type_is_float(fun->return_type)) {
    return macho_diag_at(diag, "direct AArch64 Mach-O object backend currently supports only Void, primitive integer, float, record, and span returns", fun->line, fun->column, fun->name);
  }
  for (size_t i = 0; i < fun->local_len; i++) {
    if (fun->locals[i].type == IR_TYPE_BYTE_VIEW) {
      // Span params arrive as (ptr,len) in two int regs and are spilled into the local's slot, after
      // which P3's typed-span indexing/len work unchanged.
      continue;
    }
    if (fun->locals[i].is_array && (fun->locals[i].element_type == IR_TYPE_U8 || fun->locals[i].element_type == IR_TYPE_U32 || fun->locals[i].element_type == IR_TYPE_I32 || fun->locals[i].element_type == IR_TYPE_USIZE || macho_type_is_float(fun->locals[i].element_type))) continue;
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
    macho_emit_aarch64_literal_return(text, literal);
    return true;
  }

  // Reserve FP scratch slots at the bottom of the frame, then integer binary/compare scratch slots,
  // then a 16-byte sret slot (for a record-returning function), then the locals at the top; the
  // single `frame_size` value keeps local addressing, the scratch slots, and the sret slot
  // consistent for the whole body and the matching epilogue.
  unsigned frame_size = macho_frame_size(fun) + macho_fp_scratch_bytes(fun) + macho_int_scratch_bytes(fun) + macho_sret_reserved_bytes(fun);
  bool seed_process_args = ctx && ctx->seed_main_process_args && fun->is_exported && fun->name && strcmp(fun->name, "main") == 0;
  if (seed_process_args) append_u32le(text, 0xa9bf57f4u); // stp x20, x21, [sp, #-16]!
  append_u32le(text, 0xa9bf7bfdu); // stp x29, x30, [sp, #-16]!
  append_u32le(text, 0x910003fdu); // mov x29, sp
  if (frame_size > 0) macho_emit_add_sp_imm(text, 0xd10003ffu, frame_size); // sub sp, sp, #frame_size
  if (seed_process_args) {
    macho_emit_mov_x(text, 20, 0);
    macho_emit_mov_x(text, 21, 1);
  }
  // A record-returning function gets the destination address in x8 (the AAPCS indirect-result
  // register, not an argument register). Save it so it survives the body's calls; field stores and
  // the return reload it from this slot.
  if (macho_returns_record(fun)) macho_emit_str_x_disp(text, 8, 31, macho_sret_slot_offset(fun));
  // AAPCS passes integer and FP parameters in separate register banks (x0..x7 vs v0..v7), so each
  // is consumed by its own counter. A span (byte-view) param arrives as (ptr,len) in two int regs;
  // a record param arrives as a pointer in one int reg and is copied into its inline slot for value
  // semantics. Spill each incoming argument register into its slot.
  unsigned int_arg = 0;
  unsigned fp_arg = 0;
  for (size_t i = 0; i < fun->param_count; i++) {
    IrTypeKind ptype = fun->locals[i].type;
    if (macho_type_is_float(ptype)) {
      macho_emit_str_v_local(text, fun, fp_arg, (unsigned)i, 0, frame_size, macho_type_is_f64(ptype));
      fp_arg++;
    } else if (ptype == IR_TYPE_BYTE_VIEW) {
      macho_emit_store_local_x(text, fun, int_arg, (unsigned)i, 0, frame_size); // ptr @ slot+0
      int_arg++;
      macho_emit_store_local_w(text, fun, int_arg, (unsigned)i, 8, frame_size); // len @ slot+8 (element count)
      int_arg++;
    } else if (ptype == IR_TYPE_RECORD) {
      macho_emit_copy_record_param(text, fun, (unsigned)i, int_arg, frame_size);
      int_arg++;
    } else if (macho_type_is_scalar64(ptype)) {
      macho_emit_store_local_x(text, fun, int_arg, (unsigned)i, 0, frame_size);
      int_arg++;
    } else {
      macho_emit_store_local_w(text, fun, int_arg, (unsigned)i, 0, frame_size);
      int_arg++;
    }
  }
  if (!macho_emit_instrs(text, fun, fun->instrs, fun->instr_len, frame_size, seed_process_args, ctx, diag)) return false;
  if (fun->instr_len == 0 || fun->instrs[fun->instr_len - 1].kind != IR_INSTR_RETURN) macho_emit_epilogue(text, frame_size, seed_process_args);
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

bool z_emit_macho64_object_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!program || !out) return macho_diag(diag, "direct Mach-O backend received no program");
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

  zbuf_init(out);

  ZBuf text;
  ZBuf rodata;
  ZBuf relocs;
  zbuf_init(&text);
  zbuf_init(&rodata);
  zbuf_init(&relocs);
  bool has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  unsigned rodata_base_offset = macho_rodata_base_offset(program);
  if (has_rodata) macho_append_rodata(&rodata, program, rodata_base_offset);
  size_t *offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  uint32_t *string_offsets = z_checked_calloc(program->function_len, sizeof(uint32_t));
  if (!offsets) {
    free(string_offsets);
    zbuf_free(&relocs);
    zbuf_free(&rodata);
    zbuf_free(&text);
    return macho_diag(diag, "out of memory while emitting Mach-O object");
  }
  if (!string_offsets) {
    free(offsets);
    zbuf_free(&relocs);
    zbuf_free(&rodata);
    zbuf_free(&text);
    return macho_diag(diag, "out of memory while emitting Mach-O symbols");
  }

  ZBuf strings;
  zbuf_init(&strings);
  append_u8(&strings, 0);
  MachOEmitContext ctx = {
    .program = program,
    .function_offsets = offsets,
    .function_count = program->function_len,
    .rodata_base_offset = rodata_base_offset,
    .pie_relative_data = true,
    .seed_main_process_args = true
  };
  for (size_t i = 0; i < program->function_len; i++) {
    const IrFunction *fun = &program->functions[i];
    macho_pad_to(&text, macho_align(text.len, 4));
    offsets[i] = text.len;
    if (!macho_emit_function_text(&text, fun, &ctx, diag)) {
      zbuf_free(&strings);
      free(string_offsets);
      free(offsets);
      free(ctx.math_call_patches);
      free(ctx.runtime_json_parse_bytes_patches);
      free(ctx.runtime_http_fetch_patches);
      free(ctx.runtime_http_result_ok_patches);
      free(ctx.runtime_http_result_status_patches);
      free(ctx.runtime_http_result_body_len_patches);
      free(ctx.runtime_http_result_error_patches);
      free(ctx.runtime_http_response_len_patches);
      free(ctx.runtime_http_response_headers_len_patches);
      free(ctx.runtime_http_response_body_offset_patches);
      free(ctx.runtime_http_header_value_patches);
      free(ctx.runtime_http_header_found_patches);
      free(ctx.runtime_http_header_offset_patches);
      free(ctx.runtime_http_header_len_patches);
      free(ctx.world_write_patches);
      free(ctx.data_patches);
      free(ctx.call_patches);
      zbuf_free(&relocs);
      zbuf_free(&rodata);
      zbuf_free(&text);
      return false;
    }
    string_offsets[i] = (uint32_t)strings.len;
    zbuf_append_char(&strings, '_');
    zbuf_append(&strings, fun->name ? fun->name : "zero_fn");
    append_u8(&strings, 0);
  }
  macho_append_call_relocations(&relocs, &ctx);
  if (has_rodata) {
    macho_append_data_relocations(&relocs, &ctx, (unsigned)program->function_len);
  }
  const bool has_world_write = ctx.world_write_patch_len > 0;
  const bool has_runtime_json_parse_bytes = ctx.runtime_json_parse_bytes_patch_len > 0;
  const bool has_runtime_http_fetch = ctx.runtime_http_fetch_patch_len > 0;
  const bool has_runtime_http_result_ok = ctx.runtime_http_result_ok_patch_len > 0;
  const bool has_runtime_http_result_status = ctx.runtime_http_result_status_patch_len > 0;
  const bool has_runtime_http_result_body_len = ctx.runtime_http_result_body_len_patch_len > 0;
  const bool has_runtime_http_result_error = ctx.runtime_http_result_error_patch_len > 0;
  const bool has_runtime_http_response_len = ctx.runtime_http_response_len_patch_len > 0;
  const bool has_runtime_http_response_headers_len = ctx.runtime_http_response_headers_len_patch_len > 0;
  const bool has_runtime_http_response_body_offset = ctx.runtime_http_response_body_offset_patch_len > 0;
  const bool has_runtime_http_header_value = ctx.runtime_http_header_value_patch_len > 0;
  const bool has_runtime_http_header_found = ctx.runtime_http_header_found_patch_len > 0;
  const bool has_runtime_http_header_offset = ctx.runtime_http_header_offset_patch_len > 0;
  const bool has_runtime_http_header_len = ctx.runtime_http_header_len_patch_len > 0;
  uint32_t next_runtime_symbol = (uint32_t)program->function_len + (has_rodata ? 1u : 0u);
  uint32_t world_write_symbol_index = 0;
  uint32_t runtime_json_parse_bytes_symbol_index = 0;
  uint32_t runtime_http_fetch_symbol_index = 0;
  uint32_t runtime_http_result_ok_symbol_index = 0;
  uint32_t runtime_http_result_status_symbol_index = 0;
  uint32_t runtime_http_result_body_len_symbol_index = 0;
  uint32_t runtime_http_result_error_symbol_index = 0;
  uint32_t runtime_http_response_len_symbol_index = 0;
  uint32_t runtime_http_response_headers_len_symbol_index = 0;
  uint32_t runtime_http_response_body_offset_symbol_index = 0;
  uint32_t runtime_http_header_value_symbol_index = 0;
  uint32_t runtime_http_header_found_symbol_index = 0;
  uint32_t runtime_http_header_offset_symbol_index = 0;
  uint32_t runtime_http_header_len_symbol_index = 0;
  if (has_world_write) world_write_symbol_index = next_runtime_symbol++;
  if (has_runtime_json_parse_bytes) runtime_json_parse_bytes_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_fetch) runtime_http_fetch_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_result_ok) runtime_http_result_ok_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_result_status) runtime_http_result_status_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_result_body_len) runtime_http_result_body_len_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_result_error) runtime_http_result_error_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_response_len) runtime_http_response_len_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_response_headers_len) runtime_http_response_headers_len_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_response_body_offset) runtime_http_response_body_offset_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_header_value) runtime_http_header_value_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_header_found) runtime_http_header_found_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_header_offset) runtime_http_header_offset_symbol_index = next_runtime_symbol++;
  if (has_runtime_http_header_len) runtime_http_header_len_symbol_index = next_runtime_symbol++;
  uint32_t math_symbol_index[MACHO_MATH_SYMBOL_COUNT] = {0};
  unsigned math_symbol_count = 0;
  for (unsigned m = 0; m < MACHO_MATH_SYMBOL_COUNT; m++) {
    if (macho_math_symbol_used(&ctx, (MachOMathSymbol)m)) {
      math_symbol_index[m] = next_runtime_symbol++;
      math_symbol_count++;
    }
  }
  uint32_t libc_symbol_index[MACHO_LIBC_SYMBOL_COUNT] = {0};
  unsigned libc_symbol_count = 0;
  for (unsigned l = 0; l < MACHO_LIBC_SYMBOL_COUNT; l++) {
    if (macho_libc_symbol_used(&ctx, (MachOLibcSymbol)l)) {
      libc_symbol_index[l] = next_runtime_symbol++;
      libc_symbol_count++;
    }
  }
  if (has_world_write) {
    macho_append_world_write_relocations(&relocs, &ctx, world_write_symbol_index);
  }
  if (has_runtime_json_parse_bytes) {
    macho_append_runtime_json_parse_bytes_relocations(&relocs, &ctx, runtime_json_parse_bytes_symbol_index);
  }
  if (has_runtime_http_fetch) {
    macho_append_runtime_http_fetch_relocations(&relocs, &ctx, runtime_http_fetch_symbol_index);
  }
  if (has_runtime_http_result_ok) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_result_ok_patches, ctx.runtime_http_result_ok_patch_len, runtime_http_result_ok_symbol_index);
  }
  if (has_runtime_http_result_status) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_result_status_patches, ctx.runtime_http_result_status_patch_len, runtime_http_result_status_symbol_index);
  }
  if (has_runtime_http_result_body_len) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_result_body_len_patches, ctx.runtime_http_result_body_len_patch_len, runtime_http_result_body_len_symbol_index);
  }
  if (has_runtime_http_result_error) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_result_error_patches, ctx.runtime_http_result_error_patch_len, runtime_http_result_error_symbol_index);
  }
  if (has_runtime_http_response_len) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_response_len_patches, ctx.runtime_http_response_len_patch_len, runtime_http_response_len_symbol_index);
  }
  if (has_runtime_http_response_headers_len) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_response_headers_len_patches, ctx.runtime_http_response_headers_len_patch_len, runtime_http_response_headers_len_symbol_index);
  }
  if (has_runtime_http_response_body_offset) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_response_body_offset_patches, ctx.runtime_http_response_body_offset_patch_len, runtime_http_response_body_offset_symbol_index);
  }
  if (has_runtime_http_header_value) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_header_value_patches, ctx.runtime_http_header_value_patch_len, runtime_http_header_value_symbol_index);
  }
  if (has_runtime_http_header_found) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_header_found_patches, ctx.runtime_http_header_found_patch_len, runtime_http_header_found_symbol_index);
  }
  if (has_runtime_http_header_offset) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_header_offset_patches, ctx.runtime_http_header_offset_patch_len, runtime_http_header_offset_symbol_index);
  }
  if (has_runtime_http_header_len) {
    macho_append_runtime_http_result_relocations(&relocs, ctx.runtime_http_header_len_patches, ctx.runtime_http_header_len_patch_len, runtime_http_header_len_symbol_index);
  }
  for (unsigned m = 0; m < MACHO_MATH_SYMBOL_COUNT; m++) {
    if (macho_math_symbol_used(&ctx, (MachOMathSymbol)m)) {
      macho_append_math_call_relocations(&relocs, &ctx, (MachOMathSymbol)m, math_symbol_index[m]);
    }
  }
  for (unsigned l = 0; l < MACHO_LIBC_SYMBOL_COUNT; l++) {
    if (macho_libc_symbol_used(&ctx, (MachOLibcSymbol)l)) {
      macho_append_libc_call_relocations(&relocs, &ctx, (MachOLibcSymbol)l, libc_symbol_index[l]);
    }
  }

  const uint32_t header_size = 32;
  const uint32_t section_count = has_rodata ? 2u : 1u;
  const uint32_t segment_cmd_size = 72 + section_count * 80;
  const uint32_t symtab_cmd_size = 24;
  const uint32_t sizeofcmds = segment_cmd_size + symtab_cmd_size;
  const uint32_t text_offset = header_size + sizeofcmds;
  const uint32_t const_addr = has_rodata ? (uint32_t)macho_align(text.len, 8) : 0;
  const uint32_t segment_file_size = has_rodata ? const_addr + (uint32_t)rodata.len : (uint32_t)text.len;
  const uint32_t reloff = relocs.len > 0 ? text_offset + segment_file_size : 0;
  const uint32_t symoff = text_offset + segment_file_size + (uint32_t)relocs.len;
  const uint32_t nsyms = (uint32_t)program->function_len + (has_rodata ? 1u : 0u) + (has_world_write ? 1u : 0u) + (has_runtime_json_parse_bytes ? 1u : 0u) + (has_runtime_http_fetch ? 1u : 0u) + (has_runtime_http_result_ok ? 1u : 0u) + (has_runtime_http_result_status ? 1u : 0u) + (has_runtime_http_result_body_len ? 1u : 0u) + (has_runtime_http_result_error ? 1u : 0u) + (has_runtime_http_response_len ? 1u : 0u) + (has_runtime_http_response_headers_len ? 1u : 0u) + (has_runtime_http_response_body_offset ? 1u : 0u) + (has_runtime_http_header_value ? 1u : 0u) + (has_runtime_http_header_found ? 1u : 0u) + (has_runtime_http_header_offset ? 1u : 0u) + (has_runtime_http_header_len ? 1u : 0u) + math_symbol_count + libc_symbol_count;
  const uint32_t stroff = symoff + nsyms * 16;
  uint32_t rodata_string_offset = 0;
  if (has_rodata) {
    rodata_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "l_.zero_rodata");
    append_u8(&strings, 0);
  }
  uint32_t world_write_string_offset = 0;
  if (has_world_write) {
    world_write_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_world_write");
    append_u8(&strings, 0);
  }
  uint32_t runtime_json_parse_bytes_string_offset = 0;
  if (has_runtime_json_parse_bytes) {
    runtime_json_parse_bytes_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_json_parse_bytes");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_fetch_string_offset = 0;
  if (has_runtime_http_fetch) {
    runtime_http_fetch_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_fetch_result");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_result_ok_string_offset = 0;
  if (has_runtime_http_result_ok) {
    runtime_http_result_ok_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_result_ok");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_result_status_string_offset = 0;
  if (has_runtime_http_result_status) {
    runtime_http_result_status_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_result_status");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_result_body_len_string_offset = 0;
  if (has_runtime_http_result_body_len) {
    runtime_http_result_body_len_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_result_body_len");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_result_error_string_offset = 0;
  if (has_runtime_http_result_error) {
    runtime_http_result_error_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_result_error");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_response_len_string_offset = 0;
  if (has_runtime_http_response_len) {
    runtime_http_response_len_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_response_len");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_response_headers_len_string_offset = 0;
  if (has_runtime_http_response_headers_len) {
    runtime_http_response_headers_len_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_response_headers_len");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_response_body_offset_string_offset = 0;
  if (has_runtime_http_response_body_offset) {
    runtime_http_response_body_offset_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_response_body_offset");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_header_value_string_offset = 0;
  if (has_runtime_http_header_value) {
    runtime_http_header_value_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_header_value");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_header_found_string_offset = 0;
  if (has_runtime_http_header_found) {
    runtime_http_header_found_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_header_found");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_header_offset_string_offset = 0;
  if (has_runtime_http_header_offset) {
    runtime_http_header_offset_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_header_offset");
    append_u8(&strings, 0);
  }
  uint32_t runtime_http_header_len_string_offset = 0;
  if (has_runtime_http_header_len) {
    runtime_http_header_len_string_offset = (uint32_t)strings.len;
    zbuf_append(&strings, "_zero_http_header_len");
    append_u8(&strings, 0);
  }
  uint32_t math_string_offset[MACHO_MATH_SYMBOL_COUNT] = {0};
  for (unsigned m = 0; m < MACHO_MATH_SYMBOL_COUNT; m++) {
    if (macho_math_symbol_used(&ctx, (MachOMathSymbol)m)) {
      math_string_offset[m] = (uint32_t)strings.len;
      zbuf_append(&strings, macho_math_symbol_names[m]);
      append_u8(&strings, 0);
    }
  }
  uint32_t libc_string_offset[MACHO_LIBC_SYMBOL_COUNT] = {0};
  for (unsigned l = 0; l < MACHO_LIBC_SYMBOL_COUNT; l++) {
    if (macho_libc_symbol_used(&ctx, (MachOLibcSymbol)l)) {
      libc_string_offset[l] = (uint32_t)strings.len;
      zbuf_append(&strings, macho_libc_symbol_names[l]);
      append_u8(&strings, 0);
    }
  }

  append_u32le(out, 0xfeedfacfu);      // MH_MAGIC_64
  append_u32le(out, 0x0100000cu);      // CPU_TYPE_ARM64
  append_u32le(out, 0);                // CPU_SUBTYPE_ARM64_ALL
  append_u32le(out, 1);                // MH_OBJECT
  append_u32le(out, 2);                // ncmds
  append_u32le(out, sizeofcmds);
  append_u32le(out, 0);                // flags
  append_u32le(out, 0);                // reserved

  append_u32le(out, 0x19);             // LC_SEGMENT_64
  append_u32le(out, segment_cmd_size);
  append_fixed(out, "", 16);
  append_u64le(out, 0);
  append_u64le(out, segment_file_size);
  append_u64le(out, text_offset);
  append_u64le(out, segment_file_size);
  append_u32le(out, 7);
  append_u32le(out, 5);
  append_u32le(out, section_count);
  append_u32le(out, 0);

  append_fixed(out, "__text", 16);
  append_fixed(out, "__TEXT", 16);
  append_u64le(out, 0);
  append_u64le(out, text.len);
  append_u32le(out, text_offset);
  append_u32le(out, 2);
  append_u32le(out, reloff);
  append_u32le(out, (uint32_t)(ctx.call_patch_len + macho_data_relocation_count(&ctx) + ctx.world_write_patch_len + ctx.runtime_json_parse_bytes_patch_len + ctx.runtime_http_fetch_patch_len + ctx.runtime_http_result_ok_patch_len + ctx.runtime_http_result_status_patch_len + ctx.runtime_http_result_body_len_patch_len + ctx.runtime_http_result_error_patch_len + ctx.runtime_http_response_len_patch_len + ctx.runtime_http_response_headers_len_patch_len + ctx.runtime_http_response_body_offset_patch_len + ctx.runtime_http_header_value_patch_len + ctx.runtime_http_header_found_patch_len + ctx.runtime_http_header_offset_patch_len + ctx.runtime_http_header_len_patch_len + ctx.math_call_patch_len + ctx.libc_call_patch_len));
  append_u32le(out, 0x80000400u);
  append_u32le(out, 0);
  append_u32le(out, 0);
  append_u32le(out, 0);

  if (has_rodata) {
    append_fixed(out, "__const", 16);
    append_fixed(out, "__DATA", 16);
    append_u64le(out, const_addr);
    append_u64le(out, rodata.len);
    append_u32le(out, text_offset + const_addr);
    append_u32le(out, 3);
    append_u32le(out, 0);
    append_u32le(out, 0);
    append_u32le(out, 0);
    append_u32le(out, 0);
    append_u32le(out, 0);
    append_u32le(out, 0);
  }

  append_u32le(out, 0x2);              // LC_SYMTAB
  append_u32le(out, symtab_cmd_size);
  append_u32le(out, symoff);
  append_u32le(out, nsyms);
  append_u32le(out, stroff);
  append_u32le(out, (uint32_t)strings.len);

  if (text.data) append_bytes(out, text.data, text.len);
  if (has_rodata) {
    macho_pad_to(out, text_offset + const_addr);
    if (rodata.data) append_bytes(out, rodata.data, rodata.len);
  }
  if (relocs.data) append_bytes(out, relocs.data, relocs.len);
  for (size_t i = 0; i < program->function_len; i++) {
    append_u32le(out, string_offsets[i]);
    append_u8(out, program->functions[i].is_exported ? 0x0f : 0x0e); // N_EXT | N_SECT or local N_SECT
    append_u8(out, 1);
    append_u16le(out, 0);
    append_u64le(out, offsets[i]);
  }
  if (has_rodata) {
    append_u32le(out, rodata_string_offset);
    append_u8(out, 0x0e); // local N_SECT
    append_u8(out, 2);
    append_u16le(out, 0);
    append_u64le(out, const_addr);
  }
  if (has_world_write) {
    append_u32le(out, world_write_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_json_parse_bytes) {
    append_u32le(out, runtime_json_parse_bytes_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_fetch) {
    append_u32le(out, runtime_http_fetch_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_result_ok) {
    append_u32le(out, runtime_http_result_ok_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_result_status) {
    append_u32le(out, runtime_http_result_status_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_result_body_len) {
    append_u32le(out, runtime_http_result_body_len_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_result_error) {
    append_u32le(out, runtime_http_result_error_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_response_len) {
    append_u32le(out, runtime_http_response_len_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_response_headers_len) {
    append_u32le(out, runtime_http_response_headers_len_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_response_body_offset) {
    append_u32le(out, runtime_http_response_body_offset_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_header_value) {
    append_u32le(out, runtime_http_header_value_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_header_found) {
    append_u32le(out, runtime_http_header_found_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_header_offset) {
    append_u32le(out, runtime_http_header_offset_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  if (has_runtime_http_header_len) {
    append_u32le(out, runtime_http_header_len_string_offset);
    append_u8(out, 0x01); // N_EXT undefined external
    append_u8(out, 0);
    append_u16le(out, 0);
    append_u64le(out, 0);
  }
  // libm externals (sqrtf/expf/...), emitted in symbol-index order to match the relocations.
  for (unsigned m = 0; m < MACHO_MATH_SYMBOL_COUNT; m++) {
    if (macho_math_symbol_used(&ctx, (MachOMathSymbol)m)) {
      append_u32le(out, math_string_offset[m]);
      append_u8(out, 0x01); // N_EXT undefined external
      append_u8(out, 0);
      append_u16le(out, 0);
      append_u64le(out, 0);
    }
  }
  // libSystem externals (_mmap, ...), emitted in symbol-index order to match the relocations.
  for (unsigned l = 0; l < MACHO_LIBC_SYMBOL_COUNT; l++) {
    if (macho_libc_symbol_used(&ctx, (MachOLibcSymbol)l)) {
      append_u32le(out, libc_string_offset[l]);
      append_u8(out, 0x01); // N_EXT undefined external
      append_u8(out, 0);
      append_u16le(out, 0);
      append_u64le(out, 0);
    }
  }
  if (strings.data) append_bytes(out, strings.data, strings.len);

  free(ctx.libc_call_patches);
  free(ctx.math_call_patches);
  free(ctx.runtime_json_parse_bytes_patches);
  free(ctx.runtime_http_fetch_patches);
  free(ctx.runtime_http_result_ok_patches);
  free(ctx.runtime_http_result_status_patches);
  free(ctx.runtime_http_result_body_len_patches);
  free(ctx.runtime_http_result_error_patches);
  free(ctx.runtime_http_response_len_patches);
  free(ctx.runtime_http_response_headers_len_patches);
  free(ctx.runtime_http_response_body_offset_patches);
  free(ctx.runtime_http_header_value_patches);
  free(ctx.runtime_http_header_found_patches);
  free(ctx.runtime_http_header_offset_patches);
  free(ctx.runtime_http_header_len_patches);
  free(ctx.world_write_patches);
  free(ctx.data_patches);
  free(ctx.call_patches);
  free(string_offsets);
  free(offsets);
  zbuf_free(&strings);
  zbuf_free(&relocs);
  zbuf_free(&rodata);
  zbuf_free(&text);
  return true;
}

static const IrFunction *macho_find_executable_main(const IrProgram *program, ZDiag *diag, unsigned *out_index) {
  const IrFunction *fun = NULL;
  unsigned index = 0;
  for (size_t i = 0; program && i < program->function_len; i++) {
    if (program->functions[i].is_exported && strcmp(program->functions[i].name, "main") == 0) {
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
    macho_diag_at(diag, "direct AArch64 Mach-O executable main must return Void, i32, or u32", fun->line, fun->column, fun->name);
    return NULL;
  }
  if (out_index) *out_index = index;
  return fun;
}

static size_t macho_emit_exe_start_stub(ZBuf *text) {
  macho_emit_mov_x(text, 20, 0);
  macho_emit_mov_x(text, 21, 1);
  size_t patch = macho_emit_b_placeholder(text); // tail-call main so it returns to dyld's LC_MAIN trampoline
  return patch;
}

static size_t macho_emit_exe_world_write(ZBuf *text) {
  size_t offset = text->len;
  macho_emit_movz_x(text, 16, 0x02000004u); // Darwin SYS_write(fd=x0, buf=x1, len=x2)
  append_u32le(text, 0xd4001001u); // svc #0x80
  macho_emit_movz_w(text, 0, 0);   // report success to the checked std.io shim
  append_u32le(text, 0xd65f03c0u); // ret
  return offset;
}

bool z_emit_macho64_exe_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!program || !out) return macho_diag(diag, "direct Mach-O executable backend received no program");
  if (!program->mir_valid) {
    bool ok = macho_diag_at(diag, program->mir_message[0] ? program->mir_message : "direct backend lowering failed", program->mir_line, program->mir_column, program->mir_actual);
    z_diag_set_backend_blocker(diag, &program->backend_blocker);
    return ok;
  }
  unsigned main_index = 0;
  if (!macho_find_executable_main(program, diag, &main_index)) return false;
  for (size_t i = 0; i < program->function_len; i++) {
    if (!macho_validate_function(&program->functions[i], diag)) return false;
  }

  ZBuf text;
  ZBuf rodata;
  zbuf_init(&text);
  zbuf_init(&rodata);
  bool has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  unsigned rodata_base_offset = macho_rodata_base_offset(program);
  if (has_rodata) macho_append_rodata(&rodata, program, rodata_base_offset);

  size_t *offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  if (!offsets) {
    zbuf_free(&rodata);
    zbuf_free(&text);
    return macho_diag(diag, "out of memory while emitting Mach-O executable");
  }

  MachOEmitContext ctx = {
    .program = program,
    .function_offsets = offsets,
    .function_count = program->function_len,
    .rodata_base_offset = rodata_base_offset,
    .pie_relative_data = true
  };
  size_t start_call_patch = macho_emit_exe_start_stub(&text);
  macho_pad_to(&text, macho_align(text.len, 16));
  for (size_t i = 0; i < program->function_len; i++) {
    macho_pad_to(&text, macho_align(text.len, 4));
    offsets[i] = text.len;
    if (!macho_emit_function_text(&text, &program->functions[i], &ctx, diag)) {
      free(ctx.runtime_json_parse_bytes_patches);
      free(ctx.runtime_http_fetch_patches);
      free(ctx.runtime_http_result_ok_patches);
      free(ctx.runtime_http_result_status_patches);
      free(ctx.runtime_http_result_body_len_patches);
      free(ctx.runtime_http_result_error_patches);
      free(ctx.runtime_http_response_len_patches);
      free(ctx.runtime_http_response_headers_len_patches);
      free(ctx.runtime_http_response_body_offset_patches);
      free(ctx.runtime_http_header_value_patches);
      free(ctx.runtime_http_header_found_patches);
      free(ctx.runtime_http_header_offset_patches);
      free(ctx.runtime_http_header_len_patches);
      free(ctx.world_write_patches);
      free(ctx.data_patches);
      free(ctx.call_patches);
      free(offsets);
      zbuf_free(&rodata);
      zbuf_free(&text);
      return false;
    }
  }

  if (ctx.runtime_json_parse_bytes_patch_len > 0 ||
      ctx.runtime_http_fetch_patch_len > 0 ||
      ctx.runtime_http_result_ok_patch_len > 0 ||
      ctx.runtime_http_result_status_patch_len > 0 ||
      ctx.runtime_http_result_body_len_patch_len > 0 ||
      ctx.runtime_http_result_error_patch_len > 0 ||
      ctx.runtime_http_response_len_patch_len > 0 ||
      ctx.runtime_http_response_headers_len_patch_len > 0 ||
      ctx.runtime_http_response_body_offset_patch_len > 0 ||
      ctx.runtime_http_header_value_patch_len > 0 ||
      ctx.runtime_http_header_found_patch_len > 0 ||
      ctx.runtime_http_header_offset_patch_len > 0 ||
      ctx.runtime_http_header_len_patch_len > 0) {
    free(ctx.runtime_json_parse_bytes_patches);
    free(ctx.runtime_http_fetch_patches);
    free(ctx.runtime_http_result_ok_patches);
    free(ctx.runtime_http_result_status_patches);
    free(ctx.runtime_http_result_body_len_patches);
    free(ctx.runtime_http_result_error_patches);
    free(ctx.runtime_http_response_len_patches);
    free(ctx.runtime_http_response_headers_len_patches);
    free(ctx.runtime_http_response_body_offset_patches);
    free(ctx.runtime_http_header_value_patches);
    free(ctx.runtime_http_header_found_patches);
    free(ctx.runtime_http_header_offset_patches);
    free(ctx.runtime_http_header_len_patches);
    free(ctx.world_write_patches);
    free(ctx.data_patches);
    free(ctx.call_patches);
    free(offsets);
    zbuf_free(&rodata);
    zbuf_free(&text);
    return macho_diag_at(diag, "direct AArch64 Mach-O executable runtime helpers require object emission and an explicit runtime link step", 1, 1, "use --emit obj and link zero_runtime.c");
  }

  size_t world_write_offset = 0;
  if (ctx.world_write_patch_len > 0) {
    macho_pad_to(&text, macho_align(text.len, 4));
    world_write_offset = macho_emit_exe_world_write(&text);
  }
  macho_patch_branch26(&text, start_call_patch, offsets[main_index]);
  for (size_t i = 0; i < ctx.call_patch_len; i++) {
    const MachOCallPatch *patch = &ctx.call_patches[i];
    macho_patch_branch26(&text, patch->patch_offset, offsets[patch->callee_index]);
  }
  for (size_t i = 0; i < ctx.world_write_patch_len; i++) {
    macho_patch_branch26(&text, ctx.world_write_patches[i].patch_offset, world_write_offset);
  }

  const uint64_t base_addr = 0x100000000ull;
  const uint32_t page_size = 0x4000;
  const uint32_t header_size = 32;
  const uint32_t pagezero_cmd_size = 72;
  const uint32_t section_count = has_rodata ? 2u : 1u;
  const uint32_t text_segment_cmd_size = 72 + section_count * 80;
  const uint32_t linkedit_cmd_size = 72;
  const uint32_t dyld_info_cmd_size = 48;
  const uint32_t uuid_cmd_size = 24;
  const uint32_t dylinker_cmd_size = 32;
  const uint32_t libsystem_cmd_size = 56;
  const uint32_t main_cmd_size = 24;
  const uint32_t build_version_cmd_size = 24;
  const uint32_t code_signature_cmd_size = 16;
  const uint32_t sizeofcmds = pagezero_cmd_size + text_segment_cmd_size + linkedit_cmd_size + dyld_info_cmd_size + uuid_cmd_size + dylinker_cmd_size + libsystem_cmd_size + main_cmd_size + build_version_cmd_size + code_signature_cmd_size;
  const uint32_t text_offset = (uint32_t)macho_align(header_size + sizeofcmds, 16);
  const uint32_t rodata_offset = has_rodata ? (uint32_t)macho_align(text_offset + text.len, 8) : 0;
  for (size_t i = 0; i < ctx.data_patch_len; i++) {
    const MachODataPatch *patch = &ctx.data_patches[i];
    uint64_t addr = base_addr + rodata_offset + (patch->data_offset - rodata_base_offset);
    if (ctx.pie_relative_data) macho_patch_adrp_add(&text, patch->patch_offset, base_addr + text_offset + patch->patch_offset, addr);
    else patch_u64le(&text, patch->patch_offset, addr);
  }
  const uint64_t segment_content_size = has_rodata ? rodata_offset + rodata.len : text_offset + text.len;
  const uint64_t segment_file_size = macho_align((size_t)segment_content_size, page_size);
  const uint64_t segment_vm_size = segment_file_size;
  ZBuf rebase;
  zbuf_init(&rebase);
  if (ctx.data_patch_len > 0 && !ctx.pie_relative_data) {
    append_u8(&rebase, 0x11); // REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER
    for (size_t i = 0; i < ctx.data_patch_len; i++) {
      append_u8(&rebase, 0x21); // REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB, __TEXT segment
      macho_append_uleb128(&rebase, text_offset + ctx.data_patches[i].patch_offset);
      append_u8(&rebase, 0x51); // REBASE_OPCODE_DO_REBASE_IMM_TIMES, once
    }
    append_u8(&rebase, 0x00);
  }
  const uint32_t rebase_offset = rebase.len > 0 ? (uint32_t)segment_file_size : 0;
  const uint32_t rebase_size = (uint32_t)rebase.len;
  const uint32_t code_signature_offset = (uint32_t)macho_align((size_t)segment_file_size + rebase.len, 16);
  const char *code_signature_id = "zero-direct";
  const uint32_t code_signature_hash_offset = (uint32_t)macho_align(88 + strlen(code_signature_id) + 1, 4);
  const uint32_t code_signature_slots = (code_signature_offset + 4095u) / 4096u;
  const uint32_t code_signature_size = 20 + code_signature_hash_offset + code_signature_slots * 32;
  const uint64_t linkedit_vmaddr = base_addr + segment_file_size;
  const uint64_t linkedit_vmsize = macho_align(code_signature_size, page_size);

  zbuf_init(out);
  append_u32le(out, 0xfeedfacfu);      // MH_MAGIC_64
  append_u32le(out, 0x0100000cu);      // CPU_TYPE_ARM64
  append_u32le(out, 0);                // CPU_SUBTYPE_ARM64_ALL
  append_u32le(out, 2);                // MH_EXECUTE
  append_u32le(out, 10);               // ncmds
  append_u32le(out, sizeofcmds);
  append_u32le(out, 0x200085);         // MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_PIE
  append_u32le(out, 0);

  append_u32le(out, 0x19);             // LC_SEGMENT_64
  append_u32le(out, pagezero_cmd_size);
  append_fixed(out, "__PAGEZERO", 16);
  append_u64le(out, 0);
  append_u64le(out, base_addr);
  append_u64le(out, 0);
  append_u64le(out, 0);
  append_u32le(out, 0);
  append_u32le(out, 0);
  append_u32le(out, 0);
  append_u32le(out, 0);

  append_u32le(out, 0x19);             // LC_SEGMENT_64
  append_u32le(out, text_segment_cmd_size);
  append_fixed(out, "__TEXT", 16);
  append_u64le(out, base_addr);
  append_u64le(out, segment_vm_size);
  append_u64le(out, 0);
  append_u64le(out, segment_file_size);
  append_u32le(out, 5);                // r-x
  append_u32le(out, 5);
  append_u32le(out, section_count);
  append_u32le(out, 0);

  append_fixed(out, "__text", 16);
  append_fixed(out, "__TEXT", 16);
  append_u64le(out, base_addr + text_offset);
  append_u64le(out, text.len);
  append_u32le(out, text_offset);
  append_u32le(out, 2);
  append_u32le(out, 0);
  append_u32le(out, 0);
  append_u32le(out, 0x80000400u);
  append_u32le(out, 0);
  append_u32le(out, 0);
  append_u32le(out, 0);

  if (has_rodata) {
    append_fixed(out, "__const", 16);
    append_fixed(out, "__TEXT", 16);
    append_u64le(out, base_addr + rodata_offset);
    append_u64le(out, rodata.len);
    append_u32le(out, rodata_offset);
    append_u32le(out, 3);
    append_u32le(out, 0);
    append_u32le(out, 0);
    append_u32le(out, 0);
    append_u32le(out, 0);
    append_u32le(out, 0);
    append_u32le(out, 0);
  }

  append_u32le(out, 0x19);             // LC_SEGMENT_64
  append_u32le(out, linkedit_cmd_size);
  append_fixed(out, "__LINKEDIT", 16);
  append_u64le(out, linkedit_vmaddr);
  append_u64le(out, linkedit_vmsize);
  append_u64le(out, code_signature_offset);
  append_u64le(out, code_signature_size);
  append_u32le(out, 1);                // r--
  append_u32le(out, 1);
  append_u32le(out, 0);
  append_u32le(out, 0);

  append_u32le(out, 0x80000022u);      // LC_DYLD_INFO_ONLY
  append_u32le(out, dyld_info_cmd_size);
  append_u32le(out, rebase_offset);
  append_u32le(out, rebase_size);
  for (unsigned i = 0; i < 8; i++) append_u32le(out, 0);

  append_u32le(out, 0x1b);             // LC_UUID
  append_u32le(out, uuid_cmd_size);
  const size_t uuid_offset = out->len;
  for (unsigned i = 0; i < 16; i++) append_u8(out, 0);

  append_u32le(out, 0xe);              // LC_LOAD_DYLINKER
  append_u32le(out, dylinker_cmd_size);
  append_u32le(out, 12);
  append_bytes(out, "/usr/lib/dyld", strlen("/usr/lib/dyld") + 1);
  macho_pad_to(out, header_size + pagezero_cmd_size + text_segment_cmd_size + linkedit_cmd_size + dyld_info_cmd_size + uuid_cmd_size + dylinker_cmd_size);

  append_u32le(out, 0xc);              // LC_LOAD_DYLIB
  append_u32le(out, libsystem_cmd_size);
  append_u32le(out, 24);
  append_u32le(out, 2);
  append_u32le(out, 0x054c0000);
  append_u32le(out, 0x00010000);
  append_bytes(out, "/usr/lib/libSystem.B.dylib", strlen("/usr/lib/libSystem.B.dylib") + 1);
  macho_pad_to(out, header_size + pagezero_cmd_size + text_segment_cmd_size + linkedit_cmd_size + dyld_info_cmd_size + uuid_cmd_size + dylinker_cmd_size + libsystem_cmd_size);

  append_u32le(out, 0x80000028u);      // LC_MAIN
  append_u32le(out, main_cmd_size);
  append_u64le(out, text_offset);
  append_u64le(out, 0);

  append_u32le(out, 0x32);             // LC_BUILD_VERSION
  append_u32le(out, build_version_cmd_size);
  append_u32le(out, 1);                // PLATFORM_MACOS
  append_u32le(out, 0x000b0000);       // macOS 11.0.0
  append_u32le(out, 0);
  append_u32le(out, 0);

  append_u32le(out, 0x1d);             // LC_CODE_SIGNATURE
  append_u32le(out, code_signature_cmd_size);
  append_u32le(out, code_signature_offset);
  append_u32le(out, code_signature_size);

  macho_pad_to(out, text_offset);
  if (text.data) append_bytes(out, text.data, text.len);
  if (has_rodata) {
    macho_pad_to(out, rodata_offset);
    if (rodata.data) append_bytes(out, rodata.data, rodata.len);
  }
  macho_pad_to(out, (size_t)segment_file_size);
  if (rebase.data) append_bytes(out, rebase.data, rebase.len);
  macho_pad_to(out, code_signature_offset);
  unsigned char uuid_hash[32];
  macho_sha256_hash((const unsigned char *)out->data, out->len, uuid_hash);
  uuid_hash[6] = (unsigned char)((uuid_hash[6] & 0x0fu) | 0x50u);
  uuid_hash[8] = (unsigned char)((uuid_hash[8] & 0x3fu) | 0x80u);
  patch_bytes(out, uuid_offset, uuid_hash, 16);
  ZBuf signature;
  macho_append_code_signature(&signature, (const unsigned char *)out->data, out->len, code_signature_id);
  if (signature.data) append_bytes(out, signature.data, signature.len);
  zbuf_free(&signature);

  free(ctx.runtime_json_parse_bytes_patches);
  free(ctx.runtime_http_fetch_patches);
  free(ctx.runtime_http_result_ok_patches);
  free(ctx.runtime_http_result_status_patches);
  free(ctx.runtime_http_result_body_len_patches);
  free(ctx.runtime_http_result_error_patches);
  free(ctx.runtime_http_response_len_patches);
  free(ctx.runtime_http_response_headers_len_patches);
  free(ctx.runtime_http_response_body_offset_patches);
  free(ctx.runtime_http_header_value_patches);
  free(ctx.runtime_http_header_found_patches);
  free(ctx.runtime_http_header_offset_patches);
  free(ctx.runtime_http_header_len_patches);
  free(ctx.world_write_patches);
  free(ctx.data_patches);
  free(ctx.call_patches);
  free(offsets);
  zbuf_free(&rebase);
  zbuf_free(&rodata);
  zbuf_free(&text);
  return true;
}
