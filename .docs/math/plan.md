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

### Phase 6 — llama2 smoke test

Not a conformance phase; a real-program check.

- A standalone Zero program that does one RMSNorm over a small `Span<f32>` using `sqrtf`, and one softmax slice using `expf`. Numerical output compared against a Python or C reference.
- Lives in `examples/` (not `examples/llama2/` yet — that's a separate project), or as a benchmark fixture under `benchmarks/zero/` per llama2 overview Open Question 5.

Exit: real Zero code uses the new math surface end-to-end; numerical results match reference within libm tolerance.

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
| `examples/` or `benchmarks/zero/` | 6 | RMSNorm + softmax smoke test |

## Risks

1. **Existing IR dispatch may not be type-polymorphic.** The overview claims new IR ops are "likely unneeded" but this needs to be confirmed by reading `ir.c` dispatch sites in Phase 1. If the dispatcher hardcodes integer semantics, the change ripples wider.
2. **NaN comparison semantics divergence across backends.** Phase 2 sets the precedent for the other three backends; if SSE2's PF-on-unordered behavior isn't carefully matched by AArch64/Mach-O/COFF later, fixtures will silently pass only on x64.
3. **Stdlib const globals may not work today.** Phase 4 assumes `const` works for module-level values. If not, fall back to nullary functions and revisit when `const` lands. Open Question 5.
4. **PLT call path for math functions.** Phase 3 assumes external function calls already work via `target.c` system-libraries (proven by `std.http`). If `std.http`'s path is curl-specific in any non-obvious way, libm needs the generalization.
5. **libm tolerance vs llama2 numerical expectations.** libm `expf`/`sinf`/`cosf` results match across musl/glibc/BSD to within a few ULPs but aren't bit-identical. llama2 reference outputs assume some specific libm — pick musl as the canonical reference for the v0.1 fixture.

## Exit criteria (whole plan)

- `pnpm run conformance` green with new fixtures.
- `pnpm run native:test` green.
- `pnpm run docs:test` green (no broken doc references; if math gets a docs page, it passes).
- `zero graph --json` on a package importing `std.math` shows `-lm` in linkPlan.
- llama2 smoke test produces numerically correct RMSNorm + softmax outputs.
- `llama2.zero` v0.1 work can begin without any further blockers from the float/math side.

## Future migration (out of this plan)

Strategy B (embedded math kernels, `embedded_runtime_sources.inc`-style) remains available if libm linking ever becomes a constraint — e.g. wasm comes back into scope, or a target's libm precision diverges enough to matter. Implementation surface would be 1–2 weeks importing musl/FDLIBM sources. Not on the math roadmap v1–v5.

## Connections

- [`./overview.md`](./overview.md) — scoping, rationale, API surface, open questions.
- [`../llama2/overview.md`](../llama2/overview.md) — primary forcing function; this plan unblocks llama2 v0.1 directly.
