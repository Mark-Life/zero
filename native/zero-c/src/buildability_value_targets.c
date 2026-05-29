#include "buildability_internal.h"
#include <stdint.h>
#include <stdio.h>
enum { BUILD_AARCH64_IMM12_MAX = 4095u };

bool z_build_backend_is_aarch64_direct(ZDirectBackend backend) { return backend == Z_DIRECT_BACKEND_ELF_AARCH64 || backend == Z_DIRECT_BACKEND_COFF_AARCH64; }

static bool build_const_u32_value(const IrValue *value, unsigned *out) {
  if (!value || value->kind != IR_VALUE_INT || value->int_value > UINT32_MAX) return false;
  if (out) *out = (unsigned)value->int_value;
  return true;
}

static bool build_aarch64_index_load_uses_scratch(const IrFunction *fun, const IrValue *value) {
  if (!fun || value->kind != IR_VALUE_INDEX_LOAD || value->array_index >= fun->local_len) return true;
  const IrLocal *local = &fun->locals[value->array_index];
  unsigned const_index = 0;
  return !(local->is_array && (local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_USIZE) &&
           build_const_u32_value(value->index, &const_index) && const_index < local->array_len);
}

static bool build_check_aarch64_byte_view_len_spill(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, unsigned scratch_slot, unsigned slot_count, const char *message, ZDiag *diag) {
  if (!view || view->kind != IR_VALUE_BYTE_SLICE) return true;
  unsigned start = 0, end = 0;
  bool const_start = !view->index || build_const_u32_value(view->index, &start);
  bool const_end = build_const_u32_value(view->right, &end);
  if (const_start && const_end && end >= start && end - start <= 65535u) return true;
  if ((const_start && view->right) || (view->index && view->right)) {
    if (scratch_slot >= slot_count) return z_build_diag(ctx, diag, message, view->line, view->column, "expression too deep");
    if (!z_build_check_value(ctx, fun, view->right, false, scratch_slot, diag)) return false;
    return const_start || z_build_check_value(ctx, fun, view->index, false, scratch_slot + 1, diag);
  }
  return true;
}

static bool build_check_aarch64_byte_view_ptr_spill(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, unsigned scratch_slot, unsigned slot_count, const char *message, ZDiag *diag) {
  if (!view || view->kind != IR_VALUE_BYTE_SLICE) return true;
  if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, view->left, scratch_slot, slot_count, message, diag)) return false;
  unsigned start = 0;
  if (view->index && !build_const_u32_value(view->index, &start)) {
    if (scratch_slot >= slot_count) return z_build_diag(ctx, diag, message, view->line, view->column, "expression too deep");
    if (!z_build_check_value(ctx, fun, view->index, false, scratch_slot + 1, diag)) return false;
  }
  return true;
}

static size_t build_value_abi_slots(const IrValue *value) {
  size_t slots = 0;
  for (size_t i = 0; value && i < value->arg_len; i++) {
    slots += value->args[i] && value->args[i]->type == IR_TYPE_BYTE_VIEW ? 2u : 1u;
  }
  return slots;
}

// Integer-register ABI slot count for a call: spans take 2 ints, float args take 0 ints (they go in
// XMM regs on System V x64 / FP regs on AArch64).
static size_t build_value_int_abi_slots(ZDirectBackend backend, const IrValue *value) {
  size_t slots = 0;
  for (size_t i = 0; value && i < value->arg_len; i++) {
    IrTypeKind t = value->args[i] ? value->args[i]->type : IR_TYPE_UNSUPPORTED;
    if (t == IR_TYPE_BYTE_VIEW) slots += 2;
    else if ((t == IR_TYPE_F32 || t == IR_TYPE_F64) && (backend == Z_DIRECT_BACKEND_ELF64 || backend == Z_DIRECT_BACKEND_MACHO_X64 || backend == Z_DIRECT_BACKEND_MACHO64 || z_build_backend_is_aarch64_direct(backend))) {
      // Float args use a separate FP register file on these backends.
    } else slots += 1;
  }
  return slots;
}

static bool build_aarch64_byte_view_ptr(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  if (!view) return z_build_diag(ctx, diag, "direct AArch64 byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) return true;
  // `first.value` on a Maybe<MutSpan<u8>> local is a buildable span source (ptr@8).
  if (view->kind == IR_VALUE_MAYBE_VALUE && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW && ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64) return true;
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && fun && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    if (!local->is_array) {
      return z_build_diag(ctx, diag, "direct AArch64 byte-view array source must be a fixed array local", view->line, view->column, "unsupported array view");
    }
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) return true;
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) return build_aarch64_byte_view_ptr(ctx, fun, view->left, diag);
  // span-returning calls leave ptr in x0; field load from a record's span field reads the
  // ptr at field_offset. Both lower on the ELF AArch64 emitter.
  if ((view->kind == IR_VALUE_CALL || view->kind == IR_VALUE_FIELD_LOAD) && ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64) return true;
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    if (!build_aarch64_byte_view_ptr(ctx, fun, view->left, diag)) return false;
    if (build_const_u32_value(view->index, &start) && start > BUILD_AARCH64_IMM12_MAX) {
      return z_build_diag(ctx, diag, "direct AArch64 byte slice constant start is too large", view->line, view->column, "unsupported byte slice");
    }
    return true;
  }
  return z_build_diag(ctx, diag, "direct AArch64 value is not a supported byte view", view->line, view->column, "unsupported byte view");
}

bool z_build_check_aarch64_byte_view_len(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  if (!view) return z_build_diag(ctx, diag, "direct AArch64 byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (view->data_len > 65535u) {
      return z_build_diag(ctx, diag, "direct AArch64 byte-view length is too large for this backend", view->line, view->column, "large byte view");
    }
    return true;
  }
  if (view->kind == IR_VALUE_LOCAL && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) return true;
  // `first.value` on a Maybe<MutSpan<u8>> local — span length lives at slot +16.
  if (view->kind == IR_VALUE_MAYBE_VALUE && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW && ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64) return true;
  // span-returning calls leave len in x1; span field reads len at field_offset+8.
  if ((view->kind == IR_VALUE_CALL || view->kind == IR_VALUE_FIELD_LOAD) && ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64) return true;
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) return z_build_check_aarch64_byte_view_len(ctx, fun, view->left, diag);
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    unsigned end = 0;
    bool const_start = !view->index || build_const_u32_value(view->index, &start);
    bool const_end = build_const_u32_value(view->right, &end);
    if (const_start && const_end && end >= start && end - start <= 65535u) return true;
    if (const_start && view->right) {
      if (start > BUILD_AARCH64_IMM12_MAX) {
        return z_build_diag(ctx, diag, "direct AArch64 byte slice constant start is too large", view->line, view->column, "unsupported byte view length");
      }
      return true;
    }
    if (view->index && view->right) return true;
  }
  return z_build_diag(ctx, diag, "direct AArch64 byte-view length currently requires a literal, constant slice, or byte-view local", view->line, view->column, "unsupported byte view length");
}

bool z_build_check_aarch64_byte_view(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) { return build_aarch64_byte_view_ptr(ctx, fun, view, diag) && z_build_check_aarch64_byte_view_len(ctx, fun, view, diag); }

bool z_build_check_aarch64_world_write_byte_view(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  if (!z_build_check_aarch64_byte_view(ctx, fun, view, diag)) return false;
  if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, view, 0, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 World write exceeds scratch register spill capacity", diag)) return false;
  return build_check_aarch64_byte_view_len_spill(ctx, fun, view, 0, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 World write exceeds scratch register spill capacity", diag);
}

static bool build_aarch64_byte_operation(const ZBuildability *ctx, const IrFunction *fun, const IrValue *value, unsigned scratch_slot, bool *skip_left, ZDiag *diag) {
  if (value->kind == IR_VALUE_BYTE_COPY) {
    if (scratch_slot + 3 >= BUILD_AARCH64_SCRATCH_SLOT_COUNT) {
      return z_build_diag(ctx, diag, "direct AArch64 byte copy exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
    }
    if (!z_build_check_aarch64_byte_view(ctx, fun, value->left, diag)) return false;
    if (!z_build_check_aarch64_byte_view(ctx, fun, value->right, diag)) return false;
    if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->left, scratch_slot, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte copy exceeds scratch register spill capacity", diag)) return false;
    if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot + 1, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte copy exceeds scratch register spill capacity", diag)) return false;
    if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->right, scratch_slot + 2, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte copy exceeds scratch register spill capacity", diag)) return false;
    if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->right, scratch_slot + 3, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte copy exceeds scratch register spill capacity", diag)) return false;
  }
  if (value->kind == IR_VALUE_BYTE_FILL) {
    if (scratch_slot + 2 >= BUILD_AARCH64_SCRATCH_SLOT_COUNT) {
      return z_build_diag(ctx, diag, "direct AArch64 byte fill exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
    }
    if (!z_build_check_aarch64_byte_view(ctx, fun, value->right, diag)) return false;
    if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->right, scratch_slot + 1, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte fill exceeds scratch register spill capacity", diag)) return false;
    if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->right, scratch_slot + 2, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte fill exceeds scratch register spill capacity", diag)) return false;
  }
  if (value->kind == IR_VALUE_BYTE_VIEW_EQ) {
    if (scratch_slot + 3 >= BUILD_AARCH64_SCRATCH_SLOT_COUNT) {
      return z_build_diag(ctx, diag, "direct AArch64 byte-view equality exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
    }
    if (!z_build_check_aarch64_byte_view(ctx, fun, value->left, diag)) return false;
    if (!z_build_check_aarch64_byte_view(ctx, fun, value->right, diag)) return false;
    if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte-view equality exceeds scratch register spill capacity", diag)) return false;
    if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->right, scratch_slot + 1, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte-view equality exceeds scratch register spill capacity", diag)) return false;
    if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->left, scratch_slot + 1, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte-view equality exceeds scratch register spill capacity", diag)) return false;
    if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->right, scratch_slot + 2, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte-view equality exceeds scratch register spill capacity", diag)) return false;
  }
  if (value->kind == IR_VALUE_BYTE_VIEW_LEN && !build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte-view length exceeds scratch register spill capacity", diag)) return false;
  if (value->kind == IR_VALUE_BYTE_VIEW_LEN && !z_build_check_aarch64_byte_view_len(ctx, fun, value->left, diag)) return false;
  if (value->kind == IR_VALUE_INDEX_LOAD && build_aarch64_index_load_uses_scratch(fun, value) && scratch_slot >= BUILD_AARCH64_SCRATCH_SLOT_COUNT) return z_build_diag(ctx, diag, "direct AArch64 indexed load exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
  if (value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD && scratch_slot >= BUILD_AARCH64_SCRATCH_SLOT_COUNT) return z_build_diag(ctx, diag, "direct AArch64 byte-view indexed load exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
  if (value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD && !build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot + 1, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 byte-view indexed load exceeds scratch register spill capacity", diag)) return false;
  if (value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD && !z_build_check_aarch64_byte_view(ctx, fun, value->left, diag)) return false;
  if ((value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) && scratch_slot >= BUILD_AARCH64_SCRATCH_SLOT_COUNT) return z_build_diag(ctx, diag, "direct AArch64 little-endian read exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
  if ((value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) && !build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot + 1, BUILD_AARCH64_SCRATCH_SLOT_COUNT, "direct AArch64 little-endian read exceeds scratch register spill capacity", diag)) return false;
  if ((value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) && !z_build_check_aarch64_byte_view(ctx, fun, value->left, diag)) return false;
  if (value->kind == IR_VALUE_BYTE_VIEW_LEN || value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD ||
      value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) *skip_left = true;
  return true;
}

static bool build_check_binary_operator(const ZBuildability *ctx, const IrValue *value, unsigned scratch_slot, unsigned *right_slot, ZDiag *diag) {
  if (value->kind != IR_VALUE_BINARY) return true;
  bool supported = true;
  if (ctx->backend == Z_DIRECT_BACKEND_COFF_X64) {
    // float binary ops on COFF x64 add DIV (DIVSS/DIVSD); the int restriction stays
    // (no IDIV/DIV/IMUL high lowering on COFF x64 yet — none of the other COFF gates needed it).
    bool float_op = value->left && (value->left->type == IR_TYPE_F32 || value->left->type == IR_TYPE_F64);
    supported = value->binary_op == IR_BIN_ADD || value->binary_op == IR_BIN_SUB || value->binary_op == IR_BIN_MUL ||
                (float_op && value->binary_op == IR_BIN_DIV);
  }
  if (ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 || z_build_backend_is_aarch64_direct(ctx->backend)) {
    supported = value->binary_op == IR_BIN_ADD || value->binary_op == IR_BIN_SUB || value->binary_op == IR_BIN_MUL ||
                value->binary_op == IR_BIN_DIV || value->binary_op == IR_BIN_MOD ||
                value->binary_op == IR_BIN_AND || value->binary_op == IR_BIN_OR;
  }
  if (!supported) return z_build_diag(ctx, diag, "direct backend buildability does not support this binary operator", value->line, value->column, "unsupported operator");
  if (ctx->backend == Z_DIRECT_BACKEND_MACHO64 && value->binary_op != IR_BIN_AND && value->binary_op != IR_BIN_OR && scratch_slot >= BUILD_MACHO_SCRATCH_SLOT_COUNT) {
    return z_build_diag(ctx, diag, "direct AArch64 Mach-O expression nesting exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
  }
  if (z_build_backend_is_aarch64_direct(ctx->backend) && value->binary_op != IR_BIN_AND && value->binary_op != IR_BIN_OR && scratch_slot >= BUILD_AARCH64_SCRATCH_SLOT_COUNT) {
    return z_build_diag(ctx, diag, "direct AArch64 expression nesting exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
  }
  if ((ctx->backend == Z_DIRECT_BACKEND_MACHO64 || z_build_backend_is_aarch64_direct(ctx->backend)) &&
      value->binary_op != IR_BIN_AND && value->binary_op != IR_BIN_OR) {
    *right_slot = scratch_slot + 1;
  }
  return true;
}

static bool build_check_compare(const ZBuildability *ctx, const IrValue *value, unsigned scratch_slot, unsigned *right_slot, ZDiag *diag) {
  if (value->kind != IR_VALUE_COMPARE) return true;
  if (ctx->backend == Z_DIRECT_BACKEND_MACHO64 && scratch_slot >= BUILD_MACHO_SCRATCH_SLOT_COUNT) {
    return z_build_diag(ctx, diag, "direct AArch64 Mach-O expression nesting exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
  }
  if (z_build_backend_is_aarch64_direct(ctx->backend) && scratch_slot >= BUILD_AARCH64_SCRATCH_SLOT_COUNT) {
    return z_build_diag(ctx, diag, "direct AArch64 expression nesting exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
  }
  if (ctx->backend == Z_DIRECT_BACKEND_MACHO64 || z_build_backend_is_aarch64_direct(ctx->backend)) *right_slot = scratch_slot + 1;
  return true;
}

static bool build_check_call_shape(const ZBuildability *ctx, const IrValue *value, unsigned scratch_slot, ZDiag *diag) {
  if (value->kind != IR_VALUE_CALL) return true;
  size_t max_args = ctx->backend == Z_DIRECT_BACKEND_COFF_X64 ? 4 : (ctx->backend == Z_DIRECT_BACKEND_MACHO64 || z_build_backend_is_aarch64_direct(ctx->backend) ? 8 : 6);
  size_t int_slots = build_value_int_abi_slots(ctx->backend, value);
  if (int_slots > max_args) {
    char actual[80];
    snprintf(actual, sizeof(actual), "%zu ABI argument slot(s)", int_slots);
    return z_build_diag(ctx, diag, "direct backend buildability found a call with too many arguments", value->line, value->column, actual);
  }
  if (ctx->backend == Z_DIRECT_BACKEND_MACHO64 && scratch_slot + build_value_abi_slots(value) >= BUILD_MACHO_SCRATCH_SLOT_COUNT) {
    return z_build_diag(ctx, diag, "direct AArch64 Mach-O call argument nesting exceeds scratch spill capacity", value->line, value->column, "too many nested call arguments");
  }
  if (z_build_backend_is_aarch64_direct(ctx->backend) && scratch_slot + build_value_abi_slots(value) >= BUILD_AARCH64_SCRATCH_SLOT_COUNT) {
    return z_build_diag(ctx, diag, "direct AArch64 call argument nesting exceeds scratch spill capacity", value->line, value->column, "too many nested call arguments");
  }
  return true;
}

bool z_build_check_target_value(const ZBuildability *ctx, const IrFunction *fun, const IrValue *value, unsigned scratch_slot, bool *skip_left, unsigned *right_slot, ZDiag *diag) {
  if (skip_left) *skip_left = false;
  if (right_slot) *right_slot = scratch_slot;
  if (!ctx || !value) return true;
  if (ctx->backend == Z_DIRECT_BACKEND_COFF_X64) {
    if (value->kind == IR_VALUE_BYTE_COPY) {
      if (!z_build_check_coff_byte_view(ctx, fun, value->left, diag)) return false;
      if (!z_build_check_coff_byte_view(ctx, fun, value->right, diag)) return false;
    }
    if (value->kind == IR_VALUE_BYTE_FILL && !z_build_check_coff_byte_view(ctx, fun, value->right, diag)) return false;
    if (value->kind == IR_VALUE_BYTE_VIEW_EQ) {
      if (!z_build_check_coff_byte_view(ctx, fun, value->left, diag)) return false;
      if (!z_build_check_coff_byte_view(ctx, fun, value->right, diag)) return false;
    }
    if (value->kind == IR_VALUE_BYTE_VIEW_LEN && !z_build_check_coff_byte_view_len(ctx, fun, value->left, diag)) return false;
    if (value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD && !z_build_check_coff_byte_view(ctx, fun, value->left, diag)) return false;
    // codec LE reads recurse into the byte-view source via the same checker.
    if ((value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) && !z_build_check_coff_byte_view(ctx, fun, value->left, diag)) return false;
    if ((value->kind == IR_VALUE_FIXED_BUF_ALLOC || value->kind == IR_VALUE_VEC_INIT) && !z_build_check_coff_byte_view(ctx, fun, value->left, diag)) return false;
    if (value->kind == IR_VALUE_BYTE_VIEW_LEN || value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD ||
        value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE ||
        value->kind == IR_VALUE_FIXED_BUF_ALLOC || value->kind == IR_VALUE_VEC_INIT) *skip_left = true;
  }
  if (ctx->backend == Z_DIRECT_BACKEND_MACHO_X64) {
    if (value->kind == IR_VALUE_BYTE_COPY) {
      if (!z_build_check_macho_x64_byte_view(ctx, fun, value->left, diag)) return false;
      if (!z_build_check_macho_x64_byte_view(ctx, fun, value->right, diag)) return false;
    }
    if (value->kind == IR_VALUE_BYTE_FILL && !z_build_check_macho_x64_byte_view(ctx, fun, value->right, diag)) return false;
    if (value->kind == IR_VALUE_BYTE_VIEW_EQ) {
      if (!z_build_check_macho_x64_byte_view(ctx, fun, value->left, diag)) return false;
      if (!z_build_check_macho_x64_byte_view(ctx, fun, value->right, diag)) return false;
    }
    if (value->kind == IR_VALUE_BYTE_VIEW_LEN && !z_build_check_macho_x64_byte_view_len(ctx, fun, value->left, diag)) return false;
    if (value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD && !z_build_check_macho_x64_byte_view(ctx, fun, value->left, diag)) return false;
    if ((value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) && !z_build_check_macho_x64_byte_view(ctx, fun, value->left, diag)) return false;
    if (value->kind == IR_VALUE_BYTE_VIEW_LEN || value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD ||
        value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) *skip_left = true;
  }
  if (ctx->backend == Z_DIRECT_BACKEND_MACHO64) {
    if (value->kind == IR_VALUE_BYTE_COPY) {
      if (scratch_slot + 3 >= BUILD_MACHO_SCRATCH_SLOT_COUNT) return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte copy exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
      if (!z_build_check_macho_byte_view(ctx, fun, value->left, diag)) return false;
      if (!z_build_check_macho_byte_view(ctx, fun, value->right, diag)) return false;
      if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->left, scratch_slot, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte copy exceeds scratch register spill capacity", diag)) return false;
      if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot + 1, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte copy exceeds scratch register spill capacity", diag)) return false;
      if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->right, scratch_slot + 2, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte copy exceeds scratch register spill capacity", diag)) return false;
      if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->right, scratch_slot + 3, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte copy exceeds scratch register spill capacity", diag)) return false;
    }
    if (value->kind == IR_VALUE_BYTE_FILL) {
      if (scratch_slot + 2 >= BUILD_MACHO_SCRATCH_SLOT_COUNT) return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte fill exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
      if (!z_build_check_macho_byte_view(ctx, fun, value->right, diag)) return false;
      if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->right, scratch_slot + 1, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte fill exceeds scratch register spill capacity", diag)) return false;
      if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->right, scratch_slot + 2, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte fill exceeds scratch register spill capacity", diag)) return false;
    }
    if (value->kind == IR_VALUE_BYTE_VIEW_EQ) {
      if (scratch_slot + 3 >= BUILD_MACHO_SCRATCH_SLOT_COUNT) return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte-view equality exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
      if (!z_build_check_macho_byte_view(ctx, fun, value->left, diag)) return false;
      if (!z_build_check_macho_byte_view(ctx, fun, value->right, diag)) return false;
      if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte-view equality exceeds scratch register spill capacity", diag)) return false;
      if (!build_check_aarch64_byte_view_len_spill(ctx, fun, value->right, scratch_slot + 1, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte-view equality exceeds scratch register spill capacity", diag)) return false;
      if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->left, scratch_slot + 1, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte-view equality exceeds scratch register spill capacity", diag)) return false;
      if (!build_check_aarch64_byte_view_ptr_spill(ctx, fun, value->right, scratch_slot + 2, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte-view equality exceeds scratch register spill capacity", diag)) return false;
    }
    if (value->kind == IR_VALUE_BYTE_VIEW_LEN && !build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte-view length exceeds scratch register spill capacity", diag)) return false;
    if (value->kind == IR_VALUE_BYTE_VIEW_LEN && !z_build_check_macho_byte_view_len(ctx, fun, value->left, diag)) return false;
    if (value->kind == IR_VALUE_INDEX_LOAD && build_aarch64_index_load_uses_scratch(fun, value) && scratch_slot >= BUILD_MACHO_SCRATCH_SLOT_COUNT) return z_build_diag(ctx, diag, "direct AArch64 Mach-O indexed load exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
    if (value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD && scratch_slot >= BUILD_MACHO_SCRATCH_SLOT_COUNT) return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte-view indexed load exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
    if (value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD && !build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot + 1, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O byte-view indexed load exceeds scratch register spill capacity", diag)) return false;
    if (value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD && !z_build_check_macho_byte_view(ctx, fun, value->left, diag)) return false;
    if ((value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) && scratch_slot >= BUILD_MACHO_SCRATCH_SLOT_COUNT) return z_build_diag(ctx, diag, "direct AArch64 Mach-O little-endian read exceeds scratch register spill capacity", value->line, value->column, "expression too deep");
    if ((value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) && !build_check_aarch64_byte_view_len_spill(ctx, fun, value->left, scratch_slot + 1, BUILD_MACHO_SCRATCH_SLOT_COUNT, "direct AArch64 Mach-O little-endian read exceeds scratch register spill capacity", diag)) return false;
    if ((value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) && !z_build_check_macho_byte_view(ctx, fun, value->left, diag)) return false;
    if (value->kind == IR_VALUE_BYTE_VIEW_READ_INT_LE || value->kind == IR_VALUE_BYTE_VIEW_READ_FLOAT_LE) *skip_left = true;
    if ((value->kind == IR_VALUE_FIXED_BUF_ALLOC || value->kind == IR_VALUE_VEC_INIT) && !z_build_check_macho_byte_view(ctx, fun, value->left, diag)) return false;
    if ((value->kind == IR_VALUE_JSON_PARSE_BYTES || value->kind == IR_VALUE_JSON_VALIDATE_BYTES || value->kind == IR_VALUE_JSON_STREAM_TOKENS_BYTES) && !z_build_check_macho_byte_view(ctx, fun, value->left, diag)) return false;
    if (value->kind == IR_VALUE_HTTP_FETCH) {
      if (!z_build_check_macho_byte_view(ctx, fun, value->left, diag)) return false;
      if (!z_build_check_macho_byte_view(ctx, fun, value->right, diag)) return false;
    }
    if ((value->kind == IR_VALUE_HTTP_RESPONSE_LEN || value->kind == IR_VALUE_HTTP_RESPONSE_HEADERS_LEN || value->kind == IR_VALUE_HTTP_RESPONSE_BODY_OFFSET) && !z_build_check_macho_byte_view(ctx, fun, value->left, diag)) return false;
    if (value->kind == IR_VALUE_HTTP_HEADER_VALUE) {
      if (!z_build_check_macho_byte_view(ctx, fun, value->left, diag)) return false;
      if (!z_build_check_macho_byte_view(ctx, fun, value->right, diag)) return false;
    }
  }
  if (z_build_backend_is_aarch64_direct(ctx->backend) && !build_aarch64_byte_operation(ctx, fun, value, scratch_slot, skip_left, diag)) return false;
  return build_check_binary_operator(ctx, value, scratch_slot, right_slot, diag) &&
         build_check_compare(ctx, value, scratch_slot, right_slot, diag) &&
         build_check_call_shape(ctx, value, scratch_slot, diag);
}

unsigned z_build_target_call_arg_slot(const ZBuildability *ctx, const IrValue *value, unsigned scratch_slot) {
  if (ctx && value && ctx->backend == Z_DIRECT_BACKEND_MACHO64 && value->kind == IR_VALUE_CALL) {
    return scratch_slot + (unsigned)build_value_abi_slots(value);
  }
  return scratch_slot;
}

bool z_build_check_aarch64_function_shape(const ZBuildability *ctx, const IrFunction *fun, ZDiag *diag) {
  // float lowering on win-arm64 (COFF AArch64) — the shared aarch64_direct emitter
  // already implements float on the ELF target; COFF reuses the same emitter via
  // emit_coff_aarch64.c's ZAArch64DirectContext wrapper. Build-only verification.
  bool aarch64_float = z_build_backend_is_aarch64_direct(ctx->backend);
  // general user-fn CALL + fallible CHECK/RESCUE on COFF AArch64 once the
  // record_user_call_patch callback is wired in emit_coff_aarch64.c. Build-only.
  bool aarch64_user_call = z_build_backend_is_aarch64_direct(ctx->backend);
  // records (params, returns, locals) on COFF AArch64. The shared aarch64_direct emitter
  // implements record sret + field load/store + record-returning call; COFF inherits via
  // emit_coff_aarch64.c. Build-only.
  bool aarch64_record = z_build_backend_is_aarch64_direct(ctx->backend);
  // Maybe/allocator subsystem on COFF AArch64. The shared aarch64_direct emitter
  // implements FixedBufAlloc + Maybe<byte-view> + Maybe<scalar> + ALLOC_BYTES; COFF inherits via
  // emit_coff_aarch64.c's mmap-API callback.
  bool aarch64_maybe_alloc = z_build_backend_is_aarch64_direct(ctx->backend);
  for (size_t i = 0; i < fun->param_count; i++) {
    const IrLocal *param = &fun->locals[i];
    if (param->type == IR_TYPE_BYTE_VIEW) {
      if (!aarch64_user_call) return z_build_diag(ctx, diag, "direct AArch64 buildability does not support function parameters on this backend", param->line, param->column, param->name);
      continue;
    }
    // ref<Record> / mutref<Record> lowers on the AArch64 ELF emitter (linux-arm64 via
    // aarch64_direct, sharing the same AAPCS as the macho64 host). COFF AArch64 (win-arm64) stays
    // gated until Windows COFF parity is implemented.
    if (param->is_ref && ctx->backend != Z_DIRECT_BACKEND_ELF_AARCH64) {
      return z_build_diag(ctx, diag, "direct AArch64 buildability does not support ref<Record> parameters on this backend", param->line, param->column, param->name);
    }
    if (param->is_record || param->type == IR_TYPE_RECORD) {
      if (!aarch64_record) return z_build_diag(ctx, diag, "direct AArch64 record parameters are not yet supported on this backend", param->line, param->column, param->name);
      continue;
    }
    bool param_scalar_ok = z_build_is_elf_scalar(param->type);
    bool param_float_ok = aarch64_float && (param->type == IR_TYPE_F32 || param->type == IR_TYPE_F64);
    if (!aarch64_user_call || (!param_scalar_ok && !param_float_ok)) {
      return z_build_diag(ctx, diag, "direct AArch64 buildability does not support this parameter type", param->line, param->column, z_build_type_name(param->type));
    }
  }
  bool return_ok = fun->return_type == IR_TYPE_VOID || z_build_is_scalar32(fun->return_type) ||
                   (aarch64_float && (fun->return_type == IR_TYPE_F32 || fun->return_type == IR_TYPE_F64)) ||
                   (aarch64_user_call && (fun->return_type == IR_TYPE_I64 || fun->return_type == IR_TYPE_U64)) ||
                   (aarch64_record && (fun->return_type == IR_TYPE_RECORD || fun->return_type == IR_TYPE_BYTE_VIEW));
  if (!return_ok) {
    return z_build_diag(ctx, diag, "direct AArch64 buildability currently supports Void, scalar, float, record, or Span<u8> returns", fun->line, fun->column, z_build_type_name(fun->return_type));
  }
  // raising+record landed first on macho64 (darwin-arm64), then on aarch64_direct
  // (linux-arm64 ELF). The shared AArch64 backend uses x1 (the AAPCS second return register) as
  // the tag carrier — x0 stays the sret pointer per indirect-result convention. COFF AArch64
  // (win-arm64) stays gated until Windows COFF parity is implemented. Span+raises stays rejected (x1 IS the span len carrier).
  if (fun->raises && fun->return_type == IR_TYPE_RECORD && !aarch64_record) {
    return z_build_diag(ctx, diag, "direct AArch64 buildability does not support a record-returning raising function on this backend", fun->line, fun->column, fun->name);
  }
  if (fun->raises && fun->return_type == IR_TYPE_BYTE_VIEW) {
    return z_build_diag(ctx, diag, "direct AArch64 buildability does not support a span-returning raising function", fun->line, fun->column, fun->name);
  }
  size_t frame_bytes = (fun->frame_bytes ? fun->frame_bytes : fun->local_len * 8u) + BUILD_AARCH64_SCRATCH_SLOT_COUNT * 8u + (aarch64_record && fun->return_type == IR_TYPE_RECORD ? 16u : 0u);
  if (frame_bytes > 4095u) {
    return z_build_diag(ctx, diag, "direct AArch64 stack frame exceeds current immediate-offset support", fun->line, fun->column, fun->name);
  }
  for (size_t i = 0; i < fun->local_len; i++) {
    const IrLocal *local = &fun->locals[i];
    if (local->type == IR_TYPE_BYTE_VIEW) continue;
    if (local->is_record) {
      if (!aarch64_record) return z_build_diag(ctx, diag, "direct AArch64 buildability does not support record locals on this backend", local->line, local->column, local->name);
      continue;
    }
    // Maybe/allocator subsystem on linux-arm64 (ELF). PageAlloc rides on IR_TYPE_ALLOC and
    // the slot is reserved the same way; the emit-helper rejects PAGE_ALLOC init / allocBytes
    // gracefully (deferred).
    if (aarch64_maybe_alloc && (local->type == IR_TYPE_ALLOC || local->type == IR_TYPE_MAYBE_BYTE_VIEW || local->type == IR_TYPE_MAYBE_SCALAR)) continue;
    if (local->is_array) {
      bool array_ok = local->element_type == IR_TYPE_U8 || local->element_type == IR_TYPE_BOOL ||
                      local->element_type == IR_TYPE_U32 || local->element_type == IR_TYPE_I32 ||
                      local->element_type == IR_TYPE_USIZE;
      if (!array_ok) return z_build_diag(ctx, diag, "direct AArch64 object buildability does not support this fixed-array local", local->line, local->column, z_build_type_name(local->element_type));
      continue;
    }
    if (!z_build_is_elf_scalar(local->type) && !(aarch64_float && (local->type == IR_TYPE_F32 || local->type == IR_TYPE_F64))) {
      return z_build_diag(ctx, diag, "direct AArch64 object buildability does not support this local type", local->line, local->column, z_build_type_name(local->type));
    }
  }
  return true;
}
