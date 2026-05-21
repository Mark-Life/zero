# Direct ELF64 Aggregate ABI (Phase 0d)

Backend extension landed for llama2 Phase 2 (`feat/std-math-llama2`, 2026-05-21),
then generalized to full record value semantics (2026-05-21) so it stands on its own
as a language feature for upstream. Lets shapes and spans cross function boundaries in
the direct ELF64 backend (`--backend zero-elf64`) — the only backend that builds an
fs-using program to a runnable `linux-musl-x64` exe. Mirrors the Phase-0 precedent of
extending the backend when llama2 needs it (cf.
[`./fallible-abi-widen.md`](./fallible-abi-widen.md)).

## Why

Phase 2's natural API — `readConfig(bytes) -> Config`, `mapWeights(bytes, cfg) ->
TransformerWeights`, holding a `TransformerWeights` — could not build. The direct
backend rejected **shape params, shape returns, span returns, and
shape-with-`Span`-field locals**: aggregates were function-local only. (`zero check`
accepted the clean code; the rejection was at IR-lowering / emit, and fired even
for unused functions since all functions are lowered.) Forward-pass phases (3–7)
need the same boundaries (`RunState`, `forward(...)`, kernels over weight views),
so this is foundational, not Phase-2-specific.

## ABI (internal calls only)

`export c` / hosted `main` are unchanged. Conventions for Zero→Zero calls:

| Form | Convention |
| --- | --- |
| scalar / i64 / float param | unchanged (SysV int regs `rdi,rsi,rdx,rcx,r8,r9`; xmm0–7) |
| span param | unchanged — two int regs `(ptr, len)` |
| **record param** | pointer in one int reg; callee copies the pointee into an inline frame slot on entry (value semantics; field access reuses the rbp-relative path) |
| **record return** | sret — caller passes the destination address in `rdi`, params shift to `rsi…`; the address also rides back in `rax`. `return <shape literal>` writes fields through the saved sret pointer; `return p` (a record local/param) memcpys the source slot through it; `return f()` passes our own sret pointer to `f` so the callee writes straight through to the caller's storage |
| **record bind** | `let x = recordCall(...)` binds via sret directly into `x`'s slot (no copy); `let x = p` / `x = p` memcpys one record slot to another (value copy, span fields ride along as raw 16-byte ptr+len) |
| **record argument** | a record value is passed by pointer. A plain record local passes its slot address directly; a record-returning **call** or a **shape literal** used as an argument is materialized into a hidden temp slot first, then passed by pointer (value semantics) |
| **field of a call/literal** | `f(...).field` / `(Shape{...}).field` materializes the record into a temp, then loads the field from that slot |
| **span return** | `(ptr, len)` in `rax:rdx` (mirrors the span-param two-GPR shape) |
| **`Span` field in a record** | 16 bytes / 8-aligned; `ptr@offset`, `len@offset+8`; construction accepts span locals, slices, reinterprets, and span-returning calls |

Record temporaries are allocated in a pre-pass (`ir_prepare_record_temps_*`) before
frame layout, so the locals array never reallocs during lowering (held `IrLocal*`
stay valid) and each temp gets a properly-sized slot.

Remaining restrictions (deliberate; grow later):

- **A record call/literal as an argument is only materialized in straight-line
  statement positions.** It is *not* hoisted into a loop condition (would build once,
  not per iteration) or the short-circuit operand of `&&`/`||` (would evaluate
  unconditionally). Those report `CGEN004` rather than miscompiling — hoist a `let`
  yourself. Plain record locals as arguments work everywhere.
- **Raising functions cannot return aggregates.** The `rdx` error tag would collide
  with span `len`; a fallible record return (sret + tag in `rdx`) is tractable but
  has no consumer yet (checkpoint functions are non-raising). Out of scope for this
  change.
- **Records nest only one level** — a record field may be a scalar, fixed array, or
  `Span`, not another record. (Layout limitation, unchanged.)

## Touchpoints

- `include/zero.h` — `IrFunction.record_return_size` / `record_return_shape`;
  `IrRecordTemp` + the `IrProgram` record-temp table and lowering sink.
- `ir.c` — `IR_TYPE_BYTE_VIEW` field size/align (16/8); accept byte-view fields in
  the 3 shape-layout functions; relax the function-/call-level param & return
  checks for record/byte-view; `ir_lower_byte_view` handles record byte-view fields
  + span-returning calls; `ir_lower_shape_initializer` retargetable to an sret
  sentinel (`UINT_MAX`). Record value semantics: `STMT_LET`/`STMT_ASSIGN` bind a
  record call (sret) or a record local (copy); `STMT_RETURN` lowers a record
  literal (sret stores), local/param (copy), or call (sret passthrough).
  Record temporaries: `ir_prepare_record_temps_*` reserves temp slots before frame
  layout; `ir_lower_record_temp` materializes a call/literal in front of its use via
  the statement sink (`ir_lower_stmt_to_vec` save/restore wrapper); the call-arg loop
  and `EXPR_MEMBER` substitute a local reference.
- `emit_elf64.c` — record-param copy-in + sret-pointer save in the prologue
  (params shift by one int reg when returning a record); record-arg address passing;
  record-returning call to a local *or* the function's own sret
  (`elf_emit_record_call_with_dest`); record-to-record / record-to-sret memcpy
  (`elf_emit_record_copy_to`, `elf_emit_lea_local_addr_reg`); sret field stores
  (`elf_emit_sret_field_store`, through `rcx` reloaded after each value);
  span/record return emission; span-field load/store in records; frame reserves an
  sret slot (`elf_sret_slot_offset`).

## Test

`conformance/native/pass/aggregate-shape-abi.0` — record params; record returns of a
literal, a local/param, and a call; record-to-record copy; record-returning calls and
shape literals as arguments (one, several, nested); field access on a call result;
span returns and `Span` fields. Builds via the **default** `--emit exe` path (no fs)
so it **runs in CI** (`generatedCBytes == 0`), unlike the fs-gated llama2 exe.
`conformance/native/fail/aggregate-record-arg-while.0` — a record call argument in a
loop condition reports `CGEN004` (the deliberate hoisting boundary). End-to-end
exercised by `examples/llama2` and by `transformer-forward.0` (full forward pass). No
conformance / `native:test` / `docs:test` regressions.
