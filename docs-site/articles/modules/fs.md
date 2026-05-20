## Status

Runnable today:

| API | Return | Notes |
| --- | --- | --- |
| `std.fs.read(path, buf)` | `usize` | Reads bytes from a hosted path into a caller-provided `MutSpan<u8>` buffer. |
| `std.fs.write(path, bytes)` | `usize` | Writes bytes to a hosted path and returns the byte count. |
| `std.fs.host()` | `Fs` | Creates the hosted filesystem capability. |
| `std.fs.open(fs, path)` | `Maybe<owned<File>>` | Opens a file and returns `null` when unavailable. |
| `std.fs.openOrRaise(fs, path)` | `owned<File>` | Opens a file or raises `{ NotFound, TooLarge, Io }`. |
| `std.fs.create(fs, path)` | `Maybe<owned<File>>` | Creates a file and returns `null` when unavailable. |
| `std.fs.createOrRaise(fs, path)` | `owned<File>` | Creates a file or raises `{ NotFound, TooLarge, Io }`. |
| `std.fs.readOrRaise(&mut file, buf)` | `usize` | Reads into caller storage or raises. |
| `std.fs.writeAll(&mut file, bytes)` | `Bool` | Writes bytes to an owned file handle. |
| `std.fs.writeAllOrRaise(&mut file, bytes)` | `Void` | Writes all bytes or raises. |
| `std.fs.fileLen(&mut file)` | `Maybe<usize>` | Reports file length when available. |
| `std.fs.fileLenOrRaise(&mut file)` | `usize` | Reports the file length or raises. |
| `std.fs.readAll(alloc, fs, path, limit)` | `Maybe<owned<ByteBuf>>` | Reads through an explicit allocator and size limit. |
| `std.fs.readAllOrRaise(alloc, fs, path, limit)` | `owned<ByteBuf>` | Reads through an explicit allocator and size limit. |
| `std.fs.readBytes(path, buf)` | `Maybe<usize>` | Reads bytes into caller storage. |
| `std.fs.writeBytes(path, bytes)` | `Maybe<usize>` | Writes byte spans to a hosted path. |
| `std.fs.exists(path)` | `Bool` | Checks whether a hosted path exists. |
| `std.fs.isDir(path)` | `Bool` | Checks whether a hosted path is a directory. |
| `std.fs.makeDir(path)` | `Bool` | Creates a hosted directory. |
| `std.fs.removeDir(path)` | `Bool` | Removes a hosted directory. |
| `std.fs.remove(path)` | `Bool` | Removes a hosted file path. |
| `std.fs.rename(old, new)` | `Bool` | Renames a hosted file path. |
| `std.fs.dirEntryCount(path)` | `Maybe<usize>` | Counts entries in a hosted directory. |
| `std.fs.tempName(buffer, prefix)` | `Maybe<String>` | Writes a temporary path into caller storage. |
| `std.fs.atomicWrite(path, temp, bytes)` | `Bool` | Writes through a caller-provided temporary path and renames. |
| `std.fs.close(&mut file)` | `Void` | Closes an owned file handle explicitly; remaining owned files are cleaned up deterministically. |
| `std.fs.mmap(fs, path)` | `Maybe<owned<Mapping>>` | Maps a file read-only into memory via `mmap` (`PROT_READ`/`MAP_PRIVATE`). The fd is closed after mapping; the mapping survives. `Maybe.none` if the file cannot be opened. |
| `std.fs.mmapOrRaise(fs, path)` | `owned<Mapping>` | Raising variant of `mmap`. Raises `{ NotFound }` on failure. |
| `std.fs.mappingBytes(&m)` | `Span<u8>` | Returns a read-only `(ptr, len)` view over the whole mapping. Length equals file size. Pair with `std.codec.readF32Le`/`readU32Le` etc. |
| `std.fs.munmap(&mut m)` | `Void` | Unmaps the region (`munmap`). Must be called explicitly — Zero has no implicit drop/RAII. |

Current limits:

- Richer permissions and platform-specific file modes.
- Directory walking.
- Async or nonblocking I/O.
- `mmap`/`munmap` run on `linux-musl-x64` only; other targets report the capability as denied.

Metadata labels:

- effects: filesystem, memory
- allocation behavior: no heap allocation; `Mapping` is a kernel-managed region, not heap memory
- target support: path helpers are target-neutral; `mmap`/`munmap` require `linux-musl-x64`
- error behavior: `open`/`create`/`mmap` return `Maybe.none` or raise `{ NotFound, TooLarge, Io }` on failure
- ownership notes: `owned<File>` and `owned<Mapping>` are move-only resources tracked at compile time; close/munmap must be called explicitly
- example: `conformance/native/pass/mmap-file-readonly.0`

## Example

```zero
pub fun main(world: World) -> Void raises { NotFound, TooLarge, Io } {
    let fs = std.fs.host()
    let mut file: owned<File> = check std.fs.createOrRaise(fs, ".zero/out/example.txt")
    check std.fs.writeAllOrRaise(&mut file, std.mem.span("hello\n"))
    let len = check std.fs.fileLenOrRaise(&mut file)
    std.fs.close(&mut file)
    if len == 6 && std.fs.exists(".zero/out/example.txt") {
        if std.fs.rename(".zero/out/example.txt", ".zero/out/example-renamed.txt") {
            if std.fs.remove(".zero/out/example-renamed.txt") {
                check world.out.write("fs ok\n")
            }
        }
    }
}
```

## Memory-Mapped File Example

Read-only mmap of a binary file containing three little-endian f32 values
(based on `conformance/native/pass/mmap-file-readonly.0`):

```zero
pub fun main(world: World) -> Void raises {
    let fs = std.fs.host()
    let mut m: owned<Mapping> = check std.fs.mmapOrRaise(fs, "conformance/fixtures/mmap-f32le.bin")
    let bytes = std.fs.mappingBytes(&m)
    let v0: f32 = std.codec.readF32Le(bytes, 0)
    let v1: f32 = std.codec.readF32Le(bytes, 4)
    let v2: f32 = std.codec.readF32Le(bytes, 8)
    let one: f32 = 1.0
    let two: f32 = 2.0
    let three: f32 = 3.0
    if std.mem.len(bytes) == 12 && v0 == one && v1 == two && v2 == three {
        check world.out.write("mmap file readonly ok\n")
    }
    std.fs.munmap(&mut m)
}
```

`Mapping` is an `owned<T>` resource with compile-time use-after-move tracking,
exactly like `owned<File>`. Zero has no implicit drop/RAII — `std.fs.munmap(&mut m)`
must be called explicitly, mirroring `std.fs.close(&mut f)`.

Two usage shapes mirror `open`/`openOrRaise`:

- `Maybe` shape: `let mapped = std.fs.mmap(fs, path)` then
  `if mapped.has { let mut m = mapped.value ... std.fs.munmap(&mut m) }`
- Raising shape: `let mut m = check std.fs.mmapOrRaise(fs, path)` — errors
  propagate via `check`.

`mappingBytes` returns a read-only `Span<u8>` whose length equals the file size.
Combine with `std.codec.readF32Le`, `readU32Le`, etc. for zero-copy, lazily-paged
reads of large read-only files such as model weights.

The `fs` capability gates mmap alongside all other file operations. The fd is
closed after the mapping is established; the mapping itself survives until
`munmap` is called.

## Design Notes

The path helpers are a small current API, not a hidden global filesystem.

Stable file APIs make effects, ownership, and cleanup visible through
capabilities.

Hosted filesystem APIs are denied on non-host targets with `TAR002`.
Target-neutral packages should keep filesystem code outside their cross-target
entry point.
