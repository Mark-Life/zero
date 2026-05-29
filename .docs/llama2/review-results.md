# PR-Readiness Review — `feat/llama2-row-port`

Diff range: `b134668..HEAD` (HEAD `2b05f02`) · Host: darwin-arm64 · Reviewer: lead synthesis of 9 per-dimension reviews + adversarial verification.

---

## 1. Summary

**Verdict: NOT-READY (ready-after-fixes once two PR-assembly blockers are cleared).**

- **Build: CLEAN.** HEAD compiles with `-std=c11 -Wall -Wextra -Wpedantic -Os`, zero warnings, zero errors across all 54 source files (`make clean && make`, exit 0).
- **Conformance: RED — and it is OUR regression.** `pnpm run conformance:local` exits 1, hard-aborting at `run.mjs:203` on `conformance/common/pass/cli-args.0`. The fixture is upstream-owned (added by `2f86e28`, an ancestor of `b134668`, not in our diff) **and it passes on the `b134668` baseline** (prints `alpha`), but **fails on our HEAD** (prints an empty line). Reproduced + git-bisected (build endpoints below): **the first bad commit is `8054ea0` "TB-7 — 64-bit `len` slot for Span/Mapping"** — widening the span/Mapping `len` field 32→64-bit broke the argv-seeded **String** length on the macho64 direct-exe path, so `world.out.write first.value` writes 0 bytes. TB-7's own commit message claims "No regressions" but its verification matrix never exercised the argv/String path. This is a real, branch-introduced regression, not an inherited-upstream skip.
- **Compiler metrics: PASS.** `pnpm run compiler:metrics` → `budget.ok: true`, `violations: []`. Budget bumps track real new-backend work; nothing exceeds budget. Does not block.
- **Smokes: PASS** (provenance-guardrails, type-core, mir-verifier, row-syntax).

**Residual risk.** The two confirmed code-correctness blockers found in review (COFF float-arg ABI; float-record-field sret offset) are on backends/paths that no shipped llama2 code exercises (COFF is build-only non-CLI; the float-sret path is unreached by current record returns). They are real silent-miscompile latent bugs but do not break a passing gate. The hard PR blockers are: (a) the RED conformance suite, and (b) the committed `.docs/llama2/` planning tree that must be stripped from the PR.

---

## 2. Blockers (must fix before PR)

Sorted by severity. Rejected findings excluded; severities reflect adversarial verdicts.

### B1 — Conformance suite is RED: our branch regressed argv/String on macho64 (CRITICAL — verified) — ✅ RESOLVED (commit `41c8e44`)
> **RESOLVED 2026-05-29.** Root cause confirmed by disassembly: `std.args.get` seeding stored the Maybe len with a 32-bit `store_local_w` at offset 16 on macho64 (`emit_macho64.c:1602,1621`) and aarch64_direct (`aarch64_direct.c:1620,1639`), while TB-7's read path loads it 64-bit (`load_local_x`). The upper 32 bits stayed as stack garbage → `world.out.write` read a huge len → `write()` EFAULT (0 bytes), masked as empty-output/exit-0 by the result-discarding write stub. elf64/macho_x64 already stored wide. Fix: widened both stores to `store_local_x` (the strlen reg is set by 32-bit ops that zero-extend). `cli-args.0` prints `alpha` again; `conformance:local` green (host 60 / linux-arm64 docker 123 / darwin-x64 rosetta 123 ran).

- **Where:** symptom at `conformance/run.mjs:203` (`assertCommonRuntimeOrUnsupported`) on `conformance/common/pass/cli-args.0`; **regressing commit `8054ea0` (TB-7, 64-bit `len` slot)**; fix site `native/zero-c/src/emit_macho64.c` (String/Span `len` width on the argv-seed + `world.out.write` length path; cross-check the parallel edits TB-7 made to `emit_elf64.c`, `emit_macho_x64.c`, `aarch64_direct.c`).
- **Verified (post-review, real execution + bisect):**
  - HEAD: `cli-args.0` builds clean, `run [alpha beta]` → empty line, exit 0 (should be `alpha`). Sibling `array-sum-min-max.0` runs correctly → defect is scoped to argv/String length, not the backend at large.
  - Baseline `b134668`: same fixture → prints `alpha`. So this regressed *between* baseline and HEAD.
  - `git bisect` (good=`b134668`, bad=HEAD, test = builds + `cli-args.0` prints `alpha`): **first bad commit `8054ea0` "TB-7 — 64-bit `len` slot for Span/Mapping"**. TB-7 widened the span/Mapping len 32→64-bit across all 4 backends; its commit message asserts "No regressions" but its verification matrix (typed-spans/codec/mmap/float/fallible/transformer) never wrote an argv-derived String, so the regression went undetected.
- **Why it blocks:** the branch's CI entrypoint exits 1, and the uncaught `AssertionError` aborts the run before our int8 fixtures (`run.mjs:~3714`) ever execute — so the suite cannot go green and the rest of the run is unobservable. It is a genuine miscompile our PR introduces, not an inherited skip.
- **Fix:** in TB-7's macho64 changes, make the argv-seeded String carry its full 64-bit `len` (or make `world.out.write` read the len at the widened width) so `first.value` writes its real length. Verify the same len-width consistency on the other three backends TB-7 touched (the bug surfaced on the host macho64 direct-exe path; the others may share it but weren't covered by TB-7's tests either). Re-run `conformance:local` to green. Do NOT reorder/skip the fixture to dodge the abort.

### B2 — `.docs/llama2/` planning tree (≈6.9k lines) committed on the branch (HIGH)
- **Where:** `.docs/llama2/` (15 tracked `.md` files added by bootstrap commit `e18908e`); NOT gitignored (`git check-ignore` exits 1).
- **Why it blocks:** Task states this directory is internal planning material to be deleted before the upstream PR. It is currently tracked and would leak into the PR diff. Verified safe to delete: grep for `.docs/` across `examples/llama2`, `conformance`, `docs` returns nothing — no shippable file depends on it.
- **Fix:** `git rm -r .docs/llama2` before opening the PR (it must be explicitly removed; `.gitignore` does not cover it).

### B3 — COFF (Win64) call path passes float args in a GPR, never XMM — silent ABI miscompile (HIGH; confirmed) — ✅ RESOLVED (commit `4376962`)
> **RESOLVED 2026-05-29.** Took option (a), the real fix. Added a float branch to `coff_emit_call_value` and `coff_emit_record_call_with_dest`: spill xmm0 and pop into XMM at the *unified* Win64 slot index (xmm0↔rcx, xmm1↔rdx, …), which the existing `int_slots`/`int_count` counter already tracks (float = one unified slot; sret consumes slot 0). Matches the prologue's `float_idx`/`abi_slot` pairing — not a SysV independent float bank. Verified by disassembling the emitted COFF x64 object: `addf`→xmm0/xmm1, `mix(i,x,j,y)`→x=xmm1/y=xmm3, `mkPair` via sret→a=xmm1/b=xmm2. New build-only fixture `coff-float-arg-call-builds.0` wired into `run.mjs` (win32-x64 + win32-arm64, obj+exe) closes the float-arg coverage gap. `emit_coff.c` budget 1960→1985; metrics clean.

- **Where:** `native/zero-c/src/emit_coff.c` — `coff_emit_call_value` (498–547) and `coff_emit_record_call_with_dest` (553–600).
- **Why it blocks (qualified):** Both helpers classify args only as BYTE_VIEW / RECORD / else. A float arg falls into `else`, where `coff_emit_value` leaves it in xmm0 but `z_x64_emit_push_rax` pushes RAX (garbage); the pop loop moves garbage into rcx/rdx/r8/r9. The float never reaches XMM, yet the COFF prologue (`emit_coff.c:1589-1604`) reads float params *out of* XMM — caller/callee disagree. No diagnostic rejects float args. SysV siblings (`emit_macho_x64.c`, `emit_elf64.c`) handle this correctly. Coverage gap: `coff-float-builds.0` has no float-arg call and COFF is build-only, so the wrong bytes are emitted but never executed.
- **Severity note:** Adversarial verdict `confirmed`, adjusted `high` (not critical): COFF is a documented build-only, non-CLI-exec, deferred backend that llama2 does not target. Listed as a blocker for **diagnostic-floor** reasons — a backend marked "supported" should not silently emit wrong code with no error.
- **Fix:** Either (a) add a float-arg branch mirroring SysV (`z_x64_emit_xmm_push` → pop into `xmm(N)` per Win64, coupling int/float position counters), or (b) at minimum reject float arguments with a `coff_diag_at` so the backend errors instead of miscompiling. A build-only gate cannot catch this, so a hard diagnostic is the safe floor.

---

## 3. Should-fix (medium)

### S1 — Float record field via sret ignores `field_offset` on elf64 (confirmed, medium)
- **Where:** `native/zero-c/src/emit_elf64.c` — `elf_emit_store_instr`, `local_index == UINT_MAX` branch (2457–2467).
- **What:** The float branch calls `z_x64_emit_movs_xmm_ptr_reg(text, 0, 7, is64, false)` (no-displacement MOVSS/MOVSD = `[rdi+0]`) and never adds `instr->field_offset`, followed by a stale comment `// emit displacement manually: rewrite …` and a no-op `(void)0`. A float field at non-zero offset in a by-value-returned record clobbers field 0; the intended slot is never written. Latent: the only by-value-record-return fixture (`aggregate-shape-abi.0`) and llama2 return `Span<f32>`/int fields, never a bare scalar f32/f64 at non-zero offset.
- **Fix:** Use the existing `z_x64_emit_movs_xmm_ptr_reg_disp(text, 0, 7, instr->field_offset, is64, false)` (declared `x64_emit.h:125`); delete the stale comment. Add a fixture returning by value a record whose 2nd (non-zero-offset) field is f32/f64.

### S2 — Same float-sret-offset bug on macho_x64 (confirmed, medium)
- **Where:** `native/zero-c/src/emit_macho_x64.c` (1626–1631), self-documented as untested.
- **Fix:** Same as S1 — switch to `z_x64_emit_movs_xmm_ptr_reg_disp`. (COFF path is already correct here; only elf64 + macho_x64 are broken.)

### S3 — ELF aarch64 EXE path records libm `call_patches` but never resolves them (medium)
- **Where:** `native/zero-c/src/emit_elf_aarch64.c` — `z_emit_elf_aarch64_exe_from_ir` (~416–447).
- **What:** Exe path installs `record_call_patch` so `std.math.*` emits a `bl #0` placeholder into `patch_ctx.call_patches`, but the exe emitter only post-processes `user_call_patches` and never iterates `call_patches`; a static ELF exe has no dynamic link to bind libm. A bare-ELF-aarch64 exe using `std.math` builds exit-0 and jumps into the `_start` stub at runtime. (Object path correctly emits `R_AARCH64_CALL26` relas; COFF aarch64 handles both paths.)
- **Fix:** On the exe path, if `patch_ctx.call_patch_len > 0`, return a graceful `a64_diag` ("direct AArch64 ELF executable cannot bind libm; use obj+link") instead of silently producing a broken binary.

### S4 — `driver` decode emits raw non-printable bytes that `safe_printf` suppresses (medium; confirmed)
- **Where:** `examples/llama2/main.0:319-320, 384-385`; `tokenizer.0:444-463` (decode).
- **What:** llama2.c streams pieces through `safe_printf`, which DROPS single-byte pieces that are neither `isprint` nor `isspace`. The Zero driver writes the piece unconditionally, so a `<0xNN>` fallback decoding to a control byte diverges from C output — contradicting the "byte-identical" claim for arbitrary models. Latent: stories15M (the only model in `validate.sh`'s matrix) is ASCII-only, so no current gated test is violated.
- **Fix:** In the emit step, when the decoded piece is exactly one byte, suppress it unless printable (0x20..0x7E) or whitespace, matching `run.c` `safe_printf`. Otherwise soften the "byte-identical" wording for models that can emit non-printable raw bytes.

### S5 — Bounds-trap fixtures never assert the trap fired (medium; confirmed)
- **Where:** `conformance/run.mjs` — `assertBoundsTrap` (80–93), loop (3946–3970); siblings `assertLinuxArm64BoundsTrap` (350), `assertDarwinX64BoundsTrap` (379).
- **What:** After a successful build, the helper only does `if (failedRun.stderr) assert.match(..., /zero bounds check failed/)` — never asserts non-zero exit/signal. A binary that builds and runs to exit-0 with no stderr PASSES. Covers 13 in-scope fail fixtures (bounds-bytes-as-index, codec-read-*-le-bounds, mem-bytes-as-*-bounds, mem-mut-span-u8-store-bounds, typed-span-from-array-bounds). The *other* helper in the same file was upgraded this branch to assert exit code, so the omission is conspicuous.
- **Fix:** On the runnable host: `assert.ok(failedRun.code !== 0 || failedRun.signal, 'expected bounds trap, got clean exit')`; keep the stderr check.

### S6 — `assertDirectRuntimeOrUnsupported` silently swallows runtime crashes (medium)
- **Where:** `conformance/run.mjs:117`.
- **What:** `if (run.code || run.signal) return;` bails *before* the stdout assertion — a built binary that segfaults/exits non-zero is treated as a silent skip. ~80 new fixtures flow through this (float arith/cast/compare, math libm, codec LE, bytesAs widths, int8 components). A SIGSEGV miscompile on any would not fail the suite. Pre-existing bail, but this branch greatly expands the fixture set on it.
- **Fix:** Replace the early `return` with `assert.ok(!run.code && !run.signal, ...)` (matching `assertCommonRuntimeOrUnsupported`), or push to a 'crashed' list the summary fails on.

### S7 — `pr-pq-comment-leftover` — internal planning label in shippable source (medium)
- **Where:** `conformance/run.mjs:3712` — comment "…per P-Q.2's verify gate." (only such leak in the whole diff).
- **Fix:** Drop the trailing "per P-Q.2's verify gate" phrase.

### S8 — Docs accuracy: `fs.md` mmap arity wrong + example won't compile (medium; confirmed)
- **Where:** `docs/articles/modules/fs.md:33` (table) and `:67-77` (example).
- **What:** Documents `std.fs.mmap(path)` (1 arg); real signature is 2 args `(Fs, String)` (`std_sig.c:178`, `ir.c:2421` lowers only the 2-arg form). The example binds `let fs Fs std.fs.host()` then calls `std.fs.mmap "data.bin"` without passing `fs` — will not compile.
- **Fix:** Table → `std.fs.mmap(fs, path)`; example → `std.fs.mmap fs "data.bin"`.

### S9 — Docs accuracy: `codec.md` claims wired functions are "not yet wired" (medium; confirmed)
- **Where:** `docs/articles/modules/codec.md:28-29` (false limitation) + `:13-16` (table omits 3 rows).
- **What:** Says `readU16Le`/`readI64Le`/`readU64Le` "are not yet wired" — but `ir.c:1780-1782` lowers all three, `std_sig.c:29-31` declares them, and `conformance/native/pass/codec-read-u16-le.0`/`codec-read-i64-le.0`/`codec-read-u64-le.0` pass. All added by this same branch.
- **Fix:** Delete the "not yet wired" clause; add rows for readU16Le (u16), readI64Le (i64), readU64Le (u64).

### S10 — Docs under-claim: `mem.md` omits implemented 64-bit/u8 reinterprets (medium)
- **Where:** `docs/articles/modules/mem.md:13-22`.
- **What:** Table stops at bytesAsI8/bytesAsMutI8 but `std_sig.c:72-74, 80-82` implement bytesAs{I64,U64,U8} and bytesAsMut{I64,U64,U8}, all with passing fixtures.
- **Fix:** Add the six missing rows.

### S11 — Wide COFF codegen surface gated "supported" but never executed (medium; informational)
- **Where:** `native/zero-c/src/buildability.c` (aarch64-direct switch 15-72; COFF_X64 arms 115-160; shape arms 319/332/336/340).
- **What:** Both COFF backends open a wide capability surface (FLOAT, MATH_*, REINTERPRET, READ_*_LE, CALL/CHECK/RESCUE, FIELD_LOAD, allocators, FS_MMAP/MUNMAP, record/byte-view returns, i64/u64). Build-only fixtures prove the emitters don't crash and produce linkable objects, but nothing proves the Win64/COFF-AArch64 machine code is semantically correct (param banks, sret reg, shadow space, IAT thunks). This is the accepted "build-only" residual risk; B3 is one concrete instance of it.
- **Fix:** Accept for the PR but call out explicitly in PR text that COFF codegen is build-validated only and NOT execution-verified; consider marking win32 targets "experimental/unvalidated".

---

## 4. Nits / optional (low / nit)

- **mir_verify smoke adds no cases for any new IR value kind** (`tests/mir_verify_smoke.c`). ~9 new kinds + reworked contracts covered only transitively by conformance. Add focused expect_fail/expect_ok cases (FLOAT type mismatch, MATH non-f32 arg/result, REINTERPRET unsupported element, READ_*_LE contracts, FS_MMAP/MUNMAP, PAGE_ALLOC allocBytes).
- **math arg verifier looser than lowering** (`mir_verify.c` MATH_* ~950-968). Accepts F32|F64; lowering requires F32. Tighten to require IR_TYPE_F32.
- **COFF math inline-path unverified at IR level** (`std_sig.c` math rows + `main.c:4795`). COFF must emit math inline; no IR/verifier gate enforces it. Document; consider a loud assertion.
- **`z_coff_math_emit_import_patches` is dead code** (`coff_emit_state.c:200`, decl `.h:97`). Exe path inlines the construction. Route through the helper or delete before PR.
- **`*_math_symbol_for_value` silent default → SQRTF** (`macho_emit_state.c:73-84`, `elf_emit_state.c:60-71`). Dead today; make the default assert/sentinel so a future 8th math kind can't mis-call sqrtf.
- **a64 exe user-call branch26 to un-emitted callee → offset 0** (`emit_elf_aarch64.c:444`, `emit_coff_aarch64.c:~542`). Add the `!function_emitted[callee]` guard the COFF object path already has. Likely unreachable.
- **MACHO64 float-param ABI-slot asymmetry** (`buildability.c:319` vs `buildability_value_targets.c:62`). Definition-site counts floats as int slots; call-site as FP. Over-counts only (graceful BLD reject, never miscompile). Add MACHO64 to the fp_slots branch for symmetry.
- **mathRuntime JSON + ELF-AArch64 runtime-object cache-key untested** (`target.c:396-424`, `target_backend.c:28`). No conformance assertion pins the strings/cache key. Add graph-JSON assertions mirroring httpRuntime.
- **int8 activation quantizer rounds in f32 vs runq.c double** (`ops.0` roundHA 148-157). Documented, exact for |v|≤127; int8 parity is informational on darwin-arm64. Keep; ensure PR text states int8 parity is reference-tracked, exact only on same-toolchain musl.
- **quantizeActs div-by-zero on all-zero group** (`ops.0:228-236`). Intentional parity with runq.c; unreachable for valid inputs. Leave.
- **encode dummy-space prefix skipped when not in vocab** (`tokenizer.0:332-337`). Diverges from llama2.c's unconditional insert; benign for real sentencepiece vocabs. Document or mirror exactly.
- **`--topk` parsed without allDigits gate** (`main.0:219-221`). Lenient like --temperature/--topp, asymmetric with --tokens. Acceptable.
- **Example file sizes over guidance** — transformer.0 593L, tokenizer.0 466L (guidance 400-500). Cohesive single-responsibility modules mirroring llama2.c; accept deliberately or split. Soft.
- **`validate.sh` matrix wording** — "11-prompt × 3-mode" reads as 33; actual is 11 total cases (8 parity_one + 3 parity_topp) across 3 modes, 5 distinct prompts. Reword.
- **`validate.sh` discards Zero stderr** (`2>/dev/null` at 226/243/262/277). On FAIL the Zero `error:` message is gone. Capture to a per-branch file and echo on FAIL.
- **4 pre-existing fixtures unreferenced in run.mjs** (coff-maybe-byte-view-buildable, std-io-direct, world-stream-renamed-param, std-fs-target-unsupported). First three covered by `scripts/test-native.sh`; wire `std-fs-target-unsupported` into run.mjs or confirm test-native.sh ships.
- **Commit history carries internal phase labels** (P-Q.*, TB-*, P-EX/FX, D1) across 37 commits. Squash/reword for the PR.
- **compiler-metrics budget bumps** are large but justified, gate passes. Optionally add a one-line rationale comment on the biggest jumps (aarch64_direct.c +1600, emit_coff.c +1000).

---

## 5. Test & build status

| Check | Status | Notes |
|---|---|---|
| Compiler build (`make clean && make`) | **PASS** | 54 files, `-Wall -Wextra -Wpedantic -Os`, zero warnings/errors |
| provenance-guardrails smoke | **PASS** | 29 surfaces |
| type-core smoke | **PASS** | |
| mir-verifier smoke | **PASS** | silent on success, verified standalone |
| row-syntax smoke | **PASS** | silent on success, verified standalone |
| `conformance:local` | **FAIL (exit 1)** | aborts at `run.mjs:203` on `cli-args.0` (macho64 argv miscompile); suite dies mid-run, no summary trailer emitted |
| `compiler:metrics` | **PASS** | `budget.ok:true`, `violations:[]`; largest grandfathered fns (ir_lower_expr 1809, main 893) under budget |

**Skipped (expected, not a gap):** Cross-platform Docker/Rosetta targets (linux-musl-x64, linux-musl-arm64, coff/win32) would normally SKIP on this host when docker/rosetta are unavailable — that is the harness behaving correctly, not a defect. However the run **never reached** the skip-classification stage: it aborted on the native darwin-arm64 `cli-args` assertion, so downstream skip counts cannot be enumerated until B1 is fixed.

---

## 6. PR-assembly checklist

1. **Fix B1 (conformance RED).** Land the `emit_macho64.c` argv fix so `cli-args.0` passes, or rebase past it / coordinate the upstream fix. Re-run `pnpm run conformance:local` to green before opening the PR. Do not reorder fixtures to dodge the abort.
2. **Delete the planning tree:** `git rm -r .docs/llama2` (15 files, ~6.9k lines added by `e18908e`). It is NOT gitignored; it must be explicitly removed. Verified no shippable file references it.
3. **Strip the leftover planning label** at `conformance/run.mjs:3712` ("per P-Q.2's verify gate").
4. **Fix the confirmed code blockers/should-fixes** at minimum B3 (COFF float-arg: real fix or hard diagnostic), and ideally S1/S2 (float-sret offset, trivial one-liner each), S3 (a64-elf-exe libm diagnostic).
5. **Fix the docs accuracy bugs** S8 (fs.md mmap arity + broken example), S9 (codec.md false "not yet wired" + missing rows), S10 (mem.md missing reinterpret rows).
6. **Tighten the conformance helpers** S5 (bounds-trap assert exit/signal) and S6 (runtime crash = fail not skip) — these protect the very surfaces this PR adds.
7. **Reword docs/script overclaims:** validate.sh matrix wording; ensure README/PR text states int8 parity is reference-tracked (exact only on same-toolchain musl), and that COFF codegen is build-validated only (S11).
8. **Squash commit history** into a few coherent, label-free commits (e.g. "float/i8 IR + cross-platform backend lowering", "std.math/mem/fs/codec runtime surface", "llama2 example + conformance fixtures + docs"). Ensure the squashed tree contains no `.docs/llama2`.

**Shippable change surface** (what the PR is): `examples/llama2/` (6 `.0` src + README + validate.sh + zero.json), `native/zero-c/` backend/IR/verifier/buildability/target C+H changes, `docs/articles/modules/{math,fs,mem,codec}.md` + `standard-library.md` + `docs.ts` + `docs.test.mjs`, `conformance/run.mjs` + new native fixtures, `scripts/compiler-metrics.mts`, `.gitignore`. `.gitignore` is correct (excludes models/*.vsix/.claude worktrees/large fixture; excludes no shippable file).

---

## 7. Coverage & limitations of this review

No builds or test runs were performed in review (per instructions); build/test state is from the supplied ground-truth report. Cross-platform **execution** (Docker linux-x64/arm64, Rosetta darwin-x64, Windows COFF) was not runnable on this host — those remain residual unknowns.

Per-dimension coverage:
- **ir-frontend** — full diff of zero.h/ir.c/mir_verify.c/std_sig.c/checker.c; traced every new EXPR_CALL interception, float-literal parse, value-kind contracts, MutSpan-slice mutability. Found sound; gaps are smoke coverage + two low contract items. Did NOT deep-review emit_*.c/target.c/main.c.
- **gating-routing** — buildability/target/main routing, manifest sync, mmap obj+link predicate, COFF math carve-out. Could NOT verify actual COFF machine-code correctness (build-only, never executed).
- **x64-backends** — bit-verified all new SSE2 encoders, NaN compares, SysV float ABI. Found B3 (confirmed) + S1/S2 (confirmed). Did not run.
- **arm64-backends** — bit-verified all new FP/NEON encoders, AAPCS, syscalls, fallible-float ABI; traced cli-args through macho argv (found arm64 emitter correct → defect is in frontend/macho argv lowering). Found S3 + a64 user-call nit.
- **object-format** — relocs, symtab ordering, memory hygiene, COFF multi-DLL import table. No critical/high; two low items. cli-args is outside these files.
- **llama2-kernels** — line-by-line vs run.c/runq.c for all forward-pass + int8 kernels and checkpoint mapping. No miscompile; two documented numeric-parity notes.
- **llama2-driver** — PRNG/sampler/tokenizer/CLI vs references. Found S4 (confirmed medium). **The cli-args-driver-miscompile finding was REJECTED**: llama2 always links libm and takes the seeded obj+link path (`seed_main_process_args`, covered by `std-args-libm.0` which asserts on this host), so argv works for the example; only the pure direct-exe path (cli-args.0) is affected.
- **validate-readme** — both net-new files vs main.0/sampler.0. build_ref correctly uses same-toolchain `zig cc -ffp-contract=off`; f32 hard-gate / int8 informational gating sound. **The README native-pass-overclaim finding was REJECTED** for the same libm/obj+link reason. Could NOT execute validate.sh (60MB download + Docker/Rosetta).
- **conformance** — all 127 changed fixtures wired, zero orphans; cross-target gating sound. Found S5/S6 + orphan nit. Did not run; downstream skip counts unknown (suite dies on cli-args first).
- **docs** — every documented symbol cross-checked vs std_sig.c/ir.c/fixtures. Found S8/S9/S10. math.md/standard-library.md/docs.ts/docs.test.mjs verified clean.
- **pr-readiness** — hygiene/label sweep, .gitignore, metrics bumps, commit history. Found B1/B2 + leftover-label + soft hygiene. Did not assess codegen correctness (other dimensions).
