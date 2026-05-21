## Status

Runnable today on `linux-musl-x64`:

| API | Return | Notes |
| --- | --- | --- |
| `std.math.sqrtf(x)` | `f32` | Single-precision square root. Lowered to `SQRTSS` (SSE2). Correctly rounded. |
| `std.math.expf(x)` | `f32` | Single-precision `e^x`. libm-backed. |
| `std.math.cosf(x)` | `f32` | Single-precision cosine in radians. libm-backed. |
| `std.math.sinf(x)` | `f32` | Single-precision sine in radians. libm-backed. |
| `std.math.powf(x, y)` | `f32` | Single-precision `x^y`. libm-backed. |
| `std.math.absf(x)` | `f32` | Single-precision absolute value. Bit-pattern clear of the sign bit, no libm call. |
| `std.math.floorf(x)` | `f32` | Single-precision floor (round toward negative infinity). libm-backed. |
| `std.math.isNaNf(x)` | `Bool` | NaN predicate. Compiles to inline bit-pattern check; no libm call. |
| `std.math.piF()` | `f32` | π as f32 (~3.14159274). Inline immediate; no libm call. |
| `std.math.eF()` | `f32` | e as f32 (~2.71828175). Inline immediate; no libm call. |
| `std.math.infinityF()` | `f32` | +∞ as f32. Inline immediate; no libm call. |
| `std.math.nanF()` | `f32` | Quiet NaN as f32. Inline immediate; no libm call. |

Current limits:

- f32 only. No `sqrt`, `exp`, `cos`, `sin`, `pow`, `abs`, `floor` for `f64`.
- linux-musl-x64 only. Other targets report `mathRuntime.status == "unsupported"` in
  `zero graph --json`.
- No `log`, `log2`, `log10`. No `tan`, `asin`, `acos`, `atan`, `atan2`. No `ceil`,
  `round`, `trunc`. No hyperbolic. No `isInf`, `isFinite`. No `min`, `max`, `sign`,
  `cbrt`, `fma`.

Metadata labels:

- effects: none — all helpers are pure scalar functions.
- allocation behavior: no allocation.
- target support: libm-backed helpers (`sqrtf`, `expf`, `cosf`, `sinf`, `powf`,
  `floorf`) require linux-musl-x64; `absf`, `isNaNf`, `piF`, `eF`, `infinityF`,
  `nanF` are inline and target-neutral once float codegen reaches a backend.
- error behavior: infallible. IEEE 754 propagation — NaN inputs propagate,
  `1.0 / 0.0` is `+∞`, `sqrtf` of a negative is NaN.
- ownership notes: returns scalar values; no borrowed storage.
- examples: `conformance/native/pass/math-sqrtf-known-values.0`,
  `conformance/native/pass/math-expf-known-values.0`,
  `conformance/native/pass/math-cosf-sinf-identity.0`,
  `conformance/native/pass/math-powf-known-values.0`,
  `conformance/native/pass/math-absf-floorf.0`,
  `conformance/native/pass/math-isnanf.0`,
  `conformance/native/pass/math-rmsnorm-smoke.0`,
  `conformance/native/pass/math-softmax-smoke.0`,
  `conformance/native/pass/math-rmsnorm-span.0`,
  `conformance/native/pass/math-softmax-span.0`,
  `conformance/native/pass/math-rope-span.0`,
  `conformance/native/pass/math-swiglu-span.0`

## Example

```zero
pub fun main(world: World) -> Void raises {
    let x: f32 = 3.0
    let y: f32 = 4.0
    let h: f32 = std.math.sqrtf(x * x + y * y)
    if h > 4.999 && h < 5.001 {
        check world.out.write("math ok\n")
    }
}
```

RMSNorm slice using `sqrtf`:

```zero
pub fun main(world: World) -> Void raises {
    let mut buffer: [4]f32 = [0.0, 0.0, 0.0, 0.0]
    let input: [4]f32 = [3.0, 4.0, 0.0, 0.0]
    let mut ss: f32 = 0.0
    let mut i: usize = 0
    while i < 4 {
        ss = ss + input[i] * input[i]
        i = i + 1
    }
    let inv: f32 = 1.0 / std.math.sqrtf(ss / 4.0 + 0.00001)
    let mut j: usize = 0
    while j < 4 {
        buffer[j] = inv * input[j]
        j = j + 1
    }
    if buffer[0] > 1.199 && buffer[0] < 1.201 {
        check world.out.write("rmsnorm ok\n")
    }
}
```

## libm Provider

libm is linked through the standard `target.c` system-libraries pipeline, the
same audit pattern `std.http` uses for libcurl. The link plan is surfaced in
`zero graph --json` for packages that import a libm-backed function:

- `mathRuntime.provider`: `"libm"`
- `mathRuntime.providerLink`: `"system-library"`
- `mathRuntime.systemLibraries`: `["m"]`
- `mathRuntime.capabilityGate`: `"none"` — math is always-on; there is no `math`
  capability gate.

Importing only the inline helpers (`absf`, `isNaNf`, `piF`, `eF`, `infinityF`,
`nanF`) does not trigger libm linking — the runtime emits the SSE2 sequence or
constant immediate inline.

`zero graph --json` and `zero targets --json` both include the `mathRuntime`
object. On targets without libm support, `mathRuntime.status` is `"unsupported"`
and the `reason` field explains the gap (currently:
`"math runtime is linux-only in this phase"` for non-Linux ELF64 targets).

## Precision and Reproducibility

**Canonical libm for v0.1: musl on `linux-musl-x64`.** Reference numerical
outputs — conformance tolerance bands, future `examples/llama2/` token streams,
anything else that depends on bit-level reproducibility — are derived against
musl libm. Other libms (glibc, BSD libm, Windows ucrt) agree within a few ULPs
on `expf` / `sinf` / `cosf` / `powf` but are not bit-identical.

Practical rules for code that depends on transcendental results:

- Compare with tolerance bands, never `==`. Tolerance ≥ a few ULPs on any
  libm-derived value. The existing conformance fixtures
  (`math-expf-known-values.0`, `math-cosf-sinf-identity.0`,
  `math-rmsnorm-smoke.0`, `math-softmax-smoke.0`, `math-rmsnorm-span.0`,
  `math-softmax-span.0`) all use tolerance bands of `±0.001` to `±0.01` on
  transcendental outputs.
- Bit-exact reproducibility across hosts is not guaranteed and is not a
  stability commitment. It happens to hold today because `linux-musl-x64` is
  the only supported target.
- `sqrtf`, `absf`, and `floorf` are the exceptions. `sqrtf` lowers to `SQRTSS`
  and is correctly rounded by the CPU. `absf` is a bit-pattern clear of the
  sign bit. `floorf` is correctly rounded. Compare those with `==` if needed.
- `isNaNf`, `piF`, `eF`, `infinityF`, and `nanF` are inline and have no libm
  dependency at all. They are bit-identical across every host.

When other backends land (darwin-arm64, darwin-x64, win32-x64, linux-aarch64),
the per-target decision is:

- Accept few-ULP drift with wider tolerance bands. Default while libm remains
  the implementation strategy.
- Switch to Strategy B (embedded math kernels, musl/FDLIBM sources) for
  bit-exactness. Documented in `.docs/math/overview.md` "Implementation
  strategy"; not on the v1 path.

`examples/llama2/` will pin musl libm as its reference libm when it lands.
Token-stream comparisons against the Karpathy C reference assume the reference
was generated against musl libm using the same model file.

## Design Notes

`std.math` follows C convention: `f` suffix for f32 forms (`sqrtf`, not
`sqrt`). f64 versions are not in the v1 surface — adding them is a per-function
copy with `sd` (SSE double) opcodes and a `mathRuntime` link path that is
already in place. Tracked in `.docs/math/overview.md`.

IEEE 754 strict semantics by default. No flush-to-zero, no quiet-NaN
suppression. Float compares against NaN return false except `!=`, which returns
true. See `conformance/native/pass/float-nan-compare.0` for the codegen
contract.

Pi as f32 rounds to ~3.14159274, a few ULPs away from the true value, so
`cosf(piF())` is not exactly `-1.0` and `sinf(piF())` is not exactly `0.0`. The
cos² + sin² = 1 identity holds within a small tolerance band, which is what
`math-cosf-sinf-identity.0` checks.

Constants are nullary functions (`piF()`, `eF()`, `infinityF()`, `nanF()`)
rather than module-level `const` values, because module-level `const` for f32
is not yet in the language. The function form is inlined at the call site, so
the runtime cost is identical to a constant immediate.

`isNaNf` is intentionally not libm-backed. The bit-pattern check (exponent
all-ones, mantissa non-zero) is two integer instructions and avoids the libm
link entirely. Programs that only need NaN detection compile and run on every
target with float codegen, not just `linux-musl-x64`.
