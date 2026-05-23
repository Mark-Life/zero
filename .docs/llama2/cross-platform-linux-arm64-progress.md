# llama2.zero cross-platform (linux-arm64 / ELF aarch64) — progress + findings

Handoff artifact for the Linux ARM64 port of the llama2 example. Companion to the **committed**
macOS campaign ([`cross-platform-progress.md`](./cross-platform-progress.md)) and its plan
([`cross-platform.md`](./cross-platform.md), see its "Out of scope: Linux ARM64" paragraph).
Every phase reads this doc + the macOS progress doc, does its phase, validates, appends its
findings here, and marks its row. Terse + factual. Line numbers verified at write time; correct
drift when you touch a file.

Targets: `linux-musl-arm64` (zig `aarch64-linux-musl`) and `linux-arm64` (`aarch64-linux-gnu`),
both → object emitter `zero-elf-aarch64` / exe emitter `zero-elf-aarch64-exe` (target.c:340/350).
ELF machine = EM_AARCH64 (183).

## The keystone insight (read first)

arm64 is arm64: the **instruction ENCODING is byte-identical** between Mach-O-arm64 and
ELF-aarch64. The committed macOS campaign already wrote the *entire* arm64 instruction-selection
layer in `emit_macho64.c` (4059 lines, ~109 `macho_emit_*` helpers): scalar f32/f64, integer
div/mod, byte-view reinterpret, typed-span load/store, aggregate ABI, etc. Linux ARM64 is the
**same instructions** in a different **container** (ELF, not Mach-O), **OS interface** (raw
`svc #0` Linux syscalls, NOT libSystem calls), and **relocations** (ELF `R_AARCH64_*`, not
Mach-O `ARM64_RELOC_*`). So this port is a MERGE of three existing backends:
- `emit_macho64.c` — arm64 instruction selection (FP/integer/aggregate). **Source of the leaf encoders.**
- `emit_elf64.c` (4504 lines) — ELF container (object + direct-exe dual path) + Linux raw-syscall
  ABI + libm via PLT reloc + obj+link. x86-64 instructions, but the **ELF/syscall/reloc PATTERNS**
  are what you port to aarch64.
- `emit_elf_aarch64.c` (346-line MVP today) — the backend you bring to feature parity.

Unlike macOS (where mmap forced obj+link because libSystem is the only stable interface), on
**Linux/arm64 the OS interface is raw `svc` syscalls** — so mmap/write/exit stay on the
**direct-exe** path (exactly like the x86-64 ELF backend). **Only libm forces obj+link** (P3).
This mirrors emit_elf64.c's split precisely.

## Architecture decision — PORT/duplicate the encoders (settled P0)

The arm64 leaf encoders are **ported/duplicated** into `emit_elf_aarch64.c` (under its existing
`a64_` prefix), NOT shared with `emit_macho64.c`. Rationale (full assessment in P0): the project's
established, four-times-repeated pattern is **one fully self-contained `emit_*.c` per backend**
sharing nothing below `ZBuf`/IR (`emit_elf64.c` references `macho_*` zero times; the byte-append
primitive is duplicated per file by design). Only ~450 lines of frozen-ISA leaf encoders are
genuinely shareable; everything else (the ~1700-line dispatch's reloc seam, the raw-`svc` Linux OS
interface, the ELF container) is net-new or lifted from `emit_elf64.c`. Sharing would risk
refactoring code committed at HEAD, fight `-Wunused-function` under `-Wall -Wextra -Wpedantic`, and
block the incremental phase-by-phase build. **Layout:** all new code grows `emit_elf_aarch64.c`
(346 → ~4000 lines); port macho leaves `macho_emit_X` → `a64_emit_X`; introduce an
`Aarch64ElfEmitContext` modeled on `emit_elf64.c`'s `ElfEmitContext` (ELF patch arrays + R_AARCH64
reloc writers), NOT on `MachOEmitContext`; flesh out the existing `z_emit_elf_aarch64_{object,exe}_from_ir`
entry points following the `z_emit_elf64_*` obj/exe split. A `static inline` shared-leaf header is a
clean OPTIONAL follow-up PR if maintainers later ask for DRY — out of scope for the parity PR.

## The grounded current state — the MVP is a PERMISSIVE MISCOMPILER (the #1 thing to fix)

The 346-line MVP (`a64_return_literal` + `a64_emit_function_text`) does NOT bail on programs it
can't compile — it **silently accepts them and emits a no-op integer return** (`movz w0,#0; ret`),
ignoring all FP/logic/`world.write`. Proven: 77 pass-fixtures "build cleanly" for
`linux-musl-arm64` but, run under `docker run --platform linux/arm64`, produce **empty output**
instead of their expected `"… ok\n"`. Nothing in the harness runs `linux-musl-arm64` today, so the
miscompiles are invisible.

**Therefore the #1 design principle for every phase: the backend must be HONEST — emit correct
code OR bail `CGEN004` precisely, NEVER accept-and-miscompile** (exactly how the macho backend
behaves). This is what lets the conformance harness tolerate-skip unimplemented features and lets
fixtures flip skip→run with *correct* output as phases land. The harness `linux/arm64` run branch
can only be added together with P1's honest backend (adding it against the permissive MVP would
turn 67 miscompiles into 67 failures).

## The work map (triangulated build status across all 223 `conformance/native/pass/*.0`)

Built every pass-fixture for `linux-musl-arm64` (MVP), `darwin-arm64` (committed, correct
reference), and bucketed. `darwin-arm64` = the feature set to reach.

| Bucket | Count | Meaning | Phase |
|---|---|---|---|
| **arm OK + darwin OK** | 67 | MVP *miscompiles* (empty out); darwin correct. The core instruction-selection port: FP arith/cast/compare, codec, typed-spans, aggregate-shape-abi, matmul/sampler/tokenizer/checkpoint-q kernels, generate-loop | P1–P6 |
| **arm bail + darwin OK** | 16 | bail "does not support target linux-musl-arm64 for --emit exe" → need **obj+link + libm**: `math-{sqrtf,expf,cosf-sinf,powf,absf-floorf}-known-values`, `math-{rmsnorm,softmax}-{smoke,span}`, `math-rope-span`, `math-swiglu-span`, `std-json-*`, `std-http-response-helpers` | P3 (+P5/P6 for span kernels) |
| **arm TAR002 + darwin OK** | 23 | capability gate (Heap/Fs/Args) withheld by linux-arm64 manifest: `generate-argmax(-q)`, `transformer-forward(q)`, `page-alloc-region`, `mmap-*`, `mem-bytes-as-*`, `ops-q-*`, `std-args` | P7 |
| **shared bail (both)** | 107 | CGEN004 on darwin too → shared MIR-lowering gates / caps unsupported by ALL direct backends. **OUT OF SCOPE** (tolerate-skipped on every native backend; the C-transpile path serves them) | — |
| **arm OK + darwin bail** | 10 | MVP miscompiles but darwin *bails* CGEN004 (`fallible-return-{f32,f64,i64,u64}`, `parse-integers`, `string-slices`, …). The honest backend must BAIL these too (parity with darwin), unless trivially implementable | P1 (bail) |

Reachable target ≈ 67+16+23 = **106 fixtures** darwin builds (matches the darwin campaign's "87
ran" wired-subset + unwired extras). The 107 shared bails are not our work.

## Target gates + capability manifest (target.c, grounded)

- Dispatch already wired: `linux-musl-arm64`/`linux-arm64` → `zero-elf-aarch64` (target.c:340) /
  `zero-elf-aarch64-exe` (target.c:350-351).
- `z_direct_backend_reason` target.c:372 "AArch64 ELF machine-code backend is not implemented yet"
  — lift as features land.
- `target_math_runtime_supported` target.c:437-443 currently allows only `zero-elf64`+linux and
  `zero-macho64`+macos. **P3 adds `zero-elf-aarch64`+linux.**
- **Capability manifest** (embedded TOML in target.c): `linux-musl-arm64` (target.c:50-61) +
  `linux-arm64` (target.c:76-87) declare only `["memory","stdio","time","rand"]`.
  `darwin-arm64` (target.c:11-22) and `linux-musl-x64` (target.c:~48) additionally have
  `args env fs heap [net proc]`. **stdio is present** → P1 `world.out.write` needs no manifest
  change. **P7 lifts `heap`/`fs`/`args`/`env`** on the two linux-arm64 manifests as mmap/fs/args land.

## Linux aarch64 syscall numbers (VERIFY against headers; number in x8, args x0–x5, `svc #0`)

`write=64`, `exit=93` (the MVP + the conformance assertion both use 93 — KEEP it; do not switch to
exit_group=94 or `assertElfAarch64Executable` breaks), `mmap=222`, `munmap=215`, `openat=56`,
`read=63`, `lseek=62`, `close=57`. (macho's BSD `svc #0x80` with the syscall number ORed into a
class is NOT how Linux works — Linux uses the bare number in x8.) libm stays `bl` + **R_AARCH64_CALL26**
resolved by `zig cc -target aarch64-linux-musl` (P3). Data relocs (string/global addressing in
PIE objects): R_AARCH64_ADR_PREL_PG_HI21 + R_AARCH64_ADD_ABS_LO12_NC. The **direct-exe** path needs
no relocations (self-contained at a fixed base, like the MVP + macho exe).

## Validation story (the big advantage on this host)

Apple Silicon + Docker Desktop runs `linux/arm64` containers **NATIVELY** (the Linux VM is arm64 —
no qemu, full speed; confirmed `docker run --platform linux/arm64 alpine uname -m` → `aarch64`,
Docker 28.3.0). So build a `linux-musl-arm64` ELF and run it with
`docker run --platform linux/arm64 -v <dir>:/w alpine /w/<exe>` at native speed. This is the runtime gate.

- **Conformance:** add a `linux/arm64` Docker-run branch in `conformance/run.mjs`, mirroring
  `assertDarwinNativeOrUnsupported` (run.mjs:82): build `--target linux-musl-arm64`, run via
  `docker run --platform linux/arm64`, tolerate-skip CGEN004/BLD003 build bails. Separate
  ran/skipped tally + summary line. **Available whenever docker is present, independent of host**
  (this host's `runnableDirectTarget` is `darwin-arm64`; the linux/arm64 branch is a new, parallel
  mechanism). `assertElfAarch64Object` (run.mjs:154) + `assertElfAarch64Executable` (run.mjs:189)
  already assert ELF aarch64 structure (machine 183; the `movz w0,#42; ret` + `movz x8,#93; svc #0`
  byte sequences) — keep them green.
- **llama2 (P8):** `validate.sh` already has a Docker branch (currently `--platform linux/amd64`
  under qemu). Add a `linux/arm64` path: Zero exe `--backend zero-elf-aarch64 --target
  linux-musl-arm64`; C ref via `zig cc -target aarch64-linux-musl`. **Same musl libm both sides →
  token-for-token expected for f32 AND int8** (no per-platform caveat like macOS; verify int8 — the
  macОS int8 divergence was an `-O3` FMA-contraction effect, so build the ref with `-ffp-contract=off`
  and confirm; see the macOS P6 int8 blocker doc for the pattern).

## Per-phase bar (the orchestrator verifies each before the next agent launches)

1. `make -C native/zero-c` clean (no new `-Wall -Wextra -Wpedantic` warnings).
2. `ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs` → `conformance ok`, no regression;
   `linux/arm64` ran-count rises.
3. The phase's fixtures build for `--target linux-musl-arm64` AND run correctly under
   `docker run --platform linux/arm64`; everything not-yet-implemented tolerate-skips (CGEN004),
   never miscompiles.
4. NO regression to x86-64 ELF or macOS Mach-O (build one `linux-musl-x64` + one `darwin-arm64`
   fixture to confirm — the committed macho path MUST stay green).
5. Spot-check the diff: idiomatic, mirrors emit_elf64.c / emit_macho64.c, no stubs/dead code/debug
   prints. No internal "Phase N" labels in shipped files; `.0` fixtures upstream-quality.

## Phase status

| Phase | Title | Status | Notes |
|---|---|---|---|
| P0 | Grounding + docker + encoder decision + this doc | **done** | docker linux/arm64 native confirmed; work map triangulated; MVP-is-permissive-miscompiler proven; decision = PORT (Option B); 9-phase plan |
| P1 | Honest backend: ELF container + integer codegen + `svc` write/exit + harness branch | **done** | MVP permissive-miscompiler replaced with an honest backend (integer literals incl. 64-bit, locals, params x0–x7, +/−/×, signed+unsigned 32/64-bit compares, if/else/while, same-file `bl` calls, integer/void returns, `world.{out,err}.write` + `check` via raw `svc` write=64, exit=93); everything else bails CGEN004. run.mjs `linux/arm64` docker branch landed green: **3 ran, 130 skipped**, zero miscompiles. classify() byte-identical to committed macho. See "P1 — what landed" |
| P2 | Scalar f32/f64 + FP register ABI | **done** | ported macho FP leaves (ldr/str s_/d_, fmov, fadd/fsub/fmul/fdiv, fcmp/cset, fcvt/fcvtzs/scvtf/ucvtf) + FP value dispatch + FP register ABI + FP-below-int frame scratch; **3 → 32 ran** (29 flipped: float-arith ×8, float-compare ×12, float-cast ×6, float-nan-compare, float-inf-arith, math-isnanf). f32 AND f64 FP encodings byte-identical to committed macho. libm/array smoke kernels + record float-primitives stay honest CGEN004 (P3/P4/P5). See "P2 — what landed" |
| P3 | libm via obj+link + R_AARCH64_CALL26 | **done** | lifted math gate (target.c) + wired the aarch64 obj+link executor (main.c, 3 sites); object path now emits R_AARCH64 relocations (CALL26 for libm externals, ADR_PREL_PG_HI21+ADD_ABS_LO12_NC for strings; same-file `bl` + svc-write helper patched in place). **32 → 37 ran** (5 flipped: math-{sqrtf,expf,cosf-sinf,powf,absf-floorf}-known-values, token-for-libm-correct in docker). math-{rmsnorm,softmax}-smoke still bail (need P4 arrays). See "P3 — what landed" |
| P4 | byte-view reinterpret + typed-span load/store + codec | **done** | ported the macho P3 layer (reinterpret `lsr` len, the `a64_emit_span_index_addr` bounds-checked element-address keystone, typed-span + fixed-array index load/store at every width, codec readInt/readF*Le, span-over-array, byte slices, `std.mem.len`) + admitted fixed-array & span locals. **37 → 49 ran** (+12: 6 codec, math-{rmsnorm,softmax}-smoke [P3 libm + P4 arrays, RUN not just built], mem-bytes-as-{f32,i8}, mem-mut-span-array-store, indexed-mutation). Bounds traps exit 133. Span PARAMS (typed-span-{f32,i32}) wait on P5; pageAlloc-backed mem-* on P7. Byte-identical span-index-addr vs macho. See "P4 — what landed" |
| P5 | Aggregate ABI (sret x8, span/record params) | **done** | ported the macho P4 aggregate ABI under `a64_`: sret via x8 (record literal/local/param/call returns), record params (copy-in by ptr), span params ((ptr,len) in two x-regs), span returns (ptr/len x0/x1, count in w1), span-in-record fields, record-to-record copy, record/shape-literal call args (incl. nested), float record-field load. **49 → 59 ran** (+10: aggregate-shape-abi, typed-span-{f32,i32}, math-{matmul,rmsnorm,softmax,swiglu}-span, generate-loop, float-primitives). Record-returning RAISING fn rejected. div/mod-blocked kernels (rope/sampler/tokenizer/checkpoint-q) build-past-params then bail CGEN004 on `/`/`%` (P6). makeDims sret-spill+field-store run byte-identical to macho. `compared=68 armOnlyBuilds=0 MISCOMPILES=0`. See "P5 — what landed" |
| P6 | div/mod + kernel ops + eqlBytes/slice/copy/fill | **done** | integer `/`,`%` (SDIV/UDIV + MSUB, 32- AND 64-bit, signed/unsigned off the result type) + eqlBytes/copy/fill (settle-into-stable-regs byte loops). The four latent bugs were ALL already fixed by prior phases (verified). **59 → 65 ran** (+6: math-rope-span, sampler-sample, sampler-topk-topp, tokenizer-encode, checkpoint-q-v2, std-mem-copy-fill). `compared=78 armOnlyBuilds=0 MISCOMPILES=0`. div/msub byte-identical to macho. `generate-argmax(-q)`/`transformer-forward(q)` build past div + bail TAR002 Heap (P7). See "P6 — what landed" |
| P7 | mmap/munmap/pageAlloc via raw `svc` + lift capabilities | **done** | anon mmap (pageAlloc) + file mmap/munmap + std.args via raw `svc` syscalls (mmap=222, munmap=215, openat=56, lseek=62, close=57), all direct-exe; lifted `args env fs heap` on both linux-arm64 manifests; `is_mapping` 64-bit length; entry stub seeds argc/argv from sp. **65 → 88 ran** (+23). `compared=100 MISCOMPILES=0` (armOnlyBuilds=1: parse-integers, arm-ahead-of-darwin, run-validated). generate-argmax(-q)/transformer-forward(q) token-for-token vs the libm C ref. See "P7 — what landed" |
| P7-fix | obj+link `main` process-args seed (x0/x1→x20/x21) | **done** | obj+link `main` now seeds x20←x0/x21←x1 (object path passes `seed_main_process_args=true`, mirroring emit_elf64.c:3790); P7's entry-stub seed only covered direct-exe, so the libm-using llama2 example (obj+link) segfaulted (exit 139) on argv access under crt0. New regression fixture `std-args-libm` (libm + std.args, the gap that hid it) FAILs before / PASSes after. **llama2 runs in docker** (coherent text == darwin). **88→89 ran**; darwin 87→88; `compared=101 MISCOMPILES=0`. See "P7-fix — what landed" |
| P8 | Wire example + validate.sh linux/arm64 + README; token-for-token | **done** | validate.sh gained a linux/arm64 Docker branch (3rd branch; reuses the whole matrix via the same `build_zero`/`build_ref`/`run_pair` indirections, only the arm64 target/triple/platform differ). **f32 token-for-token PASS (11/11) AND int8 GATED token-for-token PASS (8/8)** vs llama2.c/runq.c in native arm64 docker (`LLAMA2_FORCE_DOCKER=1`). int8 stays gated (NOT informational like darwin) — both sides link zig's musl, so it is bit-exact; no blocker doc needed. README linux-arm64 row added. darwin-native + conformance unregressed. **Campaign complete.** See "P8 — what landed" |

## P1 must-know

1. **Be honest, not permissive.** Replace `a64_return_literal`'s accept-everything behavior. Walk
   the IR; for any construct not yet implemented, `a64_diag(CGEN004, …)` with a precise message
   (mirror the macho backend's per-feature bails). This is what makes the harness tolerate-skip work
   and prevents false greens.
2. **Mirror emit_elf64.c's dual path.** `z_emit_elf_aarch64_object_from_ir` (object, for P3 obj+link)
   and `z_emit_elf_aarch64_exe_from_ir` (direct-exe, syscall-only) share the codegen helpers. P1's
   runnable target is the **direct-exe** path (no relocations needed yet). The object path can stay
   minimal until P3.
3. **Port the integer leaf encoders** from `emit_macho64.c:479-930` under the `a64_` prefix
   (movz/movk/mov_imm64, add/sub/mul, cmp, branches `b`/`b.cond`/`bl`, ldr/str w/x for locals,
   load/store local by frame_offset). Encodings are identical — spot-verify a couple against the
   committed macho output or `clang -c -target aarch64-linux-musl`.
4. **`world.out.write` via raw `svc` write=64.** Needs a readable rodata segment holding the string
   literal + materializing its address (PC-relative ADR in the direct-exe, or absolute from the
   fixed base — the macho exe path `macho_emit_exe_world_write` is the arm64 reference; the ELF
   container/segment layout is the emit_elf64.c reference). `check world.out.write(...)` is the
   common shape in the float fixtures — P1 should support the `check` over `world.out.write` (brk on
   failure, like macho's exe path) so P2's fixtures only block on FP, not on world.write.
5. **exit stays syscall 93 in x8** (`movz x8,#93; svc #0`) — `assertElfAarch64Executable`
   (run.mjs:189) hardcodes those bytes. Integer-42 return stays `movz w0,#42; ret` — also asserted.
6. **Harness branch.** Add `assertLinuxArm64NativeOrUnsupported` (or similar) to run.mjs, wired into
   the same fixture loops the darwin branch uses, gated on `docker` being available. Tolerate-skip
   CGEN004/BLD003 at build; otherwise run in `docker run --platform linux/arm64` and assert the same
   observable (`stdout`/`exitCode`) the darwin/linux checks use. Print `linux-arm64 (docker): N ran,
   M skipped`. Land it WITH the honest backend so it's green.
7. **No regression** to emit_elf64.c / emit_macho64.c (don't touch them) and to target.c beyond what
   P1 needs (P1 needs no target.c change — dispatch + stdio cap already present; the math gate is P3,
   capabilities are P7).

## P1 — what landed

The 346-line permissive-miscompiler MVP is replaced by an **honest** AArch64 ELF backend
(`emit_elf_aarch64.c`, now 1192 lines; **no `target.c`, `ir.c`, `emit_macho64.c`, or `emit_elf64.c`
change** — P1 needs none). The integer/control-flow/`world.write` subset emits correct code; every
other construct bails `CGEN004` with a precise message (or `TAR002`/`BLD003` upstream). The harness
`conformance/run.mjs` gained a `linux/arm64` docker branch. **`conformance ok`; darwin-arm64 native
unchanged at 87 ran / 46 skipped (committed macho path unregressed); `linux-arm64 (docker): 3 ran,
130 skipped`; zero miscompiles** (full sweep: 8 pass-fixtures build, 0 crash, 215 bail honestly).

### Architecture (as decided in P0 — PORT/duplicate, mirror emit_elf64.c container)

The arm64 leaf encoders are ported byte-for-byte from `emit_macho64.c` under the `a64_` prefix; the
container/dispatch structure mirrors `emit_elf64.c`'s obj/exe split. **Proven byte-identical:**
`exit-code-arithmetic`'s `classify()` text (0x88 bytes) from the ELF-exe path equals the committed
Mach-O object's `__text` for the same function (movz/movk, the left-spill `str x8,[sp]`/`ldr x8,[sp]`,
`cmp w`, `b.le`, `cbz`, `sub w`, prologue/epilogue all identical). The verifier:
`bin/zero build --emit exe --target linux-musl-arm64 …/exit-code-arithmetic.0` vs
`bin/zero build --emit obj --target darwin-arm64 …/exit-code-arithmetic.0` + `otool -tv`.

### New `a64_*` helpers (line ranges in emit_elf_aarch64.c)

- **Byte/ELF primitives** (8–63): `a64_append_u{8,16,32,64}`, `a64_append_bytes/zeros`, `a64_align`,
  `a64_pad_to`, `a64_emit_word` (one 32-bit instruction word), `a64_diag`/`a64_diag_at` (CGEN004).
- **Type predicates** (66–105): `a64_type_is_{float,scalar32,scalar64,scalar,unsigned}` (ported from
  macho).
- **Leaf encoders** (108–305, ported from emit_macho64.c:479–693): `a64_emit_movz_w` (110),
  `a64_emit_movz_x`, `a64_emit_mov_imm64` (126, movz/movk run for 64-bit immediates),
  `a64_emit_mov_imm64_fixed` (144) + `a64_patch_mov_imm64_fixed` (162, a **fixed 4-word** run that is
  patched in place — the string-literal-addressing mechanism, see below), `a64_emit_mov_{w,x}`,
  `a64_emit_add_sp_imm`, `a64_emit_binary_int` (184, ADD/SUB/MUL at 32/64-bit width; DIV/MOD/bitwise
  return CGEN004 — P6), `a64_emit_cmp_{w,x}`, `a64_cond_for_compare` (217, signed vs unsigned codes),
  `a64_invert_cond`, `a64_local_slot_offset` + `a64_emit_{load,store}_local_{w,x}` (locals at the top
  of the frame, sp-relative), `a64_emit_{str,ldr}_x_disp` (integer spill slots off sp),
  `a64_emit_{bl,b,b_cond,cbz_w}_placeholder`, `a64_patch_branch26`, `a64_patch_cond19`.
- **Emit context** (309–414): `Aarch64EmitContext` (330; modeled on `ElfEmitContext`, **NOT**
  `MachOEmitContext`) carrying `function_offsets`, same-file `Aarch64CallPatch[]`, string-literal
  `Aarch64RodataPatch[]`, `rodata_addr`, `world_write_used`. `a64_record_call_patch`,
  `a64_record_world_write_patch` (351, world.write call sites carry the sentinel `callee_index ==
  function_count`, resolved to the inline svc-write helper), `a64_record_rodata_patch`,
  `a64_patch_call_patches`, `a64_patch_rodata_patches` (397), `a64_emit_rodata_ptr` (409).
- **Byte view** (420–456): `a64_emit_byte_view_ptr` (string literal → rodata pointer; else CGEN004),
  `a64_emit_byte_view_len` (literal/array constant length; else CGEN004). Spans/slices are P4+.
- **Integer scratch** (462–500): `a64_value_int_depth` / `a64_instrs_int_depth` /
  `a64_int_scratch_bytes` / `a64_int_scratch_slot_offset` — one 16-byte slot per binary/compare
  nesting level (the left-spill discipline; ported from macho's int-depth machinery). No FP scratch
  in P1, so the integer region starts at sp offset 0.
- **Expression dispatch** (507–639): `a64_emit_value_to_reg_depth` (INT/BOOL incl. 64-bit, LOCAL,
  BINARY +/−/× with the left-spill across the right's evaluation, short-circuit `&&`/`||`, COMPARE
  with signed/unsigned + 32/64-bit width, CALL), `a64_emit_call_to_reg_depth` (513, integer args
  x0–x7, `bl`, result moved from x0/w0; record/span/float results+args bail).
- **world.write + instr dispatch** (641–786): `a64_emit_exe_world_write` (641, the inline `movz
  x8,#64; svc #0; movz w0,#0; ret` helper), `a64_emit_world_write` (652, fd→x0/buf→x1/len→x2, `bl`
  helper, `brk #0` on a failed `check`), `a64_emit_epilogue` (671), `a64_emit_instr` (678,
  WORLD_WRITE/LOCAL_SET/EXPR/RETURN incl. raising x1=0 tag/IF/WHILE), `a64_emit_instrs`.
- **Function shape** (789–869): `a64_frame_size`, `a64_is_literal_return_function` +
  `a64_emit_literal_return` (the `movz w0,#42; ret` leaf the conformance assertion hardcodes),
  `a64_validate_function` (780, ≤8 params, Void/integer returns, primitive scalar params/locals only;
  ported from elf64/macho `validate_function`), `a64_emit_function_text` (804, prologue: `stp x29,x30`,
  `sub sp`, spill integer params from x0–x7, optional x20/x21 process-args seed for `main`).
- **rodata + ELF emit** (873–909, ported from emit_elf64.c): `a64_rodata_base_offset`,
  `a64_append_rodata`, `a64_append_symbol`, `a64_append_section_header`, `a64_find_main`.

### ELF container / segment layout (direct-exe path, `z_emit_elf_aarch64_exe_from_ir` @1080)

Mirrors `emit_elf64.c:z_emit_elf64_exe_from_ir` exactly, only machine = 183 and arm64 syscalls:
- `base_addr = 0x400000`, ehdr 64 + a **single** phdr 56 → `text_offset = 120`, `entry = base +
  120`. **One PT_LOAD** (`p_flags = 5`, r-x, align 0x1000) covering ehdr+phdr+text+rodata. `e_phnum
  == 1` and `e_phentsize == 56` are what `assertElfAarch64Executable` (run.mjs) checks — keep one
  segment.
- Layout in the segment: a 16-byte start stub (`bl main; movz x8,#93; svc #0`, padded to 16) →
  function bodies (4-aligned) → the inline svc-write helper (if used) → rodata (8-aligned, appended
  after text). `file_size = rodata_offset + rodata.len`.
- **No relocations**: non-PIE fixed base, so string addresses are absolute constants. The
  `movz w0,#42; ret` (`40 05 80 52 c0 03 5f d6`) and `movz x8,#93; svc #0`
  (`a8 0b 80 d2 01 00 00 d4`) byte sequences asserted by run.mjs are emitted verbatim (exit stays 93
  in x8 — not exit_group=94).

### Object path (`z_emit_elf_aarch64_object_from_ir` @911)

Kept minimal, mirroring the MVP's container (machine 183, `.text`/`.symtab`/`.strtab`/`.shstrtab`,
exported symbol info `0x12`) but now sharing the real codegen helpers. It emits the integer subset
and the exported function symbol, so `assertElfAarch64Object` (machine 183 + `movz w0,#42; ret` +
`main\0`) passes on `examples/direct-exe-return.0`. **It honestly bails** if any same-file `bl` call
patch or string-literal rodata patch was recorded ("does not yet emit relocations for calls or
string literals") — those need `R_AARCH64_CALL26` / `R_AARCH64_ADR_PREL_PG_HI21 +
ADD_ABS_LO12_NC`, which **P3** adds for the libm obj+link path. So the object path covers the
no-call, no-string integer subset today; the direct-exe path carries the full runtime.

### String-literal addressing mechanism (the one non-obvious piece)

`world.write("…")` needs the literal's runtime address, but the rodata offset is only known after the
whole text is laid out. The direct-exe path emits a **fixed 4-word MOVZ/MOVK run**
(`a64_emit_mov_imm64_fixed`, always 16 bytes regardless of value so it can be edited in place),
records an `Aarch64RodataPatch{patch_offset, data_offset}`, and after `rodata_offset` is computed
fills the run with `base_addr + rodata_offset + (data_offset - rodata_base_offset)` via
`a64_patch_mov_imm64_fixed`. This is the arm64 analog of emit_elf64.c's `elf_emit_rodata_ptr_rax`
(`mov rax, imm64` patched by `elf_patch_rodata_patches`) — no ADRP, no relocation, fully
self-contained. (macho's exe path uses PC-relative ADRP+ADD because it is PIE; the ELF exe is
ET_EXEC at a fixed base, so an absolute immediate is simpler and correct.)

### emit_elf64 / macho reference-line corrections

All references used were accurate against the tree at write time:
`elf_emit_rodata_ptr_rax` @948, `elf_patch_rodata_patches` @672, `elf_emit_world_write` @2577 (raw
`syscall` write, the syscall=1/x86 analog of our svc=64), `z_emit_elf64_exe_from_ir` @4326 (the
single-PT_LOAD container blueprint), `elf_find_executable_main` @4271, `elf_validate_function`
@2540, `elf_rodata_base_offset` @3708 / `elf_append_rodata` @3717. macho leaves:
`macho_emit_movz_w` @491, `macho_emit_mov_imm64` @508, `macho_emit_binary_int` @625, the integer
`value_to_reg` left-spill @1596–1605, `macho_cond_for_compare` @1205, `macho_emit_function_text`
prologue @3076, `macho_emit_exe_world_write` @3733. No drift found.

### Harness (`conformance/run.mjs`)

- `dockerLinuxArm64` detected once at startup (`docker run --platform linux/arm64 alpine uname -m` ==
  `aarch64`); the branch no-ops when absent, so non-docker hosts/CI are unaffected (independent of
  `runnableDirectTarget`, which is darwin-arm64 here).
- `assertLinuxArm64NativeOrUnsupported(fixture, name, expected)` — builds `--json --emit exe --target
  linux-musl-arm64`; on build failure asserts the code is `CGEN004`/`BLD003`/`TAR002` (TAR002 = the
  manifest capability gate P7 lifts) and tolerate-skips; on success runs `docker run --rm --platform
  linux/arm64 -v <abs outDir>:/w alpine /w/<exe>` and asserts `expected.exitCode` else
  `expected.stdout` (string|RegExp) + `expected.stderr`. Wired into BOTH runtime-fixture loops next
  to the darwin helper, so fixtures auto-flip skip→run as later phases land.
- Explicit structure assertions added next to the elf64 ones: `assertElfAarch64Object` (on the
  aarch64 object of `direct-exe-return.0`) + `assertElfAarch64Executable` (on its exe), plus a docker
  run asserting exit 42. These were previously defined-but-unused.
- Summary line: `linux-arm64 (docker): 3 ran, 130 skipped`. The 3: **exit-code-arithmetic** (integer
  arithmetic + same-file calls + comparison branches → exit 42), **match-fallback** (control flow +
  multiple `check world.out.write`), **std-codec-widths** (integer ops + `&&` chains + `check
  world.out.write("…ok\n")`). Together they exercise the whole P1 surface. (Other integer fixtures
  that build — generic-specialization-reuse, std-http-errors, params, primitive-stdlib — are not in
  the run.mjs runtime-fixture loops, a pre-existing harness wiring detail, so they aren't counted; a
  manual sweep confirmed all produce correct output.)

### Validation evidence

```
$ make -C native/zero-c                                    # clean, no -Wall -Wextra -Wpedantic warnings
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs   # conformance ok
  darwin-arm64 native: 87 ran, 46 skipped                   # macho path unregressed
  linux-arm64 (docker): 3 ran, 130 skipped                  # honest backend, zero miscompiles
$ bin/zero build --emit exe --target linux-musl-arm64 …/exit-code-arithmetic.0 --out /tmp/exit
$ docker run --rm --platform linux/arm64 -v /tmp:/w alpine /w/exit ; echo $?   → 42
$ bin/zero build --emit exe --target linux-musl-arm64 …/std-codec-widths.0 --out /tmp/cw
$ docker run … /w/cw   → "codec widths ok"
$ bin/zero build --json --emit exe --target linux-musl-arm64 …/float-arith-add-f32.0   → CGEN004 (honest, no miscompile)
# non-regression: linux-musl-x64 (zero-elf64) builds; darwin-arm64 builds + runs as a Mach-O exe
# full sweep of conformance/native/pass: 8 build, 0 crash, 215 bail CGEN004/TAR002
```

### P2 must-know

1. **The FP seam is wide open.** P1 has **no FP register file at all** — `a64_type_is_float` is
   defined and used only to *route to CGEN004 bails*. Every FP construct currently bails with a
   precise message you replace in P2: float locals/params/returns (`a64_validate_function` @780, the
   "primitive integer locals/params/returns" diags), float LOCAL_SET / EXPR / RETURN
   (`a64_emit_instr` @678), float CALL results+args (`a64_emit_call_to_reg_depth` @513), float
   COMPARE (`a64_emit_value_to_reg_depth` COMPARE case @598). Grep the file for
   `does not yet support float` / `floating-point` to find every seam.
2. **Port the macho FP layer wholesale** — it is byte-identical arm64 (the macho P1 doc's "FP
   instruction helpers added" list): `ldr/str s_/d_` (local/field/base/reg-lsl/sp-spill), `fmov`,
   `fmov v,gpr`, `fadd/fsub/fmul/fdiv`, `fcmp`+`cset`, `fcvt`/`fcvtzs`/`scvtf`/`ucvtf`,
   `macho_float_cond_for_compare` (the IEEE-NaN-correct codes), and the
   `macho_emit_float_value_to_vreg_depth` dispatch. Add an **FP scratch region** below the integer
   scratch (P1 put integer scratch at sp offset 0 via `a64_int_scratch_slot_offset` returning
   `depth*16`; P2 inserts `fp_scratch_bytes` before it, exactly like
   `macho_int_scratch_slot_offset = fp_scratch_bytes + depth*16` and `macho_frame_size + fp + int`).
   The frame-scratch (not sp-push) discipline for FP sub-expressions across calls is mandatory — same
   reason as the integer left-spill (sp-relative locals).
3. **FP register ABI:** params spill from v0..v7 (separate counter from x0..x7) in
   `a64_emit_function_text`; FP call args marshal into v0..v7; FP result reads from v0/s0/d0; FP
   return writes v0. Mirror `macho_emit_function_text` / `macho_emit_call_to_reg`.
4. **Fixtures P2 flips** (the macho P1 set, ~37): `float-arith-*` (8), `float-compare-*` (12),
   `float-cast-*` (6), `float-primitives`, `float-{nan-compare,inf-arith}`, `math-isnanf` (inline
   `fcmp v,v; cset vs`), and the scalar-array smoke kernels `math-{rmsnorm,softmax}-smoke`. The
   `*-known-values` libm fixtures need P3 (libm obj+link), not P2 — but P2 lays the FP value emitter
   the libm calls (s0 arg / s0 result) plug into. After P2, the `linux-arm64 (docker)` ran count
   should jump well into the teens; these fixtures are already wired into the run.mjs runtime loops,
   so they auto-flip with no harness edit.
5. **No `target.c` change for P2** (the math gate is P3, capabilities P7). The dispatch already
   routes float programs to `zero-elf-aarch64-exe` on the direct-exe path (FP needs no obj+link until
   libm).

## P2 — what landed

The complete **scalar f32/f64 instruction layer + the AArch64 FP register ABI** in
`emit_elf_aarch64.c` (now 1530 lines, +338). The macho FP layer was ported byte-for-byte under the
`a64_` prefix; the FP CGEN004 seams P1 left open are replaced with correct codegen. **No `target.c`,
`ir.c`, `emit_macho64.c`, `emit_elf64.c`, or `run.mjs` change** — the float fixtures were already
wired into the run.mjs runtime loops by P1, so they auto-flipped skip→run as the backend stopped
bailing CGEN004 (the same auto-flip mechanism the macho campaign used). FP is on the **direct-exe**
path (no relocations, no obj+link — that is P3 for libm).

**Result: `conformance ok`; darwin-arm64 native unchanged at 87 ran / 46 skipped (committed macho
path unregressed); `linux-arm64 (docker): 3 ran → 32 ran, 130 → 101 skipped`; zero miscompiles**
(independent sweep: 37 pass-fixtures build for linux-musl-arm64, all 37 run correct in docker, 186
bail honestly CGEN004/TAR002).

### The flip set — 29 fixtures (3 → 32 ran), all wired, all correct in docker

float-arith-{add,sub,mul,div}-{f32,f64} (8), float-compare-{eq,ne,lt,le,gt,ge}-{f32,f64} (12),
float-cast-{f32-to-f64,f64-to-f32,i32-to-f32,i32-to-f64,f32-to-i32,f64-to-i32} (6),
**float-nan-compare**, **float-inf-arith** (both produce inf/NaN via `x/0` → require **fdiv**, see
below), and **math-isnanf** (inline `fcmp v,v; cset vs`). All print their `"… ok\n"` under
`docker run --platform linux/arm64`.

**Honestly NOT flipped (parity-correct, deferred — these are the macho-P1 set but the linux-arm64
phase order differs):**
- `math-{rmsnorm,softmax}-smoke` — use `[N]f32` array locals **and** `std.math.sqrtf`/`expf` (libm).
  Arrays are P4 and libm is **P3** (obj+link); they bail CGEN004 at the libm call. (The macho P1 doc
  lists these as flipping in its P1 only because the macho campaign did libm + FP together; here libm
  is a separate later phase.)
- `math-*-known-values` (sqrtf/expf/cosf-sinf/powf/absf-floorf) — pure libm, **P3**.
- `float-primitives` — uses a `FloatPair` **record** (f32/f64 fields + FIELD_LOAD); records are the
  aggregate-ABI phase **P5**. Bails CGEN004 `actual: pair`.
- `float-char-casts` — uses a `char` local type, rejected by the **shared** MIR-lowering gate
  (`actual: char`), **on darwin too** — not an FP gap, parity preserved.

### New `a64_*` FP helpers (line ranges in emit_elf_aarch64.c)

- **Type predicate** (91): `a64_type_is_f64`.
- **FP leaf encoders** (337–447, ported byte-identical from emit_macho64.c:712–860): `a64_emit_{ldr,str}_v_local`
  (`ldr/str s_/d_, [sp,#off]`, bit30=s/d, bit22=load/store, size-scaled offset), `a64_emit_{str,ldr}_v_sp`
  (the FP scratch-slot spill/reload), `a64_emit_fmov_v` (v↔v), `a64_emit_fmov_v_from_gpr`
  (`fmov s_,w_`/`fmov d_,x_` — raw bits, materializes a float constant), `a64_emit_float_arith`
  (fadd `0x1e202800` / fsub `0x1e203800` / fmul `0x1e200800` / **fdiv `0x1e201800`**, bit22=s/d),
  `a64_emit_fcmp`, `a64_emit_cset` (CSINC alias, encodes the inverted cond), `a64_emit_fcvt` (s↔d),
  `a64_emit_fcvtzs_w` (f→i32 round-to-zero), `a64_emit_scvtf_from_w` (i32→f), `a64_emit_ucvtf_from_x`
  (u64→f), `a64_float_cond_for_compare` (434; the IEEE-NaN-correct codes EQ/NE/MI/LS/GT/GE so every
  ordering against NaN is false and `!=` is true).
- **FP scratch machinery** (573–629, ported from emit_macho64.c:2078–2137): `a64_fp_scratch_slot_offset`
  (`depth*16`, slots at the bottom of the frame), `a64_value_fp_depth` / `a64_instr{,s}_fp_depth` /
  `a64_fp_scratch_bytes` (max FP binary/compare nesting × 16).
- **FP value dispatch** `a64_emit_float_value_to_vreg(_depth)` (879–959, ported/trimmed from
  emit_macho64.c:2232–2369): FLOAT const (movz/movk→`fmov v,gpr`), LOCAL (`ldr s_/d_`), CAST (i→f via
  scvtf/ucvtf, f→f via fcvt, f→i lands in the integer path), BINARY (fadd/fsub/fmul/fdiv with the
  left-spill-to-FP-scratch across the right's evaluation), CALL (float-returning same-file fn). FIELD_LOAD /
  INDEX_LOAD / codec readF*Le / libm kinds / CHECK bail honestly (P3/P4/P5) — the `default` arm.

### Integer-path hooks (in `a64_emit_value_to_reg_depth`)

- **COMPARE with float operands** (809–822): spill left to FP scratch slot 0, eval right, `fcmp`+`cset`
  with the IEEE codes (mirrors emit_macho64.c:1611–1625; FP depth is independent of the integer depth
  counter, so depth 0 is correct here).
- **CAST f→i** (840–851): `fcvtzs w, v` (round toward zero, the `as i32` contract).
- **`IR_VALUE_MATH_ISNANF`** (852–858): `fcmp v,v; cset reg, vs` (V=1 ⇒ unordered ⇒ NaN) — inline, no
  libm, matching ELF/macho.

### Instruction hooks (in `a64_emit_instr`)

- **float LOCAL_SET** (1009–1014): materialize into v8 via the vreg emitter, `str s_/d_` to the slot
  (float dropped from the `LOCAL_SET` bail-list).
- **float EXPR** (1020–1023): route through `a64_emit_float_value_to_vreg` into v0.
- **float RETURN** (1025–1043): emit into **v0** (s0/d0); a raising float fn still sets the `x1` error
  tag = 0 alongside the v0 result (the AAPCS tag rides in x1, separate from v0, so the float case sets
  it too).

### FP register ABI

- **`a64_emit_call_to_reg_depth`** (705–739): FP args marshal into **v0..v7** via a separate `fp_arg`
  counter (integers still x0..x7 via `int_arg`); the float result is read from **v0** (`fmov vreg, v0`).
  Span/record args + record/span results still bail (P5).
- **`a64_emit_function_text`** (1133–1164): float params spill from **v0..v7** (separate counter) in the
  prologue. Mirrors `macho_emit_function_text` / `macho_emit_call_to_reg`.
- **`a64_validate_function`** (1108–1130): now admits primitive **scalar (integer or float)** params,
  locals, and returns (record/span still rejected).

### Frame-layout change (the mandatory pitfall — FP scratch BELOW integer scratch)

Locals are sp-relative, so an FP sub-expression must NOT spill via an sp-push. P2 inserts an FP scratch
region at the **bottom** of the frame and shifts the integer scratch up by exactly its size, matching
macho's `frame = base + fp + int`:
- `a64_int_scratch_slot_offset` (686) changed signature `(unsigned depth)` → `(const IrFunction *fun,
  unsigned depth)` and now returns `a64_fp_scratch_bytes(fun) + depth*16` (was `depth*16`). Both call
  sites (the integer BINARY and COMPARE cases) pass `fun`.
- `a64_emit_function_text`'s `frame_size` is now `a64_frame_size(fun) + a64_fp_scratch_bytes(fun) +
  a64_int_scratch_bytes(fun)` (was `+ a64_int_scratch_bytes` only). FP scratch lives at
  `[0, fp_scratch_bytes)` (slot = `depth*16`), integer scratch just above it.
- A float binary/compare/call-arg spills its left/arg to its FP depth slot across the sibling's
  evaluation (`a64_emit_str_v_sp` / `a64_emit_ldr_v_sp`); the FP depth threads through the
  `_depth` variants. Verified the P1 integer fixtures still pass after the shift (exit-code-arithmetic
  → 42, std-codec-widths, match-fallback all correct in docker).

### FP-encoding byte-identity spot-check (cite the bytes)

`float-arith-add-f32` built `--emit exe --target linux-musl-arm64` vs the committed macho object
(`--emit obj --target darwin-arm64` + `otool -t`) — the FP instruction words are **identical**:
`1e270108` (fmov s8,w8), `bd002be8`/`bd0023e8` (str s8), `bd402be8`/`bd4023e9`/`bd4003e8` (ldr s8/s9),
`1e292908` (fadd s8,s8,s9), `1e292100` (fcmp s8,s9), `1a9f17e0` (cset). The f64 family matches too
(`float-arith-add-f64`): `9e670108` (fmov d8,x8), `fd0017e8`/`fd0013e8` (str d8),
`fd4017e8`/`fd4013e9`/`fd4003e8` (ldr d8/d9), `1e692908` (fadd d8,d8,d9), `1e692100` (fcmp d8,d9). Only
the container differs (ELF vs Mach-O), exactly the keystone insight.

### macho/elf64 reference-line corrections

All macho references used were accurate against the tree at write time: FP leaf encoders
emit_macho64.c:712–860, `macho_float_cond_for_compare` @1226, the integer-path float COMPARE hook
@1611–1625, CAST(f→i) @1996–2005, `IR_VALUE_MATH_ISNANF` @2007–2011, `macho_fp_scratch_*` /
`macho_value_fp_depth` @2078–2137, `macho_emit_float_value_to_vreg_depth` @2232–2369,
`macho_emit_call_to_reg_depth` FP result/args @1511–1535 + the marshaller @2436–2459,
`macho_emit_function_text` FP param spill @3107–3127, `macho_int_scratch_slot_offset =
macho_fp_scratch_bytes + depth*16` @2208. No drift found.

### Validation evidence

```
$ make -C native/zero-c                                     # clean, no -Wall -Wextra -Wpedantic warnings
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs    # conformance ok
  darwin-arm64 native: 87 ran, 46 skipped                    # macho path unregressed
  linux-arm64 (docker): 32 ran, 101 skipped                  # +29 from P1's 3; zero miscompiles
$ docker run … /w/float-arith-add-f32   → "float add f32 ok"
$ docker run … /w/float-arith-div-f64   → "float div f64 ok"     # fdiv
$ docker run … /w/float-compare-lt-f32  → "float lt f32 ok"      # fcmp + cset
$ docker run … /w/float-cast-i32-to-f32 → "float cast i32 to f32 ok"   # scvtf
$ docker run … /w/float-cast-f32-to-i32 → "float cast f32 to i32 ok"   # fcvtzs
$ docker run … /w/float-nan-compare     → "float nan compare ok"  # IEEE NaN codes + fdiv
$ docker run … /w/float-inf-arith       → "float inf arith ok"    # inf via fdiv
$ docker run … /w/math-isnanf           → "math isNaNf ok"        # inline fcmp v,v; cset vs
# deferred-but-honest: math-rmsnorm-smoke/math-sqrtf-known-values → CGEN004 (libm, P3);
#   float-primitives → CGEN004 (record, P5); float-char-casts → CGEN004 (char, shared MIR gate)
# non-regression: P1 exit-code-arithmetic → 42, std-codec-widths/match-fallback correct;
#   linux-musl-x64 (zero-elf64) builds (ELF 64-bit LSB); darwin-arm64 float-arith-add-f32 runs native
# independent sweep: 37 build for linux-musl-arm64, 37 run correct (1 is a correct exit-0 no-stdout
#   program: std-http-errors), 0 miscompiles
```

### P3 must-know

1. **The FP value emitter is the s0-arg / s0-result plug, ready now.** P2's
   `a64_emit_float_value_to_vreg_depth` already materializes a float into any v-register and reads a
   float result from v0. P3's libm path is: evaluate the arg into **s0** (the single-arg shape) or
   **s0/s1** (powf), emit a `bl` placeholder, record a math-symbol patch, and `fmov vreg, s0` for the
   result — exactly the macho `IR_VALUE_MATH_{SQRTF,…,POWF}` cases (emit_macho64.c:2317–2344). The FP
   depth/scratch machinery already survives a `bl` (the spill is frame memory, not a caller-saved reg).
   These are the value-dispatch arms P3 adds to the `default` of `a64_emit_float_value_to_vreg_depth`
   (and `IR_VALUE_CHECK` for raising float calls — the macho version @2303–2316). The libm symbols are
   **bare** ELF names (`sqrtf`, `expf`, … — NOT macho's `_`-prefixed form).
2. **libm forces obj+link → P3 must add the aarch64 object reloc path + a target-aware dispatch.** The
   direct-exe path emits a `bl` placeholder that branches to itself (hang) if unbound, so a libm program
   MUST take `z_emit_elf_aarch64_object_from_ir` + `zig cc -target aarch64-linux-musl` link. P3 adds:
   - **`R_AARCH64_CALL26`** relocations for the libm `bl` sites in the object path (the object path
     currently *bails* "does not yet emit relocations for calls or string literals" at the
     `call_patch_len > 0 || rodata_patch_len > 0` guard, emit_elf_aarch64.c:~983 region — replace that
     bail with reloc emission). Mirror emit_elf64.c's per-symbol math patch arrays
     (`runtime_math_*_patches`) but with `R_AARCH64_CALL26` (and `R_AARCH64_ADR_PREL_PG_HI21 +
     ADD_ABS_LO12_NC` for any string literal the object body addresses — same-file `bl` between Zero
     functions also needs CALL26 now).
   - **`target_math_runtime_supported`** (target.c:437–443) must additionally allow
     `zero-elf-aarch64` + linux (currently only `zero-elf64`+linux / `zero-macho64`+macos).
   - **The dispatch gate** (`ir_needs_zero_runtime_object`, main.c) already returns true for the math IR
     values on the ELF object emitter — confirm `zero-elf-aarch64` routes through obj+link the same way
     `zero-elf64` does (the macho campaign added a target-aware branch for *mmap*; libm on ELF is
     already object-routed by `ir_needs_zero_runtime_object` returning true on the math kinds, so verify
     the aarch64 object emitter is selected and `-lm` is appended — main.c link flags).
3. **Fixtures P3 flips:** `math-{sqrtf,expf,cosf-sinf,powf,absf-floorf}-known-values` (6) and — because
   they were blocked on libm, not arrays — re-check `math-{rmsnorm,softmax}-smoke` (they ALSO need
   `[N]f32` arrays from **P4**, so they flip only once BOTH P3 and P4 land; the `*-known-values` are the
   pure-P3 wins). The `*-span` kernels need P4 (typed spans) + P5 (span params) + P3 (libm) together.
4. **Same musl libm both sides → token-for-token for f32 AND int8** is expected on linux-arm64 (no
   per-platform caveat like macOS, since the Zero exe and the C ref both link zig's bundled musl). Build
   the C parity ref with `zig cc -target aarch64-linux-musl` (the Zero-parity-libm-musl rule).

## P3 — what landed

**libm via obj+link with R_AARCH64 relocations.** The AArch64 ELF object path graduated from a
relocations bail to a real relocatable object, and the obj+link *executor* in `main.c` learned the
`zero-elf-aarch64` emitter. Changes: `emit_elf_aarch64.c` (+236, 1530 → 1766), `target.c` (the math
gate + JSON), `main.c` (the obj+link dispatch executor — the one genuine gap). **No `ir.c`,
`emit_elf64.c`, `emit_macho64.c`, or `run.mjs` change** — the 5 libm fixtures were already wired into
the run.mjs runtime loops by P1, so they auto-flipped skip→run as the backend stopped bailing.

**Result: `conformance ok`; darwin-arm64 native unchanged at 87 ran / 46 skipped (committed macho
path unregressed); `linux-arm64 (docker): 32 ran → 37 ran, 101 → 96 skipped`; zero miscompiles**
(independent sweep: 42 pass-fixtures build for linux-musl-arm64, all 42 run, 41 print their `… ok`,
1 is a correct exit-0-no-stdout program — `std-http-errors`, `export c fun main()->i32 { return 0 }`,
no `world.write`; 181 bail honestly CGEN004/TAR002).

### The flip set — 5 fixtures (32 → 37 ran), all libm, all correct in docker

`math-{sqrtf,expf,cosf-sinf-identity,powf,absf-floorf}-known-values`. Each calls `std.math.*` (libm)
and prints `check world.out.write("…")`, so each exercises **both** new reloc kinds (CALL26 for the
libm `bl`, ADRP+ADD for the string). All print their `"… ok\n"` under `docker run --platform
linux/arm64` with correct numerics (sqrtf(4)=2 / sqrtf(-1)=NaN, expf(0)=1 / expf(1)∈(2.71,2.72),
cos²+sin²=1 identity + piF, powf(2,10)=1024 / powf(0,0)=1, absf/floorf with negatives).
`math-isnanf` was **already** in P2's 32 (inline `fcmp v,v; cset vs`, no libm). Honestly NOT flipped
(parity-correct, deferred): `math-{rmsnorm,softmax}-smoke` still bail CGEN004 — they use `[N]f32`
**array** locals (P4); they bail at the array, *before* the libm call, so they need P3+P4 together.
The `*-span` kernels need P4 (typed spans) + P5 (span params) + P3 (libm).

### libm value arms (`a64_emit_float_value_to_vreg_depth`, emit_elf_aarch64.c)

Ported byte-for-byte from emit_macho64.c's `IR_VALUE_MATH_*` / `IR_VALUE_CHECK` arms (the bare-ELF
symbol names replace macho's `_`-prefixed form):
- **`IR_VALUE_MATH_{SQRTF,EXPF,COSF,SINF,FABSF,FLOORF}`** (:1038): single-arg libm. Evaluate the arg
  into **s0**, emit a `bl` placeholder, `a64_record_math_call_patch`, `fmov vreg, s0` for the result.
  All std.math libm helpers operate on f32 (the result `fmov` uses the 32-bit form).
- **`IR_VALUE_MATH_POWF`** (:1055): two-arg. Compute arg0 into **s0**, spill it to the FP depth
  scratch slot, compute arg1 into **s1** (its evaluation may itself call libm — the spill is frame
  memory and survives), reload s0, `bl powf`. The FP scratch survives the `bl` because it is frame
  memory, not a caller-saved register (the P2 frame-scratch discipline, unchanged).
- **`IR_VALUE_CHECK`** (:1024): a `check <float fallible call>` — evaluate into v0 (tag in x1),
  `cbz w1, ok`, on a nonzero tag run the epilogue to propagate it, else `fmov vreg, v0`. Ported for
  parity (the macho P2b arm); **not exercised by any P3 fixture** (the 5 use `check world.out.write`,
  which the IR special-cases to a brk-on-fail WORLD_WRITE), so it is parity-ready, not load-bearing.
- `nanF/infinityF/piF/eF` are pure f32 consts (already `IR_VALUE_FLOAT` — no libm); `isNaNf` stays
  inline. The default arm now bails only on FIELD_LOAD/INDEX_LOAD/codec readF*Le (P4).

### The math-symbol table (emit_elf_aarch64.c:466–535)

A single symbol-keyed table — `Aarch64MathSymbol` enum + `a64_math_symbol_names[]`
(`sqrtf expf cosf sinf powf fabsf floorf`, bare ELF) + one `Aarch64MathCallPatch{patch_offset,
symbol}` array on the ctx + `a64_record_math_call_patch` / `a64_math_symbol_used` /
`a64_math_symbol_for_value`. This mirrors **emit_macho64.c's `MachOMathSymbol` table** (cleaner than
emit_elf64.c's seven parallel `runtime_math_*_patches` arrays while emitting the identical per-symbol
relocations). `a64_free_ctx` frees it.

### Object-path relocations (`z_emit_elf_aarch64_object_from_ir`, emit_elf_aarch64.c:1411)

The bail (`call_patch_len>0 || rodata_patch_len>0`) is replaced by a real relocatable object that
mirrors emit_elf64.c's object path. **Section layout** (verified via `readelf -SW`): `.text` (AX,
idx 1) → `.rodata` (A, idx 2, present iff the program has readonly data) → `.rela.text` (RELA, idx 3,
present iff any reloc) → `.symtab` (idx 4) → `.strtab` → `.shstrtab`. `e_type = ET_REL (1)`,
`e_machine = 183`.

- **`.rela.text`** — one `Elf64_Rela` = `r_offset (u64)`, `r_info (sym<<32 | type)`, `r_addend (i64)`
  via `a64_append_rela` (:1335, the AArch64 analog of `elf_append_rela`). Emission (:1525–1532):
  - **string literals** → the ADRP word gets **R_AARCH64_ADR_PREL_PG_HI21 = 275**, the ADD word
    (offset+4) gets **R_AARCH64_ADD_ABS_LO12_NC = 277**, both against the `.rodata` **section symbol**
    (index 1) with `addend = data_offset − rodata_base_offset` (the byte offset of the literal within
    `.rodata`). The host linker fills the immhi/immlo (ADRP) and imm12 (ADD); the backend leaves them
    zero (`a64_emit_adrp` / `a64_emit_add_imm_lo12_placeholder`, :590/:594).
  - **libm `bl` sites** → **R_AARCH64_CALL26 = 283** against the undefined external symbol, addend 0.
- **`.rodata` addressing is path-aware** (`a64_emit_rodata_ptr`, :604): the object path
  (`ctx.emit_rodata_relocations = true`, :1467) emits **ADRP+ADD + relocs** (relocatable, not
  fixed-base); the direct-exe path keeps its absolute 4-word MOVZ/MOVK run patched in place (ET_EXEC,
  fixed base 0x400000 — unchanged from P1). This is the AArch64 analog of emit_elf64.c's
  `elf_emit_rodata_ptr_rax` (reloc in the object, absolute imm in the exe).
- **Same-file Zero→Zero `bl` and the inline svc-write helper are resolved IN PLACE** (PC-relative
  within `.text`) via `a64_patch_call_patches` + the world-write-sentinel loop (:1505–1517), exactly
  like emit_elf64.c's `elf_patch_call_patches` — **no relocation** is emitted for intra-`.text`
  calls. world.write is the same self-contained `svc` write helper the direct-exe path uses (`movz
  x8,#64; svc #0`), appended after the function bodies — **no external libc symbol**, unlike macho's
  `_zero_world_write` external. So the object needs only crt0 + libm from `zig cc`.
- **`.symtab`** (:1545–1551): null(0); the `.rodata` **section symbol** (STT_SECTION `0x03`, shndx 2)
  at index 1 iff `has_rodata`; every defined function as **GLOBAL|FUNC `0x12`** at shndx 1 (kept
  global even when non-exported — the emit_elf64.c lesson: a local symbol past `.symtab`'s `sh_info`
  is rejected by the linker; intra-`.text` calls are PC-relative so binding is informational); then
  the undefined libm externals (`0x12`, shndx 0). `.symtab` `sh_info` = the function-symbol base
  (first global = 1, or 2 with a rodata section symbol); `.rela.text` `sh_info` = `.text` (1),
  `sh_link` = `.symtab`.

### target.c

`target_math_runtime_supported` (:437) now also allows `zero-elf-aarch64` + `os==linux` (mirrors the
existing `zero-elf64`+linux). `z_append_math_runtime_json` reports the libm provider with
`systemLibraries:["m"]` for aarch64-linux (same as elf64-linux; macho keeps `["System"]`), and the
unsupported-reason check admits the new emitter. **Conformance's `linuxArm64Target` targets-JSON
assertions (run.mjs:1555–1558) do NOT assert `mathRuntime`** — they check `directBackend` only — so
the gate change needs no run.mjs edit and breaks nothing.

### The genuine gap fixed in main.c (the dispatch executor)

The dispatch *decision* was already correct: `ir_value_needs_zero_runtime_object` (main.c:4189)
returns true for the math IR kinds **regardless of target**, so a libm program on `zero-elf-aarch64`
routes to obj+link, and `link_zero_runtime_executable` (main.c:623) appends `-lm` (:635) and links
via the target's `aarch64-linux-musl` toolchain. But the obj+link **executor** gated
`runtime_object_emitter_supported` to `zero-macho64`/`zero-elf64` only, and the object-emission
ternary branched only those two — so an aarch64 libm program bailed CGEN004 "runtime helpers
currently require the Mach-O or ELF64 object link plan". Fixed minimally at **three sites**:
`target_readiness_select_diag` (main.c:8420), the main build/run flow gate (main.c:9532), and the
object-emission ternary (main.c:9552 → now calls `z_emit_elf_aarch64_object_from_ir`). Also added the
emitted-object cache tag (main.c:9580) + the diag wording ("Mach-O or ELF object link plan"). The
`direct-elf-aarch64-object` plan-JSON path label was already present (main.c:4428, from P1).

### Verified reloc dump (objdump -r / readelf -SW, via docker binutils on linux/arm64)

```
# math-sqrtf-known-values.o
00000000000000f8 R_AARCH64_ADR_PREL_PG_HI21 .rodata     # the world.write string (ADRP)
00000000000000fc R_AARCH64_ADD_ABS_LO12_NC  .rodata     # +4 (ADD)
0000000000000020 R_AARCH64_CALL26           sqrtf        # 3× sqrtf calls
000000000000003c R_AARCH64_CALL26           sqrtf
0000000000000070 R_AARCH64_CALL26           sqrtf
SYMBOLS: .rodata (l d), main (g F .text size 0x124), sqrtf (*UND*)
# math-cosf-sinf-identity.o — two DISTINCT externals, each indexed correctly:
… R_AARCH64_CALL26 cosf ; … R_AARCH64_CALL26 sinf ; (×2 each) ; cosf+sinf both *UND*
# math-powf-known-values.o sections:
 [1] .text   AX ; [2] .rodata A ; [3] .rela.text RELA  link=4 info=1 ; [4] .symtab  link=5 info=2
# linked sqrtf exe (objdump -d): bl <compiler_rt.sqrt.sqrtf> ×3 ; adrp x1,… ; add x1,x1,#0x0
# direct-exe-return.o (integer subset, no libm/string): only .text/.symtab/.strtab/.shstrtab,
#   main GLOBAL FUNC size 8 — assertElfAarch64Object stays green.
```

### Validation evidence

```
$ make -C native/zero-c                                     # clean, no -Wall -Wextra -Wpedantic warnings
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs    # conformance ok
  darwin-arm64 native: 87 ran, 46 skipped                    # macho path unregressed
  linux-arm64 (docker): 37 ran, 96 skipped                   # +5 from P2's 32; zero miscompiles
$ docker run … /w/sqrtf       → math sqrtf ok      # CALL26 sqrtf + ADRP/ADD string
$ docker run … /w/expf        → math expf ok
$ docker run … /w/cosf-sinf   → math cosf sinf ok  # two distinct libm externals
$ docker run … /w/powf        → math powf ok       # two-arg s0/s1 + spill across bl
$ docker run … /w/absf-floorf → math absf floorf ok
# non-regression: float-arith-add-f32 builds 346 B in 1 ms (direct-exe, NO obj+link/link) → "float add f32 ok";
#   darwin-arm64 sqrtf native (Mach-O) → math sqrtf ok ; linux-musl-x64 powf (docker amd64) → math powf ok;
#   math-{rmsnorm,softmax}-smoke → CGEN004 (arrays, P4); math-isnanf still → math isNaNf ok
# independent sweep: 42 build for linux-musl-arm64, 42 run, 0 miscompiles (1 legit exit-0-no-stdout)
```

### emit_elf64 / macho reference-line corrections

All references used were accurate against the tree at write time. **emit_elf64.c**
(x86-64 R_X86_64 model): `z_emit_elf64_object_from_ir` @3738, `elf_append_rela` @3702 (`r_info =
sym<<32 | type`), `elf_emit_rodata_ptr_rax` @948 (reloc-vs-absolute on `emit_rodata_relocations`),
`elf_patch_call_patches` @665 (in-place rel32, the same-file-call model — no per-call reloc), the
rodata reloc emit @3977–3979 (`data_offset − rodata_base_offset` addend), the per-symbol math patch
arrays @515–535 / recorder @651–660 / rela emit @4061–4081 / symtab @4133–4153, the all-global
function-symbol comment @4083–4092, section layout @4181–4235. **emit_macho64.c** (arm64 instruction
model): math/check value arms @2303–2344, `macho_emit_rodata_ptr_literal` @1320 (the PIE ADRP+ADD
form), `macho_patch_adrp_add` @914 (the ADRP immhi/immlo + ADD imm12 bit layout), the PAGE21/PAGEOFF12
reloc pair @1187–1189. No drift found.

## P4 must-know

1. **The byte-view/typed-span seam is the next wall, and it is wide.** P3 left
   `a64_emit_byte_view_ptr` / `a64_emit_byte_view_len` (emit_elf_aarch64.c, ~:560/:570) supporting
   ONLY a string literal (rodata ptr + const len). Every other byte-view shape — `IR_VALUE_LOCAL`
   over a `IR_TYPE_BYTE_VIEW`, `IR_VALUE_BYTE_VIEW_REINTERPRET`, `IR_VALUE_BYTE_SLICE`,
   `IR_VALUE_ARRAY_BYTE_VIEW` — bails CGEN004 there. The float `default` arm and the integer
   `IR_VALUE_INDEX_LOAD` / `IR_VALUE_BYTE_VIEW_READ_{INT,FLOAT}_LE` paths also bail. P4 ports the
   **emit_macho64.c P3 layer** (its "## P3 — what landed", the committed macho doc): `reinterpret`
   (`lsr` len by log2 elemSize), the bounds-checked element-address primitive
   (`macho_emit_span_index_addr` → `a64_emit_span_index_addr`: idx→w8, `cmp idx,len`+`brk` on OOB,
   `ptr + idx*elemSize` in x9), typed-span index load/store for every width (u8 `ldrb`, i8 `ldrsb`,
   i32/u32 `ldr w`, 8-byte `ldr x`, f32/f64 `ldr s_/d_`), and the codec LE reads
   (`READ_INT_LE`/`READ_FLOAT_LE`: `add offset,#size`, bounds-check `≤ len` cond LS + brk, little-
   endian load at ptr+offset). The element-size helpers (`a64_elem_byte_size`/`a64_elem_log2`) and
   the small load/store leaves (`ldrsb`, `ldr/str w`, `str x`, `lsr w`, `ldr/str s_/d_ [base]`) are
   net-new but tiny — all in emit_macho64.c at ~586–680 / ~1117. The **span len convention** is a
   **32-bit element COUNT** at slot+8 (P3/P4 macho convention; widen to 64-bit only for mappings,
   which is P7's `is_mapping` flag — out of P4).
2. **Fixtures P4 flips:** `mem-bytes-as-f32`, `mem-bytes-as-i8`, `codec-read-{f32,f64,i32,u32}-le`,
   `codec-read-{f32,i32}-le-offset`, `mem-mut-span-array-store` (the macho-P3 set, ~9). The
   `mem-bytes-as-{mut-*,…}` and `mem-mut-span-{u8-store,slice}` fixtures additionally need
   `std.mem.pageAlloc` (**P7**), and `typed-span-{f32,i32}` need **span params** (**P5**), so they do
   NOT flip on P4 alone. **`math-{rmsnorm,softmax}-smoke` need P3 (libm, done) + P4 (`[N]f32` arrays)
   TOGETHER** — they are the first kernels that combine both, so they flip in P4 (the libm side is
   already in place from this phase; verify they run, don't just build). The bounds-trap fixtures
   exit 133 (SIGTRAP via `brk #0`) — exercise the span check with a **runtime** index (a const index
   into a typed array is caught at compile time).
3. **Reuse the P3 reloc machinery for nothing new** — P4 is pure instruction selection on the
   direct-exe path (arrays/spans/codec are syscall-free); no new relocations, no obj+link. A P4 kernel
   that ALSO calls libm (the smoke kernels) already routes to obj+link via this phase's dispatch, and
   its string/CALL26 relocs already work. The IR shape to know (from the macho P3 doc): a typed-span
   element access lowers to `IR_VALUE_INDEX_LOAD`/`IR_INSTR_INDEX_STORE` with the BYTE_VIEW local index
   + `element_type` on the local; u8 stays on `IR_VALUE_BYTE_VIEW_INDEX_LOAD`; reinterpret-derived
   span locals get `element_type` from the declared `Span<T>`/`MutSpan<T>`.

## P4 — what landed

The **byte-view reinterpret + typed-span/array index load/store + codec LE** layer in
`emit_elf_aarch64.c` (now 2329 lines, +563). The macho P3 layer was ported byte-for-byte under the
`a64_` prefix; the byte-view/codec/INDEX seams P3 left bailing are replaced with correct codegen, and
fixed-array + span locals are now admitted. **No `target.c`, `ir.c`, `main.c`, `emit_macho64.c`,
`emit_elf64.c`, or `run.mjs` change** — every flipped fixture was already wired into the run.mjs
runtime loops by P1, so they auto-flipped skip→run as the backend stopped bailing CGEN004 (the same
auto-flip mechanism the macho campaign used). P4 is **pure instruction selection on the direct-exe
path** (arrays/spans/codec are syscall-free — no new relocations); the two smoke kernels that ALSO
call libm route to obj+link through P3's already-working dispatch.

**Result: `conformance ok`; darwin-arm64 native unchanged at 87 ran / 46 skipped (committed macho
path unregressed); `linux-arm64 (docker): 37 ran → 49 ran, 96 → 84 skipped`; zero miscompiles**
(independent sweep: 55 pass-fixtures build for linux-musl-arm64, all 55 run in docker, 0 miscompiles —
49 print their `… ok`/expected stdout, 2 are correct exit-0-no-stdout programs `array-repeat-literal`
+ `std-http-errors`, `exit-code-arithmetic` exits 42 by design; 168 bail honestly CGEN004/TAR002/BLD003).

### The flip set — 12 fixtures (37 → 49 ran), all correct in docker

`codec-read-{f32,f64,i32,u32}-le` (4), `codec-read-{f32,i32}-le-offset` (2), **`math-rmsnorm-smoke`**
+ **`math-softmax-smoke`** (the first kernels combining P3 libm + P4 `[N]f32` arrays — they **RUN**,
not just build: `math rmsnorm ok` / `math softmax ok` via obj+link with `sqrtf`/`expf`),
`mem-bytes-as-f32`, `mem-bytes-as-i8`, `mem-mut-span-array-store`, and **`indexed-mutation`** (a
`[N]u8`/`[N]i32` array fixture with a runtime index, newly buildable once array INDEX_LOAD/STORE
landed). `array-repeat-literal` also builds (exit-0-no-stdout `export c fun main()->i32`) but is not
in the run.mjs runtime loops, so it is not counted (the unwired category, like params/primitive-stdlib).

**Honestly NOT flipped (parity-correct, deferred):**
- `typed-span-{f32,i32}`, `mem-mut-span-slice` (and the `*-span` kernels math-{matmul,rmsnorm,softmax,
  swiglu,rope}-span) — bail on **span PARAMETERS** (`a64_validate_function` rejects a byte-view param;
  the prologue does not yet spill `(ptr,len)` from two x-regs). **P5.** The per-element typed-span
  codegen they need is done here; only the param ABI is missing.
- `mem-bytes-as-{mut-f32,f64,i32,mut-i32,mut-i8}`, `mem-mut-span-u8-store`, `mem-bytes-as-f32-mmap` —
  bail on `std.mem.pageAlloc` (the LOCAL_SET `IR_TYPE_ALLOC` branch). **P7.** They reinterpret + index a
  mapped `MutSpan<u8>` with the code that landed here once the allocator exists.

### New `a64_*` helpers (line ranges in emit_elf_aarch64.c)

- **Small load/store leaves** (:287–349, ported byte-identical from emit_macho64.c:651–693):
  `a64_emit_ldrb_w` (u8 zero-extend), `a64_emit_ldrsb_w` (i8 sign-extend), `a64_emit_ldr_w`/`a64_emit_str_w`
  (i32/u32 + codec, base+#0), `a64_emit_str_x` (8-byte store; the `ldr x` form is the existing
  `a64_emit_ldr_x_disp` at offset 0), `a64_emit_strb_w`, `a64_emit_lsr_w_imm` (the reinterpret count
  shift, UBFM alias), `a64_emit_add_x_reg` / `a64_emit_add_x_reg_lsl` (ptr + idx, scaled by elem log2),
  `a64_emit_add_{x,w}_imm` / `a64_emit_sub_w_imm` (slice-start/codec-offset/sliced-len adjustments),
  `a64_emit_add_x_sp_imm` (fixed-array base address). Plus `a64_emit_load_local_b` (:266, LDRB for an
  unaligned u8/Bool record field — the FIELD_LOAD path).
- **FP base/reg-lsl leaves** (:447–464, ported from emit_macho64.c:737–756): `a64_emit_{ldr,str}_v_base`
  (`ldr/str s_/d_,[xbase]` #0 — the FP sibling of the GPR base loads, used by float span index + codec
  readF*Le), `a64_emit_{ldr,str}_v_reg_lsl` (`ldr/str s_/d_,[xbase,xidx,lsl #log2]` — fixed [N]f32/[N]f64
  array element access).
- **Element-size helpers** (:471/:486, keyed off IrTypeKind): `a64_elem_byte_size` / `a64_elem_log2`.
- **Byte-view const helpers** (:761–833): `a64_const_u32_value`, `a64_byte_view_const_len` (literal/array
  count, const-bounded slice, reinterpret = base bytes / elemSize), `a64_readonly_data_byte` +
  `a64_byte_view_const_byte` (compile-time fold of a `view[const]` u8 read).

### Byte-view ptr/len — every shape (`a64_emit_byte_view_ptr_depth` :834 / `_len_depth` :889)

The P1 string-literal-only `a64_emit_byte_view_{ptr,len}` are now `_depth` variants (the non-depth names
are depth-0 wrappers; the existing instruction-level callers are unchanged). `depth` threads through so a
byte-view sub-expression evaluated while outer integer left operands are spilled uses slots at or above
`depth` (mirrors emit_macho64.c's `_depth` byte-view helpers — needed because span indexing materializes
the index into x8). Shapes handled, mirroring the macho/elf reinterpret path:
- **ptr:** `IR_VALUE_LOCAL` over a BYTE_VIEW (ptr@slot+0, `ldr x`), span FIELD_LOAD of a record
  (ptr@field_offset), `IR_VALUE_ARRAY_BYTE_VIEW` (array base via `add x,sp,#off`; all primitive element
  types u8/i8/i32/u32/i64/u64/usize/f32/f64 accepted — the relaxed gate), STRING_LITERAL (rodata ptr),
  `IR_VALUE_BYTE_SLICE` (recurse on `left`, then advance the ptr by start*elemSize — const via
  `add #imm`, runtime via `add x,x,x,lsl #log2`; the **slice start scales, the len stays a count**),
  `IR_VALUE_BYTE_VIEW_REINTERPRET` (same ptr — recurse on `left`).
- **len:** literal/array const count, BYTE_VIEW LOCAL (`ldr w` @slot+8, the **32-bit element count**),
  span FIELD_LOAD (`ldr w` @field_offset+8), BYTE_SLICE (const → movz; const-start+runtime-end → eval
  end then `sub #start`; runtime both → `sub`), REINTERPRET (recurse on `left`, then `lsr w,#log2` —
  no shift for 1-byte u8/i8).

### The keystone — `a64_emit_span_index_addr_depth` (:947)

Byte-identical to `macho_emit_span_index_addr`: materialize the index into **w8**
(`a64_emit_value_to_reg_depth`), load len@slot+8 into **w9** (`ldr w`), `cmp w8,w9`, `b.cc` (cond 3, LO
= idx<len) over a **`brk #0`** (OOB trap), load ptr@slot+0 into **x9** (`ldr x`), then `add x9,x9,x8,lsl
#log2` (or plain `add` for 1-byte). Returns the element address in **x9** (callers must not rely on x8
surviving). Used by every typed-span index site (integer load, float load, store).

### Index load/store dispatch (the seam P5/P7 reuse)

- **Integer `a64_emit_value_to_reg_depth`** new arms (:1261–1373): `IR_VALUE_FIELD_LOAD` (scalar record
  field by width — u8/Bool `ldrb`, 8-byte `ldr x`, else `ldr w`), `IR_VALUE_BYTE_VIEW_LEN`
  (`std.mem.len`), `IR_VALUE_BYTE_VIEW_INDEX_LOAD` (u8 byte-granular: const-byte fold else runtime
  bounds-check + `ldrb`), `IR_VALUE_BYTE_VIEW_READ_INT_LE` (readI32Le/readU32Le: eval offset → `add
  #4` → bounds-check `≤ len` cond LS + brk → `ldr w` at ptr+offset), `IR_VALUE_INDEX_LOAD` (BYTE_VIEW
  span → span_index_addr + width-keyed load `ldrb`/`ldrsb`/`ldr w`/`ldr x`; fixed integer array →
  const-index direct frame load, or runtime-index bounds-check + scaled `add` + load).
- **Float `a64_emit_float_value_to_vreg_depth`** new arms (:1535–1556): `IR_VALUE_INDEX_LOAD` →
  `a64_emit_float_index_load` (:1376; BYTE_VIEW float span → span_index_addr + `ldr s_/d_,[x9]`; fixed
  [N]f32/[N]f64 → const-index `ldr v,[sp,#off]` or runtime bounds-check + `ldr v,[base,idx,lsl]`),
  `IR_VALUE_BYTE_VIEW_READ_FLOAT_LE` (readF32Le/readF64Le: same shape as the int codec read, size 4/8,
  `ldr s_/d_`).
- **`IR_INSTR_INDEX_STORE`** (:1641, in `a64_emit_instr`): BYTE_VIEW span → value materialized FIRST
  (float into v8 / int into x10) so the address scratch (x8/x9) survives, then span_index_addr, then
  store by width (`str s_/d_`/`strb`/`str w`/`str x`); fixed float array → `a64_emit_float_index_store`
  (:1408); fixed integer array → const-index direct frame store, or runtime-index bounds-check + scaled
  store. **Includes the u8 span store (`strb`)** — `mem-mut-span-array-store` exercises it.

### Local admission (`a64_validate_function` + LOCAL_SET)

- **`a64_validate_function`** now admits fixed primitive **array** locals (u8/i8/i32/u32/i64/u64/usize/
  f32/f64) and non-param **byte-view (span)** locals; a span/record **parameter** is still rejected (its
  prologue spill is P5), and record/allocator/Vec/Maybe locals still bail.
- **`a64_emit_instr` LOCAL_SET** gained a BYTE_VIEW branch: a `let s: Span<T> = arr` / reinterpret /
  slice stores the pointer (`str x` @slot+0) and the 32-bit element count (`str w` @slot+8). Array locals
  are **not** LOCAL_SET — a `[N]T = [...]` literal lowers to per-element `IR_INSTR_INDEX_STORE` in the
  MIR (ir.c:3233–3277), so the array store path serves the initializer with no LOCAL_SET branch.

### Span len convention (32-bit element COUNT) — confirmed, a P7 watch-item

Span len is a **32-bit element count** at slot+8 everywhere P4 touches (BYTE_VIEW LOCAL load `ldr w`,
LOCAL_SET store `str w`, span_index_addr len load `ldr w`, span FIELD_LOAD len). The `is_mapping` flag
on IrLocal exists but is **not read** here — P7 widens these to 64-bit (`ldr x`/`str x`) only for mapped
regions whose byte length can exceed 4 GiB; all P4 fixtures + llama2 weights-as-f32 counts fit in 32 bits.

### Byte-identity spot-check (cite the bytes)

`mem-mut-span-array-store` built `--emit exe --target linux-musl-arm64` (direct-exe) vs the committed
macho object (`--emit obj --target darwin-arm64`). The span-index-addr keystone is **byte-identical**:
ELF text words `0x6b09011f` (cmp w8,w9), `0x54000043` (b.cc cond3, +8 over the brk), `0xd4200000`
(brk #0), `0x9103b3e9` (add x9,sp,#off — the array-base ptr path used here), `0x8b080129` (add x9,x9,x8),
`0x3900012a` (strb w10,[x9]) — the same words appear at the committed Mach-O `__text` offset 0x20–0x34
(`otool -t`: `… 6b09011f 54000043 d4200000 9103b3e9 8b080129 3900012a`). Only the container differs
(ET_EXEC vs Mach-O object), exactly the keystone insight.

### Validation evidence

```
$ make -C native/zero-c                                     # clean, no -Wall -Wextra -Wpedantic warnings
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs    # conformance ok
  darwin-arm64 native: 87 ran, 46 skipped                    # macho path unregressed
  linux-arm64 (docker): 49 ran, 84 skipped                   # +12 from P3's 37; zero miscompiles
$ docker run … /w/mem-bytes-as-f32        → mem bytes as f32 ok    # reinterpret (lsr len) + float span index
$ docker run … /w/mem-bytes-as-i8         → mem bytes as i8 ok     # ldrsb (i8 span)
$ docker run … /w/codec-read-i32-le-offset→ codec read i32 le offset ok   # readI32Le bounds + ldr w
$ docker run … /w/mem-mut-span-array-store→ mem mut span array store ok    # u8 strb + i32 str w + f32 str s_
$ docker run … /w/math-rmsnorm-smoke      → math rmsnorm ok        # P3 libm + P4 [N]f32 arrays via obj+link
$ docker run … /w/math-softmax-smoke      → math softmax ok        # idem (expf)
$ docker run … /w/codec-read-i32-le-bounds → exit 133              # runtime offset+4>len bounds trap (brk #0)
$ docker run … /w/bounds-span-index        → exit 133              # span[3] on len-3 span traps
#   inline runtime variable-index OOB (s[i], i past end) → exit 133 too
# non-regression: P1 exit-code-arithmetic → 42, std-codec-widths ok; P2 float-arith-add-f32 ok;
#   P3 math-powf-known-values ok; linux-musl-x64 (zero-elf64) builds; darwin-arm64
#   mem-mut-span-array-store runs native (Mach-O) → mem mut span array store ok
# independent sweep: 55 build for linux-musl-arm64, all 55 run, 0 miscompiles
```

### emit_macho64 / emit_elf64 reference-line corrections

All macho references used were accurate against the tree at write time: small leaves
emit_macho64.c:651–693, FP base/reg-lsl emit_macho64.c:737–756, `macho_elem_{byte_size,log2}`
@1252/@1267, `macho_byte_view_const_len` @1274 (reinterpret @1291), `macho_readonly_data_byte` @1238,
`macho_emit_byte_view_len_depth` @1367 (reinterpret @1413) / `macho_emit_byte_view_ptr_depth` @1429
(ARRAY_BYTE_VIEW multi-type @1444, slice-start scaling @1458, reinterpret @1477),
`macho_emit_span_index_addr_depth` @1493, integer `IR_VALUE_INDEX_LOAD` @1949 /
`IR_VALUE_BYTE_VIEW_INDEX_LOAD` @1912 / `IR_VALUE_BYTE_VIEW_READ_INT_LE` @1931 / `IR_VALUE_BYTE_VIEW_LEN`
@1794, `macho_emit_float_index_load` @2024 / `_store` @2056, `IR_VALUE_BYTE_VIEW_READ_FLOAT_LE` @2345,
`IR_INSTR_INDEX_STORE` @2902, `macho_validate_function` local admission @3059–3072. The IR shape
(ir.c:3233–3277 array-literal → per-element INDEX_STORE; the typed-span lower from the macho P3 doc)
held. No drift found.

## P5 must-know

1. **Span PARAMETERS are the next wall, and the per-element codegen is already done.** P4 left
   `a64_validate_function` (emit_elf_aarch64.c, the param check ~:1791) rejecting a byte-view/record
   parameter, and `a64_emit_call_to_reg_depth` (~:1075) + `a64_emit_function_text` (~:1860) bailing
   span/record args/params. `typed-span-{f32,i32}`, `mem-mut-span-slice`, and the `*-span` kernels
   (math-{matmul,rmsnorm,softmax,swiglu,rope}-span) bail ONLY on the span param — once it lands they flip
   immediately (the typed index load/store + len they need is in place from P4). A span param is just a
   BYTE_VIEW local whose ptr/len come from two arg x-regs instead of a LOCAL_SET, so
   `a64_emit_span_index_addr` works unchanged once the prologue populates the slot (ptr `str x`@0, the
   32-bit count `str w`@8) — exactly as the macho P3→P4 handoff predicted.
2. **Port the macho P4 aggregate ABI** (the committed macho doc's "## P4 — what landed"): **sret via x8**
   (the AAPCS indirect-result reg — does NOT consume an arg reg, unlike ELF's rdi-sret; spill x8 to a
   frame slot in the prologue and reload before every field store + the return, since calls clobber x8);
   record params (copy-in by ptr); span params (`(ptr,len)` in two x-regs → spill to slot); span returns
   (ptr/len in x0/x1, the count in **w1**); span-in-record fields (ptr@fo, count@fo+8); record-to-record
   copy (8-byte chunks + 4-byte tail); record/shape-literal call args (incl. nested). The macho helpers to
   mirror: `macho_emit_lea_local_addr`, `_record_copy_to`, `_copy_record_param`, `_marshal_call_arg` (the
   single arg marshaller: record→ptr / span→two-reg / scalar-float→value), `_record_call_with_dest`,
   `_sret_field_store`, and the store-through-base `_str_{b,w,x}_disp`/`_str_v_disp`/`_ldr_w_disp`.
3. **The int-binary left-spill discipline is already in place** (P1) and `depth` already threads through
   `a64_emit_value_to_reg_depth` / the byte-view `_depth` helpers / `a64_emit_span_index_addr_depth`
   (P4). The macho P4 doc's "latent integer-binary x8-clobber" fix is therefore **already present** here —
   keep routing any new spilling op through the depth slots; the frame is `a64_frame_size + fp_scratch +
   int_scratch` and P5 adds a **sret slot** (16 bytes) to it (macho: `+ sret_bytes` when the fn returns a
   record). FP scratch stays at `[0, fp)`, int scratch above it, sret slot above that, locals on top.
4. **A record-returning RAISING function is rejected** (the error tag in x1 collides with the sret/len
   register convention) — mirror macho/elf. The 8-int / 8-param caps stay (x8 doesn't count toward them).
   `aggregate-shape-abi` is the headline flip; the four f32 `*-span` kernels + the typed-span fixtures
   flip as a bonus once span params land. **No `target.c`/`ir.c`/`run.mjs` change** (the IR already lowers
   these shapes via the aggregate-abi pre-pass; fixtures auto-flip as the backend stops bailing).
5. **div/mod is P6, not P5.** The `*-span` kernels build past span params in P5 but several then bail on
   `/`/`%` ("does not yet support this binary operator") — `a64_emit_binary_int` emits only ADD/SUB/MUL
   (DIV/MOD bail). P6 adds SDIV/UDIV + MSUB (32- and 64-bit, signed/unsigned off the result type) and
   already spills the left operand correctly (the depth machinery wraps it). FDIV is already present.

## P5 — what landed

The full **aggregate / function-boundary value semantics** (sret via x8, span/record params + returns,
record-to-record copy, span-in-record fields, record/shape-literal call args incl. nested, float
record-field load) in `emit_elf_aarch64.c` (now **2656 lines, +327**). The macho P4 layer was ported
byte-for-byte under the `a64_` prefix; the aggregate seams P4 left bailing CGEN004 are replaced with
correct codegen. **No `target.c`, `ir.c`, `main.c`, `emit_macho64.c`, `emit_elf64.c`, or `run.mjs`
change** — the IR already lowers these shapes via the aggregate-abi pre-pass, so the wired fixtures
auto-flipped skip→run as the backend stopped bailing (the same auto-flip mechanism every prior phase
used). The aggregate ABI is **pure instruction selection** on whichever path the program already takes:
the span/record fixtures are syscall-free → direct-exe; the span kernels that also call libm
(`math-{matmul,rmsnorm,softmax,swiglu}-span` — actually matmul is pure FP and stays direct-exe; rmsnorm/
softmax/swiglu route to obj+link for `sqrtf`/`expf`) reach libm through P3's already-working dispatch +
CALL26 relocs, no new relocations.

**Result: `conformance ok`; darwin-arm64 native unchanged at 87 ran / 46 skipped (committed macho path
unregressed); `linux-arm64 (docker): 49 ran → 59 ran, 84 → 74 skipped`; zero miscompiles**
(`/tmp/compare.mjs`: `compared=68 armOnlyBuilds=0 MISCOMPILES=0` — every fixture that builds on BOTH
linux-musl-arm64 and darwin-arm64 matches darwin token-for-token; up from P4's 55, no arm-only build).

### The flip set — 10 fixtures (49 → 59 ran), all correct in docker

- **`aggregate-shape-abi`** (the headline) — exercises every aggregate shape at once: record literal →
  param/return + field reads, returning a record local/param (copy through sret), returning a
  record-returning call (callee writes straight through our sret), record-to-record copy, record-call /
  shape-literal arguments materialized into temps (incl. two at once + nested `area(makeDims(4))`), field
  access on a record-call result (`makeDims(9).width`), span returns, and `Span` fields inside records
  (`Views{head,tail}` built from span-returning calls). Prints `aggregate shape abi ok`.
- **`typed-span-f32`**, **`typed-span-i32`** — span params (the per-element load/store landed in P4; only
  the param ABI was missing).
- **`math-{matmul,rmsnorm,softmax,swiglu}-span`** — the f32 span kernels (span params + the typed index
  load/store from P4). matmul stays direct-exe (pure FP mul/add); rmsnorm/softmax/swiglu route to
  obj+link via the libm dispatch (sqrtf/expf). All print `… ok`. **No integer div/mod** — they use FDIV
  (already present), so they flip in P5, not P6.
- **`generate-loop`** — the integration loop over spans/records (`generate loop ok`).
- **`float-primitives`** — a `FloatPair` record with f32/f64 fields + FIELD_LOAD (the float record-field
  read added to the FP value dispatch).

**Honestly NOT flipped (parity-correct, deferred):**
- `math-rope-span`, `sampler-sample`, `sampler-topk-topp`, `tokenizer-encode`, `checkpoint-q-v2` — build
  **past** span/record params then bail CGEN004 "unsupported operator" on `/`/`%` (`a64_emit_binary_int`
  emits only ADD/SUB/MUL). **P6.** (`tokenizer-encode` additionally needs `eqlBytes`, also P6.)
- `mem-mut-span-slice` and the `mem-bytes-as-{mut-*,…}` family — bail **TAR002 "Heap capability"**
  (`std.mem.pageAlloc`). **P7.** Not an aggregate-ABI gap.

### New `a64_*` helpers (line ranges in emit_elf_aarch64.c)

- **Store-through-base leaves** (:294–311, ported byte-identical from emit_macho64.c:762–782):
  `a64_emit_str_b_disp` / `a64_emit_str_w_disp` / `a64_emit_ldr_w_disp` / `a64_emit_str_v_disp` — write a
  record field through a pointer in a base register (the sret pointer in x8). `a64_emit_str_x_disp` /
  `a64_emit_ldr_x_disp` already existed (P1 spill slots, offset-0 = base store). Plus
  `a64_emit_store_local_b` (:274, the STRB sibling of P4's `a64_emit_load_local_b`, for a u8/Bool field).
- **sret frame slot** (:1125–1135, ported from emit_macho64.c:2217–2227): `a64_returns_record`,
  `a64_sret_reserved_bytes` (16 if record return), `a64_sret_slot_offset` (`fp_scratch_bytes +
  int_scratch_bytes` — just above the two scratch regions).
- **Field store/load helpers** (:1629–1645): `a64_emit_store_field` (scalar field by width — u8/Bool STRB,
  i64/u64/usize STR x, else STR w; the byte-store sibling of the FIELD_LOAD path) and `a64_emit_str_v_field`
  (float field). The float FIELD_**LOAD** is inlined in the FP dispatch (:1601–1607) via `a64_emit_ldr_v_local`.
- **Aggregate ABI** (:1648–1786, ported from emit_macho64.c:2385–2521): `a64_emit_lea_local_addr`
  (`add reg, sp, #localoff`), `a64_emit_record_copy_to` (slot→slot / slot→sret memcpy, 8-byte chunks +
  4-byte tail), `a64_emit_copy_record_param` (ptr-reg→slot copy, the prologue record-param value copy),
  `a64_emit_marshal_call_arg` (the **single** arg marshaller: record→pointer / span→two-reg /
  scalar-float→value emitter — shared by the regular call path and the record-call path; forward-declared
  at :1152 so `a64_emit_call_to_reg_depth` at :1158 can use it), `a64_emit_record_call_with_dest`
  (marshal args, set x8 last, `bl`), `a64_emit_sret_field_store`.

### sret (record return) via x8 — the AAPCS indirect-result register

Unlike ELF's rdi-sret (which consumes an arg reg → the 5-int cap), arm64 passes the destination pointer
in **x8**, which is *not* an argument register, so the 8-int cap is unchanged (kept here; x8 doesn't
count). Mechanism (all in `a64_emit_function_text` :2135 + the RETURN/FIELD_STORE blocks):
- **Frame slot** — the prologue spills `str x8, [sp, #sret_slot]` (:after the `sub sp` + process-args
  seed). Reloaded before *every* field store and at the return, so an intervening call (which clobbers x8)
  can never strand it.
- **Three RETURN shapes** (`IR_INSTR_RETURN`, `return_type == IR_TYPE_RECORD`, :1986): `return f()` →
  `a64_emit_record_call_with_dest(dest=-1)` passes our saved sret pointer as the callee's x8
  (straight-through); `return p` → `a64_emit_record_copy_to(dest=UINT_MAX, src)` memcpys the local through
  the sret pointer; `return <literal>` → fields already written via sret field stores, then
  `ldr x0, [sp, #sret_slot]`. All hand the sret pointer back in x0.
- **sret field store** (`IR_INSTR_FIELD_STORE`, `local_index == UINT_MAX` → `a64_emit_sret_field_store`,
  :1888/:1754): materialize the value, reload x8, `str (b/w/x/v) value, [x8, #fo]`. A span field stores
  ptr@fo and the 32-bit count len@fo+8; a span-returning-call field captures x0/x1 before the reload.

### Span / record params (the prologue, :2135)

The frame is `a64_frame_size + fp_scratch + int_scratch + sret_bytes` (the sret 16 bytes added when the fn
returns a record). After spilling x8 (if record-returning), each param is spilled by kind: a **span
(byte-view) param** = two int regs → `str x` @slot+0 (ptr), `str w` @slot+8 (the 32-bit count); a
**record param** = one int reg holding a pointer → `a64_emit_copy_record_param` copies it into the slot
for value semantics; floats from v0..v7, scalars from x0..x7 (separate counters, AAPCS). A span param is
then just a BYTE_VIEW local and P4's `a64_emit_span_index_addr` / byte-view ptr/len work unchanged. The
`a64_validate_function` (:2092) byte-view/record-param rejection is lifted; record returns are admitted
(a record-returning **RAISING** fn is rejected — the error tag in x1 collides with the sret/len register
convention — mirroring macho/elf).

### Call args + span returns + LOCAL_SET record bind

- `a64_emit_call_to_reg_depth` (:1158) now marshals every arg through `a64_emit_marshal_call_arg` and
  allows a **span result** (ptr x0 / len x1, consumed directly by span-context callers); a **record
  result** still bails ("must bind to a record local or be returned" — record calls route through
  `a64_emit_record_call_with_dest`).
- **LOCAL_SET** (:1842): a record local binds a record-returning call straight into its slot via sret
  (`let q = f()`), or copies a record local (`let q = p`); a BYTE_VIEW local additionally accepts a
  span-returning call (ptr x0 / len x1 → slot). **FIELD_STORE** into a record local (:1888) handles span
  fields (ptr@fo, count@fo+8; span-call captures x0/x1), float fields, and scalar fields — needed because
  a record/shape-literal **argument** is materialized into a temp record local then field-stored before
  being passed by pointer.

### The integer-binary left-spill + depth threading — already in place (kept)

P1/P4 already thread `depth` through `a64_emit_value_to_reg_depth` / the byte-view `_depth` helpers /
`a64_emit_span_index_addr_depth`, with one 16-byte frame slot per integer binary/compare nesting level
(the macho "latent x8-clobber" fix is therefore already present here). P5 routes the new call path's args
at the call's nesting depth (`a64_emit_marshal_call_arg(..., depth, ...)`) so a nested record-call/binary
argument takes a deeper slot, and the sret slot sits **above** both scratch regions so it never overlaps.
No change to the spill discipline — div/mod (P6) will route through the same wrap unchanged.

### Byte-identity spot-check (cite the bytes)

`aggregate-shape-abi`'s `makeDims` built `--emit exe --target linux-musl-arm64` (direct-exe, ET_EXEC) vs
the committed macho object (`--emit obj --target darwin-arm64`). **All 14 leading words byte-identical**
(the sret-spill + reload-before-every-field-store discipline): `a9bf7bfd` (stp x29,x30), `910003fd` (mov
x29,sp), `d10083ff` (sub sp,#0x20), **`f90003e8` (str x8,[sp] — the sret spill)**, `b9001be0` (str
w0,[sp,#0x18] — param spill), `b9401be9` (ldr w9,[sp,#0x18]), **`f94003e8` (ldr x8,[sp] — reload, ×3)**,
`b9000109` (str w9,[x8]), `52800069` (mov w9,#3), `b9000509` (str w9,[x8,#4]), `52800029` (mov w9,#1),
`39002109` (strb w9,[x8,#8]). Only the file offset differs (ELF 0x580 vs Mach-O 0x608) — exactly the
keystone insight.

### emit_macho64 reference-line corrections

All macho references used were accurate against the tree at write time: store-through-base leaves
emit_macho64.c:762–782, `macho_emit_store_local_b` @581 / `macho_emit_store_field` @594 /
`macho_emit_load_field` @586, `macho_emit_str_v_field` @731 / `macho_emit_ldr_v_field` (the float
FIELD_LOAD case @2259–2263), `macho_returns_record` @2217 / `macho_sret_reserved_bytes` @2221 /
`macho_sret_slot_offset` @2225, `macho_emit_lea_local_addr` @2385 / `_record_copy_to` @2393 /
`_copy_record_param` @2415 / `_marshal_call_arg` @2436 / `_record_call_with_dest` @2468 /
`_sret_field_store` @2486, the LOCAL_SET record bind @2677–2689 + byte-view span-call @2709–2713, the
FIELD_STORE block @2868–2901, the RETURN record block @2964–2980 + span return @2982–2993, the prologue
sret spill + span/record param spill @3100–3124, `macho_emit_call_to_reg_depth` record-result bail +
marshaller @1511–1536, `macho_validate_function` @3047–3074. No drift found.

### Validation evidence

```
$ make -C native/zero-c                                     # clean, no -Wall -Wextra -Wpedantic warnings
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs    # conformance ok
  darwin-arm64 native: 87 ran, 46 skipped                    # macho path unregressed
  linux-arm64 (docker): 59 ran, 74 skipped                   # +10 from P4's 49; zero miscompiles
$ node /tmp/compare.mjs   → compared=68 armOnlyBuilds=0 MISCOMPILES=0   # gold-standard miscompile gate
$ docker run … /w/aggregate-shape-abi → aggregate shape abi ok   # sret + record/span params + span-in-record
$ docker run … /w/typed-span-f32      → typed span f32 ok          # span param
$ docker run … /w/typed-span-i32      → typed span i32 ok          # span param
$ docker run … /w/math-matmul-span    → math matmul span ok        # span params, pure FP, direct-exe (1109 B)
$ docker run … /w/math-rmsnorm-span   → math rmsnorm span ok       # span params + libm via obj+link (FDIV, no int div)
$ docker run … /w/generate-loop       → generate loop ok
$ docker run … /w/float-primitives    → float primitives ok        # record float-field FIELD_LOAD
$ bin/zero build … fail/aggregate-record-arg-while → CGEN004 "record argument here must be a record local" (exit 1)
# div/mod-deferred: math-rope-span / sampler-{sample,topk-topp} / tokenizer-encode / checkpoint-q-v2
#   → build past span/record params then CGEN004 "unsupported operator" (P6); mem-mut-span-slice → TAR002 Heap (P7)
# non-regression: exit-code-arithmetic → 42, float-arith-add-f32 / math-powf-known-values /
#   math-rmsnorm-smoke / codec-read-f32-le correct; linux-musl-x64 (zero-elf64) aggregate-shape-abi builds
#   + runs "aggregate shape abi ok"; darwin-arm64 aggregate-shape-abi runs native "aggregate shape abi ok"
```

## P6 must-know

1. **Integer div/mod is the next wall, and several kernels build past P5's params then bail on it.**
   `a64_emit_binary_int` (emit_elf_aarch64.c:~188) emits only ADD/SUB/MUL — DIV/MOD return CGEN004
   "unsupported operator". Add **SDIV/UDIV** + **MSUB** (for modulo `rem = a − (a/b)*b`), at **32- AND
   64-bit** width, **signed vs unsigned off the result type** (`a64_type_is_unsigned`), width off
   `a64_type_is_scalar64` — exactly the macho P5a selection (`macho_emit_div` / `macho_emit_msub`,
   emit_macho64.c:602–650 region). The left-operand spill (`str x8,[sp,#slot]` … `ldr x8,[sp,#slot]`)
   from P1/P4 already wraps DIV/MOD unchanged, so `a / f(b)` is correct. AArch64 div-by-zero yields 0
   (no trap; `INT_MIN/-1` → `INT_MIN`) — same UB contract as ELF's idiv (#DE), no spec violation. **FDIV
   is already present** (the FP binary path). The macho P5a doc warns of **four latent bugs div exposes**
   — verify they are NOT present here (most are, from prior phases): (1) 64-bit integer literal — the
   aarch64 `IR_VALUE_INT` already uses `a64_emit_mov_imm64` for scalar64 (no truncation); check the
   `IR_VALUE_MAYBE_SCALAR_LITERAL` payload path if/when Maybe lands (P7). (2) byte-slice start scaling —
   already scaled by `a64_elem_log2` in the `IR_VALUE_BYTE_SLICE` ptr branch (P4). (3) call-arg
   scratch-slot collision — already fixed: `depth` threads through `a64_emit_marshal_call_arg` →
   `a64_emit_call_to_reg_depth` (P4+P5). (4) compare width/signedness — **check this one**: the integer
   compare currently always emits the signed codes and `cmp w`/`cmp x` off `value->left->type` — confirm
   it picks **unsigned** codes (LO/LS/HI/HS via `a64_cond_for_compare(op, is_unsigned)`, which already
   exists) for unsigned types and `cmp x` for 64-bit operands (it does — `is_unsigned`/`wide` are read
   from `value->left->type` at the COMPARE case). The macho P5a u64-equality (`s2 == big`) and usize
   ordering depend on it.
2. **`eqlBytes` (`std.mem.eqlBytes`) is the sole remaining wall on `tokenizer-encode` after div.** Port
   `IR_VALUE_BYTE_VIEW_EQ` from macho P5a (emit_macho64.c, the `BYTE_VIEW_EQ` arm): compare lengths → 0
   if unequal, else a byte loop → 0 on first mismatch, 1 if all match. macho can't push sp-relative
   locals, so it settles both pointers + the length into stable registers via the integer scratch slot
   bridge before the loop (sub-views at depth+1, loop body touches only high regs); add it to the
   `a64_value_int_depth` `spills` set so the frame reserves its slot. Same discipline applies here.
3. **`std.mem.copy` / `std.mem.fill`** (`IR_VALUE_BYTE_COPY` / `IR_VALUE_BYTE_FILL`) — inline a byte loop
   (copy: src/dst/count=min(lens); fill: dst/value/len), the same settle-into-stable-regs discipline as
   `eqlBytes`, both added to the `a64_value_int_depth` spills set (`std-mem-copy-fill` is the fixture).
   ELF inlines copy but not fill; macho P2a inlined both. (`std.mem.copy` is also a llama2 dependency.)
4. **64-bit literal materialization** for any new ≤32-bit-only movz site — the aarch64 backend already
   uses `a64_emit_mov_imm64` for scalar64 `IR_VALUE_INT`/`IR_VALUE_FLOAT(f64)`; keep `a64_emit_movz_x`
   (32-bit) only for genuinely ≤32-bit uses (loop counters, the SYS_write number).
5. **Fixtures P6 flips:** `math-rope-span` (`i % head_size` usize + FP `/`), `sampler-sample` (xor64
   `x % 2` u64 + the PRNG), `sampler-topk-topp` (heapsort + `/`), `tokenizer-encode` (div + `eqlBytes`),
   `checkpoint-q-v2` (div-heavy int8 loader), `std-mem-copy-fill` (copy+fill). They are already wired
   into the run.mjs runtime loops, so they auto-flip as the backend stops bailing — expect the
   `linux-arm64 (docker)` count to rise from 59 into the mid-60s. `generate-argmax(-q)` /
   `transformer-forward(q)` need **P7** (pageAlloc) on top of P6's ops.

## P6 — what landed

Integer `/` and `%` (the last operator gap) + the three byte-loop value kinds (`eqlBytes`, `copy`,
`fill`) in `emit_elf_aarch64.c` (now **2805 lines, +149**). The macho P5a/P2a layers were ported
byte-for-byte under the `a64_` prefix; the DIV/MOD/`BYTE_VIEW_EQ`/`BYTE_COPY`/`BYTE_FILL` seams P5
left bailing CGEN004 are replaced with correct codegen. **No `target.c`, `ir.c`, `main.c`,
`emit_macho64.c`, `emit_elf64.c`, or `run.mjs` change** — every flipped fixture was already wired
into the run.mjs runtime loops by P1, so they auto-flipped skip→run as the backend stopped bailing
(the same auto-flip mechanism every prior phase used). P6 is **pure instruction selection**: the
div/mod kernels are syscall-free → direct-exe, except `checkpoint-q-v2`/`sampler-topk-topp` which
route to obj+link only because they also call libm (already-working P3 dispatch + CALL26 relocs, no
new relocations).

**Result: `conformance ok`; darwin-arm64 native unchanged at 87 ran / 46 skipped (committed macho
path unregressed); `linux-arm64 (docker): 59 ran → 65 ran, 74 → 68 skipped`; zero miscompiles**
(`/tmp/compare.mjs`: `compared=78 armOnlyBuilds=0 MISCOMPILES=0` — every fixture that builds on BOTH
linux-musl-arm64 and darwin-arm64 matches darwin token-for-token; up from P5's 68, no arm-only build).

### The flip set — 6 fixtures (59 → 65 ran), all correct in docker

`math-rope-span` (`i % head_size` usize-32-bit + FP `/`), `sampler-sample` (the xorshift* PRNG: u64
`x % 2` + u64 `s / 4096` etc.), `sampler-topk-topp` (heapsort with integer `/`), `tokenizer-encode`
(div + `eqlBytes` decode round-trip), `checkpoint-q-v2` (the div-heavy int8 quant loader),
`std-mem-copy-fill` (`std.mem.copy` + `std.mem.fill`). All print their `… ok` under
`docker run --platform linux/arm64`.

**Honestly NOT flipped (parity-correct, deferred to P7):** `generate-argmax`, `generate-argmax-q`,
`transformer-forward`, `transformer-forwardq`, `mem-mut-span-slice` (+ the `mem-bytes-as-{mut-*,…}`
family) — they build **past** every div/mod/slice/call path and bail **TAR002 "target does not
provide required Heap capability"** (`std.mem.pageAlloc`). **P7** lifts the heap capability + lowers
mmap. Not a div/ops gap.

### Integer div/mod selection + encodings (`a64_emit_binary_int`, emit_elf_aarch64.c:209)

- **`a64_emit_div`** (:190) — SDIV/UDIV (data-processing 2-source): `op = is_unsigned ? 0x1ac00800 :
  0x1ac00c00`, `op |= (1u<<31)` for the 64-bit (Xd) form; bit 10 selects signed(1)/unsigned(0).
- **`a64_emit_msub`** (:198) — MSUB `Xd, Xn, Xm, Xa = Xa − Xn*Xm` (`0x1b008000`, bit31 = width).
- **`a64_emit_binary_int`** (:209) gained `bool is_unsigned` and the DIV/MOD arms: `IR_BIN_DIV` →
  `a64_emit_div`; `IR_BIN_MOD` → `a64_emit_div(quotient=x10)` then `a64_emit_msub(dst, x10, divisor,
  dividend)` (`rem = a − (a/b)*b`; the quotient temp is x10 — free, operands are x8/x9). Width off
  `a64_type_is_scalar64(value->type)`, signedness off `a64_type_is_unsigned(value->type)` (the
  **result** type) — exactly the macho P5a / ELF selection. Bitwise ops still bail honestly (no
  consumer in the self-host subset). The two non-binary call sites pass the new `is_unsigned` arg
  (the BINARY case at :1267 reads it from `value->type`; the byte-slice-len SUB at :983 passes
  `false`, 32-bit signed count arithmetic). **FDIV was already present** (the FP binary path,
  :1545–1560 accepts `IR_BIN_DIV` → `a64_emit_float_arith` `fdiv`).
- AArch64 **div-by-zero yields 0** (and `INT_MIN/-1` → `INT_MIN`) with no trap — the architectural
  result; the language treats these as UB, consistent with ELF x86's idiv/div (#DE, also UB). No trap
  added (same UB contract as the macho/elf backends).
- The **P1/P4 left-operand spill** (`str x8,[sp,#slot]` … `ldr x8,[sp,#slot]`, indexed by the integer
  depth) wraps DIV/MOD unchanged, so `a / f(b)` (left survives a call) is correct — confirmed by the
  kernels (e.g. `total + n_layers*qBlockBytes(...)` in checkpoint-q-v2 and the nested PRNG calls).

### The four latent bugs (macho P5a) — ALL already fixed by prior phases (verified, none re-fixed)

1. **64-bit integer literal truncation** — already fixed: `IR_VALUE_INT`/`IR_VALUE_BOOL`
   (emit_elf_aarch64.c:1226) uses `a64_emit_mov_imm64` for `a64_type_is_scalar64`, the 32-bit
   `a64_emit_movz_w` only for ≤32-bit. The PRNG's u64 multiplier `2685821657736338717` and divisors
   `4294967296`/`4096` load correctly (sampler-sample's bit-exact `rngNext`/`randomF32` checks pass).
2. **Byte-slice start not scaled by element size** — already fixed (P4): the `IR_VALUE_BYTE_SLICE`
   ptr branch (:919–936) scales the start by `a64_elem_log2(view->element_type)` (const `<<log2` +
   `add #imm`; runtime `add x,x,x,lsl #log2`); the slice LEN stays an unscaled count.
3. **Call-arg scratch-slot collision** — already fixed (P4+P5): `depth` threads through
   `a64_emit_marshal_call_arg` → `a64_emit_call_to_reg_depth`, so a call nested as a binary's operand
   marshals its args at the call's nesting depth and a nested-binary argument takes a deeper slot.
4. **Compare width/signedness** — verified correct (the one the doc flagged to check): the integer
   COMPARE case (:1270–1283) reads `wide = a64_type_is_scalar64(value->left->type)` and `is_unsigned
   = a64_type_is_unsigned(value->left->type)`, emits `cmp x`/`cmp w` accordingly, and selects the
   condition via `a64_cond_for_compare(op, is_unsigned)` (LO/LS/HI/HS for unsigned). The PRNG's u64
   equality (`s2 == 1126174793148417`, > 2³²) and every usize ordering are correct.

### eqlBytes / copy / fill (the three byte-loop value kinds)

Ported byte-for-byte from emit_macho64.c (P5a's `BYTE_VIEW_EQ` @1796, P2a's `BYTE_COPY` @1843 /
`BYTE_FILL` @1883) into the integer dispatch's new arms; locals are sp-relative so values that must
outlive a sibling's evaluation are settled into stable high registers via the integer scratch slot
(bridge) rather than pushed, and sub-views are evaluated at `depth+1` so a runtime-slice index never
reuses the slot. All three were added to the `a64_value_int_depth` `spills` set (:1095–1100) so the
frame reserves a slot.
- **`IR_VALUE_BYTE_VIEW_EQ`** (`std.mem.eqlBytes`, emit_elf_aarch64.c:1352): settle both pointers
  (x10/x11), compare lengths (`cmp w`; NE → 0), then a byte loop (i in x13; load x14/x15 via x10+i /
  x11+i; any mismatch → 0; i≥len → 1).
- **`IR_VALUE_BYTE_COPY`** (`std.mem.copy`, :1399): src x10 / dst x11, `count = min(src.len,
  dst.len)` in x12, byte loop copies src[i]→dst[i]; returns the count in `reg`.
- **`IR_VALUE_BYTE_FILL`** (`std.mem.fill`, :1438): dst x11, fill byte x14, len x12, byte loop stores
  the fill byte to dst[i]; returns the length in `reg`. (ELF x86 inlines copy but not fill; macho P2a
  inlined both; this matches macho.) `std.mem.copy` is a llama2 dependency (P7's allocator path).

### Byte-identity spot-check (cite the bytes)

`sampler-sample` built `--emit obj` for BOTH `linux-musl-arm64` and `darwin-arm64`; the u64 div/mod
words are **byte-identical at the same relative offsets** (`objdump -d` vs `otool -tv`/`-t`):
`9ac90908` (udiv x8,x8,x9 — the `/` ops, ×6), `9ac9090a` (udiv x10,x8,x9) + `9b09a148` (msub
x8,x10,x9,x8) — the modulo pair, ×3 each. The 32-bit form appears in `math-rope-span`'s linked
object: `1ac9090a` (udiv w10,w8,w9) + `1b09a148` (msub w8,w10,w9,w8) for `i % head_size`, alongside
`1e291908`/`1e201800` (fdiv s_). Only the container differs (ELF vs Mach-O), exactly the keystone
insight. (The direct-exe ELF carries the same words but has no section headers, so disassemble the
`--emit obj` form to inspect.)

### emit_macho64 reference-line corrections

All macho references used were accurate against the tree at write time: `macho_emit_div` @606 /
`macho_emit_msub` @614 / `macho_emit_binary_int` @625 (the DIV/MOD arms @635–640), the integer
COMPARE width/signedness read from `value->left->type` (the macho analog), `IR_VALUE_BYTE_VIEW_EQ`
@1796 / `IR_VALUE_BYTE_COPY` @1843 / `IR_VALUE_BYTE_FILL` @1883, and the `macho_value_int_depth`
spills set @2156–2162 (EQ/COPY/FILL contribute one slot). No drift found.

### Validation evidence

```
$ make -C native/zero-c                                     # clean, no -Wall -Wextra -Wpedantic warnings
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs    # conformance ok
  darwin-arm64 native: 87 ran, 46 skipped                    # macho path unregressed
  linux-arm64 (docker): 65 ran, 68 skipped                   # +6 from P5's 59; zero miscompiles
$ node /tmp/compare.mjs   → compared=78 armOnlyBuilds=0 MISCOMPILES=0   # gold-standard miscompile gate
$ docker run … /w/math-rope-span    → math rope span ok      # usize % (udiv/msub w) + fdiv
$ docker run … /w/sampler-sample    → sampler sample ok      # u64 % / (udiv/msub x) + bit-exact PRNG
$ docker run … /w/sampler-topk-topp → sampler topk topp ok   # heapsort /
$ docker run … /w/tokenizer-encode  → tokenizer encode ok    # div + eqlBytes
$ docker run … /w/checkpoint-q-v2   → checkpoint q v2 ok      # div-heavy int8 loader (obj+link)
$ docker run … /w/std-mem-copy-fill → mem copy fill ok       # copy + fill byte loops
# signed/unsigned correctness probe (inline /tmp/p6/divmod.0): -17/5=-3, -17%5=-2, u32 4294967294/7=
#   613566756, u64 1126174793148417%1000000=148417 → exit 42 (all assertions pass) in docker arm64
# P7-blocked stay honest: generate-argmax(-q) / transformer-forward(q) / mem-mut-span-slice →
#   TAR002 "target does not provide required Heap capability" (NOT a miscompile)
# non-regression: exit-code-arithmetic → 42; float-arith-add-f32 / math-powf-known-values /
#   math-{matmul,rmsnorm,softmax,swiglu}-span / aggregate-shape-abi all "… ok" in docker;
#   linux-musl-x64 (zero-elf64) sampler-sample runs "sampler sample ok"; darwin-arm64 sampler-sample
#   + checkpoint-q-v2 run native (Mach-O) "… ok" — committed macho/elf64 paths green
```

## P7 must-know

1. **`std.mem.pageAlloc` + `std.mem.allocBytes` are the next wall** — the kernels build past every
   div/mod/slice/eqlBytes/copy path and bail **TAR002 "Heap capability"** (the manifest gate), and
   the `mem-bytes-as-{mut-*,…}` / `mem-mut-span-slice` family bail the same way. P7 lifts the
   capability AND lowers the allocator. **On Linux mmap is a SYSCALL, not a libc/libSystem call** —
   so unlike macho's P2a (which forced obj+link to bind `_mmap`), these fixtures stay on the
   **direct-exe** path (exactly like the x86-64 ELF backend's raw `mmap` syscall). The raw aarch64
   Linux syscall numbers (number in **x8**, args x0–x5, `svc #0`): **mmap=222, munmap=215, openat=56,
   read=63, lseek=62, close=57** — NOT the BSD `svc #0x80`-with-class form macho uses. (`write=64`,
   `exit=93` are already wired from P1.)
2. **The IR value kinds + lowering** — port the ELF x86 backend's anon-mmap + file-mmap, not macho's
   libc-call clone (macho is obj+link; ELF is syscall, which is what aarch64-linux is):
   - `IR_VALUE_PAGE_ALLOC` LOCAL_SET: zero the 16-byte ALLOC slot (a PageAlloc reserves no buffer;
     each allocBytes mmaps fresh). Mirror `emit_elf64.c`'s page-alloc init (the `MAP_ANON` path).
   - `IR_VALUE_ALLOC_BYTES` (is_page_alloc source): emit the `mmap` **syscall** — `mmap(addr=0, size,
     PROT_READ|PROT_WRITE=0x3, MAP_ANON|MAP_PRIVATE, fd=-1, off=0)`. **Linux `MAP_ANON=0x20`,
     `MAP_PRIVATE=0x2` → flags `0x22`** (NOT darwin's `0x1002`). Spill the size to the integer scratch
     slot across the `svc` (the syscall clobbers x0..x5/x8 — `svc` is a "call"); add `IR_VALUE_ALLOC_
     BYTES` to the `a64_value_int_depth` spills set (the macho P2a lesson). Populate the
     `Maybe<MutSpan<u8>>` (has@0, ptr@8, len@16). Failure: `mmap` returns `MAP_FAILED` (-1) → a
     negative result; `cmp x0,#0; b.lt fail` (a valid user address is never negative). Kernel-zeroed
     pages = calloc semantics (the token-for-token bar depends on it).
   - `IR_VALUE_FS_MMAP` (file mmap, `Maybe<owned<Mapping>>`): `openat(AT_FDCWD=-100, path,
     O_RDONLY=0)` → `lseek(fd, 0, SEEK_END=2)` for the size → `mmap(0, size, PROT_READ=1,
     MAP_PRIVATE=2, fd, 0)` → `close(fd)`, all raw `svc`. Mirror `emit_elf64.c`'s
     `elf_emit_mmap_file_addr_size` (the x86 syscall sequence; aarch64 uses openat=56 not x86's
     open=2). An `owned<Mapping>` drop → `IR_VALUE_FS_MUNMAP` = `munmap(addr, len)`. `mappingBytes` is
     a span-field load (already supported). The macho doc's `is_mapping` flag on `IrLocal` already
     exists in `ir.c`/`zero.h` (P2b added it) — read it to widen the mapped span's len slot to 64-bit
     (`str x`/`ldr x` @slot+8) so a >4 GiB file length round-trips (the `mmap-maybe-4gib-len` fixture);
     all other spans keep the 32-bit count.
3. **Lift the heap/fs/args/env capabilities** in `target.c`'s **two** linux-arm64 manifests
   (`linux-musl-arm64` @target.c:50–61, `linux-arm64` @target.c:76–87) — they currently declare only
   `["memory","stdio","time","rand"]`; add `args env fs heap` (mirror the `linux-musl-x64` /
   `darwin-arm64` manifests). This is what clears the TAR002 gate. **No `ir_needs_zero_runtime_object`
   change** — on Linux these are syscalls, so the dispatch stays direct-exe (the macho target-aware
   obj+link branch does NOT apply to aarch64-linux; do not route pageAlloc through obj+link here).
4. **The Maybe ABI** — `Maybe<MutSpan<u8>>` / `Maybe<owned<Mapping>>` use has@0, ptr@8, len@16 (the
   same offsets as ELF x86 + macho); the `IR_VALUE_MAYBE_SCALAR_LITERAL` payload path (if it arises)
   must use `a64_emit_mov_imm64` for a 64-bit payload (the latent-bug-#1 discipline). The `.has`/
   `.value` accessors + `IR_TYPE_MAYBE_BYTE_VIEW`/`IR_TYPE_MAYBE_SCALAR` locals may need admitting in
   `a64_validate_function` (check what args/env/JSON-parse already lower to — the macho doc notes most
   Maybe plumbing was present from earlier phases; verify here).
5. **Fixtures P7 flips:** `generate-argmax`, `generate-argmax-q`, `transformer-forward`,
   `transformer-forwardq` (the milestone — native token-for-token, the macho P2a headline),
   `page-alloc-region`, `mmap-file-readonly`/`mmap-file-notfound`/`mmap-maybe-success`/
   `mmap-maybe-4gib-len`/`mmap-munmap-loop`/`mmap-munmap-early-return`, `mem-bytes-as-{mut-f32,f64,
   i32,mut-i32,mut-i8}`, `mem-mut-span-{slice,u8-store}`, `mem-bytes-as-f32-mmap`, `std-args`,
   `ops-q-*`. They are wired into the run.mjs runtime loops, so they auto-flip as the backend stops
   bailing — expect `linux-arm64 (docker)` to rise from 65 well into the 80s (toward the ~106
   reachable target). Verify `generate-argmax` reproduces the exact token sequence (`2 3 1 0 2 3`)
   token-for-token vs the libm C reference (the macho P2a milestone bar).

## P7 — what landed

The **anonymous + file mmap/munmap allocator and std.args** layer via raw Linux `svc` syscalls, plus
the capability lift, in `emit_elf_aarch64.c` (2805 → **3113 lines, +308**), `targets/targets.manifest`
+ `target.c` (the two linux-arm64 capability rows), and `main.c` (the dispatch-routing for the syscall
mmap family). The macho P2a/P2b arm64 register/Maybe shapes were ported under the `a64_` prefix, but
where macho issues `bl _libc` externals (obj+link), the AArch64-Linux backend issues the syscall
directly (`svc #0`), so pageAlloc + file-mmap stay on the **direct-exe** path (exactly the x86-64 ELF
model). A libm kernel still routes to obj+link via P3 — they coexist (generate-argmax is obj+link for
libm AND emits the pageAlloc/mmap `svc` sequences in the same object).

**Result: `conformance ok`; darwin-arm64 native unchanged at 87 ran / 46 skipped (committed macho path
unregressed); `linux-arm64 (docker): 65 ran → 88 ran, 68 → 45 skipped`; `/tmp/compare.mjs:
compared=100 armOnlyBuilds=1 MISCOMPILES=0`** (every fixture that builds on BOTH linux-musl-arm64 and
darwin-arm64 matches darwin token-for-token; the one armOnlyBuild is `parse-integers`, which arm64 now
runs correctly but darwin's macho backend still bails — arm-ahead-of-darwin, not a miscompile; see
below).

### The flip set — 23 fixtures (65 → 88 ran), all correct in docker

`page-alloc-region`, `mmap-file-readonly`, `mmap-file-notfound`, `mmap-maybe-success`,
`mmap-maybe-4gib-len`, `mmap-munmap-loop`, `mmap-munmap-early-return`, `mem-bytes-as-{mut-f32,f64,i32,
mut-i32,mut-i8}`, `mem-mut-span-{slice,u8-store}`, `mem-bytes-as-f32-mmap`, `std-args`,
**`generate-argmax`**, **`generate-argmax-q`**, **`transformer-forward`**, **`transformer-forwardq`**,
`ops-q-matmul`, `ops-q-quantize`, and (as a bonus, once `Maybe<scalar>` `.value` was admitted)
`parse-integers`. All print their `… ok` / expected stdout under `docker run --platform linux/arm64`.

### Syscall numbers + sequences (verified against `<asm-generic/unistd.h>` via `zig cc -E`)

`mmap=222`, `munmap=215`, `openat=56`, `lseek=62`, `close=57` (plus `write=64`/`exit=93` from P1).
The number goes in **x8** (bare, NOT the BSD `svc #0x80`-with-class form macho uses), args in x0–x5,
then `svc #0` (`0xd4000001`). A syscall clobbers x0..x5 + x8, so any live value is spilled to a frame
scratch slot (the existing left-operand-spill discipline) — `a64_emit_svc(number)` is the leaf
(`movz x8,#number; svc #0`).
- **Anon mmap (pageAlloc / allocBytes)** `a64_emit_anon_mmap_to_local`: `mmap(addr=0, len, prot=0x3
  [PROT_READ|PROT_WRITE], flags=0x22 [MAP_ANON|MAP_PRIVATE, **Linux** — NOT darwin's 0x1002], fd=-1,
  off=0)`. The size is evaluated then `str x9,[sp,#slot]` BEFORE the `svc` (survives the clobber);
  `cmp x0,xzr; b.lt fail` (MAP_FAILED=-1 is the only negative). On success the `Maybe<MutSpan<u8>>`
  gets has=1@0, ptr@8, len(count=bytes)@16; on failure it is cleared. Kernel-zeroed pages = calloc
  semantics (the token-for-token bar depends on it). fd=-1 is `movn x4,#0` (`92800004`).
- **File mmap** `a64_emit_mmap_file_addr_size`: `openat(AT_FDCWD=-100, path, O_RDONLY=0)` →
  `lseek(fd,0,SEEK_END=2)` → `mmap(0, size, PROT_READ=1, MAP_PRIVATE=2, fd, 0)` → `close(fd)`, all
  `svc`. AT_FDCWD=-100 is `movn x0,#99` (`92800c60`); the path is materialized into x1 FIRST (it may
  use scratch), then the constant args are set so none of x0/x2/x3 are clobbered. fd→slot, size→slot+8
  (64-bit), addr→slot2 (FS_MMAP reserves two scratch levels via `a64_value_int_depth` → depth 2). The
  fd is **closed on ALL paths** — success, MAP_FAILED, and lseek-failure — so no descriptor leaks
  (`mmap-munmap-loop` runs 1000× clean under `ulimit -n 64`, confirmed). On exit addr is in x0 (negative
  = any failure, flows through), size in x1.
- **munmap** (`IR_VALUE_FS_MUNMAP`): load addr@0 + len@8 (both 64-bit), `munmap(addr,len)` via `svc 215`.
- **std.fs.host** (`IR_VALUE_FS_HOST`): returns 0 (a stateless i32 token, like ELF/macho).

### Maybe<owned<Mapping>> + the `is_mapping` 64-bit length

A Mapping's byte length is stored **64-bit** (files may exceed 4 GiB) via the `is_mapping` flag on
`IrLocal` (added by macho P2b, read here — not re-added). The widened slots:
`a64_emit_byte_view_len_depth` (LOCAL → `ldr x` @slot+8 when `is_mapping`, else `ldr w`; new
`IR_VALUE_MAYBE_VALUE` arm → @slot+16 same rule), `a64_emit_byte_view_ptr_depth` (new
`IR_VALUE_MAYBE_VALUE` arm → ptr @slot+8), and the LOCAL_SET stores (mmapOrRaise → ptr@0/len@8 both
`str x`; mmap-to-Maybe → has@0/ptr@8/len@16). All non-mapping spans keep the 32-bit count. The
`mmap-maybe-4gib-len` fixture round-trips a 4294967301-byte length, confirmed.
- **`check std.fs.mmapOrRaise`** (BYTE_VIEW Mapping local): addr in x0, `cmp x0,xzr; b.ge ok` else
  `brk #0` (a failed required mapping traps — the `check world.out.write` discipline; this backend has
  no error-tag-return machinery), store ptr/len.
- **`std.fs.mmap`** (Maybe<owned<Mapping>>): `cmp x0,xzr; b.lt fail` → has=0 cleared; else has=1, ptr,
  64-bit len.
- **`std.mem.allocBytes`** over a PageAlloc → `a64_emit_anon_mmap_to_local`; a non-page allocator bails
  honestly. **`std.args.get`** → `a64_emit_args_get_to_local`. **`Maybe<scalar>` literal** →
  `IR_VALUE_MAYBE_SCALAR_LITERAL` (has + the payload via `a64_emit_mov_imm64`, the 64-bit-no-truncation
  discipline). `IR_VALUE_MAYBE_HAS` → `ldr w`@0; `IR_VALUE_MAYBE_VALUE` (scalar) → `ldr x`@8.

### std.args seed wiring (the entry-stub fix)

At the ELF entry point the kernel leaves **sp pointing at `[argc, argv0, argv1, …, NULL, envp…]`** — it
does NOT pass argc/argv in registers. P1's prologue already copied x0→x20 (argc) / x1→x21 (argv), but
the start stub did a bare `bl main`, so x0/x1 were garbage. **Fix:** `a64_emit_exe_start_stub` now emits
`ldr x0,[sp]` (argc) + `add x1,sp,#8` (argv) before `bl main` (`f94003e0 / 910023e1 / 94000006` at text
+0/+4/+8). `IR_VALUE_ARGS_LEN` reads `mov w,#20`; `a64_emit_args_get_to_local` indexes
`argv[idx]=*(x21+idx*8)`, measures the NUL-terminated length into a 32-bit count, and populates the
`Maybe<Span<u8>>` (out-of-range → cleared). `std-args alpha beta` prints `alpha` (argv[1]) in docker.
The exit sequence `movz x8,#93; svc #0` (`a8 0b 80 d2 01 00 00 d4`) the conformance assertion hardcodes
is unchanged (it just moved two words later — the assertion uses `bytes.includes`, not a fixed offset).

### Capability lift + the dispatch (NO obj+link for the mmap family)

Both linux-arm64 manifests (`linux-musl-arm64`, `linux-arm64`) gained `args env fs heap` (mirroring
`linux-musl-x64` / `darwin-arm64`), clearing the **TAR002** gate. **The active manifest is the on-disk
`native/zero-c/targets/targets.manifest`** (loaded by `ensure_targets_loaded`, with the embedded
`fallback_manifest` in `target.c` as the fallback for installed builds) — **both** were updated to stay
consistent. `z_target_has_capability` reads the parsed list via `strstr`, so the rows are all that the
gate needs.

The subtle part: the shared default-routing gate `self_host_subset_compatible` rejects any program with
the `fs` capability, which would block the file-mmap fixtures from the **direct-exe** path (they are
`fs`). macho dodges this because `ir_needs_zero_runtime_object` forces macho-mmap to obj+link (which
returns before the gate); on Linux mmap is a syscall, so the program must take direct-exe. The fix
mirrors macho's mmap obj+link special-case, inverted: a new `default_direct_exe_eligible(program, caps,
ir, target)` allows default direct-exe for an `fs`-only program **when** the target exe emitter is
`zero-elf-aarch64-exe` AND the program's fs use is entirely the syscall-lowered mmap family
(`ir_program_fs_use_is_mmap_only`: every fs IR value is FS_HOST/FS_MMAP/FS_MUNMAP — any other fs op,
or time/rand/net/proc/web, still takes the C-transpile/obj path). Wired at the two routing sites
(`target_readiness_select_diag`, the main build dispatch). **`ir_needs_zero_runtime_object` is NOT
changed** — it stays macho-scoped, so x86-64 ELF + macho routing are byte-for-byte unaffected
(verified: x64 default mmap-file still bails CGEN004 → tolerate-skipped exactly as before; darwin
mmap-file-readonly still builds via `cc` obj+link and runs native). `std.fs.mappingBytes` lowers to a
BYTE_VIEW span (ir.c), not an FS_* value, so it does not affect the mmap-only test.

### Harness (`conformance/run.mjs`)

The `assertLinuxArm64NativeOrUnsupported` helper already passed `expected.args ?? []` and
`expected.env`, so std-args's `args:["alpha","beta"]` flows through unchanged. One fix was needed: the
docker run mounted only `outDir`, so a fixture's relative data path (`mmap` of
`conformance/fixtures/*.bin`) could not resolve. Now it mounts the **repo root** at `/repo` with
`-w /repo` and runs `/repo/${outDir}/<exe>` — exactly how the darwin-native helper runs the binary from
the repo root, so the relative path resolves. (The `assertElfAarch64Object/Executable` structure
assertions on `direct-exe-return` stay green — direct-exe-return still exits 42.)

### `parse-integers` — the one arm-ahead-of-darwin build (not a miscompile)

Admitting `IR_TYPE_MAYBE_SCALAR` locals + the `IR_VALUE_MAYBE_VALUE` scalar arm (P7 must-know #5) makes
`parse-integers` (`std.parse.parseU` → `Maybe<u32>`) build AND **run correctly** on arm64 (`parse
integers ok`, harness-validated), while darwin's macho backend still bails CGEN004 "value kind 29
[IR_VALUE_MAYBE_VALUE] unsupported" (it never grew the scalar `MAYBE_VALUE` read). So `compare.mjs`
reports `armOnlyBuilds=1` — but this is a **correct capability the conformance suite independently runs
and asserts**, not a hidden miscompile (the work-map's "bail to match darwin unless trivially
implementable" — here it is trivially implementable and correct). Closing the macho gap on darwin would
restore armOnlyBuilds=0 but is out of P7 scope (it touches the committed macho backend).

### Byte / encoding spot-check (cite the bytes)

`page-alloc-region` `--emit obj` (objdump) — the anon-mmap `svc` sequence: `d2800000` (mov x0,#0
addr), `d2800062` (mov x2,#0x3 prot), `d2800443` (mov x3,#0x22 flags — **Linux** MAP_ANON|MAP_PRIVATE),
`92800004` (movn x4,#0 = fd -1), `d2801bc8` (mov x8,#0xde=222 mmap), `d4000001` (svc #0), `eb1f001f`
(cmp x0,xzr), with `f90003e9` (str x9,[sp] — the size spill across the syscall) before it.
`mmap-file-readonly` — the four syscall numbers in order: `mov x8,#0x38` (56 openat) with `mov x0,#-0x64`
(`92800c60`, AT_FDCWD=-100), `mov x8,#0x3e` (62 lseek), `mov x8,#0xde` (222 mmap), `mov x8,#0x39` (57
close) appearing **twice** (success+lseek-fail close paths). **Byte-identity vs the committed macho
object** (the keystone): page-alloc-region's Maybe-populate words are identical in the darwin-arm64
`otool -t` dump and the ELF object — `f90003e9` (str x9,[sp]), `b9004be9` (str w9 has), `f9002be0`
(str x0 ptr), `b9005be9` (str w9 len), `52800029` (movz w9,#1). Only the mmap mechanism differs
(ELF `mov x8,#0xde; svc #0` vs macho `bl _mmap`), exactly the keystone insight. Entry stub (exe text
+0): `f94003e0` (ldr x0,[sp]), `910023e1` (add x1,sp,#8), `94000006` (bl main), `d2800ba8`
(movz x8,#93 exit).

### emit_macho64 / emit_elf64 reference-line corrections

All references used were accurate against the tree at write time. macho arm64 sources ported:
`macho_emit_anon_mmap_to_local` @2640, `macho_emit_mmap_file_addr_size` @2583, `macho_emit_args_get_to_
local` @2538, the `IR_VALUE_FS_MUNMAP`/`FS_HOST`/`ARGS_LEN`/`MAYBE_HAS` value arms @1765–1786, the
LOCAL_SET mmap/pageAlloc/args/Maybe blocks @2690–2856, `macho_value_int_depth` FS_MMAP=2 / ALLOC_BYTES
spill @2147–2167, the prologue x20/x21 seed @3088–3096 + epilogue @2375–2378, `.seed_main_process_args
=true` @3202, the `is_mapping` 64-bit len reads @1377/1388. emit_elf64 syscall-sequence patterns:
`elf_emit_mmap_file_addr_size` @2892 (openat→lseek→mmap→close, fd-closed-on-all-paths), the
`IR_VALUE_PAGE_ALLOC` init / `MAP_ANON` flag `0x22` @3153/3319, `IR_VALUE_ALLOC_BYTES` is_page_alloc
@3307, `IR_VALUE_FS_MUNMAP` @1702, `IR_VALUE_ARGS_GET/LEN` @3233/1590, the process-args seed
@682/3598/3614. The aarch64 number-in-x8 / `svc #0` form replaces both x86's `syscall` and macho's
`bl _libc`. No drift found.

## P7-fix — what landed

A targeted correctness fix for a SEGFAULT that only manifested on the **obj+link** entry path — which
is exactly the path the llama2 example takes (it uses libm via rmsnorm/softmax → sqrtf/expf, forcing
obj+link per P3). The example ran perfectly on darwin-arm64 but crashed (exit 139) on linux/arm64 the
moment it was given a model arg. One-line codegen fix in `emit_elf_aarch64.c` (plus self-documenting
comments and a new regression fixture). **No `target.c`, `ir.c`, `emit_elf64.c`, or `emit_macho64.c`
change.**

### Root cause (confirmed via objdump on the linked binary)

P7's "std.args seed wiring" section above documents the **direct-exe** seed (the `_start` stub feeds
x0=argc/x1=argv, main's prologue copies them into the callee-saved x20/x21 that `ARGS_LEN`/`ARGS_GET`
read). But that prologue seed is gated on the `seed_main_process_args` flag passed into
`a64_emit_function_text`, and the **object path** (`z_emit_elf_aarch64_object_from_ir`) passed it as
**`false`**. So in an obj+link binary, `main`'s prologue never seeded x20/x21. Under the obj+link
binary's musl crt0 (`_start` → `__libc_start_main` → `main`), main is called per the C ABI with
x0=argc/x1=argv — but the AArch64 `ARGS_LEN`/`ARGS_GET` lowerings read x20/x21 **unconditionally**, and
those were never written, holding garbage. The first argv access (`add x12,x21,idx,lsl#3; ldr x12,[x12];
ldrb w14,[x12]`) faulted. Proven: the buggy `main` prologue was `stp x29,x30,…; mov x29,sp; sub sp,…;
mov w8,w20; …; cmp w10,w20` — note **no `stp x20,x21` and no `mov x20,x0`/`mov x21,x1`** — then it read
the argc-loop bound straight off the unseeded w20. (The direct-exe path worked because it passed the
flag as `true` AND its own `_start` seeded x0/x1; the `std-args` fixture is pure → direct-exe, so it
never exercised obj+link+args; `generate-argmax` is obj+link but its synthetic checkpoint has no argv —
that gap hid the bug.)

### The fix + why this design mirrors emit_elf64.c

`z_emit_elf_aarch64_object_from_ir` now passes `seed_main_process_args = true` (emit_elf_aarch64.c
~2828), exactly like emit_elf64.c's object path which sets `.seed_main_process_args = true`
(emit_elf64.c:3790) so its `main` always seeds the C-ABI arg registers (rdi/rsi) into its callee-saved
arg registers regardless of whether Zero's own `_start` or crt0 invoked it. This is the **unified
design (a)**: both aarch64 entry paths deliver argc in x0 / argv in x1 before `bl main` (the direct-exe
`_start` stub reads them off the kernel stack; crt0 supplies them per the C ABI), and main's prologue
ALWAYS does the x0→x20/x1→x21 seed when it is `main`. AArch64's read side is even simpler than x86-64's
(x86-64 gates the ARGS reads on the flag — callee-saved r14/r15 when seeded, raw `[r15]` stack-image
decode in the direct-exe; aarch64 always reads x20/x21, so flipping this one flag fully unifies both
paths). x20/x21 are callee-saved: the prologue `stp x20,x21,[sp,#-16]!` and epilogue `ldp x20,x21,
[sp],#16` (both already gated on the same flag) now also run in the obj+link `main`, correctly
preserving them for the crt0 caller. The extra save/seed is harmless for an obj+link program that does
not use args (e.g. `generate-argmax` — it stp/ldp x20/x21 but never reads them).

The post-fix `main` prologue in the linked llama2 binary (objdump): `a9bf57f4` (stp x20,x21,[sp,#-16]!),
`a9bf7bfd` (stp x29,x30,…), `910003fd` (mov x29,sp), `d11f43ff` (sub sp,sp,#0x7d0), **`aa0003f4`
(mov x20,x0 = argc)**, **`aa0103f5` (mov x21,x1 = argv)**, then `2a1403e8` (mov w8,w20) — the seed now
precedes every body read of x20/x21.

### Regression fixture (the gap that hid this)

`conformance/native/pass/std-args-libm.0` — the first fixture combining **libm (→ obj+link) AND
std.args**. It reads argv[1] via `std.args.get` (needs x20/x21), takes its byte length, applies
`std.math.sqrtf` (forces obj+link), and prints `std args libm ok` only when both the argv length and
the libm result check out (argv[1] is 9 chars → `sqrtf(9.0) == 3.0`, an exact f32). Wired into run.mjs:
the `zero check` list and the runtime-fixture loop with `args:["123456789"]`, so the linux-arm64 docker
branch runs it WITH argv (and x86-64/darwin run it too). Confirmed it **segfaults (exit 139) before the
fix and prints `std args libm ok` (exit 0) after** — on both linux-arm64 (obj+link, statically linked
with crt0: `__libc_start_main` present) and darwin-arm64 (obj+link via libSystem).

### Validation evidence

```
$ make -C native/zero-c                                      # clean, no -Wall -Wextra -Wpedantic warnings
$ bin/zero build --backend zero-elf-aarch64 --emit exe --target linux-musl-arm64 examples/llama2 --out .zero/llama2-arm64
$ docker run --rm --platform linux/arm64 -v $(pwd):/repo -w /repo alpine \
    /repo/.zero/llama2-arm64 /repo/stories15M.bin --prompt "Once upon a time" --tokens 32 --temperature 0
  Once upon a time, there was a little girl named Lily. She loved to play outside in the sunshine. One day, she saw a big
  EXIT: 0                                                    # coherent text == darwin reference; was exit 139 before the fix
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs    # conformance ok
  darwin-arm64 native: 88 ran, 46 skipped                   # 87 + std-args-libm; committed macho path unregressed
  linux-arm64 (docker): 89 ran, 45 skipped                  # 88 + std-args-libm
$ node /tmp/compare.mjs
  compared=101 armOnlyBuilds=1 MISCOMPILES=0                 # armOnlyBuild = parse-integers (arm-ahead-of-darwin, runs correct)
# non-regression: generate-argmax obj+link still token-for-token (arm + darwin "generate argmax ok");
#   std-args direct-exe still "alpha"; linux-musl-x64 std-args "alpha"; darwin-arm64 std-args "alpha"
```

### emit_elf64 reference-line corrections

Accurate at write time: `elf_function_seeds_process_args` @682, the object-path
`.seed_main_process_args = true` @3790, the exe-path ctx (no flag → defaults false) @4358, the
`IR_VALUE_ARGS_LEN` flag-gated read @1590, `elf_emit_function_text`'s rdi/rsi→r14/r15/r13 seed
@3614–3624. No drift found.

## P8 must-know

1. **Cross-module compilation already works** and the kernels run token-for-token, so P8 is wiring +
   validation, not compiler work. `generate-argmax`/`generate-argmax-q`/`transformer-forward(q)` already
   reproduce the libm C reference's exact token stream (`2 3 1 0 2 3`) in docker via the obj+link path
   (libm + the pageAlloc/mmap `svc` sequences in one object). The smallest argmax margin is 0.077, far
   above libm ULP slack.
2. **The example invocation — VERIFIED running in docker (P7-fix).** Build the llama2 example with
   `--backend zero-elf-aarch64 --target linux-musl-arm64` (the libm + pageAlloc + file-mmap of the
   checkpoint all route correctly: libm forces obj+link via P3, the mmap family rides along as `svc`
   syscalls in the same object — `generate-argmax` already exercises exactly this). The example reads its
   model path + flags from **argv**, which now works on the **obj+link** path too (P7-fix: the object
   path's `main` now seeds x20←x0/x21←x1 — P7's entry-stub seed alone only covered direct-exe, and the
   libm-using example takes obj+link). It maps the checkpoint file with `std.fs.mmap`
   (Maybe<owned<Mapping>>, 64-bit length) and uses `std.mem.pageAlloc` for the RunState — both landed in
   P7. Confirmed end-to-end: `docker run --platform linux/arm64 … /repo/.zero/llama2-arm64
   /repo/stories15M.bin --prompt "Once upon a time" --tokens 32 --temperature 0` generates the coherent
   "…a little girl named Lily…" stream (== darwin), exit 0. So P8 is purely validate.sh + README.
3. **validate.sh linux/arm64 branch.** validate.sh already has a Docker branch (currently
   `--platform linux/amd64` under qemu). Add a `linux/arm64` path: the Zero exe via the backend/target
   above, run under `docker run --platform linux/arm64` (native on Apple Silicon, no qemu — full speed);
   the C reference via **`zig cc -target aarch64-linux-musl`** (the Zero-parity-libm-musl rule — both
   sides link zig's bundled musl, so the libm is bit-identical). When mounting the repo into the
   container, run with the repo root as the working directory (or pass an absolute model path) so the
   checkpoint's relative path resolves — the same `-v <repo>:/repo -w /repo` pattern P7 added to
   `run.mjs` (a relative path mounted only at `outDir` will `brk` on a failed mmap).
4. **Same musl libm both sides → token-for-token for f32 AND int8.** Unlike macOS (where int8 diverged
   from an `-O3` FMA-contraction effect), linux-arm64 expects token-for-token for **both** f32 and int8,
   because the Zero exe and the C ref both link zig's musl. Build the C ref with
   `zig cc -target aarch64-linux-musl` (and, to be safe against any FMA contraction, `-ffp-contract=off`
   — see the macOS P6 int8 blocker doc for the pattern; the Zero backend's scalar FP already does not
   contract). The int8 quant path (`generate-argmax-q`, `transformer-forwardq`, `ops-q-*`,
   `checkpoint-q-v2`) already runs correctly in docker.
5. **README.** Add the linux-arm64 row to the cross-platform support matrix (the macOS campaign closed
   limitation #1 for darwin; P8 closes it for linux-arm64 native-in-docker). No compiler change is in
   P8 scope — if a fixture or the example bails, it is a wiring/path issue, not a backend gap.

## P8 — what landed

Wiring + validation only — **no compiler change** (`emit_elf_aarch64.c`, `target.c`, `ir.c`,
`main.c`, `emit_elf64.c`, `emit_macho64.c`, `run.mjs` all untouched; P1–P7-fix already landed every
primitive and the example was proven running in docker by P7-fix). Two files changed:
`examples/llama2/validate.sh` (the linux/arm64 branch) + `examples/llama2/README.md` (the support
matrix). The llama2 example builds + runs in **native arm64 docker** with **f32 AND int8
token-for-token** parity vs `llama2.c`/`runq.c`, both **gated**.

### validate.sh branch design — a third branch reusing the whole matrix (the three indirections)

The script already had two branches (darwin-native, Docker/amd64) sharing the entire parity matrix
(`parity_one`/`parity_topp`/`parity_one_q`/`parity_topp_q`, the flag mapping `-p 0 -s 12345` /
`--topp`/`-p <pp>` / v2 auto-detect, `norm_and_diff`, OK/FAIL/DIFF/PASS reporting, `q_result`) via
three host-aware indirections — **`build_zero`**, **`build_ref`/`zero_target`**,
**`run_pair`/`plat`/`img`** — plus the `q_gated` switch. P8 adds the linux/arm64 path by giving
those same indirections their arm64 values; **none of the matrix below them changed.**

- **Selection** (`validate.sh:73–94`): `native=1` only on a darwin-arm64 host *without*
  `LLAMA2_FORCE_DOCKER=1` (unchanged). Otherwise the **Docker branch** picks its container platform
  from the host arch — `case "$(uname -m)"`: `arm64|aarch64 → linux/arm64`, else `linux/amd64` —
  overridable with **`LLAMA2_DOCKER_PLATFORM`** (e.g. `=linux/amd64` to force the amd64 gate on an
  arm64 host / CI). `plat` is then mapped to `docker_target` (`aarch64-linux-musl` /
  `x86_64-linux-musl`) and `docker_arch` (`arm64` / `amd64`). So on this Apple Silicon host
  `LLAMA2_FORCE_DOCKER=1 bash examples/llama2/validate.sh` runs the **linux/arm64** branch (native,
  no qemu); a bare run stays darwin-native; an x86 host still defaults to amd64.
- **`build_zero`** (`validate.sh:154–162`): arm64 → `"$zero" build --backend zero-elf-aarch64 --emit
  exe --target linux-musl-arm64 …`. (amd64 `else` arm unchanged: `zero-elf64`/`linux-musl-x64`.)
- **`build_ref` / `zero_target`** (`validate.sh:126`, `134–152`): on the Docker branch
  `zero_target="$docker_target"`, so for arm64 `build_ref` runs `zig cc -target aarch64-linux-musl
  -O3 -ffp-contract=off …` — the **same `zig cc` toolchain** the Zero exe links (zig's bundled musl),
  so libm is bit-identical (the Zero-parity-libm-musl rule), and `-ffp-contract=off` (already in the
  shared `build_ref`) keeps the C ref's scalar FP non-contracted to match the Zero backend's separate
  fmul+fadd.
- **`run_pair` / `plat` / `img`** (`validate.sh:65–67`, the existing `run_pair`): runs
  `docker run --rm --platform "$plat" -v "$data":/work -w /work "$img" sh -c …` with
  `plat=linux/arm64`, `img=alpine` (multi-arch — same image serves both platforms). On Apple Silicon
  Docker's Linux VM is itself arm64, so this is native (no qemu).
- **`q_gated`** (`validate.sh:255–266`): unchanged code (`native==1 → 0, else → 1`) but the comment
  now states int8 is gated on **both** Docker platforms (amd64 AND arm64) — the arm64 branch is
  `native==0`, so it is **gated**, matching the amd64 gate (NOT darwin's informational treatment).

The amd64 path is behavior-preserving: on an x86 host `host_plat=linux/amd64`, `docker_target=
x86_64-linux-musl`, `build_zero`'s `else` arm and `run_pair`/`build_ref` resolve to the original
amd64 values (verified by diff-review; an x86 runner reproduces the prior run exactly).

### f32 + int8 token-for-token PASS evidence (`LLAMA2_FORCE_DOCKER=1`, native arm64 docker)

```
$ LLAMA2_FORCE_DOCKER=1 bash examples/llama2/validate.sh
==> host: Darwin/arm64 — Docker/Linux branch (linux/arm64 container, native on arm64; musl libm)
==> building llama2.zero (--backend zero-elf-aarch64, linux-musl-arm64)
==> building llama2.c reference (zig cc -> aarch64-linux-musl, matching the Zero exe)
==> temperature 0 (deterministic argmax) parity
  OK   [t=0, 100 tok] "Once upon a time"
  OK   [t=0, 120 tok] "Lily and Tom went to the park"
  OK   [t=0, 80 tok] "The little robot"
  OK   [t=0, 256 tok] "One day"
  OK   [t=0, 60 tok] ""
==> temperature > 0 (softmax + xorshift* PRNG + multinomial) parity
  OK   [t=0.8, 100 tok] "Once upon a time"
  OK   [t=1.0, 100 tok] "The dragon"
  OK   [t=0.5, 80 tok] ""
==> temperature > 0 with top-p (nucleus) parity
  OK   [t=0.8, p=0.9, 100 tok] "Once upon a time"
  OK   [t=1.0, p=0.95, 100 tok] "The dragon"
  OK   [t=0.9, p=0.9, 80 tok] ""
==> [quantized] temperature 0 (deterministic argmax) parity
  OK   [q, t=0, 100 tok] "Once upon a time"
  OK   [q, t=0, 120 tok] "Lily and Tom went to the park"
  OK   [q, t=0, 80 tok] "The little robot"
  OK   [q, t=0, 60 tok] ""
==> [quantized] temperature > 0 (multinomial) parity
  OK   [q, t=0.8, 100 tok] "Once upon a time"
  OK   [q, t=1.0, 100 tok] "The dragon"
==> [quantized] temperature > 0 with top-p (nucleus) parity
  OK   [q, t=0.8, p=0.9, 100 tok] "Once upon a time"
  OK   [q, t=0.9, p=0.9, 80 tok] ""
==> PASS: llama2.zero matches llama2.c token-for-token on stories15M (f32).
==> PASS: llama2.zero matches runq.c token-for-token on stories15M_q80.bin (int8).   # SCRIPT EXIT 0
```

**f32: 11/11 OK** (temp 0 ×5, temp>0 ×3, top-p ×3). **int8: 8/8 OK, GATED** (temp 0 ×4, temp>0 ×2,
top-p ×2). The int8 result is the headline divergence from darwin: on linux/arm64 the int8 path is
**bit-exact** (both sides link zig's musl + `-ffp-contract=off`), so it stays **gated** (FAIL on
divergence) and PASSes — the macOS FMA-contraction tight-margin flip does **not** occur here, so no
`.docs/llama2/blockers/` doc was needed (verified empirically: zero DIFF lines, exit 0).

Spot-runs (coherent text, exit 0):
```
$ docker run --rm --platform linux/arm64 -v $(pwd):/repo -w /repo alpine \
    /repo/.zero/llama2-arm64 /repo/stories15M.bin --prompt "Once upon a time" --tokens 40 --temperature 0
  Once upon a time, there was a little girl named Lily. She loved to play outside in the sunshine. One day, she saw a big, red ball in the sky. It
$ docker run … --prompt "The dragon" --tokens 40 --temperature 0.8
  The dragon was big and beautiful. It belonged to a little boy who was only three years old. …
```

### README change

`examples/llama2/README.md`: the **Status** line, **Quickstart** (arm64 build + an arm64 `docker run`
note), **Build** (arm64 build command + "linux-musl-x64, linux-musl-arm64, and darwin-arm64 are the
supported targets"), the **Validation** host→branch table (now 3 rows: darwin-native, Docker/arm64,
Docker/amd64) + the platform-selection prose (`LLAMA2_DOCKER_PLATFORM` / `LLAMA2_FORCE_DOCKER`), the
**int8 gating note** (gated/token-for-token on **both** Docker platforms, informational only on
darwin), and **closed limitation #1** ("Target: linux-musl-x64, linux-musl-arm64 (native-in-Docker on
Apple Silicon), and darwin-arm64 …"; Windows is the sole remaining backlog row).

### No-regression evidence

```
$ make -C native/zero-c                                      # clean (no C changed; nothing to rebuild)
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs    # conformance ok
  darwin-arm64 native: 88 ran, 46 skipped                    # unchanged (P8 is not a compiler change)
  linux-arm64 (docker): 89 ran, 45 skipped                   # unchanged
$ bash examples/llama2/validate.sh                           # darwin host → native branch, no force
  ==> PASS: llama2.zero matches llama2.c token-for-token on stories15M (f32).   # f32 11/11, exit 0
  ==> NOTE: int8 (runq.c) had 4 informational divergence(s) …                   # darwin int8 unchanged (non-gating)
```

The darwin-native branch is byte-for-byte the pre-P8 behavior (f32 11/11 gated PASS; int8 4 DIFF
informational + NOTE, exit 0) — the macOS campaign's validate.sh path is unregressed. The amd64
Docker path was diff-reviewed (an x86 host/`LLAMA2_DOCKER_PLATFORM=linux/amd64` resolves to the
original amd64 indirection values) — not run here (needs an x86 host/qemu), but the code path is
unchanged.

## Campaign complete

The Linux ARM64 (ELF aarch64) port of the llama2 example is **DONE**. End state:

| Phase | Title | Status |
|---|---|---|
| P0 | Grounding + docker + encoder decision + progress doc | **done** |
| P1 | Honest backend: ELF container + integer codegen + `svc` write/exit + harness branch | **done** |
| P2 | Scalar f32/f64 + FP register ABI | **done** |
| P3 | libm via obj+link + R_AARCH64_CALL26 | **done** |
| P4 | byte-view reinterpret + typed-span load/store + codec | **done** |
| P5 | Aggregate ABI (sret x8, span/record params) | **done** |
| P6 | div/mod + kernel ops + eqlBytes/slice/copy/fill | **done** |
| P7 | mmap/munmap/pageAlloc via raw `svc` + lift capabilities | **done** |
| P7-fix | obj+link `main` process-args seed (x0/x1→x20/x21) | **done** |
| P8 | Wire example + validate.sh linux/arm64 + README; token-for-token | **done** |

- **Backend:** `emit_elf_aarch64.c` grew from the 346-line permissive-miscompiler MVP to a complete,
  honest AArch64 ELF backend (~2800 lines) — the macho arm64 instruction-selection layer ported under
  the `a64_` prefix into an `emit_elf64.c`-shaped ELF container with raw-`svc` Linux syscalls and
  R_AARCH64 relocations for the obj+link (libm) path. Every unimplemented construct bails CGEN004/
  TAR002 precisely; **zero miscompiles**.
- **Conformance:** `linux-arm64 (docker): 82 ran, 52 skipped` (from the MVP's invisible 0; darwin-arm64
  88 ran for cross-check). `conformance ok`, no regression to the committed x86-64 ELF or macOS Mach-O
  backends across the whole campaign. (Was 89 ran before the post-campaign fs-mmap carve-out revert
  below; the ~7 pure-file-mmap fixtures now tolerate-skip without `--backend`, matching x64.)
- **llama2 example:** builds `--backend zero-elf-aarch64 --target linux-musl-arm64` and runs in
  **native arm64 docker** (no qemu on Apple Silicon), generating coherent text identical to the darwin
  reference, **token-for-token vs `llama2.c` (f32) AND `runq.c` (int8)** — both **gated** in
  `validate.sh` (`LLAMA2_FORCE_DOCKER=1`). int8 is bit-exact on linux-arm64 (unlike darwin's
  informational int8), because both sides link zig's bundled musl.

Remaining backlog (out of this campaign): Windows, Linux-ARM64-on-real-hardware/CI runner, SIMD/threads
(item H), training (item T) — see `.docs/llama2/enhancements.md`.

## Post-campaign update — fs-mmap carve-out reverted (match x64)

P7 had added an AArch64-only `default_direct_exe_eligible` carve-out in `main.c` so that
**fs-mmap-only** programs build via the default direct-exe path without `--backend` (the syscall analog
of macho's mmap obj+link routing). It was principled and safe (verified: non-mmap fs still bailed; zero
miscompiles) but it made aarch64 **diverge from x64**, which rejects the same programs without
`--backend`. Decision: **revert it so aarch64 mirrors x64 exactly** — keep the port a clean
"x64-semantics for arm64" change with no backend-local policy to justify.

- **Reverted:** removed `default_direct_exe_eligible` / `ir_program_fs_use_is_mmap_only` /
  `ir_instrs_use_non_mmap_fs` / `ir_value_uses_non_mmap_fs` from `main.c`; the two call sites use
  `self_host_subset_compatible` again (as before P7). `ir_program_uses_libsystem_mmap` stays (macho).
- **Effect:** a pure-file-mmap program with no `--backend` now bails CGEN004 on aarch64 (identical to
  x64), so the ~7 file-mmap fixtures (`mmap-*`, `mem-bytes-as-f32-mmap`) tolerate-skip in the harness
  rather than run: `linux-arm64 (docker): 89 → 82 ran, 45 → 52 skipped`. They still build + run **with**
  `--backend zero-elf-aarch64` (and the llama2 example, which uses `--backend` + libm→obj+link, is
  unaffected — still token-for-token f32 + int8). Gold-standard arm64-vs-darwin sweep: 94 compared,
  **0 miscompiles**. Clean build, `conformance ok`, x64 + darwin untouched.
- **The improvement, done right, is deferred to a separate item:** `enhancements.md` item **I**
  (precise fs-mmap direct-exe gate) — make eligibility precise and **target-general** (x64 + arm64
  together), separating the `fs` *capability* (still required for mmap) from *direct-exe build
  mechanics*, decided on its own merits rather than as a side effect of the arm64 port.
