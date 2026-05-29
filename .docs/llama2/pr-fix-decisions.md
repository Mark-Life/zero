# PR-readiness fix campaign — decision log

Branch: `feat/llama2-row-port`. Scope: review-fix campaign synthesizing B-series carryover (B1/B3 already
landed; B2 deferred), Should-fix items S1–S11, and code/doc nits.

## Verify-gate result (final sweep)

| Gate | Result |
|---|---|
| Build (`make -C native/zero-c`, -Wall -Wextra -Wpedantic) | PASS (zero warnings) |
| Smokes (provenance / type-core / mir-verifier / row-syntax) | PASS |
| Metrics (`pnpm run compiler:metrics`) | PASS (`budget.ok:true`, `violations:[]`) |
| Conformance (`pnpm run conformance:local`) | PASS — host darwin-arm64 62 ran (+2 new), linux-arm64 docker 125, darwin-x64 rosetta 125 |

**Top-line verdict: gate is GREEN.** The four review-fix budget growths in `scripts/compiler-metrics.mts`
were bumped with inline rationale: `emit_elf_aarch64.c` 475→505, `emit_coff_aarch64.c` 610→620,
`z_emit_coff_aarch64_exe_from_ir` 125→145, and a new known-large-function entry for
`z_emit_elf_aarch64_exe_from_ir` (130) — all from the intentional S3 a64-exe libm/branch26 guards. With metrics
green the conformance cascade clears: the suite ran end-to-end and the two new fixtures
(`aggregate-float-field-sret.0`, `safe-piece-suppress.0`) executed and passed. No miscompiles, no fixture
failures. Remaining manual steps: B2 (`git rm -r .docs/llama2`) and the final squash before opening the PR.

---

## BLOCKERS (must clear before PR)

### FIXED / actionable
- **METRICS-BUDGET** — compiler-metrics budget RED — FIXED — bumped 4 limits in `scripts/compiler-metrics.mts`: `emit_elf_aarch64.c` 475→505, `emit_coff_aarch64.c` 610→620, `z_emit_coff_aarch64_exe_from_ir` 125→145, + new known-large-function entry `z_emit_elf_aarch64_exe_from_ir` 130 — each with inline rationale. Growth is the intentional S3 a64-exe libm/branch26 guards. Metrics + conformance now green.
- **B1** — 64-bit len store for `std.args.get` Maybe (macho64 + aarch64_direct) — FIXED — landed in prior commit `41c8e44`.
- **B3** — COFF (Win64) float call args to XMM at the unified ABI slot — FIXED — landed in prior commit `4376962`.

### DEFERRED (by user choice, PR-assembly step)
- **B2** — strip `.docs/llama2/` planning tree (incl. this decision log) before opening the PR — DEFERRED — `git rm -r .docs/llama2` at PR-assembly; these planning docs do not ship upstream.

---

## SHOULD-FIX (S1–S11)

### FIXED
- **S1** — float record field via sret ignores `field_offset` on elf64 — FIXED — `emit_elf64.c`: swapped no-displacement `z_x64_emit_movs_xmm_ptr_reg` for `_disp(...instr->field_offset...)`; removed stale comment + `(void)0`. (Patched branch is latent/unreachable from current front-end syntax; fix removes a real bug.)
- **S2** — float-sret-offset bug on macho_x64 (`field_offset` ignored) — FIXED — `emit_macho_x64.c` `machx64_emit_field_store_instr`: same `_disp` swap + stale-comment removal. Shares the S1 fixture (gated to macho_x64 by run.mjs).
- **S3** — ELF aarch64 EXE path records libm `call_patches` but never resolves them — FIXED — `emit_elf_aarch64.c`: guard refuses direct-exe with pending libm patches (`a64_diag`). Defense-in-depth; CLI already forces obj+link for libm.
- **S3-nit** — a64 exe user-call branch26 to un-emitted callee → offset 0 — FIXED — `emit_elf_aarch64.c` + `emit_coff_aarch64.c`: added `function_emitted[]` guard before `z_aarch64_patch_branch26` in both exe user-call loops.
- **S4** — driver decode emits raw non-printable bytes that safe_printf suppresses — FIXED — `main.0`: added pure `isSafePiece` helper, guarded both emit sites (quant + f32 loops) to mirror llama2.c `safe_printf`. New fixture `safe-piece-suppress.0`.
- **S5** — bounds-trap fixtures never assert the trap fired — FIXED — `run.mjs`: added `assert.ok(code !== 0 || signal)` to all three bounds-trap helpers (host + linux-arm64 + darwin-x64).
- **S6** — `assertDirectRuntimeOrUnsupported` silently swallows runtime crashes — FIXED — `run.mjs`: replaced early `return` on crash with `assert.ok(!code && !signal)`; build-gate skip path preserved.
- **S7** — internal planning label in shippable source (run.mjs ~3712) — FIXED — `run.mjs`: dropped the "per P-Q.2's verify gate" phrase.
- **S8** — `docs/articles/modules/fs.md` mmap arity wrong + example won't compile — FIXED — corrected `std.fs.mmap(path)` → `(fs, path)` in table + example (verified vs std_sig.c:178, ir.c:2421).
- **S9** — `codec.md` claims wired functions "not yet wired" — FIXED — removed false clause, added table rows for `readU16Le`/`readI64Le`/`readU64Le` (fixtures already pass).
- **S10** — `mem.md` omits implemented 64-bit/u8 reinterprets — FIXED — added 6 rows: `bytesAs{I64,U64,U8}` + `bytesAsMut{I64,U64,U8}`.
- **S11** — COFF/Win64 codegen gated-supported but never executed — FIXED (doc) — `README.md` Limitations: Windows marked experimental/build-validated-only/never-executed; the 4 real targets relabeled "execution-verified". (See PR text.)

### LEFT / skipped
- (none in S-series — all S1–S11 fixed)

---

## NITS

### FIXED
- **NIT-macho64-fp-slot-asymmetry** — MACHO64 float-param ABI-slot asymmetry (def-site vs call-site) — FIXED — `buildability.c`: added `Z_DIRECT_BACKEND_MACHO64` to the `fp_slots` branch so darwin-arm64 floats count toward fp_slots (cap 8) not int_slots. Only loosens an over-count; no build can regress.
- **math-arg-verifier-looser-than-lowering** — math arg verifier looser than lowering — FIXED — `mir_verify.c`: tightened math-arg checks from `value_is_float` (f32|f64) to `value_type(IR_TYPE_F32)`, matching lowering exactly.
- **nit-coff-math-emit-import-patches-dead** — `z_coff_math_emit_import_patches` is dead code — FIXED — deleted def + decl/doc (`coff_emit_state.c/.h`); exe path uses growable push idiom, helper's fixed-cap was not equivalent.
- **nit-math-symbol-default-sqrtf** — `*_math_symbol_for_value` silent default → SQRTF — FIXED — `elf_emit_state.c` + `macho_emit_state.c`: replaced silent `return SQRTF` with `abort()` (unreachable-default guard for a future 8th kind).
- **nit-mir-verify-smoke** — smoke adds no cases for new IR value kinds — FIXED — `tests/mir_verify_smoke.c`: added 19 expect_ok/expect_fail cases (FLOAT, MATH, REINTERPRET, READ_*_LE, FS_MMAP/MUNMAP, PAGE_ALLOC) agreeing with the f32 math tightening.
- **NIT-topk-gate** — `--topk` parsed without allDigits gate — FIXED — `main.0`: added allDigits gate matching `--tokens`; removed duplicate later binding.
- **NIT-matrix-wording** — validate.sh "11-prompt × 3-mode" reads as 33 — FIXED — `validate.sh:78` reworded to "11 cases (5 distinct prompts) across 3 sampling modes".
- **NIT-stderr-discard** — validate.sh discards Zero stderr, hiding `error:` on FAIL — FIXED — `validate.sh`: redirect Zero stderr to per-branch file, echo+cat on FAIL/INFO; C-ref stderr stays muted.
- **int8-pr-text** — int8 parity over/underclaim in README — FIXED — `README.md`: int8 stated reference-tracked/informational/never-gated, byte-exact only on shared-musl branches, f32 is the only hard gate. (See PR text.)
- **NIT-coff-math-inline** — COFF math inline-path unverified at IR level — PARTIAL (FIXED as doc) — `main.c:4791`: strengthened the `!target_is_coff` carve-out comment (load-bearing; cross-refs emit_coff.c msvcrt IAT; no IR/verifier gate). A loud assert was rejected (assert.h not used in main.c; function returns true for non-math reasons too).
- **nit-compiler-metrics-rationale** — budget bumps lack inline rationale — FIXED — `scripts/compiler-metrics.mts`: added 2-line `//` rationale above the `aarch64_direct.c` (+~1600) and `emit_coff.c` (+~1000) budget entries; `node --check` clean.
- **NIT-orphan-fixtures** — 4 fixtures unreferenced in run.mjs — FIXED — `run.mjs`: wired `std-fs-target-unsupported.0` (TAR002 "lacks Fs" on linux-x64); other 3 confirmed covered by `scripts/test-native.sh`.
- **NIT-mathRuntime-cachekey** — mathRuntime JSON + ELF-AArch64 runtime-object cache-key untested — FIXED — `run.mjs`: added graph-JSON `targetSupport.mathRuntime` assertions (linux-musl-x64 supported; win32-x64 unsupported = COFF lacks-runtime-object branch).
- **WIRE-aggregate-float-field-sret** — wire S1 fixture — FIXED — `run.mjs`: registered in check loop + elf64 runtime list + dedicated host/linux-arm64/darwin-x64 block (stdout "aggregate float field sret ok").
- **WIRE-safe-piece-suppress** — wire S4 fixture — FIXED — `run.mjs`: registered in check loop + dedicated block asserting exact 5 bytes `41 0a 07 6f 6b` ("A\n\x07ok").

### LEFT / skipped (with rationale)
- **NIT-dummy-space** — encode dummy-space prefix skipped when not in vocab (tokenizer.0:331) — LEFT — mirroring llama2.c's unconditional insert would push str_lookup's `-1` sentinel into the token stream (used downstream as an index → unsafe). Real sentencepiece vocab always contains ' '; existing guard matches llama2.c for every real model incl. stories15M. Comment expanded; no behavior change.
- **nit-quantizeacts-divzero** — `quantizeActs` div-by-zero on all-zero group (ops.0:228) — LEFT — byte-for-byte parity with runq.c's quantize; all-zero activation group is unreachable for valid inputs. Source already carries the parity comment (ops.0:225-227).
- **nit-roundha-f32-vs-double** — int8 quantizer rounds in f32 vs runq.c double (ops.0:148) — LEFT — `floorf(v+0.5)` in f32 is exact for |v|≤127 (proof in comment ops.0:143-147); negatives mirror through zero. int8 parity is informational; bit-exact on same-toolchain musl. (Flagged into PR int8 wording.)
- **nit-example-file-sizes** — transformer.0 (593L) / tokenizer.0 (466L) over 400–500 guidance — LEFT — each is a cohesive single-responsibility module mirroring llama2.c's structure; splitting fragments the upstream-parallel layout for no gain. Soft guideline, accepted deliberately.
