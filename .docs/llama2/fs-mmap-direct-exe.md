# llama2.zero — Precise fs-mmap Direct-Exe Eligibility (item I)

Executable plan for enhancements-backlog **item I** ("Precise fs-mmap direct-exe
eligibility, x64 + arm64"). Makes the default (no-`--backend`) direct-exe path build a
pure-mmap `fs` program on **both** ELF syscall targets, instead of rejecting it as if it
needed a hosted-runtime shim.

Companion to [`enhancements.md`](./enhancements.md) (the I spec, kept for context),
[`cross-platform.md`](./cross-platform.md) (item E — where I was spun out of), and the two
cross-platform progress logs ([macOS](./cross-platform-progress.md),
[Linux-ARM64](./cross-platform-linux-arm64-progress.md)). Read the routing analysis in the
Linux-ARM64 log §"Capability lift + the dispatch" first — this item is the target-general
version of the carve-out that log describes.

## Headline: routing-only, already prototyped, reverted on purpose

This is a **build-routing** change, not a backend or capability change.

- **No backend (`emit_*.c`) change.** Every ELF target already emits file mmap as raw
  syscalls inside the direct exe — it builds today behind an explicit `--backend`. The
  only bug is that the *default* (no-`--backend`) router refuses to pick that path.
- **No manifest change.** `linux-musl-x64`, `linux-musl-arm64`, and `linux-arm64` already
  grant the `fs` capability (`targets.manifest:41,55,83` + the embedded `fallback_manifest`
  in `target.c`). The TAR002 capability gate is already clear — only the build-router gate
  is wrong.
- **Already proven.** The Linux-ARM64 campaign implemented exactly this fix (aarch64-only),
  validated it, then **reverted the routing carve-out** so arm64 mirrors x64, deferring the
  proper target-general version to this item. The leftover functions are gone from the tree
  (verified: no `default_direct_exe_eligible` / `ir_program_fs_use_is_mmap_only` remain) —
  this item reintroduces them, target-general.
- **The example does not depend on it.** `examples/llama2` builds with `--backend
  zero-elf64` and uses libm → obj+link anyway. This is a correctness/cleanliness fix for
  the *default* build path and the conformance suite, not a llama2 unblock.

The whole change is two small scanners + one predicate + two one-line call-site swaps in
`main.c`. **Effort: Small.** The cost is the validation pass, because it changes x86-64
default-routing semantics (see Acceptance).

## The asymmetry today

A pure file-mmap program (uses `fs`, but its only fs ops are `host`/`mmap`/`munmap`), built
with `--emit exe` and **no `--backend`**:

| Target | Today | Why |
|--------|-------|-----|
| **linux-musl-x64** (`zero-elf64-exe`) | ❌ rejected, **CGEN004** | `self_host_caps_allowed` is false (`caps.fs`); not macho, no math ⇒ `ir_needs_zero_runtime_object` false ⇒ falls to the self-host gate ⇒ rejected |
| **linux-musl-arm64** (`zero-elf-aarch64-exe`) | ❌ rejected, **CGEN004** | same path (deliberately made to mirror x64 post-campaign) |
| **darwin-arm64** (`zero-macho64-exe`) | ✅ builds + runs | macho mmap lowers to libSystem externals ⇒ `ir_needs_zero_runtime_object` returns true ⇒ routed to **obj+link** *before* the self-host gate is consulted |

The gate conflates two separate questions: *"what capabilities does the program use?"*
(it uses `fs` — true) vs *"can the direct backend emit a standalone exe for this program
on this target?"* (on ELF, **yes** — mmap is self-contained syscalls). Item I separates
them.

The CGEN004 is `init_direct_backend_diag` (`main.c:3664`, `diag->code = 4004` →
`"CGEN004"`, `main.c:146`). The conformance harness already tolerates it as a skip
(`run.mjs:69,139`), which is why the fs-mmap fixtures currently **skip** on x64/arm64 (and
run only on darwin via obj+link).

## Why ELF can build it directly (no shim, no link)

On the ELF syscall targets, the entire mmap family is raw `syscall` (x64) / `svc` (arm64),
fully contained in the direct exe — no external symbol, no obj+link, no libSystem:

- `IR_VALUE_FS_MMAP` → `openat`→`lseek`→`mmap`→`close` sequence
  (`emit_elf64.c` `elf_emit_mmap_file_addr_size` ~`:2892`; aarch64
  `emit_elf_aarch64.c:1127,2280`).
- `IR_VALUE_FS_MUNMAP` → `munmap` (`emit_elf64.c:1701`; `emit_elf_aarch64.c:1604`).
- `IR_VALUE_FS_HOST` → the `AT_FDCWD` constant (no syscall) (`emit_elf64.c:1674`).

Contrast Mach-O, where these lower to `bl _mmap`/`_open`/`_close`/`_munmap` externals — the
reason macho is *forced* to obj+link by `ir_needs_zero_runtime_object`. ELF has no such
need; the default router is simply too coarse.

(`std.fs.mappingBytes(&m)` lowers to a **BYTE_VIEW span** view, not an `FS_*` value —
`ir.c:2583,3763` — so it never counts as a "non-mmap fs op" below. It still sets `caps.fs`
via the `std.fs.` prefix, `main.c:1314`, which is correct: the capability stays declared.)

## The fix

Two new IR scanners + one eligibility predicate, wired at the two routing sites. Mirror the
*shape* of the existing macho-mmap scanner (`ir_value_uses_libsystem_mmap` /
`ir_program_uses_libsystem_mmap`, `main.c:4244-4278`) — but **do not touch that scanner or
`ir_needs_zero_runtime_object`**; they stay macho-scoped so x86-64 and macho routing are
byte-for-byte unaffected.

### 1. "fs use is mmap-only" scan (`main.c`, near `:4244`)

The mmap family is `{FS_HOST, FS_MMAP, FS_MUNMAP}`. Its complement is exactly the
audit-base fs set already enumerated at `main.c:4086-4105` (open/create/read*/write*/
close/exists/remove/rename/fileLen/mkdir/rmdir/isDir/dirEntry/tempName/atomicWrite). So
"fs is mmap-only" ⟺ the program uses **none** of those:

```c
// Returns true if the program performs any fs op outside the syscall-lowered mmap family
// (FS_HOST/FS_MMAP/FS_MUNMAP). Mirrors ir_*_uses_libsystem_mmap's recursion shape.
static bool ir_value_uses_non_mmap_fs(const IrValue *v) {
  if (!v) return false;
  switch (v->kind) {
    case IR_VALUE_FS_OPEN:        case IR_VALUE_FS_CREATE:
    case IR_VALUE_FS_READ_PATH:   case IR_VALUE_FS_WRITE_PATH:
    case IR_VALUE_FS_READ_BYTES_PATH: case IR_VALUE_FS_WRITE_BYTES_PATH:
    case IR_VALUE_FS_READ_ALL:    case IR_VALUE_FS_READ_FILE:
    case IR_VALUE_FS_WRITE_ALL_FILE: case IR_VALUE_FS_CLOSE_FILE:
    case IR_VALUE_FS_EXISTS:      case IR_VALUE_FS_REMOVE:
    case IR_VALUE_FS_RENAME:      case IR_VALUE_FS_FILE_LEN:
    case IR_VALUE_FS_MAKE_DIR:    case IR_VALUE_FS_REMOVE_DIR:
    case IR_VALUE_FS_IS_DIR:      case IR_VALUE_FS_DIR_ENTRY_COUNT:
    case IR_VALUE_FS_TEMP_NAME:   case IR_VALUE_FS_ATOMIC_WRITE:
      return true;
    default: break;              // FS_HOST/FS_MMAP/FS_MUNMAP and all non-fs kinds: ok
  }
  if (ir_value_uses_non_mmap_fs(v->index) ||
      ir_value_uses_non_mmap_fs(v->left)  ||
      ir_value_uses_non_mmap_fs(v->right)) return true;
  for (size_t i = 0; i < v->arg_len; i++)
    if (ir_value_uses_non_mmap_fs(v->args[i])) return true;
  return false;
}
// + ir_instrs_uses_non_mmap_fs / ir_program_uses_non_mmap_fs walkers (copy the
//   ir_instrs_use_libsystem_mmap / ir_program_uses_libsystem_mmap bodies, swap the leaf).
```

### 2. Eligibility predicate (`main.c`, near `self_host_subset_compatible` `:8233`)

```c
static bool default_direct_exe_eligible(const Program *program, const CapabilitySummary *caps,
                                        const IrProgram *ir, const ZTargetInfo *target) {
  if (self_host_subset_compatible(program, caps)) return true;   // existing no-capability subset
  if (!caps) return false;
  if (caps->time || caps->rand || caps->net || caps->proc || caps->web) return false; // fs is the *only* gated cap
  if (!caps->fs) return false;
  if (ir_program_uses_non_mmap_fs(ir)) return false;             // fs use ⊆ {host,mmap,munmap}
  const char *exe = z_direct_exe_emitter(target);               // ELF syscall exe targets only
  return exe && (strcmp(exe, "zero-elf64-exe") == 0 ||
                 strcmp(exe, "zero-elf-aarch64-exe") == 0);
}
```

### 3. Wire at the two routing sites

Replace `self_host_subset_compatible(program, &caps)` with
`default_direct_exe_eligible(program, &caps, ir, target)` in the `default_direct_exe`
computation at:

- **`target_readiness_select_diag`** — `main.c:8435` (the readiness / dry-emit selector;
  `ir` and `target` are already parameters).
- **the main build dispatch** — `main.c:9652` (uses `direct_exe_caps`; `&ir` and `target`
  are in scope).

### What stays unchanged (and why that matters)

- **`self_host_subset_compatible` / `self_host_caps_allowed`** are *not* edited — the
  capability-reporting JSON (`append_self_host_subset_json:8239`,
  `append_self_host_routing_json:8277`) keeps honestly listing `fs` as a blocked
  subset reason. A pure-mmap program is still *not* in the no-capability subset; it is
  merely direct-exe-buildable. That divergence is the whole point.
- **`ir_needs_zero_runtime_object` and the libsystem-mmap scanner** stay macho-scoped ⇒
  darwin still obj+links and runs; macho/coff routing is byte-identical.
- **No `--backend`-explicit behavior changes** — `requested_direct_exe` is untouched.

## Effort

**Small + localized**: ~3 scanner functions + 1 predicate + 2 call-site swaps, all in
`main.c`. No new fixture *code* (the fs-mmap fixtures already exist; they flip skip→run).
The real work is the validation pass, because **x86-64 default routing semantics change**.

## Acceptance / validation

**Conformance flip (the headline result).** Seven `fs`-mmap fixtures are built by the
harness with `--emit exe` and **no `--backend`** and currently tolerate-skip via CGEN004 on
the ELF targets; after the fix they **build and run**:

- `mmap-file-readonly`, `mmap-file-notfound`, `mmap-maybe-success`, `mmap-maybe-4gib-len`,
  `mmap-munmap-loop`, `mmap-munmap-early-return` (`run.mjs:2072-2077`)
- `mem-bytes-as-f32-mmap` (`run.mjs:1996`)

These run through `assertDirectRuntimeOrUnsupported` (x64) + `assertDarwinNativeOrUnsupported`
+ `assertLinuxArm64NativeOrUnsupported`. **No harness change is required** — those helpers
already build with no `--backend` and assert stdout once the build succeeds; the skip→run
flip is automatic. (`page-alloc-region` does **not** flip — it is anonymous `pageAlloc`
(`heap` cap, no `fs`), so it already passes the gate today.)

Where each actually executes:
- **x64**: build succeeds everywhere; the binary *runs* only on a linux-x64 host
  (`canRunLinuxMuslX64`), i.e. in CI — not on an Apple-Silicon dev box.
- **arm64**: builds + runs in a `linux/arm64` Docker container (native on Apple Silicon +
  Docker Desktop, no qemu).
- **darwin**: unchanged — still obj+link, still runs.

**Targeted manual checks (run before the suite):**
1. x64 flip: `bin/zero build --emit exe --target linux-musl-x64
   conformance/native/pass/mmap-file-readonly.0 --out .zero/out/mmap-x64` now **succeeds**
   (was CGEN004); run under amd64 Docker → `mmap file readonly ok`.
2. arm64 flip: same with `--target linux-musl-arm64`; run under `linux/arm64` Docker.
3. **Scope guard (must still reject):** a non-mmap fs program (e.g. `std-fs.0`, which
   opens/writes) with no `--backend` on x64 **still** bails CGEN004 → needs `--backend`.
   This proves the predicate is mmap-*only*, not "any fs".
4. **No regression:** darwin `mmap-file-readonly` still obj+links and runs; `build --json`
   for a macho/coff program reports the same `objectEmission.path` as before.

**Semantics-change audit (the reason this is its own item):** grep tests/fixtures for any
that *assert* the old x64 fs-mmap → CGEN004 rejection, or that assert `objectEmission.path`
/ routing JSON for an fs-mmap program on x64 — those now report `direct-elf64-exe` and must
be updated. (None found in the current `run.mjs`, but re-check before landing.)

**Full gate:** `make -C native/zero-c` → `pnpm run conformance` green → Docker amd64
spot-run. `pnpm run native:test` red-on-baseline caveat from `enhancements.md` still
applies; `conformance` is the reliable gate.

## Risks / gotchas

- **x86-64 blast radius.** This is the one item that changes a *shipped* x64 default-routing
  decision. The change is monotonic (a previously-rejected program now builds; nothing that
  built before stops building), but JSON/routing assertions keyed on the rejection will
  flip — hence the dedicated audit step.
- **Predicate completeness.** The scanner must recurse into `index`/`left`/`right`/`args`
  and into `then`/`else` instruction lists, or a non-mmap fs op nested in a branch slips
  through and a non-buildable program reaches the ELF exe emitter (which would then bail —
  honest, but a worse diagnostic). Copy the proven recursion from the libsystem-mmap walker.
- **`mappingBytes` is not an `FS_*`.** Don't add it to the deny-list; it is a BYTE_VIEW span
  and must remain allowed (it is how every mmap fixture reads the mapping).
- **Keep both manifests consistent** if a capability row is ever touched — `targets.manifest`
  (on-disk, authoritative) *and* `target.c`'s `fallback_manifest` (installed-build
  fallback). No change is needed for this item, but the pair must not drift.

## Out of scope

- **Other ELF-syscall fs ops** (open/read/write/close/remove/…). They *are* also raw
  syscalls on ELF and could in principle be direct-exe-eligible too, but item I deliberately
  scopes to the read-only mmap family — the proven set, and the minimal x64 semantics change.
  Widening is a separate decision (see Unresolved).
- **Windows / coff.** mmap is not implemented on `emit_coff.c`; the predicate's
  emitter check excludes it.
- **The optional `bytesAsI8`/Q-opt track** (item G) and **SIMD/threads** (item H) — unrelated.
- **`examples/llama2` itself** — unaffected (builds with `--backend`, uses libm→obj+link).

## Step-by-step

1. Add `ir_value_uses_non_mmap_fs` + `ir_instrs_uses_non_mmap_fs` +
   `ir_program_uses_non_mmap_fs` near `main.c:4244` (copy the libsystem-mmap walkers, swap
   the leaf predicate).
2. Add `default_direct_exe_eligible(program, caps, ir, target)` near `main.c:8233`.
3. Swap the predicate at the two `default_direct_exe` sites (`main.c:8435`, `:9652`).
4. `make -C native/zero-c`.
5. Manual checks 1–4 above (x64 flip, arm64 flip, scope guard rejects, darwin unchanged).
6. Semantics-change audit: grep for tests asserting the old x64 fs-mmap rejection / routing
   JSON; update any.
7. `pnpm run conformance` (confirm the 7 fs-mmap fixtures now run on x64 (CI) + arm64
   (Docker), darwin unchanged) → Docker amd64 spot-run.
8. *(Optional hardening)* tighten the harness so the 7 fs-mmap fixtures are asserted to
   **no longer** tolerate-skip on the ELF targets — turning the auto-flip into an enforced
   expectation so a future regression can't silently re-skip them.

## Appendix — commands

```sh
# build compiler
make -C native/zero-c

# x64 flip: builds now (was CGEN004), no --backend
bin/zero build --emit exe --target linux-musl-x64 \
  conformance/native/pass/mmap-file-readonly.0 --out .zero/out/mmap-x64
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work alpine \
  /work/.zero/out/mmap-x64            # → "mmap file readonly ok"

# arm64 flip: builds + runs native-in-docker (no qemu on Apple Silicon)
bin/zero build --emit exe --target linux-musl-arm64 \
  conformance/native/pass/mmap-file-readonly.0 --out .zero/out/mmap-arm64
docker run --rm --platform linux/arm64 -v "$(pwd)":/repo -w /repo alpine \
  /repo/.zero/out/mmap-arm64          # → "mmap file readonly ok"

# scope guard: a non-mmap fs program still needs --backend (must still CGEN004)
bin/zero build --json --emit exe --target linux-musl-x64 \
  conformance/native/pass/std-fs.0 --out .zero/out/std-fs-x64   # diagnostics[0].code == CGEN004

# full gate
pnpm run conformance
```

## Connections

- [`enhancements.md`](./enhancements.md) — item I spec (kept for context), backend cheat-sheet.
- [`cross-platform-linux-arm64-progress.md`](./cross-platform-linux-arm64-progress.md) — §"Capability
  lift + the dispatch": the aarch64-only prototype of this fix (since reverted), with the byte-level
  syscall evidence for the mmap family.
- [`cross-platform.md`](./cross-platform.md) — item E, the parent.
- [`plan.md`](./plan.md) — v0.1 backend gotchas (mmap = syscalls on ELF, libSystem on macho).
