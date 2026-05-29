#include "elf_emit_state.h"
#include "elf_format.h"
#include "x64_emit.h"

#include <stdio.h>
#include <stdlib.h>

static const char *const runtime_helper_symbols[ELF_RUNTIME_HELPER_COUNT] = {
  "zero_json_parse_bytes",
  "zero_http_fetch_result",
  "zero_http_result_ok",
  "zero_http_result_status",
  "zero_http_result_body_len",
  "zero_http_result_error",
  "zero_http_response_len",
  "zero_http_response_headers_len",
  "zero_http_response_body_offset",
  "zero_http_header_value",
  "zero_http_header_found",
  "zero_http_header_offset",
  "zero_http_header_len",
};

static const char *const math_symbol_names[Z_ELF_MATH_COUNT] = {
  "sqrtf", "expf", "cosf", "sinf", "powf", "fabsf", "floorf"
};

static bool elf_emit_state_diag(ZDiag *diag, const char *message, int line, int column, const char *actual) {
  if (diag) {
    diag->code = 4004;
    diag->line = line > 0 ? line : 1;
    diag->column = column > 0 ? column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct ELF64 object MVP subset");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "unsupported construct");
    snprintf(diag->help, sizeof(diag->help), "choose a supported direct target or restrict this program to exported primitive integer arithmetic functions");
  }
  return false;
}

static bool elf_runtime_helper_valid(ElfRuntimeHelper helper) {
  return helper >= 0 && helper < ELF_RUNTIME_HELPER_COUNT;
}

const char *z_elf_runtime_helper_symbol(ElfRuntimeHelper helper) {
  if (!elf_runtime_helper_valid(helper)) return "";
  return runtime_helper_symbols[helper];
}

static bool elf_math_symbol_valid(ElfMathSymbol symbol) {
  return symbol >= 0 && symbol < Z_ELF_MATH_COUNT;
}

const char *z_elf_math_symbol_name(ElfMathSymbol symbol) {
  if (!elf_math_symbol_valid(symbol)) return "";
  return math_symbol_names[symbol];
}

ElfMathSymbol z_elf_math_symbol_for_value(IrValueKind kind) {
  switch (kind) {
    case IR_VALUE_MATH_SQRTF: return Z_ELF_MATH_SQRTF;
    case IR_VALUE_MATH_EXPF: return Z_ELF_MATH_EXPF;
    case IR_VALUE_MATH_COSF: return Z_ELF_MATH_COSF;
    case IR_VALUE_MATH_SINF: return Z_ELF_MATH_SINF;
    case IR_VALUE_MATH_POWF: return Z_ELF_MATH_POWF;
    case IR_VALUE_MATH_FABSF: return Z_ELF_MATH_FABSF;
    case IR_VALUE_MATH_FLOORF: return Z_ELF_MATH_FLOORF;
    default: abort();
  }
}

void z_elf_emit_context_free(ElfEmitContext *ctx) {
  if (!ctx) return;
  free(ctx->call_patches);
  free(ctx->rodata_patches);
  for (unsigned i = 0; i < ELF_RUNTIME_HELPER_COUNT; i++) {
    free(ctx->runtime_patches[i].items);
  }
  for (unsigned i = 0; i < Z_ELF_MATH_COUNT; i++) {
    free(ctx->math_patches[i].items);
  }
}

bool z_elf_record_call_patch(ElfEmitContext *ctx, size_t patch_offset, unsigned callee_index, ZDiag *diag, const IrValue *value) {
  if (!ctx || callee_index >= ctx->function_count) {
    return elf_emit_state_diag(diag, "direct ELF64 call target index is out of range", value ? value->line : 1, value ? value->column : 1, "invalid callee");
  }
  if (ctx->call_patch_len + 1 > ctx->call_patch_cap) {
    ctx->call_patch_cap = z_grow_capacity(ctx->call_patch_cap, ctx->call_patch_len + 1, 8);
    ctx->call_patches = z_checked_reallocarray(ctx->call_patches, ctx->call_patch_cap, sizeof(ElfCallPatch));
  }
  ctx->call_patches[ctx->call_patch_len++] = (ElfCallPatch){.patch_offset = patch_offset, .callee_index = callee_index};
  return true;
}

bool z_elf_record_rodata_patch(ElfEmitContext *ctx, size_t patch_offset, unsigned data_offset, ZDiag *diag, const IrValue *value) {
  if (!ctx) return elf_emit_state_diag(diag, "direct ELF64 readonly data patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->rodata_patch_len + 1 > ctx->rodata_patch_cap) {
    ctx->rodata_patch_cap = z_grow_capacity(ctx->rodata_patch_cap, ctx->rodata_patch_len + 1, 8);
    ctx->rodata_patches = z_checked_reallocarray(ctx->rodata_patches, ctx->rodata_patch_cap, sizeof(ElfRodataPatch));
  }
  ctx->rodata_patches[ctx->rodata_patch_len++] = (ElfRodataPatch){.patch_offset = patch_offset, .data_offset = data_offset};
  return true;
}

bool z_elf_record_value_runtime_patch(ElfEmitContext *ctx, ElfRuntimeHelper helper, size_t patch_offset, ZDiag *diag, const IrValue *value) {
  if (!ctx || !elf_runtime_helper_valid(helper)) {
    return elf_emit_state_diag(diag, "direct ELF64 runtime patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  }
  ElfPatchList *list = &ctx->runtime_patches[helper];
  if (list->len + 1 > list->cap) {
    list->cap = z_grow_capacity(list->cap, list->len + 1, 4);
    list->items = z_checked_reallocarray(list->items, list->cap, sizeof(ElfPatch));
  }
  list->items[list->len++] = (ElfPatch){.patch_offset = patch_offset};
  return true;
}

size_t z_elf_runtime_patch_count(const ElfEmitContext *ctx, ElfRuntimeHelper helper) {
  if (!ctx || !elf_runtime_helper_valid(helper)) return 0;
  return ctx->runtime_patches[helper].len;
}

bool z_elf_has_runtime_patches(const ElfEmitContext *ctx) {
  if (!ctx) return false;
  for (unsigned i = 0; i < ELF_RUNTIME_HELPER_COUNT; i++) {
    if (ctx->runtime_patches[i].len > 0) return true;
  }
  return false;
}

void z_elf_patch_call_patches(ZBuf *code, const ElfEmitContext *ctx) {
  for (size_t i = 0; ctx && i < ctx->call_patch_len; i++) {
    const ElfCallPatch *patch = &ctx->call_patches[i];
    z_x64_patch_rel32(code, patch->patch_offset, ctx->function_offsets[patch->callee_index]);
  }
}

void z_elf_patch_rodata_patches(ZBuf *code, const ElfEmitContext *ctx) {
  for (size_t i = 0; ctx && i < ctx->rodata_patch_len; i++) {
    const ElfRodataPatch *patch = &ctx->rodata_patches[i];
    uint64_t addr = ctx->rodata_addr + (patch->data_offset - ctx->rodata_base_offset);
    for (unsigned b = 0; b < 8; b++) {
      code->data[patch->patch_offset + b] = (char)((addr >> (8 * b)) & 0xff);
    }
  }
}

void z_elf_append_rodata_relocations(ZBuf *rela_text, const ElfEmitContext *ctx, uint32_t rodata_symbol) {
  for (size_t i = 0; ctx && i < ctx->rodata_patch_len; i++) {
    z_elf_append_rela(rela_text, ctx->rodata_patches[i].patch_offset, rodata_symbol, 1, ctx->rodata_patches[i].data_offset - ctx->rodata_base_offset);
  }
}

void z_elf_append_runtime_relocations(ZBuf *rela_text, const ElfEmitContext *ctx, ElfRuntimeHelper helper, uint32_t runtime_symbol) {
  if (!ctx || !elf_runtime_helper_valid(helper)) return;
  const ElfPatchList *patches = &ctx->runtime_patches[helper];
  for (size_t i = 0; i < patches->len; i++) {
    z_elf_append_rela(rela_text, patches->items[i].patch_offset, runtime_symbol, 4, -4);
  }
}

bool z_elf_record_math_patch(ElfEmitContext *ctx, ElfMathSymbol symbol, size_t patch_offset, ZDiag *diag, const IrValue *value) {
  if (!ctx || !elf_math_symbol_valid(symbol)) {
    return elf_emit_state_diag(diag, "direct ELF64 math relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  }
  ElfPatchList *list = &ctx->math_patches[symbol];
  if (list->len + 1 > list->cap) {
    list->cap = z_grow_capacity(list->cap, list->len + 1, 4);
    list->items = z_checked_reallocarray(list->items, list->cap, sizeof(ElfPatch));
  }
  list->items[list->len++] = (ElfPatch){.patch_offset = patch_offset};
  return true;
}

// True when at least one call site targets the given libm symbol. Drives which undefined externals
// (and their relocations) are emitted into the object's symbol table.
bool z_elf_math_symbol_used(const ElfEmitContext *ctx, ElfMathSymbol symbol) {
  if (!ctx || !elf_math_symbol_valid(symbol)) return false;
  return ctx->math_patches[symbol].len > 0;
}

size_t z_elf_math_patch_count(const ElfEmitContext *ctx) {
  if (!ctx) return 0;
  size_t count = 0;
  for (unsigned i = 0; i < Z_ELF_MATH_COUNT; i++) count += ctx->math_patches[i].len;
  return count;
}

// One R_X86_64_PLT32 relocation per `call rel32` site targeting `symbol`, against an undefined
// GLOBAL|FUNC libm external. Same PLT32 shape as the runtime-helper externals.
void z_elf_append_math_relocations(ZBuf *rela_text, const ElfEmitContext *ctx, ElfMathSymbol symbol, uint32_t math_symbol) {
  if (!ctx || !elf_math_symbol_valid(symbol)) return;
  const ElfPatchList *patches = &ctx->math_patches[symbol];
  for (size_t i = 0; i < patches->len; i++) {
    z_elf_append_rela(rela_text, patches->items[i].patch_offset, math_symbol, 4, -4);
  }
}
