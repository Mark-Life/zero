# llama2.zero

A port of Andrej Karpathy's [llama2.c](https://github.com/karpathy/llama2.c) to
Zero: load a Llama 2 checkpoint, run forward inference on CPU, stream tokens to
stdout. Single binary, no runtime dependencies. Target: `linux-musl-x64`.

Design docs live under [`.docs/llama2/`](../../.docs/llama2/) — start with
[`overview.md`](../../.docs/llama2/overview.md) and
[`plan.md`](../../.docs/llama2/plan.md).

## Status

**Phase 7 — generation loop (current).** The binary is functionally complete for v0.1: it
parses the command line, `mmap`s the checkpoint, parses its 7×int32 header into a `Config`,
validates the file length, and builds the `TransformerWeights` as zero-copy `Span<f32>`
views (offsets exactly as `llama2.c` `memory_map_weights`, including the shared-classifier
flag); `mmap`s the tokenizer and BPE-encodes the `--prompt`; allocates a zeroed `RunState`
region (anonymous `mmap`, sized from `Config`); then runs the autoregressive generation
loop and streams the decoded tokens to stdout.

`src/main.0` ports `llama2.c` `generate`: starting from the prompt's first token, each step
forwards the transformer at the current position, forces the next prompt token while still
inside the prompt or otherwise samples from the logits, streams the decoded piece
(`world.out.write` over the `Span<u8>` returned by `decode`), and feeds the chosen token
back in. The loop ends at `--tokens` steps (clamped to `seq_len`) or when a BOS (=1)
delimiter is produced. `--tokens` is parsed at runtime by a hand-rolled `parseUsize` (the
int analog of Phase 6's `parseF32`), since `std.parse` folds only literals at compile time.
The direct ELF64 backend has no `break`, so the BOS exit ends the loop by jumping the
position to the step bound and guarding the emit. Like Phase 6, this phase needed no
compiler change — it is ordinary Zero.

`src/transformer.0` adds `RunState` (all activations + the KV cache, carved as
`MutSpan<f32>` sub-views of one `pageAlloc`'d region) and `forward(cfg, weights, state,
token, pos)` — embed → per layer (rmsnorm, QKV matmuls, RoPE, masked multi-head attention
over the KV cache, output projection + residual, SwiGLU FFN + residual) → final rmsnorm →
classifier, ported 1:1 from `llama2.c` and reusing the `src/ops.0` kernels (`rmsnorm`,
`matmul`, `softmax`, `rope`, `swiglu`).

`src/tokenizer.0` adds `buildTokenizer`, `encode`, and `decode`, ported from `llama2.c`'s
BPE: an optional BOS, a dummy space prefix, per-codepoint vocab lookup with byte-fallback,
then greedy highest-score pair merges. The vocab is the Karpathy `tokenizer.bin` format
(`[max_token_length:i32]` then per token `[score:f32][len:i32][bytes]`); lookups are linear
scans over the mapped file (no offset index, no hashmap — slow but correct for v0.1, per
overview #7). `encode` takes BOS only (the 6-int-register call ABI can't fit a 7th argument,
and the v0.1 prompt path is `encode(..., bos=1, eos=0)`); raw-byte `<0xNN>` decode is not
expanded in v0.1.

`src/sampler.0` adds `argmax`, the xorshift* PRNG (`rngNext` / `randomF32`), `sampleMult`,
`parseF32`, and `sample`, ported from `llama2.c` `run.c`: temperature 0 selects the greedy
argmax, otherwise the logits are scaled by `1/temperature`, softmaxed, and sampled from the
resulting distribution with a coin drawn from the PRNG. The direct backend has no bitwise
operators, so the PRNG's shifts become unsigned multiply/divide by powers of two and its XOR
is emulated bit-by-bit (`xor64`) — the 64-bit `imul`/`div` already wrap and truncate
correctly, so `random_u32` reproduces `llama2.c` bit-for-bit (drawn once per token, the
emulation never matters for throughput). `--temperature` is parsed by a hand-rolled
`parseF32` because `std.parse` folds only literals at compile time. top-p / top-k are
deferred (overview #?). No compiler change was needed — this phase is ordinary Zero.

The full forward is covered end-to-end on a tiny synthetic model by
`conformance/native/pass/transformer-forward.0` (numerics checked against a libm C
reference); the kernels also have `math-{matmul,rope,swiglu}-span.0` (plus
`math-{rmsnorm,softmax}-span.0`). The tokenizer's encode/decode is covered by
`conformance/native/pass/tokenizer-encode.0` (a synthetic in-memory vocab, runs in CI). The
sampler primitives are covered libm-free by `conformance/native/pass/sampler-sample.0`
(argmax, the PRNG checked bit-exactly against `llama2.c`, `sampleMult`, and `parseF32` —
runs in CI); the temperature softmax path is exercised end-to-end by the example under
Docker. The generation-loop control flow (prompt forcing, BOS stop, the no-`break` loop
exit) and `parseUsize` are covered libm-free by `conformance/native/pass/generate-loop.0`
(runs in CI); the full load → encode → generate → stream pipeline (libm + fs) is
Docker-validated through the example itself.

## Build

`main.0` now uses `std.fs`, so it falls outside the default self-host exe path
and must build with the direct ELF64 backend:

```sh
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2
```

`linux-musl-x64` is the only supported target for v0.1. On a non-Linux host, run
the produced binary under an amd64 container:

```sh
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work alpine \
  .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256
```

## Run

```
llama2 <model.bin> --prompt <text> --tokens <n> [--temperature <t>] [--tokenizer <path>]
```

| Flag | Default | Notes |
| --- | --- | --- |
| `<model.bin>` | — | Positional. Path to the checkpoint (required). |
| `--prompt` | `""` | Prompt text. BPE-encoded; must be under 1021 bytes for v0.1. |
| `--tokens` | `256` | Number of tokens to generate. Validated as a non-negative integer. |
| `--temperature` | `1.0` | Sampling temperature. `0` selects greedy argmax. |
| `--tokenizer` | `tokenizer.bin` | Path to the Karpathy `tokenizer.bin` vocab. |

Example (streams the prompt continuation; temperature 0 is deterministic argmax):

```
$ llama2 stories15M.bin --prompt "Once upon a time" --tokens 64 --temperature 0
Once upon a time, there was a little girl named Lily. ...
```

The output is the generated text itself: the prompt is re-emitted from its tokens
(the sentencepiece dummy space after BOS is stripped), then the sampled
continuation streams token by token until `--tokens` steps (capped at the model's
`seq_len`) or a BOS delimiter. Writes go straight to stdout, unbuffered. A bad
model path, a sub-header file, a header whose dimensions don't match the file
size, a non-integer `--tokens`, a missing tokenizer, an over-long prompt, or a
failed `RunState` allocation each report a distinct `error: …` on stderr.

## Weights

Fetch Karpathy's pre-trained small model and tokenizer:

```sh
# ~60 MB checkpoint
wget https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin
# tokenizer (from the llama2.c repo)
wget https://github.com/karpathy/llama2.c/raw/master/tokenizer.bin
```

## Notes

- `std.parse.*` is compile-time only, so the runtime numeric flags are parsed by
  hand: `--tokens` by `parseUsize` (an `allDigits` check, then a decimal scan over
  the argument's byte span) and `--temperature` by `parseF32` in `src/sampler.0`.
  Zero has no runtime number *formatter*, but v0.1 needs none — the output is the
  decoded token bytes.
- The PRNG is seeded with a fixed constant, so `--temperature > 0` runs are
  reproducible; `--temperature 0` (argmax) ignores it and is fully deterministic.
- The tokenizer uses linear vocab scans (no offset index, no hashmap) — correct
  but O(vocab) per lookup; a hashmap is post-v0.1. Prompt tokens land in a fixed
  1024-entry stack buffer (the backend has no integer heap region), capping the
  prompt at ~1021 bytes. Raw-byte `<0xNN>` decode tokens are not expanded yet.
