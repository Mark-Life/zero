# llama2.zero — Implementation Plan (v0.1)

Executable plan for v0.1 of [`overview.md`](./overview.md). Ships `stories15M.bin`
inference on `linux-musl-x64`: f32, single-threaded, argmax+temperature, streaming
tokens to stdout. Builds on the float/math/codec/span work in
[`../math/plan.md`](../math/plan.md) (all landed).

## Goal

`llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 [--temperature 0.9]`
compiles to a single `linux-musl-x64` binary, loads the checkpoint, runs the forward
pass, and streams generated tokens. Numerical output matches `llama2.c` on the same
CPU within libm tolerance.

## State Today (Verified)

**Done — no further work needed (from `../math/plan.md`):**

- f32/f64 arithmetic, compare, casts on ELF x86-64 (SSE2).
- `std.math`: `sqrtf, expf, sinf, cosf, powf, absf, floorf, isNaNf` + `PI_F/E_F/INFINITY_F/NAN_F`. Covers RMSNorm (`sqrtf`), softmax/SwiGLU-sigmoid (`expf`), RoPE (`sinf/cosf`). musl pinned (F5).
- `std.codec.readF32Le/readF64Le(bytes: Span<u8>, offset: usize)` — bit-preserving, bounds-checked (F4).
- Typed `Span<T>`/`MutSpan<T>` params for `T ∈ {u8,i32,u32,i64,u64,f32,f64}` (F3). `[N]f32` array locals + `MOVSS/MOVSD` index load/store (Phase 6). Kernels-over-spans proven: `math-rmsnorm-span.0`, `math-softmax-span.0`.

**Language surface — sufficient (verified in conformance fixtures):**

- `shape` (structs) with scalar/array/`Span` fields, as params + returns → `Config`, `TransformerWeights`, `RunState`.
- `for i in 0..n`, `while`, nested, `if/else`, `break/continue`.
- functions w/ `Span<f32>`/`MutSpan<f32>` params; `enum`/`choice`/`Maybe`.
- flat multi-file package (`src/*.0` + `use name`); byte-strings (`text[i] -> u8`, `text[a..b] -> Span<u8>`) → BPE.
- explicit casts (`as`), shifts, `u8→i32/u32`.

**The one hard blocker — large memory — ✅ RESOLVED in 0a:**

- **No runtime heap.** `emit_elf64.c` emits no `mmap`/`brk`/`malloc`. `std.mem.allocBytes` only services a `FixedBufAlloc` over caller-owned storage (`ir.c:1609`); `pageAlloc`/`generalAlloc` are metadata-only handles that lower to nothing.
- **Fixed arrays are stack-allocated** (`elf_local_offset` → rbp-relative; `sub rsp, frame_bytes` at `emit_elf64.c:3014`). A 60MB array ⇒ 60MB stack frame ⇒ segfault. No `.bss` path for large user buffers.
- ⇒ `stories15M.bin`'s ~60MB weights cannot be made addressable by any current mechanism. **This is the critical path.** Maps to overview "What This Forces" #1/#2 and Open Question #2.

**Soft gaps (workarounds exist):**

- No `readI32Le`/`readU32Le` over runtime spans — checkpoint header is 7×int32, tokenizer has int32 fields. Workaround: assemble from `span[i]` bytes + shifts. Cleaner: add the ops (mirror F4).
- ~~No `Span<u8>` → `Span<f32>` reinterpret~~ — ✅ **resolved in 0c.** `std.mem.bytesAsF32`/`bytesAsMutF32` (+ f64) view weight/scratch byte regions as `f32` with no copy. The asymmetry that forced it (reads had `readF32Le`, **writes had no `writeF32Le`**) is closed: f32 results write through `MutSpan<f32>`.
- No compound assignment (`+=`); use `a = a + b`.

## Sizing (stories15M — all dims compile-time constant)

`dim=288, hidden_dim=768, n_layers=6, n_heads=6, n_kv_heads=6, vocab_size=32000, seq_len=256`.

- Weights ≈ 60MB (token-embedding alone 32000×288×4 = 36.9MB). → **must be mmap'd.**
- RunState ≈ 3.5MB, dominated by KV cache (2×6×256×288×4 = 3.54MB); logits 128KB; rest KB. → **zeroed heap, runtime-sized from Config** (matches llama2.c `calloc`). Stack is the wrong tool: a 3.5MB frame is non-idiomatic and would force dims to compile-time constants.
- Tokenizer vocab ≈ few hundred KB. → mmap or fixed-buffer read.

Forcing fact: weights and RunState want *different* memory (read-only file map vs. zeroed mutable heap) — exactly llama2.c's `mmap` + `calloc` split. In Zero both reduce to one `mmap` syscall (file-backed vs. anonymous).

## How llama2.c (and C/Rust/Zig/Go) handle this

Verified against `karpathy/llama2.c` `run.c`. Memory is split by mutability into two mechanisms:

- **Read-only weights → `mmap`** (zero-copy, lazy-paged):
  `*data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);` then `memory_map_weights` sets tensor pointers *into* the mapping — no copy.
- **Mutable RunState → `calloc`** (zeroed heap, runtime-sized):
  `s->key_cache = calloc(n_layers*seq_len*kv_dim, sizeof(float));` etc.
- **Cleanup:** `munmap(data, file_size); close(fd); free_run_state(...)`.

Same split everywhere: Rust `memmap2` + `Vec`; **Zig `std.heap.page_allocator` — which *is* `mmap`** — + arena (Zero's `pageAlloc`/`arena`/`fixedBufAlloc` API is already Zig-shaped); Go `syscall.Mmap` + `make`. mmap is the rigorous choice, not a shortcut: `read()`-into-buffer doubles memory, stalls cold-start, and can't reach v0.2's 27GB.

Proper Zero translation: implement the `mmap` syscall **once**, expose it **both ways** — file-backed for weights, anonymous for RunState. Anonymous `mmap` returns kernel-zeroed pages = `calloc` semantics for free. This *completes* `std.mem.pageAlloc` (today an empty handle) rather than inventing a parallel path — and resolves overview Open Question #2.

## Phase 0 — Prerequisites (the allocator the language always needed; not llama2 code)

One syscall, two surfaces. `emit_elf64.c` already emits direct syscalls — `mmap` is #9, `munmap` #11.

> **All of Phase 0 has landed and is runtime-verified.** Polish/rigor items found
> in review — to close before an upstream PR — are tracked in
> [`./phase-0-followups.md`](./phase-0-followups.md) (none block llama2 progress).

### 0a — `mmap` foundation — HARD BLOCKER — ✅ DONE (2026-05-20, `feat/std-math-llama2`)

**File-backed (weights), mirror `owned<ByteBuf>`/`bufBytes`:**

```
std.fs.mmap(fs: Fs, path: String) -> Maybe<owned<Mapping>>   // PROT_READ, MAP_PRIVATE
std.fs.mappingBytes(&m: Mapping) -> Span<u8>                  // (ptr, len) view; munmap on drop
```

**Anonymous (RunState) — make `std.mem.pageAlloc` real:**

```
std.mem.pageAlloc() -> PageAlloc                             // now backed by anonymous mmap
std.mem.allocBytes(pageAlloc, n) -> Maybe<MutSpan<u8>>       // MAP_ANONYMOUS → zero-filled (= calloc)
// region munmap'd on cleanup; std.mem.arena layers on top to sub-allocate RunState fields
```

Touchpoints:

- `emit_elf64.c` — emit `mmap` (9) + `munmap` (11); one path shared between file (with fd) and anon (`MAP_ANONYMOUS`, fd=-1). Owned-cleanup hook for `munmap`.
- `checker.c` — register `std.fs.mmap`/`mappingBytes`, `Mapping` owned type; wire `pageAlloc`+`allocBytes` to the real anon path.
- `ir.c` — lower the calls (mirror `std.fs.readAll`/`owned<File>` and the existing `allocBytes`/`FixedBufAlloc` path).
- `target.c` / capabilities — file `mmap` under fs-read capability; anon `pageAlloc` under an allocator/heap capability (Open Question #2).
- Cleanup wiring — `owned<Mapping>` and the page region `munmap` the way `owned<File>` calls `close`.

Fixtures: `mmap-file-readonly.0` (map file, `readF32Le` over it), `mmap-file-notfound.0` (fail path), `page-alloc-region.0` (allocate a multi-page anon region, write+read back, verify zero-init).

Exit: a Zero program (a) maps a file and reads f32 from it, and (b) allocates a zeroed multi-MB region at runtime from `pageAlloc` and writes into it.

**✅ Landed** (ELF64 / `linux-musl-x64`; other backends report unsupported):

- `std.fs.mmap(fs, path) -> Maybe<owned<Mapping>>` and `std.fs.mmapOrRaise(...) -> owned<Mapping>` (the `check` variant — `mmap` alone is the Maybe form; both are needed, mirroring `readAll`/`readAllOrRaise`).
- `std.fs.mappingBytes(&m) -> Span<u8>` (len = file size); `std.fs.munmap(&mut m)` (explicit — see Note 1).
- Real `std.mem.pageAlloc()` + `allocBytes(pageAlloc, n) -> Maybe<MutSpan<u8>>` (kernel-zeroed = calloc). New **deniable `heap` capability** gates `pageAlloc`/`generalAlloc` (Open Q2).
- Fixtures `conformance/native/pass/{page-alloc-region,mmap-file-readonly,mmap-file-notfound}.0` + `conformance/fixtures/mmap-f32le.bin`, wired in `run.mjs`. Docs: `modules/{fs,mem}.md`, `target-capabilities.md`. Runtime-verified via `docker run --platform linux/amd64`.

**Notes for later phases (discovered in 0a — read before 0c / Phase 4 / Phase 8):**

1. **Zero has no implicit drop/RAII.** `owned<T>` is compile-time move-tracking only; `defer`/drop-methods are unsupported in the direct backend. Cleanup is **explicit** — `munmap` mirrors `close` (the "munmap on drop" notes above are aspirational; v0.1 calls `std.fs.munmap` or relies on process-exit). Resolves risk #3 this way.
2. **`MutSpan<u8>` indexed *store* and `std.mem.fill` are NOT emitted by the ELF backend** — only non-`u8` span stores (e.g. `MutSpan<f32>`, proven by rmsnorm) and `std.mem.copy` work; store targets must be a named local, not `maybe.value`. ⇒ **0c (`Span<u8>`→`MutSpan<f32>` reinterpret) is on Phase 4's critical path**: forward-pass kernels must write results through `MutSpan<f32>`, not byte stores. Until 0c, write heap regions via `std.mem.copy` or a typed-f32 span.
3. **Direct-exe self-host gate** (`self_host_caps_allowed`, `main.c`): programs using `fs/time/rand/net/proc/web` are excluded from the default `--emit exe` path and need explicit `--backend zero-elf64`; the conformance harness then **tolerates-skips** them (CGEN004), like every `std-fs-*` fixture. `heap`/`pageAlloc` is deliberately NOT gated, so `page-alloc-region.0` actually runs in CI. ⇒ the llama2 exe (uses `fs`) builds only with `--backend zero-elf64`, and its Phase 8 fixture is build-tolerated, not auto-run — validate it with `--backend` + Docker/sandbox.
4. **Verify recipe:** `docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work alpine ./elf`. `native:test` + libm fixtures need a cross toolchain (`scripts/setup-cross-toolchain.sh` → zig). `std.fs.writeAll` returns false under qemu amd64 (fs write; orthogonal — mmap reads fine), so fixtures use a committed input file rather than writing one.

### 0b — `std.codec.readI32Le` / `readU32Le` (runtime spans) — SOFT — ✅ DONE (2026-05-20, `feat/std-math-llama2`)

Mirror F4 `readF32Le` exactly, but load into a GPR (not XMM): bounds-checked
`MOV r32, [ptr+offset]`. Needed for the int32 header + tokenizer fields; the
byte-shift workaround is viable if we defer this.

Touchpoints: `zero.h` (new IR op or reuse), `checker.c`/`ir.c` lowering, `emit_elf64.c` load, `main.c` capability table, `codec.md`, fixtures `codec-read-i32-le.0`/`codec-read-u32-le.0`.

**✅ Landed** (ELF64 / `linux-musl-x64`; other backends report unsupported, like `readF*Le`):

- `std.codec.readI32Le(bytes: Span<u8>, offset: usize) -> i32` and `readU32Le(...) -> u32`. New IR op `IR_VALUE_BYTE_VIEW_READ_INT_LE` (parallel to `..._READ_FLOAT_LE`, not reused — the GPR vs XMM load is genuinely different). i32/u32 emit identical machine code (`mov eax, [ptr+offset]` = `8B 00`, 4-byte LE load, zero-extended into rax); the IR result type drives later signed/unsigned ops, matching how every 32-bit value lives in the backend. Same bounds-check + `ud2` trap as `readF*Le` (offset checked against `len - 4`).
- Fixtures `conformance/native/pass/{codec-read-i32-le,codec-read-u32-le,codec-read-i32-le-offset}.0` + `conformance/native/fail/codec-read-i32-le-bounds.0`, wired in `run.mjs` (3 lists). Docs row in `modules/codec.md`. `u32` fixture reads `0xFFFFFFFF` (proves high-bit/full-range); offset fixture reads at 0/4/8 incl. `-1` via `0 - 1` (proves signed interpretation; no unary minus in the language). Runtime-verified via `docker run --platform linux/amd64`; `conformance` + `docs:test` green.
- **No capability change**: `readI32Le`/`readU32Le` are `codec`/`target-neutral`, exactly like `readF*Le`. The byte-shift workaround (Open Q3) is now unnecessary for the checkpoint header + tokenizer fields.

### 0c — `Span<u8>` ↔ `Span<f32>`/`MutSpan<f32>` reinterpret slice — REQUIRED — ✅ DONE (2026-05-20, `feat/std-math-llama2`)

Promoted from optional once RunState is heap. Both the mmap'd weights (`Span<u8>` →
`Span<f32>` sub-slices) and the `pageAlloc`'d RunState (`MutSpan<u8>` → `MutSpan<f32>`)
need real typed spans so kernels keep their F3 `Span<f32>` signatures. A typed bitcast of
a fat-pointer view: `(ptr+off, byteLen/4, elem=f32)`. Unaligned `MOVSS` is fine; offsets
are 4-aligned anyway. This is F4's out-of-scope "general bitcast", now on the critical
path. Note the asymmetry that forces it: reads have a fallback (`readF32Le`), but there is
**no `writeF32Le`** — so writing f32 results into a heap region *requires* `MutSpan<f32>`.

**✅ Landed** (ELF64 / `linux-musl-x64`; other backends report unsupported, like `readF*Le`):

- `std.mem.bytesAsF32(bytes: Span<u8>) -> Span<f32>`, `bytesAsF64 -> Span<f64>`, and the
  mutable forms `bytesAsMutF32(bytes: MutSpan<u8>) -> MutSpan<f32>` / `bytesAsMutF64 -> MutSpan<f64>`.
  f64 added for symmetry with `readF{32,64}Le` (nearly free — same path, divisor 8 vs 4).
- New IR op `IR_VALUE_BYTE_VIEW_REINTERPRET` (wraps an inner byte view; `element_type` = f32/f64).
  Lowering recognizes the calls in `ir_lower_byte_view` (same dispatch as `mappingBytes`/`bufBytes`).
  Emit: **ptr is the source ptr unchanged**; **len = source len `shr` log2(elemSize)** (`48 C1 E8 02|03`);
  const-len sources fold (`elf_byte_view_const_len`). Mutable vs immutable is a checker-only
  distinction (both → `IR_TYPE_BYTE_VIEW`), so one IR/emit op serves all four names.
- The result is a normal typed span: indexing scales by elem size and **bounds-checks against the
  divided length** (proven by the fail fixture — index 4 on a 16-byte→len-4 f32 span traps). Sub-slicing
  composes for weight views: `bytesAsF32(weights[off..off + n*4])` (proven in the mmap fixture).
- No capability change — `memory`/`target-neutral`, exactly like `span`/`bufBytes`/`readF*Le` (pure
  pointer math, no alloc, no copy). Added to the `selfHostSubset.spans` helper list in `zero graph --json`.
- Fixtures `conformance/native/pass/{mem-bytes-as-f32,mem-bytes-as-mut-f32,mem-bytes-as-f64,mem-bytes-as-f32-mmap}.0`
  + `conformance/native/fail/mem-bytes-as-f32-bounds.0`, wired in `run.mjs` (3 lists). Docs row +
  "Typed Reinterpret Example" in `modules/mem.md`. Runtime-verified via `docker run --platform linux/amd64`
  (the 3 pageAlloc fixtures run in CI; the fs-gated mmap one is build-tolerated like `mmap-file-readonly`).
  `conformance` + `docs:test` green.

## Phases (llama2 itself — ordinary Zero once 0a lands)

Mirrors the overview file layout under `examples/llama2/src/`.

### Phase 1 — Scaffolding

`examples/llama2/{zero.json, README.md, src/main.0}`. `zero.json`:
`{"package":{"name":"llama2","version":"0.1.0"},"targets":{"cli":{"kind":"exe","main":"src/main.0"}}}`.
CLI parse via `std.args.len`/`get` (manual flag scan — no `std.cli`). Print parsed config; exit.

### Phase 2 — `checkpoint.0`

`mmap` the file (0a). Parse 7×int32 header → `Config` shape (0b or byte-shift). Build
`TransformerWeights` as `Span<f32>` views into the mapping (0c), offsets computed exactly
as `llama2.c` `memory_map_weights` (shared-classifier flag via sign of `vocab_size`).

### Phase 3 — `ops.0` (kernels)

`rmsnorm`, `matmul`, `softmax`, `rope`, `swiglu` (silu = `x * sigmoid(x)`, sigmoid via
`expf`). Pure functions over `Span<f32>`/`MutSpan<f32>`. `rmsnorm`/`softmax` already
exist as smoke fixtures — lift them in. `matmul` is the hot loop; weights arrive as
`Span<f32>` views (0c).

### Phase 4 — `transformer.0`

`RunState` fields = `MutSpan<f32>` sub-slices of one `pageAlloc`'d, zeroed region (arena
bump-allocation, runtime-sized from Config — mirrors `malloc_run_state`).
`forward(token, pos)`: per layer — rmsnorm → QKV matmuls → RoPE → multi-head attention
vs KV cache → output proj → residual → rmsnorm → SwiGLU FFN → residual; final rmsnorm →
classifier → logits.

### Phase 5 — `tokenizer.0`

Load `tokenizer.bin` (Karpathy format: `max_token_length:i32`, then per token
`score:f32`, `len:i32`, bytes). BPE encode prompt; decode token→bytes. Linear vocab
scan for merges (hashmap is post-v0.1 per overview #7).

### Phase 6 — `sampler.0`

`argmax(logits)`; temperature path = scale logits, softmax, sample via xorshift PRNG
(pure Zero, port `llama2.c` `random_f32`). top-p/top-k deferred.

### Phase 7 — `main.0`

Wire it: load model + tokenizer → encode prompt → generation loop (forward → sample →
`world.out.write` decoded token → append) until `--tokens` or BOS/EOS. Stream
unbuffered.

### Phase 8 — Validation

Numerical parity vs `llama2.c` on stories15M (same prompt, temperature 0 = deterministic
argmax → exact token stream modulo libm ULPs). Land as a conformance fixture
(`assertDirectRuntimeOrUnsupported`, `libm: true`) and/or `benchmarks/zero/` entry
(overview Open Question #5). Document weight download in README.

## File Touchpoints

| File | Phase | Change |
|------|-------|--------|
| `native/zero-c/src/emit_elf64.c` | 0a/0c | `mmap`(file+anon)/`munmap` emit; `Span<u8>`↔`Span<f32>` reinterpret |
| `native/zero-c/src/checker.c` | 0a/0b | register `std.fs.mmap`/`mappingBytes`; wire real `pageAlloc`/`allocBytes`; `readI32Le`/`readU32Le` |
| `native/zero-c/src/ir.c` | 0a/0b/0c | lower the new calls |
| `native/zero-c/src/target.c` | 0a | file mmap (fs-read cap); anon `pageAlloc` (heap cap) |
| `native/zero-c/src/main.c` | 0a/0b | capability-table entries |
| `native/zero-c/include/zero.h` | 0a/0b/0c | new IR ops (mmap, anon alloc, int reads, reinterpret) |
| `docs-site/articles/modules/{fs,mem,codec}.md` | 0a/0b | status entries |
| `conformance/native/{pass,fail}/mmap-*.0`, `page-alloc-*.0`, `codec-read-i32-le.0` | 0a/0b | fixtures |
| `examples/llama2/zero.json` + `README.md` | 1 | manifest, weight download |
| `examples/llama2/src/main.0` | 1/7 | CLI, load, loop |
| `examples/llama2/src/checkpoint.0` | 2 | header + weight views |
| `examples/llama2/src/ops.0` | 3 | rmsnorm/matmul/softmax/rope/swiglu |
| `examples/llama2/src/transformer.0` | 4 | RunState + forward pass |
| `examples/llama2/src/tokenizer.0` | 5 | BPE encode/decode |
| `examples/llama2/src/sampler.0` | 6 | argmax/temperature |
| `conformance/run.mjs` | 8 | llama2 fixture entry |

## Risks

1. **Anonymous-`mmap` allocator is the new surface.** Making `pageAlloc` real is the bulk of Phase 0. Keep it minimal — a bump region + `munmap`-on-drop (no free-list, no realloc) is enough for v0.1: RunState is allocated once and lives the whole run. Don't gold-plate into a general heap yet.
2. **matmul perf.** ~15M f32 loads/token. With 0c (`Span<f32>` views) these are plain `MOVSS`, no per-element bounds check. The bar is correctness/parity with llama2.c, not speed; SIMD is a later language ask (overview #6). Measure, don't pre-optimize.
3. **mmap cleanup correctness.** `owned<Mapping>` must `munmap` exactly once on every exit path. Model on `owned<File>`/`close`; cover with a fixture that maps + drops in a loop.
4. **Tokenizer linear scan.** O(vocab) per merge step over 32000 entries — slow but correct. Hashmap is post-v0.1.
5. **Numerical parity.** libm pins to musl (F5); reference must be generated against musl. Temperature 0 (argmax) gives a deterministic stream for exact-ish comparison; sampled output only checks distribution/sanity.
6. **f64 absent in `std.math`.** Forward pass is f32 throughout (matches stories15M and llama2.c) — not a blocker, just a constraint to hold.

## Exit Criteria

- `std.fs.mmap` + real `pageAlloc` land with fixtures green; a program reads f32 from a mapped file and allocates a zeroed multi-MB region at runtime.
- `examples/llama2` builds for `linux-musl-x64` and runs stories15M end-to-end.
- Generated tokens match `llama2.c` (temperature 0) within libm tolerance.
- `pnpm run conformance` / `native:test` / `docs:test` green with new fixtures + docs.
- README documents weight/tokenizer download, build, run.

## Open Questions

1. **Confirm the proper split: `std.fs.mmap` (weights) + real `pageAlloc` via anonymous `mmap` (RunState).** Matches llama2.c (`mmap`+`calloc`) and Zig (`page_allocator`). Rejected: `.bss` static arrays + `read()` (no v0.2 reuse, dims baked at compile time); `@embed` in `.rodata` (needs an embed feature; 60MB binary); `read()`-into-buffer (doubles memory, no lazy paging). *Recommendation: do the proper split — one `mmap` syscall, two surfaces.*
2. **Capability for the anon allocator.** File `mmap` reuses fs-read. Does anon `pageAlloc` get its own heap/allocator capability (auditable "this program allocates" story in `zero mem --json`), or is it always-on? *Recommendation: its own capability, mirroring how fs/net are gated.*
3. **0b now or byte-shift workaround?** Adding `readI32Le`/`readU32Le` is ~half a day and reusable; the workaround keeps Phase 0 smaller. *Recommendation: add them — trivial mirror of F4, used by both checkpoint and tokenizer.* **✅ Resolved: added (0b done) — checkpoint header + tokenizer fields no longer need byte-shift.**
4. **`pageAlloc` minimalism for v0.1.** Bump-only region (alloc-once, `munmap`-on-drop, no free/realloc) vs. a fuller general allocator. *Recommendation: bump-only — RunState lives the whole run; defer a real heap until a program needs allocation churn.*
5. **Validation home — conformance fixture, `benchmarks/zero/`, or both?** Overview leans benchmark-from-day-one. *Recommendation: both — a small deterministic conformance fixture (tiny token budget) for CI, plus a benchmark entry for perf signal.*
6. **Tokenizer file format.** Karpathy `tokenizer.bin` for v0.1 (simplest), SentencePiece later (overview #3). *Recommendation: Karpathy.*

## Connections

- [`./overview.md`](./overview.md) — mission, roadmap, forcing-function rationale.
- [`./phase-0-followups.md`](./phase-0-followups.md) — Phase 0 hardening items to close before upstream PR.
- [`../math/plan.md`](../math/plan.md) — the float/math/codec/span foundation this builds on (F1–F5 landed).
