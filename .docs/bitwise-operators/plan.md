# Bitwise Operators — Implementation Plan (v0.1)

Add `&` `|` `^` `<<` `>>` (binary) and optional unary `~` to Zero.

## Motivation

The language has **no bitwise operators**. The parser's entire binary-operator set is
`|| && ==/!=/</<=/>/>= + - +% +| * / %` (`parser.c:337-345`); `IrBinaryOp` is
`ADD/SUB/MUL/DIV/MOD/AND/OR` where **AND/OR are the logical `&&`/`||`**, and `&` is the
borrow operator. This forced llama2 Phase 6 to emulate the xorshift PRNG (XOR via a 64-step
bit loop, shifts via multiply/divide by powers of two — `examples/llama2/src/sampler.0`).
Anything bit-level (hashing, masks, flags, serialization, PRNGs, packing) is impossible or
painful. This adds the operators as ordinary integer binops.

## Goal

`let h = (x ^ (x >> 16)) * k & 0xFFFF` parses, type-checks, and emits correct 64-/32-bit
code on `linux-musl-x64` (ELF64) — **without breaking generics** (`Maybe<owned<Mapping>>`)
or the borrow operator (`&x`, `&mut m`).

## Operators & semantics

| op | name | x86-64 emit (prepend `0x48` when 64-bit) |
|----|------|------------------------------------------|
| `&` | bitwise AND | `21 C8` (`and rax, rcx`) |
| `\|` | bitwise OR | `09 C8` (`or rax, rcx`) |
| `^` | XOR | `31 C8` (`xor rax, rcx`) |
| `<<` | shift left | `D3 E0` (`shl rax, cl`) |
| `>>` | shift right | unsigned `D3 E8` (`shr`), signed `D3 F8` (`sar`) |
| `~` (opt) | bitwise NOT (unary) | `F7 D0` (`not rax`) |

- **Integer operands only.** Reject float/bool/char (diag 3006). Both sides same type
  (literals infer); result = left type. Mirrors the existing arithmetic rule.
- **`>>` signedness:** logical (`shr`) for unsigned types, arithmetic (`sar`) for signed —
  exactly the signed/unsigned split DIV/MOD already use (`emit_elf64.c:1270-1288`).
- **Width:** 64-bit for `i64`/`u64` (`elf_type_is_i64` ⇒ REX.W). `i32`/`u32`/`usize` use
  32-bit ops — note `usize` is in the 32-bit `elf_type_is_scalar` group (pre-existing quirk;
  use `u64` for full-width bit work). Shift count is `cl` (x86 masks it to the operand width).

## Precedence (Rust-like; higher = binds tighter) — edit `parser.c:precedence()`

```
1 ||      2 &&      3 == != < <= > >=      4 |      5 ^      6 &      7 << >>      8 + - +% +|      9 * / %
```

Insert `| ^ & << >>` **between** comparison and additive; existing relative order preserved
(additive 4→8, multiplicative 5→9). Bitwise binds **tighter than comparison** (Rust, not C) so
`a & b == c` parses as `(a & b) == c` — avoids C's classic footgun. Shifts looser than `+`
(`a + b << c` ⇒ `(a+b) << c`), matching Rust.

## The two real conflicts (both solved without touching generics)

### A. `>>` / `<<` vs the generic close in `Maybe<owned<Mapping>>`

`parse_type` (`parser.c:190-205`), `looks_like_type_args_before_call` (`258-275`),
`parse_generic_arg_text`/`parse_type_args` (`277-316`) all consume `<`/`>` **one token at a
time** via depth counters. So **do NOT add `<<`/`>>` to `two_char_symbol`** (`lexer.c:33`) —
that would break every one of them.

Instead: keep single `<`/`>` tokens, and in `parse_binary` (`parser.c:523-538`) treat the
operator as `<<`/`>>` when the current token is `<`/`>`, the **next** token is the same char,
and they are **adjacent** (same line, `column+1` / `offset+1` — no internal whitespace).
Consume both tokens. Type/generic parsing is left completely untouched (zero regression risk).
Adjacency = `>>` is a shift but `> >` is two comparisons — standard, matches programmer intent.

Verified safe against the generic-call heuristic: `looks_like_type_args_before_call` returns
false for `<<` (no matching `>(`) and is never reached for `>>`.

### B. `&` infix (bitwise) vs `&` prefix (borrow)

Borrow is prefix-only (`parse_unary:512-519`); infix `&` is only ever seen by the operator
step in `parse_binary`. The split is positional and unambiguous (exactly like C `&x` vs
`a & b`). No lexer change (`&` already lexes single-char, `&&` wins via `two_char_symbol`
first). Just add `&` to `precedence()` and map it to a **new** IR op (below).

## Touchpoints

| File | Change |
|------|--------|
| `lexer.c:207` | add `^` `\|` `~` to the single-char `strchr(...)` set. **Do not** add `<<`/`>>` |
| `parser.c:337` `precedence()` | renumber per table; add `\|` `^` `&` `<<` `>>` |
| `parser.c:523` `parse_binary()` | combine adjacent `<`/`>` into a shift op (peek next + adjacency; advance index by 2; synthesize op text `"<<"`/`">>"`) |
| `parser.c:499` `parse_unary()` | *(optional `~`)* prefix → `EXPR_UNARY`/new bitnot expr |
| `checker.c:5172` `EXPR_BINARY` | add a `bitwise` category: require `is_int_type` both sides (reject float/bool/char → 3006), same type, `set_expr_resolved_type(left_type)` |
| `checker.c:3047` `EXPR_BINARY` meta | *(optional)* const-fold `& \| ^ << >>` so `const MASK = 1 << 8` works at comptime |
| `zero.h:491` `IrBinaryOp` | add `IR_BIN_BITAND, IR_BIN_BITOR, IR_BIN_XOR, IR_BIN_SHL, IR_BIN_SHR` — **distinct** from AND/OR (macho short-circuits AND/OR) |
| `ir.c:1210` `ir_binary_op()` | map `&`→BITAND, `\|`→BITOR, `^`→XOR, `<<`→SHL, `>>`→SHR |
| `emit_elf64.c:1245` `IR_VALUE_BINARY` | add the 5 cases (encodings above); `>>` picks `shr`/`sar` via `elf_type_is_unsigned(value->type)` like DIV |
| `emit_macho64.c:~1037`, `emit_coff.c:~482` | add the 5 ops (AArch64 `AND/ORR/EOR/LSL/LSR/ASR` reg-reg; COFF x64 = same bytes as ELF) — **or** keep erroring (scope decision) |
| `emit_elf_aarch64.c` | none (MVP, no `IR_VALUE_BINARY` support at all) |
| `conformance/native/pass/*` + `run.mjs` | new fixtures (below), wired into check + runtime lists |
| `examples/llama2/src/sampler.0` + `conformance/native/pass/sampler-sample.0` | *(stretch)* replace `xor64` bit-loop + mul/div shifts with `^`/`<<`/`>>` |

## ELF64 emit detail (priority backend)

The integer `IR_VALUE_BINARY` path already lands left in `rax`, right in `rcx`
(`emit_elf64.c:1246-1252`: eval left→rax, `push`; eval right→rax; `mov rcx,rax`; `pop rax`).
So each new op is a single instruction on `rax`/`rcx`:
- `XOR` `[48] 31 C8` · `BITAND` `[48] 21 C8` · `BITOR` `[48] 09 C8`
- `SHL` `[48] D3 E0` · `SHR` (unsigned) `[48] D3 E8` · `SAR` (signed) `[48] D3 F8`
  — count is `cl` (low byte of `rcx`, already the right operand). `wide = elf_type_is_i64`.

## Fixtures

- `bitwise-ops.0` (pass, no libm ⇒ runs in CI, `generatedCBytes==0`): u32/u64/i32 cases with
  known results; unsigned `>>` (shr) vs signed `>>` (sar) on a negative i32; `1 << 31` (u32);
  `0xFF00 >> 8`; `a ^ b`, `a & b`, `a | b`; precedence check `a & b == c` ⇒ `(a&b)==c`.
- `bitwise-float-operand.0` (fail): `1.0 ^ 2.0` ⇒ diag 3006.
- `shift-generics-coexist.0` (pass, regression guard): a function whose signature returns a
  nested generic closing in `>>` (e.g. `Maybe<owned<Mapping>>`) AND whose body uses `x >> 2`
  and `a << b` — locks conflict A. Also exercise `a & b` next to `f(&x)` for conflict B.
- Wire into `conformance/run.mjs` (check list + runtime list); add a byte-level assertion in
  `scripts/test-native.sh` if pinning encodings.

## Risks

1. **`>>` adjacency vs generics** — mitigated by never lexing `>>` (parser-combine only) + the
   coexist guard fixture. Highest-attention item.
2. **`&` infix vs borrow** — positional split is robust; cover with a mixed fixture.
3. **macho/coff scope** — if not updated, the 5 ops error cleanly there (ELF64 is the only
   runnable v0.1 target). Recommend at least macho (the darwin-arm64 dev host) for parity.
4. **usize 32-bit arithmetic** (pre-existing) — bitwise inherits it; document, don't fix here.
5. **const-fold parity** — without the meta-eval update, `const X = 1 << 4` fails at comptime.

## Exit criteria

- `& | ^ << >>` parse, type-check (integer-only), and emit correct ELF64 (64- and 32-bit;
  signed vs unsigned `>>`).
- Generics and borrow remain unbroken (existing + new guard fixtures green).
- `conformance` / `native:test` / `docs:test` green with the new fixtures.
- *(stretch)* `sampler.0`'s xorshift rewritten with real operators (delete `xor64`).

## Open questions

1. **Unary `~`?** Cheap (`not rax`) but needs a new IR unary kind / lowering. Recommend: yes.
2. **Which backends now?** ELF64 is required. Add macho (dev host) too? coff? Recommend
   ELF64 + macho now, coff later.
3. **Shift-count type:** require same type as LHS (v1, literals infer) or allow any integer
   count (needs a relaxed checker rule + a possible `movzx` to cl)? Recommend same-type v1.
4. **Precedence:** confirm Rust-like (bitwise tighter than comparison). Alternative is C
   (looser — the `a & b == c` footgun). Recommend Rust-like.
5. **Shift ≥ width:** document x86 mask behavior as implementation-defined (no trap, matching
   the no-overflow-check arithmetic), or check/trap? Recommend document, no trap.
6. **Reuse `IR_BIN_AND`/`OR` for `&`/`|`?** No — macho lowers those as short-circuit logical
   branches; bitwise needs distinct ops. (Confirmed in the backend survey.)
```
