# llama2.zero

A port of Andrej Karpathy's [llama2.c](https://github.com/karpathy/llama2.c) to **Zero**.
It loads a Llama 2 checkpoint, runs the forward pass on the CPU, and streams the generated
tokens to stdout — a single statically-linked binary with no runtime dependencies.

**Status:** v0.1 — runs Karpathy's TinyLlamas checkpoints (`stories15M`, and the larger
`stories42M` / `stories110M` with no rebuild) on `linux-musl-x64`: single-threaded,
argmax / temperature / top-p / top-k sampling. Both f32 and an auto-detected, parity-checked
**int8 (`runq.c` v2)** path run from the same binary — the int8 path reaches real Llama-2
sizes via a one-time export (still single-threaded; no SIMD/threads yet). Output matches the
upstream `llama2.c` / `runq.c` **token-for-token**; see [Validation](#validation).

## Quickstart

```sh
# 1. build the Zero compiler (once)
make -C native/zero-c

# 2. build this example -> a static linux-musl-x64 binary
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2

# 3. fetch the pretrained model (~60 MB) and tokenizer
curl -L -O https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin
curl -L -O https://github.com/karpathy/llama2.c/raw/master/tokenizer.bin

# 4. generate (Linux x86-64)
.zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

On macOS or any non-Linux host, run the binary under an amd64 container (step 4 only):

```sh
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work alpine \
  .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

## Requirements

- **The Zero compiler** — `make -C native/zero-c` produces `bin/zero`.
- **A target-capable C toolchain** for static `libm` linking (the kernels call
  `sqrtf`/`expf`/`powf`/…). `zig` is the simplest option — `bash scripts/setup-cross-toolchain.sh`
  installs it, or point `ZERO_CC` at a `linux-musl-x64`-capable compiler.
- **Docker** — only needed to *run* the `linux-musl-x64` binary on a non-Linux host.

## Build

`src/main.0` uses `std.fs` (`mmap`), which falls outside the default self-host exe path, so
the example is built with the direct ELF64 backend explicitly:

```sh
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2
```

`linux-musl-x64` is the only supported target for v0.1.

## Run

```
llama2 <model.bin> --prompt <text> --tokens <n> [--temperature <t>] [--topp <p>] [--topk <k>] [--tokenizer <path>]
```

| Flag | Default | Notes |
| --- | --- | --- |
| `<model.bin>` | — | Positional; path to the checkpoint (required). |
| `--prompt` | `""` | Prompt text, BPE-encoded. Must fit the model's context window (`seq_len` tokens). |
| `--tokens` | `256` | Tokens to generate; clamped to the model's `seq_len` (`0` ⇒ `seq_len`). |
| `--temperature` | `1.0` | Sampling temperature. `0` ⇒ deterministic greedy argmax. |
| `--topp` | `0` | Top-p (nucleus) threshold in `(0, 1)`; `0` (or `≥1`) disables it. Matches `llama2.c -p`. |
| `--topk` | `0` | Top-k: sample from the `k` highest-probability tokens; `0` disables it. Takes precedence over `--topp`. |
| `--tokenizer` | `tokenizer.bin` | Path to the Karpathy `tokenizer.bin` vocab. |

The prompt is re-emitted from its tokens (the leading sentencepiece space after BOS is
stripped), then the continuation streams token-by-token to stdout, unbuffered, until
`--tokens` steps or a BOS delimiter. A bad model path, a malformed or mismatched header, a
non-integer `--tokens`, a missing tokenizer, an over-long prompt, or a failed allocation each
report a distinct `error: …` on stderr and exit nonzero.

```
$ .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 64 --temperature 0
Once upon a time, there was a little girl named Lily. She loved to play outside in the sunshine. One day, she saw a big, red ball in the sky. It was the sun! ...
```

## Models

Every dimension is read from the checkpoint header at runtime, and the activation / KV-cache
buffers are sized from it, so any same-format Karpathy
[TinyLlamas](https://huggingface.co/karpathy/tinyllamas) checkpoint runs **with no rebuild**:

```sh
curl -L -O https://huggingface.co/karpathy/tinyllamas/resolve/main/stories110M.bin
.zero/out/llama2 stories110M.bin --prompt "Once upon a time" --tokens 256
```

`stories42M` (159 MB) and `stories110M` (418 MB) both generate coherent text and match
`llama2.c` **token-for-token** (temperature 0 and temperature > 0). Larger models are simply
slower — single-threaded f32 inference scales ~linearly with parameter count. Real Llama-2
sizes are a [backlog](../../.docs/llama2/enhancements.md) item.

### Quantized (int8) models

The engine also reads the `runq.c` **"version 2"** format — int8 matmul weights with per-group
f32 scales (a 4× size cut over f32), the rmsnorm weights kept in f32. The on-disk format is
**auto-detected** by its magic number, so the same binary and the same CLI run a quantized
checkpoint with no extra flag:

```sh
# one-time: produce a quantized stories15M (needs PyTorch + the .pt weights)
pip install torch numpy
curl -L -O https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.pt
curl -L -O https://github.com/karpathy/llama2.c/raw/master/export.py
python export.py stories15M_q80.bin --version 2 --checkpoint stories15M.pt

# run it exactly like an f32 model — the v2 magic is detected automatically
.zero/out/llama2 stories15M_q80.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

The group size is read from the header (export.py picks the largest that divides the matmul
dimensions — 32 for `stories15M`); the int8 matmul reproduces `runq.c`'s integer products and
per-group rescale bit-for-bit, so output matches **token-for-token** (see [Validation](#validation)).

#### Real models

The 4× size cut is what makes real Llama-2 weights fit on a CPU. The same export + run flow
applies — only the checkpoint changes. `export.py` is the upstream
[`karpathy/llama2.c`](https://github.com/karpathy/llama2.c/blob/master/export.py) tool; the
export is a one-time PyTorch step, after which the resulting `*_q80.bin` runs like any other
checkpoint (the v2 magic is auto-detected, no flag).

**TinyLlama-1.1B** is the smallest real **GQA** model (`n_heads 32`, `n_kv_heads 4`), so it's
the natural first target — it fits comfortably and exercises the grouped key/value path:

```sh
# one-time export from HuggingFace weights
python export.py tinyllama_q80.bin --version 2 --hf TinyLlama/TinyLlama-1.1B-Chat-v1.0

.zero/out/llama2 tinyllama_q80.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

Because `n_kv_heads < n_heads`, this is a `kv_mul > 1` model — every TinyStories checkpoint is
MHA (`kv_mul == 1`), so TinyLlama is the first real workout for the grouped-query attention
path. That same `kv_mul > 1` key/value path is validated **token-for-token** by the int8
conformance fixture (which is deliberately built with `n_heads 2`, `n_kv_heads 1`), so the math
is already proven; TinyLlama just confirms it at scale.

**Llama-2-7B** (MHA) is the headline. Use `--hf` or `--meta-llama`; note the HF repo is gated
and needs Meta access:

```sh
python export.py llama2_7b_q80.bin --version 2 --hf meta-llama/Llama-2-7b-hf
# or, from local Meta checkpoints:
# python export.py llama2_7b_q80.bin --version 2 --meta-llama path/to/llama-2-7b

.zero/out/llama2 llama2_7b_q80.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

**Memory.** Two pools dominate RAM:

- **Weights** are `mmap`'d read-only and page in lazily. int8 7B weights are ≈ 6.7 GB
  (≈ 4× smaller than the f32 ≈ 27 GB). TinyLlama-1.1B is ≈ 1.1 GB.
- **The KV cache** is a real anonymous-`mmap` allocation sized from the header — keys plus
  values across every layer and context position:
  `n_layers · seq_len · kv_dim · 2 · 4 bytes` (f32), where `kv_dim = dim · n_kv_heads / n_heads`.
  At large `seq_len` (e.g. 4096) this reaches **multiple GB** regardless of weight precision —
  GQA shrinks it (`kv_dim < dim`) but it still grows linearly with context. For Llama-2-7B
  (`dim 4096`, `n_kv_heads 32`, `n_layers 32`) at `seq_len 4096` that is ≈ 4 GB.

So RAM is roughly *weights + KV cache*. TinyLlama-1.1B keeps both modest; Llama-2-7B wants a
roomy machine (the KV cache scales with `seq_len`, so a shorter context lowers the bar).

## Validation

[`validate.sh`](./validate.sh) checks numerical parity against the upstream `llama2.c`. It
fetches the weights, tokenizer, and `run.c`, builds both programs (the C reference inside an
amd64 Alpine container so both link the **same musl libm**), and diffs the generated token
streams over several prompts. Requires Docker.

```sh
bash examples/llama2/validate.sh
# ==> PASS: llama2.zero matches llama2.c token-for-token on stories15M.
```

- **Temperature 0** (greedy argmax) — output is **byte-identical**.
- **Temperature > 0** — also byte-identical once the seed and sampler are matched
  (`llama2.c -s 12345 -p 0`), because the xorshift\* PRNG is reproduced bit-for-bit.

Both sides emit raw-byte fallback tokens as the actual byte, so the token streams are
compared directly (`cmp`) — no normalization needed.

The same script also runs an **int8 (`runq.c`) parity matrix** when a quantized
`stories15M_q80.bin` is present: it builds `runq_ref` from `runq.c` (the same way it builds
`run_ref` from `run.c`) and diffs Zero vs `runq_ref` token-for-token across temperature 0,
temperature > 0, and top-p. That checkpoint is the one-time `export.py --version 2` step from
[Quantized models](#quantized-int8-models), so the quantized matrix self-skips (never failing
the run) when it is absent — the f32 matrix above stays dependency-free.

`validate.sh`'s matrix targets `stories15M`, but the same mechanism parity-checks a *real*
quantized model. The quick way: set `LLAMA2_DATA` to a directory holding your exported real
checkpoint renamed to `stories15M_q80.bin` plus the matching `tokenizer.bin`, and the int8
branch diffs it against `runq_ref` token-for-token. (To diff longer real continuations
directly, build `runq_ref` once — `gcc -O3 -o runq_ref runq.c -lm` — then `cmp` the Zero exe's
output against `./runq_ref <model>_q80.bin -z tokenizer.bin -t 0 -i "<prompt>"`, the exact
invocation `validate.sh` uses.)

For CI (no 60 MB download), `conformance/native/pass/generate-argmax.0` runs the full
forward → argmax → feedback loop on a tiny synthetic model and asserts the exact token
sequence against a libm C reference; `conformance/native/pass/generate-argmax-q.0` is its int8
twin, asserting the same kind of sequence against a `runq.c`-derived reference on a tiny
hand-authored quantized model.

## How it works

The pipeline mirrors `llama2.c`, split across one flat package:

| File | Role |
| --- | --- |
| `src/main.0` | CLI parsing, `mmap` load, the autoregressive generation loop, token streaming |
| `src/checkpoint.0` | Parse the header → `Config` and map weights as zero-copy `Span<f32>` views (`readConfig`/`mapWeights`); also the v2 (`runq.c`) int8 loader (`readConfigV2`/`groupSize`/`mapWeightsQ`) |
| `src/transformer.0` | `RunState` (activations + KV cache) and the `forward()` / `forwardQ()` passes |
| `src/ops.0` | Kernels: f32 `rmsnorm`/`matmul`/`softmax`/`rope`/`swiglu` + int8 `matmulQ`/`quantizeActs`/`dequantRow` |
| `src/tokenizer.0` | Karpathy `tokenizer.bin` BPE `encode` / `decode` |
| `src/sampler.0` | `argmax`, temperature / top-p / top-k sampling, the sort they share, xorshift\* PRNG |

Weights are `mmap`'d read-only and sliced into typed `Span<f32>` views with no copy; the
mutable `RunState` is one zeroed anonymous-`mmap` region (sized at runtime from the header).
The design docs under [`.docs/llama2/`](../../.docs/llama2/) cover the rationale and the
compiler work this exercised — start with [`overview.md`](../../.docs/llama2/overview.md) and
[`plan.md`](../../.docs/llama2/plan.md).

## Limitations (v0.1)

- **Target:** `linux-musl-x64` only — run via Docker on other hosts.
- **Precision / threads:** single-threaded f32, plus an int8 (`runq.c` v2) quantized path
  (auto-detected, parity-checked). Real Llama-2 sizes additionally want threading/SIMD (see the
  [backlog](../../.docs/llama2/enhancements.md)).
- **Sampling:** argmax, temperature, top-p (nucleus), and top-k — the full sampler. top-p
  is parity-checked against `llama2.c`; top-k has no upstream reference (fixture-validated).
- **Tokenizer:** sorted-index lookup — an entry-offset table (O(1) random access) plus a
  string-sorted id list binary-searched on `encode` (O(log vocab)), replacing the v0.1 linear
  scan. Token ids are identical (parity-checked against `llama2.c`).

Post-v0.1 enhancements (cross-platform, quantization, training) are scoped
in [`.docs/llama2/enhancements.md`](../../.docs/llama2/enhancements.md); bigger models and
top-p/top-k have since landed.
