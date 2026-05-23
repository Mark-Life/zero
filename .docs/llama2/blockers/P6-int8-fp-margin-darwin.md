# P6 finding: int8 token-for-token parity is FP-margin-limited on darwin-arm64

**Status:** Characterized, not a Zero bug. **The f32 matrix is token-for-token on
macOS arm64; the int8 (`runq.c` v2) matrix is not, and cannot be made so by any
reference-build flag** — its logits sit closer to the sampling decision boundary
than the smallest legal sub-ULP difference between two independent FP
implementations. The native `validate.sh` branch therefore **gates f32 and runs
int8 informationally** (a divergence is reported, the run still passes). int8
stays **gated** on the linux-musl-x64 / Docker branch, where it is bit-exact.

Date: 2026-05-23. Host: Apple Silicon (darwin-arm64), zig 0.16.0.

## What passes vs. diverges (stories15M, native branch)

Both the Zero exe (`--backend zero-macho64`) and the C reference
(`zig cc -target aarch64-macos -O3 -ffp-contract=off`) link the **same** macOS
libSystem libm, so libm is identical on this host — unlike the Linux case below,
the macOS divergence is **not** a libm mismatch.

- **f32 — token-for-token PASS (11/11):** temp 0 ×5, temp>0 multinomial ×3,
  top-p ×3. Wide logit margins absorb every sub-ULP FP-order difference.
- **int8 — divergent (≈4/8 match):** a few argmax tokens and most temp>0
  multinomial tokens flip after a long identical prefix, then the autoregressive
  feedback decorrelates the streams.

## Root cause: FMA contraction + tight int8 margins, three legal roundings

1. **The Zero arm64 backend emits a separate `fmul` then `fadd`** (two roundings)
   for `a*b+c` — it has no FMA contraction.
2. **`zig cc -target aarch64-macos -O3` contracts `a*b+c` into a single `fmadd`**
   (one rounding) by default (the C `FP_CONTRACT` pragma defaults ON, and arm64
   has scalar FMA). This is a sub-ULP-different result per multiply-accumulate in
   `matmulQ` and the f32 reductions.
3. Building the reference with **`-ffp-contract=off`** removes the FMA and brings
   the reference much closer to Zero (int8 2/8 → 4/8, f32 stays 11/11), so
   `validate.sh` uses it. But it does **not** make them bit-identical: Zero,
   `runq.c`+FMA, and `runq.c`−FMA are **three distinct legal roundings**, and on
   this checkpoint they disagree on the very tightest int8 tokens. Evidence:
   - Different prompts match *different* reference builds — e.g. `t0 "Once upon a
     time"` matches −FMA but not +FMA; `t0 "Lily and Tom…"` matches +FMA but not
     −FMA. No single ref build matches Zero on all int8 prompts.
   - **Two C builds of `runq.c` disagree with each other** on int8 temp>0
     (`-O3 -ffp-contract=off` vs `-O0 -ffp-contract=off` differ at the same
     token). Token-for-token across independent implementations is therefore not
     well-defined for this checkpoint's int8 margins.
4. **Why int8 and not f32:** the int8 path quantizes weights to 8 bits, so its
   logits land closer together; the gap between the top two is sometimes below the
   FP-order noise floor. The f32 path's margins are wider and never flip. (Same
   shared `softmax`/`expf`/sampler — the f32 matrix passing proves those are
   bit-exact; the int8 divergence is upstream in `matmulQ`/dequant accumulation.)
5. **Why temp>0 worse than temp 0:** argmax absorbs a sub-ULP logit perturbation
   unless it straddles the top-2 boundary; multinomial / top-p turn it into a
   different *drawn* token via the cumulative-distribution boundary, which is far
   more sensitive.

## Why this is *not* a Zero bug

- The Zero exe is **deterministic** (identical output across runs).
- Zero's int8 kernels are **token-for-token vs `runq.c` on linux-musl-x64**
  (committed; the Docker branch gates it) — on x86-64 SSE2 neither side
  auto-contracts, so the kernels are bit-identical there and even temp>0 matches.
- Zero's **f32** path is **bit-exact on this same macOS host** (11/11), so the
  softmax / PRNG / sampler / f32 kernels are all correct natively.
- The macOS divergence is purely the arm64 FMA-contraction difference amplified by
  int8's tight margins — a spec-legal FP-association choice, the same *class* of
  per-platform numeric tolerance the plan anticipated (cross-platform.md Risk 2 /
  Open Q4: "expect rare divergence on tight-margin tokens"; parity is *per
  platform*, not byte-identical across them).

## Relationship to the (resolved) Linux case

`../quant-temp-parity-divergence.md` documents the Linux int8 temp>0 divergence,
which was a **libm** mismatch (Alpine musl vs zig musl `expf`, 1 ULP) and was
fixed by building the reference with the matching `zig cc` toolchain — after which
the full Linux int8 matrix matched token-for-token. macOS is the *next layer* of
the same tight-margin story: libm is already identical (one libSystem), so what
remains is the **FMA-contraction** difference in the kernels themselves, which has
no "matching toolchain" cure because the Zero backend simply does not emit FMA and
the reference's roundings are build-dependent.

## Options if int8 token-for-token on darwin is ever required

- **Emit FMA (`fmadd`) in the Zero arm64 backend** for `a*b+c` (matmul inner
  loop, reductions) and build the reference *with* contraction. This would make
  Zero match the contracting reference — but it is a numerics change to the
  backend (affects every arm64 FP program, and the f32 linux/elf parity model),
  out of scope for P6 and not obviously desirable (FMA changes results vs the
  x86/elf path too). It would also need to match `runq.c`'s exact contraction
  *pattern*, which the C optimizer chooses per build.
- **Accept per-platform parity** (the adopted decision): f32 gated token-for-token
  on macOS; int8 informational on macOS, gated on Linux. This is honest, matches
  the plan's per-platform bar, and keeps the f32 PASS — the headline claim
  (llama2 runs natively on Apple Silicon, parity-validated) — fully clean.

## How to reproduce

```sh
make -C native/zero-c
bash examples/llama2/validate.sh        # darwin-arm64 host -> native branch
# f32 -> PASS (token-for-token); int8 -> a few "DIFF … informational" lines + NOTE.
```

To see the three-way FP disagreement directly (in `$LLAMA2_DATA`, default
`.zero/llama2-data`, with `stories15M_q80.bin` present):

```sh
zig cc -target aarch64-macos -O3                  -o runq_fma   runq.c
zig cc -target aarch64-macos -O3 -ffp-contract=off -o runq_nofma runq.c
zig cc -target aarch64-macos -O0 -ffp-contract=off -o runq_O0    runq.c
# These three (and the Zero exe) disagree on the tightest int8 tokens; the f32
# run.c builds all agree with the Zero exe.
```

## Pointers

- `examples/llama2/validate.sh` — `build_ref` (`-ffp-contract=off`), `q_result`
  (gated vs informational), the final int8 NOTE.
- `examples/llama2/src/ops.0` — `matmulQ` / `quantizeActs` / `dequantRow` (the
  multiply-accumulate whose rounding differs FMA vs no-FMA); `swiglu` (`expf`).
- `../quant-temp-parity-divergence.md` — the resolved Linux libm case (prior
  layer of the same tight-margin story).
- `../cross-platform.md` — Risk 2 / Open Q4 (per-platform parity bar).
