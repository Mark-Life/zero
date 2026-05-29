#include "coff_emit_state.h"
#include "x64_emit.h"

#include <stdio.h>
#include <stdlib.h>

static const char *const runtime_helper_symbols[COFF_RUNTIME_HELPER_COUNT] = {
  "zero_world_write",
};

static const char *const math_symbol_names[COFF_MATH_COUNT] = {
  "sqrtf", "expf", "cosf", "sinf", "powf", "fabsf", "floorf",
};

static const unsigned math_symbol_import_indices[COFF_MATH_COUNT] = {
  Z_COFF_IMPORT_SQRTF, Z_COFF_IMPORT_EXPF, Z_COFF_IMPORT_COSF, Z_COFF_IMPORT_SINF,
  Z_COFF_IMPORT_POWF, Z_COFF_IMPORT_FABSF, Z_COFF_IMPORT_FLOORF,
};

static bool coff_math_symbol_valid(CoffMathSymbol symbol) {
  return symbol >= 0 && symbol < COFF_MATH_COUNT;
}

static bool coff_emit_state_diag(ZDiag *diag, const char *message, int line, int column, const char *actual) {
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

static bool coff_runtime_helper_valid(CoffRuntimeHelper helper) {
  return helper >= 0 && helper < COFF_RUNTIME_HELPER_COUNT;
}

const char *z_coff_runtime_helper_symbol(CoffRuntimeHelper helper) {
  if (!coff_runtime_helper_valid(helper)) return "";
  return runtime_helper_symbols[helper];
}

const char *z_coff_math_symbol_name(CoffMathSymbol symbol) {
  if (!coff_math_symbol_valid(symbol)) return "";
  return math_symbol_names[symbol];
}

unsigned z_coff_math_symbol_import_index(CoffMathSymbol symbol) {
  if (!coff_math_symbol_valid(symbol)) return 0;
  return math_symbol_import_indices[symbol];
}

void z_coff_emit_context_free(CoffEmitContext *ctx) {
  if (!ctx) return;
  free(ctx->call_patches);
  free(ctx->rodata_patches);
  free(ctx->math_patches);
  free(ctx->mmap_patches);
  for (unsigned i = 0; i < COFF_RUNTIME_HELPER_COUNT; i++) {
    free(ctx->runtime_patches[i].items);
  }
}

bool z_coff_record_call_patch(CoffEmitContext *ctx, size_t patch_offset, unsigned callee_index, const IrValue *value, ZDiag *diag) {
  if (!ctx || callee_index >= ctx->function_count) {
    return coff_emit_state_diag(diag, "direct COFF call target index is out of range", value ? value->line : 1, value ? value->column : 1, "invalid callee");
  }
  if (ctx->call_patch_len == ctx->call_patch_cap) {
    ctx->call_patch_cap = z_grow_capacity(ctx->call_patch_cap, ctx->call_patch_len + 1, 8);
    ctx->call_patches = z_checked_reallocarray(ctx->call_patches, ctx->call_patch_cap, sizeof(CoffCallPatch));
  }
  ctx->call_patches[ctx->call_patch_len++] = (CoffCallPatch){.patch_offset = patch_offset, .callee_index = callee_index};
  return true;
}

bool z_coff_record_rodata_patch(CoffEmitContext *ctx, size_t patch_offset, unsigned data_offset, const IrValue *value, ZDiag *diag) {
  if (!ctx) return coff_emit_state_diag(diag, "direct COFF readonly data relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->rodata_patch_len == ctx->rodata_patch_cap) {
    ctx->rodata_patch_cap = z_grow_capacity(ctx->rodata_patch_cap, ctx->rodata_patch_len + 1, 8);
    ctx->rodata_patches = z_checked_reallocarray(ctx->rodata_patches, ctx->rodata_patch_cap, sizeof(ZCoffImageDataPatch));
  }
  ctx->rodata_patches[ctx->rodata_patch_len++] = (ZCoffImageDataPatch){.patch_offset = patch_offset, .data_offset = data_offset};
  return true;
}

bool z_coff_record_instr_runtime_patch(CoffEmitContext *ctx, CoffRuntimeHelper helper, size_t patch_offset, const IrInstr *instr, ZDiag *diag) {
  if (!ctx || !coff_runtime_helper_valid(helper)) {
    return coff_emit_state_diag(diag, "direct COFF runtime relocation requires an emit context", instr ? instr->line : 1, instr ? instr->column : 1, "missing context");
  }
  CoffPatchList *list = &ctx->runtime_patches[helper];
  if (list->len == list->cap) {
    list->cap = z_grow_capacity(list->cap, list->len + 1, 4);
    list->items = z_checked_reallocarray(list->items, list->cap, sizeof(CoffPatch));
  }
  list->items[list->len++] = (CoffPatch){.patch_offset = patch_offset};
  return true;
}

size_t z_coff_runtime_patch_count(const CoffEmitContext *ctx, CoffRuntimeHelper helper) {
  if (!ctx || !coff_runtime_helper_valid(helper)) return 0;
  return ctx->runtime_patches[helper].len;
}

size_t z_coff_text_relocation_count(const CoffEmitContext *ctx) {
  if (!ctx) return 0;
  size_t count = ctx->call_patch_len + ctx->rodata_patch_len + ctx->math_patch_len + ctx->mmap_patch_len;
  for (unsigned i = 0; i < COFF_RUNTIME_HELPER_COUNT; i++) {
    count += ctx->runtime_patches[i].len;
  }
  return count;
}

void z_coff_patch_call_patches(ZBuf *text, const CoffEmitContext *ctx) {
  for (size_t i = 0; ctx && i < ctx->call_patch_len; i++) {
    const CoffCallPatch *patch = &ctx->call_patches[i];
    z_x64_patch_rel32(text, patch->patch_offset, ctx->function_offsets[patch->callee_index]);
  }
}

void z_coff_patch_runtime_patches(ZBuf *text, const CoffEmitContext *ctx, CoffRuntimeHelper helper, size_t target_offset) {
  if (!ctx || !coff_runtime_helper_valid(helper)) return;
  const CoffPatchList *patches = &ctx->runtime_patches[helper];
  for (size_t i = 0; i < patches->len; i++) {
    z_x64_patch_rel32(text, patches->items[i].patch_offset, target_offset);
  }
}

void z_coff_append_call_relocations(ZBuf *relocs, const CoffEmitContext *ctx, uint32_t function_symbol_base) {
  for (size_t i = 0; ctx && i < ctx->call_patch_len; i++) {
    const CoffCallPatch *patch = &ctx->call_patches[i];
    z_coff_append_reloc_amd64(relocs, (uint32_t)patch->patch_offset, function_symbol_base + patch->callee_index, Z_COFF_RELOC_AMD64_REL32);
  }
}

void z_coff_append_rodata_relocations(ZBuf *relocs, const CoffEmitContext *ctx, uint32_t rodata_symbol) {
  for (size_t i = 0; ctx && i < ctx->rodata_patch_len; i++) {
    z_coff_append_reloc_amd64(relocs, (uint32_t)ctx->rodata_patches[i].patch_offset, rodata_symbol, Z_COFF_RELOC_AMD64_ADDR64);
  }
}

void z_coff_append_runtime_relocations(ZBuf *relocs, const CoffEmitContext *ctx, CoffRuntimeHelper helper, uint32_t runtime_symbol) {
  if (!ctx || !coff_runtime_helper_valid(helper)) return;
  const CoffPatchList *patches = &ctx->runtime_patches[helper];
  for (size_t i = 0; i < patches->len; i++) {
    z_coff_append_reloc_amd64(relocs, (uint32_t)patches->items[i].patch_offset, runtime_symbol, Z_COFF_RELOC_AMD64_REL32);
  }
}

bool z_coff_record_math_patch(CoffEmitContext *ctx, size_t patch_offset, CoffMathSymbol symbol, const IrValue *value, ZDiag *diag) {
  if (!ctx || !coff_math_symbol_valid(symbol)) {
    return coff_emit_state_diag(diag, "direct COFF math symbol is out of range", value ? value->line : 1, value ? value->column : 1, "invalid math symbol");
  }
  if (ctx->math_patch_len == ctx->math_patch_cap) {
    ctx->math_patch_cap = z_grow_capacity(ctx->math_patch_cap, ctx->math_patch_len + 1, 4);
    ctx->math_patches = z_checked_reallocarray(ctx->math_patches, ctx->math_patch_cap, sizeof(CoffMathPatch));
  }
  ctx->math_patches[ctx->math_patch_len++] = (CoffMathPatch){.patch_offset = patch_offset, .symbol = symbol};
  return true;
}

size_t z_coff_math_patch_count(const CoffEmitContext *ctx) {
  if (!ctx) return 0;
  return ctx->math_patch_len;
}

bool z_coff_math_symbol_used(const CoffEmitContext *ctx, CoffMathSymbol symbol) {
  if (!ctx || !coff_math_symbol_valid(symbol)) return false;
  for (size_t i = 0; i < ctx->math_patch_len; i++) {
    if (ctx->math_patches[i].symbol == symbol) return true;
  }
  return false;
}

size_t z_coff_math_used_symbol_count(const CoffEmitContext *ctx) {
  if (!ctx) return 0;
  size_t count = 0;
  for (unsigned s = 0; s < COFF_MATH_COUNT; s++) {
    if (z_coff_math_symbol_used(ctx, (CoffMathSymbol)s)) count++;
  }
  return count;
}

void z_coff_append_math_relocations(ZBuf *relocs, const CoffEmitContext *ctx, uint32_t math_symbol_base) {
  if (!ctx) return;
  unsigned slot[COFF_MATH_COUNT];
  unsigned used = 0;
  for (unsigned s = 0; s < COFF_MATH_COUNT; s++) {
    slot[s] = z_coff_math_symbol_used(ctx, (CoffMathSymbol)s) ? used++ : 0u;
  }
  for (size_t i = 0; i < ctx->math_patch_len; i++) {
    const CoffMathPatch *patch = &ctx->math_patches[i];
    z_coff_append_reloc_amd64(relocs, (uint32_t)patch->patch_offset, math_symbol_base + slot[patch->symbol], Z_COFF_RELOC_AMD64_REL32);
  }
}

// kernel32 mmap-call patches. The seven importable kernel32 functions
// (CreateFileA, GetFileSizeEx, CreateFileMappingA, MapViewOfFile, UnmapViewOfFile, CloseHandle,
// VirtualAlloc) all share the `ff 15 disp32` CALL [rip+disp32] pattern through the .idata IAT.
static const char *const mmap_import_names[Z_COFF_IMPORT_COUNT] = {
  "ExitProcess", "GetStdHandle", "WriteFile",
  "CreateFileA", "GetFileSizeEx", "CreateFileMappingA",
  "MapViewOfFile", "UnmapViewOfFile", "CloseHandle", "VirtualAlloc",
  "sqrtf", "expf", "cosf", "sinf", "powf", "fabsf", "floorf",
};

const char *z_coff_mmap_import_name(unsigned import_index) {
  if (import_index >= Z_COFF_IMPORT_COUNT) return "";
  return mmap_import_names[import_index];
}

static bool coff_mmap_import_valid(unsigned import_index) {
  return import_index == Z_COFF_IMPORT_CREATE_FILE_A
      || import_index == Z_COFF_IMPORT_GET_FILE_SIZE_EX
      || import_index == Z_COFF_IMPORT_CREATE_FILE_MAPPING_A
      || import_index == Z_COFF_IMPORT_MAP_VIEW_OF_FILE
      || import_index == Z_COFF_IMPORT_UNMAP_VIEW_OF_FILE
      || import_index == Z_COFF_IMPORT_CLOSE_HANDLE
      || import_index == Z_COFF_IMPORT_VIRTUAL_ALLOC;
}

bool z_coff_record_mmap_patch(CoffEmitContext *ctx, size_t patch_offset, unsigned import_index, const IrValue *value, ZDiag *diag) {
  if (!ctx || !coff_mmap_import_valid(import_index)) {
    return coff_emit_state_diag(diag, "direct COFF mmap import index is out of range", value ? value->line : 1, value ? value->column : 1, "invalid mmap import");
  }
  if (ctx->mmap_patch_len == ctx->mmap_patch_cap) {
    ctx->mmap_patch_cap = z_grow_capacity(ctx->mmap_patch_cap, ctx->mmap_patch_len + 1, 4);
    ctx->mmap_patches = z_checked_reallocarray(ctx->mmap_patches, ctx->mmap_patch_cap, sizeof(CoffMmapPatch));
  }
  ctx->mmap_patches[ctx->mmap_patch_len++] = (CoffMmapPatch){.patch_offset = patch_offset, .import_index = import_index};
  return true;
}

size_t z_coff_mmap_patch_count(const CoffEmitContext *ctx) {
  if (!ctx) return 0;
  return ctx->mmap_patch_len;
}

bool z_coff_mmap_import_used(const CoffEmitContext *ctx, unsigned import_index) {
  if (!ctx || !coff_mmap_import_valid(import_index)) return false;
  for (size_t i = 0; i < ctx->mmap_patch_len; i++) {
    if (ctx->mmap_patches[i].import_index == import_index) return true;
  }
  return false;
}

// Per-symbol slot indices for mmap externals on the object path: the 7 importable kernel32 fns
// appear in the order their Z_COFF_IMPORT_* index defines, so the slot layout is monotonic.
static unsigned mmap_used_slot(const CoffEmitContext *ctx, unsigned import_index, unsigned *out_used) {
  unsigned used = 0;
  unsigned slot = 0;
  static const unsigned ordered[] = {
    Z_COFF_IMPORT_CREATE_FILE_A, Z_COFF_IMPORT_GET_FILE_SIZE_EX, Z_COFF_IMPORT_CREATE_FILE_MAPPING_A,
    Z_COFF_IMPORT_MAP_VIEW_OF_FILE, Z_COFF_IMPORT_UNMAP_VIEW_OF_FILE, Z_COFF_IMPORT_CLOSE_HANDLE,
    Z_COFF_IMPORT_VIRTUAL_ALLOC,
  };
  for (unsigned i = 0; i < sizeof(ordered) / sizeof(ordered[0]); i++) {
    if (!z_coff_mmap_import_used(ctx, ordered[i])) continue;
    if (ordered[i] == import_index) slot = used;
    used++;
  }
  if (out_used) *out_used = used;
  return slot;
}

size_t z_coff_mmap_used_symbol_count(const CoffEmitContext *ctx) {
  if (!ctx) return 0;
  unsigned used = 0;
  mmap_used_slot(ctx, Z_COFF_IMPORT_CREATE_FILE_A, &used);
  return used;
}

void z_coff_append_mmap_relocations(ZBuf *relocs, const CoffEmitContext *ctx, uint32_t mmap_symbol_base) {
  if (!ctx) return;
  for (size_t i = 0; i < ctx->mmap_patch_len; i++) {
    const CoffMmapPatch *patch = &ctx->mmap_patches[i];
    unsigned slot = mmap_used_slot(ctx, patch->import_index, NULL);
    z_coff_append_reloc_amd64(relocs, (uint32_t)patch->patch_offset, mmap_symbol_base + slot, Z_COFF_RELOC_AMD64_REL32);
  }
}
