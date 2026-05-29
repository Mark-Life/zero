## Status

Runnable today on `darwin-arm64`, `darwin-x64`, `linux-musl-x64`, and `linux-musl-arm64`:

| API | Return | Notes |
| --- | --- | --- |
| `std.math.sqrtf(x)` | `f32` | Single-precision square root. libm-backed. |
| `std.math.expf(x)` | `f32` | Single-precision `e^x`. libm-backed. |
| `std.math.cosf(x)` | `f32` | Single-precision cosine in radians. libm-backed. |
| `std.math.sinf(x)` | `f32` | Single-precision sine in radians. libm-backed. |
| `std.math.powf(x, y)` | `f32` | Single-precision `x^y`. libm-backed. |
| `std.math.absf(x)` | `f32` | Single-precision absolute value. Lowered to libm `fabsf`. |
| `std.math.floorf(x)` | `f32` | Single-precision floor (round toward negative infinity). libm-backed. |
| `std.math.isNaNf(x)` | `Bool` | NaN predicate. Compiles to an inline self-compare; no libm call. |
| `std.math.piF()` | `f32` | π as f32 (~3.14159274). Inline IEEE-bit literal; no libm call. |
| `std.math.eF()` | `f32` | e as f32 (~2.71828175). Inline IEEE-bit literal; no libm call. |
| `std.math.infinityF()` | `f32` | +∞ as f32. Inline IEEE-bit literal; no libm call. |
| `std.math.nanF()` | `f32` | Quiet NaN as f32. Inline IEEE-bit literal; no libm call. |

Current limits:

- f32 only. No `sqrt`/`exp`/`cos`/`sin`/`pow`/`abs`/`floor` for `f64`.
- No `log`, `log2`, `log10`. No `tan`, `asin`, `acos`, `atan`, `atan2`. No `ceil`,
  `round`, `trunc`. No hyperbolic. No `isInf`, `isFinite`. No `min`, `max`, `sign`,
  `cbrt`, `fma`.
- Windows (COFF) builds are gated — float lowering on the Windows backends is
  backlog. Calling a libm helper on `win32-x64.exe` reports `BLD004`.

Metadata labels:

- effects: none — all helpers are pure scalar functions.
- allocation behavior: no allocation.
- target support: libm-backed helpers (`sqrtf`, `expf`, `cosf`, `sinf`, `powf`,
  `absf`, `floorf`) require a target the direct backend can route through the
  runtime obj+link path (currently `darwin-arm64`, `darwin-x64`, `linux-musl-x64`,
  and `linux-musl-arm64`). `isNaNf`, `piF`, `eF`, `infinityF`, `nanF` are inline
  and target-neutral on any float-capable backend.
- error behavior: infallible. IEEE 754 propagation — NaN inputs propagate,
  `1.0 / 0.0` is `+∞`, `sqrtf` of a negative is NaN.
- ownership notes: returns scalar values; no borrowed storage.

## Example

```zero
pub fn main Void world World !
  let x f32 3.0
  let y f32 4.0
  let h f32 std.math.sqrtf (+ (* x x) (* y y))
  if && (> h 4.999) (< h 5.001)
    check world.out.write "math ok\n"
```

RMSNorm slice using `sqrtf`:

```zero
pub fn main Void world World !
  mut buffer [4]f32 [0.0, 0.0, 0.0, 0.0]
  let input [4]f32 [3.0, 4.0, 0.0, 0.0]
  mut ss f32 0.0
  mut i usize 0
  while < i 4
    set ss + ss (* input[i] input[i])
    set i + i 1
  let inv f32 / 1.0 (std.math.sqrtf (+ (/ ss 4.0) 0.00001))
  mut j usize 0
  while < j 4
    set buffer[j] (* inv input[j])
    set j + j 1
  if && (> buffer[0] 1.199) (< buffer[0] 1.201)
    check world.out.write "rmsnorm ok\n"
```

## libm Provider

libm is linked through the zero runtime obj+link path. A package that imports any
libm-backed helper (`sqrtf`, `expf`, `cosf`, `sinf`, `powf`, `absf`, `floorf`)
forces the direct backend to emit an object and link it against the host's libm:

- On Linux (`linux-musl-x64`, `linux-musl-arm64`), the link plan appends `-lm` and
  resolves to zig's bundled musl libm.
- On macOS (`darwin-arm64`, `darwin-x64`), libm is part of `libSystem` and binds
  automatically through the same runtime object — no `-lm` needed.

Importing only the inline helpers (`isNaNf`, `piF`, `eF`, `infinityF`, `nanF`)
does not trigger the obj+link path — the backend emits the self-compare or
constant immediate inline and the build stays on the pure direct-exe route.

## Precision and Reproducibility

**libm is per-platform.** Every libm rounds `expf` / `sinf` / `cosf` / `powf`
slightly differently, so numerical outputs that go through libm are bit-identical
**only when the reference is built against the same libm**. The
`examples/llama2/validate.sh` parity gate enforces exactly this by building its
`llama2.c` reference with `zig cc -target <same triple>` so the Zero exe and the
reference share one libm per platform.

Practical rules for code that depends on transcendental results:

- Compare with tolerance bands, never `==` on libm outputs. The bands in the
  conformance fixtures (`math-expf-known-values.0`, `math-cosf-sinf-identity.0`,
  `math-rmsnorm-smoke.0`, `math-softmax-smoke.0`, `math-rmsnorm-span.0`,
  `math-softmax-span.0`) sit at `±0.001` to `±0.01` on transcendental outputs.
- `isNaNf`, `piF`, `eF`, `infinityF`, and `nanF` are the exceptions — they have
  no libm dependency, so they are bit-identical across every host. `==` is safe
  on those.
- `sqrtf` is libm-backed for consistency, but every libm rounds `sqrtf`
  correctly (it's an IEEE 754 requirement), so `sqrtf` outputs agree bit-for-bit
  across libms on the same input — `==` is safe in practice, just not promised
  by this module.
- Bit-exact reproducibility across platforms is not a stability commitment.
  Within a single platform with a fixed libm it does hold (the llama2 parity
  matrix is the working proof).

The Zero backend emits separate `fmul + fadd` for `a*b + c` and never a fused
multiply-add (`fmadd`). `zig cc -O3` on AArch64 would otherwise contract a
multiply-add into a single rounded `fmadd`, so the parity gate passes
`-ffp-contract=off` to keep the reference's FP behavior aligned.

## Design Notes

`std.math` follows C convention: `f` suffix for f32 forms (`sqrtf`, not `sqrt`).
f64 versions are not in the surface — adding them is a per-function copy with
double-precision opcodes (`SQRTSD`, `fsqrt d`, …) and an `fabsf` → `fabs` swap
in the runtime symbol list.

IEEE 754 strict semantics by default. No flush-to-zero, no quiet-NaN
suppression. Float compares against NaN return `false` except `!=`, which
returns `true`. See `conformance/native/pass/float-nan-compare.0` for the
codegen contract.

Pi as f32 rounds to ~3.14159274, a few ULPs away from the true value, so
`cosf(piF())` is not exactly `-1.0` and `sinf(piF())` is not exactly `0.0`. The
`cos² + sin² = 1` identity holds within a small tolerance band, which is what
`math-cosf-sinf-identity.0` checks.

Constants are nullary functions (`piF()`, `eF()`, `infinityF()`, `nanF()`)
rather than module-level `const` values, because module-level `const` for `f32`
is not yet in the language. The function form is inlined at the call site, so
the runtime cost is identical to a constant immediate.

`isNaNf` is intentionally not libm-backed. A self-compare detects NaN in one
instruction (`fcmp x, x` + `cset vs` on AArch64; `UCOMISS` + `SETP` on x64),
which avoids the runtime obj+link path entirely. Programs that only need NaN
detection compile on every float-capable backend, not just the libm-routable ones.
