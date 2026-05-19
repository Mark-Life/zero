# Typed `Span<T>` Parameters — Implementation Plan

Executable plan for F3 from [`../math/plan.md`](../math/plan.md). Lifts the `Span<u8>`-only restriction on the direct ELF64 backend so functions can take `Span<f32>`, `MutSpan<f32>`, `Span<i32>`, etc. — unblocking `examples/llama2/` and any non-byte numerical kernel.

## Goal

Ship `fun rmsnorm(x: Span<f32>, out: MutSpan<f32>) -> Void { ... }` compiling and running correctly on `linux-musl-x64`, with element types covering the same set fixed `[N]T` arrays already support: `u8`, `i32`, `u32`, `i64`, `u64`, `f32`, `f64`.

## State Today (Verified)

- Checker accepts `Span<T>` / `MutSpan<T>` / `Span<const T>` for arbitrary `T` (`checker.c:1760-1768`). Type-equality and mutability rules already work for typed spans — `conformance/native/pass/mutable-spans.0` and `generic-spans.0` parse and pass `Span<i32>` / `MutSpan<i32>` through the checker.
- IR lowering bottles out: `ir.c:161-167` only maps the `Span<u8>` family to `IR_TYPE_BYTE_VIEW`; other element types return `IR_TYPE_UNSUPPORTED`.
- Even for the `u8` case, the ELF64 backend rejects byte-view parameters outright: `emit_elf64.c:2273-2276` errors any `is_param && type == IR_TYPE_BYTE_VIEW` local with "direct ELF64 object backend does not yet support byte-view parameters". Today `mutable-spans.0` and `generic-spans.0` are CGEN004-skipped through `assertDirectRuntimeOrUnsupported`'s tolerant path.
- Fixed-array Phase 6 work (`ir.c:518`, `emit_elf64.c:2023-2057`, `emit_elf64.c:2824-2841`) is the template: element-type-keyed `IR_VALUE_INDEX_LOAD` / `IR_INSTR_INDEX_STORE` already emit MOVSS/MOVSD for `f32`/`f64` on `[N]T` locals.

Net effect: `fun foo(values: Span<f32>) -> Void { ... }` fails at `zero build` with CGEN004; the conformance harness tolerantly accepts that failure today.

## Scope

**In:**

- Element types: `u8`, `i32`, `u32`, `i64`, `u64`, `f32`, `f64` (same set as fixed `[N]T`).
- `Span<T>`, `MutSpan<T>`, `Span<const T>` as function parameters.
- Indexed read `span[i]` and indexed write `mutSpan[i] = x` with bounds-check against the span's `len` slot.
- `.len()` (and `std.mem.len(span)`) returning element count, not byte count.
- `[N]T → Span<T>` coercion for the same element-type set.
- Conformance fixtures for every element type + math kernels (RMSNorm, softmax) as functions over `Span<f32>`.
- ELF x86-64 only (`linux-musl-x64`). AArch64 / Mach-O / COFF replicate later (same shape as F1 NaN-compare across backends).

**Out (later):**

- Slicing typed spans (`span[a..b]` where stride > 1). Today `IR_VALUE_BYTE_SLICE` ptr math is byte-granular; typed-span slicing needs stride-scaled start. Track as F3.b — fix when first user hits it.
- `Span<RecordType>`, nested spans. Same reasoning as the fixed-array allow-list.
- `Maybe<Span<T>>` for non-u8 T. `IR_TYPE_MAYBE_BYTE_VIEW` would need the same treatment; defer until a user surfaces.
- `std.mem.copy` / `eqlBytes` / other intrinsics over typed spans. They take `Span<u8>` today; element-type-generic versions are a separate stdlib pass.
- Other backends — same migration cost as F1; covered when each backend's float codegen reaches parity.

## Design

**Carry `element_type` on `BYTE_VIEW` locals.** `IrLocal.element_type` already exists for arrays. Extend its meaning so a `BYTE_VIEW` local also carries the span's `T`. Default to `IR_TYPE_U8` for non-span byte views (`String`, `ByteBuf`, `BufferedReader`, etc.) — those already index/length in bytes, so `U8` is correct.

**Why not add a new `IR_TYPE_TYPED_SPAN`:** ripples through every existing `IR_TYPE_BYTE_VIEW` check (`ir.c`, `emit_elf64.c`, plus the other backends). The element-type-on-existing-kind shape minimizes churn — every existing `BYTE_VIEW` consumer behaves identically when `element_type == U8`, and only the new typed paths inspect `element_type` for stride/opcode dispatch.

**ABI.** Two GPR slots, same as today: ptr in the first int reg, len in the next. For an `f32` span of length 4, `len = 4` (element count). Stride is `ir_type_byte_size(element_type)`.

**Bounds-check.** Element-keyed, against slot 8's len. Generalize `elf_emit_bounds_checked_address` to take a source — `local->array_len` for fixed arrays or `local`+slot-8 read for byte views.

## Phases

Each phase is independently shippable. Tree stays green throughout.

### Phase 1 — element_type plumbing for `BYTE_VIEW`

Touchpoints:

- `ir.c:140-181` — `ir_type_kind` already returns `IR_TYPE_BYTE_VIEW` for `Span<u8>` etc. Add a helper `ir_parse_span_element_type(const char *type, IrTypeKind *out)` that recognizes `Span<T>` / `MutSpan<T>` / `Span<const T>` for the supported element-type set and returns `T`. Non-span byte views (`String`, `ByteBuf`, `BufferedReader`, ...) keep `element_type = IR_TYPE_U8`.
- `ir.c:3175-3186` (`ir_collect_function_locals`) — for each `BYTE_VIEW` param, compute element_type and pass through `ir_function_push_local`.
- `ir.c:3202-3243` (`ir_collect_stmt_locals`) — same for `let span: Span<T> = ...` locals.
- `include/zero.h:511-525` — `IrLocal.element_type` already exists; no shape change.
- `ir.c:1004-1067` (`ir_lower_byte_view`) — when constructing a `BYTE_VIEW` from another value (string literal, slice, identifier), preserve element_type.

Exit: `zero check` / `zero graph --json` on a program with `Span<f32>` params shows the local typed `byte_view` with `element_type: f32`. No backend changes yet.

### Phase 2 — Lift ELF64 byte-view-parameter gate

Touchpoints:

- `emit_elf64.c:2273-2276` — delete the rejection. Replace with a check: allow `BYTE_VIEW` params iff `element_type` is in the supported allow-list (mirror `elf_type_is_supported_scalar` plus `U8`).
- `emit_elf64.c:2956-2980` (function prologue) — span params (BYTE_VIEW) consume **two** int regs. Update the prologue loop to: detect `IR_TYPE_BYTE_VIEW` params, pull the next two int regs into slot 0 (ptr) and slot 8 (len). Bump `int_idx += 2`. Note: the current loop iterates `param_count` which is local count; each iteration already targets one local. So a span param's iteration consumes two int regs, not one.
- `emit_elf64.c` `elf_validate_function` — extend the allowed-locals list to keep `BYTE_VIEW` locals (already allowed for non-params); preserve the existing rejection for `BYTE_VIEW` whose `element_type` is unsupported.

Exit: a Zero program with `fun read(span: Span<u8>) -> u8 { return span[0] }` compiles, links, and runs. Existing `Span<u8>` index-load path is preserved.

### Phase 3 — Generalize INDEX_LOAD / INDEX_STORE for `BYTE_VIEW`

Touchpoints:

- `emit_elf64.c:2023-2045` (`IR_VALUE_INDEX_LOAD`) — accept either `local->is_array` or `local->type == IR_TYPE_BYTE_VIEW`. For BYTE_VIEW: load len from slot 8 (instead of constant `array_len`), bounds-check, load base from slot 0 (instead of lea), scale index by `ir_type_byte_size(element_type)`, then emit the existing element-type-keyed load opcode set (movzx for u8, mov for i32/u32, MOVSS for f32, MOVSD for f64, REX.W mov for i64/u64).
- `emit_elf64.c:2824-2841` (`IR_INSTR_INDEX_STORE`) — same generalization on the store side.
- `emit_elf64.c:812-826` (`elf_emit_bounds_checked_address`) — factor into a helper that takes the "len source" (constant `array_len` or runtime-loaded from a byte-view slot) and the "base source" (lea for array, slot 0 load for byte view). Or duplicate as `elf_emit_bounds_checked_address_byte_view` — pick whichever is shorter.

Exit: `IR_VALUE_INDEX_LOAD` / `IR_INSTR_INDEX_STORE` work uniformly on arrays and typed byte views, dispatching on `element_type`.

### Phase 4 — IR lowering for typed-span indexing

Touchpoints:

- `ir.c:1322-1342` (`span[i]` lowering) — when the base is a `BYTE_VIEW` with `element_type != IR_TYPE_U8`, emit `IR_VALUE_INDEX_LOAD` (carrying `element_type`) targeting the span local. For `element_type == IR_TYPE_U8` we can keep emitting `IR_VALUE_BYTE_VIEW_INDEX_LOAD` (existing path) — or route everything through the generalized form for uniformity. Pick the route that keeps generic-spans.0 passing without code churn.
- `ir.c:2722-2790` (`IR_INSTR_INDEX_STORE` lowering for mutable arrays) — extend the rule to also accept mutable typed byte-view locals (`MutSpan<T>`). Source is the same `IR_INSTR_INDEX_STORE` instruction; the backend already handles BYTE_VIEW after Phase 3.

Exit: `let mut buf: MutSpan<f32> = ...; buf[i] = x; let y: f32 = buf[i]` lowers to IR ops that the ELF64 backend handles.

### Phase 5 — `[N]T → Span<T>` coercion for non-u8 T

Touchpoints:

- `ir.c:1011-1025` (`ir_lower_byte_view` for array sources) — currently rejects non-`u8` array element types. Allow any supported element type; preserve element_type on the resulting `IR_VALUE_ARRAY_BYTE_VIEW`.
- `emit_elf64.c:1011-1015` (`elf_emit_byte_view_ptr` for `IR_VALUE_ARRAY_BYTE_VIEW`) — drop the `element_type != IR_TYPE_U8` rejection. `lea` base unchanged.
- `emit_elf64.c:947-995` (`elf_emit_byte_view_len`) — for `IR_VALUE_ARRAY_BYTE_VIEW` of length N elements, return N (already does — `view->data_len` carries length in elements for arrays).

Exit: `let arr: [4]f32 = [1.0, 2.0, 3.0, 4.0]; let span: Span<f32> = arr; foo(span)` works.

### Phase 6 — Conformance fixtures

Touchpoints under `conformance/native/pass/`:

- New `typed-span-f32.0` — `fun sum(values: Span<f32>) -> f32` summing four floats, plus `fun fill(out: MutSpan<f32>, value: f32) -> Void`.
- New `typed-span-i32.0` — same shape for `i32`.
- New `math-rmsnorm-span.0` — RMSNorm extracted as `fun rmsnorm(x: Span<f32>, w: Span<f32>, out: MutSpan<f32>, eps: f32) -> Void`. Same tolerance bands as `math-rmsnorm-smoke.0`.
- New `math-softmax-span.0` — softmax as `fun softmax(x: Span<f32>, out: MutSpan<f32>) -> Void`. Same tolerance bands as `math-softmax-smoke.0`.

Verify side-effect: `mutable-spans.0` and `generic-spans.0` now actually run on Linux x86-64 (rather than CGEN004-skipping) and emit `"mutable spans ok\n"` / `"generic spans ok\n"`. Update fixture metadata in `conformance/run.mjs` if any `libm` flag or expected-skip annotation is wrong now.

Exit: `pnpm run conformance` green with new fixtures; `mutable-spans` / `generic-spans` reach the runtime assertion path on Linux instead of returning early at the BLD003/CGEN004 branch.

## File touchpoints summary

| File | Phase | Change |
|------|-------|--------|
| `ir.c` `ir_type_kind` / `ir_parse_span_element_type` | 1 | extract `T` for `Span<T>` family |
| `ir.c:3175-3186` | 1 | thread element_type through `ir_collect_function_locals` |
| `ir.c:3202-3243` | 1 | same for `ir_collect_stmt_locals` |
| `ir.c` `ir_lower_byte_view` | 1, 5 | preserve element_type; accept non-u8 array sources |
| `emit_elf64.c:2273-2276` | 2 | drop byte-view-param rejection (with element-type allow-list) |
| `emit_elf64.c:2956-2980` | 2 | prologue loop consumes two int regs for BYTE_VIEW params |
| `emit_elf64.c:2023-2045` | 3 | `IR_VALUE_INDEX_LOAD` accepts byte-view locals, runtime len source |
| `emit_elf64.c:2824-2841` | 3 | `IR_INSTR_INDEX_STORE` same generalization |
| `emit_elf64.c:812-826` | 3 | factor bounds-check to accept runtime len |
| `ir.c:1322-1342` | 4 | `span[i]` for typed T emits `IR_VALUE_INDEX_LOAD` |
| `ir.c:2722-2790` | 4 | `mutSpan[i] = x` for typed T emits `IR_INSTR_INDEX_STORE` |
| `emit_elf64.c:1011-1015` | 5 | allow non-u8 array sources for `ARRAY_BYTE_VIEW` ptr |
| `conformance/native/pass/typed-span-{f32,i32}.0` | 6 | new fixtures |
| `conformance/native/pass/math-{rmsnorm,softmax}-span.0` | 6 | math kernels as span fns |
| `conformance/run.mjs` | 6 | register new fixtures |

## Risks

1. **Other `BYTE_VIEW` consumers assume u8 implicitly.** Things like `IR_VALUE_BYTE_VIEW_LEN` lowering today returns the slot-8 value as bytes; for typed spans this becomes "element count". For non-span byte views (`String`, `ByteBuf`), it remains bytes — but those have `element_type = U8` so stride is 1 and the math is identical. Confirm by reading every `BYTE_VIEW_LEN` / `BYTE_VIEW_INDEX_LOAD` callsite in Phase 1.
2. **Slicing.** `span[a..b]` currently emits `IR_VALUE_BYTE_SLICE` with byte-granular `start`. For typed spans this is wrong: `start * stride` is needed for the ptr, while `end - start` is correct for the len (in elements). Documented as out-of-scope F3.b; the new fixtures must avoid slicing typed spans.
3. **Coercion vs assignment.** `let s: Span<f32> = arr` may go through a different path than `foo(arr)` (call-site coercion). Need to confirm both in Phase 5.
4. **Stdlib intrinsics over `Span<u8>`.** `std.mem.copy`, `std.fs.read`, `std.mem.len`, `std.mem.eqlBytes` are typed for `Span<u8>` / `MutSpan<u8>`. Typed spans cannot be passed to them — checker rejects on element-type mismatch, which is the correct behavior for v1. Document in the fixture: typed spans interact via `[i]`, `len()`, and structural ops only.
5. **Cross-backend parity.** Mach-O / COFF / AArch64 inherit the same gate at their own validate functions. F3 lands ELF64 only; replicate the same shape per backend when float codegen extends there. Same precedent as F1.

## Exit criteria (whole plan)

- `pnpm run conformance` green with new fixtures.
- `pnpm run native:test` green.
- `mutable-spans.0` and `generic-spans.0` reach the runtime assertion path on Linux x86-64, emitting their expected stdout — no longer CGEN004-skipped.
- A Zero program can declare a function taking `Span<f32>` / `MutSpan<f32>` / `Span<i32>` / etc., index it, write through it (mut case), read its length, and pass an `[N]T` literal to it.
- `examples/llama2/` is unblocked from the typed-span side. Binary-f32-decoding (F4) and libm precision pinning (F5) remain separate.

## Future migration (out of this plan)

- **F3.b — slicing typed spans.** `IR_VALUE_BYTE_SLICE` ptr arithmetic generalized by stride. Scope: ~2 days once a user hits it.
- **AArch64 / Mach-O / COFF parity.** Each backend lifts its byte-view-param gate and adds element-type-keyed INDEX_LOAD/STORE. Same shape as the F1 NaN-compare migration. Scope: ~1 day per backend.
- **Typed `Maybe<Span<T>>`.** When a user needs optional typed spans. `IR_TYPE_MAYBE_BYTE_VIEW` extends with element_type analogously.
- **Element-type-generic stdlib intrinsics.** `std.mem.copyT<T>(dst: MutSpan<T>, src: Span<T>)` etc. Separate language/stdlib design — not blocked by this plan.

## Connections

- [`../math/plan.md`](../math/plan.md) — F3 source. This plan resolves it.
- [`../llama2/overview.md`](../llama2/overview.md) — primary forcing function; typed spans unblock reusable kernels (matmul, attention, FFN).
