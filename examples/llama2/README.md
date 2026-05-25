# llama2.zero

A port of Andrej Karpathy's [llama2.c](https://github.com/karpathy/llama2.c) to **Zero**.
It loads a Llama 2 checkpoint, runs the forward pass on the CPU, and streams the generated
tokens to stdout — a single statically-linked binary with no runtime dependencies.

**Status:** v0.1 — runs Karpathy's TinyLlamas checkpoints (`stories15M`, and the larger
`stories42M` / `stories110M` with no rebuild) on `linux-musl-x64`, `linux-musl-arm64`
(native-in-Docker on Apple Silicon), **and natively on `darwin-arm64` (Apple Silicon, no
Docker)**: single-threaded,
argmax / temperature / top-p / top-k sampling. Both f32 and an auto-detected, parity-checked
**int8 (`runq.c` v2)** path run from the same binary — the int8 path reaches real Llama-2
sizes via a one-time export (still single-threaded; no SIMD/threads yet). Output matches the
upstream `llama2.c` / `runq.c` **token-for-token**; see [Validation](#validation).

## Quickstart

```sh
# 1. build the Zero compiler (once)
make -C native/zero-c

# 2. build this example -> a static binary for your host
#    Linux x86-64:
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2
#    Linux ARM64:
bin/zero build --backend zero-elf-aarch64 --emit exe --target linux-musl-arm64 examples/llama2 --out .zero/out/llama2
#    macOS (Apple Silicon) — native, no Docker:
bin/zero build --backend zero-macho64 --emit exe --target darwin-arm64 examples/llama2 --out .zero/out/llama2

# 3. fetch the pretrained model (~60 MB) and tokenizer
curl -L -O https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin
curl -L -O https://github.com/karpathy/llama2.c/raw/master/tokenizer.bin

# 4. generate
.zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

On Apple Silicon the `--backend zero-macho64 --target darwin-arm64` build is a native
Mach-O binary — it runs directly, no container. The `linux-musl-arm64` build also runs in
an arm64 container, **natively (no qemu)** on Apple Silicon — Docker Desktop's Linux VM is
itself arm64:

```sh
docker run --rm --platform linux/arm64 -v "$(pwd)":/work -w /work alpine \
  .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

On a non-Apple-Silicon, non-Linux host, run the `linux-musl-x64` binary under an amd64
container (step 4 only):

```sh
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work alpine \
  .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

## Requirements

- **The Zero compiler** — `make -C native/zero-c` produces `bin/zero`.
- **A target-capable C toolchain** for `libm` linking (the kernels call
  `sqrtf`/`expf`/`powf`/…). `zig` is the simplest option — `bash scripts/setup-cross-toolchain.sh`
  installs it, or point `ZERO_CC` at a target-capable compiler. On macOS, `zig cc` links the
  system `libSystem` (which provides `libm`); on Linux it links zig's bundled `musl`.
- **Docker** — only needed to *run* the `linux-musl-x64` binary on a non-Linux host. Not needed
  on Apple Silicon, where the native `darwin-arm64` binary runs directly.

## Build

`src/main.0` uses `std.fs` (`mmap`), which falls outside the default self-host exe path, so
the example is built with an explicit backend:

```sh
# Linux x86-64 (static musl ELF):
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2

# Linux ARM64 (static musl ELF, AArch64):
bin/zero build --backend zero-elf-aarch64 --emit exe --target linux-musl-arm64 examples/llama2 --out .zero/out/llama2

# macOS / Apple Silicon (native Mach-O arm64):
bin/zero build --backend zero-macho64 --emit exe --target darwin-arm64 examples/llama2 --out .zero/out/llama2
```

`linux-musl-x64`, `linux-musl-arm64`, and `darwin-arm64` are the supported targets. The Mach-O
build links `mmap` and `libm` through `libSystem`; both ELF builds link them through zig's
bundled `musl` (`mmap` via raw syscalls, `libm` via the same external-call path) — so the example
needs no source change across the three, the same `.0` sources compile for all.

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
sizes become practical through the int8 path below.

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
fetches the weights, tokenizer, and `run.c`, builds both the Zero exe and the C reference, and
diffs the generated token streams over several prompts. It runs one of three branches, picked
automatically from the host:

| Host | Branch | Zero exe | C reference | Run via |
| --- | --- | --- | --- | --- |
| `darwin-arm64` (Apple Silicon) | **native, no Docker** | `zero-macho64`, `darwin-arm64` | `zig cc -target aarch64-macos` (`libSystem` `libm`) | **directly on the host** |
| arm64 host / `LLAMA2_FORCE_DOCKER=1` on Apple Silicon | Docker (arm64) | `zero-elf-aarch64`, `linux-musl-arm64` | `zig cc -target aarch64-linux-musl` (musl `libm`) | arm64 Alpine container — **native (no qemu)** on Apple Silicon |
| x86 host / non-arm64 / `LLAMA2_DOCKER_PLATFORM=linux/amd64` | Docker (amd64) | `zero-elf64`, `linux-musl-x64` | `zig cc -target x86_64-linux-musl` (musl `libm`) | amd64 Alpine container (qemu on non-x64) |

The Docker branch defaults its container arch to the host (`linux/arm64` on arm64 — native, no
qemu; `linux/amd64` on x86). Override with `LLAMA2_DOCKER_PLATFORM=linux/arm64` or `=linux/amd64`
(force an arch, e.g. an x86 CI gate), or `LLAMA2_FORCE_DOCKER=1` to take the Docker branch on
Apple Silicon.

```sh
bash examples/llama2/validate.sh
# darwin-arm64 host (native):
# ==> PASS: llama2.zero matches llama2.c token-for-token on stories15M (f32).
```

Both branches build the C reference with the **same toolchain and triple** as the Zero exe under
test — the crux of token-for-token parity. Every `libm` rounds `expf`/`sinf`/… by ~1 ULP
differently, so the reference links the *same* `libm` (zig's `musl` on Linux, `libSystem` on
macOS) and builds with `-ffp-contract=off` to match the backend's scalar FP (separate multiply
then add, never fused). So the bar is **parity per platform**, not byte-identity *across* them —
`libm` and FP-contraction differences make that impossible.

**f32 — gated on every branch:**

- **Temperature 0** (greedy argmax) — **byte-identical**.
- **Temperature > 0** — byte-identical once the seed and sampler match (`-s 12345 -p 0`); the
  xorshift\* PRNG is reproduced bit-for-bit.

Raw-byte fallback tokens are emitted as the actual byte on both sides, so streams are compared
directly with `cmp` — no normalization.

**int8 (`runq.c`)** runs whenever a quantized `stories15M_q80.bin` is present (the one-time
`export.py --version 2` step from [Quantized models](#quantized-int8-models)): the script builds
`runq_ref` from `runq.c` and diffs Zero against it across temperature 0, temperature > 0, and
top-p. It self-skips when the checkpoint is absent, so the f32 matrix stays dependency-free. The
int8 matrix is:

- **Gated on both Docker branches** — bit-exact vs `runq.c`; both sides link the same zig `musl`,
  so the `matmulQ` reductions round identically.
- **Informational on native darwin** — int8 logits sit nearer the sampling boundary than the
  sub-ULP FP-reduction-order gap between the Zero backend and any single `runq.c` build, so a few
  tokens flip. Expected per-platform behavior, not a Zero bug: f32 is bit-exact on the same host,
  and int8 is token-for-token on both `linux-musl` gates.

CI runs the Linux/Docker branch (the continuous parity gate); the native darwin branch is a
local superset that an Apple Silicon dev runs with zero extra setup.

**Checking a real quantized model.** The matrix targets `stories15M`, but the same mechanism
works on any quantized checkpoint: point `LLAMA2_DATA` at a directory holding your exported model
renamed `stories15M_q80.bin` plus its `tokenizer.bin`, and the int8 branch diffs it against
`runq_ref` token-for-token. For longer continuations, build the ref once
(`gcc -O3 -o runq_ref runq.c -lm`) and `cmp` Zero against
`./runq_ref <model>_q80.bin -z tokenizer.bin -t 0 -i "<prompt>"` (the exact invocation
`validate.sh` uses).

**CI without the 60 MB download.** `conformance/native/pass/generate-argmax.0` runs the full
forward → argmax → feedback loop on a tiny synthetic model and asserts the exact token sequence
against a libm C reference; `generate-argmax-q.0` is its int8 twin, against a `runq.c`-derived
reference on a tiny hand-authored quantized model.

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

## Limitations

- **Windows is not yet supported.** The supported targets are `linux-musl-x64`,
  `linux-musl-arm64` (native-in-Docker on Apple Silicon), and `darwin-arm64` (native Apple
  Silicon, no Docker) — all three parity-validated (f32 token-for-token per platform; the two
  `linux-musl` targets are also int8 token-for-token; see [Validation](#validation)). Other hosts
  run the `linux-musl-x64` binary via Docker.
- **Single-threaded, no SIMD.** Both the f32 and the int8 (`runq.c` v2) paths are scalar
  single-thread, so larger models are simply slower — throughput scales ~linearly with parameter
  count. Correctness is unaffected.
- **top-k is fixture-validated, not parity-checked.** It has no upstream `llama2.c` reference;
  every other sampling path (argmax, temperature, top-p) is token-for-token against `llama2.c` /
  `runq.c`.
