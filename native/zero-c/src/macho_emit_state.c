#include "macho_emit_state.h"
#include "macho_format.h"

#include <stdio.h>
#include <stdlib.h>

static const char *const runtime_helper_symbols[MACHO_RUNTIME_HELPER_COUNT] = {
  "_zero_world_write",
  "_zero_json_parse_bytes",
  "_zero_http_fetch_result",
  "_zero_http_result_ok",
  "_zero_http_result_status",
  "_zero_http_result_body_len",
  "_zero_http_result_error",
  "_zero_http_response_len",
  "_zero_http_response_headers_len",
  "_zero_http_response_body_offset",
  "_zero_http_header_value",
  "_zero_http_header_found",
  "_zero_http_header_offset",
  "_zero_http_header_len",
};

static const char *const math_symbol_names[Z_MACHO_MATH_COUNT] = {
  "_sqrtf", "_expf", "_cosf", "_sinf", "_powf", "_fabsf", "_floorf"
};

static const char *const libc_symbol_names[Z_MACHO_LIBC_COUNT] = {
  "_mmap", "_munmap", "_open", "_lseek", "_close"
};

static bool macho_emit_state_diag_at(ZDiag *diag, const char *message, int line, int column, const char *actual) {
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

static bool macho_runtime_helper_valid(MachORuntimeHelper helper) {
  return helper >= 0 && helper < MACHO_RUNTIME_HELPER_COUNT;
}

const char *z_macho_runtime_helper_symbol(MachORuntimeHelper helper) {
  if (!macho_runtime_helper_valid(helper)) return "";
  return runtime_helper_symbols[helper];
}

static bool macho_math_symbol_valid(MachOMathSymbol symbol) {
  return symbol >= 0 && symbol < Z_MACHO_MATH_COUNT;
}

const char *z_macho_math_symbol_name(MachOMathSymbol symbol) {
  if (!macho_math_symbol_valid(symbol)) return "";
  return math_symbol_names[symbol];
}

static bool macho_libc_symbol_valid(MachOLibcSymbol symbol) {
  return symbol >= 0 && symbol < Z_MACHO_LIBC_COUNT;
}

const char *z_macho_libc_symbol_name(MachOLibcSymbol symbol) {
  if (!macho_libc_symbol_valid(symbol)) return "";
  return libc_symbol_names[symbol];
}

MachOMathSymbol z_macho_math_symbol_for_value(IrValueKind kind) {
  switch (kind) {
    case IR_VALUE_MATH_SQRTF: return Z_MACHO_MATH_SQRTF;
    case IR_VALUE_MATH_EXPF: return Z_MACHO_MATH_EXPF;
    case IR_VALUE_MATH_COSF: return Z_MACHO_MATH_COSF;
    case IR_VALUE_MATH_SINF: return Z_MACHO_MATH_SINF;
    case IR_VALUE_MATH_POWF: return Z_MACHO_MATH_POWF;
    case IR_VALUE_MATH_FABSF: return Z_MACHO_MATH_FABSF;
    case IR_VALUE_MATH_FLOORF: return Z_MACHO_MATH_FLOORF;
    default: abort();
  }
}

void z_macho_emit_context_free(MachOEmitContext *ctx) {
  if (!ctx) return;
  for (unsigned i = 0; i < MACHO_RUNTIME_HELPER_COUNT; i++) {
    free(ctx->runtime_patches[i].items);
  }
  free(ctx->math_call_patches);
  free(ctx->libc_call_patches);
  free(ctx->data_patches);
  free(ctx->call_patches);
}

bool z_macho_record_call_patch(MachOEmitContext *ctx, size_t patch_offset, unsigned callee_index, const IrValue *value, ZDiag *diag) {
  if (!ctx || callee_index >= ctx->function_count) {
    return macho_emit_state_diag_at(diag, "direct AArch64 Mach-O call target is out of range", value ? value->line : 1, value ? value->column : 1, "invalid callee");
  }
  if (ctx->call_patch_len == ctx->call_patch_cap) {
    ctx->call_patch_cap = z_grow_capacity(ctx->call_patch_cap, ctx->call_patch_len + 1, 8);
    ctx->call_patches = z_checked_reallocarray(ctx->call_patches, ctx->call_patch_cap, sizeof(MachOCallPatch));
  }
  ctx->call_patches[ctx->call_patch_len++] = (MachOCallPatch){.patch_offset = patch_offset, .callee_index = callee_index, .line = value ? value->line : 1, .column = value ? value->column : 1};
  return true;
}

bool z_macho_record_data_patch(MachOEmitContext *ctx, size_t patch_offset, unsigned data_offset, const IrValue *value, ZDiag *diag) {
  if (!ctx) return macho_emit_state_diag_at(diag, "direct AArch64 Mach-O data relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->data_patch_len == ctx->data_patch_cap) {
    ctx->data_patch_cap = z_grow_capacity(ctx->data_patch_cap, ctx->data_patch_len + 1, 8);
    ctx->data_patches = z_checked_reallocarray(ctx->data_patches, ctx->data_patch_cap, sizeof(MachODataPatch));
  }
  ctx->data_patches[ctx->data_patch_len++] = (MachODataPatch){.patch_offset = patch_offset, .data_offset = data_offset};
  return true;
}

static bool macho_record_runtime_patch_at(MachOEmitContext *ctx, MachORuntimeHelper helper, size_t patch_offset, int line, int column, ZDiag *diag) {
  if (!ctx || !macho_runtime_helper_valid(helper)) {
    return macho_emit_state_diag_at(diag, "direct AArch64 Mach-O runtime relocation requires an emit context", line, column, "missing context");
  }
  MachOPatchList *list = &ctx->runtime_patches[helper];
  if (list->len == list->cap) {
    list->cap = z_grow_capacity(list->cap, list->len + 1, 4);
    list->items = z_checked_reallocarray(list->items, list->cap, sizeof(MachOPatch));
  }
  list->items[list->len++] = (MachOPatch){.patch_offset = patch_offset};
  return true;
}

bool z_macho_record_value_runtime_patch(MachOEmitContext *ctx, MachORuntimeHelper helper, size_t patch_offset, const IrValue *value, ZDiag *diag) {
  return macho_record_runtime_patch_at(ctx, helper, patch_offset, value ? value->line : 1, value ? value->column : 1, diag);
}

bool z_macho_record_instr_runtime_patch(MachOEmitContext *ctx, MachORuntimeHelper helper, size_t patch_offset, const IrInstr *instr, ZDiag *diag) {
  return macho_record_runtime_patch_at(ctx, helper, patch_offset, instr ? instr->line : 1, instr ? instr->column : 1, diag);
}

size_t z_macho_runtime_patch_count(const MachOEmitContext *ctx, MachORuntimeHelper helper) {
  if (!ctx || !macho_runtime_helper_valid(helper)) return 0;
  return ctx->runtime_patches[helper].len;
}

const MachOPatchList *z_macho_runtime_patch_list(const MachOEmitContext *ctx, MachORuntimeHelper helper) {
  if (!ctx || !macho_runtime_helper_valid(helper)) return NULL;
  return &ctx->runtime_patches[helper];
}

bool z_macho_record_math_call_patch(MachOEmitContext *ctx, size_t patch_offset, MachOMathSymbol symbol, const IrValue *value, ZDiag *diag) {
  if (!ctx || !macho_math_symbol_valid(symbol)) {
    return macho_emit_state_diag_at(diag, "direct Mach-O math relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  }
  if (ctx->math_call_patch_len == ctx->math_call_patch_cap) {
    ctx->math_call_patch_cap = z_grow_capacity(ctx->math_call_patch_cap, ctx->math_call_patch_len + 1, 4);
    ctx->math_call_patches = z_checked_reallocarray(ctx->math_call_patches, ctx->math_call_patch_cap, sizeof(MachOMathCallPatch));
  }
  ctx->math_call_patches[ctx->math_call_patch_len++] = (MachOMathCallPatch){.patch_offset = patch_offset, .symbol = symbol};
  return true;
}

// True when at least one call site targets the given libm symbol. Drives which undefined
// externals (and their relocations) are emitted into the object's symbol table.
bool z_macho_math_symbol_used(const MachOEmitContext *ctx, MachOMathSymbol symbol) {
  for (size_t i = 0; ctx && i < ctx->math_call_patch_len; i++) {
    if (ctx->math_call_patches[i].symbol == symbol) return true;
  }
  return false;
}

size_t z_macho_math_call_patch_count(const MachOEmitContext *ctx) {
  return ctx ? ctx->math_call_patch_len : 0;
}

bool z_macho_record_libc_call_patch(MachOEmitContext *ctx, size_t patch_offset, MachOLibcSymbol symbol, const IrValue *value, ZDiag *diag) {
  if (!ctx || !macho_libc_symbol_valid(symbol)) {
    return macho_emit_state_diag_at(diag, "direct Mach-O libSystem relocation requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  }
  if (ctx->libc_call_patch_len == ctx->libc_call_patch_cap) {
    ctx->libc_call_patch_cap = z_grow_capacity(ctx->libc_call_patch_cap, ctx->libc_call_patch_len + 1, 4);
    ctx->libc_call_patches = z_checked_reallocarray(ctx->libc_call_patches, ctx->libc_call_patch_cap, sizeof(MachOLibcCallPatch));
  }
  ctx->libc_call_patches[ctx->libc_call_patch_len++] = (MachOLibcCallPatch){.patch_offset = patch_offset, .symbol = symbol};
  return true;
}

// True when at least one call site targets the given libSystem symbol. Drives which undefined
// externals (and their relocations) are emitted into the object's symbol table.
bool z_macho_libc_symbol_used(const MachOEmitContext *ctx, MachOLibcSymbol symbol) {
  for (size_t i = 0; ctx && i < ctx->libc_call_patch_len; i++) {
    if (ctx->libc_call_patches[i].symbol == symbol) return true;
  }
  return false;
}

size_t z_macho_libc_call_patch_count(const MachOEmitContext *ctx) {
  return ctx ? ctx->libc_call_patch_len : 0;
}

// One BRANCH26 external relocation per `bl <libc>` site that targets `symbol`. Same external
// call-relocation shape as the runtime-helper and libm symbols.
void z_macho_append_libc_call_relocations(ZBuf *relocs, const MachOEmitContext *ctx, MachOLibcSymbol symbol, unsigned symbol_index) {
  for (size_t i = 0; ctx && i < ctx->libc_call_patch_len; i++) {
    const MachOLibcCallPatch *patch = &ctx->libc_call_patches[i];
    if (patch->symbol != symbol) continue;
    uint32_t reloc_info = (symbol_index & 0x00ffffffu) |
                          (1u << 24) |  // r_pcrel
                          (2u << 25) |  // r_length: 4 bytes
                          (1u << 27) |  // r_extern: symbol table index
                          (2u << 28);   // ARM64_RELOC_BRANCH26
    z_macho_append_u32(relocs, (uint32_t)patch->patch_offset);
    z_macho_append_u32(relocs, reloc_info);
  }
}

bool z_macho_has_unsupported_exe_runtime_patches(const MachOEmitContext *ctx) {
  if (!ctx) return false;
  for (unsigned i = 0; i < MACHO_RUNTIME_HELPER_COUNT; i++) {
    if (i == MACHO_RUNTIME_WORLD_WRITE) continue;
    if (ctx->runtime_patches[i].len > 0) return true;
  }
  return false;
}

static void macho_append_branch_relocations(ZBuf *relocs, const MachOPatchList *patches, unsigned symbol_index) {
  for (size_t i = 0; patches && i < patches->len; i++) {
    uint32_t reloc_info = (symbol_index & 0x00ffffffu) |
                          (1u << 24) |
                          (2u << 25) |
                          (1u << 27) |
                          (2u << 28);
    z_macho_append_u32(relocs, (uint32_t)patches->items[i].patch_offset);
    z_macho_append_u32(relocs, reloc_info);
  }
}

void z_macho_append_call_relocations(ZBuf *relocs, const MachOEmitContext *ctx) {
  for (size_t i = 0; ctx && i < ctx->call_patch_len; i++) {
    const MachOCallPatch *patch = &ctx->call_patches[i];
    uint32_t reloc_info = (patch->callee_index & 0x00ffffffu) |
                          (1u << 24) |
                          (2u << 25) |
                          (1u << 27) |
                          (2u << 28);
    z_macho_append_u32(relocs, (uint32_t)patch->patch_offset);
    z_macho_append_u32(relocs, reloc_info);
  }
}

void z_macho_append_runtime_relocations(ZBuf *relocs, const MachOEmitContext *ctx, MachORuntimeHelper helper, unsigned symbol_index) {
  macho_append_branch_relocations(relocs, z_macho_runtime_patch_list(ctx, helper), symbol_index);
}

// One BRANCH26 external relocation per `bl <libm>` site that targets `symbol`. Same external
// call-relocation shape as the runtime-helper symbols.
void z_macho_append_math_call_relocations(ZBuf *relocs, const MachOEmitContext *ctx, MachOMathSymbol symbol, unsigned symbol_index) {
  for (size_t i = 0; ctx && i < ctx->math_call_patch_len; i++) {
    const MachOMathCallPatch *patch = &ctx->math_call_patches[i];
    if (patch->symbol != symbol) continue;
    uint32_t reloc_info = (symbol_index & 0x00ffffffu) |
                          (1u << 24) |  // r_pcrel
                          (2u << 25) |  // r_length: 4 bytes
                          (1u << 27) |  // r_extern: symbol table index
                          (2u << 28);   // ARM64_RELOC_BRANCH26
    z_macho_append_u32(relocs, (uint32_t)patch->patch_offset);
    z_macho_append_u32(relocs, reloc_info);
  }
}

size_t z_macho_data_relocation_count(const MachOEmitContext *ctx) {
  if (!ctx) return 0;
  if (!ctx->pie_relative_data) return ctx->data_patch_len;
  size_t count = ctx->data_patch_len * 2;
  for (size_t i = 0; i < ctx->data_patch_len; i++) {
    const MachODataPatch *patch = &ctx->data_patches[i];
    if (patch->data_offset != ctx->rodata_base_offset) count += 2;
  }
  return count;
}

size_t z_macho_text_relocation_count(const MachOEmitContext *ctx) {
  if (!ctx) return 0;
  size_t count = ctx->call_patch_len + z_macho_data_relocation_count(ctx);
  for (unsigned i = 0; i < MACHO_RUNTIME_HELPER_COUNT; i++) {
    count += ctx->runtime_patches[i].len;
  }
  count += ctx->math_call_patch_len;
  count += ctx->libc_call_patch_len;
  return count;
}

static void macho_append_reloc(ZBuf *relocs, uint32_t address, uint32_t symbol_or_addend, bool pcrel, unsigned length, bool external, unsigned type) {
  uint32_t reloc_info = (symbol_or_addend & 0x00ffffffu) |
                        ((pcrel ? 1u : 0u) << 24) |
                        ((length & 3u) << 25) |
                        ((external ? 1u : 0u) << 27) |
                        ((type & 15u) << 28);
  z_macho_append_u32(relocs, address);
  z_macho_append_u32(relocs, reloc_info);
}

void z_macho_append_data_relocations(ZBuf *relocs, const MachOEmitContext *ctx, unsigned data_symbol_index) {
  for (size_t i = 0; ctx && i < ctx->data_patch_len; i++) {
    const MachODataPatch *patch = &ctx->data_patches[i];
    if (ctx->pie_relative_data) {
      uint32_t addend = patch->data_offset - ctx->rodata_base_offset;
      if (addend != 0) macho_append_reloc(relocs, (uint32_t)patch->patch_offset + 4u, addend, false, 2, false, 10);
      macho_append_reloc(relocs, (uint32_t)patch->patch_offset + 4u, data_symbol_index, false, 2, true, 4);
      if (addend != 0) macho_append_reloc(relocs, (uint32_t)patch->patch_offset, addend, false, 2, false, 10);
      macho_append_reloc(relocs, (uint32_t)patch->patch_offset, data_symbol_index, true, 2, true, 3);
    } else {
      macho_append_reloc(relocs, (uint32_t)patch->patch_offset, data_symbol_index, false, 3, true, 0);
    }
  }
}
