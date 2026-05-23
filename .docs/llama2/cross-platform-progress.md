# llama2.zero cross-platform (darwin-arm64) — progress + findings

Handoff artifact for the phased plan in [`cross-platform.md`](./cross-platform.md). Every phase
reads this, appends its findings, and marks its row. Terse + factual. Line numbers verified
against the tree at the time the row was written; correct drift when you touch a file.

**DONE — llama2 runs natively on Apple Silicon, parity validated.** All phases P0–P6 complete:
the example builds + runs natively (`--backend zero-macho64 --target darwin-arm64`, no Docker)
and `validate.sh`'s native branch passes the **f32 parity matrix token-for-token** (temp 0,
temp>0, top-p) vs `llama2.c` built with the matching `zig cc -target aarch64-macos` toolchain.
README limitation #1 (cross-platform / native macOS) is **closed**. (int8 is informational on
darwin — a tight-margin FP-contraction effect, not a bug; see P6 below + the blocker doc.)

## Phase status

| Phase | Title | Status | Notes |
|---|---|---|---|
| P0 | Harness: build+run macho64 exes natively | **done** | exit-code fixture + darwin-native harness branch; keystone documented below |
| P1 | Lift math gate + arm64 libm ABI | **done** | scalar f32/f64 layer + libm external-call ABI landed; 37 FP/libm fixtures flipped skip→run (6→43). See "P1 — what landed" |
| P2 | mmap/munmap + pageAlloc via libSystem | **done** | **anon (heap) in P2a**; **file mmap in P2b**: `std.fs.host`/`std.fs.mmap`/`mmapOrRaise`/`mappingBytes`/`munmap` lower to `_open`/`_lseek`/`_mmap`/`_close`/`_munmap` libSystem calls; `owned<Mapping>` is a 64-bit-len byte view; the dispatch scanner forces obj+link for any macho program using the mmap family. All 7 file-mmap fixtures (incl. the 4 GiB-length and 1000× map/unmap-no-leak cases) run native. Also added the macho fallible-function ABI (error tag in x1) + float `IR_VALUE_CHECK`. See "P2a/P2b — what landed" |
| P3 | byte-view reinterpret + typed-span idx load/store | **done** | reinterpret (`lsr` len), typed-span index load/store (u8/i8/i32/u32/f32/f64; i64/u64 width parity-ready for P2/P4), codec readF*/readInt LE, span-over-typed-array ptr. 9 fixtures flipped skip→run (43→52). `typed-span-*`/`mem-*-mut-*`/`mem-mut-span-slice` still need P2(pageAlloc)/P4(span params). See "P3 — what landed" |
| P4 | Aggregate ABI (sret via x8, span-in-record) | **done** | sret via x8 (record literal/local/param/call returns), record params (copy-in by ptr), span (byte-view) params (ptr,len in two x-regs), span returns (ptr/len in x0/x1), span-in-record fields, record-to-record copy, record/shape-literal call args (incl. nested). Also fixed a latent integer-binary/compare x8-clobber via per-depth frame spill slots. 8 fixtures flipped skip→run (52→60). `fail/aggregate-record-arg-while` still CGEN004. See "P4 — what landed" |
| P5 | f32 / NEON instruction selection (long pole) | partial | **scalar f32/f64 DONE in P1**; **Span<f32> typed-index load/store DONE in P3**; **Span params (P4)**; **scalar instruction set COMPLETE in P5a** — integer `/`,`%` (sdiv/udiv/msub, 32- AND 64-bit) + FP `/` (fdiv) now emit; the four operator-blocked kernel/sampler/tokenizer fixtures run native token-for-token. Remaining: kernel **integration** (`transformer-forward`/`generate-argmax`/`-q`) blocks only on `std.mem.pageAlloc` (P2); NEON deferred to item H. See "P5a — what landed" |
| P6 | Wire example + native validation | **done** | `validate.sh` host-aware native darwin branch (no Docker) + README darwin row + CI note. **f32 matrix token-for-token native (11/11)**; int8 informational on darwin (FP-contraction tight-margin, documented). No compiler change. See "P6 — what landed" |

## P0 — what landed

1. **Exit-code acceptance fixture** `conformance/native/pass/exit-code-arithmetic.0`. A freestanding
   `export c fun main() -> i32` that computes `classify(7*6)` through a helper call + branch and
   returns 42 as the process status. Proves non-trivial native execution beyond stdout.
2. **Darwin-native harness branch** in `conformance/run.mjs`:
   `assertDarwinNativeOrUnsupported(fixture, name, expected)` (defined right after
   `assertDirectRuntimeOrUnsupported`). On a `darwin-arm64` host it ALSO builds each native
   fixture `--emit exe --target darwin-arm64`, runs it, and asserts the same observable result.
   Tolerate-skips fixtures whose feature the Mach-O backend rejects (CGEN004) — the macho analog
   of the libm/BLD003 skip. No-op on non-darwin hosts (suite unchanged on Linux/CI). Prints a
   summary line: `darwin-arm64 native: N ran, M skipped`. Today: **6 ran, 126 skipped** (only
   integer/no-FP/no-aggregate fixtures build; everything FP/mmap/aggregate bails as expected).
   `assertDirectRuntimeOrUnsupported` (the linux build-check) is untouched.
3. Wired the fixture into the `zero check` list, both runtime loops (linux + darwin), and the
   summary print.

### P0 acceptance evidence

```
$ make -C native/zero-c            # clean, no warnings
$ bin/zero build --emit exe --target darwin-arm64 \
    conformance/native/pass/exit-code-arithmetic.0 --out .zero/out/exit-code-arithmetic
$ .zero/out/exit-code-arithmetic; echo $?
42
$ file .zero/out/exit-code-arithmetic
  Mach-O 64-bit executable arm64
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs   # → conformance ok
  darwin-arm64 native: 6 fixture(s) ran, 126 skipped …
```

How to add a fixture to the darwin branch: append it to either runtime loop in `run.mjs`
(it already calls both `assertDirectRuntimeOrUnsupported` and `assertDarwinNativeOrUnsupported`),
or call the helper directly. `expected` reuses the linux shape (`stdout` string|RegExp,
`stderr`, `args`, `env`, `file`) and additionally honours `expected.exitCode` for
status-returning fixtures. As a phase implements a feature, its fixtures flip skip→run
automatically — no harness edit needed.

## External-call / symbol-resolution mechanism (the keystone — verified)

**There are TWO macho64 codegen paths, and they resolve externals very differently. The plan's
P0 prose conflated them — corrected here.**

### Object path — `z_emit_macho64_from_ir` (emit_macho64.c) — uses external relocations

This is the path the plan's keystone description actually refers to. External callees are emitted
as **undefined external symbols** + **`ARM64_RELOC_BRANCH26` relocations with `r_extern`**:

- Reloc shape: `macho_append_call_relocations` (emit_macho64.c:704) builds
  `(symtab_index) | (1u<<24 r_pcrel) | (2u<<25 r_length=4) | (1u<<27 r_extern) | (2u<<28
  ARM64_RELOC_BRANCH26)` (emit_macho64.c:707-711). The world/json/http variants repeat this exact
  word (e.g. emit_macho64.c:720-724).
- Symbol string: `_zero_world_write` appended at emit_macho64.c:1776; the runtime symbols
  (`_zero_json_parse_bytes`, `_zero_http_fetch_result`, …) at emit_macho64.c:1782-1855.
- Symbol-table entry: `N_EXT undefined external` = type byte `0x01` (emit_macho64.c:1936 for
  world_write; same pattern for each runtime symbol). The host linker binds them.
- Call site: a `bl <placeholder>` is emitted and the offset recorded
  (`macho_record_world_write_patch` emit_macho64.c:632; `macho_record_runtime_http_fetch_patch`
  emit_macho64.c:652). The linker — NOT the backend — patches the BRANCH26.

### Direct-exe path — `z_emit_macho64_exe_from_ir` (emit_macho64.c:2104) — NO dynamic binds

This is what `bin/zero build --emit exe --target darwin-arm64` uses for self-host-subset
programs. It is fully self-contained:

- Header flag is **`MH_NOUNDEFS`** (`0x200085` at emit_macho64.c:2271) → declares "no undefined
  symbols".
- `LC_DYLD_INFO_ONLY` (emit_macho64.c:2338) carries **rebase info only** (for PIE data); its
  bind / weak-bind / lazy-bind / export offsets are all **0** (the 8 zeroed u32s at
  emit_macho64.c:2342). **There is no dyld symbol binding at all.**
- `world.out.write` is implemented as an **inline raw `svc #0x80`** Darwin syscall
  (`SYS_write` = `0x02000004`), `macho_emit_exe_world_write` (emit_macho64.c:2095-2102) — it is
  NOT an external libSystem call.
- `LC_LOAD_DYLIB /usr/lib/libSystem.B.dylib` is present (emit_macho64.c:2355-2361) but **no bind
  references it** — it satisfies dyld's load expectation only.
- If any net/HTTP/JSON runtime patch is present, this path **bails**:
  "executable runtime helpers require object emission and an explicit runtime link step"
  (emit_macho64.c:2200). So the direct-exe path can never emit a libSystem-bound external call as
  written today.

### Which path drives obj+link, and which path llama2 will use

`ir_needs_zero_runtime_object` (main.c:4237) returns **true** for the math IR values
`IR_VALUE_MATH_{SQRTF,EXPF,COSF,SINF,POWF,FABSF,FLOORF}` (main.c:4206-4212), in addition to
JSON/HTTP. When true, the build takes the **obj + link** branch (main.c:9478-9595):
`z_emit_macho64_from_ir` → `compile_zero_runtime_object` → `link_zero_runtime_executable`
(main.c:623, invoked at main.c:9550). Otherwise the **direct-exe** branch runs
(main.c:9596 / `z_emit_macho64_exe_from_ir` at main.c:9626).

`link_zero_runtime_executable` shells out via `z_toolchain_link_objects` (the same `zig cc`
toolchain the ELF path uses) and appends `-lm` when `direct_math_runtime_import_count > 0`
(main.c:9549, link flags main.c:633-635). On macOS `-lm` is a harmless no-op (libm IS libSystem).

**Decision for llama2 on macOS:** llama2 uses libm AND mmap, so it takes the **obj + link** path.
External symbols are emitted by the **object** backend as `_`-prefixed undefined N_EXT externals
(the mechanism above) and resolved by `zig cc` against `/usr/lib/libSystem.B.dylib`. This is the
exact analog of the ELF path linking zig's bundled musl for libm. There is **no need to make the
direct-exe path bind libSystem symbols** for llama2.

- **libm** (`_sqrtf`/`_expf`/`_cosf`/`_sinf`/`_powf`/`_fabsf`/`_floorf`): P1 mirrors the ELF
  per-symbol patch arrays (`runtime_math_*_patches`, emit_elf64.c:515-535 / 651-660) into
  emit_macho64.c, emitting one undefined external per math symbol with the BRANCH26+r_extern
  reloc. The ELF math values are emitted at emit_elf64.c:1552-1571 (XMM ABI); macho uses the
  arm64 FP ABI (arg in `s0`/`s1`, result `s0`).
- **mmap/open/close/lseek/munmap** (P2): same object-path external-call mechanism, integer args
  in x0–x7 / result x0 — strictly simpler than libm (no FP).

### libSystem provides everything (verified on this host)

macOS 11+ has **no on-disk `/usr/lib/libSystem.B.dylib`** — symbols live in the dyld shared
cache (`/System/Volumes/Preboot/Cryptexes/OS/.../dyld/`); the dylib path in the load command is a
stub the linker/dyld resolves against the cache. Verified by linking a C program that calls all
target symbols with `zig cc -target aarch64-macos` (no extra flags): it links and runs, and
`nm -u` shows `_sqrtf _expf _cosf _powf _sinf _mmap _munmap _open _close _lseek` as undefined
externals bound to `/usr/lib/libSystem.B.dylib` (via `otool -L`). `_fabsf`/`_floorf`/`_sinf` may
be optimizer-inlined at -O, but all resolve when referenced. So libSystem covers the full
llama2 set: `mmap munmap open close lseek sqrtf expf sinf cosf powf fabsf floorf`.

## Where the FP / instruction helpers should live (the seam P1/P5 extend)

`emit_macho64.c` has **no FP instruction emit today**. The FP helpers seed in P1 and grow in P5.
Put them next to the existing integer emit helpers:

- Integer movz/add/ldr/strb/cmp/bl/b helpers and `macho_emit_mov_x`/`macho_emit_movz_*` live
  ~emit_macho64.c:280–600 and ~2088–2102. Add FP siblings there:
  `ldr s_, [..]` / `str s_, [..]` / `fmov` (P1 minimum: value-in `s0`/`s1`, value-out `s0`);
  then P5 adds `fadd/fsub/fmul/fdiv s_`, `fcmp`+`fcsel`/branch, casts `fcvtzs`/`scvtf`/`ucvtf`.
- The math call-patch arrays + reloc emitters belong with the existing runtime-patch plumbing:
  context struct fields ~emit_macho64.c:323–364, record fns ~632–702, reloc emitters ~704–767,
  symbol strings ~1773–1856, symtab entries ~1934–2031. Clone the `world_write` set per math
  symbol (or one array keyed by symbol name).
- ELF reference to port from: `elf_emit_xmm_arith` (emit_elf64.c:297; plan said 292),
  XMM load/store/local helpers ~emit_elf64.c:218–226, math value emit emit_elf64.c:1552-1571
  (plan said 1543), `elf_emit_mmap_file_addr_size` (emit_elf64.c:2892; plan said 2877),
  page-alloc init / `MAP_ANON` flag `0x22` (emit_elf64.c:3153 + 3319; plan said 3138).

## Corrections to the plan's references / claims

- **Keystone prose (plan "The keystone insight" / P0 bullet 2):** the direct-exe path does NOT
  bind `_zero_world_write` via dyld bind/lazy-bind — it uses a raw `svc` syscall and ships
  `MH_NOUNDEFS` with zero bind info. The external-relocation + N_EXT-undefined mechanism the plan
  cites lives in the **object** path, which is what obj+link (and therefore llama2) uses. The
  net runtime "already proves the external-call path" via obj+link, not via the direct exe.
- **emit_macho64.c line refs are accurate:** `1u<<27` r_extern at 704/710 ✓, `_zero_world_write`
  at 1776 ✓, pageAlloc bail at 1321 ✓, indexed load/store bails at 1224/1452 ✓, byte-view-param
  gate at 1519 ✓, return-type gate diag at 1514 (plan said 1513), code signing at 177 ✓.
- **target.c refs accurate:** `target_math_runtime_supported` at 437 ✓, `target_http_runtime_supported`
  at 410 ✓. NOTE: lifting the math gate in target.c only flips the *reported JSON* status. On
  darwin today a math fixture build is blocked **earlier** by emit_macho64.c's CGEN004 (f32
  locals: "supports only primitive scalar locals", emit_macho64.c:1528 / return-type
  emit_macho64.c:1514), NOT by the target.c gate. P1 must lift BOTH the gate and the backend.
- **emit_elf64.c refs drifted slightly:** `elf_emit_xmm_arith` 297 (not 292); math value emit
  ~1552 (not 1543); `elf_emit_mmap_file_addr_size` 2892 (not 2877); page-alloc init 3153/3319
  (not 3138). Per-symbol math patch arrays: emit_elf64.c:515-535, recorder 651-660.

## P1 must-know before starting

1. **Path:** llama2 (libm) goes through obj+link, so implement libm as **object-path** external
   calls in `z_emit_macho64_from_ir` — clone the world_write/http patch+reloc+symtab plumbing per
   math symbol. Do NOT try to bind libSystem in the direct-exe path.
2. **Two gates block math on darwin** — the `target.c` JSON gate (437-443) AND the real
   emit_macho64.c CGEN004 (f32 locals 1528 / return type 1514). Lifting target.c alone is
   insufficient; the harness skip is CGEN004-driven, so a fixture only flips skip→run once the
   backend actually emits the FP. Plan's "math gate is the only target.c blocker" is true for
   *target.c* but the backend is the real wall.
3. **Minimum FP for P1:** only value-in (`s0`/`s1`) and value-out (`s0`) — no FP arithmetic yet
   (that's P5). Add `ldr/str s_` + `fmov` helpers next to the integer emit helpers.
4. **Symbol names** are `_`-prefixed Mach-O form: `_sqrtf`, `_expf`, … (vs ELF bare `sqrtf`).
5. **Acceptance:** a macho math fixture (e.g. lift `math-sqrtf-known-values.0` coverage) builds +
   runs natively, result within ULP of the C value. It will auto-run in the darwin harness branch
   once it stops bailing CGEN004.
6. **`-lm` is already wired** (main.c:9549) and harmless on macOS — no link-flag change needed.

## CI decision (plan Open Q5)

**Recommendation (adopted): keep darwin validation local-only for now; Linux/Docker stays the CI
gate.** Rationale: the conformance suite already gates correctness on Linux x64 (and the
`validate.sh` Docker/musl branch gates llama2 numerics) on every CI run; the darwin-native harness
branch added here is a *local* superset that runs automatically on an Apple Silicon dev host with
zero config and stays a no-op in CI. Per-platform parity (Open Q4) means the darwin row is a
*native-host* claim, not something a Linux CI runner can verify anyway. Add a macOS CI runner only
if/when the project wants the darwin row continuously green in CI (P6 can wire a `validate.sh`
darwin branch into a macOS GitHub Actions runner at that point). No CI config changed in P0.

## P1 — what landed

The complete **scalar f32/f64 instruction layer + the arm64 libm external-call ABI** in the
object path (`z_emit_macho64_object_from_ir`). All changes are in `emit_macho64.c` (+~610 lines)
and `target.c` (+13). The harness (`run.mjs`) and `target.c`'s *build dispatch* were untouched —
fixtures auto-flipped skip→run as the backend stopped bailing CGEN004.

**Result: darwin-arm64 native 6 ran / 126 skipped → 43 ran / 89 skipped** (`conformance ok`,
no regressions). The 37 fixtures that flipped: the 6 libm (`math-{sqrtf,expf,powf,absf-floorf,
cosf-sinf-identity}`* and `math-isnanf`), `float-primitives`, `float-{nan-compare,inf-arith}`,
all 8 `float-arith-*`, all 12 `float-compare-*`, all 6 `float-cast-*`, and the two scalar-array
smoke kernels `math-{rmsnorm,softmax}-smoke`.

### FP instruction helpers added (the seam P3/P5 extend — do NOT re-invent)

All next to the integer emit helpers (~emit_macho64.c:540–740). Encodings verified against
`clang -c -target arm64-apple-macos11` + `otool`:

- **Type predicates:** `macho_type_is_f64`, `macho_type_is_float`, `macho_type_is_unsigned`.
- **FP load/store:** `macho_emit_{ldr,str}_v_local` (`ldr/str s_/d_, [sp,#off]`, bit30=s/d,
  bit22=load/store, size-scaled offset), `macho_emit_{ldr,str}_v_field` (record-field wrappers),
  `macho_emit_{ldr,str}_v_reg_lsl` (`ldr/str s_/d_, [xbase, xidx, lsl #log2]` — the indexed-array
  form P3 reuses for Span<f32>), `macho_emit_{ldr,str}_v_sp` (fixed `[sp,#off]` spill slots).
- **FP moves/consts:** `macho_emit_fmov_v` (v↔v), `macho_emit_fmov_v_from_gpr` (`fmov s_,w_` /
  `fmov d_,x_` — raw bits, no convert), `macho_emit_mov_imm64` (movz/movk run for f64 bit
  patterns; f32 reuses the existing `macho_emit_movz_w`).
- **FP arith/compare:** `macho_emit_float_arith` (fadd/fsub/fmul/fdiv, bit22=s/d),
  `macho_emit_fcmp`, `macho_emit_cset` (CSINC alias; encodes the inverted cond),
  `macho_float_cond_for_compare` (IEEE-correct codes: EQ/NE/MI/LS/GT/GE so every NaN ordering is
  false, `!=` true).
- **FP casts:** `macho_emit_fcvt` (s↔d), `macho_emit_fcvtzs_w` (f→i32, round-to-zero),
  `macho_emit_scvtf_from_w` (i32→f), `macho_emit_ucvtf_from_x` (u64→f — **already present** for
  the P5 PRNG `randomF32`).

### FP value/instruction dispatch

- **`macho_emit_float_value_to_vreg(_depth)`** (the FP sibling of `macho_emit_value_to_reg`):
  produces an f32/f64 value into NEON reg `vreg`. Handles FLOAT const, LOCAL, FIELD_LOAD,
  INDEX_LOAD (`[N]f32`/`[N]f64`), CAST (i→f, f→f), BINARY, CALL (float-returning Zero fn), and
  the libm math kinds. Integer operands inside it (cast source, array index) call back into
  `macho_emit_value_to_reg` — the two register files (x*/v*) never overlap so interleaving is safe.
- **Integer path hooks** (in `macho_emit_value_to_reg`): `IR_VALUE_COMPARE` with float operands →
  `fcmp`+`cset`; `IR_VALUE_CAST` f→i → `fcvtzs`; `IR_VALUE_MATH_ISNANF` → `fcmp v,v; cset vs`.
- **Instruction hooks** (in `macho_emit_instr`): float LOCAL_SET / FIELD_STORE / INDEX_STORE route
  the value through the vreg emitter + an FP store; float RETURN/EXPR emit into v0.
- **ABI:** FP params spilled from v0..v7 in the prologue with a **separate counter** from the x0..x7
  integer counter (`macho_emit_function_text`); FP call args marshalled into v0..v7 with their own
  counter and the float result read from v0 (`macho_emit_call_to_reg`). Return value in s0/d0.

### libm external-call ABI (the keystone mechanism)

Single symbol-keyed table instead of 7 copies of the world_write/http plumbing:
`MachOMathSymbol` enum + `macho_math_symbol_names[]` (`_sqrtf _expf _cosf _sinf _powf _fabsf
_floorf`), one `MachOMathCallPatch{offset,symbol}` array on the ctx, `macho_record_math_call_patch`
/ `macho_append_math_call_relocations` / `macho_math_symbol_used`. In the object body, a single
loop assigns each *used* math symbol the next symtab index, appends its BRANCH26+r_extern
relocations, its undefined N_EXT symtab entry (type byte `0x01`), and its `_`-prefixed string.
Single-arg libm: arg in **s0**, result **s0**, `bl <reloc>`. powf: **s0,s1**. `nanF/infinityF/piF/eF`
are pure f32 constants (lowered to `IR_VALUE_FLOAT` in ir.c) — no libm call; `isNaNf` is inline
(`x != x` via `fcmp;cset vs`), matching ELF.

Verified: `otool -rv` shows 3× `_sqrtf` + `_zero_world_write` BR26 externals; `nm -u` lists
`_sqrtf` undefined; `otool -tv` shows `ldr s0,[sp,#…]; bl …; fmov s8,s0`. Linked via the existing
obj+link path (`-lm`, a no-op on macOS; libSystem provides libm).

### The one non-obvious correctness pitfall (P3/P5 MUST keep this)

macho addresses **locals relative to `sp`** (not a frame pointer). So an FP sub-expression cannot
spill via `str s_,[sp,#-16]!` (push) the way the ELF/x86 backend pushes rsp — moving sp corrupts
every subsequent local load in the sibling's evaluation. **Fix used:** reserve fixed FP scratch
slots at the *bottom* of the frame. `macho_emit_function_text` enlarges the frame by
`macho_fp_scratch_bytes(fun)` (= max FP binary/compare nesting depth × 16, from
`macho_value_fp_depth`/`macho_instr*_fp_depth`); the single enlarged `frame_size` keeps locals
(now shifted up) and the scratch region (`[sp, depth*16]`, via `macho_fp_scratch_slot_offset`)
consistent for the body and the matching epilogue. A binary/compare/powf spills its left/arg0 to
its depth slot across the right/arg1 evaluation — this survives nested libm calls (the slot is our
own frame memory, not a caller-saved reg). Stress-tested with depth-3 nests and
`powf(sqrtf(c), b)`. **If P5 adds more FP temporaries that must outlive a call/sub-expr, use a
depth slot, never an sp-push.**

### target.c

`target_math_runtime_supported` now also allows `zero-macho64`+`os==macos` (mirrors the http
gate). `z_append_math_runtime_json` reports `provider:libm, systemLibraries:["System"]` with a
libSystem reason for macho; linux's `["m"]` report is unchanged. NOTE (re-confirming P0): the build
**dispatch** never consulted this gate — it routes via `ir_needs_zero_runtime_object` → obj+link.
The gate only colours the `mathRuntime` plan JSON (`zero build --json --emit obj`). Verified both
darwin (supported/libSystem) and linux (unchanged) outputs.

### P2 must-know

1. **Clone the math-symbol table mechanism, not the 14 http copies.** P2's `_mmap/_open/_close/
   _lseek/_munmap` are integer external calls — add a `MachOLibcSymbol` enum + one
   `MachOLibcCallPatch` array exactly like `MachOMathCallPatch`/`macho_append_math_call_relocations`
   (~emit_macho64.c:855–945 + the object-body loops at ~2160/2210/2495). Args/return in x0..x7/x0,
   so no FP — strictly simpler than P1.
2. **No new FP work needed for mmap**; pageAlloc replaces the bail at the `IR_TYPE_ALLOC` /
   `IR_VALUE_PAGE_ALLOC` branch (~emit_macho64.c:1700s; search the diag string "does not support
   std.mem.pageAlloc"). Darwin `MAP_ANON`=**0x1000** (Linux 0x20), `MAP_PRIVATE`=0x2.
3. **Frame-scratch trick is FP-only.** Integer external calls don't need it; but if P2 ever spills
   an integer temp across a call, use the same fixed-slot idea (sp is still fixed).
4. The `-lm`/`needs_math_runtime` link wiring is already in place (main.c:9549); P2's mmap is libc,
   already linked via the runtime object — no new link flags.

\* libm fixtures use exact/banded checks (`sqrtf(4)==2`, `powf(2,10)==1024`, `expf(1)∈(2.71,2.72)`,
softmax bands) so no ULP risk; all pass natively against libSystem libm.

## P3 — what landed

Byte-view **reinterpret**, **typed-span index load/store** (all element widths), and **codec LE
reads** in the object path (`emit_macho64.c`, +~100 lines). No `target.c` change (P3 is all
backend). Harness: 2 lines in `run.mjs` to register one new fixture; the existing fixtures
auto-flipped skip→run as the backend stopped bailing CGEN004.

**Result: darwin-arm64 native 43 ran / 89 skipped → 52 ran / 81 skipped** (`conformance ok`, no
regressions). The 9 fixtures that flipped: `mem-bytes-as-f32`, `mem-bytes-as-i8`,
`codec-read-{f32,f64,i32,u32}-le`, `codec-read-{f32,i32}-le-offset`, and the new
`mem-mut-span-array-store`.

### What was added (the seam P2/P4 extend — do NOT re-invent)

Small instruction emitters next to the existing GPR/FP helpers (~emit_macho64.c:586–680):
- `macho_emit_ldrsb_w` (`ldrsb w,[x]` — sign-extending byte load for **i8** spans),
  `macho_emit_ldr_w`/`macho_emit_str_w` (`ldr/str w,[x]` #0-offset, for i32/u32 spans + codec),
  `macho_emit_str_x` (`str x,[x]`; the `ldr x` form already existed as `macho_emit_ldr_x_imm`),
  `macho_emit_lsr_w_imm` (`lsr w,w,#n` = UBFM alias — the reinterpret element-count shift),
  `macho_emit_{ldr,str}_v_base` (`ldr/str s_/d_,[x]` #0-offset — the FP sibling of the GPR
  base-only loads, used by span float index + `readF*Le`).
- Element-size helpers `macho_elem_byte_size` / `macho_elem_log2` (~1117) keyed off `IrTypeKind`.

Byte-view helper extensions (mirroring the ELF reinterpret path):
- `macho_byte_view_const_len` (~1156): added `IR_VALUE_BYTE_VIEW_REINTERPRET` (const count =
  byteLen / elemSize).
- `macho_emit_byte_view_len` (~1250): `REINTERPRET` → emit base len, then `lsr` by log2(elemSize)
  (no shift for 1-byte u8/i8).
- `macho_emit_byte_view_ptr` (~1300): `REINTERPRET` → recurse on `left` (ptr unchanged). Also
  **relaxed the `ARRAY_BYTE_VIEW` `[N]u8`-only gate** to all primitive element types (u8/i8/i32/
  u32/i64/u64/usize/f32/f64), matching ELF — needed for `MutSpan<i32> = ibuf` over a typed array.

The keystone for typed-span indexing — **`macho_emit_span_index_addr`** (~1312): given a BYTE_VIEW
local + index value + element type, materializes the index into w8, bounds-checks `idx < len`
(len@slot+8) with `brk #0` on failure (the macho analog of the array OOB trap), loads ptr@slot+0,
and computes `ptr + idx*elemSize` into **x9** (via `add x,x,x,lsl #log2`, or plain `add` for
1-byte). Returns the element address in x9. Used by all three index sites:
- **Integer `IR_VALUE_INDEX_LOAD`** (~1602, `macho_emit_value_to_reg`): when the local is
  `IR_TYPE_BYTE_VIEW`, route here → load by width (u8 `ldrb`, i8 `ldrsb`, i32/u32 `ldr w`,
  8-byte `ldr x`).
- **Float `IR_VALUE_INDEX_LOAD`** (~1679, `macho_emit_float_index_load`): BYTE_VIEW branch →
  `ldr s_/d_,[x9]`.
- **`IR_INSTR_INDEX_STORE`** (~2114): BYTE_VIEW branch → value first (vreg for float, x10 for int),
  then `span_index_addr`, then `str` by width (`strb`/`str w`/`str x`/`str s_/d_`). The value is
  materialized **before** the address so the address scratch (x8/x9) survives — same ordering as
  the existing array store.

New codec value kinds:
- **`IR_VALUE_BYTE_VIEW_READ_INT_LE`** (~1582, integer dispatch): `readI32Le`/`readU32Le` —
  evaluate the **byte** offset into w8, `add w10,w8,#4`, bounds-check `offset+4 <= len` (cond LS,
  `brk` else), `ldr w,[ptr+offset]`. AArch64 is little-endian so the 32-bit load is the value.
- **`IR_VALUE_BYTE_VIEW_READ_FLOAT_LE`** (~1891, float dispatch): `readF32Le`/`readF64Le` — same
  shape with size = 4/8 and `ldr s_/d_`.

### ELF-reference corrections

- ELF reinterpret/index points were accurate: `elf_byte_view_const_len` reinterpret @902,
  `elf_emit_byte_view_len` reinterpret @1116, `elf_emit_byte_view_ptr` reinterpret @1200 +
  `ARRAY_BYTE_VIEW` multi-type @1165–1175; `IR_VALUE_INDEX_LOAD` (handles array **and** byte-view
  locals via `elf_emit_bounds_checked_address` @833) @2224; `READ_FLOAT_LE` @2462, `READ_INT_LE`
  @2500; `IR_INSTR_INDEX_STORE` @3429. The plan's "~2415–2518" for the codec reads is ~2462–2533.
- **IR shape (verified, must-know):** a typed-span element access (`s[i]`, element ≠ u8) lowers to
  `IR_VALUE_INDEX_LOAD` / `IR_INSTR_INDEX_STORE` with `array_index` = the **BYTE_VIEW local** index
  and `element_type` carried on the local (ir.c:1633–1645 load, 3199–3228 store). u8 reads stay on
  `IR_VALUE_BYTE_VIEW_INDEX_LOAD`. A reinterpret-derived span local gets its `element_type` from the
  declared `Span<T>`/`MutSpan<T>` (ir.c:3795–3797 via `ir_byte_view_element_type`); usize→u8
  fallback, so `Span<usize>` never arises.

### Span len convention (must-know for P2/P4)

A span's stored **len slot (slot+8) is the element COUNT, not bytes** — `ARRAY_BYTE_VIEW` stores
`data_len = array_len` (count), and reinterpret divides the base count-or-bytes by elemSize. macho
stores len with a 32-bit `str w` (slot+8); ELF uses 64-bit. Fine for spans whose count fits in
32 bits (all current fixtures + llama2 weights-as-f32 counts). If P2 maps a region whose **byte**
length exceeds 4 GiB, widen the macho len slot to 64-bit (and `lsr x`), matching ELF's wide len.

### Bounds traps (must-know)

`assertBoundsTrap` in `run.mjs` builds the fail fixtures for **linux-musl-x64 only** (it does not
exercise the darwin native path), so the darwin harness runs only the *pass* fixtures. The new
runtime `brk #0` traps were nonetheless verified natively: `codec-read-{f32,i32}-le-bounds` and an
inline typed-span OOB all exit **133** (SIGTRAP) with no "unreachable" output. Use a **runtime**
index (not a const) to exercise the span check — a const index into a typed *array* is caught at
compile time.

### i64/u64 span width — parity-ready, not yet reachable

The integer span load/store handle the 8-byte width (`ldr x`/`str x`), mirroring ELF's
`elf_type_is_i64` branch. It is **not reachable today**: `[N]i64` array locals are rejected by the
local gate (both backends), `Span<usize>` collapses to u8, and there are no span params yet. It
becomes live once **P2** maps an `i64`/`u64` region or **P4** enables i64 span params — keeping it
now avoids a silent gap and matches the reference backend's coverage.

### P4 / P2 must-know

1. **P4 (span params):** `typed-span-f32`/`typed-span-i32` and `mem-mut-span-slice` already exercise
   the typed index load/store landed here — they bail only on **byte-view parameters**
   (`macho_validate_function` ~emit_macho64.c:2053). Lift that + pass `(ptr,len)` in two x-regs and
   they flip immediately; the per-element codegen is done. A span param is just a BYTE_VIEW local
   whose ptr/len come from arg regs instead of a LOCAL_SET — `macho_emit_span_index_addr` works
   unchanged on it once the local is populated in the prologue.
2. **P2 (pageAlloc):** `mem-bytes-as-{mut-f32,f64,i32,mut-i32,mut-i8}`, `mem-mut-span-u8-store`,
   `mem-bytes-as-f32-mmap` bail only on `std.mem.pageAlloc` (LOCAL_SET `IR_TYPE_ALLOC` branch,
   ~emit_macho64.c:1830). Once pageAlloc + allocBytes land, the reinterpret/index code here serves
   them with no change (they reinterpret the mapped `MutSpan<u8>` then index it).
3. **Reuse, don't re-invent:** `macho_emit_span_index_addr` is the single bounds-checked
   element-address primitive; `macho_emit_{ldr,str}_v_base` + `macho_emit_{ldr,str}_w`/`_x` +
   `macho_emit_ldrsb_w` cover every element width; `macho_elem_{byte_size,log2}` give size/shift.

## P4 — what landed

The full **aggregate/function-boundary value semantics** (sret, span params, record params, record
copy, span-in-record, span returns, record/literal call args) in the object path, ported from
`emit_elf64.c` to the arm64 AAPCS. All changes are in `emit_macho64.c` (+~330 lines); no `target.c`,
no `ir.c`, no harness change — the IR already lowers these shapes (the aggregate-abi.md / Phase-0d
pre-pass), and fixtures auto-flipped skip→run as the backend stopped bailing CGEN004.

**Result: darwin-arm64 native 52 ran / 81 skipped → 60 ran / 73 skipped** (`conformance ok`, no
regressions). The 8 fixtures that flipped: `aggregate-shape-abi` (the headline), `typed-span-f32`,
`typed-span-i32`, and — as a bonus, unblocked once span params existed — the four f32 span kernels
`math-{matmul,rmsnorm,softmax,swiglu}-span` and `generate-loop`. `fail/aggregate-record-arg-while`
still fails to build with CGEN004 "record argument here must be a record local".

### sret (record return) via x8 — the AAPCS indirect-result register

Unlike ELF's rdi-sret (which consumes an arg reg, forcing the 5-int cap), arm64 passes the
destination pointer in **x8**, which is *not* an argument register. So sret does not reduce the arg
count. Mechanism:

- **Frame slot for x8** — a record-returning function must keep the dest pointer live across the body
  (calls clobber x8), so the prologue spills it: `str x8, [sp, #sret_slot]`
  (`macho_sret_slot_offset`). It is reloaded before *every* field store and at the return, so an
  intervening call can never strand it. Verified in `otool -tv`: `makeDims` does
  `str x8,[sp]` … `str w9,[x8]` / `str w9,[x8,#0x4]` / `strb w9,[x8,#0x8]` … `ldr x0,[sp]; ret`; a
  caller does `add x8, sp, #off; bl makeDims`.
- **Three return shapes** (`IR_INSTR_RETURN` with `return_type == IR_TYPE_RECORD`): `return f()` →
  `macho_emit_record_call_with_dest(dest = -1)` passes our own saved sret pointer as the callee's x8
  (straight-through); `return p` → `macho_emit_record_copy_to(dest = UINT_MAX, src)` memcpys the
  local through the sret pointer; `return <literal>` → fields already written via sret field stores,
  then `ldr x0, [sp, #sret_slot]`. All hand the sret pointer back in **x0**.
- **sret field store** (`IR_INSTR_FIELD_STORE`, `local_index == UINT_MAX` →
  `macho_emit_sret_field_store`): materialize the value, reload x8, `str (b/w/x/v) value, [x8, #fo]`.
  Span fields store ptr@fo and the 32-bit count len@fo+8; a span-returning-call field captures
  x0/x1.

### New helpers (the seam P5/P2 reuse — do NOT re-invent)

- **Aggregate ABI** (next to the epilogue, ~emit_macho64.c:1990–2120):
  `macho_emit_lea_local_addr` (`add reg, sp, #localoff`), `macho_emit_record_copy_to`
  (slot→slot / slot→sret memcpy, 8-byte chunks + 4-byte tail), `macho_emit_copy_record_param`
  (ptr-reg→slot copy, the prologue's record-param value-copy), `macho_emit_marshal_call_arg` (the one
  arg-marshaller: record→pointer, span→(ptr,len) two regs, scalar/float→value emitter — shared by the
  regular call path and the record-call path), `macho_emit_record_call_with_dest` (marshal args, set
  x8 last, `bl`), `macho_emit_sret_field_store`.
- **Store-through-base** (next to the FP base helpers, ~emit_macho64.c:686): `macho_emit_str_{b,w,x}_disp`
  / `macho_emit_str_v_disp` / `macho_emit_ldr_w_disp` — `str/ldr (b/w/x/s/d) reg, [xbase, #scaled]`,
  used to write record fields through the sret pointer (and as the int-spill load).
- **Prologue param spilling** (`macho_emit_function_text`): byte-view param = two int regs spilled to
  slot+0 (ptr, `str x`) and slot+8 (len count, `str w`); record param = ptr in one int reg →
  `macho_emit_copy_record_param`. x8 saved first when the function returns a record.
- **Span-field byte-view** (`macho_emit_byte_view_ptr/len`): added the `IR_VALUE_FIELD_LOAD` +
  `type == IR_TYPE_BYTE_VIEW` + `is_record` case (ptr@field_offset via `ldr x`, len@field_offset+8 via
  `ldr w`) — the macho analog of ELF's `elf_record_field_disp` span-field path. A span **param** needs
  *no* new byte_view case: it is a BYTE_VIEW local once spilled, so the existing LOCAL branch +
  `macho_emit_span_index_addr` work unchanged (exactly as the P3 handoff predicted).

### The latent integer-binary x8-clobber (fixed here — P5/P2 MUST keep this)

`typed-span-i32` exposed a **pre-existing** bug (latent since P3, unreachable until span params made
it buildable): the integer binary/compare path evaluated `left → x8`, `right → x9`, `op reg, x8, x9`,
but the right operand's evaluation can clobber x8 — a span/array **index** materializes into x8
(`macho_emit_span_index_addr`), and a **call** trashes the caller-saved set. So `v[0] + v[1]` (two
span index loads) computed `op` on a destroyed left. (The FP path already avoided this via FP scratch
slots; plain `two()+three()` only *happened* to work because trivial leaves don't touch x8.)

**Fix (mirrors the FP-scratch design exactly):** integer binary/compare now spill the left operand to
a fixed frame slot across the right's evaluation —
`str x8,[sp,#int_slot]; <eval right→x9>; ldr x8,[sp,#int_slot]; op reg,x8,x9`. One 16-byte slot per
**integer** binary/compare nesting level (`macho_value_int_depth` / `macho_int_scratch_bytes` /
`macho_int_scratch_slot_offset`), parallel to the FP depth machinery. To keep slot assignment correct
under nesting, a `depth` parameter now threads through `macho_emit_value_to_reg`,
`macho_emit_byte_view_ptr`, `macho_emit_byte_view_len`, and `macho_emit_span_index_addr` (each gets a
`_depth` variant; the original names are depth-0 wrappers so the ~90 instruction-level call sites are
unchanged). A binary/compare consumes slot `depth` and evaluates its operands at `depth+1`. **If P5
adds division/modulo or any new integer binary op, route it through the same spill (it already is —
the spill wraps all of ADD/SUB/MUL and the compare).** Stress-tested: `(a[0]+b[0])+(a[1]+b[1])+...`,
`base() + idxIntoSpan(s,1)`.

### Frame layout (must-know for P2/P5)

`frame_size = macho_frame_size + fp_scratch_bytes + int_scratch_bytes + sret_bytes(16 if record
return)`. Regions, from sp upward: **FP scratch** `[0, fp_scratch_bytes)` (slot = depth*16), **int
scratch** `[fp_scratch_bytes, +int_scratch_bytes)` (slot = fp_scratch_bytes + depth*16), **sret slot**
at `fp_scratch_bytes + int_scratch_bytes` (16 bytes), **locals** at the top
(`frame_size - frame_offset`). All sp-relative and constant for the body; locals never overlap the
scratch/sret regions because the frame is enlarged by exactly those bytes. Record locals/temps and the
record-return size are already laid out by `ir.c` (`is_record` byte_size in `frame_bytes`,
`record_return_size`), so `macho_frame_size` covers them.

### Span-len width (32-bit count) — confirmed, still a P2 watch-item

Span len is stored as a **32-bit element COUNT** everywhere: byte-view param len@slot+8 (`str w`),
span field len@fo+8 (`str w`), span return len in **w1**, record copy moves the field's raw 16 bytes
(ptr + the 4-byte count in the low half) so the view round-trips. This matches P3's local convention.
**P2 reminder unchanged:** if a mapped region's *byte* length can exceed 4 GiB, widen these len stores
to 64-bit (`str x` at slot+8, span return in full x1) — but spans whose **count** fits in 32 bits
(all current fixtures + llama2 weights-as-f32 counts) are fine.

### ABI caps (kept portable — did NOT widen)

The macho backend already capped at 8 int args / 8 params (its natural reg count, wider than ELF's 6;
pre-existing, not introduced here). P4 kept that: record params/returns use the x8-doesn't-count rule
and the existing `param_count > 8` / `arg_len > 8` caps — no new widening. A record-returning **raising**
function is rejected (`fun->raises && return_type == RECORD`), mirroring ELF (the error tag would
collide with the sret/len register convention; no consumer needs it).

### ELF-reference corrections

ELF refs were accurate against the current tree: `elf_sret_slot_offset` @701, `elf_emit_record_copy_to`
@998, `elf_emit_copy_record_param` @1020, `elf_emit_sret_field_store` @3016,
`elf_emit_record_call_with_dest` @3055, the record/span/sret RETURN block @3500–3535, the prologue
record/byte-view/sret param spill @3629–3676, and the span-field byte-view ptr/len cases @1134/1153
(`elf_record_field_disp` @168). The plan's "sret in rdi + 5-int cap ~3050/3612" maps to the cap check
inside `elf_emit_record_call_with_dest` (@3065, `int_count` starts at 1) and the prologue's
`int_idx = 1` reservation (@3632) — arm64 needs neither because x8 is separate.

### P2 / P5 must-know

1. **P5 (next integer op):** `math-rope-span` and `sampler-sample` now build past span params but bail
   on **"binary operator is unsupported"** — the kernels use `/` (and the PRNG uses `%`); only
   `+`/`-`/`*` are emitted. Add SDIV/UDIV (+ MSUB for modulo) in the integer binary path and FDIV in
   the FP binary path; both already spill the left operand correctly (the depth fix wraps them).
2. **P5 (record/span everywhere):** record params/returns + span params now cross boundaries, so the
   llama2 `Config`/`TransformerWeights`/`RunState`/`Tokenizer` shapes and the kernel span signatures
   compile natively; what remains for the kernels is purely the FP/integer *ops* (div/mod), not the
   ABI.
3. **P2 (pageAlloc) unaffected:** `mem-mut-span-slice` still needs `std.mem.pageAlloc` (the LOCAL_SET
   `IR_TYPE_ALLOC` bail, ~emit_macho64.c:2179) — P4 does not unblock it. Once pageAlloc lands, its
   `MutSpan<u8>` flows through the span params/reinterpret/index code here unchanged. The new
   `macho_emit_record_copy_to` / sret machinery is also what an `owned<Mapping>`-returning or
   record-wrapping allocator API would reuse.
4. **Reuse, don't re-invent:** `macho_emit_marshal_call_arg` is the single call-arg marshaller (record
   ptr / span two-reg / scalar-float); `macho_emit_record_copy_to` + `macho_emit_copy_record_param`
   are the record memcpys; `macho_emit_sret_field_store` writes any field type through x8;
   `macho_int_scratch_slot_offset` + `macho_emit_str_x_disp`/`macho_emit_ldr_x_imm(…,31,slot)` are the
   integer spill primitives.

## P5a — what landed

Completed the **scalar integer/float instruction set** in the object path (`emit_macho64.c`, all
changes there; no `target.c`, `ir.c`, or harness edits — fixtures auto-flipped skip→run as the
backend stopped bailing CGEN004). The headline is integer `/` and `%` (the last operator gap), but
making the four blocked fixtures actually *run correctly* surfaced four latent backend bugs
(reachable only once div compiled), all fixed here.

**Result: darwin-arm64 native 60 ran / 73 skipped → 65 ran / 68 skipped** (`conformance ok`, no
regressions). The 5 fixtures that flipped: `math-rope-span`, `sampler-sample`, `sampler-topk-topp`,
`tokenizer-encode` (the four operator targets) and — as a bonus, unblocked the moment div compiled —
`checkpoint-q-v2` (the int8 quant loader, div-heavy).

### Operators added (the actual P5a scope)

- **Integer `/`** — `SDIV`/`UDIV`, 32-bit (`Wd`) and 64-bit (`Xd`); signed vs unsigned chosen off
  the result type (`macho_type_is_unsigned`), width off `macho_type_is_scalar64` — exactly ELF's
  `wide`/`unsigned` selection. New `macho_emit_div`.
- **Integer `%`** — `rem = a - (a/b)*b` via `SDIV`/`UDIV` then `MSUB` (quotient in x10; operands are
  x8/x9, result in `dst`). New `macho_emit_msub`.
- **FP `/`** — already present in `macho_emit_float_arith` (`fdiv s_/d_`, opcode `0x1e201800`) and the
  float-binary dispatch already accepted `IR_BIN_DIV`; verified, nothing to add.
- `macho_emit_binary_w` → **`macho_emit_binary_int(op, dst, lhs, rhs, wide, is_unsigned)`** now covers
  ADD/SUB/MUL/DIV/MOD at both widths (64-bit ADD `0x8b`, SUB `0xcb`, MUL `0x9b007c00`). The integer
  binary/compare path passes `wide`/`is_unsigned` from `value->type` (binary) / `value->left->type`
  (compare). The three address/length-arithmetic call sites (slice-len SUB, alloc-len ADD) stay 32-bit.
- AArch64 **div-by-zero returns 0** (and `INT_MIN/-1` → `INT_MIN`) with no trap — the architectural
  result. The language treats these as UB, so this is consistent with ELF's idiv/div (#DE fault, also
  UB) — same UB contract, different observable, no spec violation. (Encodings verified vs
  `clang -c -target arm64-apple-macos11` + `otool`.)
- **Compare width + signedness** (latent, fixed): the integer compare emitted `cmp w` and *signed*
  condition codes unconditionally. Now `cmp x` for 64-bit operands and unsigned codes (LO/LS/HI/HS)
  for unsigned types. `macho_cond_for_compare(op, is_unsigned)`. Required for the PRNG's u64 equality
  (`s2 == 1126174793148417`, > 2³²) and correct for every `usize` ordering (previously only
  *happened* to work because all loop bounds were small positive values where signed≡unsigned).

### The four latent bugs div exposed (P2/integration MUST keep these)

These were unreachable before P5a (their fixtures bailed on div at compile time); each is a real
correctness fix, not a div feature:

1. **64-bit integer literal truncation** — `IR_VALUE_INT` for a u64/i64 type used the *32-bit*
   `macho_emit_movz_x((uint32_t)int_value)`, silently truncating any constant > 2³² (the PRNG
   multiplier `2685821657736338717`, divisors `4294967296`, etc.). Now uses `macho_emit_mov_imm64`
   (full MOVZ/MOVK run). Same one-line truncation also fixed in the `Maybe<u64>` scalar-literal init
   (`IR_VALUE_MAYBE_SCALAR_LITERAL`, payload stored in the 8-byte slot). `macho_emit_movz_x` survives
   only for ≤32-bit uses (eqlBytes loop counter, the SYS_write number).
2. **Byte-slice start offset not scaled by element size** — `IR_VALUE_BYTE_SLICE` ptr added the slice
   start (an element COUNT) to the base **unscaled** (correct only for u8). For `MutSpan<i32> =
   idx[vocab..total]` (the tokenizer's combined index region, and llama2's sliced weight regions) the
   pointer landed at `base + start` instead of `base + start*4`, so `sortedId` overlapped `entryOff`.
   Now scales by `macho_elem_log2(element_type)`: const path shifts the immediate, runtime path uses
   `add x,x,x,lsl #log2` (`macho_emit_add_x_reg_lsl`, `lsl #0` for u8). Mirrors ELF's
   `elf_type_byte_size` slice-start scaling. **The slice LEN is a count and stays unscaled** — only
   the ptr scales.
3. **Call-argument scratch-slot collision (the big one)** — `macho_emit_call_to_reg` /
   `macho_emit_marshal_call_arg` dropped the `depth` parameter, so a call nested as a binary's operand
   (e.g. `total + qBlockBytes(…)`) marshalled its arguments at depth 0. An argument that is itself a
   spilling binary (`vocab * dim`) then reused frame slot 0 — clobbering the *outer* binary's spilled
   left operand (`total`). Fix: thread `depth` through `macho_emit_call_to_reg_depth` →
   `macho_emit_marshal_call_arg` → the `_depth` arg emitters, so call args evaluate at the call's
   nesting depth and their nested binaries take deeper slots. `macho_value_int_depth`/`_fp_depth`
   already size the frame for this (a CALL recurses into its args; the enclosing binary's `+1` reserves
   the extra slot). This is the **exact analog of the P4 x8-clobber fix, one level up** — the same
   discipline now also covers the frame *slot* across a nested call. This was the `checkpoint-q-v2`
   failure (`total + n_layers*qBlockBytes(...)` accumulations).
4. (Same root as #3 for FP) the float-binary/compare dispatch and FP call args now thread depth too,
   so a float arg that is a spilling float binary uses a deeper FP scratch slot. Threaded via the
   existing `macho_emit_float_value_to_vreg_depth`.

### New value kind: `IR_VALUE_BYTE_VIEW_EQ` (`std.mem.eqlBytes`)

Not an operator, but the *sole* remaining wall on `tokenizer-encode` after div (decode round-trip
checks). Ported from ELF (`elf_emit ... BYTE_VIEW_EQ` @2374): compare lengths → 0 if unequal, else a
byte loop → 0 on the first mismatch, 1 if all match. macho can't push (sp-relative locals), so it
settles both pointers (x10/x11) and the length (x12) via the integer scratch slot bridge before the
loop, evaluates sub-views at `depth+1`, and the loop body (`add`/`ldrb`/`cmp`/branches) touches only
x13–x15. Added to the `macho_value_int_depth` `spills` set so the frame reserves its slot. Verified
in isolation (equal / byte-differs / length-differs) and in `tokenizer-encode`.

### Disasm evidence

```
# sampler-sample (xor64 'x % 2', u64): udiv x then msub
udiv x10, x8, x9 ; msub x8, x10, x9, x8
# math-rope-span ('i % head_size', usize→32-bit) + FP '/'
udiv w10, w8, w9 ; msub w8, w10, w9, w8 ; fdiv s8, s8, s9
# signed i32 (-17/5=-3, -17%5=-2, verified at runtime)
sdiv w8, w8, w9 ; sdiv w10, w8, w9 ; msub w8, w10, w9, w8
```

The left-operand spill (`str x8,[sp,#slot]` … `ldr x8,[sp,#slot]`) from P4 wraps DIV/MOD unchanged,
so `a / f(b)` (left survives the call) is correct — stress-tested with nested div/mod across calls and
u64.

### P2 / integration must-know

1. **Integration is now ONLY blocked on pageAlloc.** `transformer-forward`, `generate-argmax`,
   `generate-argmax-q` build cleanly past every operator/slice/call path and bail solely on
   `std.mem.pageAlloc` (the `IR_TYPE_ALLOC` LOCAL_SET gate, the "does not support std.mem.pageAlloc"
   diag). They will flip the moment P2 lands — no further FP/integer/ABI work needed for them.
2. **The four latent fixes are load-bearing for P2 + llama2.** llama2 slices weight regions with
   runtime offsets (#2), uses u64 RNG constants (#1), and accumulates `off + n_layers*blockBytes(...)`
   with nested calls (#3) — all now correct. Do not regress them.
3. **Reuse, don't re-invent:** `macho_emit_binary_int` is the one integer-arith emitter (width +
   signedness); `macho_emit_div`/`macho_emit_msub` are the div primitives; `macho_emit_call_to_reg_depth`
   /`macho_emit_marshal_call_arg(...depth...)` are the depth-correct call path; the slice-ptr scaling
   lives in the `IR_VALUE_BYTE_SLICE` branch of `macho_emit_byte_view_ptr_depth`.
4. **No new fixtures.** Coverage for div/mod/eqlBytes/slice/64-bit-imm already exists in the four
   targets + `checkpoint-q-v2`; the suite exercises signed+unsigned, 32+64-bit, and spill-across-call.

## P2a — what landed (the headline milestone: native token-for-token generate-argmax)

The **anonymous-heap** half of P2: `std.mem.pageAlloc` + `std.mem.allocBytes` lowering to a libSystem
`_mmap` external call, plus `std.mem.copy`/`std.mem.fill`. With these the three integration fixtures
(`transformer-forward`, `generate-argmax`, `generate-argmax-q`) — which P5a left blocking *only* on
`std.mem.pageAlloc` — run natively on Apple Silicon, and **`generate-argmax` reproduces the exact
token sequence (`2 3 1 0 2 3`) token-for-token vs the libm C reference**. Changes in
`emit_macho64.c` (+~150 lines), `main.c` (the obj+link dispatch, +~45), and `run.mjs` (two stale
negative assertions flipped to positive). No `target.c`, no `ir.c` change.

**Result: darwin-arm64 native 65 ran / 68 skipped → 80 ran / 53 skipped** (`conformance ok`, no
regressions). The 15 fixtures that flipped: `page-alloc-region`, `transformer-forward`,
`transformer-forwardq`, `generate-argmax`, `generate-argmax-q` (the milestone five), the byte-view
fixtures P3 left waiting on pageAlloc (`mem-bytes-as-{mut-f32,f64,i32,mut-i32,mut-i8}`,
`mem-mut-span-{u8-store,slice}`), `std-mem-copy-fill`, `std-mem-arrays`, `std-json-allocator-capacity`,
and — unblocked the moment `std.mem.copy` compiled — the multi-module `examples/memory-package`
(its buffer module's only gap was `std.mem.copy`; it now builds + runs `memory package ok` natively,
proving cross-module calls already work on macho).

### Darwin mmap constants (differ from Linux — got these exactly right)

`_mmap(addr=NULL, len, prot, flags, fd, offset)`, AAPCS integer args x0..x5, result x0:
- `prot = PROT_READ|PROT_WRITE = 0x3` (same as Linux).
- `flags = MAP_ANON|MAP_PRIVATE = 0x1000|0x2 = **0x1002**` (Linux `MAP_ANON` is `0x20` → `0x22`; a
  wrong flag silently returns `MAP_FAILED`). Darwin `MAP_ANON=0x1000`, `MAP_PRIVATE=0x2`.
- `fd = -1` (via `MOVN x4,#0`), `offset = 0`, `addr = NULL` (kernel chooses).
- Failure check: `cmp x0, xzr; b.lt fail` — `mmap` returns `MAP_FAILED` (-1) on error; a valid
  user-space address is never negative. Kernel-zeroed pages = calloc semantics (the same contract as
  ELF's MAP_ANON syscall), which the token-for-token bar depends on.

### The libc external-call clone (the keystone, mirrored from P1's libm table)

A symbol-keyed table exactly parallel to `MachOMathSymbol`: `MachOLibcSymbol` enum
(emit_macho64.c:357, currently just `MACHO_LIBC_MMAP`) + `macho_libc_symbol_names[]` (`_mmap`,
emit_macho64.c:362) + one `MachOLibcCallPatch{offset,symbol}` array on the ctx +
`macho_record_libc_call_patch` (:1116) / `macho_libc_symbol_used` (:1128) /
`macho_append_libc_call_relocations` (:1137). In the object body, a single loop assigns each *used*
libc symbol the next symtab index (after the math symbols), appends its BRANCH26+r_extern
relocations, its undefined N_EXT symtab entry (type byte `0x01`), and its `_`-prefixed string —
the same five edit sites the math table touches. **P2b adds `_open`/`_lseek`/`_close`/`_munmap` by
extending this one enum + names array; no new plumbing.**

### The dispatch fix (the bug that cost the most — read this for P2b)

`_mmap` is an **external** call, so a pageAlloc program MUST take the **object + link** path (the
direct-exe path emits the `bl` placeholder + reloc but never binds libSystem — the unpatched `bl`
branches to itself = infinite loop). On **ELF** pageAlloc is a raw `mmap` *syscall* (no external),
so ELF stays on the direct-exe path. The decision is therefore **target-aware**:
`ir_needs_zero_runtime_object` now takes `const ZTargetInfo *target` (main.c:4276) and additionally
returns true when the object emitter is `zero-macho64` AND `ir_program_uses_page_alloc(ir)` (a new
`IR_VALUE_PAGE_ALLOC` scanner, main.c:4269). All three call sites pass `target`. Verified: linux
page-alloc still reports `objectEmission.path: direct-elf64-exe` (syscall, 1 KiB), macho page-alloc
routes through obj+link and `zig cc` resolves `_mmap` against libSystem. **P2b's file mmap is also an
external set, so it inherits this — any pageAlloc/file-mmap macho program is obj+link.**

### Allocator + Maybe lowering (most of it was ALREADY present from prior phases)

The Mach-O `IR_TYPE_ALLOC`, `IR_VALUE_ALLOC_BYTES`, `IR_TYPE_MAYBE_BYTE_VIEW`, `IR_TYPE_MAYBE_SCALAR`
locals + `.has`/`.value` were **already implemented** (the FixedBufAlloc path, args/env get, JSON
parse Maybe). P2a only added the **page-alloc** branches:
- `IR_VALUE_PAGE_ALLOC` LOCAL_SET (emit_macho64.c:2588): zero the 16-byte ALLOC slot — a PageAlloc
  carries no pre-reserved buffer (mirrors ELF's page-alloc init); each allocBytes mmaps fresh.
- `IR_VALUE_ALLOC_BYTES` with an `is_page_alloc` source (emit_macho64.c:2622): route to
  `macho_emit_anon_mmap_to_local` (:2522), which evaluates the size, **spills it to the integer
  scratch slot across the call** (mmap clobbers x0..x7), sets up the six args, records the `bl _mmap`
  patch, and populates the `Maybe<MutSpan<u8>>` (has@0, ptr@8, len@16 — same offsets as ELF; len is
  the byte count, stored 32-bit per the established span-len convention). To reserve the spill slot,
  `IR_VALUE_ALLOC_BYTES` was added to the `macho_value_int_depth` `spills` set.
- `is_page_alloc` is set on the `IrLocal` by ir.c (PageAlloc type) — no IR change needed.

### std.mem.copy / std.mem.fill (inlined — ELF has copy, neither backend had fill)

`std.mem.copy` lowers to `IR_VALUE_BYTE_COPY` (left=src, right=dst), `std.mem.fill` to
`IR_VALUE_BYTE_FILL` (left=u8 value, right=dst). ELF inlines `BYTE_COPY` (a byte loop) and **does not
implement `BYTE_FILL` at all** (`std-mem-copy-fill` does not build on linux either). P2a inlines both
on macho (emit_macho64.c:1809/1849), mirroring the `IR_VALUE_BYTE_VIEW_EQ` discipline: macho cannot
push sp-relative temporaries, so the pointers/length are settled into stable registers (copy: src x10,
dst x11, count=min(lens) x12; fill: dst x11, value x14, len x12) via the integer scratch slot before
the loop, sub-views evaluated at depth+1. Both were added to the `macho_value_int_depth` `spills` set
so the frame reserves a slot. `copy` returns `min(src.len, dst.len)`, `fill` returns dst length.

### Disasm evidence (the pageAlloc site)

```
# page-alloc-region object — _mmap arg setup (x0..x5) + self-relative bl placeholder
mov  x1, x9        ; len (0x400000)
mov  x0, #0x0      ; addr = NULL
mov  x2, #0x3      ; prot = PROT_READ|PROT_WRITE
mov  x3, #0x1002   ; flags = MAP_ANON|MAP_PRIVATE (Darwin)
mov  x4, #-0x1     ; fd = -1
mov  x5, #0x0      ; offset
bl   0x48          ; placeholder; linker patches via the BR26 reloc
cmp  x0, xzr ; b.lt <fail>
# otool -rv: 00000048 True long True BR26 False _mmap   (r_extern, ARM64_RELOC_BRANCH26)
# linked exe: bl 0x... ; symbol stub for: _mmap   (resolved against libSystem)
```

In `generate-argmax`'s object, `_mmap` + `_sqrtf`/`_expf`/`_cosf`/`_sinf`/`_powf` + `_zero_world_write`
all coexist as undefined N_EXT externals (72 BR26 relocs, 2 mmap sites = the 2 allocBytes calls).

### Acceptance evidence

```
$ make -C native/zero-c                                   # clean, no warnings
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs  # conformance ok
  darwin-arm64 native: 80 ran, 53 skipped
$ .zero/out/genargmax  → generate argmax ok               # token-for-token (good==6)
$ .zero/out/tf         → transformer forward ok
$ .zero/out/par        → page alloc region ok             # 4 MiB zeroed + copy + readback
$ .zero/out/mp_exe     → memory package ok                # cross-module + std.mem.copy
```

### Still skipped (out of P2a scope — left for later, parity with ELF preserved)

- `mem-bytes-as-f32-mmap` — **file** mmap (`std.fs.host`/`std.fs.mmapOrRaise`/`owned<Mapping>`/
  `std.fs.mappingBytes`), i.e. **P2b**, not anonymous heap.
- `allocator-primitives` — `std.mem.nullAlloc()` → a `NullAlloc` local type; **also unsupported on
  ELF/linux** (CGEN004 "NullAlloc"), so not reachable / not in scope.
- `std-mem-arena` (non-identifier callee), `std-mem-collections` (`GeneralAlloc`) — both **also fail
  on ELF** (CGEN004); arena/general-allocator features, unrelated to pageAlloc.
- `null-maybe` (a `Maybe` **field inside a record** shape), `match-payload-binding` (a choice/match
  top-level decl) — separate aggregate/match features, not pageAlloc.

### P2b + example-phase (P6) must-know

1. **P2b (file mmap) is now mostly plumbing.** Extend `MachOLibcSymbol`/`macho_libc_symbol_names`
   with `_open`/`_lseek`/`_close`/`_munmap` (the recorder/used/reloc/symtab/string loops already
   iterate the whole enum). Port `elf_emit_mmap_file_addr_size` (emit_elf64.c:2892, syscalls
   257/8/9/3) to a `bl`-sequence: `_open(path, O_RDONLY=0)`, `_lseek(fd, 0, SEEK_END=2)`,
   `_mmap(NULL, size, PROT_READ=1, MAP_PRIVATE=2, fd, 0)`, `_close(fd)`. Then wire the
   `IR_TYPE_MAYBE_BYTE_VIEW` `IR_VALUE_FS_MMAP` branch (mirror ELF emit_elf64.c:3283) and an
   `owned<Mapping>` drop → `_munmap(addr, len)`. `mappingBytes` is a span-field load. The dispatch is
   already correct (the page-alloc scanner forces obj+link; do the same for the file-mmap value kinds,
   or generalize the scanner to "uses any libc external"). `mem-bytes-as-f32-mmap` is the target.
2. **Spill-across-call discipline is mandatory.** Any value that must outlive an external `bl`
   (mmap/open/…) spills to the integer scratch slot, never an sp-push (sp-relative locals). The size
   spill in `macho_emit_anon_mmap_to_local` is the template.
3. **Cross-module calls already work on macho** (proven by `examples/memory-package` running
   natively). So **P6 (multi-file llama2)** does NOT need new cross-module codegen — the IR merges
   modules into same-object direct calls, identical to linux. P6 is `validate.sh` darwin branch +
   README, not compiler work. The remaining llama2 gap for P6 is only the **file** mmap of the real
   weights file (P2b) + the `--backend zero-macho64` self-host invocation.
4. **The dispatch is target-aware now** (`ir_needs_zero_runtime_object(ir, target)`); pass `target`
   at any new call site. Linux/ELF page-alloc stays a syscall (direct-exe), unchanged.

## P2b — what landed (the file-mmap half: native weights mmap)

The **file-mmap** half of P2: `std.fs.host`/`std.fs.mmap`/`std.fs.mmapOrRaise`/`std.fs.mappingBytes`/
`std.fs.munmap` + the `owned<Mapping>` lifecycle now lower to libSystem calls on Mach-O. With these
the real llama2 weights/tokenizer files can be mmap'd natively (the last compiler gap for P6).
Changes in `emit_macho64.c` (+~150 lines), `main.c` (the dispatch scanner, generalized from
page-alloc to "uses any libSystem mmap-family value"), `ir.c` (a one-line-per-site `is_mapping`
flag), and `include/zero.h` (the flag field). No `target.c`, no harness edit — the 7 file-mmap
fixtures auto-flipped skip→run as the backend stopped bailing CGEN004 (they were already wired by an
earlier phase, so the harness *required* them green, including the 4 GiB one).

**Result: darwin-arm64 native 80 ran / 53 skipped → 87 ran / 46 skipped** (`conformance ok`, no
regressions). The 7 fixtures that flipped: `mmap-file-readonly`, `mmap-file-notfound`,
`mmap-maybe-success`, `mmap-maybe-4gib-len`, `mmap-munmap-loop`, `mmap-munmap-early-return`,
`mem-bytes-as-f32-mmap`.

### libSystem symbols added (extends P2a's `MachOLibcSymbol` table)

Four new enum entries + names: `_open`, `_lseek`, `_close`, `_munmap` (joining P2a's `_mmap`). The
recorder/used/reloc/symtab/string loops already iterate the whole `MACHO_LIBC_SYMBOL_COUNT` enum, so
this was the only edit to the table itself. The nreloc count uses the single `ctx.libc_call_patch_len`
(every libc symbol shares one patch array), so it auto-counts the new call sites — no per-symbol
nreloc term to forget (the latent bug P2a warned about does not recur for the libc table).

### File mmap helper — `macho_emit_mmap_file_addr_size` (ports `elf_emit_mmap_file_addr_size`)

Open + size + read-only-mmap a path; leaves **addr in x0, byte length in x1** (the arm64 analog of
ELF's rax/rdx contract). Call sequence: `_open(path, O_RDONLY=0)` → `_lseek(fd, 0, SEEK_END=2)` →
`_mmap(NULL, size, PROT_READ=1, MAP_PRIVATE=2, fd, 0)` → `_close(fd)`. Darwin constants verified:
`O_RDONLY=0`, `SEEK_END=2`, `PROT_READ=1`, `MAP_PRIVATE=0x2` (same as Linux for these). On any failure
x0 is negative and never a valid mapping. **The fd is `_close`d on both the mmap-success and
MAP_FAILED paths** (and on the rare lseek-failure path) so no descriptor leaks — proven by running
`mmap-munmap-loop` under `ulimit -n 64` (1000 maps succeed; a leaked fd would EMFILE at the 65th).
macho cannot push sp-relative temporaries, so fd/size/addr that must outlive a `bl` are spilled to
**two integer scratch slots** (fd@slot+0, size@slot+8 — both 64-bit; addr@slot2+0); `IR_VALUE_FS_MMAP`
contributes **depth 2** in `macho_value_int_depth` so the frame reserves them. `otool -rv` shows the
`_open`/`_lseek`/`_mmap`/`_close`(×2)/`_munmap` BR26 r_extern relocs; `otool -tv` shows the arg setup
(`x1=0`/`x2=2` for lseek, `x2=1`/`x3=2`/`x4=fd` for mmap) and the self-relative `bl` placeholders the
linker patches.

### Value/instr wiring

- **`IR_VALUE_FS_HOST`** (value switch): `movz reg, #0` — the host Fs handle carries no state on a
  hosted target (mirrors ELF's `xor rax,rax`).
- **`IR_VALUE_FS_MMAP` Maybe branch** (`Maybe<owned<Mapping>>` LOCAL_SET, `let m = std.fs.mmap(...)`):
  helper → on `addr >= 0` set has@0=1, ptr@8, len@16 (**64-bit**); else clear the Maybe.
- **`check std.fs.mmapOrRaise`** (BYTE_VIEW LOCAL_SET, CHECK→FS_MMAP): helper → on `addr >= 0` store
  ptr@0, len@8 (**64-bit**); else **`brk #0`**, mirroring the macho `check world.out.write` discipline
  (this backend traps a failed `check` rather than threading an error-tag return — see below — and the
  fixtures never fail the raise path since the file exists).
- **`std.fs.mappingBytes(&m)`** needs **no new value case**: the IR already lowers it to an
  `IR_VALUE_LOCAL` over the Mapping (P3's typed byte-view machinery serves it). `std.codec.readF*Le`
  / `std.mem.bytesAsF32` / indexing over the mapped span all work unchanged from P3/P5.
- **`IR_VALUE_FS_MUNMAP`** (value switch, `std.fs.munmap(&mut m)`): load addr@0 / len@8 (64-bit),
  `bl _munmap`. The drop is **explicit** in the IR (Zero's `std.fs.munmap`), exactly like
  `std.fs.close` — there is no implicit scope-exit drop to synthesize, so the early-return and loop
  fixtures munmap correctly because the source calls munmap on each path (the IR carries it).

### `owned<Mapping>` 64-bit length — the `is_mapping` flag (the 4 GiB resolution)

macho stores ordinary span lengths as a **32-bit element count** (P3/P4 convention). A mapping's
length is a **64-bit byte count** (files may exceed 4 GiB — `mmap-maybe-4gib-len` maps a 4294967301-byte
sparse file). Rather than widen *every* span length to 64-bit (a broad, regression-prone change to the
established convention, and out of P2b scope), a new `IrLocal.is_mapping` flag marks exactly the locals
whose length slot must be 64-bit: an `owned<Mapping>` / `Mapping` view, the `Maybe<owned<Mapping>>`
wrapping it, **and any `Span<u8>` bound straight to `std.fs.mappingBytes(&m)`** (set in
`ir_collect_stmt_locals`, mirroring how `is_page_alloc` is set there). The macho backend uses `str x`/
`ldr x` at the length slot when `is_mapping`, else `str w`/`ldr w`. This is robust under reassignment:
a 64-bit slot round-trips a 32-bit count unchanged (the producing reg zero-extends), so a mapping-span
local later assigned an ordinary span value stays correct. ELF is untouched (it is 64-bit everywhere
and ignores the flag). **`mmap-maybe-4gib-len` passes** (`actual == 4294967301`); no assertion was
weakened, and the full-width-span-length rewrite was avoided.

### Macho fallible-function ABI + float `IR_VALUE_CHECK` (needed by `mmap-munmap-early-return`)

`mmap-munmap-early-return` exercises `check firstFloat(fs)` — a true `IR_VALUE_CHECK` over a float-
returning raising Zero function — which surfaced that the macho object backend had **no `check`/
error-tag machinery at all** (only `check world.out.write`, which the IR special-cases to a brk-on-fail
WORLD_WRITE, ever worked). Added the minimal fallible ABI, mirroring ELF's rax/rdx split but AAPCS-
appropriate:
- **Error tag rides in `x1`** (ELF uses rdx). A `raises` function's scalar/float `RETURN` now sets
  `x1 = 0` (no error) after materializing the value in x0/v0 — skipped for span returns, where x1 is
  the length (raising span returns are not part of the ABI, matching ELF's same constraint).
- **`IR_VALUE_CHECK`** in the float value path: evaluate the call into v0 (tag in x1), `cbz w1, ok`;
  on a nonzero tag run the epilogue to propagate it (the tag is already in x1), mirroring ELF's check;
  on success the value is in v0 (moved to vreg if needed). The propagation epilogue's
  `restore_process_args` is re-derived locally (the value path lacks the instr-level flag) the same
  way `macho_emit_function_text` computes `seed_process_args`. The **integer** `IR_VALUE_CHECK` is not
  implemented (no P2b fixture or llama2 path needs it — llama2's only non-world.write checks are the
  graceful `std.fs.mmap` + `mapped.has`/`mapped.value` Maybe pattern); adding it would be unexercised.

### Dispatch (the keystone — generalized from P2a)

File mmap is an external-call set (`_open`/…/`_munmap`), so a macho file-mmap program MUST take the
**object + link** path — the direct-exe path emits unbound `bl` placeholders that branch to themselves
(hang/crash). P2a routed macho **pageAlloc** programs to obj+link via
`ir_program_uses_page_alloc`; P2b **generalized** that scanner to `ir_program_uses_libsystem_mmap`,
which also detects `IR_VALUE_FS_MMAP` / `IR_VALUE_FS_MUNMAP` (the CHECK/Maybe wrappers carry FS_MMAP as
a `left`/sub-value the recursion reaches). `ir_needs_zero_runtime_object` calls it under the existing
`object_emitter == "zero-macho64"` guard, so **ELF is unaffected**: a linux file-mmap program still
reports `objectEmission.path: direct-elf64-exe`, `externalToolchain: none`, `systemLibraries: []`
(verified). The obj+link path does not gate on `self_host_caps_allowed`, so an fs-using file-mmap
program routes to obj+link **without** an explicit `--backend` — exactly how the 7 fixtures build under
the harness's plain `--emit exe --target darwin-arm64`. Confirmed by all 7 running (not hanging).

### Acceptance evidence

```
$ make -C native/zero-c                                   # clean, no warnings (-Wall -Wextra -Wpedantic)
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs  # conformance ok
  darwin-arm64 native: 87 ran, 46 skipped
$ .zero/out/mmap-file-readonly       → mmap file readonly ok
$ .zero/out/mmap-file-notfound       → mmap notfound ok
$ .zero/out/mmap-maybe-success       → mmap maybe success ok
$ .zero/out/mmap-maybe-4gib-len      → mmap maybe 4gib len ok      # 64-bit length (4294967301)
$ (ulimit -n 64; .zero/out/mmap-munmap-loop) → mmap munmap loop ok # 1000× map/unmap, no fd leak
$ .zero/out/mmap-munmap-early-return → mmap early return ok        # check over float raising fn
$ .zero/out/mem-bytes-as-f32-mmap    → mem bytes as f32 mmap ok
# ELF sanity: linux mmap-file-readonly stays direct-elf64-exe / no system libraries (unaffected)
# generate-argmax (P2a) still token-for-token native (no pageAlloc regression)
```

### P6 (example) readiness — what wiring llama2 for darwin-arm64 now requires

**File mmap was the last compiler gap.** With P2b, every primitive the llama2 example uses on
`darwin-arm64` is implemented natively: libm (P1), f32/integer kernels (P1/P3/P5/P5a), aggregate ABI
(P4), anonymous heap (P2a), and now **file mmap of the real weights/tokenizer files (P2b)**. llama2's
`main.0` uses the **graceful Maybe** form — `let mapped = std.fs.mmap(fs, model)` then
`if mapped.has { let m = mapped.value; … std.fs.munmap(&mut m) }` — which is exactly the
`mmap-maybe-success` shape now working; its only `check`s are `world.{out,err}.write` (already
special-cased). So no further `emit_macho64.c` work is expected for the example.

What P6 (the example phase) requires is **wiring only, no compiler changes**:
1. **Invocation:** `bin/zero build --backend zero-macho64 --emit exe --target darwin-arm64
   examples/llama2 --out .zero/out/llama2`. The `--backend zero-macho64` is the **self-host-cap gate**
   analog of linux's `--backend zero-elf64`: the example uses the `fs` capability, so
   `self_host_caps_allowed` is false and the default-direct-exe path won't pick it up — but the
   *object emission itself* is forced to obj+link by the mmap-family dispatch regardless, and `--backend
   zero-macho64` selects the macho object emitter. (A pure-mmap program already routes to obj+link
   without `--backend`; passing it is the explicit, documented form and matches the goal command in
   `cross-platform.md`.) Cross-module compilation already works on macho (P2a proved
   `examples/memory-package`), so the multi-file llama2 needs no new cross-module codegen.
2. **`validate.sh` darwin branch:** add a native-macOS path that builds the Zero exe as above and runs
   the parity matrix against `llama2.c` **compiled natively with the system libm** (not the Docker/musl
   reference — see the libm/musl parity rule: parity is *per platform*). `validate.sh` *can* build the
   example natively now; it just needs the branch. Keep the Docker/Linux branch as the CI gate.
3. **README:** document the native macOS build/run + add a darwin row to the validation table.

The remaining risk for P6 is purely numeric (macOS libm vs musl libm ULPs on tight-margin tokens),
not a missing capability — temp-0 argmax margins are well above ULP. **Net: the llama2 example can now
build + run natively on Apple Silicon, modulo the P6 example-wiring (the `--backend zero-macho64`
invocation + `validate.sh` darwin branch + README).**

## P6 — what landed (the example phase: native validation, no compiler change)

Wiring + validation only — **no `emit_macho64.c` / `target.c` / `ir.c` change** (P2b already
landed every compiler primitive). The example builds + runs natively and `validate.sh` proves
parity on the host without Docker.

### What changed

1. **`examples/llama2/validate.sh` — host-aware native darwin branch.** Refactored to detect the
   host (`uname -s`/`-m`) and run one of two branches, sharing the *entire* parity matrix via
   three small indirections — `build_zero` (backend/target), `build_ref` (reference toolchain),
   `run_pair` (host vs Docker execution):
   - **darwin-arm64 (native):** builds the Zero exe `--backend zero-macho64 --emit exe --target
     darwin-arm64`, builds the C reference `zig cc -target aarch64-macos -O3 -ffp-contract=off`
     (matching `libSystem` libm + the Zero backend's no-FMA scalar FP), runs **both directly on
     the host** (no `docker run`), diffs the same matrix.
   - **Linux/x64 (or `LLAMA2_FORCE_DOCKER=1`):** unchanged — `zero-elf64`/`linux-musl-x64` exe +
     `zig cc -target x86_64-linux-musl` ref, both in an amd64 Alpine container.
   - The matrix (`parity_one`/`parity_topp`/`parity_one_q`/`parity_topp_q`), flag mapping
     (`-p 0 -s 12345` temp>0, `--topp`/`-p <pp>`, v2 auto-detect), `norm_and_diff`, OK/FAIL/PASS
     reporting are all preserved. Reuses the repo-root `stories15M.bin`/`tokenizer.bin` (symlink
     into `$LLAMA2_DATA`) to avoid a redundant 60 MB download; keeps the download fallback. Native
     deps = `zig` + the Zero binary (NOT docker); Docker branch still checks docker.
2. **`examples/llama2/README.md`** — darwin-arm64 build + run commands (Quickstart, Build,
   Requirements), a host→branch validation table with a darwin row, the per-platform parity model
   (macOS `libSystem` vs linux `musl`; `-ffp-contract=off`), the int8-on-darwin informational note,
   and **closed limitation #1** ("Target: linux-musl-x64 and darwin-arm64 (native …)").
3. **`.docs/llama2/blockers/P6-int8-fp-margin-darwin.md`** — the int8 finding (below), characterized.

### Native parity result (stories15M, this Apple Silicon host)

- **f32 matrix: token-for-token PASS, 11/11** — temp 0 ×5, temp>0 multinomial ×3, top-p ×3.
  `==> PASS: llama2.zero matches llama2.c token-for-token on stories15M (f32).` The headline.
- **int8 (runq.c v2) matrix: informational on darwin, ≈4/8 match.** A few tokens flip after a long
  identical prefix. Made **non-gating** on the native branch (prints `DIFF … informational` + a
  closing NOTE; the run still exits 0). **Gated** (FAIL on divergence) on the Linux branch, where
  int8 is bit-exact and committed.

### The int8 finding — FMA contraction × tight margins (NOT a Zero bug)

Both the Zero exe and the C reference link the **same** macOS `libSystem` libm, so unlike the
resolved *Linux* case (`quant-temp-parity-divergence.md`, an Alpine-vs-zig `musl` `expf` mismatch)
this is **not** a libm difference. Root cause, isolated by experiment:

- The Zero arm64 backend emits **separate `fmul` then `fadd`** (two roundings); `zig cc
  -target aarch64-macos -O3` **contracts `a*b+c` into a single `fmadd`** (one rounding) by default.
  A sub-ULP difference per multiply-accumulate in `matmulQ`/reductions.
- Building the ref `-ffp-contract=off` matches Zero's no-FMA FP and **keeps f32 at 11/11 while
  lifting int8 from 2/8 → 4/8** — so the script uses it. But it does **not** make int8 bit-exact:
  the Zero backend, `runq.c`+FMA, and `runq.c`−FMA are **three distinct legal roundings** that
  disagree on the tightest int8 tokens. Proof: different prompts match different ref builds (`t0
  "Once"` matches −FMA, `t0 "Lily"` matches +FMA), and **two C builds of `runq.c` (`-O3` vs `-O0`,
  both −FMA) disagree with each other** on int8 temp>0 — token-for-token across independent
  implementations is undefined at these margins.
- int8 (8-bit weights) has tighter logit margins than f32, so the gap between the top-two logits
  is sometimes below the FP-order noise floor; argmax mostly survives, multinomial/top-p flip via
  the CDF boundary. f32's wider margins absorb it entirely (11/11). Zero is deterministic,
  f32-bit-exact on this host, and int8 token-for-token on the linux-musl-x64 gate — the macOS int8
  divergence is the spec-legal FMA-association difference the plan anticipated (Risk 2 / Open Q4:
  per-platform parity, "expect rare divergence on tight-margin tokens"). Full write-up + repro:
  `blockers/P6-int8-fp-margin-darwin.md`. (If int8-on-darwin token-for-token is ever required, the
  option is emitting `fmadd` in the arm64 backend — a numerics change affecting every arm64 FP
  program and the elf parity model — out of P6 scope.)

### CI story (plan Open Q5 — document-only, as P0 decided)

No CI runner added. Linux/Docker stays the continuous parity gate (it runs in CI and matches the
shipped linux-musl-x64 binary); the native darwin branch is a **local superset** auto-selected on
an Apple Silicon dev host. Per-platform parity (Open Q4) makes the darwin row a native-host claim a
Linux CI runner can't verify anyway. Noted in `validate.sh`'s header and the README.

### Acceptance evidence

```
$ make -C native/zero-c                                    # clean
$ ZERO_NATIVE_TEST_ALLOW_LOCAL=1 node conformance/run.mjs  # conformance ok
  darwin-arm64 native: 87 ran, 46 skipped                  # unchanged (no compiler change)
$ bin/zero build --backend zero-macho64 --emit exe --target darwin-arm64 examples/llama2 --out .zero/out/llama2-darwin
  → Mach-O 64-bit executable arm64 (84 KiB, ~160 ms)
$ ./.zero/out/llama2-darwin stories15M.bin --prompt "Once upon a time" --tokens 64 --temperature 0
  → "Once upon a time, there was a little girl named Lily. …"  (coherent, ~5 s)
$ bash examples/llama2/validate.sh                         # darwin host → native branch
  ==> temperature 0 (deterministic argmax) parity        : 5×  OK
  ==> temperature > 0 (multinomial) parity               : 3×  OK
  ==> temperature > 0 with top-p (nucleus) parity        : 3×  OK
  ==> PASS: llama2.zero matches llama2.c token-for-token on stories15M (f32).
  ==> [quantized] … 4× OK, 4× DIFF (informational) + NOTE  (exit 0; int8 non-gating on darwin)
```

**Net: P6 done. llama2 builds + runs natively on Apple Silicon with f32 token-for-token parity
proven on the host (no Docker). README limitation #1 closed.**
