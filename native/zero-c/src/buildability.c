#include "buildability_internal.h"

#include <string.h>

static bool build_value_supported(const ZBuildability *ctx, const IrValue *value, bool local_set_value) {
  if (!ctx || !value) return false;
  if (z_build_backend_is_aarch64_direct(ctx->backend)) {
    switch (value->kind) {
      case IR_VALUE_INT: case IR_VALUE_BOOL: case IR_VALUE_LOCAL: case IR_VALUE_CAST: case IR_VALUE_BINARY: case IR_VALUE_COMPARE:
      case IR_VALUE_STRING_LITERAL: case IR_VALUE_ARRAY_BYTE_VIEW: case IR_VALUE_BYTE_SLICE: case IR_VALUE_BYTE_VIEW_LEN:
      case IR_VALUE_BYTE_VIEW_INDEX_LOAD: case IR_VALUE_BYTE_COPY: case IR_VALUE_BYTE_FILL: case IR_VALUE_BYTE_VIEW_EQ:
      case IR_VALUE_INDEX_LOAD:
        (void)local_set_value;
        return true;
      // Float lowering on COFF AArch64 (win-arm64) — `aarch64_direct.c` already implements the
      // lowering on the AArch64 ELF emitter; COFF AArch64 reuses the same emitter via
      // emit_coff_aarch64.c's ZAArch64DirectContext wrapper. Both ELF and COFF now lower float.
      case IR_VALUE_FLOAT:
        (void)local_set_value;
        return true;
      // std.math on COFF AArch64. The shared aarch64_direct emitter already lowers math fns to BL
      // placeholders + record_call_patch; emit_coff_aarch64.c wires the patch to a msvcrt.dll IAT
      // thunk per used math symbol. isNaNf stays inline (no symbol).
      case IR_VALUE_MATH_SQRTF: case IR_VALUE_MATH_EXPF: case IR_VALUE_MATH_COSF: case IR_VALUE_MATH_SINF:
      case IR_VALUE_MATH_POWF: case IR_VALUE_MATH_FABSF: case IR_VALUE_MATH_FLOORF: case IR_VALUE_MATH_ISNANF:
        (void)local_set_value;
        return true;
      // bytesAs* typed-span reinterprets on COFF AArch64. The shared aarch64_direct emitter
      // already implements element-scaled load/store + ptr/len computation; COFF inherits it.
      // Verifiable on linux-arm64 (ELF) AND win-arm64 (COFF).
      case IR_VALUE_BYTE_VIEW_REINTERPRET:
        (void)local_set_value;
        return true;
      // codec read{I32,U32,U16,I64,U64,F32,F64}Le on COFF AArch64. The shared aarch64_direct
      // emitter already implements the unaligned LE reads with bounds check; COFF inherits via
      // emit_coff_aarch64.c. Verifiable on linux-arm64 (ELF) AND win-arm64 (COFF).
      case IR_VALUE_BYTE_VIEW_READ_INT_LE: case IR_VALUE_BYTE_VIEW_READ_FLOAT_LE:
        (void)local_set_value;
        return true;
      // General user-fn CALL + fallible CHECK/RESCUE on COFF AArch64. The shared aarch64_direct
      // emitter implements both; emit_coff_aarch64.c wires `record_user_call_patch` to redirect BL
      // placeholders through the import directory thunks OR (on the object path) symbol REL32
      // relocs. Build-only smoke.
      case IR_VALUE_CALL:
      case IR_VALUE_CHECK:
      case IR_VALUE_RESCUE:
      // Record field load on COFF AArch64. The shared aarch64_direct emitter implements it; both
      // ELF + COFF aarch64 now lower it.
      case IR_VALUE_FIELD_LOAD:
      // Maybe/allocator subsystem on COFF AArch64. The shared aarch64_direct emitter implements
      // FixedBufAlloc + Maybe<byte-view> + Maybe<scalar> + .has/.value; both ELF + COFF aarch64
      // lower these.
      case IR_VALUE_FIXED_BUF_ALLOC: case IR_VALUE_ALLOC_BYTES: case IR_VALUE_MAYBE_SCALAR_LITERAL:
      case IR_VALUE_MAYBE_HAS: case IR_VALUE_MAYBE_VALUE:
      // PageAlloc + FS_MMAP/MUNMAP/HOST on COFF AArch64. The shared aarch64_direct emitter inlines
      // raw Linux syscalls on ELF; on COFF, the new mmap-callback context routes them through
      // KERNEL32.dll APIs (VirtualAlloc, CreateFileA, CreateFileMappingA, MapViewOfFile,
      // UnmapViewOfFile, CloseHandle).
      case IR_VALUE_PAGE_ALLOC: case IR_VALUE_FS_MMAP: case IR_VALUE_FS_MUNMAP: case IR_VALUE_FS_HOST:
        (void)local_set_value;
        return true;
      // std.args.{len,get} on ELF AArch64. The shared aarch64_direct emitter spills x20/x21 in
      // the main prologue from x0/x1 (argc/argv seeded by the ELF `_start` stub) and lowers
      // ARGS_LEN to `mov w, w20` + ARGS_GET to bounds-check + strlen-loop. COFF AArch64 stays
      // gated — Windows main(argc, argv) is reachable, but there's no consumer today.
      case IR_VALUE_ARGS_LEN:
        return ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64;
      case IR_VALUE_ARGS_GET:
        return ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64 && local_set_value;
      default:
        (void)local_set_value;
        return false;
    }
  }
  if (ctx->backend == Z_DIRECT_BACKEND_MACHO_X64) {
    switch (value->kind) {
      case IR_VALUE_INT: case IR_VALUE_BOOL: case IR_VALUE_LOCAL: case IR_VALUE_CAST: case IR_VALUE_BINARY: case IR_VALUE_COMPARE: case IR_VALUE_CALL:
      case IR_VALUE_STRING_LITERAL: case IR_VALUE_ARRAY_BYTE_VIEW: case IR_VALUE_BYTE_SLICE: case IR_VALUE_BYTE_VIEW_LEN:
      case IR_VALUE_BYTE_VIEW_INDEX_LOAD: case IR_VALUE_BYTE_COPY: case IR_VALUE_BYTE_FILL: case IR_VALUE_BYTE_VIEW_EQ:
      case IR_VALUE_INDEX_LOAD: case IR_VALUE_FIELD_LOAD: case IR_VALUE_CHECK: case IR_VALUE_RESCUE: case IR_VALUE_FLOAT:
      // std.math lowers to libm calls (isNaNf inline) on the x86_64 Mach-O emitter.
      case IR_VALUE_MATH_SQRTF: case IR_VALUE_MATH_EXPF: case IR_VALUE_MATH_COSF: case IR_VALUE_MATH_SINF:
      case IR_VALUE_MATH_POWF: case IR_VALUE_MATH_FABSF: case IR_VALUE_MATH_FLOORF: case IR_VALUE_MATH_ISNANF:
      case IR_VALUE_BYTE_VIEW_REINTERPRET:
      // codec readI32Le/readU32Le/readF32Le/readF64Le: little-endian read at a byte offset.
      case IR_VALUE_BYTE_VIEW_READ_INT_LE: case IR_VALUE_BYTE_VIEW_READ_FLOAT_LE:
      // Maybe/allocator subsystem (FixedBufAlloc + Maybe<byte-view> + Maybe<scalar> + .has/.value
      // extraction); ALLOC_BYTES on a FixedBufAlloc is the supported path.
      // PageAlloc init + allocBytes-on-PageAlloc + std.fs.mmap/munmap/host all lower via libSystem
      // (_mmap/_munmap/_open/_lseek/_close) through the same obj+link routing the host macho64
      // backend uses.
      case IR_VALUE_FIXED_BUF_ALLOC: case IR_VALUE_ALLOC_BYTES: case IR_VALUE_MAYBE_SCALAR_LITERAL:
      case IR_VALUE_MAYBE_HAS: case IR_VALUE_MAYBE_VALUE:
      case IR_VALUE_PAGE_ALLOC: case IR_VALUE_FS_MMAP: case IR_VALUE_FS_MUNMAP: case IR_VALUE_FS_HOST:
        return true;
      // std.args.{len,get} on the x86_64 Mach-O backend. ARGS_LEN reads argc from r14 (seeded by
      // main's prologue from rdi under the LC_MAIN dyld ABI); ARGS_GET appears only as a
      // Maybe<MutSpan<u8>> local initializer — guarded by local_set_value to keep arbitrary
      // value-position uses out (mirrors the ELF64 / Mach-O AArch64 gate).
      case IR_VALUE_ARGS_LEN:
        (void)local_set_value;
        return true;
      case IR_VALUE_ARGS_GET:
        return local_set_value;
      default:
        (void)local_set_value;
        return false;
    }
  }
  switch (value->kind) {
    case IR_VALUE_INT: case IR_VALUE_BOOL: case IR_VALUE_LOCAL: case IR_VALUE_CAST: case IR_VALUE_BINARY: case IR_VALUE_COMPARE: case IR_VALUE_CALL:
    case IR_VALUE_STRING_LITERAL: case IR_VALUE_ARRAY_BYTE_VIEW: case IR_VALUE_BYTE_SLICE: case IR_VALUE_BYTE_VIEW_LEN:
    case IR_VALUE_BYTE_VIEW_INDEX_LOAD: case IR_VALUE_INDEX_LOAD: case IR_VALUE_FIELD_LOAD:
      return true;
    case IR_VALUE_FLOAT:
      // Float literals on COFF x64 (win-x64). Build-only verification; Windows is not a CLI exec
      // target. Lowering in emit_coff.c mirrors the SysV x64 SSE pattern with Win64 ABI shifts
      // (xmm0-3 param bank, sret in rcx, 32-byte shadow space).
      return ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
    case IR_VALUE_BYTE_VIEW_REINTERPRET:
      // bytesAs* typed-span reinterprets: verifiable on the AArch64 Mach-O host, System V ELF64,
      // and the Windows COFF x64 emitter (build-only).
      return ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
    case IR_VALUE_BYTE_VIEW_READ_INT_LE:
    case IR_VALUE_BYTE_VIEW_READ_FLOAT_LE:
      // codec readI32Le/readU32Le/readF32Le/readF64Le: little-endian read at a byte offset.
      // Verifiable on the AArch64 Mach-O host, System V ELF64, AND the Windows COFF x64 emitter
      // (build-only).
      return ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
    case IR_VALUE_FIXED_BUF_ALLOC: case IR_VALUE_VEC_INIT: case IR_VALUE_ALLOC_BYTES: case IR_VALUE_MAYBE_SCALAR_LITERAL:
      return local_set_value;
    case IR_VALUE_VEC_PUSH: case IR_VALUE_VEC_LEN: case IR_VALUE_VEC_CAPACITY: case IR_VALUE_MAYBE_HAS:
      return true;
    case IR_VALUE_ARGS_GET:
      // ARGS_GET lowers on all four floated backends — host macho64 + the three secondary
      // backends. ELF64/MACHO_X64 share the SysV r14/r15 spill mechanism; MACHO64/ELF_AARCH64
      // share the AAPCS x20/x21 spill. COFF (Windows) stays gated — not a CLI exec target.
      return (ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64) ? local_set_value : false;
    case IR_VALUE_ARGS_LEN:
      return ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64;
    case IR_VALUE_ENV_GET:
      return ctx->backend == Z_DIRECT_BACKEND_ELF64 && local_set_value;
    case IR_VALUE_TIME_WALL_SECONDS: case IR_VALUE_TIME_MONOTONIC: case IR_VALUE_TIME_AS_MS:
    case IR_VALUE_RAND_NEXT_U32: case IR_VALUE_RAND_ENTROPY_U32:
      return ctx->backend == Z_DIRECT_BACKEND_ELF64;
    case IR_VALUE_FS_HOST:
      // std.fs.host() is a stateless capability token (lowers to 0). All floated backends plus
      // both COFF backends accept it.
      return ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_AARCH64;
    case IR_VALUE_FS_OPEN: case IR_VALUE_FS_CREATE: case IR_VALUE_FS_READ_PATH:
    case IR_VALUE_FS_WRITE_PATH: case IR_VALUE_FS_READ_BYTES_PATH: case IR_VALUE_FS_WRITE_BYTES_PATH:
    case IR_VALUE_FS_READ_ALL: case IR_VALUE_FS_READ_FILE: case IR_VALUE_FS_WRITE_ALL_FILE:
    case IR_VALUE_FS_CLOSE_FILE: case IR_VALUE_FS_EXISTS: case IR_VALUE_FS_REMOVE: case IR_VALUE_FS_RENAME:
    case IR_VALUE_FS_FILE_LEN: case IR_VALUE_FS_MAKE_DIR: case IR_VALUE_FS_REMOVE_DIR: case IR_VALUE_FS_IS_DIR:
    case IR_VALUE_FS_DIR_ENTRY_COUNT: case IR_VALUE_FS_TEMP_NAME: case IR_VALUE_FS_ATOMIC_WRITE:
    case IR_VALUE_CRC32_BYTES:
      return ctx->backend == Z_DIRECT_BACKEND_ELF64;
    case IR_VALUE_BYTE_COPY: case IR_VALUE_BYTE_FILL:
      return ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 ||
             ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
    case IR_VALUE_BYTE_VIEW_EQ:
      return ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 ||
             ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
    case IR_VALUE_CHECK: return ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
    case IR_VALUE_RESCUE: return ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
    case IR_VALUE_MAYBE_VALUE:
      // ELF64 supports both byte-view and scalar payloads. macho64 (host arm64) supports the
      // byte-view form (Maybe<MutSpan<u8>>.value as a span). The x86_64 Mach-O backend (macho_x64)
      // is handled by the dedicated MACHO_X64 switch above. COFF x64 (build-only) supports the
      // same byte-view + scalar surface.
      return ctx->backend == Z_DIRECT_BACKEND_ELF64 || (ctx->backend == Z_DIRECT_BACKEND_MACHO64 && value->type == IR_TYPE_BYTE_VIEW) || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
    case IR_VALUE_JSON_PARSE_BYTES: case IR_VALUE_JSON_VALIDATE_BYTES: case IR_VALUE_JSON_STREAM_TOKENS_BYTES:
    case IR_VALUE_HTTP_FETCH: case IR_VALUE_HTTP_RESULT_OK: case IR_VALUE_HTTP_RESULT_STATUS: case IR_VALUE_HTTP_RESULT_BODY_LEN:
    case IR_VALUE_HTTP_RESULT_ERROR: case IR_VALUE_HTTP_RESPONSE_LEN: case IR_VALUE_HTTP_RESPONSE_HEADERS_LEN:
    case IR_VALUE_HTTP_RESPONSE_BODY_OFFSET: case IR_VALUE_HTTP_HEADER_VALUE: case IR_VALUE_HTTP_HEADER_FOUND:
    case IR_VALUE_HTTP_HEADER_OFFSET: case IR_VALUE_HTTP_HEADER_LEN:
      return ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64;
    case IR_VALUE_MATH_SQRTF: case IR_VALUE_MATH_EXPF: case IR_VALUE_MATH_COSF: case IR_VALUE_MATH_SINF:
    case IR_VALUE_MATH_POWF: case IR_VALUE_MATH_FABSF: case IR_VALUE_MATH_FLOORF: case IR_VALUE_MATH_ISNANF:
      // std.math lowers to libm calls (isNaNf inline) on the System V x64 emitters (ELF64, x86_64
      // Mach-O), the AArch64 Mach-O emitter, AND the Windows COFF x64 emitter (build-only, libm
      // via msvcrt.dll .idata import directory).
      return ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
    case IR_VALUE_PAGE_ALLOC:
      // std.mem.pageAlloc handle init. Mach-O uses libSystem _mmap (MAP_ANON); ELF uses raw mmap
      // syscalls; COFF uses kernel32!VirtualAlloc(NULL, n, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)
      // via the .idata import directory.
      return local_set_value && (ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_AARCH64);
    case IR_VALUE_FS_MMAP:
      // std.fs.mmap file mapping -> Maybe<owned<Mapping>>. Mach-O via libSystem; ELF via raw
      // syscalls; COFF via kernel32!CreateFileA + GetFileSizeEx + CreateFileMappingA +
      // MapViewOfFile + CloseHandle chain.
      return ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_AARCH64;
    case IR_VALUE_FS_MUNMAP:
      // std.fs.munmap release. Mach-O via libSystem _munmap; ELF via raw munmap syscall; COFF via
      // kernel32!UnmapViewOfFile.
      return ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_AARCH64;
  }
  return false;
}

bool z_build_check_value(const ZBuildability *ctx, const IrFunction *fun, const IrValue *value, bool local_set_value, unsigned scratch_slot, ZDiag *diag) {
  if (!value) return z_build_diag(ctx, diag, "direct backend buildability found a missing expression", 1, 1, "missing expression");
  if (!build_value_supported(ctx, value, local_set_value)) {
    return z_build_diag(ctx, diag, "direct backend buildability does not support this MIR value", value->line, value->column, z_build_value_kind_name(value->kind));
  }
  bool skip_left = false;
  unsigned right_slot = scratch_slot;
  if (!z_build_check_target_value(ctx, fun, value, scratch_slot, &skip_left, &right_slot, diag)) return false;
  if (value->kind == IR_VALUE_LOCAL && fun && value->local_index < fun->local_len && fun->locals[value->local_index].is_array) {
    return z_build_diag(ctx, diag, "direct backend buildability cannot use fixed array locals as scalar values", value->line, value->column, "array local");
  }
  if (value->index && !z_build_check_value(ctx, fun, value->index, false, scratch_slot, diag)) return false;
  if (value->left && !skip_left && !z_build_check_value(ctx, fun, value->left, false, scratch_slot, diag)) return false;
  if (value->right && !z_build_check_value(ctx, fun, value->right, false, right_slot, diag)) return false;
  unsigned arg_slot = z_build_target_call_arg_slot(ctx, value, scratch_slot);
  for (size_t i = 0; i < value->arg_len; i++) {
    if (!z_build_check_value(ctx, fun, value->args[i], false, arg_slot, diag)) return false;
  }
  return true;
}

static bool build_check_instrs(const ZBuildability *ctx, const IrFunction *fun, const IrInstr *instrs, size_t len, ZDiag *diag);

static bool build_check_instr(const ZBuildability *ctx, const IrFunction *fun, const IrInstr *instr, ZDiag *diag) {
  if (!ctx || !instr) return z_build_diag(ctx, diag, "direct backend buildability found a missing instruction", 1, 1, "missing instruction");
  if (z_build_backend_is_aarch64_direct(ctx->backend) && instr->kind == IR_INSTR_WORLD_WRITE) {
    // World write lowers to an inline svc on the AArch64 ELF backend, valid in both the executable
    // and the object (obj+link) paths — the latter is how std.math programs reach a linked binary.
    // The COFF AArch64 object path has no inline writer yet, so it stays gated to executables.
    if (!ctx->executable && ctx->backend != Z_DIRECT_BACKEND_ELF_AARCH64) {
      return z_build_diag(ctx, diag, "direct AArch64 object buildability does not support World write instructions", instr->line, instr->column, "IR_INSTR_WORLD_WRITE");
    }
    if (instr->value && !z_build_check_aarch64_world_write_byte_view(ctx, fun, instr->value, diag)) return false;
    if (instr->index && !z_build_check_value(ctx, fun, instr->index, false, 0, diag)) return false;
    return true;
  }
  if (z_build_backend_is_aarch64_direct(ctx->backend) && instr->kind == IR_INSTR_WHILE) {
    // WHILE lowers on the ELF aarch64 emitter (mirrors macho64's trivial loop_start + condition +
    // body + back-branch pattern). COFF AArch64 stays gated — no consumer today.
    if (ctx->backend != Z_DIRECT_BACKEND_ELF_AARCH64) {
      return z_build_diag(ctx, diag, "direct AArch64 buildability does not support this instruction yet", instr->line, instr->column, "unsupported instruction");
    }
    if (instr->value && !z_build_check_value(ctx, fun, instr->value, false, 0, diag)) return false;
    if (instr->then_instrs && !build_check_instrs(ctx, fun, instr->then_instrs, instr->then_len, diag)) return false;
    return true;
  }
  // IR_INSTR_RAISE on aarch64-direct lowers to a packed-tag-in-high-32 epilogue on the shared
  // aarch64_direct emitter. Both ELF + COFF AArch64 now lower RAISE.
  // IR_INSTR_FIELD_STORE lowers on the AArch64 ELF emitter (records on linux-arm64); COFF stays gated.
  if (z_build_backend_is_aarch64_direct(ctx->backend) && instr->kind == IR_INSTR_FIELD_STORE && ctx->backend != Z_DIRECT_BACKEND_ELF_AARCH64) {
    return z_build_diag(ctx, diag, "direct AArch64 buildability does not support this instruction yet", instr->line, instr->column, "unsupported instruction");
  }
  switch (instr->kind) {
    case IR_INSTR_LOCAL_SET:
      if (ctx->backend == Z_DIRECT_BACKEND_COFF_X64 && fun && instr->local_index < fun->local_len && fun->locals[instr->local_index].type == IR_TYPE_BYTE_VIEW) {
        return !instr->value || z_build_check_coff_byte_view(ctx, fun, instr->value, diag);
      }
      if (ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 && fun && instr->local_index < fun->local_len && fun->locals[instr->local_index].type == IR_TYPE_BYTE_VIEW) {
        return !instr->value || z_build_check_macho_x64_byte_view(ctx, fun, instr->value, diag);
      }
      if (ctx->backend == Z_DIRECT_BACKEND_MACHO64 && fun && instr->local_index < fun->local_len && fun->locals[instr->local_index].type == IR_TYPE_BYTE_VIEW) {
        if (instr->value && !z_build_check_macho_byte_view(ctx, fun, instr->value, diag)) return false;
      }
      if (z_build_backend_is_aarch64_direct(ctx->backend) && fun && instr->local_index < fun->local_len && fun->locals[instr->local_index].type == IR_TYPE_BYTE_VIEW) {
        if (instr->value && !z_build_check_aarch64_byte_view(ctx, fun, instr->value, diag)) return false;
      }
      if (instr->value && !z_build_check_value(ctx, fun, instr->value, true, 0, diag)) return false;
      return true;
    case IR_INSTR_INDEX_STORE:
    case IR_INSTR_FIELD_STORE: {
      if (instr->value && !z_build_check_value(ctx, fun, instr->value, false, 0, diag)) return false;
      unsigned index_scratch_slot = instr->kind == IR_INSTR_INDEX_STORE && z_build_backend_is_aarch64_direct(ctx->backend) ? 1 : 0;
      if (instr->index && !z_build_check_value(ctx, fun, instr->index, false, index_scratch_slot, diag)) return false;
      return true;
    }
    case IR_INSTR_WORLD_WRITE:
      if (ctx->backend == Z_DIRECT_BACKEND_COFF_X64 && instr->value && !z_build_check_coff_byte_view(ctx, fun, instr->value, diag)) return false;
      if (ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 && instr->value && !z_build_check_macho_x64_byte_view(ctx, fun, instr->value, diag)) return false;
      if (ctx->backend == Z_DIRECT_BACKEND_MACHO64 && instr->value && !z_build_check_macho_byte_view(ctx, fun, instr->value, diag)) return false;
      if (ctx->backend != Z_DIRECT_BACKEND_COFF_X64 && instr->value && !z_build_check_value(ctx, fun, instr->value, false, 0, diag)) return false;
      if (instr->index && !z_build_check_value(ctx, fun, instr->index, false, 0, diag)) return false;
      return true;
    case IR_INSTR_EXPR:
    case IR_INSTR_RETURN:
      if (instr->value && !z_build_check_value(ctx, fun, instr->value, false, 0, diag)) return false;
      if (instr->index && !z_build_check_value(ctx, fun, instr->index, false, 0, diag)) return false;
      return true;
    case IR_INSTR_IF:
    case IR_INSTR_WHILE:
      if (instr->value && !z_build_check_value(ctx, fun, instr->value, false, 0, diag)) return false;
      if (!build_check_instrs(ctx, fun, instr->then_instrs, instr->then_len, diag)) return false;
      return build_check_instrs(ctx, fun, instr->else_instrs, instr->else_len, diag);
    case IR_INSTR_RAISE:
      if (ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 ||
          ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64 ||
          ctx->backend == Z_DIRECT_BACKEND_COFF_AARCH64) return true;
      return z_build_diag(ctx, diag, "direct backend buildability does not support raise instructions for this emitter", instr->line, instr->column, "IR_INSTR_RAISE");
  }
  return z_build_diag(ctx, diag, "direct backend buildability does not support this instruction", instr->line, instr->column, "unsupported instruction");
}

static bool build_check_instrs(const ZBuildability *ctx, const IrFunction *fun, const IrInstr *instrs, size_t len, ZDiag *diag) {
  for (size_t i = 0; i < len; i++) {
    if (!build_check_instr(ctx, fun, &instrs[i], diag)) return false;
  }
  return true;
}

static bool build_check_function_shape(const ZBuildability *ctx, const IrFunction *fun, ZDiag *diag) {
  if (!fun) return z_build_diag(ctx, diag, "direct backend buildability found a missing function", 1, 1, "missing function");
  // On System V x64 (ELF64, x86_64 Mach-O) and AArch64 Mach-O (MACHO64) floats consume a separate
  // FP register file, not integer GPR slots; count them separately so the int-slot cap is not
  // over-charged. COFF unified arg-register pool stays under the existing 4-slot cap. Kept in sync
  // with build_value_int_abi_slots() (the call-site counterpart).
  size_t int_slots = 0;
  size_t fp_slots = 0;
  for (size_t i = 0; i < fun->param_count; i++) {
    IrTypeKind t = fun->locals[i].type;
    if (t == IR_TYPE_BYTE_VIEW) int_slots += 2;
    else if ((t == IR_TYPE_F32 || t == IR_TYPE_F64) && (ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64)) fp_slots += 1;
    else int_slots += 1;
  }
  size_t max_int_slots = ctx->backend == Z_DIRECT_BACKEND_COFF_X64 ? 4 : (ctx->backend == Z_DIRECT_BACKEND_MACHO64 ? 8 : 6);
  if (!z_build_backend_is_aarch64_direct(ctx->backend) && int_slots > max_int_slots) {
    return z_build_diag(ctx, diag, "direct backend object buildability has too many ABI argument slots", fun->line, fun->column, fun->name);
  }
  if (fp_slots > 8) {
    return z_build_diag(ctx, diag, "direct backend object buildability has too many floating-point ABI argument slots", fun->line, fun->column, fun->name);
  }
  if (z_build_backend_is_aarch64_direct(ctx->backend)) return z_build_check_aarch64_function_shape(ctx, fun, diag);
  // COFF x64 admits i64/u64 locals + non-main returns to support codec readI64Le/readU64Le result
  // types. Build-only — the main fn signature gate at the emit layer keeps main scalar32.
  bool wide_scalars = ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
  // f32/f64 lowering is verifiable on the System V x64 emitters (ELF64, x86_64 Mach-O), the
  // AArch64 Mach-O emitter, AND the Windows x64 COFF emitter (build-only smoke since Windows is
  // not a CLI exec target).
  bool float_backend = ctx->backend == Z_DIRECT_BACKEND_MACHO64 || ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 || ctx->backend == Z_DIRECT_BACKEND_COFF_X64;
  bool float_ret = float_backend && (fun->return_type == IR_TYPE_F32 || fun->return_type == IR_TYPE_F64);
  // Record + byte-view returns are buildable on the floated backends (macho64, elf64, macho-x64)
  // and on COFF x64 (build-only Win64 sret). float_backend already includes COFF_X64.
  bool aggregate_ret = float_backend && (fun->return_type == IR_TYPE_RECORD || fun->return_type == IR_TYPE_BYTE_VIEW);
  bool return_ok = (wide_scalars ? (fun->return_type == IR_TYPE_VOID || z_build_is_elf_scalar(fun->return_type))
                                 : (fun->return_type == IR_TYPE_VOID || z_build_is_scalar32(fun->return_type))) || float_ret || aggregate_ret;
  if (!return_ok) return z_build_diag(ctx, diag, "direct backend object buildability does not support this return type", fun->line, fun->column, z_build_type_name(fun->return_type));
  // Raising+record now lands on all four floated backends — macho64 (darwin-arm64), macho_x64
  // (darwin-x64), elf64 (linux-musl-x64), aarch64_direct (linux-musl-arm64). COFF (Windows) stays
  // gated. Span+raises stays rejected everywhere (the second-return register IS the span len
  // carrier). NB: aarch64_direct dispatches through z_build_check_aarch64_function_shape; this
  // general gate is defensive for non-aarch64 paths.
  if (fun->raises && fun->return_type == IR_TYPE_RECORD && ctx->backend != Z_DIRECT_BACKEND_MACHO64 && ctx->backend != Z_DIRECT_BACKEND_MACHO_X64 && ctx->backend != Z_DIRECT_BACKEND_ELF64 && ctx->backend != Z_DIRECT_BACKEND_ELF_AARCH64) {
    return z_build_diag(ctx, diag, "direct backend object buildability does not support a record-returning raising function on this backend", fun->line, fun->column, fun->name);
  }
  if (fun->raises && fun->return_type == IR_TYPE_BYTE_VIEW) {
    return z_build_diag(ctx, diag, "direct backend object buildability does not support a span-returning raising function", fun->line, fun->column, fun->name);
  }
  for (size_t i = 0; i < fun->local_len; i++) {
    const IrLocal *local = &fun->locals[i];
    if (local->type == IR_TYPE_BYTE_VIEW && local->is_param && ctx->backend != Z_DIRECT_BACKEND_ELF64 && ctx->backend != Z_DIRECT_BACKEND_MACHO64 && ctx->backend != Z_DIRECT_BACKEND_MACHO_X64) return z_build_diag(ctx, diag, "direct backend object buildability does not support byte-view parameters", local->line, local->column, local->name);
    // ref<Record> / mutref<Record> lowers on all four floated backends — macho64 (darwin-arm64),
    // macho_x64 (darwin-x64), elf64 (linux-musl-x64), elf_aarch64 (linux-arm64 via
    // aarch64_direct). COFF (Windows) stays gated.
    if (local->is_ref && ctx->backend != Z_DIRECT_BACKEND_MACHO64 && ctx->backend != Z_DIRECT_BACKEND_MACHO_X64 && ctx->backend != Z_DIRECT_BACKEND_ELF64 && ctx->backend != Z_DIRECT_BACKEND_ELF_AARCH64) {
      return z_build_diag(ctx, diag, "direct backend object buildability does not support ref<Record> parameters on this backend", local->line, local->column, local->name);
    }
    // MACHO_X64 now supports ALLOC + MAYBE_BYTE_VIEW + MAYBE_SCALAR locals (the Maybe/allocator
    // subsystem). VEC is still rejected on macho_x64 — its push/grow ABI needs more emitter work
    // and isn't required for llama2's f32 path.
    if (ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 && local->type == IR_TYPE_VEC) {
      return z_build_diag(ctx, diag, "direct x86_64 Mach-O object buildability does not support this local type", local->line, local->column, z_build_type_name(local->type));
    }
    if (local->is_record || local->type == IR_TYPE_BYTE_VIEW || local->type == IR_TYPE_ALLOC || local->type == IR_TYPE_VEC || local->type == IR_TYPE_MAYBE_BYTE_VIEW) continue;
    if (local->type == IR_TYPE_MAYBE_SCALAR) continue;
    if (local->is_array) {
      bool array_ok = local->element_type == IR_TYPE_U8 || local->element_type == IR_TYPE_I32 || local->element_type == IR_TYPE_U32 ||
                      local->element_type == IR_TYPE_USIZE || ((ctx->backend == Z_DIRECT_BACKEND_ELF64 || ctx->backend == Z_DIRECT_BACKEND_MACHO64) && local->element_type == IR_TYPE_BOOL) ||
                      (ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 && local->element_type == IR_TYPE_BOOL) ||
                      (ctx->backend == Z_DIRECT_BACKEND_ELF64 && (local->element_type == IR_TYPE_I64 || local->element_type == IR_TYPE_U64));
      if (!array_ok) return z_build_diag(ctx, diag, "direct backend object buildability does not support this fixed-array local", local->line, local->column, z_build_type_name(local->element_type));
      continue;
    }
    bool local_ok = (wide_scalars ? z_build_is_elf_scalar(local->type) : z_build_is_scalar32(local->type)) ||
                    (float_backend && (local->type == IR_TYPE_F32 || local->type == IR_TYPE_F64));
    if (!local_ok) return z_build_diag(ctx, diag, "direct backend object buildability does not support this local type", local->line, local->column, z_build_type_name(local->type));
  }
  return true;
}

static const IrFunction *build_find_main(const ZBuildability *ctx, const IrProgram *ir, ZDiag *diag) {
  const IrFunction *main_fun = NULL;
  for (size_t i = 0; ir && i < ir->function_len; i++) {
    if (!ir->functions[i].is_exported || !ir->functions[i].name || strcmp(ir->functions[i].name, "main") != 0) continue;
    if (main_fun) {
      z_build_diag(ctx, diag, "direct executable buildability requires exactly one exported main function", ir->functions[i].line, ir->functions[i].column, ir->functions[i].name);
      return NULL;
    }
    main_fun = &ir->functions[i];
  }
  if (!main_fun) z_build_diag(ctx, diag, "direct executable buildability requires an exported main function", 1, 1, "missing main");
  return main_fun;
}

static bool build_check_executable_shape(const ZBuildability *ctx, const IrProgram *ir, ZDiag *diag) {
  const IrFunction *main_fun = build_find_main(ctx, ir, diag);
  if (!main_fun) return false;
  if (main_fun->param_count != 0) {
    const char *message = ctx->backend == Z_DIRECT_BACKEND_ELF64 ? "direct ELF64 executable main must not take parameters" :
                          (ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64 ? "direct AArch64 ELF executable main must not take parameters" :
                          (ctx->backend == Z_DIRECT_BACKEND_COFF_AARCH64 ? "direct COFF AArch64 executable main must not take parameters" :
                          (ctx->backend == Z_DIRECT_BACKEND_COFF_X64 ? "direct COFF x64 executable main must not take parameters" :
                           (ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 ? "direct x86_64 Mach-O executable main must not take parameters" :
                            "direct AArch64 Mach-O executable main must not take parameters"))));
    return z_build_diag(ctx, diag, message, main_fun->line, main_fun->column, main_fun->name);
  }
  bool return_ok = ctx->backend == Z_DIRECT_BACKEND_ELF64 ? z_build_is_scalar32(main_fun->return_type)
                                                         : (main_fun->return_type == IR_TYPE_VOID || z_build_is_scalar32(main_fun->return_type));
  if (!return_ok) {
    const char *message = ctx->backend == Z_DIRECT_BACKEND_ELF64 ? "direct ELF64 executable main must return a 32-bit-or-smaller scalar" :
                          (ctx->backend == Z_DIRECT_BACKEND_ELF_AARCH64 ? "direct AArch64 ELF executable main must return Void or a 32-bit-or-smaller scalar" :
                          (ctx->backend == Z_DIRECT_BACKEND_COFF_AARCH64 ? "direct COFF AArch64 executable main must return Void or a 32-bit-or-smaller scalar" :
                          (ctx->backend == Z_DIRECT_BACKEND_COFF_X64 ? "direct COFF x64 executable main must return Void or a 32-bit-or-smaller scalar" :
                           (ctx->backend == Z_DIRECT_BACKEND_MACHO_X64 ? "direct x86_64 Mach-O executable main must return Void or a 32-bit-or-smaller scalar" :
                            "direct AArch64 Mach-O executable main must return Void or a 32-bit-or-smaller scalar"))));
    return z_build_diag(ctx, diag, message, main_fun->line, main_fun->column, z_build_type_name(main_fun->return_type));
  }
  return true;
}

bool z_direct_buildability_check(const IrProgram *ir, const ZTargetInfo *target, const char *emit_kind, ZDiag *diag) {
  ZBuildability ctx;
  if (!z_build_select(ir, target, emit_kind, &ctx, diag)) return false;
  if (!ir->mir_valid) {
    return z_build_diag(&ctx, diag, ir->mir_message[0] ? ir->mir_message : "direct backend lowering failed", ir->mir_line, ir->mir_column, ir->mir_actual);
  }
  if (ir->function_len == 0) return z_build_diag(&ctx, diag, "direct backend buildability requires at least one exported function", 1, 1, "empty program");
  bool has_export = false;
  for (size_t i = 0; i < ir->function_len; i++) {
    if (ir->functions[i].is_exported) has_export = true;
    // Even on the aarch64-direct fast-path, where buildability normally only inspects exported
    // callees, gate raising+record everywhere so the per-backend emitter never sees an unsupported
    // callee. This produces a uniform BLD004 across backends rather than a CGEN004 surfacing from
    // a downstream LOCAL_SET handler.
    // Same for ref<Record> / mutref<Record> params — non-host backends don't yet emit ref-record
    // marshaling, so reject in buildability even for non-exported callees.
    bool has_ref_param = false;
    for (size_t p = 0; p < ir->functions[i].param_count; p++) {
      if (ir->functions[i].locals[p].is_ref) { has_ref_param = true; break; }
    }
    if (z_build_backend_is_aarch64_direct(ctx.backend) && !ir->functions[i].is_exported &&
        !(ir->functions[i].raises && ir->functions[i].return_type == IR_TYPE_RECORD) &&
        !has_ref_param) continue;
    if (!build_check_function_shape(&ctx, &ir->functions[i], diag)) return false;
    if (!build_check_instrs(&ctx, &ir->functions[i], ir->functions[i].instrs, ir->functions[i].instr_len, diag)) return false;
  }
  if (!has_export) return z_build_diag(&ctx, diag, "direct backend buildability requires at least one exported function", 1, 1, "no exported function");
  if (ctx.executable && !build_check_executable_shape(&ctx, ir, diag)) return false;
  return true;
}
