# Floats + std.math — Implementation Plan (Strategy A)

Executable plan for v1 of the work scoped in [`overview.md`](./overview.md). Lands libm-linked `std.math` on `linux-musl-x64`, unblocking `llama2.zero` v0.1.

## Goal

Ship `let y: f32 = sqrt(x * x + 1.0)` compiling and running correctly on `linux-musl-x64`, with `std.math` exposing the 5 functions `llama2.zero` v0.1 needs (`expf`, `sqrtf`, `cosf`, `sinf`, `powf`) plus the basic arithmetic/cast surface around them.

## Scope (this plan only)

**In:**

- `IR_TYPE_F32` and `IR_TYPE_F64` in the IR.
- Float arithmetic, comparison, casts on `linux-musl-x64` (ELF x86-64, SSE2).
- Float literals.
- `std.math` module with the 5 llama2 functions + a handful of inevitable companions (`absf`, `floorf`, `isNaNf`, constants `PI_F`, `INFINITY_F`, `NAN_F`).
- libm linked through the existing `target.c` system-libraries pipeline — same audit pattern as `std.http` → libcurl.
- Conformance fixtures for every op and every shipped math function.

**Out (later phases):**

- Other backends (AArch64, Mach-O, COFF) — covered in math roadmap v2.
- Full `std.math` API (logs, trig beyond sin/cos, hyperbolic, etc.) — math roadmap v3.
- NaN/Inf/denormal conformance sweep — math roadmap v4.
- `f16` / `bf16` — math roadmap v5.
- Strategy B (embedded kernels). Documented as a future migration path only.

## Prerequisites

Verified in `overview.md` "State Today":

- Checker accepts `f32` and `f64` (`checker.c:3515`).
- No IR float types (`include/zero.h:372-388`).
- No backend emits float instructions.

Nothing else to land first.

## Phases

Each phase is independently shippable and leaves the tree green.

### Phase 1 — IR float types

Touchpoints:

- `include/zero.h:372-388` — add `IR_TYPE_F32`, `IR_TYPE_F64`.
- `ir.c` — extend dispatch tables for `IR_VALUE_ADD/SUB/MUL/DIV/COMPARE/CAST` to recognize the new types. No new IR ops expected; confirm by reading dispatch sites.
- `checker.c` — wire the existing float type checks to emit the new IR types.

Exit: `zero check` on a float program still passes; `zero graph --json` shows IR nodes typed `f32`/`f64`.

### Phase 2 — ELF x86-64 float codegen

Touchpoints:

- `emit_elf64.c:79-80` — extend `elf_type_is_supported_scalar()` to accept the float types.
- `emit_elf64.c` — emit SSE2 for:
  - Load/store: `movss` / `movsd`.
  - Arithmetic: `addss/sd`, `subss/sd`, `mulss/sd`, `divss/sd`.
  - Compare: `ucomiss/sd` + flag branch. Document NaN-PF behavior.
  - Casts: `cvtsi2ss/sd`, `cvttss2si/sd`, `cvtss2sd`, `cvtsd2ss`.
  - `sqrtss/sd` for intrinsic `sqrt` (also reused by `std.math.sqrtf`).
- Register allocation: use XMM0–XMM7 for FP args/temps (System V ABI). GPR path unchanged.

Default policy: IEEE 754 strict, no flush-to-zero. Document in code comment near the SSE2 emitter, not as a runtime knob.

Exit: programs using `+ - * /` `< <= > >= == !=` and casts on `f32`/`f64` compile and run correct values on `linux-musl-x64`.

### Phase 3 — libm link plumbing

Touchpoints:

- `target.c` (around lines 420–434 where `std.http` registers `curl`) — register `m` in `systemLibraries` for `linux-musl-x64`.
- Confirm `zero graph --json` linkPlan surfaces `-lm` for any package that imports `std.math`.
- No capability gate (per Open Question 1 recommendation — math is always-on).

Exit: a stub `std.math.sqrtf` declaration links and resolves to `sqrtf@PLT`. Nothing functional yet — just the link path proven.

### Phase 4 — `std.math` module (llama2 subset)

Touchpoints:

- New stdlib module — `std.math`. Location and registration: mirror whatever pattern `std.http` uses for declaring external-libc-backed functions. Functions to expose:
  - `sqrtf(x: f32) -> f32`
  - `expf(x: f32) -> f32`
  - `cosf(x: f32) -> f32`
  - `sinf(x: f32) -> f32`
  - `powf(x: f32, y: f32) -> f32`
  - `absf(x: f32) -> f32`
  - `floorf(x: f32) -> f32`
  - `isNaNf(x: f32) -> Bool` (likely a Zero-side bit-pattern check, not a libm call)
- Constants: `PI_F: f32`, `E_F: f32`, `INFINITY_F: f32`, `NAN_F: f32`. Confirm `const` works for stdlib globals today; if not, fall back to nullary functions (Open Question 5).
- Code emit: math calls lower to PLT calls. No special casing — same path as any extern declaration.

Naming follows C convention (`sqrtf` not `sqrt`) per Open Question 3 recommendation. f64 versions skipped in this phase — llama2 doesn't need them.

Exit: a Zero program can `import std.math` and call all 8 functions; output matches libm bit-for-bit on `linux-musl-x64`.

### Phase 5 — Conformance fixtures

Touchpoints under `conformance/native/pass/`:

**Layer 0 (codegen):**

- `float-arith-{add,sub,mul,div}-{f32,f64}.0`
- `float-compare-{eq,lt,le,gt,ge,ne}-{f32,f64}.0`
- `float-cast-{f32-to-f64,f64-to-f32,i32-to-f32,f32-to-i32,...}.0`
- `float-nan-compare.0` — NaN is never equal, including to itself.
- `float-inf-arith.0` — `1.0 / 0.0 == Inf`, `-1.0 / 0.0 == -Inf`.

**Layer 1 (std.math, llama2 subset):**

- `math-sqrtf-known-values.0` — `sqrtf(4.0) == 2.0`, `sqrtf(0.0) == 0.0`, `sqrtf(-1.0)` is NaN.
- `math-expf-known-values.0` — `expf(0.0) == 1.0`, `expf(1.0) ≈ 2.71828`.
- `math-cosf-sinf-identity.0` — `cosf(x)*cosf(x) + sinf(x)*sinf(x) ≈ 1.0` within tolerance.
- `math-powf-known-values.0` — `powf(2.0, 10.0) == 1024.0`, `powf(0.0, 0.0) == 1.0`.
- `math-isNaNf.0` — predicate behaves on NaN, Inf, normal, zero.

Defer denormal + full NaN/Inf sweep to math roadmap v4.

Exit: `pnpm run conformance` green; new fixtures all pass.

**Phase 5 implementation caveats:**

- ~~NaN-compare codegen gap: `emit_elf64.c`'s `elf_xmm_setcc_opcode` maps IR compare ops to raw `setcc` off UCOMISS with no PF (parity flag) fixup. Result under current codegen: `nan == nan` → TRUE, `nan != nan` → FALSE, `nan < x` / `nan <= x` → TRUE — all non-IEEE. Only `>` and `>=` accidentally return correct (false) results for NaN comparisons (SETA/SETAE happen to read the right flag combinations). `float-nan-compare.0` is scoped to those provably-correct cases plus `isNaNf` detection — consistent with the "Defer denormal + full NaN/Inf sweep to math roadmap v4" decision. A full IEEE-strict NaN sweep would surface this as a Phase 2 bug; the fix is a PF fixup branch (or `setnp`+`sete` AND / `setp`+`setne` OR for == and !=).~~ *Resolved by F1 — `emit_elf64.c` now emits PF-aware sequences after UCOMISS/UCOMISD for `==`, `!=`, `<`, `<=`. `float-nan-compare.0` was widened to exercise all six ops against NaN. `>` and `>=` are unchanged. See "F1 — NaN comparison codegen fix" below.*
- libm-backed math fixtures (`math-sqrtf-*`, `math-expf-*`, `math-cosf-sinf-*`, `math-powf-*`, `math-absf-floorf`) only validate runtime in environments with a target-capable C compiler. On hosts without one they get build-skipped via BLD003 and the test passes through the `assertDirectRuntimeOrUnsupported` tolerant path — runtime behavior is only checked in the Vercel sandbox or any environment with a cross-compiler installed. `math-isnanf` and all 29 Layer 0 float fixtures avoid this gap because they don't link libm. *Made observable by F2 — the harness now warns per-fixture + summary line on BLD003 skips, and `scripts/setup-cross-toolchain.sh` bootstraps a target-capable toolchain on macOS.*

### Phase 6 — llama2 smoke test

Not a conformance phase; a real-program check.

- A standalone Zero program that does one RMSNorm over a small `Span<f32>` using `sqrtf`, and one softmax slice using `expf`. Numerical output compared against a Python or C reference.
- Lives in `examples/` (not `examples/llama2/` yet — that's a separate project), or as a benchmark fixture under `benchmarks/zero/` per llama2 overview Open Question 5.

Exit: real Zero code uses the new math surface end-to-end; numerical results match reference within libm tolerance.

**Phase 6 as shipped:**

- Landed as two conformance fixtures, not under `examples/`: `conformance/native/pass/math-rmsnorm-smoke.0` and `conformance/native/pass/math-softmax-smoke.0`. Reuses the existing harness pattern (`assertDirectRuntimeOrUnsupported`) so a target-capable host validates numerics and a host without one falls back to BLD003 — same shape as the libm-backed Layer 1 fixtures.
- RMSNorm fixture: `[4]f32` input `[3,4,0,0]`, weights all `1.0`, epsilon `1e-5`. Tolerance bands `[1.199,1.201]` for index 0, `[1.599,1.601]` for index 1, `±0.001` for zeros.
- Softmax fixture: `[3]f32` input `[1,2,3]`, max-subtract path. Tolerance bands per-index plus a sum-to-1.0 check and ascending-monotonicity check.
- Both fixtures inline the computation in `main()` over fixed-size array locals; no helper functions take spans. See "Side discoveries" below for why.

**Phase 6 implementation caveats:**

- Same BLD003 host caveat as the Phase 5 libm-backed fixtures: runtime numerics only validate on hosts with a target-capable C compiler (Vercel sandbox, or any Linux x86-64 with a cross-toolchain). On Darwin without one, `assertDirectRuntimeOrUnsupported` returns through the tolerant path. *F2 made this observable — the harness now emits a visible warning per-skip and a summary at the end; `scripts/setup-cross-toolchain.sh` is the macOS opt-in.*
- Phase 6 surfaced gaps not anticipated when Phase 2 was written: `[N]f32`/`[N]f64` array locals were rejected by both the IR layer and the ELF64 backend (Phase 2's `elf_type_is_supported_scalar` covered scalars only). Required four follow-on edits to land the smoke test, all in-scope for "f32 math end-to-end":
  - `ir.c:518` — `ir_parse_fixed_array_type_for_program` now accepts `IR_TYPE_F32`/`IR_TYPE_F64` as element types (previously only `U8`/`I32`/`U32`).
  - `emit_elf64.c:2241` — fixed-array local allow-list extended to F32/F64.
  - `emit_elf64.c` `IR_VALUE_INDEX_LOAD` — emits `MOVSS xmm0, [rax]` (F3 0F 10 00) for f32, `MOVSD` (F2 0F 10 00) for f64.
  - `emit_elf64.c` `IR_INSTR_INDEX_STORE` — emits `MOVSS [rcx], xmm0` (F3 0F 11 01) for f32, `MOVSD` (F2 0F 11 01) for f64.

**Side discoveries that block llama2 v0.1 (out of this plan):**

These surfaced during Phase 6 but were not in scope to fix here. Each is a concrete prerequisite before `examples/llama2/` makes sense.

1. **`Span<f32>`/`MutSpan<f32>` as function parameters not lowered.** `ir.c:161-167` maps only `Span<u8>` / `Span<const u8>` / `MutSpan<u8>` to `IR_TYPE_BYTE_VIEW`; other typed spans return `IR_TYPE_UNSUPPORTED`. Result: the smoke test had to inline both kernels in `main()` over `[N]f32` arrays. Llama2's matmul, RMSNorm, softmax, RoPE, attention all want to be reusable functions taking `Span<f32>`. The fix is a typed-span IR shape (fat pointer with ptr+len+element-type) analogous to `BYTE_VIEW`, plus matching ABI/lowering for params. Non-trivial — likely its own plan.
2. **No binary f32 file reading.** ~~`std.codec` exposes `readU8/U16/U32` but no `readF32Le` and no `u32→f32` bit-pattern reinterpret op. Existing float casts are numeric (`cvtsi2ss`), not bit-preserving. Without one of these, `checkpoint.0` cannot decode `stories15M.bin` (or any IEEE-754 little-endian weight file). The lighter fix is `std.codec.readF32Le(bytes, offset) -> f32`; the more general fix is a typed bitcast op.~~ *Resolved by F4 — `std.codec.readF32Le(bytes: Span<u8>, offset: usize) -> f32` and `readF64Le` lower to a new IR op that emits a bounds-checked `MOVSS`/`MOVSD` load directly from `[ptr + offset]`. Bit-preserving by construction (no GPR↔XMM hop, no integer narrowing).*
3. **Span passing for the matmul hot loop.** Tied to (1). Even with `Span<f32>` lowered as a value type, the matmul inner loop benefits from contiguous, bounds-checked indexing of a 2D-shaped view; today only flat 1D arrays are codegen-supported.

Cross-reference: the llama2 overview's "What This Forces Into Zero" items 3 and 5 align with these — item 3 (f32 math coverage) is now satisfied for scalar/inlined-array use; item 5 (endianness-aware binary reads) corresponds to side discovery #2 above. Note the overview's mention of `tanhf` is incorrect — Llama 2 uses SwiGLU which only needs `expf` (for sigmoid). No tanhf required for v0.1.

## File touchpoints summary

| File | Phase | Change |
|------|-------|--------|
| `include/zero.h:372-388` | 1 | add `IR_TYPE_F32`, `IR_TYPE_F64` |
| `ir.c` | 1 | extend dispatch tables |
| `checker.c` | 1 | wire float types to new IR types |
| `emit_elf64.c:79-80` | 2 | extend `elf_type_is_supported_scalar` |
| `emit_elf64.c` | 2 | SSE2 emit for arith/compare/cast |
| `target.c` (~420) | 3 | register `m` in `systemLibraries` |
| `stdlib/math.0` (new) | 4 | the module itself |
| `conformance/native/pass/` | 5 | float-* and math-* fixtures |
| `conformance/native/pass/math-rmsnorm-smoke.0` (new) | 6 | inlined RMSNorm over `[4]f32` |
| `conformance/native/pass/math-softmax-smoke.0` (new) | 6 | inlined softmax over `[3]f32` |
| `ir.c:518` | 6 | `ir_parse_fixed_array_type_for_program` accepts F32/F64 elements |
| `emit_elf64.c:2241` | 6 | fixed-array allow-list extended to F32/F64 |
| `emit_elf64.c` `IR_VALUE_INDEX_LOAD` | 6 | MOVSS/MOVSD load for float array element |
| `emit_elf64.c` `IR_INSTR_INDEX_STORE` | 6 | MOVSS/MOVSD store for float array element |
| `include/zero.h` | F4 | add `IR_VALUE_BYTE_VIEW_READ_FLOAT_LE` |
| `ir.c` `EXPR_CALL` | F4 | lower `std.codec.readF{32,64}Le(bytes, offset)` |
| `checker.c` | F4 | wire `readF{32,64}Le` types (`Span<u8>`, `usize`) and returns (f32/f64) |
| `emit_elf64.c` `IR_VALUE_BYTE_VIEW_READ_FLOAT_LE` | F4 | bounds-checked MOVSS/MOVSD load from `[ptr+offset]` |
| `main.c` | F4 | capability-table entries for both functions |
| `docs-site/articles/modules/codec.md` | F4 | status-table entries |
| `conformance/native/pass/codec-read-f32-le.0` (new) | F4 | f32 1.0 round-trip |
| `conformance/native/pass/codec-read-f64-le.0` (new) | F4 | f64 1.5 round-trip |
| `conformance/native/pass/codec-read-f32-le-offset.0` (new) | F4 | three f32 reads at offsets 0/4/8 |
| `conformance/native/fail/codec-read-f32-le-bounds.0` (new) | F4 | offset 1 into 4-byte buffer → bounds trap |

## Risks

1. **Existing IR dispatch may not be type-polymorphic.** The overview claims new IR ops are "likely unneeded" but this needs to be confirmed by reading `ir.c` dispatch sites in Phase 1. If the dispatcher hardcodes integer semantics, the change ripples wider. *Materialized in Phase 6:* scalar dispatch (Phase 1/2) was sufficient, but the **array** dispatch hardcoded integer element types in both `ir.c:518` and `emit_elf64.c:2241` + the `INDEX_LOAD`/`INDEX_STORE` emitters. Resolved in Phase 6 as four small edits — see Phase 6 caveats. Typed-span (`Span<T>` for non-`u8` `T`) is the next layer of the same risk and remains unresolved; see Phase 6 "Side discoveries" #1.
2. **NaN comparison semantics divergence across backends.** Phase 2 sets the precedent for the other three backends; if SSE2's PF-on-unordered behavior isn't carefully matched by AArch64/Mach-O/COFF later, fixtures will silently pass only on x64.
3. **Stdlib const globals may not work today.** Phase 4 assumes `const` works for module-level values. If not, fall back to nullary functions and revisit when `const` lands. Open Question 5.
4. **PLT call path for math functions.** Phase 3 assumes external function calls already work via `target.c` system-libraries (proven by `std.http`). If `std.http`'s path is curl-specific in any non-obvious way, libm needs the generalization.
5. **libm tolerance vs llama2 numerical expectations.** libm `expf`/`sinf`/`cosf` results match across musl/glibc/BSD to within a few ULPs but aren't bit-identical. llama2 reference outputs assume some specific libm — pick musl as the canonical reference for the v0.1 fixture.

## Exit criteria (whole plan)

- `pnpm run conformance` green with new fixtures.
- `pnpm run native:test` green.
- `pnpm run docs:test` green — `docs-site/articles/modules/math.md` landed with F5; module count + per-module label + F5 precision-policy assertions all pass.
- `zero graph --json` on a package importing `std.math` shows `-lm` in linkPlan.
- llama2 smoke test produces numerically correct RMSNorm + softmax outputs.
- ~~`llama2.zero` v0.1 work can begin without any further blockers from the float/math side.~~ *Revised after Phase 6:* The **scalar + inlined-array** math surface is unblocked end-to-end on `linux-musl-x64`. ~~Llama2 v0.1 still has two non-math-but-math-adjacent blockers surfaced during Phase 6 — see Phase 6 "Side discoveries" #1 (typed `Span<f32>` params) and #2 (binary f32 file reading).~~ *Revised after F3:* Side discovery #1 is now resolved — F3 landed typed `Span<T>` parameters for `T ∈ {u8, i32, u32, i64, u64, f32, f64}`, so reusable kernels over `Span<f32>` work end-to-end. ~~Side discovery #2 (binary f32 file reading) is now the only remaining hard blocker for llama2 v0.1 from this plan's perspective — tracked as F4 below.~~ *Revised after F4:* Side discovery #2 is now resolved — `std.codec.readF32Le(bytes, offset)` and `readF64Le(bytes, offset)` land as a runtime byte-view + offset op with bounds-checked `MOVSS`/`MOVSD` loads. `examples/llama2/` is now unblocked from the float/math/codec side end-to-end on `linux-musl-x64`. *Revised after F5:* libm precision policy is now documented in `docs-site/articles/modules/math.md` — musl pinned as the canonical libm on `linux-musl-x64`, tolerance ≥ a few ULPs the rule for libm-derived values, and `examples/llama2/README.md` (when it lands) will link to that page rather than restating the policy.

## Follow-up plan (post-Phase 6)

Five items surfaced during Phases 5–6 that this plan did not resolve. Numbered F1–F5 in the order they appeared in the post-implementation review (not in priority order — see the table at the end of this section for sequencing).

### F1 — NaN comparison codegen fix *(landed)*

**Problem.** `emit_elf64.c`'s `elf_xmm_setcc_opcode` maps IR compare ops to raw `setcc` off `UCOMISS`/`UCOMISD` with no PF (parity flag) fixup. NaN propagates through PF, but only `>` and `>=` (`SETA`/`SETAE`) read the right flag combination by accident. Result: `nan == nan → TRUE`, `nan != nan → FALSE`, `nan < x → TRUE`, `nan <= x → TRUE` — all non-IEEE.

**Impact.** Latent correctness bug. Llama2's forward pass shouldn't produce NaN under normal operation, so this wasn't a v0.1 blocker. But it silently corrupted results when triggered, and the same gap would replicate into AArch64 / Mach-O / COFF if not fixed first.

**Resolution.** `emit_elf64.c` now emits a PF-aware fixup pair inline after the existing `UCOMISS`/`UCOMISD` + primary `setcc`:

- `==` → `SETE al; SETNP cl; AND al, cl` (NaN → unordered → not equal).
- `!=` → `SETNE al; SETP cl; OR al, cl` (NaN → unordered → not equal → true).
- `<`  → `SETB al; SETNP cl; AND al, cl`.
- `<=` → `SETBE al; SETNP cl; AND al, cl`.
- `>`, `>=` unchanged — `SETA`/`SETAE` already read CF in a way that yields false on PF=1.

`cl`/`rcx` is free at this point in the compare lowering (operands have already been popped into `xmm0`/`xmm1`; result is built into `eax`). No new IR ops; the fixup is an inline byte sequence gated by `elf_xmm_compare_needs_parity_fixup`.

**Code touchpoints.**

- `native/zero-c/src/emit_elf64.c` `elf_xmm_setcc_opcode` — comment rewritten to document the new contract.
- `native/zero-c/src/emit_elf64.c` `elf_xmm_compare_needs_parity_fixup` (new) — gates the fixup emit.
- `native/zero-c/src/emit_elf64.c` `IR_VALUE_COMPARE` (float path) — emits the fixup pair when needed.
- `conformance/native/pass/float-nan-compare.0` — widened from 4 cases (`>`, `>=`) to 14 (`==`, `!=`, `<`, `<=`, `>`, `>=` against NaN, including `nan == nan` and `nan != nan`).

**Verified.**

- `make -C native/zero-c` clean.
- `ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs` → `conformance ok`.
- Disassembly of the rebuilt `float-nan-compare.0` ELF confirms expected byte sequences for all six ops. Runtime numerics validate in CI (Linux x86-64) and the Vercel sandbox — Darwin host without a Linux cross-toolchain takes the build-only path.

**Must comply with (carried forward to other backends).**

- IEEE 754: any compare with NaN is false except `!=`.
- `isNaNf` (bit-pattern based) keeps working — fix is orthogonal.
- Same semantics across every backend. AArch64 / Mach-O / COFF must replicate when float codegen lands there (NEON `fcmp` sets a different flag layout; the *contract* is shared, the *encoding* isn't).

**Scope.** Landed: ~half a day on x64. Each new backend re-implements per its FP unit.

**Dependencies.** None.

### F2 — Host-environment test gap (BLD003) *(landed)*

**Problem.** libm-linked fixtures (`math-sqrtf-*`, `math-expf-*`, `math-cosf-sinf-*`, `math-powf-*`, `math-absf-floorf`, `math-rmsnorm-smoke`, `math-softmax-smoke`) only validate numerics on hosts with a target-capable C compiler. On Darwin without a Linux cross-toolchain they skip via `BLD003` through `assertDirectRuntimeOrUnsupported`'s tolerant path. The 29 Layer-0 float fixtures + `math-isnanf` avoid the gap because they don't link libm.

**Impact.** Mac developers may merge libm-breaking code without seeing it locally. Real validation only happens in the Vercel sandbox or Linux CI. Not a runtime correctness issue — a feedback-loop issue.

**Resolution.** Three changes land the visibility + bootstrap:

- `conformance/run.mjs` `assertDirectRuntimeOrUnsupported` now takes an `expected.libm` flag. When a libm-flagged fixture hits BLD003, the harness pushes its name to a module-level `libmSkipped` list and prints a per-fixture `console.warn` line. After the suite finishes, a summary `console.warn` lists every skipped libm fixture and points at the bootstrap script. The tolerant skip path is preserved — exit code unchanged.
- The seven libm-linked math fixtures are tagged `libm: true` in their fixture entry (`math-sqrtf-known-values`, `math-expf-known-values`, `math-cosf-sinf-identity`, `math-powf-known-values`, `math-absf-floorf`, `math-rmsnorm-smoke`, `math-softmax-smoke`). `math-isnanf` is intentionally untagged — its calls (`isNaNf`, `nanF`, `infinityF`) lower to inline bit-pattern code (`ir.c:1896-1918`), not libm.
- `scripts/setup-cross-toolchain.sh` installs the bundled cross-toolchain (zig) via Homebrew on macOS, no-ops on Linux x86-64, and verifies in either case by attempting to build `math-sqrtf-known-values.0` for `linux-musl-x64` and grepping the resulting JSON for absence of `BLD003`. Supports `ZERO_CC` override (e.g. for `FiloSottile/musl-cross/musl-cross` users) and exits non-zero if BLD003 still fires post-install.
- `README.md` "Validation" gains a paragraph naming the libm fixtures, what skipping looks like, and the bootstrap script.

**Code touchpoints.**

- `conformance/run.mjs` `libmSkipped` (new), `assertDirectRuntimeOrUnsupported` BLD003 branch, end-of-suite summary.
- `conformance/run.mjs` libm-fixture table entries — seven `libm: true` flags added.
- `scripts/setup-cross-toolchain.sh` (new, executable).
- `README.md` — one-paragraph note in "Validation".

**Verified.**

- `bash -n scripts/setup-cross-toolchain.sh` clean.
- `ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs` on Darwin without zig: emits seven inline warnings + one summary warning + `conformance ok`. Exit code 0.
- `node --test docs-site/src/tests/*.test.mjs` green — README change clears the docs registry asserts.
- BLD003 grep pattern `"code": *"BLD003"` matches the actual pretty-printed JSON from `zero build --json`.

**Must comply with (carried forward).**

- CI behavior unchanged — Linux runners still validate end-to-end.
- No new mandatory developer dependency — skip-with-warning preserves the tolerant path; the script is opt-in.
- New libm-linked fixtures must add `libm: true` to keep the warning observable.

**Scope.** Landed: ~0.5 day docs + harness + script. New backends inherit the warning automatically once they land libm-using fixtures.

**Dependencies.** None.

### F3 — Typed `Span<T>` parameters *(landed)*

**Problem.** `ir.c:161-167` mapped only `Span<u8>`, `Span<const u8>`, and `MutSpan<u8>` to `IR_TYPE_BYTE_VIEW`; other element types returned `IR_TYPE_UNSUPPORTED`. The ELF64 backend also rejected every byte-view parameter outright (`emit_elf64.c:2273-2276`), so even `Span<u8>` params CGEN004-skipped. Functions couldn't take typed float slices, so numerical kernels had to inline.

**Impact.** Hard blocker for `examples/llama2/`. Llama2's matmul, RMSNorm, softmax, RoPE, attention, FFN are reusable functions over `Span<f32>` in any sane port. Without typed spans, llama2 collapses into one inline `main()` or isn't writable at length.

**Resolution.** Element-type-on-existing-kind: `IrLocal.element_type` (already used for fixed arrays) now also carries the span's `T` when `type == IR_TYPE_BYTE_VIEW`. Non-span byte views (`String`, `ByteBuf`, `BufferedReader`, ...) keep `element_type = IR_TYPE_U8`, so every existing consumer behaves identically. Five phases landed:

1. **IR plumbing** — `ir.c` `ir_type_kind` now recognizes `Span<T>` / `MutSpan<T>` / `Span<const T>` for `T ∈ {u8, i32, u32, i64, u64, f32, f64}` (matches the fixed-array element-type set). New helper `ir_parse_span_inner_type` parses the inner `T`; `ir_byte_view_element_type` returns the element kind. `ir_collect_function_locals` and `ir_collect_stmt_locals` thread `element_type` onto the local; `let mut span: MutSpan<T> = ...` is recognized as mutable for any `T` (not just `MutSpan<u8>`).
2. **ELF64 parameter passing** — `elf_validate_function` (line 2273-2276) now accepts byte-view params whose `element_type` is in the supported allow-list. The prologue loop (line 2956-2980) consumes **two** GPRs per byte-view param: ptr → slot 0, len → slot 8 (both 64-bit stores). The IR-level direct-ABI gate on parameters (`ir_collect_function_locals`) and call sites (`IR_VALUE_CALL` lowering) now lets `Span<T>` / `MutSpan<T>` through.
3. **INDEX_LOAD / INDEX_STORE generalization** — `elf_emit_bounds_checked_address` (line 812-826) now dispatches on `local->is_array` vs `local->type == IR_TYPE_BYTE_VIEW`. For byte views: load len from slot 8, `cmp index, len`, `jb` on success; load ptr from slot 0; scale by `element_type`. The element-type-keyed load/store opcodes (movzx for u8, mov for i32/u32, MOVSS/MOVSD for f32/f64, REX.W mov for i64/u64) are unchanged — both arrays and byte views share the same dispatch tail.
4. **IR lowering for typed-span indexing** — `span[i]` on a `BYTE_VIEW` local with `element_type != U8` now emits `IR_VALUE_INDEX_LOAD` (carrying the local index and element type) instead of `IR_VALUE_BYTE_VIEW_INDEX_LOAD`. `span[i] = x` for mutable typed spans routes through `IR_INSTR_INDEX_STORE`. The u8 path is unchanged for backward compat (string literals, `Span<u8>`, etc.).
5. **`[N]T → Span<T>` coercion** — `ir_lower_byte_view` accepts any supported element type when constructing `IR_VALUE_ARRAY_BYTE_VIEW`; the value carries `element_type`. `elf_emit_byte_view_ptr` for `ARRAY_BYTE_VIEW` dropped its `element_type != IR_TYPE_U8` rejection.

**Code touchpoints.**

- `native/zero-c/src/ir.c` — `ir_parse_span_inner_type` (new), `ir_byte_view_element_type` (new), `ir_type_kind` (parse `Span<T>` family generically), `ir_collect_function_locals` (line 3180-3196), `ir_collect_stmt_locals` (line 3234-3236), `IR_VALUE_CALL` parameter gate (line 2646-2665), `EXPR_INDEX` lowering (line 1322-1356 typed-span read path), `ir_lower_index_store` (line 2776-2806 typed-span write path), `ir_lower_byte_view` (line 1075-1088 typed-array coercion).
- `native/zero-c/src/emit_elf64.c` — forward declarations for `elf_emit_load_local_slot_*` (line 717-718), `elf_emit_bounds_checked_address` byte-view branch (line 812-840), `elf_emit_byte_view_ptr` for typed arrays (line 1011-1020), `IR_VALUE_CALL` byte-view arg push/pop (line 1233-1283), parameter prologue byte-view two-GPR consumption (line 2956-2985), `elf_validate_function` byte-view param allow-list (line 2273-2284).
- `conformance/native/pass/typed-span-f32.0` (new) — `sumThree(Span<f32>) -> f32`, `setFirst(MutSpan<f32>, f32) -> Void`, `copyThree(Span<f32>, MutSpan<f32>) -> Void`. Covers element read, indexed write, and pass-through.
- `conformance/native/pass/typed-span-i32.0` (new) — same shape for `i32`.
- `conformance/native/pass/math-rmsnorm-span.0` (new) — RMSNorm as `rmsnorm(input: Span<f32>, weights: Span<f32>, output: MutSpan<f32>, eps: f32) -> Void`, called from `main` over `[4]f32` locals. Tolerance bands match `math-rmsnorm-smoke.0`.
- `conformance/native/pass/math-softmax-span.0` (new) — softmax as `softmax(input: Span<f32>, output: MutSpan<f32>) -> Void`. Tolerance bands match `math-softmax-smoke.0`.
- `conformance/run.mjs` — six new fixture entries (two `libm: true` for the math ones).
- `.docs/spans/plan.md` (new) — full F3 plan.

**Verified.**

- `make -C native/zero-c` clean.
- `ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs` → `conformance ok` with 9 libm fixtures correctly skipped on Darwin (two new typed-span math kernels join the existing seven).
- `--emit obj` builds for `typed-span-f32.0`, `typed-span-i32.0`, `math-rmsnorm-span.0`, `math-softmax-span.0` all succeed; objdump confirms the SysV ABI is right (rdi=ptr, rsi=len, then xmm0=f32) and the index-load/store dispatch picks the correct opcode per element type (MOVSS for f32 spans, MOVSD for f64 spans, mov 32-bit for i32, etc.).
- Pre-existing `mutable-spans.0` builds cleanly now (was CGEN004-skipping before); `generic-spans.0` still CGEN004-skips because it has a function **returning** `Span<i32>` — return-by-span is a separate item (see F3.b in `.docs/spans/plan.md`).
- `node --test docs-site/src/tests/*.test.mjs` green.

**Must comply with (carried forward to other backends).**

- Two-GPR ABI per byte-view parameter: ptr first, len second, both 64-bit slots in the local frame. AArch64 / Mach-O / COFF must replicate the shape when they lift their own byte-view-param gates.
- Bounds-check is element-count vs index, not byte-count — len slot stores element count for typed spans (and for u8 spans, where stride=1, byte count == element count).
- Element-type allow-list at the backend validate stage: explicit and fail-closed.

**Scope.** Landed: ~1 day on x64. Each new backend re-implements the prologue and INDEX_LOAD/STORE per its FP/GPR layout.

**Dependencies.** None (was the design intent).

**Out of scope (tracked as F3.b).**

- Returning `Span<T>` from a function (SysV multi-reg return rax+rdx). Surfaced via `generic-spans.0`'s `arrayTail` which still CGEN004-skips.
- Slicing typed spans (`span[a..b]`) — `IR_VALUE_BYTE_SLICE` ptr arithmetic is byte-granular today; typed slicing needs `start * stride` scaling.
- Stdlib intrinsics over typed spans (`std.mem.copy`, `std.mem.eqlBytes`, ...) — they're typed for `Span<u8>` and remain so. Element-type-generic versions are a separate stdlib pass.

### F4 — Binary f32 reading *(landed)*

**Problem.** `std.codec` exposes `readU8/U16/U32` but no `readF32Le` and no bit-preserving `u32 → f32` reinterpret. Existing float casts (`cvtsi2ss`) are numeric (`5 → 5.0`), not bit-preserving. No way to decode IEEE-754 little-endian weight files.

**Impact.** Hard blocker for `examples/llama2/`. `stories15M.bin` is 60 MB of `f32` weights packed little-endian; llama2 v0.1 cannot load any model without this.

**Resolution.** Landed Option A as a runtime byte-view + offset op (not literal-only — the existing `readU*` literal-only special case wouldn't unblock llama2's file-loading path):

- `std.codec.readF32Le(bytes: Span<u8>, offset: usize) -> f32`
- `std.codec.readF64Le(bytes: Span<u8>, offset: usize) -> f64`

Both lower to a new IR op `IR_VALUE_BYTE_VIEW_READ_FLOAT_LE` whose ELF64 emission is a bounds-checked `MOVSS`/`MOVSD` load directly from `[ptr + offset]` — no intermediate u32 + bitcast register hop. The byte-view's existing `Span<u8>` two-GPR ABI (F3) feeds straight in. Bounds check is `offset + sizeof(T) <= len` (vs INDEX_LOAD's `index < len`); a `UD2` trap on violation matches every other bounds check in the backend.

Option B (general `bitcast<f32>(u32)`) was not done — `MOVSS [mem]` is a strict bit-preserving load from memory, so going through an integer register adds nothing for the byte-decode use case. Bitcast still has uses (half-precision, endian flips, byte-flat reinterpretation of existing f32 values), tracked separately for math roadmap v5.

**Code touchpoints.**

- `native/zero-c/include/zero.h` — `IR_VALUE_BYTE_VIEW_READ_FLOAT_LE` enum.
- `native/zero-c/src/checker.c` — `std_call_return_type` (f32/f64), `std_call_arg_count` (2 each), `std_call_arg_type` (index 0 → `Span<u8>`, index 1 → `usize`).
- `native/zero-c/src/ir.c` — `EXPR_CALL` lowering after the `readU*` block: parses both args via `ir_lower_byte_view` and `ir_lower_expr`, builds `IR_VALUE_BYTE_VIEW_READ_FLOAT_LE` with `type` and `element_type` set to F32 or F64.
- `native/zero-c/src/emit_elf64.c` — new case in the value-emit switch right after `IR_VALUE_BYTE_VIEW_INDEX_LOAD`: emits the load sequence described above. COFF / Mach-O backends fall through to their `default:` diag, which routes through CGEN004 cleanly (float math is x64-ELF-only per the plan).
- `native/zero-c/src/main.c` — capability-table entries (`codec`, `target-neutral`, `little-endian f{32,64} read from byte view`, audit-flagged).
- `docs-site/articles/modules/codec.md` — "Status" table now lists both functions.
- `conformance/native/pass/codec-read-f32-le.0` (new) — `[4]u8` buffer with f32 1.0 LE bytes, reads at offset 0, compares against `1.0` literal.
- `conformance/native/pass/codec-read-f64-le.0` (new) — `[8]u8` buffer with f64 1.5 LE bytes, reads at offset 0, compares against `1.5` literal.
- `conformance/native/pass/codec-read-f32-le-offset.0` (new) — `[12]u8` buffer with three f32 values (1.0, 2.0, 3.0) packed LE; reads at offsets 0, 4, 8 and verifies each. The llama2-style "iterate over weights" loop pattern.
- `conformance/native/fail/codec-read-f32-le-bounds.0` (new) — reads at offset 1 into a 4-byte buffer (would read bytes 1..5, past end); registered via `assertBoundsTrap`.
- `conformance/run.mjs` — four new entries (three pass, one fail). No `libm: true` flag — the op compiles to inline SSE; no external math link.

**Verified.**

- `make -C native/zero-c` clean.
- `ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs` → `conformance ok`.
- All three pass fixtures executed under `linux/amd64` via Docker on a Darwin host: each emits its expected `... ok` line. Bounds fixture traps with `SIGILL` (exit 132).
- `node --test docs-site/src/tests/*.test.mjs` → green (docs-registry asserts unchanged).
- Disassembly of `codec-read-f32-le` shows the expected sequence: `mov eax, 0; push rax; mov rax, [rbp-0x10] (len); mov rcx, rax; pop rax; mov rdx, rax; add rdx, 4; cmp rcx, rdx; jae ok; ud2; ok: push rax; mov rax, [rbp-0x18] (ptr); pop rcx; add rax, rcx; movss xmm0, [rax]`.

**Must comply with (carried forward to other backends).**

- Bit-preserving: a `readF32Le` over bytes written by `writeF32Le` (when that lands) is identity, including NaN payload and signed zero.
- Bounds-check on `offset + sizeof(T) <= len` (element-count == byte-count for `Span<u8>`).
- Explicit endianness in the name (`Le` suffix); no host-byte-order assumption.
- AArch64 / Mach-O / COFF must add their own case for `IR_VALUE_BYTE_VIEW_READ_FLOAT_LE` when float codegen reaches them. The contract is shared (bounds check + load from `[ptr + offset]`); the encoding (`LDR Sn, [Xptr, Xoff]` on AArch64; same `MOVSS`/`MOVSD` on Mach-O/COFF) is per-backend.

**Scope.** Landed: ~half a day on x64. Each new backend re-implements the load + bounds-check per its FP unit.

**Dependencies.** None. Independent of F3 (these are byte-view byte reads — typed-span parameter ABI doesn't intersect).

**Out of scope (tracked separately).**

- `writeF32Le` / `writeF64Le` — the inverse direction; not needed for inference, but llama2 quantization or checkpoint export would need it. Same shape, `MOVSS [ptr+offset], xmm0`.
- General bitcast op (`bitcast<f32>(u32)`, `bitcast<u32>(f32)`). Useful for endian flips, future half-precision conversion, sign-bit extraction. Math roadmap v5.
- `readF32Be` / `readF64Be` — big-endian variants. Same lowering with a per-byte swap before/after `MOVSS`/`MOVSD`; not on the v1 path.

### F5 — libm precision pinning *(landed)*

**Problem.** `expf`/`sinf`/`cosf`/`powf` agree across musl, glibc, BSD libm only within a few ULPs. Llama2's reference token stream is implicitly tied to whichever libm produced it.

**Impact.** Bit-for-bit token reproducibility across hosts is impossible without pinning a libm. For inference quality the difference is negligible; for tests with tight tolerances it matters.

**Resolution.** Documentation-only landing. The std.math module did not have a public docs page despite shipping in the Phase 4 / F3 / F4 timeframe — F5 closed that gap and used it as the canonical home for the libm pinning policy. Three surfaces updated:

- `docs-site/articles/modules/math.md` (new) — full std.math API surface (12 functions/constants), libm provider notes, the **musl on `linux-musl-x64`** pin, tolerance ≥ a few ULPs rule, the `sqrtf`/`absf`/`floorf`/inline-helper exceptions that *are* bit-exact, and the per-target decision menu for when other backends land.
- `docs-site/articles/standard-library.md` — std.math added to the runnable modules list.
- `docs-site/articles/target-capabilities.md` — new "Math Runtime" section documenting the `mathRuntime` JSON contract (`provider: libm`, `systemLibraries: [m]`, `capabilityGate: none`), pointing at `modules/math.md` for the full policy.

The original "must comply" item — *document the chosen libm in `examples/llama2/README.md` once it exists* — is satisfied transitively: when `examples/llama2/` lands, its README links to `modules/math.md` rather than restating the policy. This keeps the precision pin in one place and avoids drift between two copies.

**Code touchpoints.**

- `docs-site/articles/modules/math.md` (new) — module doc page.
- `docs-site/articles/standard-library.md` — added std.math line item.
- `docs-site/articles/target-capabilities.md` — new "Math Runtime" section.
- `docs-site/lib/docs.js` — new `module-math` registry entry (slug, path, sourcePath).
- `docs-site/src/tests/docs.test.mjs` — module count bumped 15 → 16, `module-math` added to the canonical-labels assertion list, new `mathModule` term assertion locks in the F5 prose pillars (`musl`, `libm`, `tolerance`, `ULP`, `linux-musl-x64`, `mathRuntime`, plus the API surface).

**Verified.**

- `node --test docs-site/src/tests/*.test.mjs` → green. Module count assertion now passes at 16; `module-math` clears the metadata-label assertion (effects, allocation behavior, target support, error behavior, ownership notes, example); the F5 prose assertions all match.

**Must comply with (carried forward).**

- Tolerance ≥ a few ULPs on any libm-derived value; no exact equality. The conformance fixture tolerance bands already follow this — keep new fixtures consistent.
- musl is the canonical libm for v0.1. Reference outputs (future llama2 token streams, anything else bit-comparable) are generated against musl on `linux-musl-x64`.
- Per-target decision when other backends land: either accept few-ULP drift with wider tolerance, or switch to Strategy B (embedded kernels) for bit-exactness. Documented in `modules/math.md` and `.docs/math/overview.md` "Implementation strategy".
- `examples/llama2/README.md` links to `modules/math.md` for libm policy rather than restating it.

**Scope.** Landed: ~0.5 day docs + registry + test additions. No runtime changes; the libm link path and conformance tolerance bands were already in place from earlier phases.

**Dependencies.** F3 and F4 done (which was the stated prereq for llama2 to exist as a fixture; now that those landed, F5 closes the docs side).

### Priority and sequencing

| Item | Blocks llama2 v0.1? | Est. scope | Suggested order | Status |
|------|---------------------|------------|-----------------|--------|
| F4 — `readF32Le` / bitcast | Yes | 1–3 days | 1st | **landed** |
| F3 — Typed `Span<T>` | Yes | 1–2 weeks (own plan) | 2nd | **landed** |
| F1 — NaN compare fix | No (latent) | 1 day | Before 2nd backend | **landed** |
| F2 — Host test gap doc | No | 0.5–1 day | Anytime | **landed** |
| F5 — libm pinning | No | 0.5 day (docs) | When the docs gap on `std.math` surfaced | **landed** |

All five items have landed. `examples/llama2/` is unblocked from the float/math/codec/docs side on `linux-musl-x64`: typed `Span<f32>` kernels (F3), bit-preserving little-endian `f32`/`f64` reads from a byte view (F4), IEEE-correct NaN compares (F1), visible host-skip warnings + cross-toolchain bootstrap (F2), and the canonical libm precision policy + std.math docs page (F5). F1 landed first because the fix was small and lifted the Phase 5 scoping caveat on `float-nan-compare.0`; AArch64 / Mach-O / COFF must replicate the same IEEE contract when float codegen reaches them. F2 followed because the harness + bootstrap was self-contained and the visibility win pays back any local cross-toolchain churn. F3 landed third because it unblocks the typed-kernel surface that `math-rmsnorm-span.0` / `math-softmax-span.0` need. F4 landed fourth and directly enables `stories15M.bin` decoding once an `examples/llama2/` skeleton lands. F5 landed last because it consolidates the precision contract — `examples/llama2/README.md` will link to `docs-site/articles/modules/math.md` rather than restating it.

## Future migration (out of this plan)

Strategy B (embedded math kernels, `embedded_runtime_sources.inc`-style) remains available if libm linking ever becomes a constraint — e.g. wasm comes back into scope, or a target's libm precision diverges enough to matter. Implementation surface would be 1–2 weeks importing musl/FDLIBM sources. Not on the math roadmap v1–v5.

## Connections

- [`./overview.md`](./overview.md) — scoping, rationale, API surface, open questions.
- [`../llama2/overview.md`](../llama2/overview.md) — primary forcing function; this plan unblocks llama2 v0.1 directly.
