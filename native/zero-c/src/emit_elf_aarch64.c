#include "zero.h"
#include "aarch64_direct.h"
#include "aarch64_emit.h"
#include "elf_format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  R_AARCH64_ABS64 = 257u,
  R_AARCH64_CALL26 = 283u
};

// Bare ELF names for the libm externals, indexed by ZAArch64MathSymbol. The host linker binds the
// CALL26 relocations against these undefined GLOBAL|FUNC symbols (no leading underscore on ELF).
static const char *const a64_elf_math_symbol_names[Z_AARCH64_MATH_SYMBOL_COUNT] = {
  "sqrtf", "expf", "cosf", "sinf", "powf", "fabsf", "floorf"
};

typedef struct {
  size_t patch_offset;
  unsigned data_offset;
} A64ElfDataPatch;

typedef struct {
  size_t patch_offset;        // the `bl` site that branches to the libm symbol
  ZAArch64MathSymbol symbol;
} A64ElfCallPatch;

typedef struct {
  size_t patch_offset;        // the `bl` site that branches to a user function
  unsigned callee_index;      // IR function index
} A64ElfUserCallPatch;

typedef struct {
  A64ElfDataPatch *data_patches;
  size_t data_patch_len;
  size_t data_patch_cap;
  A64ElfCallPatch *call_patches;
  size_t call_patch_len;
  size_t call_patch_cap;
  A64ElfUserCallPatch *user_call_patches;
  size_t user_call_patch_len;
  size_t user_call_patch_cap;
} A64ElfPatchContext;

static bool a64_diag(ZDiag *diag, const char *message, int line, int column, const char *actual) {
  if (diag) {
    diag->code = 4004;
    diag->line = line > 0 ? line : 1;
    diag->column = column > 0 ? column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "direct AArch64 ELF backend subset");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "unsupported construct");
    snprintf(diag->help, sizeof(diag->help), "choose a supported direct target or restrict this program to AArch64 ELF supported direct-backend constructs");
  }
  return false;
}

static bool a64_elf_record_data_patch(void *user, size_t patch_offset, unsigned data_offset, const IrValue *value, ZDiag *diag) {
  A64ElfPatchContext *ctx = user;
  if (!ctx) return a64_diag(diag, "direct AArch64 ELF readonly data patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->data_patch_len + 1 > ctx->data_patch_cap) {
    ctx->data_patch_cap = z_grow_capacity(ctx->data_patch_cap, ctx->data_patch_len + 1, 8);
    ctx->data_patches = z_checked_reallocarray(ctx->data_patches, ctx->data_patch_cap, sizeof(A64ElfDataPatch));
  }
  if (!ctx->data_patches) return a64_diag(diag, "direct AArch64 ELF backend ran out of memory", value ? value->line : 1, value ? value->column : 1, "allocation failed");
  ctx->data_patches[ctx->data_patch_len++] = (A64ElfDataPatch){.patch_offset = patch_offset, .data_offset = data_offset};
  return true;
}

static bool a64_elf_record_call_patch(void *user, size_t patch_offset, ZAArch64MathSymbol symbol, const IrValue *value, ZDiag *diag) {
  A64ElfPatchContext *ctx = user;
  if (!ctx) return a64_diag(diag, "direct AArch64 ELF libm call patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->call_patch_len + 1 > ctx->call_patch_cap) {
    ctx->call_patch_cap = z_grow_capacity(ctx->call_patch_cap, ctx->call_patch_len + 1, 8);
    ctx->call_patches = z_checked_reallocarray(ctx->call_patches, ctx->call_patch_cap, sizeof(A64ElfCallPatch));
  }
  if (!ctx->call_patches) return a64_diag(diag, "direct AArch64 ELF backend ran out of memory", value ? value->line : 1, value ? value->column : 1, "allocation failed");
  ctx->call_patches[ctx->call_patch_len++] = (A64ElfCallPatch){.patch_offset = patch_offset, .symbol = symbol};
  return true;
}

static bool a64_elf_record_user_call_patch(void *user, size_t patch_offset, unsigned callee_index, const IrValue *value, ZDiag *diag) {
  A64ElfPatchContext *ctx = user;
  if (!ctx) return a64_diag(diag, "direct AArch64 ELF user-function call patch requires an emit context", value ? value->line : 1, value ? value->column : 1, "missing context");
  if (ctx->user_call_patch_len + 1 > ctx->user_call_patch_cap) {
    ctx->user_call_patch_cap = z_grow_capacity(ctx->user_call_patch_cap, ctx->user_call_patch_len + 1, 8);
    ctx->user_call_patches = z_checked_reallocarray(ctx->user_call_patches, ctx->user_call_patch_cap, sizeof(A64ElfUserCallPatch));
  }
  if (!ctx->user_call_patches) return a64_diag(diag, "direct AArch64 ELF backend ran out of memory", value ? value->line : 1, value ? value->column : 1, "allocation failed");
  ctx->user_call_patches[ctx->user_call_patch_len++] = (A64ElfUserCallPatch){.patch_offset = patch_offset, .callee_index = callee_index};
  return true;
}

static bool a64_elf_math_symbol_used(const A64ElfPatchContext *ctx, ZAArch64MathSymbol symbol) {
  for (size_t i = 0; ctx && i < ctx->call_patch_len; i++) {
    if (ctx->call_patches[i].symbol == symbol) return true;
  }
  return false;
}

static void a64_elf_patch_context_free(A64ElfPatchContext *ctx) {
  if (!ctx) return;
  free(ctx->data_patches);
  free(ctx->call_patches);
  free(ctx->user_call_patches);
}

static bool a64_elf_emit_world_write(ZBuf *text, const IrInstr *instr, ZAArch64DirectContext *ctx, ZDiag *diag) {
  (void)ctx;
  z_aarch64_emit_movz_x(text, 8, 64); // Linux SYS_write(fd=x0, buf=x1, len=x2)
  z_aarch64_emit_svc(text, 0);
  z_aarch64_emit_cmp_x(text, 0, 31);
  size_t ok_patch = z_aarch64_emit_b_cond_placeholder(text, 10);
  z_aarch64_emit_brk(text);
  z_aarch64_patch_cond19(text, ok_patch, text->len);
  (void)instr;
  (void)diag;
  return true;
}

size_t z_elf_aarch64_stack_bytes_from_ir(const IrProgram *program) {
  return z_aarch64_direct_stack_bytes_from_ir(program);
}

size_t z_elf_aarch64_max_frame_bytes_from_ir(const IrProgram *program) {
  return z_aarch64_direct_max_frame_bytes_from_ir(program);
}

static void a64_patch_data_patches(ZBuf *text, const A64ElfPatchContext *patches, uint64_t rodata_addr, unsigned rodata_base_offset) {
  for (size_t i = 0; patches && i < patches->data_patch_len; i++) {
    const A64ElfDataPatch *patch = &patches->data_patches[i];
    uint64_t addr = rodata_addr + (patch->data_offset - rodata_base_offset);
    z_aarch64_patch_u64(text, patch->patch_offset, addr);
  }
}

static void a64_append_data_relocations(ZBuf *rela_text, const A64ElfPatchContext *patches, unsigned rodata_base_offset, uint32_t rodata_symbol) {
  for (size_t i = 0; patches && i < patches->data_patch_len; i++) {
    const A64ElfDataPatch *patch = &patches->data_patches[i];
    z_elf_append_rela(rela_text, patch->patch_offset, rodata_symbol, R_AARCH64_ABS64, patch->data_offset - rodata_base_offset);
  }
}

bool z_emit_elf_aarch64_object_from_ir(const IrProgram *ir, ZBuf *out, ZDiag *diag) {
  if (!ir) return a64_diag(diag, "direct AArch64 ELF object backend requires MIR", 1, 1, "missing MIR");
  if (!ir->mir_valid) {
    bool ok = a64_diag(diag, ir->mir_message[0] ? ir->mir_message : "direct backend lowering failed", ir->mir_line, ir->mir_column, ir->mir_actual);
    z_diag_set_backend_blocker(diag, &ir->backend_blocker);
    return ok;
  }

  ZBuf text;
  ZBuf rodata;
  ZBuf rela_text;
  ZBuf strtab;
  ZBuf symtab;
  zbuf_init(&text);
  zbuf_init(&rodata);
  zbuf_init(&rela_text);
  zbuf_init(&strtab);
  zbuf_init(&symtab);
  z_elf_append_u8(&strtab, 0);
  z_elf_append_zeros(&symtab, 24);

  bool has_rodata = ir->readonly_data_bytes > 0 || ir->data_segment_len > 0;
  unsigned rodata_base_offset = z_aarch64_direct_rodata_base_offset(ir);
  if (has_rodata) {
    z_aarch64_direct_append_rodata(&rodata, ir, rodata_base_offset);
    z_elf_append_symbol(&symtab, 0, 0x03, 2, 0, 0);
  }

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
    return a64_diag(diag, "direct AArch64 ELF object backend ran out of memory", 1, 1, "allocation failed");
  }
  A64ElfPatchContext patch_ctx = {0};
  ZAArch64DirectContext ctx = {
    .program = ir,
    .function_offsets = function_offsets,
    .function_count = ir->function_len,
    .rodata_base_offset = rodata_base_offset,
    .patch_user = &patch_ctx,
    .record_data_patch = a64_elf_record_data_patch,
    // The object path can be linked into an executable (obj+link, e.g. std.math programs); world
    // write lowers to the same inline svc helper the exe path uses, so wire it here too.
    .emit_world_write = a64_elf_emit_world_write,
    .record_call_patch = a64_elf_record_call_patch,
    .record_user_call_patch = a64_elf_record_user_call_patch,
    // The obj+link path is reached by a Zero `main` linked against musl crt0, which
    // invokes `main(argc, argv)` via the AAPCS C calling convention — argc in x0, argv in x1
    // already. Still seed the prologue x20/x21 capture so std.args.{len,get} reads them.
    .seed_main_process_args = true
  };

  // Emit ALL functions whose IR is non-degenerate: an exported function always emits; a non-exported
  // (private) function emits when it has at least one instruction (others are dead). This lets a Zero
  // user CALL bind to a private callee in the same object; the callee gets a STB_LOCAL symbol with
  // STT_FUNC type so the linker can resolve R_AARCH64_CALL26 against it without leaking visibility.
  bool has_export = false;
  bool *function_emitted = z_checked_calloc(ir->function_len, sizeof(bool));
  if (!function_emitted) {
    a64_elf_patch_context_free(&patch_ctx);
    free(function_offsets);
    free(function_sizes);
    free(symbol_names);
    zbuf_free(&text);
    zbuf_free(&rodata);
    zbuf_free(&rela_text);
    zbuf_free(&strtab);
    zbuf_free(&symtab);
    return a64_diag(diag, "direct AArch64 ELF object backend ran out of memory", 1, 1, "allocation failed");
  }
  for (size_t i = 0; i < ir->function_len; i++) {
    const IrFunction *fun = &ir->functions[i];
    bool should_emit = fun->is_exported || fun->instr_len > 0;
    if (!should_emit) continue;
    if (fun->is_exported) has_export = true;
    z_aarch64_pad_to(&text, z_aarch64_align(text.len, 16));
    function_offsets[i] = text.len;
    if (!z_aarch64_direct_emit_function_text(&text, fun, &ctx, diag)) {
      a64_elf_patch_context_free(&patch_ctx);
      free(function_offsets);
      free(function_sizes);
      free(symbol_names);
      free(function_emitted);
      zbuf_free(&text);
      zbuf_free(&rodata);
      zbuf_free(&rela_text);
      zbuf_free(&strtab);
      zbuf_free(&symtab);
      return false;
    }
    function_sizes[i] = text.len - function_offsets[i];
    symbol_names[i] = (uint32_t)strtab.len;
    zbuf_append(&strtab, fun->name ? fun->name : "zero_fn");
    z_elf_append_u8(&strtab, 0);
    function_emitted[i] = true;
  }
  if (!has_export) {
    a64_elf_patch_context_free(&patch_ctx);
    free(function_offsets);
    free(function_sizes);
    free(symbol_names);
    free(function_emitted);
    zbuf_free(&text);
    zbuf_free(&rodata);
    zbuf_free(&rela_text);
    zbuf_free(&strtab);
    zbuf_free(&symtab);
    return a64_diag(diag, "direct AArch64 ELF object backend requires at least one exported function", 1, 1, "no exported function");
  }

  if (has_rodata) a64_append_data_relocations(&rela_text, &patch_ctx, rodata_base_offset, 1);

  // Undefined external libm symbols (bare ELF names) are appended to .strtab after the function
  // names; their .symtab indices follow the function symbols (and the optional .rodata section
  // symbol, which is local index 1). Record each used math symbol's strtab name offset.
  uint32_t math_symbol_names[Z_AARCH64_MATH_SYMBOL_COUNT];
  for (int s = 0; s < Z_AARCH64_MATH_SYMBOL_COUNT; s++) {
    math_symbol_names[s] = 0;
    if (a64_elf_math_symbol_used(&patch_ctx, (ZAArch64MathSymbol)s)) {
      math_symbol_names[s] = (uint32_t)strtab.len;
      zbuf_append(&strtab, a64_elf_math_symbol_names[s]);
      z_elf_append_u8(&strtab, 0);
    }
  }

  // Symbol-table layout (ELF rule = LOCAL symbols precede GLOBAL/UNDEF):
  //   0: null
  //   [1]: optional .rodata section symbol (LOCAL)
  //   then all LOCAL function symbols (private fns) in source order
  //   then all GLOBAL function symbols (exported fns) in source order
  //   then UNDEF GLOBAL math externals in math-enum order
  // The CALL26 reloc target index depends on this final ordering, so build a per-function symbol
  // index map after the layout is fixed.
  uint32_t *function_symbol_index = z_checked_calloc(ir->function_len, sizeof(uint32_t));
  if (!function_symbol_index) {
    a64_elf_patch_context_free(&patch_ctx);
    free(function_offsets);
    free(function_sizes);
    free(symbol_names);
    free(function_emitted);
    zbuf_free(&text);
    zbuf_free(&rodata);
    zbuf_free(&rela_text);
    zbuf_free(&strtab);
    zbuf_free(&symtab);
    return a64_diag(diag, "direct AArch64 ELF object backend ran out of memory", 1, 1, "allocation failed");
  }
  uint32_t next_symbol = has_rodata ? 2u : 1u;
  size_t local_function_count = 0;
  for (size_t i = 0; i < ir->function_len; i++) {
    if (function_emitted[i] && !ir->functions[i].is_exported) {
      function_symbol_index[i] = next_symbol++;
      local_function_count++;
    }
  }
  for (size_t i = 0; i < ir->function_len; i++) {
    if (function_emitted[i] && ir->functions[i].is_exported) {
      function_symbol_index[i] = next_symbol++;
    }
  }
  uint32_t math_symbol_index[Z_AARCH64_MATH_SYMBOL_COUNT];
  for (int s = 0; s < Z_AARCH64_MATH_SYMBOL_COUNT; s++) {
    math_symbol_index[s] = 0;
    if (a64_elf_math_symbol_used(&patch_ctx, (ZAArch64MathSymbol)s)) math_symbol_index[s] = next_symbol++;
  }
  for (size_t i = 0; i < patch_ctx.call_patch_len; i++) {
    z_elf_append_rela(&rela_text, patch_ctx.call_patches[i].patch_offset, math_symbol_index[patch_ctx.call_patches[i].symbol], R_AARCH64_CALL26, 0);
  }
  for (size_t i = 0; i < patch_ctx.user_call_patch_len; i++) {
    unsigned callee = patch_ctx.user_call_patches[i].callee_index;
    z_elf_append_rela(&rela_text, patch_ctx.user_call_patches[i].patch_offset, function_symbol_index[callee], R_AARCH64_CALL26, 0);
  }

  // Emit LOCAL function symbols first (STB_LOCAL|STT_FUNC = 0x02).
  for (size_t i = 0; i < ir->function_len; i++) {
    if (function_emitted[i] && !ir->functions[i].is_exported) {
      z_elf_append_symbol(&symtab, symbol_names[i], 0x02, 1, function_offsets[i], function_sizes[i]);
    }
  }
  // Then GLOBAL function symbols (STB_GLOBAL|STT_FUNC = 0x12).
  for (size_t i = 0; i < ir->function_len; i++) {
    if (function_emitted[i] && ir->functions[i].is_exported) {
      z_elf_append_symbol(&symtab, symbol_names[i], 0x12, 1, function_offsets[i], function_sizes[i]);
    }
  }
  // Undefined GLOBAL|FUNC libm externals (0x12, shndx 0), emitted in symbol-index order.
  for (int s = 0; s < Z_AARCH64_MATH_SYMBOL_COUNT; s++) {
    if (a64_elf_math_symbol_used(&patch_ctx, (ZAArch64MathSymbol)s)) z_elf_append_symbol(&symtab, math_symbol_names[s], 0x12, 0, 0, 0);
  }

  uint32_t local_symbol_count = (has_rodata ? 2u : 1u) + (uint32_t)local_function_count;
  ZElfObjectImage image = {
    .machine = Z_ELF_MACHINE_AARCH64,
    .text = &text,
    .text_align = 16,
    .rodata = has_rodata ? &rodata : NULL,
    .rodata_align = 8,
    .rela_text = rela_text.len > 0 ? &rela_text : NULL,
    .symtab = &symtab,
    .strtab = &strtab,
    .local_symbol_count = local_symbol_count
  };
  z_elf_write_object64(out, &image);
  free(function_emitted);
  free(function_symbol_index);

  a64_elf_patch_context_free(&patch_ctx);
  free(function_offsets);
  free(function_sizes);
  free(symbol_names);
  zbuf_free(&text);
  zbuf_free(&rodata);
  zbuf_free(&rela_text);
  zbuf_free(&strtab);
  zbuf_free(&symtab);
  return true;
}

bool z_emit_elf_aarch64_exe_from_ir(const IrProgram *ir, ZBuf *out, ZDiag *diag) {
  if (!ir) return a64_diag(diag, "direct AArch64 ELF executable backend requires MIR", 1, 1, "missing MIR");
  if (!ir->mir_valid) {
    bool ok = a64_diag(diag, ir->mir_message[0] ? ir->mir_message : "direct backend lowering failed", ir->mir_line, ir->mir_column, ir->mir_actual);
    z_diag_set_backend_blocker(diag, &ir->backend_blocker);
    return ok;
  }
  unsigned main_index = 0;
  if (!z_aarch64_direct_find_main(ir, &main_index, diag)) return false;

  ZBuf text;
  ZBuf rodata;
  zbuf_init(&text);
  zbuf_init(&rodata);
  bool has_rodata = ir->readonly_data_bytes > 0 || ir->data_segment_len > 0;
  unsigned rodata_base_offset = z_aarch64_direct_rodata_base_offset(ir);
  if (has_rodata) z_aarch64_direct_append_rodata(&rodata, ir, rodata_base_offset);

  // The kernel hands `_start` argc at [sp] and argv (the char** array) at sp+8. Seed x0
  // and x1 to mirror the AAPCS argc/argv calling convention before branching into `main`; the
  // main prologue stashes them into x20/x21 so std.args.{len,get} can read them anywhere in the
  // body. Without this seed, x0/x1 hold whatever transient values the kernel left and the
  // backend would silently use garbage for argc/argv.
  z_aarch64_emit_load_x_imm(&text, 0, 31, 0); // x0 = argc = [sp]
  z_aarch64_emit_add_x_imm(&text, 1, 31, 8);  // x1 = argv = sp + 8
  size_t start_call_patch = z_aarch64_emit_bl_placeholder(&text);
  z_aarch64_emit_movz_x(&text, 8, 93);
  z_aarch64_emit_svc(&text, 0);
  z_aarch64_pad_to(&text, z_aarch64_align(text.len, 16));

  size_t *function_offsets = z_checked_calloc(ir->function_len, sizeof(size_t));
  bool *function_emitted = z_checked_calloc(ir->function_len, sizeof(bool));
  if (!function_offsets || !function_emitted) {
    free(function_offsets);
    free(function_emitted);
    zbuf_free(&text);
    zbuf_free(&rodata);
    return a64_diag(diag, "direct AArch64 ELF executable backend ran out of memory", 1, 1, "allocation failed");
  }
  A64ElfPatchContext patch_ctx = {0};
  ZAArch64DirectContext ctx = {
    .program = ir,
    .function_offsets = function_offsets,
    .function_count = ir->function_len,
    .rodata_base_offset = rodata_base_offset,
    .patch_user = &patch_ctx,
    .record_data_patch = a64_elf_record_data_patch,
    .emit_world_write = a64_elf_emit_world_write,
    .record_call_patch = a64_elf_record_call_patch,
    .record_user_call_patch = a64_elf_record_user_call_patch,
    // The exe path entry stub above puts argc/argv in x0/x1 then branches to main —
    // request the main-function argv seed so the prologue captures them into x20/x21.
    .seed_main_process_args = true
  };
  // Emit ALL functions (exported + private with at least one instruction) so user CALLs can bind
  // to private callees inline. The exe path resolves user calls by direct branch26 patching to the
  // callee's final text offset — no relocation section involved.
  for (size_t i = 0; i < ir->function_len; i++) {
    const IrFunction *fun = &ir->functions[i];
    bool should_emit = fun->is_exported || fun->instr_len > 0;
    if (!should_emit) continue;
    z_aarch64_pad_to(&text, z_aarch64_align(text.len, 16));
    function_offsets[i] = text.len;
    if (!z_aarch64_direct_emit_function_text(&text, fun, &ctx, diag)) {
      a64_elf_patch_context_free(&patch_ctx);
      free(function_offsets);
      free(function_emitted);
      zbuf_free(&text);
      zbuf_free(&rodata);
      return false;
    }
    function_emitted[i] = true;
  }
  // A static ELF executable has no dynamic linker to bind libm: any std.math.* recorded a `bl #0`
  // libm placeholder (call_patches), and unlike the object path there is no R_AARCH64_CALL26 to
  // hand a linker. Emitting anyway would silently produce a binary that jumps to offset 0. Refuse
  // up front and point at the object path, which resolves these via relocations.
  if (patch_ctx.call_patch_len > 0) {
    a64_elf_patch_context_free(&patch_ctx);
    free(function_offsets);
    free(function_emitted);
    zbuf_free(&text);
    zbuf_free(&rodata);
    return a64_diag(diag, "direct AArch64 ELF executable cannot bind libm (std.math); use obj+link instead", 1, 1, "no dynamic linker for libm in a static executable");
  }
  z_aarch64_patch_branch26(&text, start_call_patch, function_offsets[main_index]);
  for (size_t i = 0; i < patch_ctx.user_call_patch_len; i++) {
    unsigned callee = patch_ctx.user_call_patches[i].callee_index;
    // Mirror the object path's guard: a user CALL placeholder must target a function we actually
    // laid out, else branch26 would patch a bogus 0 offset. Unreachable in practice (every
    // referenced callee is emitted above) but fail gracefully rather than emit a 0-offset jump.
    if (callee >= ir->function_len || !function_emitted[callee]) {
      a64_elf_patch_context_free(&patch_ctx);
      free(function_offsets);
      free(function_emitted);
      zbuf_free(&text);
      zbuf_free(&rodata);
      return a64_diag(diag, "direct AArch64 ELF executable user-function call targets a function that was not emitted", 1, 1, "missing callee");
    }
    z_aarch64_patch_branch26(&text, patch_ctx.user_call_patches[i].patch_offset, function_offsets[callee]);
  }

  const uint64_t base_addr = 0x400000;
  const size_t ehdr_size = 64;
  const size_t phdr_size = 56;
  const size_t text_offset = ehdr_size + phdr_size;
  const uint64_t entry_addr = base_addr + text_offset;
  size_t rodata_offset = has_rodata ? z_elf_align(text_offset + text.len, 8) : 0;
  uint64_t rodata_addr = has_rodata ? base_addr + rodata_offset : 0;
  a64_patch_data_patches(&text, &patch_ctx, rodata_addr, rodata_base_offset);
  ZElfExecutableImage image = {
    .machine = Z_ELF_MACHINE_AARCH64,
    .base_addr = base_addr,
    .entry_addr = entry_addr,
    .text_offset = text_offset,
    .text = &text,
    .rodata = has_rodata ? &rodata : NULL,
    .rodata_offset = rodata_offset,
    .segment_align = 0x1000
  };
  z_elf_write_executable64(out, &image);
  a64_elf_patch_context_free(&patch_ctx);
  free(function_offsets);
  free(function_emitted);
  zbuf_free(&text);
  zbuf_free(&rodata);
  return true;
}
