## Status

Runnable today:

| API | Return | Notes |
| --- | --- | --- |
| `std.mem.copy(dst, src)` | `usize` | Copies from `Span<u8>` into caller-owned `MutSpan<u8>` storage and returns the copied byte count. |
| `std.mem.fill(dst, value)` | `usize` | Fills caller-owned `MutSpan<u8>` storage with a `u8` byte and returns the filled byte count. |
| `std.mem.eql(a, b)` | `Bool` | Compares string-backed byte inputs. |
| `std.mem.span(value)` | `Span<u8>` | Builds a native `Span<u8>` view over a string literal. |
| `std.mem.len(bytes)` | `usize` | Returns the length of a fixed array, `Span<T>`, or `MutSpan<T>`. |
| `std.mem.eqlBytes(a, b)` | `Bool` | Compares two `Span<T>`/`MutSpan<T>` values with the same element type. |
| `std.mem.nullAlloc()` | `NullAlloc` | Creates an allocator that always returns `null`, useful for proving code does not allocate. |
| `std.mem.fixedBufAlloc(buffer)` | `FixedBufAlloc` | Creates a mutable fixed-buffer allocator from caller-owned `MutSpan<u8>` bytes. |
| `std.mem.arena(buffer)` | `FixedBufAlloc` | Arena-style alias over the fixed-buffer allocator model; `reset` rewinds the caller-owned storage. |
| `std.mem.pageAlloc()` | `PageAlloc` | Returns a `PageAlloc` handle backed by anonymous `mmap` (`MAP_ANONYMOUS`). Never creates an ambient global allocator. |
| `std.mem.generalAlloc()` | `GeneralAlloc` | Explicit general allocator handle metadata; callers still pass allocator state deliberately. |
| `std.mem.allocBytes(alloc, len)` | `Maybe<MutSpan<u8>>` | Allocates `len` bytes. With `NullAlloc` or `FixedBufAlloc`: uses caller-owned storage. With `PageAlloc`: returns a zero-filled `MutSpan<u8>` from a fresh anonymous mapping (kernel-zeroed pages — `calloc` semantics). `Maybe.none` if the mapping fails. |
| `std.mem.byteBuf(alloc, len)` | `Maybe<owned<ByteBuf>>` | Creates an owned byte buffer backed by explicit caller-provided allocator storage. |
| `std.mem.bufBytes(&buf)` | `MutSpan<u8>` | Borrows writable bytes from an owned `ByteBuf`. |
| `std.mem.bytesAsF32(bytes)` / `std.mem.bytesAsF64(bytes)` | `Span<f32>` / `Span<f64>` | Reinterprets a `Span<u8>` as a typed float view over the same bytes — no copy, no allocation. Length becomes `byteLen / 4` (or `/ 8`); the pointer is unchanged. |
| `std.mem.bytesAsMutF32(bytes)` / `std.mem.bytesAsMutF64(bytes)` | `MutSpan<f32>` / `MutSpan<f64>` | Mutable form over `MutSpan<u8>`. Required to *write* float results into a heap region: there is no `writeF32Le`, so kernels store through a typed span. |
| `std.mem.bufLen(&buf)` | `usize` | Returns the live length of a `ByteBuf`. |
| `std.mem.reset(&mut arena)` | `Void` | Resets caller-owned arena/fixed-buffer allocation state. |
| `std.mem.capacity(arena)` | `usize` | Reports fixed-buffer capacity. |
| `std.mem.vec(storage)` | `Vec` | Monomorphic byte vector over caller-owned mutable storage. |
| `std.mem.vecPush(&mut vec, value)` | `Bool` | Appends one byte when capacity remains; returns `false` instead of growing implicitly. |
| `std.mem.vecLen(&vec)` | `usize` | Reports current vector length. |
| `std.mem.vecCapacity(&vec)` | `usize` | Reports caller-provided vector capacity. |
| `std.mem.mapEmpty()` / `std.mem.setEmpty()` | `Map` / `Set` | Empty fixed metadata values with no allocation. |
| `std.mem.mapLen(&map)` / `std.mem.setLen(&set)` | `usize` | Reports `0` for the current empty metadata values. |

## Example

```zero
shape SliceView {
    bytes: Span<u8>,
    values: Span<i32>,
}

pub fun main(world: World) -> Void raises {
    let bytes: Span<u8> = std.mem.span("zero-memory")
    let same = std.mem.span("zero-memory")
    let mut scratch: [11]u8 = [0_u8; 11]
    let copied = std.mem.copy(scratch, bytes)
    let mut ints: [3]i32 = [1, 2, 3]
    let intSpan: MutSpan<i32> = ints
    intSpan[1] = 20
    let view = SliceView { bytes: bytes, values: intSpan }
    if copied == 11 && std.mem.len(view.bytes) == 11 && std.mem.eqlBytes(view.bytes, same) &&
        std.mem.len(view.values) == 3 && std.mem.eqlBytes(view.values, intSpan) {
        check world.out.write("memory type forms runnable\n")
    }
}
```

## Allocator Example

```zero
pub fun main(world: World) -> Void raises {
    let mut storage: [8]u8 = [0, 0, 0, 0, 0, 0, 0, 0]
    let mut alloc: FixedBufAlloc = std.mem.fixedBufAlloc(storage)
    let bytes = std.mem.allocBytes(alloc, 4)

    if bytes.has {
        bytes.value[0] = 90
        check world.out.write("fixed buffer allocated\n")
    }
}
```

Effects: none beyond writes performed by caller code.

Allocation behavior:

- `NullAlloc` always returns `null`.
- `FixedBufAlloc` and `Arena` return `MutSpan<u8>` views into caller-owned
  storage.
- `ByteBuf` owns a slice of explicit allocator storage and never reaches for a
  global heap.
- `PageAlloc` allocates via anonymous `mmap`. `allocBytes(pageAlloc, n)` returns
  a zero-filled region (kernel-zeroed pages — equivalent to `calloc` semantics).
  Bump/region-style for v0.1: allocate once, live for the run; the region is
  reclaimed at process exit. No per-allocation free is provided yet.
- `GeneralAlloc` is metadata only at this phase.

Ownership: spans from `FixedBufAlloc`/`Arena` borrow from the original fixed
buffer; regions from `PageAlloc` are process-scoped and have no per-allocation
ownership handle.

Target support: `FixedBufAlloc`, `Arena`, `NullAlloc`, and `ByteBuf` are
target-neutral. `PageAlloc` (`MAP_ANONYMOUS`) requires `linux-musl-x64`. The
`bytesAs*` reinterpret helpers are lowered by the direct backend
(`linux-musl-x64`); other backends report them as unsupported, like the
`std.codec.readF*Le` reads.

Capability: `FixedBufAlloc`, `Arena`, `NullAlloc`, and `allocBytes` over
caller-owned buffers fall under the `alloc` capability. `PageAlloc` and
`GeneralAlloc` are gated under the dedicated deniable `heap` capability —
separate from `alloc`, so callers that use only fixed-buffer or arena allocators
are not affected by a `heap` denial. The `bytesAs*` reinterpret helpers allocate
nothing and copy nothing, so they require no allocator capability beyond
`memory`.

## PageAlloc Example

Anonymous-mmap region allocation (based on `conformance/native/pass/page-alloc-region.0`):

```zero
pub fun main(world: World) -> Void raises {
    let alloc = std.mem.pageAlloc()
    let region = std.mem.allocBytes(alloc, 4194304)
    if region.has {
        let dst: MutSpan<u8> = region.value
        let z0: u8 = dst[0]
        let zmid: u8 = dst[2097152]
        let zlast: u8 = dst[4194303]
        let pattern: [4]u8 = [65, 66, 67, 68]
        let copied = std.mem.copy(dst, pattern)
        if z0 == 0_u8 && zmid == 0_u8 && zlast == 0_u8 && copied == 4 &&
            dst[0] == 65_u8 && dst[1] == 66_u8 && dst[2] == 67_u8 && dst[3] == 68_u8 {
            check world.out.write("page alloc region ok\n")
        }
    }
}
```

The kernel zeroes pages on delivery (`calloc` semantics). The region lives for
the duration of the process; there is no per-allocation free in v0.1.
`PageAlloc` requires the `heap` capability; `FixedBufAlloc`/`Arena`/`NullAlloc`
require only `alloc`.

## Typed Reinterpret Example

`bytesAs*` views a byte region as floats without copying, so kernels keep their
`Span<f32>`/`MutSpan<f32>` signatures over mmap'd weights or a `pageAlloc`'d
scratch region (based on `conformance/native/pass/mem-bytes-as-mut-f32.0`):

```zero
pub fun main(world: World) -> Void raises {
    let alloc = std.mem.pageAlloc()
    let region = std.mem.allocBytes(alloc, 4096)
    if region.has {
        let bytes: MutSpan<u8> = region.value
        let floats: MutSpan<f32> = std.mem.bytesAsMutF32(bytes)
        floats[0] = 1.5
        floats[1] = 2.5
        floats[3] = floats[0] + floats[1]
        if std.mem.len(floats) == 1024 && floats[3] == 4.0 {
            check world.out.write("typed reinterpret ok\n")
        }
    }
}
```

The result aliases the same memory: a 4096-byte region becomes a `MutSpan<f32>`
of length `4096 / 4 = 1024`. Reinterpreting a read-only `Span<u8>` (for example
from `std.fs.mappingBytes`) yields a `Span<f32>`; compose with a byte slice to
carve a tensor at an offset, e.g. `std.mem.bytesAsF32(weights[off..off + n * 4])`.
Offsets are 4-/8-aligned in practice; loads and stores use unaligned
`MOVSS`/`MOVSD`, so misaligned regions stay correct.

## Reporting Contract

`zero mem --json <input>` reports the allocator contract in machine-readable form:

- `memoryBudgets`: stack, static, heap, arena, fixed-buffer, collection-capacity, allocator-capacity, requested-allocation, and linear-memory floor budgets.
- `allocatorFacts`: `NullAlloc`, `FixedBufAlloc`, `Arena`, `PageAlloc`, and
  `GeneralAlloc` usage, capacity, failure behavior, and
  hidden-global-allocator status.
- `allocationInstrumentation`: pay-as-used hooks for attempts, successes, failures, requested bytes, granted bytes, and peak live bytes.
- `collectionFacts`: fixed-capacity `Vec`, owned `ByteBuf`, and empty `Map`/`Set` metadata, including growth/failure/cleanup behavior.

All heap budgets are explicit. A program that only uses `std.mem` remains at
`heapBytes: 0`, `globalHeapBytes: 0`, and `hiddenHeapAllocation: false` unless
an allocator API documents otherwise.

## Design Notes

No standard collection may silently allocate from a global heap. Heap-owning APIs
will require an allocator capability and document ownership, capacity, and
cleanup.
