# llama2.zero

A port of Andrej Karpathy's [llama2.c](https://github.com/karpathy/llama2.c) to **Zero**.
It loads a Llama 2 checkpoint, runs the forward pass on the CPU, and streams the generated
tokens to stdout — a single statically-linked binary with no runtime dependencies.

**Status:** v0.1 — runs Karpathy's TinyLlamas checkpoints (`stories15M`, and the larger
`stories42M` / `stories110M` with no rebuild) on `linux-musl-x64`: f32, single-threaded,
argmax + temperature sampling. Output matches the upstream `llama2.c` **token-for-token**;
see [Validation](#validation).

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
llama2 <model.bin> --prompt <text> --tokens <n> [--temperature <t>] [--tokenizer <path>]
```

| Flag | Default | Notes |
| --- | --- | --- |
| `<model.bin>` | — | Positional; path to the checkpoint (required). |
| `--prompt` | `""` | Prompt text, BPE-encoded. Must fit the model's context window (`seq_len` tokens). |
| `--tokens` | `256` | Tokens to generate; clamped to the model's `seq_len` (`0` ⇒ `seq_len`). |
| `--temperature` | `1.0` | Sampling temperature. `0` ⇒ deterministic greedy argmax. |
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
slower — single-threaded f32 inference scales ~linearly with parameter count. Quantization
and real Llama-2 sizes are [backlog](../../.docs/llama2/enhancements.md) items.

## Validation

[`validate.sh`](./validate.sh) checks numerical parity against the upstream `llama2.c`. It
fetches the weights, tokenizer, and `run.c`, builds both programs (the C reference inside an
amd64 Alpine container so both link the **same musl libm**), and diffs the generated token
streams over several prompts. Requires Docker and node.

```sh
bash examples/llama2/validate.sh
# ==> PASS: llama2.zero matches llama2.c token-for-token on stories15M.
```

- **Temperature 0** (greedy argmax) — output is **byte-identical**.
- **Temperature > 0** — also byte-identical once the seed and sampler are matched
  (`llama2.c -s 12345 -p 0`), because the xorshift\* PRNG is reproduced bit-for-bit.

Both sides emit raw-byte fallback tokens as the actual byte, so the token streams are
compared directly (`cmp`) — no normalization needed.

For CI (no 60 MB download), `conformance/native/pass/generate-argmax.0` runs the full
forward → argmax → feedback loop on a tiny synthetic model and asserts the exact token
sequence against a libm C reference.

## How it works

The pipeline mirrors `llama2.c`, split across one flat package:

| File | Role |
| --- | --- |
| `src/main.0` | CLI parsing, `mmap` load, the autoregressive generation loop, token streaming |
| `src/checkpoint.0` | Parse the checkpoint header → `Config`; map weights as zero-copy `Span<f32>` views |
| `src/transformer.0` | `RunState` (activations + KV cache) and the `forward()` pass |
| `src/ops.0` | Kernels: `rmsnorm`, `matmul`, `softmax`, `rope`, `swiglu` |
| `src/tokenizer.0` | Karpathy `tokenizer.bin` BPE `encode` / `decode` |
| `src/sampler.0` | `argmax`, temperature sampling, xorshift\* PRNG |

Weights are `mmap`'d read-only and sliced into typed `Span<f32>` views with no copy; the
mutable `RunState` is one zeroed anonymous-`mmap` region (sized at runtime from the header).
The design docs under [`.docs/llama2/`](../../.docs/llama2/) cover the rationale and the
compiler work this exercised — start with [`overview.md`](../../.docs/llama2/overview.md) and
[`plan.md`](../../.docs/llama2/plan.md).

## Limitations (v0.1)

- **Target:** `linux-musl-x64` only — run via Docker on other hosts.
- **Precision / threads:** f32, single-threaded, no quantization — so the practical ceiling
  is the TinyLlamas family; real Llama-2 sizes need int8 (see the
  [backlog](../../.docs/llama2/enhancements.md)).
- **Sampling:** argmax + temperature only — top-p / top-k are post-v0.1.
- **Tokenizer:** linear vocab scan, no hashmap — correct but O(vocab) per lookup.

Post-v0.1 enhancements (bigger models, top-p/top-k, cross-platform, quantization, training)
are scoped in [`.docs/llama2/enhancements.md`](../../.docs/llama2/enhancements.md).
