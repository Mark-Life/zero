# llama2.zero — Enhancements Backlog (post-v0.1)

Starter specs for taking llama2.zero past v0.1. Each item is self-contained: goal,
why, current state (with file refs), a concrete approach, effort tier, and the
backend gotchas that bite. Pick one, read its section, go.

Companion to [`plan.md`](./plan.md) (v0.1, complete) and [`overview.md`](./overview.md)
(mission + roadmap). Where the overview's roadmap assumes something is missing, this
doc reflects what actually landed in v0.1 — read the "Current state" lines, not the
overview's aspirations.

## How to use this doc

- Items are ordered by **impressive ÷ effort** (best ratio first). Do them top-down.
- Effort tiers: **Small** (hours–1 day), **Medium** (days), **Large** (1–3 weeks),
  **Very Large** (multi-week, decomposable program of work). # lmao claude you funny
- Each "compiler change" item touches `native/zero-c/src/` and needs: `make -C
  native/zero-c`, a new `conformance/native/{pass,fail}/*.0` fixture wired into
  `conformance/run.mjs`, full conformance green, and Docker amd64 verification.
- "Pure Zero" items touch only `examples/llama2/src/*.0` — no compiler change.

## Backend constraints cheat-sheet (read before writing any `.0`)

The direct ELF64 backend (`--backend zero-elf64`, the only one that builds the fs+libm
example) has hard limits. Violating them gives `CGEN004` or silently-wrong codegen, not
a clean error. From v0.1 phases:

- **No `break` / `continue`.** Use a sentinel: set `pos = bound` to end a `while pos <
  bound` loop, guard the body with `if !stop`.
- **No bitwise operators.** Only `+ - * / %` and logical `&& ||`. `&` is the *borrow*
  operator, not bitwise-AND. Shifts → multiply/divide by powers of two (u64 `imul`
  keeps low 64 bits, no overflow trap; unsigned `div` is exact). XOR → a bit loop (see
  `sampler.0` `xor64`).
- **No `+=`, no unary minus.** Write `a = a + b`, `0 - x`.
- **6-integer-register argument cap** per call. A `Span` costs 2 int regs, a shape ptr 1,
  a `Bool` 1. Design signatures around it (see `tokenizer.0` dropping the EOS arg).
- **One type per local NAME** per function. Two blocks binding `id` as `i32` then `usize`
  resolves the wrong type at the use site (passes `zero check`, fails codegen). Use
  distinct names.
- **Fixed stack arrays must be compile-time sized.** Runtime-sized buffers come from
  `std.mem.pageAlloc` + `bytesAsMut*` reinterpret, not stack arrays.
- **A nested `std.x(...)` call passed directly as a call argument** trips CGEN004. Hoist
  it into a `let` first.
- **Records nest one level only** (a field is scalar / fixed array / `Span`, not another
  record). Raising functions cannot return aggregates. See [`aggregate-abi.md`](./aggregate-abi.md).
- **Verify recipe:** `docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work
  alpine .zero/out/llama2 <args>`. libm fixtures need the zig cross-toolchain
  (`bash scripts/setup-cross-toolchain.sh`).

## Status overview

| # | Item | Impressive | Effort | Kind | Status |
|---|------|-----------|--------|------|--------|
| A | Bigger TinyStories models (42M / 110M) | High | ~Free | Validation | ✅ Done |
| C | Lift prompt-length cap (`bytesAsMutI32`) | Low-Med | Small | Compiler | ✅ Done |
| D | Raw-byte `<0xNN>` decode (u8 span store) | Low* | Low-Med | Compiler | ✅ Done |
| B | top-p / top-k sampling | Med-High | Medium | Pure Zero | ✅ Done |
| F | Faster tokenizer lookup (no linear scan) | Low | Medium | Pure Zero | ✅ Done |
| G | Real Llama-2 + int8 quantization | High | Large | Both | Backlog |
| E | Cross-platform (macOS / Windows / ARM) | High | Med-Large | Compiler | Backlog |
| H | SIMD / multi-threaded matmul (perf) | Medium | Large | Compiler | Backlog |
| T | **Training** | High (risky) | **Very Large** | Both | Backlog (see deep-dive) |

\* D's cosmetic payoff is small, but it unlocks general mutable `u8` buffers — a real
backend capability worth having for the upstream PR.

## Landed this round (A, C, D)

Merged onto `feat/std-math-llama2` (commits `d2893de` = C, `cae4850` = D; A was
validation-only, no code change). Full `conformance` + `docs:test` green; example
Docker-verified token-for-token vs `llama2.c`.

- **A — bigger models.** `stories42M` (159 MB) and `stories110M` (418 MB) run **with no
  code change**, exit clean, and match `llama2.c` token-for-token at temp 0 and temp > 0.
  Speed under amd64 qemu on Apple Silicon: ≈11 / 4.6 / 1.5 tok/s for 15M / 42M / 110M
  (qemu-bound — far faster native). 42M/110M have `seq_len 1024` and are MHA
  (`n_kv_heads == n_heads`), so they confirm runtime-dims but do **not** exercise the GQA
  `kv_mul > 1` branch (see G).
- **C — `bytesAs{,Mut}{I32,U32}` + prompt cap lifted.** New typed reinterprets, no new emit
  code (4-byte ints reuse the GPR index load/store path). The prompt buffer is now a
  `pageAlloc`'d region sized to `cfg.seq_len` via `bytesAsMutI32`; the cap is the model's
  context window, not a fixed 1024. (The old 1024-*byte* cap was actually unsafe for
  stories15M's 256-token context — the new bound is more correct.)
- **D — u8 span store + raw-byte `<0xNN>` decode.** Only the **store** guard
  (`ir.c:3195`) was relaxed; the u8 **load** guard stays (u8 reads already route through
  `IR_VALUE_BYTE_VIEW_INDEX_LOAD`, handled by every backend — relaxing it broke
  darwin-arm64 Mach-O builds, caught by `native:test`). `decode` now emits the real byte;
  `validate.sh` compares with `cmp`, no normalization.

**Env caveat for future runs:** `pnpm run native:test` is red even on the pristine baseline
in bare Alpine/qemu (`direct-rescue-basic` exits 1 vs expected 9 — an emulation artifact; it
targets native-Linux / Vercel-Sandbox CI). `conformance` (also runnable under Docker amd64
with zig) is the reliable gate here and is green.

## Landed next (B)

**B — top-p / top-k sampling.** Pure Zero, no compiler change. `sampler.0` gains a `Sampler`
record (vocab-sized `prob`/`index` scratch + temperature/topp/topk, passed by pointer = one
int reg), an in-place descending **heapsort** over the parallel `(prob, index)` arrays
(`sortDescByProb`; MIN-heap so extract-min-to-back yields descending; index arithmetic only,
no `break`, no parent-index underflow), `sampleTopp` (byte-for-byte port of `run.c`
`sample_topp`), and `sampleTopk` (renormalized top-k prefix draw — no `run.c` reference).
`sample` now takes the `Sampler` record and dispatches argmax → top-k → top-p → multinomial;
with `--topk 0` the top-p branch is identical to `sample_topp`, so parity holds. `main.0`
parses `--topp`/`--topk` (default 0 = off, preserving v0.1 behavior), allocates the two
scratch regions once (`pageAlloc`), and builds the `Sampler` before the loop. Validated by a
new libm-free `conformance/native/pass/sampler-topk-topp.0` (Docker-amd64 green, hand-computed
sort/top-p/top-k asserts) and by `validate.sh`'s new `parity_topp` cases (real `-t`/`-p` vs
`llama2.c`, seed 12345). The shared sort is the prerequisite primitive for item F.

**Gotchas confirmed:** a sampler helper taking both scratch spans + the logits span + a
scalar (top-k's `usize`) is 3 spans + 1 int = 7 int regs → `CGEN004` (the arg cap is 6; 3
spans alone = 6 is fine, cf. `rmsnorm(out,in,w,eps)`). Bundling the scratch into the
`Sampler` record (1 reg) keeps every helper well under the cap — and is needed for `sample`
regardless, since it carries scratch + temp/topp/topk. A bare float literal in an
un-annotated `let` defaults to f64: `let c = (1.0 - topp) / d` is `TYP002` (f64 vs f32) even
though `d - 1.0` is fine (the f32 left operand coerces the literal); bind `let one: f32 =
1.0` first. And the top-p fixture must avoid landing `cumulative` exactly on `topp`
(`0.4f + 0.3f` rounds just *above* `0.7f`), so cases use `topp 0.75` with ≥0.04 margins.

## Landed next (F)

**F — sorted-index tokenizer lookup.** Pure Zero, no compiler change. `buildTokenizer` now takes
one combined `pageAlloc`'d i32 region of `2 * vocab` elements (allocated in `main.0`) and slices
it into `entryOff` (entry byte-offset table, O(1) random access for `tokenStr`/score reads —
killing the v0.1 O(id) `entryStart` walk) and `sortedId` (token ids ordered ascending by string).
A new `sortIdByStr` heapsort (the `sortDescByProb` skeleton from `sampler.0`, but a **max-heap**
over a single id array, keyed by `cmpBytes` over the entry strings) sorts `sortedId` once at load.
`findStr` becomes a binary search; the merge step's whole-vocab `concatMatch` scan becomes
`findStrConcat`, a binary search whose `cmpConcatEntry` comparator compares the virtual `a++b`
**without materializing** it (so no mutable byte scratch is needed and the call stays within the
register cap). `entryStart`/`concatMatch` are deleted. Token ids are unchanged — `validate.sh`
still matches `llama2.c` token-for-token across all argmax/multinomial/top-p cases — and the
rewritten `conformance/native/pass/tokenizer-encode.0` asserts the sorted order plus the same
encode/decode results (Docker-amd64 green). `validate.sh` gained an informational tok/s line.

**Gotchas confirmed:** a **record-returning** call caps at *five* integer arguments, not six —
the hidden return-struct (sret) pointer takes one register — so `buildTokenizer(bytes, eo, so)`
(3 spans = 6) is `CGEN004`; bundling the two indices into one sliced region drops it to two span
args. And **indexing a sliced-span record field directly** (`t.entryOff[i]` where the field is a
sub-slice like `idx[0..vocab]`) mis-resolves the slice base in the direct backend and silently
reads wrong data; bind a local first (`let eo = t.entryOff; eo[i]`), the same shape `findStr`/
`strById` already use. Passing the field as a call argument is fine — only direct indexing breaks.
The string compare/sort needed a new `cmpBytes` (no lexicographic compare exists in `std.mem`).

---

# B. top-p / top-k sampling

**Status: ✅ Landed** (see "Landed next (B)" above for what shipped). The spec below is the
original plan, kept for context.

**Goal.** Match `llama2.c`'s full sampler: argmax (have), temperature multinomial
(have), **top-k** (sample from the k highest-probability tokens), **top-p / nucleus**
(sample from the smallest set whose cumulative probability ≥ p). Closes README
limitation #3 and makes temperature > 0 output genuinely good.

**Why.** Nucleus sampling is what real generation uses; plain multinomial over the full
32k vocab produces worse text. This is the difference between "we have a sampler" and
"we have *the* sampler." Pure Zero, no compiler risk to the validated core.

**Current state.** `examples/llama2/src/sampler.0`:
- `argmax(logits)` — O(vocab) max.
- `softmax` (in `ops.0`) — max-subtract, exp, normalize.
- `sampleMult(probs, coin)` — CDF walk, O(vocab).
- `rngNext` / `randomF32` / `xor64` — xorshift\* PRNG, ported bit-exact.
- `sample(logits, temperature, coin)` — temp 0 ⇒ argmax; else scale by `1/T`, softmax,
  `sampleMult`. Signature is **2 int + 2 float regs** — within the cap; preserve that.

**The one missing primitive: a sort.** There is no sort anywhere in the codebase.
`llama2.c` builds a `ProbIndex[]` (prob + original index), `qsort`s it descending, then
walks it. You need the same.

**Approach.**
1. Allocate two scratch buffers from `pageAlloc`, sized `vocab_size`: an `f32` prob copy
   (`bytesAsMutF32`) and an `i32` index array (`bytesAsMutI32` — **landed in item C**;
   that's why C comes first). Parallel arrays, not a struct (records nest one level and
   a `ProbIndex` array of structs is awkward here).
2. **top-k:** partial selection of the k largest. Simplest correct option: an in-place
   **heapsort** or **selection of top-k** over (prob, index) kept in lockstep. Avoid
   recursion-heavy quicksort (stack-depth + no clean base-case `return` mid-loop).
   Heapsort is index-arithmetic only — fine without bitwise (use `*2`/`/2` for child/parent).
3. **top-p:** sort descending, accumulate probabilities until the running sum ≥ p, then
   `sampleMult` restricted to that prefix (renormalize the prefix or scale the coin).
4. Thread `topp`/`topk` from CLI flags in `main.0` (mirror `--temperature` parsing;
   `parseF32` exists, `parseUsize` exists). Keep `sample`'s register budget — pass the
   two scratch spans, not more scalars, or stash them in a small `Sampler` record.

**Gotchas.**
- No `break`: the CDF-prefix walk and the sort's inner loops need sentinel-bound style.
- The swap in heapsort moves *both* the prob and its index — keep them in lockstep or the
  sampled index is meaningless.
- Reuse the scratch across tokens (alloc once, like RunState) — don't alloc per step.

**Effort.** Medium. The sort is the only real work; everything else mirrors existing code.

**Acceptance.** New `conformance/native/pass/sampler-topk-topp.0` (self-contained, libm-free
if possible — sort + prefix logic on a fixed prob array, assert the selected set). Extend
`validate.sh` to parity-check temp > 0 with top-p against `llama2.c` (drop the `-p 0` it
currently passes, set a real `-t`/`-p`, match the PRNG seed). Token-for-token parity is the
bar — the PRNG is already bit-exact, so a matched top-p path should reproduce exactly.

---

# F. Faster tokenizer lookup (kill the linear scan)

**Status: ✅ Landed** (see "Landed next (F)" above for what shipped — option 1, sort + binary
search, with the merge step also converted via an in-place virtual-concat comparator). The spec
below is the original plan, kept for context.

**Goal.** Replace the O(vocab) linear vocab scan with O(log vocab) or O(1). Closes README
limitation #5.

**Why.** Honesty/polish more than speed — for a short prompt over 32k entries it's a few
×10⁷ ops, imperceptible. It matters if prompts get long or vocab grows (real tokenizers
are 32k–128k). Low user-visible impact ⇒ low priority, but it's contained pure-Zero work.

**Current state.** `examples/llama2/src/tokenizer.0`: tokens are variable-length, stored
back-to-back in the mapping, so `encode`'s merge step and `findStr` **walk the vocab from
the start** every lookup (`entryStart` / `tokenStr` / `findStr`). No offset index, no map.

**Approach (two options).**
1. **Sort + binary search (recommended).** At `buildTokenizer`, build a `pageAlloc`'d
   `i32` array of vocab entry offsets (`bytesAsMutI32` — landed in C), sort it by the
   token *string* (lexicographic `eqlBytes`-style compare extended to `<`). `findStr`
   becomes a binary search. Needs the same sort primitive as item B — **do B first and
   reuse it.** O(vocab log vocab) once at load, O(log vocab) per lookup.
2. **Hashmap.** A `pageAlloc`'d open-addressing table (string hash → entry offset). No
   bitwise ops, so the hash is multiply-add mod table-size (FNV-style with `*` and `%`,
   not XOR/shift). More code than option 1 for the same asymptotics here.

**Gotchas.** String compare/hash walks bytes — fine. The vocab is in a read-only mapping;
the index/table goes in a separate `pageAlloc` region. No `break` in the search loop
(sentinel bound).

**Effort.** Medium. Option 1 is mostly "write a string-keyed sort + bsearch."

**Acceptance.** Output identical to today (same token IDs); a fixture that encodes a known
string and asserts the token sequence; optionally a timing note in `validate.sh`.

---

# G. Real Llama-2 models + int8 quantization

**Goal.** Run a model people actually use — TinyLlama-1.1B or Llama-2-7B — not just
TinyStories. Closes README limitation #2's "tuned for stories15M" framing for real.

**Why.** The headline jump from "toy story generator" to "runs a real LLM." High
impressiveness. But it's a *program of work*, not a quick win — hence below B/F.

**Current state — better than you'd think.**
- All dims are runtime from the checkpoint header (`checkpoint.0` `readConfig`).
- **GQA is implemented** (`transformer.0`: `kv_mul = n_heads / n_kv_heads`, `kvh = (h /
  kv_mul) * head_size`). Real Llama-2-7B uses GQA, so the attention math should already be
  correct — but it has **never run on a grouped checkpoint** (every TinyStories model is MHA,
  `kv_mul == 1`), so verify the `kv_mul > 1` path on the first real grouped export.
- RunState/KV-cache are `pageAlloc`'d and runtime-sized (`mallocRunState`). A 7B KV cache
  (~134 MB) is fine; mmap handles a large weights file lazily.
- Item C's prompt-buffer fix removes the last fixed-size dimension blocker.

**What blocks it — two real things.**
1. **f32 is too big.** Llama-2-7B in f32 is ~28 GB (it ships fp16/quantized). mmap pages
   it lazily, but the working set + scalar single-thread speed make pure f32 impractical.
   The realistic target is **int8 quantization** (`llama2.c`'s `runq.c`, format "v2").
2. **No quantized path.** Needs:
   - `checkpoint.0`: parse the `runq.c` v2 header + per-group scale factors; build
     `QuantizedTensor` views (int8 weights `Span<i8>` + f32 scales `Span<f32>`). Note
     there is **no `Span<i8>`** typed span today — add `bytesAsI8` (trivial reinterpret
     mirror, like item C) or read via existing `u8` and reinterpret sign.
   - `ops.0`: a **quantized matmul** — dequantize-on-the-fly (`w[i] * scale[group]`) in the
     hot loop, plus a `quantize`/`dequantize` for activations (runq quantizes x per group
     too). This is the bulk of the work.
   - `transformer.0`: swap the f32 weight views for `QuantizedTensor`, call the quantized
     matmul. Forward structure is otherwise unchanged.
3. **Model export.** Use Karpathy's `export.py` to produce the `.bin` (don't write a
   quantizer — overview Non-Goals). Document the export command.

**Gotchas.** int8 multiply-accumulate into i32 then scale to f32 — watch the cast path
(`i as i32`/`i32 as f32` are the supported casts). Per-group scales mean the matmul inner
loop indexes a second array; keep it a bare `Span` load (matmul perf, risk #2). `head_size`,
RoPE, attention all already generalize.

**Effort.** Large. GQA being done removes the scariest part; quantized matmul + the v2
loader is the real cost. Sequence: int8 first (4× memory cut), int4 later if wanted.

**Acceptance.** Parity vs `runq.c` on a quantized stories model first (same harness as
Phase 8), then a real quantized TinyLlama/Llama-2-7B producing coherent output under Docker.

---

# E. Cross-platform (macOS / Windows / ARM) — "not only Linux"

**Goal.** Run the binary natively off linux-musl-x64. For *this* repo's owner the prize is
native **macOS arm64** (kills the Docker dependency on your own machine). Closes README
limitation #1.

**Why.** "Single portable binary" is a strong story. But it's the most infrastructure-heavy
item — high value, not low effort.

**Current state — scaffolding exists, llama2 features don't (yet).**
- Zero already has **four backends**: `emit_elf64.c` (x64 Linux, the only complete one),
  `emit_macho64.c` (Mach-O, arm64 — real, hosts the net runtime), `emit_coff.c` (Windows
  x64), `emit_elf_aarch64.c` (Linux ARM64, **MVP — integer-literal returns only**).
- Target table + emitter dispatch: `target.c:11-112` (targets), `target.c:334-353`
  (emitter map). darwin-arm64 → `zero-macho64`; win32-x64 → `zero-coff-x64`.
- **The hard gate:** math (sqrtf/expf/…) is restricted to ELF64-on-Linux by
  `target_math_runtime_supported` (`target.c:438-442`), reason string literally
  *"math runtime is linux-only in this phase."* The llama2 kernels need libm, so this
  gate alone blocks every non-Linux build today.
- **Deeper reality:** every llama2-critical feature — mmap (0a), byte-view reinterpret
  (0c), the aggregate ABI (0d), the typed-slice/STB_GLOBAL fixes (Phase 4) — was
  implemented in `emit_elf64.c` **only**. The other backends report unsupported for these.

**Approach (macOS arm64, the recommended first target).**
1. **Lift the math gate** for macho + Darwin (`target.c:438-452`) and implement the
   **arm64 math-call ABI** — Darwin/AAPCS passes/returns FP in `v0/d0/s0`, *not* x86 XMM;
   the current libm lowering assumes "arg in XMM0, return in XMM0" (`emit_elf64.c:1549+`).
   Link against the system libm in libSystem.
2. **Port the llama2-critical emit paths** to `emit_macho64.c`: mmap (Darwin syscall
   numbers differ — BSD class, `0x2000000` offset), anonymous pageAlloc, byte-view
   reinterpret + typed-span index load/store, the aggregate-ABI (sret/span-in-record).
3. **arm64 instruction selection for the kernels** — the f32 hot loops currently emit SSE2
   (`MOVSS`/`MULSS`/…); arm64 needs NEON/FP equivalents. This is the long pole.
4. Windows (`emit_coff.c`) is a further step: no POSIX syscalls — needs a Win32 API
   shim (CreateFileMapping/MapViewOfFile for mmap) and MSVCRT math.

**Effort.** Medium-Large for native macOS-arm64 llama2; Large for Windows. The scaffolding
(object formats, target table, a real Mach-O backend) means it's *not* a from-scratch
backend — but it's far more than wiring. **Most leveraged first step:** the math gate +
arm64 math ABI (item 1), since it's the single named blocker and unblocks all libm programs,
not just llama2.

**Acceptance.** `bin/zero build --target darwin-arm64 …` produces a binary that runs
stories15M natively on Apple Silicon, token-for-token vs the Linux build (libm ULP caveats
aside). Add a darwin row to the example's CI/validation.

---

# H. SIMD / multi-threaded matmul (performance)

**Goal.** Make it fast. matmul is ~15M f32 loads/token and dominates runtime; today it's
scalar single-thread. Addresses README limitation #2's "single-threaded."

**Why.** Perf, not capability — the output is already correct (Phase 8). Bumps tok/s,
which the validation script already surfaces. overview #6 files this as a known language ask.

**Current state.** `ops.0` `matmul(output, input, weights)` is a scalar triple loop; weight
access is a bare `Span<f32>` load (no per-element bounds check — already the fast shape).
The backend has **no SIMD emission and no threads** (single syscall model, no `clone`).

**Approach (two independent axes).**
1. **SIMD** — a language/backend feature. Either (a) add packed-f32 intrinsics to Zero and
   emit AVX/SSE in `emit_elf64.c` for the matmul inner loop, or (b) auto-vectorize the
   recognized dot-product pattern. (a) is more tractable and reusable. This is a
   `native/zero-c` change, not pure Zero.
2. **Threads** — needs `clone(2)` (or a thread capability) in the backend + a join
   primitive, then parallelize matmul rows across workers (`llama2.c` uses OpenMP here).
   No concurrency primitives exist today; this is a meaningful language addition.

**Effort.** Large for either; SIMD first (single-thread speedup, no concurrency model
needed). Measure before/after with `validate.sh`'s tok/s.

**Acceptance.** Same token output, materially higher tok/s; a benchmark entry (overview #5)
if the single-file bench harness can be extended to the multi-file fs+libm package.

---

# T. Training (the deep-dive)

**Status: Very Large. Not a numbered next-step — a separate program of work.** Kept here
because it may become interesting after the items above. Read this before starting so the
scope is honest.

## What training even means here

v0.1 does **inference**: load fixed weights, forward, sample. Training adds the other half:
compute a loss, backpropagate gradients through every op, and update the weights with an
optimizer — repeated over a dataset for many steps. There is **no reference to port**:
Karpathy's `llama2.c` is inference-only; he trains in PyTorch (`train.py`). So this is an
*original* implementation in Zero, not a transcription. That raises the difficulty and the
impressiveness both.

**Realistic goal:** not "train a good model" (CPU scalar single-thread is far too slow to
train even stories15M to quality — that's GPU-hours). The achievable, still-impressive goal
is **end-to-end training *correctness***: overfit a tiny batch, and gradient-check against
PyTorch on a tiny config for a few steps. "Zero trains a transformer, gradients verified
against PyTorch" is the headline — perf is explicitly not the bar.

## What's already done (reusable for training)

- **Forward pass + all kernels** (`transformer.0`, `ops.0`): rmsnorm, matmul, softmax,
  rope, swiglu. Backprop reuses these and their intermediates.
- **Zeroed writable heap** (`pageAlloc` + `bytesAsMut{F32,I32}`): exactly what gradient
  buffers and optimizer state need (calloc semantics for free).
- **f32 math** (`sqrtf` for AdamW, `expf` for softmax grad, `powf` for bias correction) —
  the optimizer's math is covered.
- **RNG** (`sampler.0` xorshift\*) — weight init, batch shuffling, dropout if wanted.
- **mmap reads + the aggregate ABI** — for the dataset and for passing tensors around.
- **File write exists** (`std.fs.writeAll`) — for checkpoint saving (works on real Linux;
  returns false under qemu amd64 per the 0a notes — test on real Linux/CI).

## What's NOT done (the work)

1. **Writable weights.** Weights are mmap'd `PROT_READ` (`Span<f32>`, immutable by
   design). Training must update them. Simplest with current primitives: at load, **copy**
   weights into a `pageAlloc`'d `MutSpan<f32>` region (`std.mem.copy`), train on that.
   Doubles weight memory (~120 MB for 15M — fine). No compiler change needed. (Alternative:
   expose `mmap` `PROT_WRITE|MAP_PRIVATE` COW — a small backend addition.)
2. **Backward kernels** (`ops.0`, new): the core effort.
   - `matmul_backward` → dW (outer product) and dx (W·dout). The hot path, twice.
   - `rmsnorm_backward`, `swiglu_backward`, `rope_backward` (RoPE rotation is its own
     structured inverse), residual/embedding gradient accumulation.
   - `softmax + cross-entropy` gradient: **`dL/dlogits = softmax(logits) − onehot(target)`**
     — needs only exp/softmax (already have). **No `log` required for the gradient.**
   - attention backward (through the QK·softmax·V chain + KV cache) — the fiddliest.
3. **Loss value (for monitoring).** Cross-entropy loss = `logsumexp(logits) −
   logits[target]`, which needs **`logf`** — and `std.math` has **no `logf`** (it has
   sqrtf/expf/sinf/cosf/powf/absf/floorf/isNaNf). Add `std.math.logf` (trivial mirror of
   `expf`'s libm linking: `checker.c`/`ir.c` registration + `emit_elf64.c` runtime patch +
   a fixture). Note: only the *displayed loss* needs it; gradient descent runs without it.
4. **Optimizer (AdamW).** Per-parameter `m` and `v` state = **2× the parameter memory**, in
   `pageAlloc`'d f32 regions. Update math uses `sqrtf`, `powf` (bias correction), `+ * /`
   — all available. Pure Zero.
5. **Data loader.** Training data is pretokenized `.bin` shards of **uint16** tokens
   (TinyStories). Reading them needs a u16 path — add `bytesAsU16`/`readU16Le` (trivial
   reinterpret mirror, like item C) or assemble from 2 bytes (`hi*256 + lo`, no bitwise
   needed). Then sample random `seq_len` windows (RNG exists). Batching = an outer loop.
6. **Training loop** (new `src/train.0`, or a `--train` mode in `main.0`): for each step —
   sample batch → forward (caching activations) → loss → backward → AdamW step → zero
   grads. The activation cache makes memory bigger than inference; size it from Config via
   pageAlloc. Mind the **no-`break`** rule for the step loop and any early stop.
7. **Checkpoint save** (`checkpoint.0`): write the trained `pageAlloc`'d weights back to a
   `.bin` in the Karpathy format via `std.fs.writeAll`.

## Related files

- `examples/llama2/src/ops.0` — add `*_backward` kernels (biggest delta).
- `examples/llama2/src/transformer.0` — a `backward(...)` mirroring `forward(...)`, plus an
  activation cache in RunState (or a new TrainState).
- `examples/llama2/src/checkpoint.0` — writable-weight load + checkpoint save.
- `examples/llama2/src/{train.0 (new), main.0}` — the training loop / `--train` mode.
- `native/zero-c/src/{checker.c, ir.c, emit_elf64.c}` + `include/zero.h` — `std.math.logf`
  (loss display) and `bytesAsU16`/`readU16Le` (uint16 dataset). Both are trivial mirrors of
  existing ops, not new codegen.
- Reference for the math: PyTorch `train.py` / `model.py` in karpathy/llama2.c (the
  gradients to reproduce), and any minimal-C-backprop transformer for cross-checking.

## A genuine implementation path (phased)

1. **Primitives** (compiler, Small each): `std.math.logf`; `bytesAsU16`/`readU16Le`.
2. **Writable weights** (pure Zero, Small): load-into-pageAlloc + copy.
3. **Loss + softmax/CE backward** (pure Zero, Medium): the simplest gradient end; verify
   `dL/dlogits` numerically vs PyTorch on a 1-token example.
4. **Backward kernels** (pure Zero, Large): matmul → rmsnorm → swiglu → rope → attention,
   gradient-checking each in isolation (finite differences) before composing.
5. **AdamW + train loop** (pure Zero, Medium): overfit a single fixed batch to ~0 loss —
   the canonical "is my backprop correct" test.
6. **Data loader + checkpoint save** (Medium): real shards, save/reload a trained model.
7. **Validation:** gradient parity vs PyTorch on a tiny config (dim 4, 1–2 layers) for a
   few steps; overfit-a-batch curve; a CI fixture mirroring the Phase-8 pattern but for one
   training step with asserted post-step weights.

**Effort.** Very Large, but cleanly decomposable — steps 1–3 are quick and de-risk the rest.
The honest framing for a PR: *correctness-validated training on a tiny model*, not
production training.

---

## Appendix — commands

```sh
# build compiler
make -C native/zero-c

# cross-toolchain for libm linking (zig)
bash scripts/setup-cross-toolchain.sh

# build the example (only backend that builds the fs+libm package)
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2

# run under amd64 Docker (non-Linux host)
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work alpine \
  .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0

# tests (confirm exact script names in package.json)
pnpm run conformance && pnpm run native:test && pnpm run docs:test

# parity vs llama2.c
bash examples/llama2/validate.sh
```

## Connections

- [`plan.md`](./plan.md) — v0.1 implementation (complete), phase notes, backend gotchas.
- [`overview.md`](./overview.md) — mission, original roadmap (v0.2–v0.7), open questions.
- [`aggregate-abi.md`](./aggregate-abi.md) — the direct-backend record/span ABI and its limits.
- [`phase-0-followups.md`](./phase-0-followups.md) — Phase 0 hardening items for upstream.
