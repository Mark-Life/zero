# llama2.zero — Overview

A port of Andrej Karpathy's `[llama2.c](https://github.com/karpathy/llama2.c)` to Zero. Loads a Llama 2 checkpoint from disk, runs forward inference on CPU, streams tokens to stdout. Single binary, no runtime dependencies.

This is an **example**, not stdlib. It lives under `examples/llama2/` and serves three purposes: a showcase artifact, a stress test for the language and stdlib, and an honest measure of what Zero can do today.

## Mission

Demonstrate that Zero — pre-1, no GC, explicit capabilities, agent-first — can host real LLM inference end-to-end. The reference C implementation is ~800 lines for a reason: the math is dense but the surface is small. A Zero port at comparable size would be the strongest single artifact the language has.

The pitch: *"LLM inference written in an agent-first language. Single binary. No deps. ~1000 lines."*

## Mental Model

llama2.c does five things in sequence:

1. **Load weights** — mmap a checkpoint file (`stories15M.bin` ≈ 60 MB; Llama 2 7B ≈ 27 GB f32) and slice into tensor views.
2. **Tokenize** — byte-pair-encode the prompt using a tokenizer file (`tokenizer.bin`).
3. **Forward pass** — for each layer: RMSNorm → QKV projections + RoPE → attention against the KV cache → output projection → residual → RMSNorm → SwiGLU FFN → residual.
4. **Sample** — pick a next token (argmax, temperature, top-p).
5. **Loop** — append the token, repeat until EOS or token budget exhausted.

Step 3 dominates compute (matmul-heavy). Step 1 dominates memory. Everything else is glue.

A Zero port mirrors this shape exactly. No clever architectural reframing — the structure of `llama2.c` is already minimal.

## What v0.1 Ships

The smallest useful slice. Forward-compatible with everything in the roadmap.

**In:**

- Load `stories15M.bin` (60 MB, the smallest Karpathy-trained model — fits in memory, no mmap strictly required for a first pass).
- BPE tokenizer using Karpathy's `tokenizer.bin` format.
- f32-only forward pass. Single-threaded.
- Sampling: argmax + temperature. No top-p / top-k yet.
- CLI: `llama2 <model.bin> --prompt "<text>" --tokens N [--temperature T]`.
- Linux-musl-x64 only.
- Streams tokens to stdout as they're generated (uses `World.out` — no buffering games).

**Out:**

- Llama 2 7B / 13B / 70B (forced when v0.2 lands mmap + real allocator).
- Quantization (int8/int4).
- Multi-threading.
- GPU.
- Training / fine-tuning.
- Server mode (defer until `std.http` ships, then revisit).
- Alternate architectures (Mistral, Qwen, Phi). Llama 2 only.
- Cross-target. Linux first. Darwin and wasm come after the math is stable.

## What This Forces Into Zero

The point of doing this isn't the demo — it's the language pressure. Each item below is a concrete stdlib or language gap llama2.zero will expose. Many are already on the roadmap; this project would prioritize them.

1. **Large-buffer allocation.** `FixedBufAlloc` is per-request scratch; weights are tens of MB to tens of GB and outlive the program. Need either a large up-front block alloc (`std.mem.allocLargeRegion` or similar) or `std.fs.mmap`. Likely both — mmap for the read-only weights, an allocator for the KV cache.
2. `**std.fs.mmap`.** Cold-start time on a 27 GB model is unacceptable without mmap. Currently a stdlib gap.
3. **f32 math coverage.** Need `expf`, `sqrtf`, `tanhf` (for the GELU variant) or `siluf` directly, fast reciprocal sqrt, and tight loops over `Span<f32>`. Verify what's in `std.math` (if it exists) vs what would need to be authored.
4. **f16/bf16 representation, eventually.** v0.1 stays f32, but real Llama2-7B is shipped in f16 or bf16. Forces decisions about how Zero represents half-precision floats — language feature, not stdlib.
5. **Endianness-aware binary file reading.** llama2.c assumes little-endian everywhere. Need confidence that Zero's binary file reads match — `std.codec` may need to grow `readF32Le`, `readI32Le`, etc.
6. **Inlining and codegen quality for hot loops.** The matmul inner loop is the hot path. The native backend's output for that loop is a real benchmark; this project surfaces optimizer weaknesses Karpathy's gcc-tuned C wouldn't expose otherwise.
7. **Hash map (post-v0.1).** Tokenizer string→id lookup is currently a linear scan over a sorted vocab in llama2.c. A real hashmap in `std.mem` would clean this up — same gap blocking Redis-shaped examples.
8. **CLI parsing ergonomics.** `std.args` exists but composing a CLI with named flags and defaults will stress it. Likely surfaces argument-parsing patterns that could go into stdlib later.

## File Layout

```
examples/llama2/
├── zero.json                  # package manifest, linux-musl-x64 target
├── README.md                  # how to download weights, build, run
└── src/
    ├── main.0                 # CLI entry, loads model, runs loop
    ├── checkpoint.0           # binary file parsing → tensor views
    ├── tokenizer.0            # BPE encode/decode
    ├── transformer.0          # forward pass: RMSNorm, attention, FFN
    ├── ops.0                  # matmul, softmax, rope, silu — the math kernels
    └── sampler.0              # argmax, temperature, top-p (post-v0.1)
```

Roughly mirrors `llama2.c`'s file structure (which is one file because Karpathy was making a point — Zero can split it cleanly).

## Roadmap


| Version  | What                                                                   | What it unblocks                                |
| -------- | ---------------------------------------------------------------------- | ----------------------------------------------- |
| **v0.1** | stories15M.bin, f32, single-threaded, argmax+temperature, linux        | Proves the math. Smallest publishable artifact. |
| v0.2     | mmap + large-region allocator → load real Llama 2 7B                   | First "useful" model. ~3-5 tokens/sec on CPU.   |
| v0.3     | int8 quantization                                                      | 4x memory cut, ~2x faster.                      |
| v0.4     | Multi-threaded matmul (requires Stage B concurrency or worker threads) | Real throughput.                                |
| v0.5     | OpenAI-compatible HTTP server mode (depends on `std.http` server v1)   | Drop-in replacement for local LLM servers.      |
| v0.6     | Darwin-arm64 port                                                      | Apple Silicon perf — important for adoption.    |
| v0.7     | wasm32-web target — browser inference                                  | Demo runnable from a webpage.                   |


Each version is independently shippable. Stop after any of them and have a working program.

## Cross-Target Strategy


| Target         | Status     | Notes                                                                                                           |
| -------------- | ---------- | --------------------------------------------------------------------------------------------------------------- |
| linux-musl-x64 | **v0.1**   | Primary. Direct syscalls via `emit_elf64.c`. Weight loading via read() in v0.1, mmap() in v0.2.                 |
| darwin-arm64   | v0.6       | After Mach-O backend matures. NEON intrinsics would help matmul — separate language ask.                        |
| darwin-x64     | v0.6       | Trails arm64.                                                                                                   |
| wasm32-web     | v0.7       | Browser inference is achievable for small models (stories15M). Loads weights via `fetch`. Big demo opportunity. |
| wasm32-wasi    | After v0.7 | Server-side wasm — interesting for "deploy LLM inference as a wasm module."                                     |


## Non-Goals

Things that look adjacent but aren't on this project's path:

- **Training, fine-tuning, LoRA.** Inference-only.
- **Distributed inference.** Single-machine.
- **Multiple architectures.** Llama 2 first; Mistral / Qwen / Phi / Gemma are post-v1 if at all.
- **Tooling around it.** No model converter, no quantizer-from-scratch (use existing tools, just load their output). Pure inference engine.
- **A library API.** This is a binary, not a stdlib module. If something here generalizes (matmul, RMSNorm), it migrates to `std.tensor` as a *separate* effort once shapes are clear.
- **Beating llama.cpp.** Performance parity with a multi-year, multi-contributor C++ project tuned for every ISA is not the bar. The bar is "works correctly, comparable to llama2.c on the same CPU."

## Open Questions

1. **Float representation strategy.** v0.1 ships f32, but the real models ship in f16/bf16. Does Zero get `f16` as a primitive type, or do we represent it as `u16` and convert in math kernels? Big language design question — affects every numerical program after this.
2. **Allocator API.** v0.1 needs ~60 MB up-front. v0.2 needs ~30 GB. Does `std.mem` grow a `largeRegion(bytes) -> Maybe<owned<Region>>` API, or do we go straight to `std.fs.mmap`? Probably both, but the order matters for v0.1 vs v0.2 sequencing.
3. **Tokenizer file format.** Karpathy's `tokenizer.bin` is custom. SentencePiece `.model` is the industry standard. Pick one. Recommend Karpathy's for v0.1 (simplest), SentencePiece for v0.2.
4. **CLI parser.** Inline parsing or a `std.cli` module? Likely inline for v0.1, then extract if patterns repeat across examples.
5. **Benchmarking discipline.** `benchmarks/zero/` exists. Should llama2 inference be a benchmark fixture from day one? Strongly leaning yes — it's the most useful perf signal the language can have.
6. **Hot-loop codegen.** If the matmul inner loop doesn't lower well, do we add SIMD intrinsics to Zero (language-level), accept the slowdown for v0.1, or write the kernel in C and link it (defeats the "no deps" pitch)? Defer until measured.

## Why This Project (Strategic Argument)

Three reasons llama2.zero is the right showcase:

1. **It matches the language pitch exactly.** Zero is "agent-first." LLM inference is the substrate every agent runs on. A language that can't host inference isn't actually agent-first; it's agent-adjacent.
2. **It's compact enough to finish.** Real Redis is years of work to be honest; a real database is more. llama2.c is 800 lines, well-commented, and the algorithms are public. The Zero port is a 1-3 month project for a working v0.1, not a multi-year commitment.
3. **It surfaces the right language gaps.** The stdlib pressure llama2 creates (allocator, mmap, f32 math, file I/O at scale) is pressure the language needs anyway. Compare: a database forces transactions and recovery (harder, less universally needed); a terminal editor forces termios (specialized). llama2's gaps are *the* gaps every numerical/systems Zero program will hit.

