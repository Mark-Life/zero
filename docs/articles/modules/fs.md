## Status

Runnable today:

| API | Return | Notes |
| --- | --- | --- |
| `std.fs.read(path, buf)` | `usize` | Reads bytes from a hosted path into a caller-provided `MutSpan<u8>` buffer. |
| `std.fs.write(path, bytes)` | `usize` | Writes bytes to a hosted path and returns the byte count. |
| `std.fs.host()` | `Fs` | Creates the hosted filesystem capability. |
| `std.fs.open(fs, path)` | `Maybe<owned<File>>` | Opens a file and returns `null` when unavailable. |
| `std.fs.openOrRaise(fs, path)` | `owned<File>` | Opens a file or raises `![NotFound TooLarge Io]`. |
| `std.fs.create(fs, path)` | `Maybe<owned<File>>` | Creates a file and returns `null` when unavailable. |
| `std.fs.createOrRaise(fs, path)` | `owned<File>` | Creates a file or raises `![NotFound TooLarge Io]`. |
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
| `std.fs.mmap(fs, path)` | `Maybe<owned<Mapping>>` | Memory-maps a file read-only. Returns `null` when the file is absent or unreadable; otherwise an owned `Mapping` that releases the mapping at end-of-scope (or explicit `munmap`). |
| `std.fs.mappingBytes(&m)` | `Span<u8>` | Borrows the mapped bytes from an owned `Mapping`. Zero-copy; combine with `std.mem.bytesAs*` to read typed views. |
| `std.fs.munmap(&mut m)` | `Void` | Explicitly releases the mapping. |

Current limits:

- Mappings are read-only and `MAP_PRIVATE` only. No writable / shared mmap yet.
- 32-bit mapping length (sufficient for all current targets; >4 GiB files
  return their first 4 GiB).
- Richer permissions and platform-specific file modes.
- Directory walking.
- Async or nonblocking I/O.

## Example

```zero
pub fn main Void world World ![NotFound TooLarge Io]
  let fs std.fs.host()
  mut file owned<File> check std.fs.createOrRaise fs ".zero/out/example.txt"
  check std.fs.writeAllOrRaise (&mut file) (std.mem.span "hello\n")
  let len check std.fs.fileLenOrRaise (&mut file)
  std.fs.close (&mut file)
  if && (== len 6) (std.fs.exists ".zero/out/example.txt")
    if std.fs.rename ".zero/out/example.txt" ".zero/out/example-renamed.txt"
      if std.fs.remove ".zero/out/example-renamed.txt"
        check world.out.write "fs ok\n"
```

## Mmap Example

Read a small file into a typed view via `mmap` + `bytesAs*` with no intermediate
copy:

```zero
pub fn main Void world World !
  let fs Fs std.fs.host()
  let region Maybe<owned<Mapping>> std.fs.mmap fs "data.bin"
  if == region.has false
    ret
  let m owned<Mapping> region.value
  let bytes Span<u8> std.fs.mappingBytes (&m)
  let words Span<f32> std.mem.bytesAsF32 bytes
  if > (std.mem.len words) 0
    check world.out.write "mmap ok\n"
  std.fs.munmap (&mut m)
```

On `darwin-arm64` and `darwin-x64`, `mmap`/`munmap` lower to `libSystem` calls;
the build routes through the runtime obj+link path so libSystem binds
automatically. On `linux-musl-x64` and `linux-musl-arm64`, the same surface
lowers to raw `mmap`/`munmap`/`openat`/`lseek`/`close` syscalls and stays on
the pure direct-exe path — but the `fs` capability is excluded from the default
direct-exe heuristic, so the explicit `--backend` flag is required
(`zero-elf64` for `linux-musl-x64`, `zero-elf-aarch64` for `linux-musl-arm64`).

## Design Notes

The path helpers are a small current API, not a hidden global filesystem.

Stable file APIs make effects, ownership, and cleanup visible through
capabilities.

Hosted filesystem APIs are denied on non-host targets with `TAR002`.
Target-neutral packages should keep filesystem code outside their cross-target
entry point.
