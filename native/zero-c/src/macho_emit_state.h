#ifndef ZERO_C_MACHO_EMIT_STATE_H
#define ZERO_C_MACHO_EMIT_STATE_H

#include "zero.h"

#include <stdint.h>

typedef enum {
  MACHO_RUNTIME_WORLD_WRITE,
  MACHO_RUNTIME_JSON_PARSE_BYTES,
  MACHO_RUNTIME_HTTP_FETCH,
  MACHO_RUNTIME_HTTP_RESULT_OK,
  MACHO_RUNTIME_HTTP_RESULT_STATUS,
  MACHO_RUNTIME_HTTP_RESULT_BODY_LEN,
  MACHO_RUNTIME_HTTP_RESULT_ERROR,
  MACHO_RUNTIME_HTTP_RESPONSE_LEN,
  MACHO_RUNTIME_HTTP_RESPONSE_HEADERS_LEN,
  MACHO_RUNTIME_HTTP_RESPONSE_BODY_OFFSET,
  MACHO_RUNTIME_HTTP_HEADER_VALUE,
  MACHO_RUNTIME_HTTP_HEADER_FOUND,
  MACHO_RUNTIME_HTTP_HEADER_OFFSET,
  MACHO_RUNTIME_HTTP_HEADER_LEN,
  MACHO_RUNTIME_HELPER_COUNT
} MachORuntimeHelper;

// The libm symbols std.math lowers to. The order is the symbol-table order; one undefined
// external is emitted per symbol actually called, each resolved by the host link step against
// libSystem (-lm). Underscore-prefixed Mach-O symbol names. Shared by both Mach-O backends
// (arm64 + x86_64): the symbol names and BRANCH26 relocation shape are identical.
typedef enum {
  Z_MACHO_MATH_SQRTF = 0,
  Z_MACHO_MATH_EXPF,
  Z_MACHO_MATH_COSF,
  Z_MACHO_MATH_SINF,
  Z_MACHO_MATH_POWF,
  Z_MACHO_MATH_FABSF,
  Z_MACHO_MATH_FLOORF,
  Z_MACHO_MATH_COUNT
} MachOMathSymbol;

// The libSystem (libc) symbols the OS-interface lowerings call. Like the libm set, the order is the
// symbol-table order and one undefined external is emitted per symbol actually called, resolved by
// the host link step (zig cc) against libSystem. Args/return are integer (x0..x7/x0), so these are
// simpler than the libm FP calls. Anonymous std.mem.pageAlloc uses `_mmap`; the file mmap path
// (std.fs.mmap) uses `_open`/`_lseek`/`_mmap`/`_close` and std.fs.munmap uses `_munmap`. Kept
// separate from MachOMathSymbol: libc is bound by libSystem, the zero runtime object, not -lm.
typedef enum {
  Z_MACHO_LIBC_MMAP = 0,
  Z_MACHO_LIBC_MUNMAP,
  Z_MACHO_LIBC_OPEN,
  Z_MACHO_LIBC_LSEEK,
  Z_MACHO_LIBC_CLOSE,
  Z_MACHO_LIBC_COUNT
} MachOLibcSymbol;

typedef struct {
  size_t patch_offset;
} MachOPatch;

typedef struct {
  MachOPatch *items;
  size_t len;
  size_t cap;
} MachOPatchList;

// One recorded `bl <libm>` call site. `symbol` selects which libm external the BRANCH26
// relocation binds to. Kept separate from MachORuntimeHelper: libm is linked via libSystem/-lm,
// not the zero runtime object, so it must not interact with runtime-object accounting.
typedef struct {
  size_t patch_offset;
  MachOMathSymbol symbol;
} MachOMathCallPatch;

// One recorded `bl <libc>` call site. `symbol` selects which libSystem external the BRANCH26
// relocation binds to (same external-call shape as the libm sites).
typedef struct {
  size_t patch_offset;
  MachOLibcSymbol symbol;
} MachOLibcCallPatch;

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
  const IrProgram *program;
  size_t *function_offsets;
  size_t function_count;
  MachOCallPatch *call_patches;
  size_t call_patch_len;
  size_t call_patch_cap;
  MachODataPatch *data_patches;
  size_t data_patch_len;
  size_t data_patch_cap;
  MachOPatchList runtime_patches[MACHO_RUNTIME_HELPER_COUNT];
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

const char *z_macho_runtime_helper_symbol(MachORuntimeHelper helper);
void z_macho_emit_context_free(MachOEmitContext *ctx);
bool z_macho_record_call_patch(MachOEmitContext *ctx, size_t patch_offset, unsigned callee_index, const IrValue *value, ZDiag *diag);
bool z_macho_record_data_patch(MachOEmitContext *ctx, size_t patch_offset, unsigned data_offset, const IrValue *value, ZDiag *diag);
bool z_macho_record_value_runtime_patch(MachOEmitContext *ctx, MachORuntimeHelper helper, size_t patch_offset, const IrValue *value, ZDiag *diag);
bool z_macho_record_instr_runtime_patch(MachOEmitContext *ctx, MachORuntimeHelper helper, size_t patch_offset, const IrInstr *instr, ZDiag *diag);
size_t z_macho_runtime_patch_count(const MachOEmitContext *ctx, MachORuntimeHelper helper);
const MachOPatchList *z_macho_runtime_patch_list(const MachOEmitContext *ctx, MachORuntimeHelper helper);
bool z_macho_has_unsupported_exe_runtime_patches(const MachOEmitContext *ctx);
void z_macho_append_call_relocations(ZBuf *relocs, const MachOEmitContext *ctx);
void z_macho_append_runtime_relocations(ZBuf *relocs, const MachOEmitContext *ctx, MachORuntimeHelper helper, unsigned symbol_index);
const char *z_macho_math_symbol_name(MachOMathSymbol symbol);
MachOMathSymbol z_macho_math_symbol_for_value(IrValueKind kind);
bool z_macho_record_math_call_patch(MachOEmitContext *ctx, size_t patch_offset, MachOMathSymbol symbol, const IrValue *value, ZDiag *diag);
bool z_macho_math_symbol_used(const MachOEmitContext *ctx, MachOMathSymbol symbol);
size_t z_macho_math_call_patch_count(const MachOEmitContext *ctx);
void z_macho_append_math_call_relocations(ZBuf *relocs, const MachOEmitContext *ctx, MachOMathSymbol symbol, unsigned symbol_index);
const char *z_macho_libc_symbol_name(MachOLibcSymbol symbol);
bool z_macho_record_libc_call_patch(MachOEmitContext *ctx, size_t patch_offset, MachOLibcSymbol symbol, const IrValue *value, ZDiag *diag);
bool z_macho_libc_symbol_used(const MachOEmitContext *ctx, MachOLibcSymbol symbol);
size_t z_macho_libc_call_patch_count(const MachOEmitContext *ctx);
void z_macho_append_libc_call_relocations(ZBuf *relocs, const MachOEmitContext *ctx, MachOLibcSymbol symbol, unsigned symbol_index);
size_t z_macho_data_relocation_count(const MachOEmitContext *ctx);
size_t z_macho_text_relocation_count(const MachOEmitContext *ctx);
void z_macho_append_data_relocations(ZBuf *relocs, const MachOEmitContext *ctx, unsigned data_symbol_index);

#endif
