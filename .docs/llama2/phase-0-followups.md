# Phase 0 — Follow-ups Before Upstream PR

Hardening items found while reviewing the Phase 0 changes (0a mmap+heap, 0b codec
int reads, 0c span reinterpret) on `feat/std-math-llama2` (2026-05-20). See
[`./plan.md`](./plan.md) for what landed.

**None of these block llama2 progress** — Phase 0 is runtime-verified working
(all fixtures compile; runtime confirmed via `docker run --platform linux/amd64`;
bounds fixtures trap with SIGILL; heap denial + darwin-unsupported both fail
closed). These are the polish/rigor items to close before a PR to the *origin*
repo (not the fork), where "it's a language, don't cut corners" applies.

Verify recipe (used for all runtime claims below):

```
zero build --emit exe --target linux-musl-x64 --backend zero-elf64 <fixture> --out /tmp/x
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -v /tmp:/tmp -w /work alpine /tmp/x
```

(`--backend zero-elf64` only needed for `fs`-using programs — they're excluded
from the default `--emit exe` self-host path; see plan Note 3.)

---

## 1. Untested `Maybe` mmap success path — REQUIRED (regression risk)

**What.** `std.fs.mmap` (the non-raising `Maybe<owned<Mapping>>` form) has emit
code for the success branch, but no fixture exercises it. Only the `none` branch
is covered (`mmap-file-notfound.0`). The raising form (`mmapOrRaise`) is the only
one with a runtime-tested success path.

**Where.**
- Emit: `emit_elf64.c:3034-3055` (`IR_VALUE_FS_MMAP` → Maybe local: tag@0, ptr@8,
  len@16, then `maybe_clear` on failure).
- Covered today: `conformance/native/pass/mmap-file-notfound.0` (none branch only).

**Why it matters.** The success branch (`if mapped.has { let mut m = mapped.value
... }`) plus `.value` extraction into a `Mapping` local is real, shipped codegen
with zero test coverage. A future change to the Maybe layout or `.value` lowering
could silently regress it. Manually verified working during review, but that
proof isn't committed.

**Fix.** Add `conformance/native/pass/mmap-maybe-success.0` and wire it into the
three lists in `conformance/run.mjs` (pass-list, runtime-list, and note it's
`fs`-gated so it's build-tolerated in CI like `mmap-file-readonly` — runtime
only via the Docker recipe). Validated body:

```zero
pub fun main(world: World) -> Void raises {
    let fs = std.fs.host()
    let mapped = std.fs.mmap(fs, "conformance/fixtures/mmap-f32le.bin")
    if mapped.has {
        let mut m: owned<Mapping> = mapped.value
        let bytes = std.fs.mappingBytes(&m)
        let v0: f32 = std.codec.readF32Le(bytes, 0)
        let v2: f32 = std.codec.readF32Le(bytes, 8)
        let one: f32 = 1.0
        let three: f32 = 3.0
        if std.mem.len(bytes) == 12 && v0 == one && v2 == three {
            check world.out.write("mmap maybe success ok\n")
        }
        std.fs.munmap(&mut m)
    }
}
```

Confirmed: prints `mmap maybe success ok`, exit 0 under `linux/amd64`.

- [x] Done (2026-05-20) — `conformance/native/pass/mmap-maybe-success.0` added
  with the validated body and wired into both lists in `run.mjs` (check pass-list +
  fs runtime-list). Build-tolerated CGEN004 like `mmap-file-readonly`;
  runtime-verified via the Docker recipe (`mmap maybe success ok`, exit 0). Full
  `conformance` run green.

---

## 2. No map/munmap-in-a-loop fixture — RECOMMENDED (plan risk #3)

**What.** plan.md Risk #3: "`owned<Mapping>` must `munmap` exactly once on every
exit path. Model on `owned<File>`/`close`; cover with a fixture that maps + drops
in a loop." Only single map+munmap is tested today.

**Where.** Covered today: `mmap-file-readonly.0`, `mem-bytes-as-f32-mmap.0` (each
one map + one munmap). No repeated/looped map+drop.

**Why it matters.** mmap without a matching munmap leaks address space; a
double-munmap or a missed munmap on an early-return path is a resource bug a
language must not ship. A loop that maps + reads + munmaps N times proves the
explicit-cleanup model holds across iterations and doesn't exhaust mappings.

**Fix.** Add a fixture that maps + `mappingBytes` + reads + `munmap` inside a
`for`/`while` loop (e.g. 1000 iterations), asserting the read value each pass.
Build-tolerated (`fs`), runtime via Docker recipe. Consider also an early-return
path that still munmaps, once move-tracking semantics for `owned<Mapping>` on
early return are confirmed.

- [x] Done (2026-05-20) — two fixtures added (both wired into the check pass-list
  + fs runtime-list, build-tolerated CGEN004, runtime-verified via Docker):
  - `mmap-munmap-loop.0`: maps + `mappingBytes` + `readF32Le` + `munmap` 1000×
    in a `while` loop, counting successful passes and asserting `ok == 1000` at the
    end (`mmap munmap loop ok`, exit 0). Proves the explicit-cleanup model holds
    across iterations without exhausting mappings.
  - `mmap-munmap-early-return.0`: confirms the early-return path via a
    `fun firstFloat(fs) -> f32 raises` helper that munmaps on the taken
    early-`return` path *and* the fall-through path, exactly once each
    (`mmap early return ok`, exit 0). Move-tracking semantics are settled — Zero has
    no implicit drop (Note 1), so `owned<Mapping>` needs no drop on early `return`
    and the checker doesn't complain (same as `owned<File>` in
    `std-fs-resource.0`/`std-fs-readall.0`).

  Constraint found *and fixed* while writing the early-return case: the direct ELF64
  backend rejected a **fallible (`raises`) function returning `f32`** (CGEN004
  "fallible return type is unsupported"). Root cause was the single-register fallible
  ABI (error tag packed into the high 32 bits of `rax`, so only ≤32-bit values fit).
  Fixed by widening it to a two-register ABI (tag in `rdx`, value in `rax`/`xmm0`) —
  see [`./fallible-abi-widen.md`](./fallible-abi-widen.md). Fallible returns now
  accept f32/f64/i64/u64 and a full-width usize; the early-return fixture uses the
  clean `f32 raises` helper form. (Was never llama2-blocking — forward pass is
  non-raising kernels — but closed for upstream rigor.)

---

## 3. `Maybe` mmap truncates length to 32-bit — LOW (latent, >4GB only)

**What.** The raising form stores the mapping length as a full 64-bit value; the
`Maybe` form stores it as 32-bit. A file > 4 GiB mapped via the `Maybe` form
would get a truncated length.

**Where.**
- Raising: `emit_elf64.c:2870` — `elf_emit_store_local_slot_rax(text, local, 8)`
  (8-byte store into Mapping len@8).
- Maybe: `emit_elf64.c:3049` — `elf_emit_store_local_slot_reg(text, local, 16, 0,
  false)` (4-byte store into Maybe len@16).

**Why it matters / why it's low.** llama2 uses `mmapOrRaise` (the 64-bit path), so
stories15M (60 MB) and even v0.2's 27 GB weights are fine *through the path llama2
actually takes*. The 32-bit Maybe store is also **consistent with the existing
`Maybe<MutSpan<u8>>` convention** (`emit_elf64.c:3113`, allocBytes) — so it's not
a new defect, it's an inherited backend-wide convention. But for upstream rigor:
a 4 GiB+ file silently truncating its length via `std.fs.mmap` is a footgun.

**Fix (pick one).**
- **Minimal:** add a one-line comment at `3049` (and the `Maybe<MutSpan<u8>>`
  site) noting the 32-bit length limit, so it's a known constraint not a surprise.
- **Proper:** widen the Maybe-payload length store to 8-byte. This touches the
  shared `Maybe<...span...>` convention (len@16), not just mmap — so it's a
  backend-wide change (maybe_clear, allocBytes, byteBuf, the `.value` extraction
  reader) and needs its own fixture proving a >4GB-length span survives a
  round-trip. Bigger than it looks; scope deliberately.

- [x] Done (2026-05-20) — **Proper widen** chosen over the comment. The
  `Maybe<...span...>` len slot (`MAYBE_BYTE_VIEW` offset 16) is now a full 64-bit
  store/load everywhere, matching the raising `BYTE_VIEW` len@8 and the 24-byte local
  the layout already reserved (`ir.c:775` `byte_size`) — so this was a store/load
  *width* bug, not a layout change. Widened all 9 `len@16` access sites in
  `emit_elf64.c`: the shared `elf_emit_maybe_clear` (now zeroes all 8 bytes — required
  once reads are wide, else a cleared Maybe keeps a stale high dword), the `.value`
  length reader (`elf_emit_byte_view_len` ← `IR_VALUE_MAYBE_VALUE`), and the 7
  producers (`std.fs.mmap`, anon + FixedBuf `allocBytes`, `readAll`/`owned<ByteBuf>`,
  `args.get`/`env.get`, `fs.tempName`). Verified each producer leaves a clean 64-bit
  value in the source register before the store (mmap size in `rdx`; alloc sizes popped
  from the stack; `strlen` builds `rcx` via `xor ecx,ecx`+`inc ecx`, so its high dword
  is zero; small immediates via zero-extending 32-bit `mov`s). Layout invariant now
  documented at `elf_emit_maybe_clear`.

  Fixture `conformance/native/pass/mmap-maybe-4gib-len.0` (wired into the check
  pass-list + fs runtime-list, build-tolerated CGEN004 like the other mmap fixtures):
  maps a **sparse 4 GiB + 5 (= 4294967301-byte)** file via the *Maybe* form, extracts
  `.value`, and asserts `std.mem.len(bytes) as u64 == 4294967301`. The `as u64`
  relabels the usize len so both the byte-view len read and the comparison stay
  full-width (usize is otherwise 32-bit in this backend's user-land, so the kernel —
  via `lseek` `SEEK_END`, not a usize literal — is the only way to introduce a >4 GiB
  length). The sparse file is generated locally and **gitignored, never committed**:

  ```
  dd if=/dev/zero of=conformance/fixtures/mmap-4gib-sparse.bin bs=1 count=0 seek=4294967301
  ```

  CI only *builds* the fixture (fs-gated → CGEN004); runtime is the Docker recipe.
  Reading only the *length* faults zero data pages, so the 4 GiB map is just an
  address-space reservation — safe under qemu amd64. Runtime-verified: prints
  `mmap maybe 4gib len ok`, exit 0. Negative controls confirm exactness — the same
  fixture with `expected = 5` (the old 32-bit-truncated low dword) and with
  `4294967302` (off-by-one) each print nothing. Full `conformance` run green.

---

## 4. Misleading diagnostic for `pageAlloc` on non-ELF backends — LOW (DX)

**What.** Using `std.mem.pageAlloc()` on the Mach-O or COFF backend reports
`"... FixedBufAlloc local requires std.mem.fixedBufAlloc"` — confusing, since the
user wrote `pageAlloc`, not `fixedBufAlloc`. It fails closed (correct — no
miscompile), but the message misdirects.

**Where.**
- `emit_macho64.c:1321` and `emit_coff.c:699` — the ALLOC-local init handler hits
  this when `instr->value->kind` is `IR_VALUE_PAGE_ALLOC` (these backends have no
  `is_page_alloc` branch; ELF64 handles it at `emit_elf64.c:2901` before the
  `FixedBufAlloc` check at `2910`, so ELF64 is unaffected).
- Reproduce: `zero build --emit obj --target darwin-arm64
  conformance/native/pass/page-alloc-region.0` → CGEN004 with the misleading text.

**Why it matters.** `heap` is declared as a capability on darwin targets
(`targets.manifest`), so a darwin user can pass the capability gate and *then*
hit a confusing codegen error. The model is "capability available ≠ codegen
implemented" (pageAlloc is ELF64-only) — the diagnostic should say that.

**Fix.** In the macho/coff ALLOC-local init handler, special-case
`IR_VALUE_PAGE_ALLOC` (or a `is_page_alloc` local) with a clear message, e.g.
*"direct AArch64 Mach-O backend does not support std.mem.pageAlloc (heap
capability is ELF64/linux-musl-x64 only); use FixedBufAlloc/Arena over
caller-owned storage."* Pure DX, no behavior change.

- [ ] Done

---

## Suggested order

1, 2, and 3 are **done** (2026-05-20). Remaining: 4 is a small DX polish (the
misleading `pageAlloc` diagnostic on the Mach-O/COFF backends). None gate llama2
Phase 1.
