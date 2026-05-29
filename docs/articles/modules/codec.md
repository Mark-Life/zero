## Status

Runnable today:

| API | Return | Notes |
| --- | --- | --- |
| `std.codec.crc32(bytes)` | `u32` | Computes CRC-32 for a string-backed byte input. |
| `std.codec.crc32Bytes(bytes)` | `u32` | Computes CRC-32 for a span or mutable span without allocation. |
| `std.codec.encodedVarintLen(value)` | `usize` | Returns the byte length of an unsigned varint encoding. |
| `std.codec.readU8(bytes)` | `u8` | Reads one byte. |
| `std.codec.readU16(bytes)` | `u16` | Reads two bytes as little-endian. |
| `std.codec.readU32(bytes)` | `u32` | Reads four bytes as little-endian. |
| `std.codec.readI32Le(bytes, offset)` | `i32` | Reads four bytes as a signed little-endian `i32` at the byte offset. Unaligned-safe on x64/ARM64. Bounds-checked; out-of-range traps. |
| `std.codec.readU32Le(bytes, offset)` | `u32` | Same as `readI32Le`, unsigned. |
| `std.codec.readU16Le(bytes, offset)` | `u16` | Reads two bytes as an unsigned little-endian `u16` at the byte offset. |
| `std.codec.readI64Le(bytes, offset)` | `i64` | Reads eight bytes as a signed little-endian `i64` at the byte offset. |
| `std.codec.readU64Le(bytes, offset)` | `u64` | Same as `readI64Le`, unsigned. |
| `std.codec.readF32Le(bytes, offset)` | `f32` | Reads four bytes as an IEEE 754 single-precision little-endian `f32` at the byte offset. |
| `std.codec.readF64Le(bytes, offset)` | `f64` | Reads eight bytes as an IEEE 754 double-precision little-endian `f64` at the byte offset. |
| `std.codec.writeU16(value)` | `u32` | Packs a `u16` value into the current write representation. |
| `std.codec.writeU32(value)` | `u32` | Packs a `u32` value into the current write representation. |

Current limits:

- Buffer-backed write APIs.
- Error-producing reads for short inputs (today's reads trap on out-of-range
  rather than returning a `Maybe<T>`).
- Streaming encoders and decoders.
- `readU8` / `readU16` / `readU32` are compile-time literal folds; the runtime
  offset reads above (`readU16Le` / `readI32Le` / `readU32Le` / `readI64Le` /
  `readU64Le` / `readF32Le` / `readF64Le`) are the dynamic counterparts.

## Example

```zero
use std.codec
use std.mem

pub fn main Void world World !
  let len std.codec.encodedVarintLen 300
  let checksum std.codec.crc32 "zero"
  let bytes std.mem.span "zero"
  let byte_checksum std.codec.crc32Bytes bytes
  if && (== len 2) (== checksum byte_checksum)
    check world.out.write "codec primitives ok\n"
```

## Runtime LE Example

The `readI32Le` / `readU32Le` / `readF32Le` / `readF64Le` helpers read a typed
scalar from a `Span<u8>` at a dynamic byte offset. The offset is not scaled by
the element size, so they handle headers and packed records that mix widths and
sit at arbitrary positions:

```zero
pub fn main Void world World !
  let bytes [20]u8 [0, 0, 192, 63, 0, 0, 0, 64, 0, 0, 64, 64, 0, 0, 128, 64, 0, 0, 160, 64]
  let view Span<u8> bytes
  let a f32 std.codec.readF32Le view 0
  let b f32 std.codec.readF32Le view 4
  let c f32 std.codec.readF32Le view 8
  if && (&& (== a 1.5) (== b 2.0)) (== c 3.0)
    check world.out.write "codec le reads ok\n"
```

The reads are unaligned-safe on x86-64 and AArch64 (both ISAs tolerate unaligned
scalar loads). A bounds-violating offset traps deterministically (`brk` on
AArch64, `ud2` on x64); programs that need to recover from a short input should
pre-check `std.mem.len bytes` against `offset + size`.

## Design Notes

The current helpers are intentionally narrow. They prove integer widths and
deterministic byte math before allocator-backed buffers are added.
