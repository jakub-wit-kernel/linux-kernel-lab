# Stage 2 / M2: `lkl_buf`, a seekable buffer character device

`/dev/lkl_buf0 .. N-1` each behave like a fixed-capacity file. Standard
tools work on them (`dd`, `cmp`, `head`), and a small library and CLI
expose the ioctls.

| Path | What |
|---|---|
| [`lkl_buf.c`](lkl_buf.c) | The driver |
| [`../../include/uapi/linux/lkl_buf.h`](../../include/uapi/linux/lkl_buf.h) | User-space API: ioctl numbers and extensible structs |
| [`user/lklbuf.[ch]`](user/) | `liblklbuf`: typed ioctl wrappers that return `-errno` and never clobber `errno` |
| [`user/lklbuf_cli.c`](user/lklbuf_cli.c) | `lklbuf` CLI: `info`, `stats`, `read`, `write`, `fill`, `truncate`, `clear` |
| [`../../tests/drivers/`](../../tests/drivers/) | C test suite (17 cases) and the shell test that runs it with the CLI, `dd` and lifecycle checks |

## Semantics

| Operation | Behaviour |
|---|---|
| `read` | From the file position up to the data size; returns 0 at end of file. |
| `write` | At the file position (or at the end with `O_APPEND`), growing the size. A write that crosses capacity is short; one starting at capacity fails with `ENOSPC`. |
| Gaps | Writing past the end leaves a gap that reads back as zeros. |
| `lseek` | `SEEK_SET`/`CUR`/`END` within `[0, capacity]`; `SEEK_DATA` and `SEEK_HOLE` treat `[0, size)` as data. |
| `open(O_TRUNC)` | Empties the device for write opens; ignored for read-only opens, as for regular files. |
| ioctl | `GET_INFO`, `FILL`, `TRUNCATE`, `CLEAR`, `GET_STATS`, `RESET_STATS`. |

## Design points

**The zero-beyond-EOF invariant.** Every byte in `[size, capacity)` is
zero:
- Paths that shrink the data (truncate, clear, `O_TRUNC`) scrub what they
  cut off.
- `copy_from_user()` zero-fills whatever it fails to copy.

So growing the size is just moving a number, and a truncate-then-grow can
never resurface stale data. A test checks exactly that. An earlier draft
zeroed the gap *after* copying the user's data, which would have wiped
every extending write. The invariant made that bug impossible by
construction.

**Extensible ioctls.** The structs only grow at the end, and the kernel
reads the caller's struct size from `_IOC_SIZE(cmd)`:

| Direction | Caller's struct is smaller (older) | Caller's struct is larger (newer) |
|---|---|---|
| input (`FILL`) | zero-extended with `copy_struct_from_user()`, so a v0 caller gets `byte = 0` | accepted if the unknown bytes are zero, otherwise `E2BIG` |
| output (`GET_INFO`, `GET_STATS`) | gets the fields it knows | gets the known fields; the unknown tail is zeroed |

This is the pattern `openat2(2)` and `clone3(2)` use for syscalls, applied
to ioctls. The tests build ioctl numbers with v0 and "future" sizes to
check every row. Fixed-size ioctls (`TRUNCATE`) still match their exact
command number.

**Locking.** One mutex per device covers the data, size and counters.
It is held across `copy_{to,from}_user()`, which is safe here because the
driver has no `mmap`, so a page fault can't re-enter it. That also makes
each `read()`/`write()` atomic against other callers. A concurrency test
has two threads overwrite a 4 KiB block with all-`A` and all-`B` while
three readers check that every read sees one whole block, never a torn
mix.

**Lifecycle.**
- `cdev_add()` runs before `device_create()`, so the node only appears
  once `open()` can succeed.
- `fops.owner` pins the module while any file is open. The shell test
  checks that `rmmod` is refused with an fd open.
- Since 6.4, `class_create()` takes no owner argument. This is
  feature-detected in `lkl_compat.h`, because the old API was a macro.

## Run

```bash
make && cmake -S . -B build && cmake --build build
sudo tests/run_all.sh lkl_buf
sudo insmod drivers/chardev/lkl_buf.ko nr_devs=2 capacity=1048576
sudo build/lklbuf write 0 "hello" && sudo build/lklbuf read 0 5
sudo build/lklbuf info
```
