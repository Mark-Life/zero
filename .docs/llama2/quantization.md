# llama2.zero — int8 Quantization + Real Llama-2 (item G)

Executable plan for enhancements-backlog **item G** ("Real Llama-2 models + int8
quantization"). Takes the validated f32 inference engine and adds a quantized path so
the example runs models people actually use — TinyLlama-1.1B, Llama-2-7B — instead of
only TinyStories.

Companion to [`enhancements.md`](./enhancements.md) (the G spec, kept for context),
[`plan.md`](./plan.md) (v0.1, complete — the phase model this mirrors), and
[`overview.md`](./overview.md) (roadmap v0.3 = quantization). Read the v0.1 plan's
backend-gotchas first; this plan assumes them.

## Headline: G needs **no compiler change** for correctness

The G sketch in `enhancements.md` assumed a new `bytesAsI8` / `Span<i8>` backend
primitive was a prerequisite. It isn't. The whole int8 path lands in **pure Zero**, same
as items B and F, by exploiting what already shipped:

- **Weight int8 quants** live in the read-only mmap. Read them as `u8` (always worked)
  and reconstruct the signed value with branchless arithmetic:
  `sw = uw - 256 * (uw / 128)` (for `uw` in `0..255`, `uw/128` is `0`/`1`). No `i8` type,
  no sign-extending load, no new op.
- **Activation int8 quants** are *our* scratch — store them in a `MutSpan<i32>` region
  (already unlocked by item C's `bytesAsMutI32`); no packing into bytes, so the matmul
  reads them directly as signed `i32`.
- **Scales** are f32 — sub-slice them with `bytesAsF32` (landed 0c).
- **Region alloc** is `pageAlloc` (0a); the heapsort/`Sampler`-record register-budgeting
  patterns from B/F carry over verbatim.

So Phases Q1–Q5 are pure `examples/llama2/src/*.0`. The original "Both (compiler + Zero)"
classification is wrong; G is **Pure Zero** for the working model, with an *optional*
compiler track (Q-opt) that swaps the arithmetic sign-decode for a hardware `movsbl` and
shrinks the activation scratch — a perf/cleanliness win and a nice reusable primitive for
the upstream PR, but not on the critical path.

This is the single biggest de-risk: the validated f32 core (Phase 8 parity) is never
touched, and the quantized path needs zero backend risk to reach token-for-token parity
vs `runq.c`.

## What stays unchanged

- The **legacy f32 loader** (`checkpoint.0` `readConfig`/`mapWeights`) and the f32
  `forward` are untouched. `stories15M.bin` runs exactly as today.
- Format is **auto-detected** by magic number (below): legacy `.bin` → f32 path; `ak42`
  v2 → quantized path. No flag required (a `--quantized` override is optional).
- `ops.0` f32 kernels (`matmul`, `rmsnorm`, `softmax`, `rope`, `swiglu`) stay; the
  quantized kernels are *added* alongside.

## The reference format (runq.c "version 2") — captured here

`runq.c` is **not vendored** in this repo (validate.sh fetches `run.c` from
`karpathy/llama2.c` master at runtime). Q4 will fetch `runq.c` the same way. The v2
binary format is recorded here so the loader can be written without it in hand.

### Header — fixed 256 bytes

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 4 | `magic` u32 | **`0x616b3432`** = `"ak42"` LE |
| 4 | 4 | `version` i32 | **`2`** |
| 8 | 4 | `dim` i32 | |
| 12 | 4 | `hidden_dim` i32 | |
| 16 | 4 | `n_layers` i32 | |
| 20 | 4 | `n_heads` i32 | |
| 24 | 4 | `n_kv_heads` i32 | |
| 28 | 4 | `vocab_size` i32 | **positive** (sign trick is gone) |
| 32 | 4 | `seq_len` i32 | |
| 36 | 1 | `shared_classifier` u8 | `1` ⇒ `wcls` aliases token table |
| 37 | 4 | `group_size` i32 | **GS** — read it, never hardcode |
| 41 | 215 | padding | zero |
| 256 | … | weights | start of weight section |

Two differences from the legacy header the f32 loader parses: (1) a `magic`+`version`
prefix and a 256-byte pad, and (2) `shared_classifier` is an explicit byte — `vocab_size`
is always positive (no `0 - raw_vocab` dance).

`group_size` (GS) is chosen at **export** so it divides the matmul inner dimensions;
`export.py` backs off from 64 by halving until it fits. For `stories15M` (dim 288,
hidden 768) it lands at **32**. Read GS from the header and thread it through; the matmul
walks the inner dim in steps of GS and indexes scales at `.../GS`, so a wrong GS is silently
wrong output.

### Weight section layout

**First, the f32 (un-quantized) tensors** — the 1-D rmsnorm weights, contiguous:

```
rms_att_weight   : n_layers * dim   f32
rms_ffn_weight   : n_layers * dim   f32
rms_final_weight : dim              f32
```

Note this order differs from the legacy file (which interleaves rmsnorm groups between the
matmul tensors). v2 front-loads all three rmsnorm groups.

**Then the quantized tensors.** Each *logical* weight is stored as **n separate
`QuantizedTensor`s** (one per layer; `n = 1` for the token table / classifier), and each
`QuantizedTensor` is laid out **quants-then-scales**:

```
QuantizedTensor(size_each):
    int8  q[size_each]            // size_each bytes
    f32   s[size_each / GS]       // (size_each / GS) * 4 bytes
per-layer block bytes = size_each + (size_each / GS) * 4
```

Order and `size_each` (with `head_size = dim / n_heads`):

| Tensor | count | `size_each` |
|--------|-------|-------------|
| `q_tokens` | 1 | `vocab * dim` |
| `wq` | n_layers | `dim * (n_heads * head_size)` = `dim*dim` |
| `wk` | n_layers | `dim * (n_kv_heads * head_size)` = `dim*kv_dim` |
| `wv` | n_layers | `dim * (n_kv_heads * head_size)` = `dim*kv_dim` |
| `wo` | n_layers | `(n_heads * head_size) * dim` = `dim*dim` |
| `w1` | n_layers | `dim * hidden` |
| `w2` | n_layers | `hidden * dim` |
| `w3` | n_layers | `dim * hidden` |
| `wcls` | shared ? 0 : 1 | `dim * vocab` (aliases `q_tokens` when shared) |

Crucially the layers are **interleaved q/s** (`[L0.q L0.s][L1.q L1.s]…`), unlike the
legacy f32 file where a whole tensor's layers are contiguous. The loader computes a
per-layer **byte** stride, not an f32-element offset.

### The two math operations to port

**Quantized matmul** (`W (d,n) @ x (n,) -> xout (d,)`, both operands quantized):

```c
for i in 0..d:
  ival = 0 (int32); val = 0 (f32); in = i*n
  for j in 0..n step GS:
    for k in 0..GS:  ival += (int32)x.q[j+k] * (int32)w.q[in+j+k]
    val += (float)ival * w.s[(in+j)/GS] * x.s[j/GS]
    ival = 0
  xout[i] = val
```

**Activation quantize** (per group, symmetric, Q_MAX = 127):

```c
for g in 0..(n/GS):
  wmax = max(|x[g*GS .. g*GS+GS]|)
  scale = wmax / 127.0;  qx.s[g] = scale
  for i in 0..GS:  qx.q[g*GS+i] = round(x[g*GS+i] / scale)   // round = half away from zero
```

`dequantize(i) = q[i] * s[i/GS]`. No clamp is needed: `|x/scale| ≤ 127` by construction
(matches runq, which casts to `int8_t` without clamping).

## Zero design (pure-Zero)

### Record types (register-budgeted like B's `Sampler` / F's combined index)

```
// A single matrix's quantized weight, read-only, sliced out of the mmap per layer.
shape QWeight  { q: Span<u8>,      s: Span<f32> }     // q is raw bytes; sign-decode at read
// Quantized-activation scratch, writable (lives in RunState).
shape QActs    { q: MutSpan<i32>,  s: MutSpan<f32> }  // signed quants kept as i32 — no packing
```

Both are two-span records (one nesting level — within the aggregate ABI). Passed **by
pointer = 1 int reg** (per [`aggregate-abi.md`](./aggregate-abi.md) shape-param ABI), which
is what keeps the kernels under the 6-reg cap:

- `matmulQ(out: MutSpan<f32>, x: QActs, w: QWeight, gs: usize)` → 2 + 1 + 1 + 1 = **5 regs**.
- `quantizeActs(dst: QActs, x: Span<f32>, gs: usize)` → 1 + 2 + 1 = **4 regs**.

(`x: QActs` passed where only reads happen is fine — `MutSpan<T>` is covariant to `Span<T>`,
Phase 4.) Reading/writing a span **field** of these records must **bind a local first**
(`let q = w.q; … q[idx]`), the F-item gotcha — direct `w.q[idx]` mis-resolves the slice base.

### `ops.0` — three new kernels (pure Zero)

```
pub fun matmulQ(out: MutSpan<f32>, x: QActs, w: QWeight, gs: usize) -> Void
pub fun quantizeActs(dst: QActs, x: Span<f32>, gs: usize) -> Void
pub fun dequantRow(out: MutSpan<f32>, w: QWeight, row: usize, n: usize, gs: usize) -> Void
```

- **`matmulQ`** — port of the matmul above. Weight quant read + sign-decode in the inner
  loop: `let uw: i32 = wq[in+j+k] as i32; let sw = uw - 256 * (uw / 128)`. Activation quant
  read directly from `x.q` (already signed i32). Per-group `ival` (i32) → `val += (ival as
  f32) * ws[(in+j)/gs] * xs[j/gs]`. Same op order as runq ⇒ bit-exact. Inner load is a bare
  `Span<u8>` index — the hot-loop fast shape (risk #2).
- **`quantizeActs`** — port of quantize. `absf` for `wmax`; `scale = wmax / 127.0`; quant
  `= roundHA(x[i] / scale)`. No `roundf` in `std.math`, so `roundHA(v)` = half-away-from-zero
  via `floorf`: `v >= 0 ? floorf(v + 0.5) : 0 - floorf((0 - v) + 0.5)`. Store the signed quant
  into `dst.q` (i32). Guard `wmax == 0` (degenerate group ⇒ scale 0 ⇒ div-by-zero); runq
  produces inf/NaN there but real groups never hit it — match runq (no guard) for parity, or
  guard to `scale = 1` and document the (unreachable) divergence.
- **`dequantRow`** — embedding gather: `out[i] = sw(w.q[row*n + i]) * w.s[(row*n + i)/gs]`,
  the same sign-decode. Used for the token embedding and (optionally) any single-row dequant.
  This is a deviation-that-improves over runq: runq dequantizes the **entire** token table to
  a `vocab*dim` f32 buffer at load (37 MB for stories15M, 524 MB for 7B); we dequantize only
  the one embedded row per step, so no big buffer and identical numbers.

### `checkpoint.0` — v2 loader (new functions, legacy untouched)

```
pub fun isQuantizedV2(bytes: Span<u8>) -> Bool           // readU32Le(bytes,0) == 0x616b3432
pub fun readConfigV2(bytes: Span<u8>) -> Config          // offsets 8..36; shared byte @36; positive vocab
pub fun groupSize(bytes: Span<u8>) -> usize              // readI32Le(bytes, 37)
pub fun mapWeightsQ(bytes: Span<u8>, cfg, gs) -> QuantizedWeights
pub fun expectedFileBytesQ(cfg, gs) -> usize             // 256 + f32 block + Σ quantized blocks
```

`QuantizedWeights` carries the **byte base offset** and **`size_each`** of each weight
group (all `usize`), the f32 rmsnorm `Span<f32>` views (sliced exactly like the f32 path,
but from byte 256, in the new order), and a reference to `bytes` + `gs`. Per-layer slicing
happens in `forward` (or a tiny helper that returns **scalars**, not a record — a
record-returning helper with `(bytes, base, se, gs, l)` is 5 reg-args + sret = the limit; keep
it scalar-returning or inline). For layer `l` of a group at `base`/`se`:

```
block = se + (se / gs) * 4
qstart = base + l * block;  qend = qstart + se
sstart = qend;              send  = sstart + (se / gs) * 4
let qw = QWeight { q: bytes[qstart..qend], s: std.mem.bytesAsF32(bytes[sstart..send]) }
```

`expectedFileBytesQ` validates the mapping length (mirrors `expectedFileFloats`):
`256 + (n_layers*dim + n_layers*dim + dim)*4 + Σ_groups count * (se + (se/gs)*4)`, with the
`wcls` group present only when `shared_classifier == 0`.

### `transformer.0` — `forwardQ` + RunState additions

Add quantized-activation scratch to `RunState` (mirrors runq's `xq`/`hq`): a `MutSpan<i32>`
quant buffer + `MutSpan<f32>` scale buffer for the `dim`-width activation (`xq`), and the
same for the `hidden`-width activation (`hq`). Sizes: quants `dim` / `hidden` i32; scales
`dim/gs` / `hidden/gs` f32. Extend `runStateFloats`/`mallocRunState` (or a parallel
`runStateBytesQ`/`mallocRunStateQ`) to lay these into the page region.

`forwardQ(cfg, qw, state, token, pos, gs)` mirrors `forward` one-to-one, with each matmul
replaced by *quantize-then-matmulQ* (exactly runq's forward):

| f32 site (transformer.0) | quantized replacement |
|--------------------------|------------------------|
| embedding copy (`x[ce]=emb[ce]`) | `dequantRow(x, qTokens, token, dim, gs)` |
| `matmul(q, xb, wq_l)` | `quantizeActs(xq, xb, gs); matmulQ(q, xq, wq_L, gs)` |
| `matmul(k, xb, wk_l)` / `matmul(v, …)` | reuse `xq` (same `xb`); `matmulQ(k, xq, wk_L, gs)` … |
| `matmul(xb2, xb, wo_l)` | `quantizeActs(xq, xb, gs); matmulQ(xb2, xq, wo_L, gs)` |
| `matmul(hb, xb, w1_l)` / `matmul(hb2, …)` | `quantizeActs(xq, xb, gs); matmulQ(hb, xq, w1_L, gs); matmulQ(hb2, xq, w3_L, gs)` |
| `matmul(xb, hb, w2_l)` | `quantizeActs(hq, hb, gs); matmulQ(xb, hq, w2_L, gs)` |
| `matmul(logits, x, wcls)` | `quantizeActs(xq, x, gs); matmulQ(logits, xq, wcls_L, gs)` |

rmsnorm/rope/softmax/swiglu/attention/residuals are **unchanged** — they operate on f32
activations exactly as today. GQA (`kv_mul`, `kvh`) is unchanged and finally exercised on a
real grouped checkpoint in Q5 (every TinyStories model is MHA; this branch has never run
with `kv_mul > 1`).

### `main.0` — dispatch

After mmap, branch on `isQuantizedV2(bytes)`: quantized ⇒ `readConfigV2` + `groupSize` +
`mapWeightsQ` + `mallocRunStateQ` + `forwardQ`; else the existing f32 path. Tokenizer,
sampler, and the generation loop are format-agnostic and shared. (A `--quantized` /
`--ctx` override is optional polish.)

## Phases

Ordered to put a working quantized model on screen with zero backend risk first, then real
models, then the optional compiler optimization.

### Q1 — v2 checkpoint loader (pure Zero) — Small/Medium
`checkpoint.0`: `isQuantizedV2`, `readConfigV2`, `groupSize`, `mapWeightsQ`,
`expectedFileBytesQ`, the `QWeight` shape + per-layer byte-offset arithmetic. **Acceptance:**
a hand-authored tiny v2 byte array (tiny dims, GS small, hand-computable offsets) →
`conformance/native/pass/checkpoint-q-v2.0` asserts the magic/version detection, the f32
rmsnorm offsets, and a couple of per-layer `(qstart, sstart)` boundaries. libm-free ⇒ runs
in CI.

### Q2 — quantized kernels (pure Zero, `ops.0`) — Medium
`matmulQ`, `quantizeActs`, `dequantRow` + `QActs`/`QWeight`. **Acceptance:** split fixtures —
`sampler-q-matmul.0` (matmulQ + sign-decode on a fixed tiny `(q,s)` weight and `i32`
activation, hand-computed `ival`/`val`, **libm-free → CI**); `ops-q-quantize.0` (quantizeActs
round-trip: quantize a known f32 vector, assert quants/scales, dequant within tolerance;
`libm: true` for `floorf`/`absf`). Verify the round-half-away path against C `round()` on a
±0.5 case.

### Q3 — wire `forwardQ` + `main.0` (pure Zero) — Medium
RunState `xq`/`hq` scratch; `forwardQ`; `main.0` magic dispatch. **Acceptance:** end-to-end on
a tiny synthetic v2 model under `--backend zero-elf64` + Docker (`forward: ok`-class smoke,
then the Q4 fixture). Confirm the `QActs` record with `MutSpan` fields lays out and writes via
bind-local (`let q = dst.q; q[i] = …`) — expected from F's read pattern; this is the one ABI
point to verify early (if it fails, fall back to passing a `MutSpan<u8>` base + scalar
offsets, splitting quantize into two calls).

### Q4 — parity validation vs `runq.c` — Medium
- **CI fixture** `conformance/native/pass/generate-argmax-q.0` (`libm: true`): the
  `generate-argmax.0` twin on a **hand-authored tiny v2 quantized** model (dim 4 / 1 layer /
  2 heads / vocab 4 / GS 2 or 4), full forward→argmax→feedback, asserting an exact token
  sequence vs a `runq.c`-derived reference (`.zero/probe/llama2_genq_ref.c`). Hand-author the
  bytes (Non-Goals forbid shipping a quantizer; tiny dims make quants hand-computable) — same
  approach `generate-argmax.0` uses for f32.
- **Real-model parity** in `examples/llama2/validate.sh`: fetch `runq.c` (alongside `run.c`),
  build `runq_ref` (`gcc -O3 -o runq_ref runq.c -lm`), and add a quantized branch that runs
  Zero vs `runq_ref` token-for-token on a quantized `stories15M` (temp 0 + temp>0 + top-p,
  reusing the existing seed/normalization machinery). The quantized `.bin` comes from a
  documented one-time `export.py --version 2` step (needs PyTorch + `stories15M.pt`); the
  branch self-skips with a note if the file is absent so the f32 parity matrix still runs
  dependency-free.

### Q5 — real models — Medium (validation, no new code)
- **TinyLlama-1.1B** first: it's **GQA** (`n_heads 32`, `n_kv_heads 4`) ⇒ first real exercise
  of the `kv_mul > 1` path. Verify token-for-token vs `runq.c`, then coherence under Docker.
- **Llama-2-7B** (MHA): bigger memory — int8 weights ≈ 6.7 GB + scales mmap'd lazily; the KV
  cache scales with `seq_len` (can be GBs at seq_len 4096), so size from Config and note the
  RAM bar. Document `export.py` for `--meta-llama` / `--hf`. Coherence check under Docker.
- README: quantized download/export/run section; results line in `validate.sh`.

### Q-opt — `bytesAsI8` + `i8` scalar (compiler, **optional, deferred**) — Small/Medium
Swap the arithmetic sign-decode for a hardware sign-extending load and let activation quants
pack into `i8` (smaller scratch). A clean reusable primitive for the upstream PR; **not
required** for parity. Trivial mirror of item C plus one genuinely new bit — a signed 8-bit
load (`movsbl 0F BE`) vs `u8`'s zero-extend (`movzbl 0F B6`). Touch points (mapped, exact):
- `zero.h`: add `IR_TYPE_I8` to `IrTypeKind`.
- `checker.c`: `bytesAsI8`→`Span<i8>` / `bytesAsMutI8`→`MutSpan<i8>` in the three tables
  (~990, ~1161, ~1324); `main.c` builtins table (~867) + helpers JSON (~8218).
- `ir.c`: `ir_byte_view_reinterpret_element` (+I8 arm); allow `IR_TYPE_I8` in the Span
  element-type checks (~202, ~231).
- `emit_elf64.c`: `elf_type_byte_size` (I8→1), `elf_type_name`, `elf_emit_load_field_rax`
  (I8 ⇒ `0F BE`), `IR_VALUE_INDEX_LOAD` (I8 arm `0F BE`), **`IR_VALUE_BYTE_VIEW_INDEX_LOAD`**
  (today always zero-extends — gate on `element_type == IR_TYPE_I8` for `0F BE`; this is the
  one that matters for `bytesAsI8` indexing), and the element-type allow-lists in
  `elf_validate_function` + `elf_emit_byte_view`.
- Fixtures: `mem-bytes-as-i8.0` (signedness round-trip, e.g. byte `0x80` → `-128`) + a fail
  bounds case; then drop the `uw - 256*(uw/128)` decode in `matmulQ`/`dequantRow`.

## File touchpoints

| File | Phase | Change |
|------|-------|--------|
| `examples/llama2/src/checkpoint.0` | Q1 | `isQuantizedV2`, `readConfigV2`, `groupSize`, `mapWeightsQ`, `expectedFileBytesQ`, `QWeight` |
| `examples/llama2/src/ops.0` | Q2 | `matmulQ`, `quantizeActs`, `dequantRow`, `QActs` |
| `examples/llama2/src/transformer.0` | Q3 | RunState `xq`/`hq` scratch, `mallocRunStateQ`, `forwardQ` |
| `examples/llama2/src/main.0` | Q3 | magic dispatch f32 vs quantized |
| `examples/llama2/validate.sh` | Q4 | fetch+build `runq.c`; quantized parity branch (self-skip if no q-model) |
| `examples/llama2/README.md` | Q4/Q5 | export.py + quantized download/run/parity |
| `conformance/native/pass/checkpoint-q-v2.0` | Q1 | loader fixture (CI) |
| `conformance/native/pass/sampler-q-matmul.0` | Q2 | matmulQ + sign-decode (CI, libm-free) |
| `conformance/native/pass/ops-q-quantize.0` | Q2 | quantizeActs round-trip (libm) |
| `conformance/native/pass/generate-argmax-q.0` | Q4 | end-to-end quantized token seq (libm) |
| `conformance/run.mjs` | Q1/Q2/Q4 | fixture entries |
| `native/zero-c/src/{checker,ir,emit_elf64,main}.c`, `include/zero.h` | Q-opt | `bytesAsI8`/`i8` (optional) |
| `native/zero-c/conformance/native/{pass,fail}/mem-bytes-as-i8*.0` | Q-opt | i8 fixtures (optional) |

## Risks

1. **Parity is exact-or-bust.** Quantized matmul must reproduce runq's integer products and
   float scale order bit-for-bit; the only subtlety is `roundHA` matching C `round()` (half
   away from zero) and the per-group accumulate/reset. Cover with a hand-computed kernel
   fixture (Q2) before the integration fixture (Q4).
2. **`group_size` from the header, never hardcoded.** Wrong GS = silently wrong output, no
   trap. It also constrains which models load (GS must divide the matmul inner dims) — but
   that's `export.py`'s problem; just read and use it.
3. **`QActs` MutSpan-in-record write-through** (Q3) is the one unproven ABI point. Verify
   early; fallback is scalar-offset params (no record). Low risk — F proved span-field reads;
   writes go through a bound local.
4. **matmulQ inner-loop cost.** The `uw - 256*(uw/128)` decode adds two int ops per weight in
   the hottest loop. Correctness-first is the bar (risk #2 of v0.1); Q-opt removes it with
   `movsbl`. Don't pre-optimize — measure with `validate.sh`'s tok/s line (added in F).
5. **Real-model memory** (Q5). 7B int8 weights mmap lazily, but the KV cache is a real
   `pageAlloc` sized off `seq_len` — GBs at large contexts. TinyLlama-1.1B first keeps the
   bar modest and exercises GQA.
6. **export.py / torch dependency** for the real q-model. Keep it a documented one-time step
   and self-skip the validate.sh branch when the file is absent, so the existing dependency-free
   f32 matrix is never weakened. The CI fixture hand-authors bytes (no torch).

## Open questions

1. **Auto-detect by magic vs explicit `--quantized`?** *Recommend auto-detect* (`ak42`
   magic is unambiguous; legacy files start with a small `dim`). Add `--quantized` only if a
   malformed file ever needs forcing.
2. **Token table: dequant-on-gather (proposed) vs runq's full-table dequant?** *Recommend
   on-gather* — identical numbers, no `vocab*dim` f32 buffer (37 MB → 524 MB saved), and the
   shared classifier still uses the quantized `q_tokens` for its matmul.
3. **Q-opt now or later?** *Recommend later* — land correctness in pure Zero (Q1–Q5), then
   add `bytesAsI8`/`i8` as a standalone primitive PR with its own fixtures. Decouples backend
   risk from the model bring-up.
4. **First real model — TinyLlama-1.1B or 7B?** *Recommend TinyLlama-1.1B first*: it's the
   smallest real GQA model, so it both fits comfortably and is the first true `kv_mul > 1`
   test; 7B follows for the headline.
5. **int4 after int8?** Out of scope here; int8 is the 4× cut that unblocks real models. Note
   for a future item if wanted (overview keeps quantization at int8 for v0.3).

## Appendix — commands

```sh
# build compiler (only needed for Q-opt)
make -C native/zero-c

# cross-toolchain for libm linking (zig)
bash scripts/setup-cross-toolchain.sh

# build the example
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2

# export a quantized stories model (one-time; needs torch + stories15M.pt)
python export.py stories15M_q80.bin --version 2 --checkpoint stories15M.pt

# export a real model
python export.py llama2_7b_q80.bin   --version 2 --hf meta-llama/Llama-2-7b-hf
python export.py tinyllama_q80.bin   --version 2 --hf TinyLlama/TinyLlama-1.1B-Chat-v1.0

# run a quantized model under amd64 Docker
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work alpine \
  .zero/out/llama2 stories15M_q80.bin --prompt "Once upon a time" --tokens 256 --temperature 0

# tests + parity
pnpm run conformance && pnpm run docs:test
bash examples/llama2/validate.sh
```

## Connections

- [`enhancements.md`](./enhancements.md) — item G spec (this plan supersedes its
  "Both/compiler-needed" framing with a pure-Zero path); items B/F primitives reused here.
- [`plan.md`](./plan.md) — v0.1 phase model + backend gotchas this mirrors; Phase 8 is the
  parity harness Q4 extends.
- [`aggregate-abi.md`](./aggregate-abi.md) — the shape/span ABI the `QWeight`/`QActs` records
  rely on (by-pointer params, span/MutSpan fields, the 5-arg sret limit).
- [`overview.md`](./overview.md) — roadmap v0.3 (int8) and the f16/bf16 open question (a
  separate, larger language ask not on G's path).
