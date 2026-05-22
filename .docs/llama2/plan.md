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
- **`std.parse.*` is compile-time only** (folds string *literals* → constants; emits no runtime code — confirmed in Phase 1, `ir.c:~1591`). No runtime number *parser* (int or float) and no number→string *formatter* exist in stdlib. Phase 1 validates `--tokens` via a manual digit scan over `std.mem.span(arg)`; Phase 6 landed an inline pure-Zero `parseF32` in `sampler.0` for `--temperature`→f32; Phase 7 landed the int analog `parseUsize` inline in `main.0` for `--tokens`→int (no `std.parse`/`std.fmt` change). No runtime number *formatter* exists, but v0.1 needs none — output is the decoded token bytes.

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

### Phase 1 — Scaffolding — ✅ DONE (2026-05-21, `feat/std-math-llama2`)

`examples/llama2/{zero.json, README.md, src/main.0}`. `zero.json`:
`{"package":{"name":"llama2","version":"0.1.0"},"targets":{"cli":{"kind":"exe","main":"src/main.0"}}}`.
CLI parse via `std.args.len`/`get` (manual flag scan — no `std.cli`). Print parsed config; exit.

**✅ Landed:** `llama2 <model.bin> --prompt <t> --tokens <n> [--temperature <t>]` — positional
model + a flat flag scan (advance one arg, `std.mem.eql(flag, "--x")`, take the next arg as the
value), echoes the parsed config, exits 0. Builds on the **default `--emit exe` path (no
`--backend`)** — uses only `std.args` + `World`, not `fs`, so unlike every later phase it actually
runs in CI rather than being build-tolerated. Runtime-verified under `docker run --platform
linux/amd64` (full args, defaults, no-args usage, bad `--tokens`); success exits 0 (the harness
skips stdout checks on any nonzero exit). Row added to `examples/README.md`.

**Deviations discovered here (carry into later phases):**

- `std.parse` is comptime-only (see Soft gaps), so `--tokens` is digit-validated by hand, not
  `parseU32`. `--tokens`/`--temperature` stay as raw arg *text* (no runtime number formatter
  either); their numeric forms are produced where consumed — Phase 6 (f32) and Phase 7 (int).
- Direct-backend syntax/codegen limits hit while writing `main.0`: no `else if` (PAR100 — nest, or
  use independent `if`s); `a < b || c > d` mis-parses as a generic call (split each comparison onto
  its own `let`); `String` is **not** a valid user-function parameter type (pass `Span<u8>` via
  `std.mem.span`); call args must be plain locals, not nested calls (`f(g(x))` → hoist `g(x)`);
  falling off `main` after `check world.out.write(...)` leaks that write's return value into the
  process exit code — end the success path with an explicit `return`.

### Phase 2 — `checkpoint.0` — ✅ DONE (2026-05-21, `feat/std-math-llama2`)

`mmap` the file (0a). Parse 7×int32 header → `Config` shape (0b). Build
`TransformerWeights` as `Span<f32>` views into the mapping (0c), offsets computed exactly
as `llama2.c` `memory_map_weights` (shared-classifier flag via sign of `vocab_size`).

**✅ Landed:** `examples/llama2/src/checkpoint.0` with `Config` + `TransformerWeights`
shapes and pure functions `readConfig(bytes) -> Config`, `mapWeights(bytes, cfg) ->
TransformerWeights`, `weightView(bytes, off, count) -> Span<f32>`, and `expectedFileFloats(cfg)
-> usize`. `main.0` now mmaps the model, parses the header, validates the file length against
the header (`expectedFileFloats`), builds the 12 weight views, and prints `checkpoint: ok`.
Offsets match `memory_map_weights` exactly incl. the legacy `freq_cis` skip and the shared
(`vocab_size > 0`) vs. unshared (`< 0`, abs'd) classifier. Runtime-verified under
`docker run --platform linux/amd64` against synthetic shared + unshared checkpoints (size match,
`token_embedding_table` length), plus the three error paths (missing / too-small / size-mismatch).

**Phase 0d (unplanned) — aggregate ABI for the direct ELF64 backend.** Phase 2 surfaced that
the only backend that builds an fs-using program to a runnable `linux-musl-x64` exe (the direct
ELF64 MVP, `--backend zero-elf64`) could not pass **shapes or spans across function boundaries**
— shape params, shape returns, span returns, and shape-with-`Span`-field locals were all rejected
(verified: even an unused such function failed the build; `zero check` accepted the clean code).
The natural `readConfig -> Config` / `mapWeights(cfg) -> TransformerWeights` API was therefore
un-buildable. Rather than contort the code, the backend was extended (the Phase-0 precedent):

- **Shape params** — passed by pointer in one int reg, copied into an inline frame slot on entry
  (value semantics; field access reuses the existing rbp-relative path). `ir.c` + `emit_elf64.c`.
- **Shape returns (sret)** — caller passes the destination address in `rdi` (params shift to
  `rsi…`); the result address rides back in `rax`. `return` accepts a shape literal (fields
  stored through sret), a record local/param (copied through sret), or a record-returning call
  (callee writes straight through our sret). `let x = recordCall(...)` binds via sret into `x`'s
  slot; `let x = p` copies a record.
- **Span returns** — `(ptr, len)` in `rax:rdx`, mirroring the existing two-GPR span *param* ABI.
- **`Span` fields in records** — 16-byte/8-aligned field layout; field load/store split into
  ptr@offset + len@offset+8; construction handles span locals, slices, reinterprets, and
  span-returning calls.
- **Records/literals as arguments** — a record-returning call or shape literal passed as an
  argument (or field-accessed) is materialized into a pre-reserved temp slot and passed by
  pointer, so the natural `f(makeDims(7))` / `area(Dims{…})` work, not just record locals. These
  + return-by-value were generalized in a post-Phase-6 side effort (see "Side effort (after
  Phase 6)" below) so the feature stands alone for upstream
  ([`aggregate-abi.md`](./aggregate-abi.md)); raising aggregate returns remain out of scope.

These are internal-call conventions only (`export c`/`main` unchanged) and gate nothing new.
Covered by `conformance/native/pass/aggregate-shape-abi.0` (runs in CI — no fs) and exercised
end-to-end by the llama2 exe. No conformance/`native:test`/`docs:test` regressions. Because
`main.0` now uses `std.fs`, the example builds **only** via `--backend zero-elf64` (self-host
gate, Note 3) and is `check`-exercised in CI + Docker-validated, not auto-run.

### Phase 3 — `ops.0` (kernels) — ✅ DONE (2026-05-21, `feat/std-math-llama2`)

`rmsnorm`, `matmul`, `softmax`, `rope`, `swiglu` (silu = `x * sigmoid(x)`, sigmoid via
`expf`). Pure functions over `Span<f32>`/`MutSpan<f32>`. `rmsnorm`/`softmax` already
exist as smoke fixtures — lift them in. `matmul` is the hot loop; weights arrive as
`Span<f32>` views (0c).

**✅ Landed:** `examples/llama2/src/ops.0` — five pure kernels over `Span<f32>` /
`MutSpan<f32>`, ported 1:1 from `llama2.c`. Dimensions are read from the span lengths
(no separate size args), so the caller slices the right tensor view and the kernel stays
signature-stable:

- `rmsnorm(output, input, weights, eps)` — output-first to match `llama2.c`'s
  `rmsnorm(o, x, weight, size)` and `matmul`; `eps` (the reference's 1e-5) is the caller's.
- `matmul(output, input, weights)` — `W (d,n) @ x (n,) -> output`, with `d`/`n` from the
  output/input lengths and `weights` the `d*n` row-major view. The hot loop: each weight
  access is a bare `Span<f32>` load (no per-element fat-pointer math), per risk #2.
- `softmax(x)` — **in place** (the attention path needs it), max-subtraction for stability.
- `rope(vec, pos, head_size)` — rotates each (even, odd) pair in place; call once per
  vector (q over `dim`, k over `kv_dim`). Because the angle depends only on
  `idx % head_size`, rotating each vector over its own length reproduces `llama2.c`'s
  `rotn` (q for all i, k only for i < kv_dim) exactly.
- `swiglu(hb, hb2)` — **in place**: `hb[i] = silu(hb[i]) * hb2[i]`, `silu(v)=v/(1+exp(-v))`.

Verification: `matmul` builds + runs end-to-end in Zero (`math matmul span ok`; no libm —
direct ELF, `generatedCBytes == 0`, so it builds even without a cross toolchain).
`rope`/`swiglu` numerics validated against libm (`rope -> [0.5403, 0.8415, -0.0100,
0.99995]`, `swiglu -> [2.1932, 7.0464]`, both inside the fixture bands); they use the same
`std.math` calls (`powf`/`cosf`/`sinf`, `expf`) as the already-green libm fixtures. All
five call signatures type-check in a throwaway package using the exact calls Phase 4's
`transformer.0` will make. New fixtures `conformance/native/pass/math-{matmul,rope,swiglu}-span.0`
wired into `run.mjs` (check + runtime lists; `rope`/`swiglu` `libm: true`, `matmul` not).

**Notes (carry into Phase 4):**

- Signatures are **output-first** and `softmax`/`rope`/`swiglu` are **in place** — the
  forward pass needs in-place attention softmax and the FFN gate combine. This differs
  from the original out-of-place `math-{rmsnorm,softmax}-span.0` smoke fixtures (identical
  math), which stay as-is; the new fixtures cover the genuinely new kernels.
- Because `main.0` does not yet `use ops`, the module is **not** type-checked by
  `zero check examples/llama2` (an unused package module is skipped); its fixtures + Phase
  4's wiring are the coverage. Wiring `ops` into `main.0`/`transformer.0` in Phase 4 brings
  it under the package check.
- Direct-backend quirks applied in `ops.0`: no `+=` (`a = a + b`), no unary minus
  (`0.0 - x`), usize→f32 via `(x as i32) as f32`, `idx % head_size` modulo is fine.

### Phase 4 — `transformer.0` — ✅ DONE (2026-05-21, `feat/std-math-llama2`)

`RunState` fields = `MutSpan<f32>` sub-views of one `pageAlloc`'d, zeroed region
(runtime-sized from Config — mirrors `malloc_run_state`).
`forward(token, pos)`: per layer — rmsnorm → QKV matmuls → RoPE → multi-head attention
vs KV cache → output proj → residual → rmsnorm → SwiGLU FFN → residual; final rmsnorm →
classifier → logits.

**✅ Landed:** `examples/llama2/src/transformer.0` — `RunState` shape (one `MutSpan<u8>`
`base` + ten f32-element offsets), `kvDim`/`runStateFloats`/`mallocRunState` (lay the ten
buffers — x, xb, xb2, hb, hb2, q, att, logits, key_cache, value_cache — over one zeroed
anonymous-`mmap` region), `view(base, off, count)` (writable f32 sub-view via
`bytesAsMutF32(base[off*4 .. (off+count)*4])`), `logitsOf`, and
`forward(cfg, weights, state, token, pos)` ported 1:1 from `llama2.c` `forward`, reusing
the `ops.0` kernels. k/v are sub-views into the KV cache (no separate scratch, matching
current `llama2.c`). `main.0` now `use`s it: after `checkpoint: ok` it allocates the
RunState region, runs one `forward(token=1, pos=0)`, and prints `forward: ok` (the package
is now under `zero check`). Runtime-verified end-to-end via zig+`docker run --platform
linux/amd64`: the standalone fixture matches a libm C reference, and the example runs on a
synthetic checkpoint (`checkpoint: ok` / `forward: ok`, exit 0).

**Three backend fixes Phase 4 required (the "extend the compiler when llama2 needs it"
precedent, cf. Phase 0a–0d):**

1. **Slicing a mutable span yields a mutable span** (`checker.c` `EXPR_SLICE`, both the
   `expr_type` and check-pass arms). Was always `Span<T>`; now `MutSpan<T>` when the base is
   a `MutSpan<T>` (immutable spans/arrays/strings stay `Span<T>`, keeping `PROT_READ` mmap
   views un-writable). Required so a `pageAlloc`'d `MutSpan<u8>` region can be sub-sliced and
   reinterpreted into writable f32 views (`bytesAsMutF32(base[a..b])`) and a typed view can be
   sub-sliced for in-place per-head softmax (`att[lo..hi]`). Safe: `MutSpan<T>` is already
   covariant to `Span<T>` (`types_compatible`).
2. **Typed-span slice start scales by element size** (`ir.c` sets `IR_VALUE_BYTE_SLICE`
   `element_type` from the resolved type; `emit_elf64.c` `elf_emit_byte_view_ptr` shifts the
   start by `log2(elemSize)`). Latent bug: `f32span[1..]` advanced the pointer by 1 byte, not
   4 — no prior fixture sliced a typed (non-`u8`) span at a nonzero offset. `forward` slices
   `f32` weight/att views constantly, so this was on the critical path.
3. **All defined function symbols emit as `STB_GLOBAL`** (`emit_elf64.c` symtab loop). The
   `.symtab` `sh_info` already declared every function symbol global, but non-exported
   functions were emitted local (`0x02`) — a local symbol past `sh_info`, which the ELF spec
   forbids and modern linkers (lld, recent GNU `ld`) reject ("local symbol at index N >=
   sh_info"). This is the root cause of the long-documented "static-musl libm link fails"
   rabbit hole; with it fixed, libm exes (the math-`*`-span fixtures, `transformer-forward`,
   and the fs+libm llama2 example) link via zig and run under Docker locally. Intra-object
   calls are PC-relative (no function-symbol relocations), so the binding is informational.

Fixtures: `conformance/native/pass/transformer-forward.0` (tiny synthetic model, full
forward over two positions, numerics vs a libm C reference; `libm: true`) and
`mem-mut-span-slice.0` (the mutable-slice fix; no libm, runs in CI), both wired into
`run.mjs` (check + runtime lists). `conformance` + `native:test` green; the previously
build-tolerated libm fixtures now actually build+run locally.

### Phase 5 — `tokenizer.0` — ✅ DONE (2026-05-21, `feat/std-math-llama2`)

Load `tokenizer.bin` (Karpathy format: `max_token_length:i32`, then per token
`score:f32`, `len:i32`, bytes). BPE encode prompt; decode token→bytes. Linear vocab
scan for merges (hashmap is post-v0.1 per overview #7).

**✅ Landed:** `examples/llama2/src/tokenizer.0` — `Tokenizer` shape (mapped `bytes` +
`vocab_size` + `max_token_length`) and `buildTokenizer(bytes, vocab_size)`, `encode(t,
text, out, bos) -> usize`, `decode(t, prev, token) -> Span<u8>`, ported from `llama2.c`
`encode`/`decode`. Tokens are variable-length, so accessors **walk the vocab from the
start** (`entryStart`/`tokenStr`/`findStr`) — no offset index, no hashmap (risk #4 /
overview #7): O(vocab) per lookup, a few ×10^7 ops for a short prompt over 32000 entries.
`main.0` now `use`s it: after `checkpoint: ok` it mmaps `--tokenizer` (default
`tokenizer.bin`), BPE-encodes `--prompt` into a fixed `[1024]i32` stack buffer, and prints
`tokenizer: ok`. Covered by `conformance/native/pass/tokenizer-encode.0` (self-contained
synthetic 8-token vocab as a `u8` array literal → **runs in CI**, no fs/libm,
`generatedCBytes == 0`); the example's full path is `--backend zero-elf64` +
Docker-validated end-to-end (`checkpoint: ok` / `tokenizer: ok` / `forward: ok` on a
synthetic checkpoint + matching tokenizer, exit 0).

**Two adaptations to the direct backend (no compiler change needed this phase):**

- **No mutable byte scratch.** `llama2.c` builds each merge candidate by `sprintf`-ing
  `vocab[a]+vocab[b]` into `str_buffer`, then looks it up. `MutSpan<u8>` stores are
  unemitted, so instead `concatMatch(cand, a, b)` compares the two halves of `cand`
  against `a` and `b` **in place** (`eqlBytes` on `cand[0..la]` / `cand[la..]`) — no
  buffer, and `max_token_length` becomes informational.
- **BOS-only `encode`.** The natural `encode(t, text, bos, eos, out)` is 7 integer
  register args (Tok ptr 1 + Span 2 + 2 Bools + MutSpan 2) — over the 6-reg cap. EOS isn't
  used on the v0.1 prompt path (`llama2.c` `run.c` calls `encode(..., bos=1, eos=0)`; EOS
  termination is the generation loop's job), so it's dropped → 6 args.

**Notes / deviations (carry into Phase 6/7):**

- **Prompt token buffer is a fixed `[1024]i32` stack array.** The backend has no integer
  heap region (`bytesAsMut{F32,F64}` exist; there is **no `bytesAsMutI32`**), and stack
  arrays must be compile-time sized — so prompts are capped at ~1021 bytes for v0.1
  (`main.0` guards `len(prompt)+3 > 1024` with an error). Phase 7's generation buffer is
  separate (current token + position; no growing token list needed).
- **Raw-byte `<0xNN>` decode is not expanded.** Emitting a single computed byte needs a
  writable 1-byte buffer the backend can't store into; these fallback tokens are rare in
  argmax story output. Documented limitation.
- Two new direct-backend frontend gotchas surfaced (both worked around in pure Zero, no
  compiler change): **`&` is the borrow operator, not bitwise-AND** (the UTF-8 continuation
  test `(c & 0xC0) == 0x80` is rewritten as the range `c >= 128 && c <= 191`); and **a
  local NAME may hold only one type per function** — two sibling blocks each binding `id`
  with different types (`i32` vs `usize`) makes the backend resolve the wrong type at the
  use site (CGEN004 at the store, though `zero check` passes). Use distinct names.

### Phase 6 — `sampler.0` — ✅ DONE (2026-05-21, `feat/std-math-llama2`)

`argmax(logits)`; temperature path = scale logits, softmax, sample via xorshift PRNG
(pure Zero, port `llama2.c` `random_f32`). top-p/top-k deferred. **Parse `--temperature`
text → f32 here** (Phase 1 kept it raw; `std.parse` is comptime-only, so this needs a runtime
float parser — inline pure-Zero, or a new `std.parse` runtime op). `temperature == 0` ⇒ argmax.

**✅ Landed:** `examples/llama2/src/sampler.0` — `argmax`, the xorshift\* PRNG (`rngNext`
advances the state, `randomF32` reads a draw out of it), `sampleMult` (CDF walk), a runtime
`parseF32`, and `sample` (temperature 0 ⇒ argmax; else scale logits by `1/temperature`,
`softmax` in place, draw with the coin), ported 1:1 from `llama2.c` `run.c`. `main.0` now
`use`s it: after `forward: ok` it parses `--temperature`, draws one coin from a fixed-seed
PRNG, samples a token, and prints `sampler: ok` (the module is now under `zero check`).
Runtime-verified end-to-end via zig+`docker run --platform linux/amd64` on a synthetic
checkpoint+tokenizer for temperature 1.0 (softmax path), 0 (argmax), and 0.5.

**This is the first llama2 phase to need NO compiler change** — ordinary Zero, exactly as the
plan anticipated ("pure Zero"). The one hard constraint surfaced here and was resolved without
touching the backend:

- **The direct ELF64 backend has no bitwise operators.** The parser's only binary operators
  are `|| && == != < <= > >= + - +% +| * / %` (precedence table, `parser.c`); `IrBinaryOp` is
  `ADD/SUB/MUL/DIV/MOD/AND/OR` where `AND`/`OR` are the logical `&&`/`||` (and `&` is the borrow
  operator, not bitwise-AND — Phase 5). So the xorshift port cannot use `^`, `<<`, `>>`. Instead:
  - **shifts → unsigned multiply/divide by powers of two**: `x >> n` is `x / 2^n`, `x << n` is
    `x * 2^n` mod 2^64. Verified the backend's u64 `imul` keeps the low 64 bits with **no
    overflow check** (`emit_elf64.c` emits `48 0F AF C1`, no `jo`/trap) and unsigned `div`
    (`48 31 D2; 48 F7 F1`) is correct, so `random_u32`'s wrapping multiply by
    `0x2545F4914F6CDD1D` and every shift are bit-exact.
  - **XOR → a 64-step bit loop** (`xor64`): each result bit is `(a_bit + b_bit) % 2` scaled by
    its place value. The PRNG is drawn once per token, so the loop never matters for throughput.
  - **u64 → f32 directly** via `cvtsi2ss` with a 64-bit source (`emit_elf64.c` cast path); exact
    for the `<2^24` mantissa `random_f32` produces. (Int→int casts collapse to a retype in
    `ir.c`, so `i as i32` etc. stay free; only float casts emit a `CAST` node.)
- **ABI shape kept inside the tested envelope:** `sample(logits: MutSpan<f32>, temperature: f32,
  coin: f32) -> i32` — 2 int regs + 2 float regs, scalar return, **no sret/float-field combo**.
  State threading is explicit in the caller (`rngNext` then `randomF32`), so each token draws
  exactly one coin — matching `llama2.c` for temperature > 0, harmless for temperature 0
  (argmax ignores the RNG). `MutSpan<f32>` passes to the `Span<f32>` params of `argmax`/`sampleMult`
  via the existing covariance (Phase 4).

Fixture `conformance/native/pass/sampler-sample.0` — a self-contained mirror of the libm-free
primitives (argmax, `rngNext`/`randomF32` checked **bit-exactly** against a `llama2.c` reference
— `rngNext(1)=33554433`, `rngNext(33554433)=1126174793148417`, `randomF32 ≈ 0.2808/0.6711` —
plus `sampleMult` and `parseF32`). No `std.math` ⇒ `generatedCBytes == 0` ⇒ **runs in CI**;
wired into `run.mjs` (check + runtime lists). The temperature `softmax` path (the only
libm-tainted part) is Docker-validated through the example, not the fixture. `conformance` green.

**Gotcha (carry into Phase 7):** a nested std call as an argument —
`parseF32(std.mem.span(temperature))` — trips `CGEN004` "non-identifier callee" (the dotted
`std.mem.span` is not a plain-identifier callee in arg position). Hoist the `std.mem.span` into
its own `let` first, as the rest of `main.0` already does.

### Side effort (after Phase 6) — Aggregate ABI generalized to full record value semantics

Not a numbered phase. After Phase 6 the llama2 pipeline was functionally complete through the
sampler, so we turned to hardening the **direct-backend aggregate ABI** (Phase 0d) for an
upstream PR — it is a *language* change, so "no corners". Phase 0d (born in Phase 2) had shipped
the minimum the checkpoint loader needed and **deferred four restrictions** on how records/spans
cross function boundaries; reviewing the post-Phase-6 tree, three of those were lifted so records
behave as first-class values. Full design + ABI + machine code: [`./aggregate-abi.md`](./aggregate-abi.md).

**Now supported (was rejected before; future phases can use these freely):**

- `return p` — return a record local/param (copied through sret), not just a `return Shape{…}` literal.
- `return f()` — return a record-returning call (the callee writes straight through our sret).
- `let q = p` / `q = p` — record-to-record value copy (`Span` fields ride along).
- `f(makeDims(7))`, `f(Dims{…})` — a record-returning **call** or **shape literal** as an
  argument (materialized into a hidden temp, passed by pointer); also several at once and nested.
- `makeDims(7).field` — field access on a call/literal result.

**Still NOT supported (deliberate — design these around, don't fight them):**

- A record **call/literal argument** is only materialized in straight-line statement positions.
  In a loop condition or a `&&`/`||` right operand it reports `CGEN004` (would mis-evaluate) —
  hoist a `let` yourself (`let p = mk(i)` then `useP(p)`). A plain record **local** as an arg
  works everywhere.
- **Raising functions cannot return aggregates** (#4 — the `rdx` error tag collides with span
  `len`; no consumer yet). Keep aggregate-returning helpers non-raising.
- Records still **nest only one level** (a field is a scalar, fixed array, or `Span` — not
  another record).

Impl: pre-pass reserves record temps before frame layout (so the locals array can't realloc
mid-lowering); `ir.c` + `emit_elf64.c` + `zero.h` (`elf_emit_record_copy_to`,
`elf_emit_record_call_with_dest`, `ir_prepare_record_temps_*`, `ir_lower_record_temp`,
`IrRecordTemp`). Tests: `aggregate-shape-abi.0` (expanded, runs in CI) +
`fail/aggregate-record-arg-while.0`. Clean `-Wall` build; `conformance` / `native:test` /
`docs:test` green; Docker-verified. Uncommitted on `feat/std-math-llama2` with the rest of the tree.

### Phase 7 — `main.0` — ✅ DONE (2026-05-21, `feat/std-math-llama2`)

Wire it: load model + tokenizer → encode prompt → generation loop (forward → sample →
`world.out.write` decoded token → append) until `--tokens` or BOS/EOS. Stream
unbuffered. **Parse `--tokens` text → int here** for the loop bound (Phase 1 only
digit-validated it; needs a runtime int parser, same gap as Phase 6's float).

**✅ Landed:** `examples/llama2/src/main.0` now runs the full v0.1 pipeline —
load+validate checkpoint → mmap+encode tokenizer → allocate RunState → autoregressive
generation loop → stream decoded tokens to stdout, ported 1:1 from `llama2.c` `generate`.
Each step `forward`s at `pos`, forces `prompt_tokens[pos+1]` while `pos+1 < n_prompt` else
draws one coin and `sample`s, then (unless the token is BOS) `decode`s and writes the piece
via `world.out.write`, feeding the token back in. `--tokens` is parsed by an inline
`parseUsize` (the int analog of Phase 6's `parseF32`) and clamped to `seq_len` (0 ⇒
seq_len), matching `llama2.c`'s `steps` clamp. The per-phase stdout smoke markers
(`checkpoint: ok` … `sampler: ok`) are gone — stdout is now the generated token stream; the
error paths still report distinct `error: …` on stderr. Runtime-verified end-to-end via
zig+`docker run --platform linux/amd64` on a synthetic checkpoint+tokenizer: full prompt
echo (BOS dummy-space stripped), deterministic argmax (temperature 0) and sampled
(temperature 0.5/1.0; softmax+libm+PRNG) continuations, BOS termination, `--tokens 0`→seq_len
clamp, and every error path.

**Second llama2 phase to need NO compiler change** (after Phase 6) — ordinary Zero. Two
direct-backend constraints shaped the loop, both handled in pure Zero:

- **`world.out.write` already accepts a runtime `Span<u8>`.** The checker allows `String` *or*
  `Span<u8>` (`checker.c` 3005 path) and the backend lowers the argument through
  `ir_lower_byte_view` (the slice/reinterpret path), so streaming `decode`'s `Span<u8>` piece
  — a view into the live tokenizer mapping — needs no copy and no new op.
- **The direct ELF64 backend has no `break`/`continue`** (neither is lowered in `ir.c`; the
  `break-continue.0` fixture is tolerate-skipped on this backend). The data-dependent BOS exit
  is expressed without `break`: set `pos = steps` to end the `while pos < steps` loop and guard
  the emit with `if stop == false`.

**Notes / ordering fix:**

- **The tokenizer mapping must outlive the loop.** `decode` returns `Span<u8>` views into the
  mmap'd vocab, so the tokenizer `munmap` moved from right after `encode` (Phases 5/6) to after
  the generation loop; the RunState-alloc-failure path now unmaps both mappings.
- Fixed PRNG seed (12345) ⇒ reproducible temperature>0 runs; temperature 0 ignores it.
- Carries Phase 6's gotcha: the `--tokens`/`--temperature` spans are hoisted into `let`s before
  `parseUsize`/`parseF32` (a nested `std.mem.span(...)` arg trips CGEN004).

Fixture `conformance/native/pass/generate-loop.0` — a self-contained, libm-free, fs-free mirror
of the new logic (`parseUsize`, the `seq_len` clamp, and the prompt-force/BOS-stop sequencing
incl. the no-`break` exit), `generatedCBytes == 0` ⇒ **runs in CI**; wired into `run.mjs` (check
+ runtime lists). The full libm+fs pipeline is Docker-validated through the example.
`conformance` + `docs:test` green.

### Phase 8 — Validation — ✅ DONE (2026-05-22, `feat/std-math-llama2`)

Numerical parity vs `llama2.c` on stories15M (same prompt, temperature 0 = deterministic
argmax → exact token stream modulo libm ULPs). Land as a conformance fixture
(`assertDirectRuntimeOrUnsupported`, `libm: true`) and/or `benchmarks/zero/` entry
(overview Open Question #5). Document weight download in README.

**✅ Landed:** the Zero example matches `karpathy/llama2.c` **token-for-token** on the real
60MB `stories15M.bin`. Verified end-to-end via zig + `docker run --platform linux/amd64`
(both binaries link the **same musl libm** — the Zero exe statically via zig, the C reference
compiled under amd64 Alpine — so libm is not a variable):

- **Temperature 0 (greedy argmax):** byte-identical output across prompts (`"Once upon a
  time"`, `"Lily and Tom went to the park"`, `"The little robot"`, `"One day"` @256 tok, and
  the empty prompt = pure BOS generation). The canonical `"Once upon a time"` continuation is
  the expected *"...a little girl named Lily. She loved to play outside in the sunshine..."*.
- **Temperature > 0 (softmax + PRNG + multinomial):** also byte-identical once the seed and
  sampler are aligned — `llama2.c` with `-s 12345 -p 0` (fixed seed; top-p disabled, since
  v0.1 uses plain multinomial) reproduces the Zero stream exactly. This works because the
  xorshift\* PRNG was ported bit-for-bit in Phase 6, so the whole softmax→coin→`sampleMult`
  path matches, not just argmax.
- **One documented deviation (not a numeric/token difference):** raw-byte `<0xNN>` fallback
  tokens are printed literally by v0.1 (Phase 5), where `llama2.c` emits the byte — same token
  *ids*. The harness normalizes `<0xNN>`→byte before diffing. (top-p/top-k remain deferred;
  hence `-p 0` on the reference.)

**Deliverables (Open Q5 "both"):**

- **Reproducible script** `examples/llama2/validate.sh` — fetches weights + tokenizer +
  `run.c`, builds both, runs the parity matrix (5 temp-0 + 3 temp>0 cases), normalizes raw-byte
  tokens, diffs. Prints `PASS: llama2.zero matches llama2.c token-for-token on stories15M.`
  Requires Docker + node (the busybox container has no perl/python, so normalization runs
  host-side via node, a repo dependency).
- **CI conformance fixture** `conformance/native/pass/generate-argmax.0` (`libm: true`) — the
  "small deterministic conformance fixture (tiny token budget)" Open Q5 calls for. A
  self-contained mirror of the example (kernels + `forward` + `argmax` + the `main.0`
  generation loop) on a tiny synthetic model (dim 4 / 1 layer / 2 heads / **seq_len 6 / vocab
  4**), running the full forward→argmax→feedback loop for six steps and asserting the exact
  emitted token sequence `2 3 1 0 2 3` against a libm C reference
  (`.zero/probe/llama2_gen_ref.c`). Wired into `run.mjs` (check + runtime lists). Two design
  points: it uses an **unshared classifier** (`wcls` ≠ `tet`) so the argmax isn't pinned to a
  self-similar fixed point (a shared classifier makes token 0 an attractor → degenerate `0 0 0
  0 0 0`); and the smallest argmax margin is 0.077 (far above libm ULP slack), so the sequence
  is robust. This is the integration the per-component fixtures (`transformer-forward`,
  `sampler-sample`, `generate-loop`) don't cover: forward feeds argmax feeds the KV cache feeds
  forward, producing a specific multi-token stream.
- **No compiler change** — Phase 8 is pure tooling/validation (third llama2 phase needing none,
  after 6 and 7). No conformance/`native:test`/`docs:test` regressions.

**Deferred (not blocking v0.1):** a `benchmarks/zero/` perf entry — the bench harness runs
single `.0` files, but llama2 is a multi-file fs+libm package needing the 60MB model under
`--backend zero-elf64` + Docker, so it doesn't fit the single-file harness. The validation
script already surfaces a perf signal (the reference prints `achieved tok/s`); a formal
benchmark is a post-v0.1 follow-up (overview #6, ties to SIMD).

## File Touchpoints

| File | Phase | Change |
|------|-------|--------|
| `native/zero-c/src/emit_elf64.c` | 0a/0c | `mmap`(file+anon)/`munmap` emit; `Span<u8>`↔`Span<f32>` reinterpret |
| `native/zero-c/src/checker.c` | 0a/0b | register `std.fs.mmap`/`mappingBytes`; wire real `pageAlloc`/`allocBytes`; `readI32Le`/`readU32Le` |
| `native/zero-c/src/ir.c` | 0a/0b/0c | lower the new calls |
| `native/zero-c/src/target.c` | 0a | file mmap (fs-read cap); anon `pageAlloc` (heap cap) |
| `native/zero-c/src/main.c` | 0a/0b | capability-table entries |
| `native/zero-c/include/zero.h` | 0a/0b/0c/2 | new IR ops (mmap, anon alloc, int reads, reinterpret); `IrFunction` record-return fields |
| `native/zero-c/src/{emit_elf64,ir}.c` | 2 | aggregate ABI — shape params/returns, span returns, span-in-record locals ([`aggregate-abi.md`](./aggregate-abi.md)) |
| `docs-site/articles/modules/{fs,mem,codec}.md` | 0a/0b | status entries |
| `conformance/native/{pass,fail}/mmap-*.0`, `page-alloc-*.0`, `codec-read-i32-le.0`, `aggregate-shape-abi.0` | 0a/0b/2 | fixtures |
| `conformance/native/pass/generate-loop.0` | 7 | `parseUsize` + generation control-flow fixture (CI) |
| `conformance/native/pass/generate-argmax.0` | 8 | end-to-end forward→argmax→feedback fixture, exact token seq (CI, libm) |
| `examples/llama2/validate.sh` | 8 | real-stories15M parity vs `llama2.c` (Docker; temp 0 + temp>0) |
| `examples/llama2/zero.json` + `README.md` | 1/2/7/8 | manifest, weight download, `--backend` build, run/stream, validation |
| `examples/llama2/src/main.0` | 1/2/7 | CLI, load, generation loop |
| `examples/llama2/src/checkpoint.0` | 2 | header + weight views |
| `examples/llama2/src/ops.0` | 3 | rmsnorm/matmul/softmax/rope/swiglu |
| `examples/llama2/src/transformer.0` | 4 | RunState + forward pass |
| `examples/llama2/src/tokenizer.0` | 5 | BPE encode/decode |
| `examples/llama2/src/sampler.0` | 6 | argmax/temperature |
| `conformance/run.mjs` | 7/8 | llama2 fixture entries |

## Risks

1. **Anonymous-`mmap` allocator is the new surface.** Making `pageAlloc` real is the bulk of Phase 0. Keep it minimal — a bump region + `munmap`-on-drop (no free-list, no realloc) is enough for v0.1: RunState is allocated once and lives the whole run. Don't gold-plate into a general heap yet.
2. **matmul perf.** ~15M f32 loads/token. With 0c (`Span<f32>` views) these are plain `MOVSS`, no per-element bounds check. The bar is correctness/parity with llama2.c, not speed; SIMD is a later language ask (overview #6). Measure, don't pre-optimize.
3. **mmap cleanup correctness.** `owned<Mapping>` must `munmap` exactly once on every exit path. Model on `owned<File>`/`close`; cover with a fixture that maps + drops in a loop.
4. **Tokenizer linear scan.** O(vocab) per merge step over 32000 entries — slow but correct. Hashmap is post-v0.1.
5. **Numerical parity.** libm pins to musl (F5); reference must be generated against musl. Temperature 0 (argmax) gives a deterministic stream for exact-ish comparison; sampled output only checks distribution/sanity.
6. **f64 absent in `std.math`.** Forward pass is f32 throughout (matches stories15M and llama2.c) — not a blocker, just a constraint to hold.

## Exit Criteria — ✅ all met (v0.1, 2026-05-22)

- ✅ `std.fs.mmap` + real `pageAlloc` land with fixtures green; a program reads f32 from a mapped file and allocates a zeroed multi-MB region at runtime. (Phase 0a)
- ✅ `examples/llama2` builds for `linux-musl-x64` and runs stories15M end-to-end. (Phase 7; verified Phase 8 under Docker)
- ✅ Generated tokens match `llama2.c` (temperature 0) within libm tolerance — in fact byte-identical, and also at temperature > 0 with the seed/sampler aligned. (Phase 8)
- ✅ `pnpm run conformance` / `native:test` / `docs:test` green with new fixtures + docs.
- ✅ README documents weight/tokenizer download, build, run — plus a `validate.sh` parity harness.

## Open Questions

1. **Confirm the proper split: `std.fs.mmap` (weights) + real `pageAlloc` via anonymous `mmap` (RunState).** Matches llama2.c (`mmap`+`calloc`) and Zig (`page_allocator`). Rejected: `.bss` static arrays + `read()` (no v0.2 reuse, dims baked at compile time); `@embed` in `.rodata` (needs an embed feature; 60MB binary); `read()`-into-buffer (doubles memory, no lazy paging). *Recommendation: do the proper split — one `mmap` syscall, two surfaces.*
2. **Capability for the anon allocator.** File `mmap` reuses fs-read. Does anon `pageAlloc` get its own heap/allocator capability (auditable "this program allocates" story in `zero mem --json`), or is it always-on? *Recommendation: its own capability, mirroring how fs/net are gated.*
3. **0b now or byte-shift workaround?** Adding `readI32Le`/`readU32Le` is ~half a day and reusable; the workaround keeps Phase 0 smaller. *Recommendation: add them — trivial mirror of F4, used by both checkpoint and tokenizer.* **✅ Resolved: added (0b done) — checkpoint header + tokenizer fields no longer need byte-shift.**
4. **`pageAlloc` minimalism for v0.1.** Bump-only region (alloc-once, `munmap`-on-drop, no free/realloc) vs. a fuller general allocator. *Recommendation: bump-only — RunState lives the whole run; defer a real heap until a program needs allocation churn.*
5. **Validation home — conformance fixture, `benchmarks/zero/`, or both?** Overview leans benchmark-from-day-one. *Recommendation: both — a small deterministic conformance fixture (tiny token budget) for CI, plus a benchmark entry for perf signal.*
6. **Tokenizer file format.** Karpathy `tokenizer.bin` for v0.1 (simplest), SentencePiece later (overview #3). *Recommendation: Karpathy.*

## Connections

- [`./overview.md`](./overview.md) — mission, roadmap, forcing-function rationale.
- [`./enhancements.md`](./enhancements.md) — post-v0.1 enhancements backlog (bigger models, top-p/top-k, cross-platform, quantization, training); items A/C/D landed.
- [`./phase-0-followups.md`](./phase-0-followups.md) — Phase 0 hardening items to close before upstream PR.
- [`./aggregate-abi.md`](./aggregate-abi.md) — the direct-backend shape/span ABI: born in Phase 2 (record params/returns, span returns, span-in-record locals), generalized after Phase 6 to full record value semantics (return/copy records, calls/literals as args, field-on-call). See "Side effort (after Phase 6)".
- [`../math/plan.md`](../math/plan.md) — the float/math/codec/span foundation this builds on (F1–F5 landed).
