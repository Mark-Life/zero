# llama2.zero — Cross-platform plan (enhancements item E)

Implementation plan for **item E** of [`enhancements.md`](./enhancements.md): run the llama2
example natively off `linux-musl-x64`. Companion to [`plan.md`](./plan.md) (v0.1, ELF64-only)
and [`aggregate-abi.md`](./aggregate-abi.md) (the record/span ABI being ported here).

Scope of *this* doc: **native macOS arm64** (`darwin-arm64`) — the repo owner's prize, and the
only non-Linux target with a real backend today. Windows (`win32-x64.exe`) and Linux ARM64
(`linux-arm64`) are sketched at the end as separate follow-ons, not in the main plan.

## Goal

```
bin/zero build --backend zero-macho64 --emit exe --target darwin-arm64 \
  examples/llama2 --out .zero/out/llama2
.zero/out/llama2 stories15M.bin --prompt "Once upon a time" --tokens 256 --temperature 0
```

runs stories15M **natively on Apple Silicon** — no Docker, no qemu — and matches the Linux
build token-for-token (modulo libm ULPs; see Risks). Closes README limitation #1.

## The keystone insight (read this first)

On **Linux/ELF**, llama2 leans on two distinct host mechanisms: raw `mmap`/`munmap`
**syscalls** (weights + heap) and **PLT external calls** to libm (`sqrtf`/`expf`/…). Two
mechanisms, two ABIs.

On **macOS**, both collapse into **one**: Apple ships libm *and* the `mmap`/`munmap` POSIX
wrappers in **libSystem**, and raw syscalls are explicitly unsupported (only libSystem is a
stable interface). So everything OS- or math-flavored is "call an external libSystem symbol
with the arm64 AAPCS ABI."

And that mechanism's skeleton **already exists** in `emit_macho64.c`: the net/HTTP/JSON runtime
records call-site patches (`runtime_http_fetch_patches`, `runtime_json_parse_bytes_patches`, …)
and emits Mach-O **external relocations** (`macho_append_call_relocations`, the `r_extern`
bit `1u<<27` at ≈`emit_macho64.c:704`) that the host linker resolves against named symbols.
This is the exact shape of ELF's `elf_record_runtime_math_patch` libm path — already ported.

⇒ The genuinely *new* codegen for macOS llama2 is **floating point**: f32 instruction
selection (NEON/FP) and the FP register ABI (`s0..s7`/`d0..d7`, return in `s0` — not x86 XMM).
mmap/munmap are *integer* external calls — strictly easier than libm and unblocked by the same
work. **FP is the long pole; external-call ABI is the keystone that unblocks libm + mmap + heap
together.**

## Current state (grounded)

What exists vs. what llama2 needs on `darwin-arm64`:

| Capability | ELF64 reference | Mach-O arm64 today | Gap |
|---|---|---|---|
| arm64 integer codegen | n/a | ✅ movz/add/ldr/strb/cmp/bl/b, x0–x7 call ABI | — |
| External-symbol call + reloc | `elf_record_runtime_math_patch` | ✅ for net runtime (`macho_append_call_relocations`, `r_extern`) | generalize to libm + libc |
| Ad-hoc code signing (exe must be signed on Apple Silicon) | n/a | ✅ present (`emit_macho64.c` ≈177–227) | — |
| math gate | open for elf64+linux | ❌ blocked | lift `target_math_runtime_supported` |
| libm calls (sqrtf/expf/sinf/cosf/powf) | `emit_elf64.c` ≈1543–1563 (XMM0/1) | ❌ none | new FP-arg external-call ABI |
| f32 arithmetic (kernels) | SSE2 `elf_emit_xmm_arith` ≈292–306 | ❌ **zero FP support** | NEON/FP instr selection (long pole) |
| mmap file (weights) | `elf_emit_mmap_file_addr_size` ≈2877–2960 (syscalls 257/8/9/3) | ❌ none | call `_mmap`/`_open`/`_close`/`_lseek` |
| anon mmap / pageAlloc | flags `r10=0x22`, init ≈3138 | ❌ rejected (`emit_macho64.c` ≈1321) | call `_mmap` MAP_ANON |
| byte-view reinterpret + typed-span idx load/store | ≈1111 / 2415–2518 | ⚠️ partial (consts only; idx load/store bail ≈1224/1452) | emit indexed load/store, incl. u8 store |
| aggregate ABI (sret, span-in-record, record params/returns) | sret in rdi, 5-int cap ≈3050/3612 | ❌ rejected (scalar/void only ≈1513; byte-view params ≈1519) | sret via **x8**, span fields, record copy |
| 6-int / 5-int(sret) arg cap | `param_regs` ≈1398/1406 | x0–x7 used, no cap enforced | keep cap for ABI parity (see gotchas) |

Targets/dispatch (`target.c`): `darwin-arm64` → object emitter `zero-macho64`
(≈`target.c:334-344`), exe emitter `zero-macho64-exe` (≈346–355) — both already wired. The HTTP
gate already allows `zero-macho64` (≈410–435), proving the host-link + external-call path runs
on this backend. The math gate (≈437–443) is the only `target.c` blocker; every other gap is a
per-feature `macho_diag_at(...)` bail inside `emit_macho64.c`.

## ABI deltas cheat-sheet (x64→arm64, what does NOT carry over)

- **Int args:** x86-64 SysV `rdi,rsi,rdx,rcx,r8,r9` → arm64 AAPCS `x0–x7` (8, not 6). Return `rax`→`x0`.
- **FP args/return:** XMM0–7 → `s0–s7` (f32) / `d0–d7` (f64); return `s0`/`d0`. **This is the libm ABI change.**
- **sret (struct return):** hidden ptr in `rdi` → **`x8`** (AAPCS indirect-result register); does *not* consume an x0–x7 slot.
- **OS interface:** raw `syscall` (rax=#, args rdi/rsi/rdx/r10/r8/r9) → **libSystem external calls** (recommended) or raw `svc #0` (x16=BSD-class #, args x0–x7). See Open Q1.
- **f32 instr:** SSE2 scalar (`MOVSS/ADDSS/MULSS/DIVSS`, prefixes f3/f2) → arm64 `ldr/str s_`, `fmov`, `fadd/fsub/fmul/fdiv s_`, `fcvtzs`/`scvtf`/`ucvtf` casts, `fcmp`. Optional NEON (`v_`, `fmla`) for matmul.
- **Reloc types:** `R_X86_64_PLT32` → Mach-O `ARM64_RELOC_BRANCH26` (calls) / `ARM64_RELOC_PAGE21`+`PAGEOFF12` (data). The macho64 net path already emits the `r_extern` call form.
- **Stack:** 16-byte aligned at call (same intent); Apple ABI packs sub-8-byte stack args tighter than AAPCS — irrelevant while we stay in registers.

## Phased plan

Ordered by leverage. P1 unblocks all libm programs (not just llama2); P1+P2 are integer/FP
external calls sharing one mechanism; P5 (FP kernels) is the long pole. Each "compiler change"
phase needs: `make -C native/zero-c`, a `conformance/native/{pass,fail}/*.0` fixture wired into
`run.mjs`, full conformance green, and — the new bar — **native execution on an Apple Silicon
host** (not Docker).

### P0 — Harness: build + run macho64 exes natively, prove the pipeline

Before any feature, confirm the existing macho64-exe path produces a runnable signed binary on
the host and that the link step can pull libSystem.

- Build a trivial `--backend zero-macho64 --emit exe --target darwin-arm64` program (integer
  return) and run it natively; confirm exit code. Confirms ad-hoc signing + load on this Mac.
- Confirm the exe link resolves an external symbol against libSystem (the net runtime already
  does — verify the link line / `-lSystem`). This is the prerequisite for P1/P2.
- Decide CI story: native macOS runner vs. local-only validation (Open Q5).

**Acceptance.** A hand-written integer `.0` builds + runs natively, exit code correct.

### P1 — Lift the math gate + arm64 libm ABI (the keystone)

**Files:** `target.c` (gate), `emit_macho64.c` (FP external-call ABI), `zero.h` if new patch
arrays need types.

1. **Lift `target_math_runtime_supported`** (`target.c` ≈437–443): allow `zero-macho64` +
   `os==macos`, mirroring how `target_http_runtime_supported` (≈410–435) already lists both
   `zero-macho64` and `zero-elf64`. Update the unsupported reason strings (≈451–452) and the
   "supported" JSON (≈446) to name the macho/libSystem provider.
2. **Emit libm calls on macho64**, mirroring `emit_elf64.c` ≈1543–1563 but with the arm64 ABI:
   - single-arg (`sqrtf/expf/sinf/cosf/fabsf/floorf`): arg in **`s0`**, `bl <placeholder>`,
     return read from **`s0`**.
   - two-arg (`powf`): arg0 `s0`, arg1 `s1`.
   - Record the call patch in a new `runtime_math_*_patches` array (clone the
     `runtime_http_fetch_patch` plumbing ≈329–364/652–658) and append an external relocation to
     `_sqrtf`/`_expf`/… via the existing `macho_append_call_relocations` shape (≈704). Symbol
     names are the underscore-prefixed Mach-O form (`_expf`).
   - This requires the **minimum FP support**: load an f32 value into `s0`/`s1` and move the
     result out. Full FP arithmetic is P5; here we only need value-in / value-out.

**Gotcha.** macho64 has *no* FP instruction emit today — P1 must add `ldr s_, [..]` /
`fmov`/`str s_` helpers (the seed of P5). Keep them minimal; P5 extends.

**Acceptance.** `conformance/native/pass/macho-math-sqrtf.0` (or lift an existing libm fixture's
coverage to macho) builds + runs natively, result within ULP of the C/musl value. Mirrors the
ELF math fixtures.

### P2 — mmap/munmap + pageAlloc via libSystem (weights + heap)

**Files:** `emit_macho64.c`. Reuse P1's external-call mechanism — these are **integer** args, no
FP, so strictly simpler than libm.

1. **File mmap** (weights): port `elf_emit_mmap_file_addr_size` (≈`emit_elf64.c:2877-2960`) but
   instead of syscalls 257/8/9/3, emit `bl` to libSystem `_open`(`O_RDONLY`=0), `_lseek`(
   `SEEK_END`=2), `_mmap`(`PROT_READ`=1, `MAP_PRIVATE`=2, fd, 0), `_close`. Args/return in
   x0–x7/x0. Record external relocs as in P1.
   - Darwin mmap flag constants differ from Linux: `MAP_PRIVATE`=0x2 (same), `MAP_ANON`=**0x1000**
     (Linux 0x20), `PROT_READ|WRITE`=0x3 (same).
2. **Anonymous pageAlloc** (RunState heap): replace the bail at `emit_macho64.c` ≈1321 with a
   `_mmap(NULL, n, PROT_READ|WRITE=3, MAP_ANON|MAP_PRIVATE=0x1002, -1, 0)`. Kernel-zeroed =
   calloc semantics (same contract as ELF). Wire the `IR_VALUE_PAGE_ALLOC` local-init the way
   ELF does (≈`emit_elf64.c:3138`).
3. **munmap cleanup**: `_munmap(addr, len)` for `owned<Mapping>` drop + the page region, mirroring
   ELF (syscall 11 → `_munmap` call).

**Gotcha.** Self-host gate: an fs-using program builds only with explicit `--backend`
(`self_host_caps_allowed`, `main.c`) — so the example is `--backend zero-macho64` on macOS, the
analog of `--backend zero-elf64` on Linux. The conformance harness tolerate-skips fs fixtures on
the default exe path, same as today.

**Acceptance.** macho ports of `page-alloc-region.0`, `mmap-file-readonly.0`,
`mmap-file-notfound.0` build + run natively: allocate a zeroed multi-MB region, map a file, read
f32 from it. (Reads need P1's f32-load; pure-integer alloc can land first.)

### P3 — byte-view reinterpret + typed-span index load/store

**Files:** `emit_macho64.c` (≈1224 load, ≈1452 store, byte-view len/ptr ≈901–974).

Port from ELF (`emit_elf64.c` ≈1111 reinterpret, ≈2415–2518 indexed load / readInt / readFloat):

1. **Reinterpret** (`bytesAsF32`/`bytesAsMutF32`/I32/U32/F64): ptr unchanged; len `>> log2(elem)`
   (`lsr` on arm64). The four typed names share one path, as on ELF.
2. **Indexed load/store** for typed spans: bounds-check (`cmp` + branch-to-`brk`/`udf` trap),
   `ptr + index*elemSize`, then `ldr/str` of the right width (`ldrb`/`ldr w`/`ldr s`/`ldr d`).
   The f32 forms need P1's FP load/store; integer forms are independent.
3. **u8 span store** (item D on ELF): emit `strb` — the macho equivalent of the relaxed ELF store
   guard. Unlocks raw-byte `<0xNN>` decode parity.
4. `readI32Le`/`readU32Le`/`readF32Le`/`readF64Le` over runtime spans: `ldr w`/`ldr s`/`ldr d`
   with the same bounds check.

**Acceptance.** macho ports of `mem-bytes-as-*`, `codec-read-*`, and a u8-store fixture run
natively, incl. the bounds-trap fail fixtures.

### P4 — Aggregate ABI (sret, span-in-record, record params/returns)

**Files:** `emit_macho64.c` (≈1513 return-type gate, ≈1519 byte-view-param gate, prologue/epilogue,
record copy). Port the design in [`aggregate-abi.md`](./aggregate-abi.md) from ELF to arm64.

1. **sret**: replace the "scalar/void only" bail (≈1513). Caller passes the destination address in
   **`x8`** (AAPCS indirect-result), *not* a general arg reg — so the 6-arg cap is not reduced by
   sret on arm64 (an ABI nicety vs. ELF's rdi). Callee writes fields through `x8`; convention for
   the returned address mirrors ELF (`aggregate-abi.md`).
2. **Span (byte-view) params** (≈1519): pass `(ptr,len)` in two int regs, like ELF's two-GPR span
   ABI but x-regs.
3. **Span-in-record**: 16-byte field (ptr@off, len@off+8), load/store split — port ELF layout.
4. **Record copy / record-or-literal as argument**: port `elf_emit_record_copy_to` /
   `elf_emit_record_call_with_dest` analogs.

**Gotcha — arg cap.** arm64 has 8 int + 8 FP arg regs, so the Zero 6-int / 5-int(sret) caps are
*not* hardware limits here. **Keep the existing caps** so signatures stay portable across
backends (the `.0` sources were written to them; widening is a separate, deliberate change — out
of scope). Note in passing: arm64's x8-sret means a record-returning call could afford 6 int args,
but don't diverge.

**Acceptance.** macho port of `aggregate-shape-abi.0` runs natively; `fail/aggregate-record-arg-while.0`
still fails. The llama2 `Config`/`TransformerWeights`/`RunState`/`Tokenizer` shapes cross function
boundaries natively.

### P5 — f32 / NEON instruction selection for the kernels (the long pole)

**Files:** `emit_macho64.c` — a real FP/NEON instruction layer. Port the *intent* of
`elf_emit_xmm_arith` (≈`emit_elf64.c:292-306`) and the XMM load/store/local helpers (≈218–226)
to arm64.

1. **Scalar f32 first** (correctness before speed): `fmov`, `ldr/str s_` (f32 locals + index
   load/store from P3), `fadd/fsub/fmul/fdiv s_`, `fcmp`+`fcsel`/branch for compares, casts
   `fcvtzs` (f32→i32), `scvtf`/`ucvtf` (i32/u64→f32). Covers rmsnorm/matmul/softmax/rope/swiglu
   exactly as the ELF scalar path does — token-for-token parity is the bar, not throughput.
2. **u64→f32** for the PRNG (`sampler.0` `randomF32`): `ucvtf s_, x_` — arm64 has a direct 64-bit
   source, mirror the ELF `cvtsi2ss` note.
3. **(Optional, later)** NEON-vectorize the matmul inner loop (`v_` regs, `fmla`) — this is item
   **H** territory (SIMD), not required for E. Land scalar parity first; defer NEON to H.

**Acceptance.** macho ports of `math-{rmsnorm,matmul,rope,swiglu}-span.0`, `transformer-forward.0`,
`sampler-sample.0` run natively, numerics matching the libm C reference. Then the integration
fixture `generate-argmax.0` reproduces the exact token sequence natively.

### P6 — Wire the example + native validation

**Files:** `examples/llama2/validate.sh`, `examples/llama2/README.md`, CI config.

- Add a `darwin-arm64` build path: `--backend zero-macho64 --emit exe --target darwin-arm64`.
- Extend `validate.sh` with a native-macOS branch (no Docker): build the Zero exe natively, run
  the parity matrix against the **same** `llama2.c` compiled natively with the system libm, diff
  token-for-token. Keep the existing Docker/Linux branch for CI parity.
- Document the native macOS build/run in the README; add a darwin row to the validation table.
- **libm parity caveat** (Risks): macOS libm ≠ musl libm; argmax (temp 0) should still match
  token-for-token given the kernels' margins, but expect rare divergence on tight-margin tokens —
  validate the Linux build matches Linux-`llama2.c` (musl) and the macOS build matches
  macOS-`llama2.c` (system libm), i.e. parity *per platform*, not byte-identical *across* them.

**Acceptance.** stories15M runs natively on Apple Silicon, token-for-token vs. natively-built
`llama2.c`; `validate.sh` prints PASS on macOS without Docker.

## Risk register

1. **FP/NEON instruction emission is net-new** (P5) — the largest chunk. Mitigate: scalar f32
   first (correctness), defer NEON to item H; reuse ELF's op→opcode structure.
2. **libm ULP divergence macOS vs musl** — token-for-token *across* platforms is not guaranteed;
   parity is *per platform* (Risk handled in P6). Temp-0 argmax margins (smallest 0.077 in the
   fixture) are well above ULP, so divergence should be rare to nil.
3. **Raw syscall vs libSystem** (Open Q1) — choosing raw `svc` would couple to an Apple-unstable
   interface and need a second mechanism; libSystem reuses P1. Strongly prefer libSystem.
4. **Self-host gate friction** — fs example needs `--backend zero-macho64`; harness already
   tolerate-skips this class. No new risk, just the macOS analog of the Linux flow.
5. **Apple ABI corner cases** (sub-8-byte stack args, variadic) — avoided while we stay in
   registers; llama2 signatures fit. Flag if a helper ever spills to the stack.
6. **Scope creep to Windows/Linux-ARM64** — explicitly out of the main plan; see below.

## Out of scope here (separate follow-ons)

- **Windows x64** (`win32-x64.exe` → `zero-coff-x64`, `emit_coff.c`): no POSIX. Needs a Win32
  shim — `CreateFileMapping`/`MapViewOfFile` for mmap, `VirtualAlloc` for heap, MSVCRT/UCRT math
  (`expf`/`sqrtf`), and the Windows x64 call ABI (`rcx/rdx/r8/r9` + shadow space). The COFF
  backend exists but lacks all six llama2 features. **Large.**
- **Linux ARM64** (`linux-arm64`/`linux-musl-arm64` → `zero-elf-aarch64`, `emit_elf_aarch64.c`):
  the backend is an MVP (integer-literal returns; `target.c` reason "AArch64 ELF machine-code
  backend is not implemented yet"). It would reuse macOS's **arm64 instruction selection** (P5)
  but with **Linux syscalls** (raw `svc`, like ELF64) and **ELF relocations** (`R_AARCH64_CALL26`)
  instead of libSystem/Mach-O. Best done *after* P5 lands, harvesting the FP/NEON layer. **Large.**

The macOS-arm64 plan deliberately front-loads the reusable pieces (arm64 FP instruction
selection, external-call ABI) that both follow-ons inherit.

## Open questions

1. **mmap/libm via libSystem external calls, or raw `svc #0`?** Recommendation: **libSystem** —
   it's the only Apple-stable interface, reuses P1's mechanism for both math and OS, and matches
   how the net runtime already links. Raw `svc` would be a second mechanism on an unstable ABI.
2. **Capability gate for the macOS heap/mmap path** — reuse the existing `heap`/fs capabilities
   (the `darwin-arm64` manifest already lists `fs`/`heap`), or add a macho-specific gate?
   Recommendation: reuse existing capabilities; this is a backend, not a new capability.
3. **Scalar f32 only for v1, or NEON from the start?** Recommendation: **scalar first** (P5.1) —
   correctness/parity is the bar; NEON is item H. Don't entangle E with SIMD.
4. **Token-for-token across platforms, or per-platform?** Recommendation: **per-platform** parity
   (macOS-Zero vs macOS-`llama2.c`; Linux-Zero vs Linux-`llama2.c`). Cross-platform byte-identity
   is not a goal given libm differences. Confirm this is acceptable for the README claim.
5. **CI for native macOS** — add a macOS runner (real native gate) or keep darwin validation
   local-only (Linux/Docker stays the CI gate)? Recommendation: at least a local `validate.sh`
   macОS branch now; a macOS CI runner if the project wants the darwin row continuously green.
6. **Does this plan target macOS-arm64 only, or commit to all three (E in full)?** This doc plans
   macOS-arm64; Windows + Linux-ARM64 are scoped as separate Large follow-ons. Confirm that split. – all three

## Connections

- [`enhancements.md`](./enhancements.md) — item E spec (the backlog entry this plans), plus the
  backend constraints cheat-sheet shared by every `.0`.
- [`plan.md`](./plan.md) — v0.1 (ELF64); the features being ported (mmap 0a, reinterpret 0c,
  aggregate ABI 0d/Phase 2, STB_GLOBAL/typed-slice Phase 4) and their ELF reference points.
- [`aggregate-abi.md`](./aggregate-abi.md) — the record/span ABI ported in P4.
- [`overview.md`](./overview.md) — mission + roadmap; cross-platform = limitation #1.
