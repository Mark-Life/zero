#ifndef ZERO_C_COFF_EMIT_STATE_H
#define ZERO_C_COFF_EMIT_STATE_H

#include "zero.h"
#include "coff_format.h"

typedef enum {
  COFF_RUNTIME_WORLD_WRITE,
  COFF_RUNTIME_HELPER_COUNT
} CoffRuntimeHelper;

// libm transcendentals on COFF. isNaNf stays inline (UCOMISS + SETP). Other 7 fns
// resolve through msvcrt.dll via the .idata import directory on the exe path, and as undefined
// external symbols (REL32 reloc) on the object path.
typedef enum {
  COFF_MATH_SQRTF,
  COFF_MATH_EXPF,
  COFF_MATH_COSF,
  COFF_MATH_SINF,
  COFF_MATH_POWF,
  COFF_MATH_FABSF,
  COFF_MATH_FLOORF,
  COFF_MATH_COUNT
} CoffMathSymbol;

typedef struct {
  size_t patch_offset;
} CoffPatch;

typedef struct {
  CoffPatch *items;
  size_t len;
  size_t cap;
} CoffPatchList;

typedef struct {
  size_t patch_offset;
  unsigned callee_index;
} CoffCallPatch;

typedef struct {
  size_t patch_offset;
  CoffMathSymbol symbol;
} CoffMathPatch;

// kernel32.dll mmap/munmap/CreateFile/.../VirtualAlloc call sites. Recorded by
// emit_coff.c's mmap helpers, transferred onto the exe path's import_patches list after function
// emit (and into the object's external symbol table on the obj path).
typedef struct {
  size_t patch_offset;
  unsigned import_index; // Z_COFF_IMPORT_*
} CoffMmapPatch;

typedef struct {
  const IrProgram *program;
  size_t *function_offsets;
  size_t function_count;
  CoffCallPatch *call_patches;
  size_t call_patch_len;
  size_t call_patch_cap;
  ZCoffImageDataPatch *rodata_patches;
  size_t rodata_patch_len;
  size_t rodata_patch_cap;
  CoffPatchList runtime_patches[COFF_RUNTIME_HELPER_COUNT];
  CoffMathPatch *math_patches;
  size_t math_patch_len;
  size_t math_patch_cap;
  CoffMmapPatch *mmap_patches;
  size_t mmap_patch_len;
  size_t mmap_patch_cap;
  unsigned rodata_base_offset;
} CoffEmitContext;

const char *z_coff_runtime_helper_symbol(CoffRuntimeHelper helper);
const char *z_coff_math_symbol_name(CoffMathSymbol symbol);
unsigned z_coff_math_symbol_import_index(CoffMathSymbol symbol);
void z_coff_emit_context_free(CoffEmitContext *ctx);
bool z_coff_record_call_patch(CoffEmitContext *ctx, size_t patch_offset, unsigned callee_index, const IrValue *value, ZDiag *diag);
bool z_coff_record_rodata_patch(CoffEmitContext *ctx, size_t patch_offset, unsigned data_offset, const IrValue *value, ZDiag *diag);
bool z_coff_record_instr_runtime_patch(CoffEmitContext *ctx, CoffRuntimeHelper helper, size_t patch_offset, const IrInstr *instr, ZDiag *diag);
bool z_coff_record_math_patch(CoffEmitContext *ctx, size_t patch_offset, CoffMathSymbol symbol, const IrValue *value, ZDiag *diag);
size_t z_coff_runtime_patch_count(const CoffEmitContext *ctx, CoffRuntimeHelper helper);
size_t z_coff_text_relocation_count(const CoffEmitContext *ctx);
size_t z_coff_math_patch_count(const CoffEmitContext *ctx);
bool z_coff_math_symbol_used(const CoffEmitContext *ctx, CoffMathSymbol symbol);
size_t z_coff_math_used_symbol_count(const CoffEmitContext *ctx);
void z_coff_patch_call_patches(ZBuf *text, const CoffEmitContext *ctx);
void z_coff_patch_runtime_patches(ZBuf *text, const CoffEmitContext *ctx, CoffRuntimeHelper helper, size_t target_offset);
void z_coff_append_call_relocations(ZBuf *relocs, const CoffEmitContext *ctx, uint32_t function_symbol_base);
void z_coff_append_rodata_relocations(ZBuf *relocs, const CoffEmitContext *ctx, uint32_t rodata_symbol);
void z_coff_append_runtime_relocations(ZBuf *relocs, const CoffEmitContext *ctx, CoffRuntimeHelper helper, uint32_t runtime_symbol);
// Append REL32 relocs for each math patch against the per-symbol symbol-table index. The caller
// must have placed math symbols at `math_symbol_base + COFF_MATH_<X>` in the COFF symbol table.
void z_coff_append_math_relocations(ZBuf *relocs, const CoffEmitContext *ctx, uint32_t math_symbol_base);

// kernel32 mmap-call patch tracking. Record a CALL [rip+disp32] site against a kernel32
// import index (CreateFileA, GetFileSizeEx, CreateFileMappingA, MapViewOfFile, UnmapViewOfFile,
// CloseHandle, VirtualAlloc). The exe path transfers these into the .idata import_patches list;
// the object path materializes them as undefined external symbol REL32 relocs.
bool z_coff_record_mmap_patch(CoffEmitContext *ctx, size_t patch_offset, unsigned import_index, const IrValue *value, ZDiag *diag);
size_t z_coff_mmap_patch_count(const CoffEmitContext *ctx);
bool z_coff_mmap_import_used(const CoffEmitContext *ctx, unsigned import_index);
size_t z_coff_mmap_used_symbol_count(const CoffEmitContext *ctx);
void z_coff_append_mmap_relocations(ZBuf *relocs, const CoffEmitContext *ctx, uint32_t mmap_symbol_base);
const char *z_coff_mmap_import_name(unsigned import_index);

#endif
