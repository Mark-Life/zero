# llama2.zero — SIMD + multi-threaded matmul plan (enhancements item H)

Executable plan for backlog **item H** ("SIMD / multi-threaded matmul (performance)") in
[`enhancements.md`](./enhancements.md). Goal: make the validated inference engine *fast* —
the output is already correct (Phase 8), this only moves tok/s. Companion to
[`plan.md`](./plan.md) (v0.1, the phase model + backend gotchas), [`cross-platform.md`](./cross-platform.md)
(item E — the scalar arm64 FP layer that NEON/threads build on; it explicitly defers NEON to
H), and [`quantization.md`](./quantization.md) (the int8 `matmulQ` hot path + its parity
caveats). Read the v0.1 backend-gotchas first; this plan assumes them.

## Headline: two axes, opposite parity behavior (read this first)

`matmul` is the hot path — ~15M f32 multiply-accumulates/token, dominating runtime
(`ops.0:51-67`). Today it's **scalar SSE2**: one `MULSS` + one `ADDSS` in `xmm0/xmm1` per
element (`emit_elf64.c:295-311`), with a spill/reload between the two FP sub-expressions
(`emit_elf64.c:1272-1284`) **and a per-element bounds branch** (`cmp/jb/ud2`,
`emit_elf64.c:838-851`). No packed, AVX, or FMA emission exists anywhere
(`enhancements.md:370`; confirmed — the only non-scalar XMM op in the backend is a `MOVAPS`
register *copy*, `emit_elf64.c:243-254`).

Two **independent, multiplicative** axes speed it up:

- **SIMD** — vectorize the *inner* reduction (the dot product over `n`).
- **Threads** — partition the *outer* loop (the `d` output rows) across workers.

**The keystone — they behave oppositely on numeric parity:**

- **Threads are bit-exact.** Each `output[i]` is an independent dot product; a worker that
  owns rows `[lo,hi)` still sums each row `j=0..n` strictly in order. Row-partitioning does
  **not** reorder any reduction ⇒ output is **bit-identical** to the scalar build, *and* to
  the `llama2.c` reference (which validate.sh builds `zig cc -O3 -ffp-contract=off`, **no
  `-ffast-math`** — strictly ordered, `validate.sh:140-151`). Parity is automatic, regardless
  of thread count.
- **SIMD is sub-ULP off.** Lane-parallel partial sums reorder the per-row reduction. FP add
  isn't associative, so the result drifts by sub-ULP from the strictly-ordered reference. The
  wide f32 argmax margins (≥0.077 in the fixture) absorb it at **temp 0** (token-for-token
  holds), but **temp>0 / int8** tight margins can flip a token — `validate.sh:144-146` says
  exactly this about sub-ULP and the int8 path.

So the counterintuitive truth: **threads are the parity-clean win; SIMD is the
parity-perturbing one.** Effort runs the other way — SIMD is contained single-thread codegen;
threads need a concurrency model the language doesn't have yet (no `clone`/`futex`/atomics
anywhere, `enhancements.md:370`).

**Recommended order: SIMD first** (single-thread speedup, no concurrency model, baseline
SSE2 is free on all x86-64), **threads second** (bigger, but bit-exact and scales with
cores). They compose: SIMD the inner loop *and* thread the outer rows.

## Current state (grounded)

| Capability | x64 ELF64 today | Gap for H |
|---|---|---|
| f32 scalar arith | SSE2 `MULSS`/`ADDSS` xmm0/xmm1 (`emit_elf64.c:295-311`), spill/reload per binop (`1272-1284`) | packed `MULPS`/`ADDPS`, then AVX/FMA |
| Span element access | per-element `cmp/jb/ud2` + `lea` + `MOVSS` (`838-851`, `2242`, `3445`, scale `400-414`) | hoist the bounds check out of the hot loop |
| packed/AVX/FMA | **none** (only scalar `MOVAPS` copy `243-254`) | the whole SIMD layer |
| dot-product / reduction primitive | none (open-coded scalar loop in `.0`) | a `std.simd.dotF32` intrinsic |
| syscalls emitted | read 0 / write 1 / close 3 / lseek 8 / mmap 9 / munmap 11 / exit 60 / openat 257 (`mov eax,nr` + `0F 05`, e.g. `2585-2590`) | `clone` 56, `futex` 202 |
| concurrency | **none** — no clone/futex/atomics/locks/TLS anywhere in repo | thread spawn + join |
| heap | anon `mmap` `MAP_PRIVATE\|MAP_ANON` (0x22) per `allocBytes` (`3307-3330`), unlocked | workers must not allocate (no MAP_SHARED needed; CLONE_VM shares the VM) |
| entry/exit | hand-emitted `_start`, `exit`(60) (`4301-4324`); or musl crt0 on the obj+link path (`fs.c:1652`) that llama2 uses for libm | worker child-exit; keep main's exit |
| bench | `validate.sh:361-376` tok/s — coarse (256 tok, temp0, whole-second, incl. load, single run, 15M only, non-gated); no llama2 entry in `benchmarks/zero` (single-file harness; overview #5/#6) | a real before/after measurement |

Kernels (`ops.0`): `matmul` `:51-67` (hot), `matmulQ` `:186-218` (int8 hot), `rmsnorm`
`:27-45` (sum-of-squares = a dot product), `softmax` `:73-96` (`expf`), `rope` `:104-126`,
`swiglu` `:131-145`. matmul/matmulQ dominate; the rest are O(dim) glue.

## Design

### The intrinsic, not auto-vectorization

Add a **`std.simd.dotF32(a: Span<f32>, b: Span<f32>) -> f32`** builtin and lower it **inline**
per backend. matmul becomes one call per row:

```
output[i] = std.simd.dotF32(weights[row..row + n], input)
```

(Slicing a `Span` by computed bounds is supported — item F used sub-slices.) `rmsnorm`'s
sum-of-squares reuses it as `dotF32(x, x)`.

Why an intrinsic, not a pattern-matching auto-vectorizer:
- **Contained codegen** — one emit case per backend, no fragile loop-recognition pass.
- **Reusable + explicit** — it's *the* kernel; expose it once.
- **It hoists the bounds check** — one length check up front instead of `cmp/jb/ud2` per
  element. That alone is a parity-*safe* speedup before any vectorization.

It follows the **inlined-intrinsic template** (the `std.math.isNaNf` shape — single op, no
runtime link), **not** the libm/`expf` template (external symbol + obj+link). Precedent for
span-argument std builtins: `std.mem.len` / `std.mem.copy` / `std.mem.bytesAsF32`.

**Touch-points** (mirrors the libm checklist in the agent-audited intrinsic pattern, *minus*
the obj-link half since this is inlined):

| File | Edit |
|---|---|
| `include/zero.h` | new `IR_VALUE_SIMD_DOTF32` in `IrValueKind` (near `:486`) |
| `checker.c` | three ladders: `std_call_return_type` (~`939`, → `f32`), `std_call_arg_count` (~`1116`, → 2), `std_call_arg_type` (~`1288`, → `Span<f32>`) |
| `ir.c` | `ir_lower_expr` (~`2302`): name→kind, lower the two span args. **Do not** set `direct_math_runtime_import_count` (inlined, no libm) |
| `emit_elf64.c` | one emit case: bounds-check once, emit the (scalar→packed) reduction loop |
| `conformance/run.mjs` + `conformance/native/pass/simd-dotf32.0` | hand-computed fixture, **no** `libm` flag (libm-free ⇒ CI), registered in the check list (~`402`) and the build-run array (~`1978`) |

No `target.c` gate and no `main.c` `ir_value_needs_zero_runtime_object` entry — those are only
for ops that need an external runtime link.

### SIMD codegen ladder

1. **SSE2 packed (baseline, no CPUID).** SSE2 is guaranteed on every x86-64 CPU. 128-bit =
   4×f32: `MOVUPS` (unaligned load), `MULPS` (`0F 59`), `ADDPS` (`0F 58`) into a vector
   accumulator; scalar tail for `n % 4`; horizontal-sum the 4 lanes at the end. Always safe,
   no feature detection.
2. **AVX2 + FMA (follow-on).** 256-bit ymm (VEX) = 8×f32, `VFMADD231PS`. Gate by **runtime
   `cpuid` dispatch** (leaf 1 → AVX/FMA, leaf 7 → AVX2) with the SSE2 path as fallback, so the
   binary stays single + portable. FMA does one rounding (vs the reference's two) → extra
   sub-ULP drift; reconcile with the reference's `-ffp-contract` (see Risks).
3. **NEON (arm64).** Port `dotF32` to `emit_macho64.c` + `emit_elf_aarch64.c` (`FMLA` over
   `v`-registers). Builds directly on item E's scalar arm64 FP (`cross-platform.md` P5 defers
   NEON here on purpose).

### Threads codegen

Parallelize matmul's **outer row loop**: worker `w` owns a disjoint row range, reads the
shared read-only `weights`/`input`, writes its disjoint `output` rows. No write conflicts;
**no allocation inside the kernel** (the allocator is unlocked — workers must never
`pageAlloc`). matmul needs none, so this is safe.

Zero has no closures / function values, so a general `parallelFor(fn, …)` is a large language
addition (overview files general concurrency as a known language ask). The pragmatic scope is
a **backend-emitted parallel-matmul lowering** (the worker body is the emitted dot loop; only
data + range vary) — i.e. parallelize *this kernel*, not "add threads to the language."

Mechanism (x64 Linux):
- `clone(56)` with `CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_CHILD_CLEARTID`.
  Child stack = an anon `mmap` region (reuse the existing mmap emit). Skip `CLONE_SETTLS`
  (Zero has no TLS).
- Post-`clone` split on `rax` (child `rax==0` runs its row range, then `exit(60)` to terminate
  just itself; parent stores the returned child tid).
- **Join via `CLONE_CHILD_CLEARTID` + `futex(202)` `FUTEX_WAIT`** on each child-tid word: the
  kernel atomically clears the word and wakes the parent on child exit. This needs **no
  user-space atomics** — which is essential, the backend emits none.
- Worker count from a `--threads` flag (default 1 = today's behavior) or `nproc`.

Other per-token work (rmsnorm/softmax/attention/rope/swiglu) stays serial — matmul dominates,
so Amdahl is mild, but it bounds scaling. **errno/TLS caveat:** raw-`clone` workers without
`CLONE_SETTLS` share the parent's TLS, so `errno` races. Keep libm calls (`sqrtf`/`expf`) out
of worker bodies — matmul workers do pure mul/add, so they never touch `errno`. (If softmax/
rmsnorm are ever threaded, set up TLS or accept the benign edge-case `errno` race.)

## Phases (ordered by impressive ÷ effort)

### M0 — measurement first (Small, pure harness)
Can't claim "materially higher tok/s" without a real number. Extend `validate.sh`'s tok/s
(`:361-376`): warmup run + N repeats + median, **time only the generation loop** (exclude model
load), report across 15M/42M/110M, keep it informational/non-gated. Record the **baseline**
before any change. (Optional: stand up a `benchmarks/zero` llama2 entry — needs the single-file
`scripts/bench.{mjs,sh}` harness extended to the multi-file fs+libm package, overview #5.)

### S1 — f32 dot-product intrinsic, x64 (Medium)
- **S1a (parity-safe):** `std.simd.dotF32`, **scalar** inner reduction, single bounds check.
  Bit-exact (same order) — proves the intrinsic plumbing end-to-end with **zero** parity risk;
  modest speedup from dropping the per-element branch + cleaner pipelining. Wire `matmul` and
  `rmsnorm` sum-of-squares to it.
- **S1b (ULP):** vectorize the same intrinsic with **SSE2 packed** (4-wide + scalar tail +
  horizontal sum). Re-validate parity: temp-0 token-for-token must still match; re-run temp>0.
- **Acceptance:** `conformance/native/pass/simd-dotf32.0` (hand-computed, libm-free, CI);
  `validate.sh` temp-0 unchanged, temp>0 re-validated; tok/s up vs M0.

### S2 — AVX2 + FMA, x64 (Medium-Large)
`cpuid` runtime dispatch, 8-wide ymm + `VFMADD231PS`, SSE2 fallback. Note the FMA↔reference
`-ffp-contract` interaction. Measure 15M/42M/110M.

### S3 — NEON, arm64 backends (Medium, after E's scalar FP lands)
Port `dotF32` to `emit_macho64.c` + `emit_elf_aarch64.c`. Parity **per platform** (the libm-ULP
caveat from `cross-platform.md` already applies). Native macOS + linux-arm64 tok/s.

### S4 — int8 SIMD for `matmulQ` (optional, later)
`PMADDUBSW`/`PMADDWD` widening MACs. **Gated** on the existing int8 **temp>0** parity
divergence being understood first ([`quant-temp-parity-divergence.md`](./quant-temp-parity-divergence.md)) —
int8 margins are tight (`validate.sh:144-146`). Correctness-first; likely defer.

### T1 — clone/futex thread primitives, x64 (Large)
Emit `clone(56)` + child-stack `mmap` + post-clone split + worker `exit(60)` + `futex(202)`
join via `CLONE_CHILD_CLEARTID`. New IR node(s) + emit case. **Fixture:** spawn N workers that
each write a disjoint word, join, assert all written (libm-free, CI).

### T2 — parallel matmul lowering, x64 (Medium, on T1)
Wire matmul's outer loop to T1: `--threads N`, partition rows, join. Output is
**bit-identical** to scalar (row-partition preserves order) — assert bit-equality vs the
single-thread build. Measure scaling vs core count. **Composes** with S (SIMD inner +
threaded outer).

### T3 — aarch64 threads (Medium)
Same model: `clone` = 220 via `svc`, aarch64 syscall numbers/regs (`emit_elf_aarch64.c`).

### T4 — macOS threads (Large, separate mechanism)
macOS has no stable raw-`clone` ABI — must use libSystem `pthread_create`/`bsdthread_create`
**external calls** (the obj+link mechanism, unlike Linux's raw syscall). A distinct effort;
scoped out of the Linux-first path.

## Measurement & acceptance

- **SIMD:** temp-0 token-for-token preserved (argmax margins ≫ ULP); temp>0 re-validated and
  documented as per-platform/ULP, not bit-identical; **materially higher tok/s** vs the M0
  baseline.
- **Threads:** **bit-identical** token output, independent of `--threads`; tok/s scaling
  ~linear in cores until non-matmul work dominates; fixture proving parallel == scalar
  bit-for-bit.

## Risk register

1. **SIMD breaks bit-exactness.** Sub-ULP reorder; argmax margins absorb it at temp 0, but
   temp>0 / int8 are fragile (`validate.sh:144-146`). *Mitigate:* S1a (scalar, bit-exact)
   first; int8 SIMD gated/deferred (S4); consider a `--strict` scalar fallback for callers who
   need bit-exact.
2. **AVX portability.** Not all x86-64 has AVX2/FMA. *Mitigate:* `cpuid` dispatch keeps one
   portable binary; SSE2 fallback always present.
3. **Threads are net-new and per-backend.** clone/futex (Linux) vs pthread (macOS); no atomics
   exist. *Mitigate:* x64 first; narrow parallel-matmul (not general threads);
   `CLONE_CHILD_CLEARTID` join avoids adding user-space atomics.
4. **Allocator not thread-safe** (unlocked per-call `mmap`). *Mitigate:* workers never
   allocate; matmul doesn't.
5. **errno/TLS race** in raw-clone workers. *Mitigate:* keep libm out of worker bodies (matmul
   = pure mul/add); revisit only if softmax/rmsnorm get threaded.
6. **Reference build flags.** FMA (one rounding) widens drift vs the reference's two-rounding
   `-ffp-contract=off`. *Mitigate:* document; for FMA validation, build the reference with
   matching contraction or accept per-platform parity.
7. **Bench noise** (qemu inflates/obscures). *Mitigate:* measure native; M0 adds warmup +
   repeats + median.
8. **Scope creep to general concurrency.** Explicitly out — parallel-matmul only; general
   `parallelFor`/function-values is a separate language item.

## Open questions

1. **SIMD parity policy:** accept sub-ULP argmax-margin parity (re-validate temp>0), or also
   ship a `--strict` scalar fallback for bit-exact runs?
2. **AVX selection:** runtime `cpuid` dispatch (single portable binary) vs a `--cpu`/target
   build flag (simpler emit, less portable)?
3. **Threads abstraction:** hardcoded parallel-matmul lowering (contained, no frontend change)
   vs a reusable `std.par` primitive (needs function-values — much bigger)?
4. **`--threads` default:** 1 (preserve current behavior) or auto `nproc`?
5. **int8 SIMD (S4):** pursue after the temp>0 bug is root-caused, or leave `matmulQ` scalar?
6. **Bench surface:** extend `validate.sh` only, or also stand up a `benchmarks/zero` llama2
   entry (overview #5)?
7. **Target scope for H now:** x64 only, or commit to arm64 (S3/T3) + macOS (T4) in this round?

## Connections

- [`enhancements.md`](./enhancements.md) — item H spec (the backlog entry this plans) + the
  backend-constraints cheat-sheet.
- [`cross-platform.md`](./cross-platform.md) — item E; its P5 scalar arm64 FP is the substrate
  S3 (NEON) and T3 (arm64 threads) extend; it defers NEON to H by name.
- [`quantization.md`](./quantization.md) — `matmulQ` (S4 target) + the int8 parity caveats;
  [`quant-temp-parity-divergence.md`](./quant-temp-parity-divergence.md) gates S4.
- [`overview.md`](./overview.md) — #5 benchmarking discipline, #6 matmul-as-benchmark (M0).
- [`plan.md`](./plan.md) — v0.1 phases + backend gotchas this assumes.

## Appendix — commands

```sh
# build compiler (every compiler-change phase)
make -C native/zero-c

# cross-toolchain for libm linking (zig) — the example links libm
bash scripts/setup-cross-toolchain.sh

# build the example (only backend that builds the fs+libm package)
bin/zero build --backend zero-elf64 --emit exe --target linux-musl-x64 examples/llama2 --out .zero/out/llama2

# run under amd64 Docker (non-Linux host) — add --threads N once T2 lands
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work alpine \
  .zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0

# tests + parity (the reliable gate is conformance + validate.sh)
pnpm run conformance && pnpm run docs:test
bash examples/llama2/validate.sh
```
