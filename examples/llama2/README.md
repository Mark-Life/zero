# llama2.zero

A port of Andrej Karpathy's [llama2.c](https://github.com/karpathy/llama2.c) to **Zero**.
It loads a Llama 2 checkpoint, runs the forward pass on the CPU, and streams the generated
tokens to stdout — a single statically-linked binary with no runtime dependencies.

**Status:** runs Karpathy's TinyLlamas checkpoints (`stories15M`, and the larger `stories42M`
/ `stories110M` with no rebuild) on four targets: `darwin-arm64` (Apple Silicon, native),
`linux-musl-x64` (CI / sandbox), `linux-musl-arm64` (Docker `linux/arm64`, native on Apple
Silicon), and `darwin-x64` (Rosetta 2 on Apple Silicon). Single-threaded f32, argmax /
temperature / top-p / top-k sampling. It also reads int8-quantized checkpoints (the `runq.c`
"version 2" format), auto-detected by magic number — see [Quantized (int8) models](#quantized-int8-models).
Output matches the upstream `llama2.c` **token-for-token** per platform; see
[Validation](#validation). Windows is backlog.

## Quickstart

```sh
# 1. build the Zero compiler (once)
make -C native/zero-c

# 2. build this example -> a static binary for your host
#    macOS (Apple Silicon) -- native, no Docker:
bin/zero build --backend zero-macho64 --emit exe --target darwin-arm64 examples/llama2 --out .zero/out/llama2
#    Linux x86-64 (static musl ELF; CI / Vercel sandbox):
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2
#    Linux ARM64 (static musl ELF; native in Docker on Apple Silicon):
bin/zero build --backend zero-elf-aarch64 --emit exe --target linux-musl-arm64 examples/llama2 --out .zero/out/llama2
#    macOS x86-64 (Mach-O; runs via Rosetta on Apple Silicon):
ZERO_SYSROOT_X86_64_MACOS="$(xcrun --show-sdk-path)" \
  bin/zero build --backend zero-macho-x64 --emit exe --target darwin-x64 examples/llama2 --out .zero/out/llama2

# 3. fetch the pretrained model (~60 MB) and tokenizer
curl -L -O https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin
curl -L -O https://github.com/karpathy/llama2.c/raw/master/tokenizer.bin

# 4. generate
.zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

On Apple Silicon the `darwin-arm64` build is a native Mach-O binary that runs directly,
and the `darwin-x64` build runs natively via Rosetta 2 (also no container). On any other
host run the appropriate Linux binary under a matching container (step 4 only):

```sh
# linux-musl-x64 binary under amd64 container:
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work alpine \
  .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0

# linux-musl-arm64 binary under arm64 container (native on Apple Silicon, qemu elsewhere):
docker run --rm --platform linux/arm64 -v "$(pwd)":/work -w /work alpine \
  .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

## Requirements

- **The Zero compiler** — `make -C native/zero-c` produces `bin/zero`.
- **A target-capable C toolchain** for `libm` linking (the kernels call
  `sqrtf`/`expf`/`powf`/…). `zig` is the simplest option — point `ZERO_CC` at a
  target-capable compiler or rely on the bundled wrapper. On macOS, `zig cc` links
  the system `libSystem` (which provides `libm`); on Linux it links zig's bundled `musl`.
- **Docker** — only needed to *run* the `linux-musl-x64` binary on a non-Linux host.
  Not needed on Apple Silicon, where the native `darwin-arm64` binary runs directly.

## Build

`src/main.0` uses `std.fs` (`mmap`), which falls outside the default self-host exe path,
so the example is built with an explicit backend:

```sh
# macOS / Apple Silicon (native Mach-O arm64):
bin/zero build --backend zero-macho64 --emit exe --target darwin-arm64 examples/llama2 --out .zero/out/llama2

# Linux x86-64 (static musl ELF):
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2

# Linux ARM64 (static musl ELF):
bin/zero build --backend zero-elf-aarch64 --emit exe --target linux-musl-arm64 examples/llama2 --out .zero/out/llama2

# macOS x86-64 (Mach-O, runs under Rosetta on Apple Silicon):
ZERO_SYSROOT_X86_64_MACOS="$(xcrun --show-sdk-path)" \
  bin/zero build --backend zero-macho-x64 --emit exe --target darwin-x64 examples/llama2 --out .zero/out/llama2
```

Four supported targets cover the matrix: `darwin-arm64`, `darwin-x64`, `linux-musl-x64`,
`linux-musl-arm64`. Mach-O builds link `mmap` + `libm` through `libSystem`; ELF builds
link `mmap` through raw Linux syscalls (no libc) and `libm` through zig's bundled `musl`
(via the external-call path). The same `.0` sources compile for all four with no source
change.

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
`--tokens` steps or a BOS delimiter. A bad model path, a malformed or mismatched header,
a non-integer `--tokens`, a missing tokenizer, an over-long prompt, or a failed allocation
each report a distinct `error: …` on stderr and exit nonzero.

```
$ .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 64 --temperature 0
Once upon a time, there was a little girl named Lily. She loved to play outside in the sunshine. One day, she saw a big, red ball in the sky. It was the sun! ...
```

## Models

Every dimension is read from the checkpoint header at runtime, and the activation /
KV-cache buffers are sized from it, so any same-format Karpathy
[TinyLlamas](https://huggingface.co/karpathy/tinyllamas) checkpoint runs **with no rebuild**:

```sh
curl -L -O https://huggingface.co/karpathy/tinyllamas/resolve/main/stories110M.bin
.zero/out/llama2 stories110M.bin --prompt "Once upon a time" --tokens 256
```

`stories42M` (159 MB) and `stories110M` (418 MB) both generate coherent text and match
`llama2.c` **token-for-token**. Larger models are simply slower — single-threaded f32
inference scales ~linearly with parameter count.

### Quantized (int8) models

The engine also reads the `runq.c` **"version 2"** checkpoint format: weights are stored as
int8 with a per-group f32 scale, and the matmul does an exact int32 dot product that is
rescaled to f32 per group. The format is identified by a magic number in the header, so it
is **auto-detected** — the *same* binary and the *same* CLI flags run a quantized checkpoint
with no extra flag, and the quantization group size is read straight from the header.

A quantized checkpoint is produced once with the upstream `karpathy/llama2.c` export tool
(needs PyTorch and the `.pt` weights):

```sh
# one-time export (PyTorch required)
curl -L -O https://github.com/karpathy/llama2.c/raw/master/export.py
curl -L -O https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.pt
python export.py stories15M_q80.bin --version 2 --checkpoint stories15M.pt

# then run it exactly like any other checkpoint (no extra flag)
.zero/out/llama2 stories15M_q80.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

int8 cuts the checkpoint to roughly a quarter of its f32 size, which is what lets the larger
real Llama-2 weights fit in CPU memory: the export and run flow is identical, only the
checkpoint changes.

## Validation

[`validate.sh`](./validate.sh) checks numerical parity against the upstream `llama2.c`.
It fetches the weights, tokenizer, and `run.c`, builds both the Zero exe and the C
reference, and diffs the generated token streams over several prompts. It picks every
fast branch available on the host (auto), or accepts an explicit list via
`LLAMA2_BRANCHES=<csv>`:

| Branch | Zero exe | C reference | Run via | Probe |
| --- | --- | --- | --- | --- |
| `native` | `zero-macho64`, `darwin-arm64` | `zig cc -target aarch64-macos` (`libSystem` libm) | directly on the host | darwin/arm64 host |
| `docker-amd64` | `zero-elf64`, `linux-musl-x64` | `zig cc -target x86_64-linux-musl` (musl libm) | amd64 Alpine container (qemu on non-x64) | `docker` present |
| `docker-arm64` | `zero-elf-aarch64`, `linux-musl-arm64` | `zig cc -target aarch64-linux-musl` (musl libm) | arm64 Alpine container (native on Apple Silicon, qemu elsewhere) | `docker run --platform linux/arm64` returns `aarch64` AND host kernel is arm64 |
| `rosetta` | `zero-macho-x64`, `darwin-x64` | `zig cc -target x86_64-macos` (`libSystem` libm) | natively under Rosetta 2 on Apple Silicon | darwin/arm64 host with Rosetta 2 |

Default selection (auto) on Apple Silicon picks `native` + `docker-arm64` + `rosetta`
(every fast local branch — no qemu). On Linux/x64 it picks `docker-amd64`.
`LLAMA2_FORCE_DOCKER=1` is preserved for backward compat and selects only `docker-amd64`.

```sh
bash examples/llama2/validate.sh
# Apple Silicon host (default auto = native + docker-arm64 + rosetta):
# ==> PASS: llama2.zero matches llama2.c token-for-token on stories15M (f32) across: native docker-arm64 rosetta
```

Both branches build the C reference with the **same toolchain and triple** as the Zero
exe under test — the crux of token-for-token parity. Every `libm` rounds `expf`/`sinf`/…
by ~1 ULP differently, so the reference links the *same* `libm` (zig's `musl` on Linux,
`libSystem` on macOS) and builds with `-ffp-contract=off` to match the backend's scalar
FP (separate multiply then add, never fused). So the bar is **parity per platform**, not
byte-identity *across* them — `libm` and FP-contraction differences make that impossible.

**f32 — gated on every selected branch:**

- **Temperature 0** (greedy argmax) — **byte-identical**.
- **Temperature > 0** (multinomial) — byte-identical once the seed and sampler match
  (`-s 12345 -p 0`); the xorshift\* PRNG is reproduced bit-for-bit.
- **Top-p (nucleus)** — byte-identical with the same seed; the cropped+sorted nucleus
  walk mirrors `sample_topp` exactly.

Raw-byte fallback tokens are emitted as the actual byte on both sides, so streams are
compared directly with `cmp` — no normalization.

**int8 (`runq.c`) — informational.** When a quantized checkpoint (`stories15M_q80.bin`) is
present, `validate.sh` also runs the full parity matrix against `runq.c`: it builds the
`runq_ref` reference exactly the way it builds `run_ref` from `run.c` (same `zig cc` toolchain,
triple, and `-ffp-contract=off`) and diffs the Zero exe — the same auto-detecting binary with
the same flags — against it token-for-token. This matrix is **informational, not gating**: it
prints `OK` / `INFO(diverged)` per case and never changes the script's exit code. The reason is
inherent to int8: the matmul rescales an exact int32 dot product back to f32 once per group, and
that final f32 accumulation differs by about one ULP between Zero's scalar code generation and an
optimizing C compiler's vectorized reduction. At temperature 0 this occasionally flips a near-tied
argmax; with temperature > 0 the multinomial / top-p sampler amplifies it. It is a property of
running int8 through two different code generators, not a defect: byte-for-byte parity with `runq.c`
holds only where the two reductions round identically — exact on the shared-`musl` branches, and
drifting on others (notably native `darwin-arm64`, whose `libSystem` libm differs). The **f32** path
stays byte-for-byte identical to `llama2.c` on every platform and remains the only must-pass gate
(its wide dynamic range absorbs the sub-ULP noise). The int8 matrix self-skips when
`stories15M_q80.bin` is absent (it is a one-time export), so a fresh checkout never fails for lack of it.

CI runs the `docker-amd64` branch (the continuous parity gate, the same `linux-musl-x64`
binary the project ships); the other three branches are local supersets that an Apple
Silicon dev runs with zero extra setup (`native` always; `docker-arm64` needs Docker;
`rosetta` needs Rosetta 2).

**CI without the 60 MB download.** `conformance/native/pass/generate-argmax.0` runs the
full forward → argmax → feedback loop on a tiny synthetic model and asserts the exact
token sequence against a libm C reference; `conformance/native/pass/generate-argmax-q.0` does
the same on a tiny synthetic int8 checkpoint, exercising the v2 loader + int8 matmul path;
`conformance/native/pass/transformer-forward.0` covers a single-step forward in isolation. All
run in the conformance suite without external data.

## How it works

The pipeline mirrors `llama2.c`, split across one flat package:

| File | Role |
| --- | --- |
| `src/main.0` | CLI parsing, `mmap` load, the autoregressive generation loop, token streaming |
| `src/checkpoint.0` | Parse the f32 header → `Config` and map weights as zero-copy `Span<f32>` views (`readConfig` / `mapWeights`), plus the int8 v2 loader (`readConfigV2` / `groupSize` / `mapWeightsQ`) |
| `src/transformer.0` | `RunState` (activations + KV cache), the f32 `forward()` pass, and the int8 `forwardQ()` pass |
| `src/ops.0` | Kernels: f32 `rmsnorm` / `matmul` / `softmax` / `rope` / `swiglu`, plus int8 `matmulQ` / `quantizeActs` / `dequantRow` |
| `src/tokenizer.0` | Karpathy `tokenizer.bin` BPE `encode` / `decode` |
| `src/sampler.0` | `argmax`, temperature / top-p / top-k sampling, the sort they share, xorshift\* PRNG |

Weights are `mmap`'d read-only and sliced into typed `Span<f32>` views with no copy; the
mutable `RunState` is one zeroed anonymous-`mmap` region (sized at runtime from the header).

## Limitations / backlog

- **Targets:** the supported, execution-verified targets are `darwin-arm64`, `darwin-x64`,
  `linux-musl-x64`, and `linux-musl-arm64`. All four pass token-for-token parity vs
  `llama2.c` on `stories15M` (see [Validation](#validation)). Windows (COFF / Win64) is
  **experimental and unvalidated** — the `emit_coff` + `emit_coff_aarch64` backends are
  **build-validated only** (they emit linkable objects that pass build-only smoke fixtures),
  but the generated Win64 machine code has **never been executed or parity-checked**: Windows
  is not a CLI exec target and `main(argc, argv)` args plumbing is not wired. Do not rely on
  the Windows backends for correct runtime behavior.
- **Single-threaded, no SIMD.** The f32 path is scalar single-thread, so larger models
  are simply slower — throughput scales ~linearly with parameter count.
- **int8 parity is reference-tracked but informational, never gated.** Quantized (`runq.c` v2)
  checkpoints run through the same binary (see [Quantized (int8) models](#quantized-int8-models))
  and `validate.sh` diffs them against `runq.c` on every selected branch — but the result only
  prints `OK` / `INFO(diverged)` and never changes the exit code. The int8 matmul's per-group f32
  rescale sits about one ULP from an optimizing C compiler's vectorized reduction, so byte-for-byte
  token parity holds only where the two reductions happen to round identically — i.e. when the Zero
  exe and the C reference link the *same* toolchain `libm` and the rescale order matches. In
  practice it is bit-exact on the shared-`musl` branches and drifts on others (notably native
  `darwin-arm64`, where the platform `libSystem` libm differs), flipping a near-tied argmax at
  temperature 0 and amplifying under the sampler at temperature > 0. This is a property of running
  int8 through two different code generators, not a defect. The **f32** path stays byte-for-byte
  identical to `llama2.c` on every platform and is the only hard, must-pass gate.
- **top-k is fixture-validated, not parity-checked.** It has no upstream `llama2.c`
  reference; every other sampling path (argmax, temperature, top-p) is token-for-token
  against `llama2.c`.
