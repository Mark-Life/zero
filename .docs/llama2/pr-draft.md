# PR draft — llama2 example + float/int8/std-library native codegen

> Internal draft. Lives in `.docs/llama2/` (planning tree) — **not** part of the shippable diff.
> Before opening the PR, drop the planning commit (B2): `git reset --soft HEAD~1` to uncommit
> `.docs/llama2/` while keeping it on disk, or `git rm -r .docs/llama2`.

---

## Title

`Add llama2 inference example with f32/f64 + int8 native codegen and std.math/mem/fs/codec`

## Summary

Adds an end-to-end **llama2 transformer inference example** (Karpathy's `llama2.c`, ported to Zero
row syntax) and the native-codegen + standard-library surface it needs, across all four
execution-verified targets and two build-only Windows targets.

The example runs real `stories15M`/`42M`/`110M` checkpoints and produces output that is
**token-for-token identical to `llama2.c`** (f32) on every execution-verified target. A quantized
(int8) path matching `runq.c` "version 2" is included and reference-tracked.

This lands four things that did not previously exist in the compiler:

1. **Floating-point codegen** — f32/f64 arithmetic, casts, NaN-correct comparisons, and IEEE
   literals on `elf64`, `macho64`, `macho_x64`, `aarch64_direct`, and (build-only) `coff`.
2. **Standard-library runtime surface** — `std.math` (libm-backed transcendentals + inline
   `isNaNf`), `std.mem` (`bytesAs*`/`bytesAsMut*` typed-span reinterprets, `pageAlloc`),
   `std.fs` (`mmap`/`munmap`/`mappingBytes`), and `std.codec` (unaligned little-endian reads).
3. **Aggregate and fallible ABI** — by-value record params/returns (sret), span-in-record,
   and `check`/`rescue`/fallible-return over scalar, float, and record carriers.
4. **int8 quantization** — `matmulQ`/`quantizeActs`/`dequantRow` and the `runq.c` v2 checkpoint
   format, auto-detected by magic.

## What's in the diff

| Area | Paths |
|---|---|
| Example | `examples/llama2/` (6 `.0` source modules, `README.md`, `validate.sh`, `zero.json`) |
| Compiler / backends | `native/zero-c/` (IR lowering, verifier, buildability, targets, all five emitters, shared x64/aarch64 encoders, `std_sig`) |
| Conformance | `conformance/` (≈190 fixtures + `run.mjs` wiring across all targets) |
| Docs | `docs/articles/modules/{math,fs,mem,codec}.md`, `standard-library.md`, `docs/lib/docs.ts`, `docs/src/tests/docs.test.mjs` |
| Metrics / infra | `scripts/compiler-metrics.mts`, `.gitignore` |

## Targets

| Target | Status |
|---|---|
| darwin-arm64 (macho64) | **execution-verified** |
| darwin-x64 (macho_x64) | **execution-verified** (Rosetta) |
| linux-x64 (elf64) | **execution-verified** (Docker amd64) |
| linux-arm64 (aarch64_direct / elf_aarch64) | **execution-verified** (Docker arm64) |
| win-x64 / win-arm64 (coff) | **build-validated only — experimental** |

## Validation

- **f32 — hard gate.** `examples/llama2/validate.sh` runs the full 11-case × 3-mode matrix
  (greedy argmax, multinomial, top-p) against `llama2.c` on real `stories15M.bin`.
  Token-for-token identical on darwin-arm64 (native), darwin-x64 (Rosetta), and linux-arm64
  (Docker). The C reference must be built with the same `zig cc` bundled musl and
  `-ffp-contract=off` as Zero, or libm diverges ~1 ULP.
- **int8 — reference-tracked, informational (never gated).** Byte-exact only where Zero and
  `runq.c` share the same musl reduction order (shared-musl branches: linux-arm64, darwin-x64
  via Rosetta are bit-exact); drifts ~1 ULP on darwin-arm64 (libSystem) from float
  reduction/instruction-order noise. The activation quantizer rounds in f32 (`floorf(v+0.5)`,
  exact for `|v| ≤ 127`) versus the reference's double. **f32 is the only must-pass parity gate.**
- `make -C native/zero-c` clean with `-Wall -Wextra -Wpedantic` (zero warnings).
- `pnpm run conformance:local`, `pnpm run compiler:metrics`, and the provenance / type-core /
  mir-verifier / row-syntax smokes all pass.

## Caveats and known limitations

**Windows (COFF / Win64) is build-validated only.** The COFF backends emit linkable objects that
pass build-only fixtures, but the generated Win64 machine code has never been executed or
parity-checked. The libm path is a `msvcrt.dll` `.idata` IAT inside the direct emitter (only
`isNaNf` is truly inline), and the `!target_is_coff(target)` runtime-object carve-out in `main.c`
is the load-bearing invariant keeping COFF math out of the ELF/Mach-O obj+link plan — there is no
IR/verifier gate behind it. Do not rely on Windows output until it is execution-verified.

**Compiler-metrics budget growth.** `aarch64_direct.c` (~+1600 lines): f32/f64 lowering,
`std.math`/libm call patches, `fs.mmap`/page-alloc. `emit_coff.c` (~+1000 lines): Win64 XMM
float-arg ABI, inline `std.math`, kernel32 mmap import-bake. The two AArch64 exe emitters grew
slightly for the unresolved-libm and branch26-to-un-emitted-callee guards.

**Intentional `llama2.c` divergences (documented in-code):** the BPE encoder does not insert the
dummy-space prefix when `' '` is absent from the vocabulary (avoids pushing a `-1` sentinel into
the token stream; no-op for any real sentencepiece vocab); `quantizeActs` preserves `runq.c`'s
unguarded all-zero-group division (unreachable for valid activations).

## Test plan

```sh
make -C native/zero-c
pnpm run compiler:metrics
pnpm run conformance:local
# end-to-end parity (downloads stories15M.bin, see examples/llama2/README.md):
LLAMA2_BRANCHES=native bash examples/llama2/validate.sh
```

## Pre-merge checklist

- [ ] Drop the planning commit (`.docs/llama2/`) — `git reset --soft HEAD~1` then re-confirm
      `git diff <base> HEAD` contains no `.docs/llama2` paths.
- [ ] Confirm base: PR targets the upstream commit the branch was cut from (`b134668`), not a
      stale local `main`.
- [ ] Paste the **Targets / Validation / Caveats** sections above into the PR description.
