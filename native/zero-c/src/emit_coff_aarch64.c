#include "zero.h"
#include "aarch64_direct.h"
#include "aarch64_emit.h"
#include "coff_format.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  ZCoffImageDataPatch *items;
  size_t len;
  size_t cap;
} CoffA64DataPatches;

typedef struct {
  size_t *items;
  size_t len;
  size_t cap;
} CoffA64BranchPatches;

typedef struct {
  size_t *items;
  size_t len;
  size_t cap;
} CoffA64MathPatchList;

// One entry per `bl <user_fn>` placeholder recorded by the shared aarch64_direct emitter.
// Exe path resolves via z_aarch64_patch_branch26 after every function is laid out; object path
// emits a R_ARM64_BRANCH26 reloc against the callee's COFF symbol so the linker resolves it.
typedef struct {
  size_t patch_offset;
  unsigned callee_index;
} CoffA64UserCallPatch;

typedef struct {
  CoffA64UserCallPatch *items;
  size_t len;
  size_t cap;
} CoffA64UserCallPatchList;

typedef struct {
  CoffA64DataPatches data;
  CoffA64BranchPatches world_write;
  // One BL-site list per used libm symbol. Each entry is the offset of the BL placeholder
  // that the shared aarch64_direct emitter recorded via record_call_patch; we redirect them at
  // emit-finish to a per-symbol thunk that loads through the msvcrt.dll IAT (exe) or an absolute
  // 64-bit symbol slot patched at link time (object).
  CoffA64MathPatchList math_patches[Z_AARCH64_MATH_SYMBOL_COUNT];
  // BL placeholders for Zero-to-Zero user-function calls.
  CoffA64UserCallPatchList user_call_patches;
} CoffA64EmitState;

static bool coff_a64_diag(ZDiag *diag, const char *message, int line, int column, const char *actual) {
  if (diag) {
    diag->code = 4004;
    diag->line = line > 0 ? line : 1;
    diag->column = column > 0 ? column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct COFF AArch64 object subset");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "unsupported construct");
    snprintf(diag->help, sizeof(diag->help), "choose a supported direct target or restrict this program to AArch64 COFF supported direct-backend constructs");
  }
  return false;
}

static bool coff_a64_record_data_patch(void *user, size_t patch_offset, unsigned data_offset, const IrValue *value, ZDiag *diag) {
  CoffA64EmitState *state = user;
  CoffA64DataPatches *patches = state ? &state->data : NULL;
  if (!patches) return coff_a64_diag(diag, "direct COFF AArch64 readonly data patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (patches->len == patches->cap) {
    patches->cap = z_grow_capacity(patches->cap, patches->len + 1, 8);
    patches->items = z_checked_reallocarray(patches->items, patches->cap, sizeof(ZCoffImageDataPatch));
  }
  if (!patches->items) return coff_a64_diag(diag, "direct COFF AArch64 backend ran out of memory", value ? value->line : 1, value ? value->column : 1, "allocation failed");
  patches->items[patches->len++] = (ZCoffImageDataPatch){.patch_offset = patch_offset, .data_offset = data_offset};
  return true;
}

static void coff_a64_data_patches_free(CoffA64DataPatches *patches) {
  if (!patches) return;
  free(patches->items);
}

static bool coff_a64_record_branch_patch(CoffA64BranchPatches *patches, size_t patch_offset, const IrInstr *instr, ZDiag *diag) {
  if (!patches) return coff_a64_diag(diag, "direct COFF AArch64 branch patch requires an emit context", instr ? instr->line : 1, instr ? instr->column : 1, "missing context");
  if (patches->len == patches->cap) {
    patches->cap = z_grow_capacity(patches->cap, patches->len + 1, 8);
    patches->items = z_checked_reallocarray(patches->items, patches->cap, sizeof(size_t));
  }
  if (!patches->items) return coff_a64_diag(diag, "direct COFF AArch64 backend ran out of memory", instr ? instr->line : 1, instr ? instr->column : 1, "allocation failed");
  patches->items[patches->len++] = patch_offset;
  return true;
}

// Shared aarch64_direct emitter invokes this for each `bl <math>` placeholder. We record
// the BL site offset per symbol so the container can emit a single thunk per used symbol after the
// user functions and patch every BL site to branch into that thunk.
static bool coff_a64_record_call_patch(void *user, size_t patch_offset, ZAArch64MathSymbol symbol, const IrValue *value, ZDiag *diag) {
  CoffA64EmitState *state = user;
  if (!state) return coff_a64_diag(diag, "direct COFF AArch64 libm call patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if ((int)symbol < 0 || (int)symbol >= (int)Z_AARCH64_MATH_SYMBOL_COUNT) return coff_a64_diag(diag, "direct COFF AArch64 libm call patch symbol is out of range", value ? value->line : 1, value ? value->column : 1, "invalid symbol");
  CoffA64MathPatchList *list = &state->math_patches[symbol];
  if (list->len == list->cap) {
    list->cap = z_grow_capacity(list->cap, list->len + 1, 4);
    list->items = z_checked_reallocarray(list->items, list->cap, sizeof(size_t));
  }
  if (!list->items) return coff_a64_diag(diag, "direct COFF AArch64 backend ran out of memory", value ? value->line : 1, value ? value->column : 1, "allocation failed");
  list->items[list->len++] = patch_offset;
  return true;
}

// The shared aarch64_direct emitter invokes this for each `bl <user_fn>` placeholder it
// emits when lowering an IR_VALUE_CALL to a Zero function defined in the same program. Exe path
// resolves the BL by direct branch26 patching to the callee's final text offset; object path
// emits a R_ARM64_BRANCH26 reloc against the callee's COFF symbol index.
static bool coff_a64_record_user_call_patch(void *user, size_t patch_offset, unsigned callee_index, const IrValue *value, ZDiag *diag) {
  CoffA64EmitState *state = user;
  if (!state) return coff_a64_diag(diag, "direct COFF AArch64 user-function call patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  CoffA64UserCallPatchList *list = &state->user_call_patches;
  if (list->len == list->cap) {
    list->cap = z_grow_capacity(list->cap, list->len + 1, 8);
    list->items = z_checked_reallocarray(list->items, list->cap, sizeof(CoffA64UserCallPatch));
  }
  if (!list->items) return coff_a64_diag(diag, "direct COFF AArch64 backend ran out of memory", value ? value->line : 1, value ? value->column : 1, "allocation failed");
  list->items[list->len++] = (CoffA64UserCallPatch){.patch_offset = patch_offset, .callee_index = callee_index};
  return true;
}

static bool coff_a64_math_symbol_used(const CoffA64EmitState *state, ZAArch64MathSymbol symbol) {
  if (!state || (int)symbol < 0 || (int)symbol >= (int)Z_AARCH64_MATH_SYMBOL_COUNT) return false;
  return state->math_patches[symbol].len > 0;
}

// Map each ZAArch64MathSymbol to its Z_COFF_IMPORT_* index in the PE .idata import directory.
// Order MUST mirror the ZAArch64MathSymbol enum (sqrtf, expf, cosf, sinf, powf, fabsf, floorf).
static const unsigned coff_a64_math_import_index[Z_AARCH64_MATH_SYMBOL_COUNT] = {
  Z_COFF_IMPORT_SQRTF, Z_COFF_IMPORT_EXPF, Z_COFF_IMPORT_COSF, Z_COFF_IMPORT_SINF,
  Z_COFF_IMPORT_POWF, Z_COFF_IMPORT_FABSF, Z_COFF_IMPORT_FLOORF,
};

// COFF object-path external symbol names for the same set, indexed by ZAArch64MathSymbol. The host
// linker resolves the R_ARM64_ADDR64 reloc in each math thunk's 8-byte literal slot against these
// undefined external symbols.
static const char *const coff_a64_math_symbol_name[Z_AARCH64_MATH_SYMBOL_COUNT] = {
  "sqrtf", "expf", "cosf", "sinf", "powf", "fabsf", "floorf"
};

static void coff_a64_emit_state_free(CoffA64EmitState *state) {
  if (!state) return;
  coff_a64_data_patches_free(&state->data);
  free(state->world_write.items);
  for (int s = 0; s < (int)Z_AARCH64_MATH_SYMBOL_COUNT; s++) {
    free(state->math_patches[s].items);
  }
  free(state->user_call_patches.items);
}

static void coff_a64_append_object_rodata_relocations(ZBuf *text, ZBuf *relocs, const CoffA64DataPatches *patches, unsigned rodata_base_offset) {
  for (size_t i = 0; patches && i < patches->len; i++) {
    const ZCoffImageDataPatch *patch = &patches->items[i];
    z_coff_patch_u64(text, patch->patch_offset, patch->data_offset - rodata_base_offset);
    z_coff_append_reloc(relocs, (uint32_t)patch->patch_offset, 1u, Z_COFF_RELOC_ARM64_ADDR64);
  }
}

typedef struct {
  ZCoffImportPatch *items;
  size_t len;
  size_t cap;
} CoffA64ImportPatchList;

static void coff_a64_import_patches_push(CoffA64ImportPatchList *list, ZCoffImportPatch patch) {
  if (list->len == list->cap) {
    list->cap = z_grow_capacity(list->cap, list->len + 1, 8);
    list->items = z_checked_reallocarray(list->items, list->cap, sizeof(ZCoffImportPatch));
  }
  list->items[list->len++] = patch;
}

static size_t coff_a64_emit_import_call(ZBuf *text, CoffA64ImportPatchList *patches, unsigned import_index) {
  while (((text->len + 8) % 8) != 0) z_aarch64_emit_nop(text);
  z_aarch64_emit_ldr_x_literal8(text, 16);
  z_aarch64_emit_b_offset_words(text, 3);
  size_t patch = text->len;
  z_aarch64_append_u64(text, 0);
  z_aarch64_emit_load_x_imm(text, 16, 16, 0);
  z_aarch64_emit_blr_x(text, 16);
  if (patches) coff_a64_import_patches_push(patches, (ZCoffImportPatch){.patch_offset = patch, .import_index = import_index});
  return patch;
}

static size_t coff_a64_emit_exe_start_stub(ZBuf *text, CoffA64ImportPatchList *import_patches) {
  z_aarch64_emit_sub_sp_imm(text, 16);
  size_t main_patch = z_aarch64_emit_bl_placeholder(text);
  coff_a64_emit_import_call(text, import_patches, Z_COFF_IMPORT_EXIT_PROCESS);
  z_aarch64_emit_brk(text);
  return main_patch;
}

static bool coff_a64_emit_world_write_call(ZBuf *text, const IrInstr *instr, ZAArch64DirectContext *ctx, ZDiag *diag) {
  CoffA64EmitState *state = ctx ? ctx->patch_user : NULL;
  size_t patch = z_aarch64_emit_bl_placeholder(text);
  if (!coff_a64_record_branch_patch(state ? &state->world_write : NULL, patch, instr, diag)) return false;
  size_t ok_patch = z_aarch64_emit_cbz_w_placeholder(text, 0);
  z_aarch64_emit_brk(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  return true;
}

// `br x16` tail-call (D61F0200): branch to the address in x16 without touching LR. The caller's
// original BL already set LR to the post-call site, so when the libm function returns it returns
// directly into the caller (skipping this thunk). No need to save/restore LR or shadow space.
static void coff_a64_emit_br_x16(ZBuf *text) {
  z_aarch64_append_u32(text, 0xd61f0200u);
}

// Math thunk for the EXE path. Loads the IAT slot RVA from the inline literal (patched by the PE
// writer via ZCoffImportPatch), dereferences it to the libm function address, then tail-calls it.
//
//   math_thunk_for_X:
//     ldr  x16, [pc+8]      ; load literal
//     b    skip             ; jump over the 8-byte literal
//     .quad 0               ; patched with msvcrt!X IAT RVA at PE-write time
//   skip:
//     ldr  x16, [x16]       ; deref IAT slot to function address
//     br   x16              ; tail call
//
// Returns the offset of the 8-byte literal (the ZCoffImportPatch patch_offset).
static size_t coff_a64_emit_math_thunk_exe(ZBuf *text) {
  while (((text->len + 8) % 8) != 0) z_aarch64_emit_nop(text);
  z_aarch64_emit_ldr_x_literal8(text, 16);
  z_aarch64_emit_b_offset_words(text, 3);
  size_t literal_offset = text->len;
  z_aarch64_append_u64(text, 0);
  z_aarch64_emit_load_x_imm(text, 16, 16, 0);
  coff_a64_emit_br_x16(text);
  return literal_offset;
}

// Math thunk for the OBJECT path. Same shape as the EXE thunk, but instead of dereferencing an IAT
// slot at runtime we load the resolved function address from the 8-byte literal directly (linker
// patches the literal at link time via R_ARM64_ADDR64 against the external libm symbol).
//
//   math_thunk_for_X:
//     ldr  x16, [pc+8]      ; load literal = absolute address of libm!X (linker patches)
//     b    skip
//     .quad 0               ; R_ARM64_ADDR64 against `X`
//   skip:
//     br   x16              ; tail call directly — no extra deref
//
// Returns the offset of the 8-byte literal (the R_ARM64_ADDR64 patch site).
static size_t coff_a64_emit_math_thunk_object(ZBuf *text) {
  while (((text->len + 8) % 8) != 0) z_aarch64_emit_nop(text);
  z_aarch64_emit_ldr_x_literal8(text, 16);
  z_aarch64_emit_b_offset_words(text, 3);
  size_t literal_offset = text->len;
  z_aarch64_append_u64(text, 0);
  coff_a64_emit_br_x16(text);
  return literal_offset;
}

static size_t coff_a64_emit_exe_world_write(ZBuf *text, CoffA64ImportPatchList *import_patches) {
  size_t offset = text->len;
  z_aarch64_emit_sub_sp_imm(text, 48);
  z_aarch64_emit_store_x_sp(text, 1, 0);
  z_aarch64_emit_store_x_sp(text, 2, 8);
  z_aarch64_emit_store_w_sp(text, 31, 16);
  z_aarch64_emit_store_x_sp(text, 30, 40);
  z_aarch64_emit_movz_w(text, 8, 2);
  z_aarch64_emit_cmp_w(text, 0, 8);
  z_aarch64_emit_movz_w(text, 0, 0xfffffff5u);
  size_t stdout_patch = z_aarch64_emit_b_cond_placeholder(text, 1);
  z_aarch64_emit_movz_w(text, 0, 0xfffffff4u);
  z_aarch64_patch_cond19(text, stdout_patch, text->len);
  coff_a64_emit_import_call(text, import_patches, Z_COFF_IMPORT_GET_STD_HANDLE);
  z_aarch64_emit_load_x_sp(text, 1, 0);
  z_aarch64_emit_load_w_sp(text, 2, 8);
  z_aarch64_emit_add_x_sp_imm(text, 3, 16);
  z_aarch64_emit_movz_x(text, 4, 0);
  coff_a64_emit_import_call(text, import_patches, Z_COFF_IMPORT_WRITE_FILE);
  z_aarch64_emit_cmp_w(text, 0, 31);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 1);
  z_aarch64_emit_brk(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  z_aarch64_emit_movz_w(text, 0, 0);
  z_aarch64_emit_load_x_sp(text, 30, 40);
  z_aarch64_emit_add_sp_imm(text, 48);
  z_aarch64_emit_ret(text);
  return offset;
}

bool z_emit_coff_aarch64_object_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!program || !out) return coff_a64_diag(diag, "direct COFF AArch64 backend received no program", 1, 1, "missing MIR");
  if (!program->mir_valid) {
    bool ok = coff_a64_diag(diag, program->mir_message[0] ? program->mir_message : "direct backend lowering failed", program->mir_line, program->mir_column, program->mir_actual);
    z_diag_set_backend_blocker(diag, &program->backend_blocker);
    return ok;
  }
  if (program->function_len == 0) return coff_a64_diag(diag, "direct COFF AArch64 object backend requires at least one exported function", 1, 1, "empty program");

  ZBuf text;
  ZBuf rodata;
  ZBuf relocs;
  zbuf_init(&text);
  zbuf_init(&rodata);
  zbuf_init(&relocs);

  bool has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  unsigned rodata_base_offset = z_aarch64_direct_rodata_base_offset(program);
  if (has_rodata) z_aarch64_direct_append_rodata(&rodata, program, rodata_base_offset);

  size_t *offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  // Worst case the program exports every function and uses every libm symbol; size accordingly.
  ZCoffSymbol *symbols = z_checked_calloc(program->function_len + (size_t)Z_AARCH64_MATH_SYMBOL_COUNT, sizeof(ZCoffSymbol));
  if (!offsets || !symbols) {
    free(offsets);
    free(symbols);
    zbuf_free(&relocs);
    zbuf_free(&rodata);
    zbuf_free(&text);
    return coff_a64_diag(diag, "out of memory while emitting COFF AArch64 object", 1, 1, "allocation failed");
  }

  CoffA64EmitState state = {0};
  ZAArch64DirectContext ctx = {
    .program = program,
    .function_offsets = offsets,
    .function_count = program->function_len,
    .rodata_base_offset = rodata_base_offset,
    .patch_user = &state,
    .record_data_patch = coff_a64_record_data_patch,
    // std.math BL placeholders are routed through per-symbol thunks emitted after the user
    // functions; the thunks pull the absolute libm address from an 8-byte literal patched at link
    // time via R_ARM64_ADDR64 against the bare symbol name (e.g. "sqrtf").
    .record_call_patch = coff_a64_record_call_patch,
    // Zero-to-Zero user CALL placeholders are resolved by R_ARM64_BRANCH26 relocations
    // against the callee's COFF symbol (object) or by direct branch26 patching (exe).
    .record_user_call_patch = coff_a64_record_user_call_patch
  };

  // Emit ALL functions whose IR is non-degenerate so a Zero user CALL can bind to a
  // private callee in the same object. Exported fns become EXTERNAL symbols, private fns become
  // STATIC; the COFF symbol index is recorded so the BRANCH26 reloc can target either kind.
  bool has_export = false;
  size_t symbol_len = 0;
  uint32_t *function_symbol_position = z_checked_calloc(program->function_len, sizeof(uint32_t));
  bool *function_emitted = z_checked_calloc(program->function_len, sizeof(bool));
  if (!function_symbol_position || !function_emitted) {
    free(function_symbol_position);
    free(function_emitted);
    coff_a64_emit_state_free(&state);
    free(offsets);
    free(symbols);
    zbuf_free(&relocs);
    zbuf_free(&rodata);
    zbuf_free(&text);
    return coff_a64_diag(diag, "out of memory while emitting COFF AArch64 object", 1, 1, "allocation failed");
  }
  uint32_t section_symbol_count = has_rodata ? 2u : 1u;
  for (size_t i = 0; i < program->function_len; i++) {
    const IrFunction *fun = &program->functions[i];
    bool should_emit = fun->is_exported || fun->instr_len > 0;
    if (!should_emit) continue;
    if (fun->is_exported) has_export = true;
    z_aarch64_pad_to(&text, z_aarch64_align(text.len, 16));
    offsets[i] = text.len;
    if (!z_aarch64_direct_emit_function_text(&text, &program->functions[i], &ctx, diag)) {
      free(function_symbol_position);
      free(function_emitted);
      coff_a64_emit_state_free(&state);
      free(offsets);
      free(symbols);
      zbuf_free(&relocs);
      zbuf_free(&rodata);
      zbuf_free(&text);
      return false;
    }
    function_symbol_position[i] = section_symbol_count + (uint32_t)symbol_len;
    function_emitted[i] = true;
    symbols[symbol_len++] = (ZCoffSymbol){
      .name = program->functions[i].name ? program->functions[i].name : "zero_fn",
      .value = (uint32_t)offsets[i],
      .section_number = 1,
      .type = 0x20,
      .storage_class = fun->is_exported ? Z_COFF_SYMBOL_EXTERNAL : Z_COFF_SYMBOL_STATIC
    };
  }
  if (!has_export) {
    free(function_symbol_position);
    free(function_emitted);
    coff_a64_emit_state_free(&state);
    free(offsets);
    free(symbols);
    zbuf_free(&relocs);
    zbuf_free(&rodata);
    zbuf_free(&text);
    return coff_a64_diag(diag, "direct COFF AArch64 object backend requires at least one exported function", 1, 1, "no exported function");
  }
  if (has_rodata) coff_a64_append_object_rodata_relocations(&text, &relocs, &state.data, rodata_base_offset);

  // Object path: emit a R_ARM64_BRANCH26 reloc for each Zero user CALL placeholder
  // recorded by the shared aarch64_direct emitter. The reloc's symbol index targets the callee's
  // COFF symbol (EXTERNAL or STATIC) recorded in function_symbol_position; the linker resolves
  // it to the final text-section offset.
  size_t text_reloc_count = state.data.len;
  for (size_t i = 0; i < state.user_call_patches.len; i++) {
    unsigned callee = state.user_call_patches.items[i].callee_index;
    if (callee >= program->function_len || !function_emitted[callee]) {
      free(function_symbol_position);
      free(function_emitted);
      coff_a64_emit_state_free(&state);
      free(offsets);
      free(symbols);
      zbuf_free(&relocs);
      zbuf_free(&rodata);
      zbuf_free(&text);
      return coff_a64_diag(diag, "direct COFF AArch64 user-function call targets a function that was not emitted", 1, 1, "missing callee symbol");
    }
    z_coff_append_reloc(&relocs, (uint32_t)state.user_call_patches.items[i].patch_offset, function_symbol_position[callee], Z_COFF_RELOC_ARM64_BRANCH26);
    text_reloc_count++;
  }

  // Object path: emit one math thunk per used libm symbol, redirect every BL placeholder
  // to its thunk via z_aarch64_patch_branch26, and append an undefined external symbol + an
  // R_ARM64_ADDR64 reloc against the thunk's 8-byte literal slot. The linker fills the literal at
  // link time with the absolute address of the libm function; the thunk's `br x16` tail-calls it.
  size_t math_thunk_literal_offset[Z_AARCH64_MATH_SYMBOL_COUNT] = {0};
  for (int s = 0; s < (int)Z_AARCH64_MATH_SYMBOL_COUNT; s++) {
    if (!coff_a64_math_symbol_used(&state, (ZAArch64MathSymbol)s)) continue;
    z_aarch64_pad_to(&text, z_aarch64_align(text.len, 16));
    size_t thunk_offset = text.len;
    math_thunk_literal_offset[s] = coff_a64_emit_math_thunk_object(&text);
    const CoffA64MathPatchList *list = &state.math_patches[s];
    for (size_t i = 0; i < list->len; i++) {
      z_aarch64_patch_branch26(&text, list->items[i], thunk_offset);
    }
  }
  // Math externals are appended to the symbol table after the user functions; each one's .symtab
  // index is the corresponding R_ARM64_ADDR64 reloc target. Append both the reloc and the symbol.
  uint32_t next_symbol = (uint32_t)symbol_len;
  for (int s = 0; s < (int)Z_AARCH64_MATH_SYMBOL_COUNT; s++) {
    if (!coff_a64_math_symbol_used(&state, (ZAArch64MathSymbol)s)) continue;
    z_coff_append_reloc(&relocs, (uint32_t)math_thunk_literal_offset[s], next_symbol, Z_COFF_RELOC_ARM64_ADDR64);
    text_reloc_count++;
    symbols[symbol_len++] = (ZCoffSymbol){
      .name = coff_a64_math_symbol_name[s],
      .value = 0,
      .section_number = 0,
      .type = 0x20,
      .storage_class = Z_COFF_SYMBOL_EXTERNAL
    };
    next_symbol++;
  }

  ZCoffObjectImage image = {
    .machine = Z_COFF_MACHINE_ARM64,
    .text = &text,
    .rodata = has_rodata ? &rodata : NULL,
    .text_relocs = &relocs,
    .text_reloc_count = (uint16_t)text_reloc_count,
    .symbols = symbols,
    .symbol_len = symbol_len
  };
  z_coff_write_object(out, &image);

  free(function_symbol_position);
  free(function_emitted);
  coff_a64_emit_state_free(&state);
  free(offsets);
  free(symbols);
  zbuf_free(&relocs);
  zbuf_free(&rodata);
  zbuf_free(&text);
  return true;
}

bool z_emit_coff_aarch64_exe_from_ir(const IrProgram *program, ZBuf *out, ZDiag *diag) {
  if (!program || !out) return coff_a64_diag(diag, "direct COFF AArch64 executable backend received no program", 1, 1, "missing MIR");
  if (!program->mir_valid) {
    bool ok = coff_a64_diag(diag, program->mir_message[0] ? program->mir_message : "direct backend lowering failed", program->mir_line, program->mir_column, program->mir_actual);
    z_diag_set_backend_blocker(diag, &program->backend_blocker);
    return ok;
  }

  unsigned main_index = 0;
  if (!z_aarch64_direct_find_main(program, &main_index, diag)) return false;

  ZBuf text;
  ZBuf rdata;
  zbuf_init(&text);
  zbuf_init(&rdata);

  bool has_rodata = program->readonly_data_bytes > 0 || program->data_segment_len > 0;
  unsigned rodata_base_offset = z_aarch64_direct_rodata_base_offset(program);
  if (has_rodata) z_aarch64_direct_append_rodata(&rdata, program, rodata_base_offset);

  size_t *offsets = z_checked_calloc(program->function_len, sizeof(size_t));
  bool *function_emitted = z_checked_calloc(program->function_len, sizeof(bool));
  if (!offsets || !function_emitted) {
    free(offsets);
    free(function_emitted);
    zbuf_free(&rdata);
    zbuf_free(&text);
    return coff_a64_diag(diag, "out of memory while emitting COFF AArch64 executable", 1, 1, "allocation failed");
  }

  CoffA64EmitState state = {0};
  ZAArch64DirectContext ctx = {
    .program = program,
    .function_offsets = offsets,
    .function_count = program->function_len,
    .rodata_base_offset = rodata_base_offset,
    .patch_user = &state,
    .record_data_patch = coff_a64_record_data_patch,
    .emit_world_write = coff_a64_emit_world_write_call,
    // std.math BL placeholders recorded here; redirected post-emit to per-symbol thunks
    // that load through the msvcrt.dll IAT and tail-call the libm function via `br x16`.
    .record_call_patch = coff_a64_record_call_patch,
    // Zero-to-Zero user CALL placeholders. Exe path resolves by direct branch26 patching
    // to the callee's final text-section offset (no relocation section involved).
    .record_user_call_patch = coff_a64_record_user_call_patch
  };

  CoffA64ImportPatchList import_patches = {0};
  size_t start_main_patch = coff_a64_emit_exe_start_stub(&text, &import_patches);
  z_aarch64_pad_to(&text, z_aarch64_align(text.len, 16));
  // Emit ALL functions (exported + private with at least one instruction) so user CALLs
  // can bind to a private callee inline. The exe path resolves user calls by direct branch26
  // patching to the callee's final text offset — no relocation section involved.
  for (size_t i = 0; i < program->function_len; i++) {
    const IrFunction *fun = &program->functions[i];
    bool should_emit = fun->is_exported || fun->instr_len > 0;
    if (!should_emit) continue;
    z_aarch64_pad_to(&text, z_aarch64_align(text.len, 16));
    offsets[i] = text.len;
    if (!z_aarch64_direct_emit_function_text(&text, &program->functions[i], &ctx, diag)) {
      free(import_patches.items);
      coff_a64_emit_state_free(&state);
      free(offsets);
      free(function_emitted);
      zbuf_free(&rdata);
      zbuf_free(&text);
      return false;
    }
    function_emitted[i] = true;
  }
  z_aarch64_patch_branch26(&text, start_main_patch, offsets[main_index]);
  // Resolve every recorded Zero-to-Zero user CALL BL placeholder by direct branch26
  // patching to the callee's final text-section offset.
  for (size_t i = 0; i < state.user_call_patches.len; i++) {
    unsigned callee = state.user_call_patches.items[i].callee_index;
    // Mirror the object path's guard: a user CALL placeholder must target a function we actually
    // laid out, else branch26 would patch a bogus 0 offset. Unreachable in practice but fail
    // gracefully rather than emit a 0-offset jump.
    if (callee >= program->function_len || !function_emitted[callee]) {
      free(import_patches.items);
      coff_a64_emit_state_free(&state);
      free(offsets);
      free(function_emitted);
      zbuf_free(&rdata);
      zbuf_free(&text);
      return coff_a64_diag(diag, "direct COFF AArch64 user-function call targets a function that was not emitted", 1, 1, "missing callee symbol");
    }
    z_aarch64_patch_branch26(&text, state.user_call_patches.items[i].patch_offset, offsets[callee]);
  }
  size_t world_write_offset = 0;
  if (state.world_write.len > 0) {
    z_aarch64_pad_to(&text, z_aarch64_align(text.len, 16));
    world_write_offset = coff_a64_emit_exe_world_write(&text, &import_patches);
    for (size_t i = 0; i < state.world_write.len; i++) {
      z_aarch64_patch_branch26(&text, state.world_write.items[i], world_write_offset);
    }
  }

  // Exe path: emit one math thunk per used libm symbol after the user functions and the
  // world.out.write helper. Each thunk's 8-byte literal becomes a ZCoffImportPatch against the
  // corresponding msvcrt.dll IAT slot — the PE writer fills it with the IAT RVA. Then every BL
  // placeholder for this symbol is patched to branch into the thunk via z_aarch64_patch_branch26.
  for (int s = 0; s < (int)Z_AARCH64_MATH_SYMBOL_COUNT; s++) {
    if (!coff_a64_math_symbol_used(&state, (ZAArch64MathSymbol)s)) continue;
    z_aarch64_pad_to(&text, z_aarch64_align(text.len, 16));
    size_t thunk_offset = text.len;
    size_t literal_offset = coff_a64_emit_math_thunk_exe(&text);
    coff_a64_import_patches_push(&import_patches, (ZCoffImportPatch){
      .patch_offset = literal_offset,
      .import_index = coff_a64_math_import_index[s],
    });
    const CoffA64MathPatchList *list = &state.math_patches[s];
    for (size_t i = 0; i < list->len; i++) {
      z_aarch64_patch_branch26(&text, list->items[i], thunk_offset);
    }
  }

  ZCoffExecutableImage image = {
    .machine = Z_COFF_MACHINE_ARM64,
    .image_base = 0x140000000ull,
    .section_alignment = 0x1000,
    .file_alignment = 0x200,
    .text = &text,
    .rdata = &rdata,
    .rodata_base_offset = rodata_base_offset,
    .rodata_patches = state.data.items,
    .rodata_patch_len = state.data.len,
    .import_patches = import_patches.items,
    .import_patch_len = import_patches.len,
  };
  z_coff_write_pe64_executable(out, &image);

  free(import_patches.items);
  coff_a64_emit_state_free(&state);
  free(offsets);
  free(function_emitted);
  zbuf_free(&rdata);
  zbuf_free(&text);
  return true;
}
