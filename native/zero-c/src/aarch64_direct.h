#ifndef ZERO_C_AARCH64_DIRECT_H
#define ZERO_C_AARCH64_DIRECT_H

#include "zero.h"

typedef struct ZAArch64DirectContext ZAArch64DirectContext;

// libm symbols reached through a `bl` placeholder that the container (ELF) binds via a CALL26
// relocation against a bare external symbol. isNaNf is inline (no symbol) so it is absent here.
typedef enum {
  Z_AARCH64_MATH_SQRTF = 0,
  Z_AARCH64_MATH_EXPF,
  Z_AARCH64_MATH_COSF,
  Z_AARCH64_MATH_SINF,
  Z_AARCH64_MATH_POWF,
  Z_AARCH64_MATH_FABSF,
  Z_AARCH64_MATH_FLOORF,
  Z_AARCH64_MATH_SYMBOL_COUNT
} ZAArch64MathSymbol;

typedef bool (*ZAArch64DirectDataPatchFn)(void *user, size_t patch_offset, unsigned data_offset, const IrValue *value, ZDiag *diag);
typedef bool (*ZAArch64DirectWorldWriteFn)(ZBuf *text, const IrInstr *instr, ZAArch64DirectContext *ctx, ZDiag *diag);
// Records a `bl` site (at patch_offset, the start of the BL word) that calls the given libm symbol;
// the container assigns it a symbol index and emits the CALL26 relocation. May be NULL on backends
// that do not bind libm externals (the emitter then reports a graceful diagnostic).
typedef bool (*ZAArch64DirectCallPatchFn)(void *user, size_t patch_offset, ZAArch64MathSymbol symbol, const IrValue *value, ZDiag *diag);
// Records a `bl` site that calls another Zero function in the same program, by IR function index.
// The container resolves it to a final offset (exe path: inline branch26 patch) or to a symbol
// index (object path: R_AARCH64_CALL26 reloc against the callee symbol). May be NULL on backends
// that do not yet bind user functions (the emitter then reports a graceful diagnostic).
typedef bool (*ZAArch64DirectUserCallPatchFn)(void *user, size_t patch_offset, unsigned callee_index, const IrValue *value, ZDiag *diag);

struct ZAArch64DirectContext {
  const IrProgram *program;
  size_t *function_offsets;
  size_t function_count;
  unsigned rodata_base_offset;
  void *patch_user;
  ZAArch64DirectDataPatchFn record_data_patch;
  ZAArch64DirectWorldWriteFn emit_world_write;
  ZAArch64DirectCallPatchFn record_call_patch;
  ZAArch64DirectUserCallPatchFn record_user_call_patch;
  // enable the main-function argv/argc seed. When true and the emitter is lowering the
  // exported `main`, the prologue spills x20/x21 then captures argc/argv (AAPCS x0/x1) into
  // x20/x21 — std.args.{len,get} reads them from those callee-saved regs throughout the body.
  // The epilogue restores x20/x21 from the pre-frame slot. Left false on backends whose link
  // step lets crt0 seed the args natively (e.g. obj+link via musl crt0).
  bool seed_main_process_args;
};

bool z_aarch64_direct_emit_function_text(ZBuf *text, const IrFunction *fun, ZAArch64DirectContext *ctx, ZDiag *diag);
bool z_aarch64_direct_validate_function(const IrFunction *fun, ZDiag *diag);
const IrFunction *z_aarch64_direct_find_main(const IrProgram *program, unsigned *out_index, ZDiag *diag);
unsigned z_aarch64_direct_rodata_base_offset(const IrProgram *program);
void z_aarch64_direct_append_rodata(ZBuf *rodata, const IrProgram *program, unsigned base_offset);
size_t z_aarch64_direct_stack_bytes_from_ir(const IrProgram *program);
size_t z_aarch64_direct_max_frame_bytes_from_ir(const IrProgram *program);

#endif
