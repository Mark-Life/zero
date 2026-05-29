#include "buildability_internal.h"

#include <stdint.h>

enum {
  BUILD_AARCH64_IMM12_MAX = 4095u
};

static bool build_const_u32_value(const IrValue *value, unsigned *out) {
  if (!value || value->kind != IR_VALUE_INT || value->int_value > UINT32_MAX) return false;
  if (out) *out = (unsigned)value->int_value;
  return true;
}

static bool build_byte_view_const_len(const IrValue *view, unsigned *out) {
  if (!view) return false;
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (out) *out = view->data_len;
    return true;
  }
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned base_len = 0;
    if (!build_byte_view_const_len(view->left, &base_len)) return false;
    unsigned start = 0;
    unsigned end = base_len;
    if (view->index && !build_const_u32_value(view->index, &start)) return false;
    if (view->right && !build_const_u32_value(view->right, &end)) return false;
    if (start > end || end > base_len) return false;
    if (out) *out = end - start;
    return true;
  }
  return false;
}

static bool build_check_coff_byte_view_ptr(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  if (!view) return z_build_diag(ctx, diag, "direct COFF byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_MAYBE_VALUE && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && fun && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    if (!local->is_array) {
      return z_build_diag(ctx, diag, "direct COFF byte-view array source must be a fixed array local", view->line, view->column, "unsupported array view");
    }
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) return true;
  // bytesAs* typed-span reinterpret is a buildable byte-view source on COFF x64. The
  // underlying span ptr is unchanged; only the element count divides by sizeof(T).
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) return build_check_coff_byte_view_ptr(ctx, fun, view->left, diag);
  // a span-returning call lands its fat-pointer pair in (rax, rdx); a span field on a
  // record local lives at field_offset (ptr) + field_offset+8 (len) in the record's frame slot.
  if (view->kind == IR_VALUE_CALL && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    if (!build_const_u32_value(view->index, &start)) {
      return z_build_diag(ctx, diag, "direct COFF byte slice currently requires a constant start", view->line, view->column, "unsupported byte slice");
    }
    return build_check_coff_byte_view_ptr(ctx, fun, view->left, diag);
  }
  return z_build_diag(ctx, diag, "direct COFF value is not a supported byte view", view->line, view->column, "unsupported byte view");
}

bool z_build_check_coff_byte_view_len(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  unsigned len = 0;
  if (build_byte_view_const_len(view, &len)) return true;
  if (view && view->kind == IR_VALUE_LOCAL && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) return true;
  if (view && view->kind == IR_VALUE_MAYBE_VALUE && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) return true;
  // reinterpret length lowers via a runtime right-shift over the underlying byte length.
  if (view && view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) return z_build_check_coff_byte_view_len(ctx, fun, view->left, diag);
  // a span-returning call carries len in rdx; a span field's len lives 8 bytes past ptr.
  if (view && view->kind == IR_VALUE_CALL && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view && view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view && view->kind == IR_VALUE_BYTE_SLICE && view->index && view->right) {
    unsigned start = 0;
    unsigned end = 0;
    if (build_const_u32_value(view->index, &start) && build_const_u32_value(view->right, &end) && start <= end) return true;
  }
  return z_build_diag(ctx, diag, "direct COFF byte-view length currently requires a literal, constant slice, or byte-view local", view ? view->line : 1, view ? view->column : 1, "unsupported byte view length");
}

bool z_build_check_coff_byte_view(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  return build_check_coff_byte_view_ptr(ctx, fun, view, diag) && z_build_check_coff_byte_view_len(ctx, fun, view, diag);
}

static bool build_check_macho_x64_byte_view_ptr(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  if (!view) return z_build_diag(ctx, diag, "direct x86_64 Mach-O byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) return true;
  // `first.value` on a Maybe<MutSpan<u8>> local is a buildable span source (ptr@8).
  if (view->kind == IR_VALUE_MAYBE_VALUE && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && fun && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    if (!local->is_array) {
      return z_build_diag(ctx, diag, "direct x86_64 Mach-O byte-view array source must be a fixed array local", view->line, view->column, "unsupported array view");
    }
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) return true;
  // A call returning a byte view materializes a fat pointer in the destination local; accept it as a
  // buildable byte-view source. Same for a span field loaded from a record local.
  if (view->kind == IR_VALUE_CALL && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) return build_check_macho_x64_byte_view_ptr(ctx, fun, view->left, diag);
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    // Accept const start (small enough not to overflow `add rax, imm32`) OR a dynamic start expression;
    // dynamic-start is lowered as base+push, eval index, shl-scale, pop+add (mirrors elf64/macho64).
    if (!build_check_macho_x64_byte_view_ptr(ctx, fun, view->left, diag)) return false;
    return true;
  }
  return z_build_diag(ctx, diag, "direct x86_64 Mach-O value is not a supported byte view", view->line, view->column, "unsupported byte view");
}

bool z_build_check_macho_x64_byte_view_len(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  unsigned len = 0;
  if (build_byte_view_const_len(view, &len)) return true;
  if (view && view->kind == IR_VALUE_LOCAL && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) return true;
  // `first.value` on a Maybe<MutSpan<u8>> local — span length lives at slot +16.
  if (view && view->kind == IR_VALUE_MAYBE_VALUE && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) return true;
  if (view && view->kind == IR_VALUE_CALL && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view && view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view && view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) return z_build_check_macho_x64_byte_view_len(ctx, fun, view->left, diag);
  if (view && view->kind == IR_VALUE_BYTE_SLICE && view->right) {
    // Accept const+const (fast emit), const+dynamic, and dynamic+dynamic. Emitter mirrors macho64/elf64
    // (push end / pop start) for the dynamic forms.
    unsigned start = 0;
    unsigned end = 0;
    bool const_start = !view->index || build_const_u32_value(view->index, &start);
    bool const_end = build_const_u32_value(view->right, &end);
    if (const_start && const_end && end >= start) return true;
    if (const_start) return true;
    if (view->index && view->right) return true;
  }
  return z_build_diag(ctx, diag, "direct x86_64 Mach-O byte-view length currently requires a literal, constant slice, or byte-view local", view ? view->line : 1, view ? view->column : 1, "unsupported byte view length");
}

bool z_build_check_macho_x64_byte_view(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  return build_check_macho_x64_byte_view_ptr(ctx, fun, view, diag) && z_build_check_macho_x64_byte_view_len(ctx, fun, view, diag);
}

static bool build_check_macho_byte_view_ptr(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  if (!view) return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_LOCAL && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_MAYBE_VALUE && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_ARRAY_BYTE_VIEW && fun && view->array_index < fun->local_len) {
    const IrLocal *local = &fun->locals[view->array_index];
    if (!local->is_array) {
      return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte-view array source must be a fixed array local", view->line, view->column, "unsupported array view");
    }
    return true;
  }
  if (view->kind == IR_VALUE_STRING_LITERAL) return true;
  // A call returning a byte view materializes a fat pointer in the destination local;
  // accept it as a buildable byte-view source.
  if (view->kind == IR_VALUE_CALL && view->type == IR_TYPE_BYTE_VIEW) return true;
  // Field load of a byte-view field is also a buildable byte-view source.
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) return build_check_macho_byte_view_ptr(ctx, fun, view->left, diag);
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    if (!build_check_macho_byte_view_ptr(ctx, fun, view->left, diag)) return false;
    if (build_const_u32_value(view->index, &start) && start > BUILD_AARCH64_IMM12_MAX) {
      return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte slice constant start is too large", view->line, view->column, "unsupported byte slice");
    }
    return true;
  }
  return z_build_diag(ctx, diag, "direct AArch64 Mach-O value is not a supported byte view", view->line, view->column, "unsupported byte view");
}

bool z_build_check_macho_byte_view_len(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  if (!view) return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte view is missing", 1, 1, "missing byte view");
  if (view->kind == IR_VALUE_STRING_LITERAL || view->kind == IR_VALUE_ARRAY_BYTE_VIEW) {
    if (view->data_len > 65535u) {
      return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte-view length is too large for the this backend", view->line, view->column, "large byte view");
    }
    return true;
  }
  if (view->kind == IR_VALUE_LOCAL && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_MAYBE_VALUE && fun && view->local_index < fun->local_len && fun->locals[view->local_index].type == IR_TYPE_MAYBE_BYTE_VIEW) return true;
  // A call returning a byte view materializes a fat pointer; the length is the upper half of that pair.
  if (view->kind == IR_VALUE_CALL && view->type == IR_TYPE_BYTE_VIEW) return true;
  // A byte-view field load carries its length in the fat-pointer pair.
  if (view->kind == IR_VALUE_FIELD_LOAD && view->type == IR_TYPE_BYTE_VIEW) return true;
  if (view->kind == IR_VALUE_BYTE_VIEW_REINTERPRET) return z_build_check_macho_byte_view_len(ctx, fun, view->left, diag);
  if (view->kind == IR_VALUE_BYTE_SLICE) {
    unsigned start = 0;
    unsigned end = 0;
    bool const_start = !view->index || build_const_u32_value(view->index, &start);
    bool const_end = build_const_u32_value(view->right, &end);
    if (const_start && const_end && end >= start && end - start <= 65535u) return true;
    if (const_start && view->right) {
      if (start > BUILD_AARCH64_IMM12_MAX) {
        return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte slice constant start is too large", view->line, view->column, "unsupported byte view length");
      }
      return true;
    }
    if (view->index && view->right) return true;
  }
  return z_build_diag(ctx, diag, "direct AArch64 Mach-O byte-view length currently requires a literal, constant slice, or byte-view local", view->line, view->column, "unsupported byte view length");
}

bool z_build_check_macho_byte_view(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag) {
  return build_check_macho_byte_view_ptr(ctx, fun, view, diag) && z_build_check_macho_byte_view_len(ctx, fun, view, diag);
}
