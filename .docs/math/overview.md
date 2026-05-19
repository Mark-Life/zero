# Floats + std.math — Overview

What it takes to unblock numerical programs in Zero. This doc covers two layers that have to land in order: **(0) float codegen** and **(1) `std.math` stdlib coverage**. Neither exists today.

## Mission

Make `f32` and `f64` first-class in Zero. Today the checker accepts them but no backend can lower them — a fact that surfaced the moment we tried to plan `llama2.zero`. Every numerical program (inference, simulation, graphics, audio, statistics) is blocked on this.

Goal of v1 of this work: a Zero program can do `let y: f32 = sqrt(x * x + 1.0)` and produce a binary that runs correctly on linux-x64, linux-arm64, darwin (x64+arm64), and windows-x64.

## State Today (Verified)

What's in:

- **Type checker** recognizes `f32` and `f64`. `checker.c:3515` defines `is_float_type()`. Float literals lex and typecheck. Casts to/from int compile through the checker.
- **Documentation** lists `f32` and `f64` under "Runnable" primitives (`docs-site/articles/primitives.md:9`), and language reference notes that `1.0` defaults to `f64` (`language-reference.md:314-322`).

What's missing (codegen layer):

- **IR has no float types.** `include/zero.h:372-388` declares `IR_TYPE_VOID`, `IR_TYPE_BOOL`, `IR_TYPE_U8`, `IR_TYPE_U16`, `IR_TYPE_USIZE`, `IR_TYPE_I32`, `IR_TYPE_U32`, `IR_TYPE_I64`, `IR_TYPE_U64`. No `IR_TYPE_F32`. No `IR_TYPE_F64`.
- **ELF x86-64 backend** (`emit_elf64.c:79-80`): `elf_type_is_supported_scalar()` excludes floats. Arithmetic emits integer instructions only.
- **ELF AArch64**, **Mach-O**, **COFF** backends: same shape. None emit SSE/AVX/NEON float instructions.
- **Float comparison**: rejected by every backend.
- **Float casts**: typecheck but never reach codegen.

What's missing (stdlib layer):

- **No `std.math` module.** The 15 stdlib modules listed in `docs-site/articles/standard-library.md` do not include math. No `sqrt`, `exp`, `log`, `sin`, `cos`, `pow`, `tanh`, `floor`, `ceil`, `abs`, `min`, `max`, `isNaN`, `isInf`.

Net effect: `let x: f32 = 1.0; let y = x * 2.0` passes `zero check` and fails at codegen with an "unsupported type" diagnostic.

## Scope

**In (this work):**

- Float arithmetic (`+ - * /`) for `f32` and `f64`.
- Float comparison (`< <= > >= == !=`).
- Float ↔ integer casts (`as i32`, `as f32`, etc.).
- Float ↔ float casts (`f32 as f64`, `f64 as f32`).
- Float literals (`1.0`, `1.5_f32`, `1e-3`).
- All four active backends: ELF x86-64, ELF AArch64, Mach-O (x64 + arm64), COFF x64.
- `std.math` with the function set needed by `llama2.zero` and common numerical work (see API below).
- Conformance fixtures for every op, every backend.

**Out (defer to a later doc):**

- `f16` / `bf16` half-precision types. Real conversation only after this work lands. Tracked as Open Question 1 of `.docs/llama2/overview.md`.
- SIMD types (`f32x4`, `f64x2`). Would help matmul significantly but is a separate language feature.
- Vectorization at the codegen level. Scalar correctness first.
- Quad precision (`f128`).
- Complex numbers (`c64`, `c128`). Not on the agent-first critical path.
- WASM target. Removed in PR #109; not in scope for this work.
- GPU offload. Far future.
- Decimal floating point. Different use case (finance/billing), separate library.

## Layer 0 — Float Codegen

The prerequisite. Without this, `std.math` has nothing to compile against.

### IR additions

Add to `include/zero.h:372-388`:

```c
IR_TYPE_F32,
IR_TYPE_F64,
```

Update `ir.c` dispatch tables. New IR ops likely unneeded — existing `IR_VALUE_ADD`, `IR_VALUE_SUB`, `IR_VALUE_MUL`, `IR_VALUE_DIV`, `IR_VALUE_COMPARE`, `IR_VALUE_CAST` should dispatch on operand type and let each backend pick instructions. Confirm by reading the dispatch sites.

Update `elf_type_is_supported_scalar()` (and equivalents in other backends) to accept the new IR types.

### Per-backend plan

**ELF x86-64** (`emit_elf64.c`):

- SSE2 baseline. Universal on x86-64 since 2003 — no feature detection.
- `f32`: `movss` (load/store), `addss`, `subss`, `mulss`, `divss`, `ucomiss` (compare), `cvtsi2ss` (int→f32), `cvttss2si` (f32→int trunc), `cvtss2sd` (f32→f64), `sqrtss`.
- `f64`: same with `sd` suffix (`movsd`, `addsd`, etc.), `cvtsd2ss` (f64→f32 narrow).
- Register file: use XMM0–XMM7 for FP args/temps (System V ABI). Existing GPR allocation unaffected.
- Comparison: `ucomiss`/`ucomisd` set EFLAGS; branch on `jae`/`jne`/etc. NaN handling needs care (`ucomis`* sets PF on unordered).

**ELF AArch64** (`emit_elf_aarch64.c`):

- ARMv8 baseline includes scalar FP. No feature detection needed.
- `f32`: `fadd s0, s1, s2`, `fsub`, `fmul`, `fdiv`, `fcmp`, `fcvtzs` (f32→int), `scvtf` (int→f32), `fsqrt`.
- `f64`: `fadd d0, d1, d2`, etc. `fcvt s0, d0` (narrow), `fcvt d0, s0` (widen).
- Register file: V0–V31 (use S/D views). AAPCS64 passes FP args in V0–V7.
- Comparison: `fcmp` then conditional branch (`b.ge`, `b.ne`, etc.).

**Mach-O** (`emit_macho64.c`):

- darwin-arm64: identical to ELF AArch64 instruction-wise; different file format only.
- darwin-x64: identical to ELF x86-64 instruction-wise.
- Reuse instruction emitters across ELF and Mach-O where possible. Split only where the ABI differs (e.g., variadic float args on darwin pass through XMM registers too, but the count tracking differs).

**COFF x64** (`emit_coff.c`):

- Windows x64 ABI: first four FP args in XMM0–XMM3, integer and FP share the slot index (`fn(int, float)` uses RCX + XMM1, not RCX + XMM0). Different from System V — must handle.
- Otherwise same SSE2 instructions as ELF x86-64.

### NaN, Inf, denormals

Decide upfront:

- **IEEE 754 strict semantics** by default. `0.0 / 0.0 == NaN`, `1.0 / 0.0 == Inf`, no silent quiet-NaN suppression.
- **No flush-to-zero by default.** Numerical algorithms break in subtle ways with denormals flushed.
- Document that `==` on NaN is always false, matching IEEE. Encourage `isNaN()` from `std.math` once it exists.
- Conformance fixtures must cover the corner cases — they're the most common source of subtle FP bugs across backends.

### Conformance fixtures (Layer 0)

Per backend × per op:

- `conformance/native/pass/float-arith-{add,sub,mul,div}-{f32,f64}.0`
- `conformance/native/pass/float-compare-{eq,lt,le,gt,ge,ne}-{f32,f64}.0`
- `conformance/native/pass/float-cast-{f32-to-f64,f64-to-f32,i32-to-f32,f32-to-i32,...}.0`
- `conformance/native/pass/float-nan-compare.0` — NaN is never equal, including to itself.
- `conformance/native/pass/float-inf-arith.0` — `1.0 / 0.0 == Inf`, `-1.0 / 0.0 == -Inf`.
- `conformance/native/pass/float-denormal.0` — denormals preserved (no FTZ).

## Layer 1 — `std.math`

Once Layer 0 lands. Function set sized to "what numerical Zero programs actually need," not the entire C `<math.h>` surface.

### API (v1)

Both `f32` and `f64` versions. `f` suffix for the 32-bit form (matches C convention).

```
// Basic
std.math.absf(x: f32) -> f32                      std.math.abs(x: f64) -> f64
std.math.minf(a: f32, b: f32) -> f32              std.math.min(a: f64, b: f64) -> f64
std.math.maxf(a: f32, b: f32) -> f32              std.math.max(a: f64, b: f64) -> f64
std.math.signf(x: f32) -> f32                     std.math.sign(x: f64) -> f64

// Rounding
std.math.floorf(x: f32) -> f32                    std.math.floor(x: f64) -> f64
std.math.ceilf(x: f32) -> f32                     std.math.ceil(x: f64) -> f64
std.math.roundf(x: f32) -> f32                    std.math.round(x: f64) -> f64
std.math.truncf(x: f32) -> f32                    std.math.trunc(x: f64) -> f64

// Roots and powers
std.math.sqrtf(x: f32) -> f32                     std.math.sqrt(x: f64) -> f64
std.math.cbrtf(x: f32) -> f32                     std.math.cbrt(x: f64) -> f64
std.math.powf(x: f32, y: f32) -> f32              std.math.pow(x: f64, y: f64) -> f64

// Exp / log
std.math.expf(x: f32) -> f32                      std.math.exp(x: f64) -> f64
std.math.logf(x: f32) -> f32                      std.math.log(x: f64) -> f64
std.math.log2f(x: f32) -> f32                     std.math.log2(x: f64) -> f64
std.math.log10f(x: f32) -> f32                    std.math.log10(x: f64) -> f64

// Trig
std.math.sinf(x: f32) -> f32                      std.math.sin(x: f64) -> f64
std.math.cosf(x: f32) -> f32                      std.math.cos(x: f64) -> f64
std.math.tanf(x: f32) -> f32                      std.math.tan(x: f64) -> f64
std.math.asinf(x: f32) -> f32                     std.math.asin(x: f64) -> f64
std.math.acosf(x: f32) -> f32                     std.math.acos(x: f64) -> f64
std.math.atanf(x: f32) -> f32                     std.math.atan(x: f64) -> f64
std.math.atan2f(y: f32, x: f32) -> f32            std.math.atan2(y: f64, x: f64) -> f64

// Hyperbolic
std.math.tanhf(x: f32) -> f32                     std.math.tanh(x: f64) -> f64
std.math.sinhf(x: f32) -> f32                     std.math.sinh(x: f64) -> f64
std.math.coshf(x: f32) -> f32                     std.math.cosh(x: f64) -> f64

// Predicates
std.math.isNaN(x: f64) -> Bool                    std.math.isNaNf(x: f32) -> Bool
std.math.isInf(x: f64) -> Bool                    std.math.isInff(x: f32) -> Bool
std.math.isFinite(x: f64) -> Bool                 std.math.isFinitef(x: f32) -> Bool

// Constants
std.math.PI: f64                                  std.math.PI_F: f32
std.math.E: f64                                   std.math.E_F: f32
std.math.INFINITY: f64                            std.math.INFINITY_F: f32
std.math.NAN: f64                                 std.math.NAN_F: f32
```

Subset needed for llama2.zero v0.1: `expf` (softmax), `sqrtf` (RMSNorm), `cosf`/`sinf` (RoPE), `powf` (some tokenizers). That's 5 functions. The rest is for the broader "useful stdlib" pitch.

### Implementation strategy

Three viable approaches. They're not mutually exclusive.

**A. Link to libm.**

- musl libm on linux, BSD libm on darwin, ucrt on windows.
- Fastest path: each stdlib function compiles to a single `call expf@PLT` (or equivalent).
- Precedent exists: `std.http` already links `libcurl` as a system library — registered via `target.c`'s `systemLibraries`, visible in `zero graph --json` linkPlan, gated by the `net` capability. libm follows the same audit pattern with a smaller surface (universal across targets, no host-discovery story, no TLS audit).
- Note: emitted binaries today use *direct syscalls* (not libc) for I/O — see `emit_elf64.c` (`syscall` 0x0f 0x05 on linux-x64) and `emit_macho64.c` (`svc #0x80` on darwin-arm64). libm would be the *second* externally-linked system library after libcurl, not a regression from a no-link baseline.
- Negatives: not pure-Zero, ties stdlib correctness to libc.
- Effort: days.

**B. Author intrinsics-as-builtins (C, embedded).**

- Like `std.json` is today: state machines in `embedded_runtime_sources.inc`, registered in checker.c and ir.c.
- Use existing fast implementations (musl's `expf.c`, FDLIBM's `sqrtf`, etc.) — public domain or MIT licensed.
- Self-contained, no libm dependency.
- Negatives: bigger maintenance surface, must match libm precision (matters less for inference than for scientific code).
- Effort: 1-2 weeks if importing existing implementations; months if authoring from scratch.

**C. Author in Zero, eventually.**

- Math kernels in `.0` files once a stdlib-in-Zero hosting path exists.
- Self-contained, agent-readable.
- Negatives: today there is no such hosting path — would need to invent it (large infra task). Performance gap until codegen matures.
- Effort: months.

**Recommendation for v1 of this work: A (link libm).** Get unblocked fast, prove the API surface is right, swap implementation strategy later. The "no external deps" pitch is mostly about *runtime* deps for shipped binaries — libm is a compile-time link that produces a self-contained binary on musl, so even the deps story holds.

### Linking architecture

libm goes through the existing `target.c` system-libraries plumbing — same mechanism as `std.http`/libcurl, not a parallel path. Concretely:

- Registered in `target.c`'s `systemLibraries` per target (`m` on linux/darwin, equivalent on windows).
- The link plan surfaces in `zero graph --json` so callers can audit math-using packages alongside HTTP-using ones.
- Strategy A is the v1 implementation; Strategy B (embedded math kernels) can swap in later without changing the link-audit surface, the same way `std.http` could in principle replace libcurl without changing its capability story.

One pipeline, not two. If math eventually diverges from this pattern (e.g. an embedded kernel ships and the system-library entry disappears), the divergence is visible in `target.c` rather than hidden in a special-case code path.

### Conformance fixtures (Layer 1)

For each function:

- Identity / known-value test: `sqrt(4.0) == 2.0`, `sin(0.0) == 0.0`, `exp(0.0) == 1.0`.
- Edge cases: `sqrt(-1.0) == NaN`, `log(0.0) == -Inf`, `pow(0.0, 0.0) == 1.0`.
- Round-trip tests where applicable: `log(exp(x)) ≈ x` within tolerance.
- Tolerance: 1 ULP for f64 basics, looser for transcendentals (`exp`, `sin`, etc. — match libm's documented error bounds).

## Roadmap


| Version | What                                                                                                                | Unblocks                                |
| ------- | ------------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| **v1**  | `IR_TYPE_F32`/`F64` + ELF x86-64 codegen + libm-linked `std.math` minimum (`sqrtf`, `expf`, `cosf`, `sinf`, `powf`) | `llama2.zero` v0.1 on linux-x64 only    |
| v2      | ELF AArch64, Mach-O (x64+arm64), COFF x64 codegen                                                                   | All platforms                           |
| v3      | Full `std.math` API (logs, trig, hyperbolic, predicates)                                                            | Broader numerical work                  |
| v4      | NaN/Inf/denormal conformance test sweep + docs                                                                      | Production-ready confidence             |
| v5      | `f16`/`bf16` primitive types (separate doc)                                                                         | `llama2.zero` v0.2 with real Llama 2 7B |


## Open Questions

1. **libm capability gating.** Linking libm itself is resolved — follow the `std.http`/libcurl audit pattern (see "Linking architecture" under Layer 1). Open question is *how* math surfaces in the capability system. Options: (a) always-on like arithmetic, since math is universally available across the four backend targets; (b) its own capability (`math`?) mirroring `net`; (c) bundled with a future numeric capability. *Recommendation: always-on for v1 — math is universal in a way libcurl isn't (no host discovery, no TLS audit, no per-target gating beyond "linker can find -lm"). Revisit if cross-target divergence emerges.*
2. **f64 default for literals — keep it?** `language-reference.md:314-322` says `1.0` is f64 by default. Common but bigger than necessary for inference (which is f32-dominant). Stay with f64-default for consistency with C/Rust, or change to f32-default to match the agent-numerical-program profile? *Recommendation: keep f64 default, require `_f32` suffix or context inference for f32 literals — matches every other language.*
3. `**std.math.sqrt(x)` overload resolution.** Two functions named `sqrt` (one f32, one f64) or single function with type-driven dispatch? Today's `std_call_return_type` infrastructure (`checker.c:368`) takes the callee name only. Could be extended for type-driven overload, or we use the C-style `sqrt` / `sqrtf` naming. *Recommendation: C-style naming for v1. Cleaner with the existing checker. Revisit if Zero adds overload resolution as a language feature.*
4. **Float-int comparison.** Is `1.0 == 1` allowed? C says yes (implicit promotion). Rust requires explicit `as`. Zero so far has been strict about implicit promotion (no automatic int widening). *Recommendation: require explicit cast — match Zero's stated regularity principle.*
5. **Constants as functions or values.** `std.math.PI: f64` (const) vs `std.math.pi() -> f64` (function). *Recommendation: const. Cleaner at call sites. Confirm `const` works for global stdlib values today.*
6. **FMA (fused multiply-add).** `x * y + z` as a single rounded op. Modern CPUs have it; helps numerical accuracy. Worth exposing as `std.math.fma(x, y, z)` or inferring from `x * y + z` patterns? *Recommendation: expose explicitly via `fma`, don't auto-infer. Auto-infer is a codegen optimization for later.*
7. **Strict float ordering in `<`/`>` comparison.** When one side is NaN, every IEEE comparison returns false. Users expect `!(a < b)` to mean `a >= b`. With NaN, both are false. Document loudly or surface as a "NaN-aware compare" helper. *Recommendation: document; add `std.math.compare(a, b) -> Ordering` if patterns demand it later.*

## Non-Goals

- **Match every libm function bit-for-bit across backends.** Floating-point determinism across CPUs is a rabbit hole. Document tolerances per function; accept platform differences.
- **Replace libm with pure-Zero implementations on day one.** Strategy A first; migration is a later, separate effort.
- **Cover stats / linear algebra in `std.math`.** `mean`, `variance`, `dot`, `norm` belong in `std.stat` or `std.linalg` — separate modules, separate design.
- **Format/parse floats in `std.math`.** Float ↔ string is `std.parse` + `std.fmt` territory.
- **Random-number generation tied to floats.** `std.rand` covers that surface.

## Connections to Other `.docs`

- `**.docs/llama2/overview.md`** — primary forcing function for this work. v1 of math unblocks `llama2.zero` v0.1 on linux-x64. The llama2 overview's "Prerequisites" should reference this doc.
- **Future `.docs/half-precision/overview.md`** (if/when written) — covers `f16`/`bf16` after this work lands. Open Question 1 of llama2 overview migrates there.
- **Future `.docs/simd/overview.md`** (if/when written) — `f32x4` / `f64x2` types. Different problem, different design. Useful for matmul performance but orthogonal to scalar correctness.

