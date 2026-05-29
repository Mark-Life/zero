# Widen the direct-ELF64 fallible-return ABI to all value types

**Status:** DONE (2026-05-20, `feat/std-math-llama2`). All five new fixtures pass
at runtime under `docker run --platform linux/amd64`; error propagation exits 1;
full `conformance` green; no regressions across the existing fallible surface.

## Problem

A `raises` function (or fallible std call) in the direct ELF64 backend could only
return `{Void, Bool, u8, u16, usize, i32, u32}`. Returning `f32`, `f64`, `i64`, or
`u64` from a fallible function failed with `CGEN004: direct backend fallible return
type is unsupported`. Found while writing `mmap-munmap-early-return.0` (an
`f32 raises` helper).

## Root cause

The old fallible ABI packs the result into a single register, `rax`:

- **Error:** `rax = (error_code << 32)` — tag in the **high 32 bits**
  (`elf_emit_packed_error_rax`).
- **Success:** value in the **low 32 bits**, high word 0.
- **Detect:** `mov rcx,rax; shr rcx,32; test ecx,ecx` — high word nonzero = error
  (`elf_emit_error_condition_from_rax`, and the `_start` stub for `main`).

So a fallible success value had to fit in 32 bits. `f64`/`i64`/`u64` need all 64
bits (they collide with the tag); `f32` fits but lived in `xmm0`, not `rax`.
(Also latent: a fallible `usize` success was masked to 32 bits — silent truncation
of values > 4 GiB, the same family as phase-0-followups #3.)

## New ABI — two registers

- **`rdx` = error tag.** `0` = success, nonzero = error code.
- **value = its natural register**, exactly like an infallible return:
  - integers (incl. `i64`/`u64`/`usize`) → `rax`, full width.
  - `f32`/`f64` → `xmm0`.

Why this is the natural shape: the `IR_INSTR_RETURN` emitter already leaves floats
in `xmm0` and skips the low-32 masking for `i64`/float — the success side was
already wide-ready. Only the tag, sharing `rax`, blocked wide/float values.
`Maybe<scalar>` is an in-memory 16-byte local `{tag@0, value@8}` (already 64-bit),
not register-packed, so it is unaffected.

## Touchpoints (direct backend)

`native/zero-c/src/emit_elf64.c`:

- `elf_emit_packed_error_rax` → set `rdx = code` (was `rax = code<<32`).
- `elf_emit_error_condition_from_rax` → `test rdx,rdx` (was high-word test).
- Inline fs intrinsics that pack a fallible result: `FS_OPEN`/`FS_CREATE`,
  `FS_FILE_LEN`, `FS_READ_FILE`, `FS_WRITE_ALL_FILE` — success sets `rdx=0` and
  leaves the value in `rax`; failure sets `rdx=code`.
- Fused `*OrRaise` emitters: `elf_emit_read_all_or_raise_to_local`,
  `elf_emit_mmap_or_raise_to_local` — error paths set `rdx=code`.
- `IR_VALUE_CHECK` / `IR_VALUE_RESCUE` — test `rdx`; drop the low-32 masking; value
  is already in the right register (`rax`/`xmm0`) per the result type.
- `IR_INSTR_RAISE` → `rdx=code`. `IR_INSTR_RETURN` (success) → `rdx=0`, value left
  in `rax`/`xmm0` (no masking).
- `_start` stub → `test rdx,rdx`; exit code from `eax` on success, `1` on error.

`native/zero-c/src/ir.c`:

- `ir_type_is_direct_fallible_value` → `Void || ir_type_is_direct_abi(type)`
  (same set as infallible returns).
- Fallible user-call transit type: float returns are typed by their natural float
  type (value in `xmm0`); all other fallible returns keep the `i64` transit marker
  (value in `rax`, full width — no truncation).
- Three lowering guards keyed on the `i64` transit marker had to also accept the
  float transit type, or float fallible calls were rejected before emit:
  `EXPR_CHECK`, `EXPR_RESCUE`, and `STMT_CHECK` (`!= IR_TYPE_I64` →
  `!= IR_TYPE_I64 && !ir_type_is_float(...)`).

## Verification (runtime, docker amd64)

- `fallible-return-{f32,f64,i64,u64,usize-wide}` → all print `... ok`, exit 0.
  `usize-wide` returns 5_000_000_000 and checks it round-trips (old ABI truncated
  it to 705_032_704).
- Error propagation: a fallible `f32` that `raise`s, unrescued, exits 1 with no
  output (rdx tag → `_start` → exit 1).
- Regression: `mmap-*`, `hello`, `primitive-stdlib`, `float-primitives`,
  `page-alloc-region` still pass; the pre-existing CGEN004 fallible fixtures
  (`rescue-check`, `fallibility-error-sets`, `check-maybe-fallibility`,
  `maybe-error-flow`) are unchanged (they fail on unrelated features — String,
  `MutSpan<u8>`/drop, `Maybe<u8>` returns — not the fallible value type).

## Tests

New conformance fixtures (runtime-verified via the Docker amd64 recipe), each a
fallible helper returning the type and a `check` at the call site:
`fallible-return-f32`, `-f64`, `-i64`, `-u64`, `-usize-wide` (> 4 GiB to prove the
truncation fix). Plus regression: the whole existing fallible surface
(`std-fs-*`, `mmap-*`, `rescue-check`, `fallibility-*`, `maybe-error-flow`) must
stay green. `mmap-munmap-early-return.0` can revert to the cleaner `f32 raises`
helper form once this lands.
