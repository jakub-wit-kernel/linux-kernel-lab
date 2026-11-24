# Kernel versions and dependencies

## Supported range

The code targets **Linux 6.2 and later**. 6.2 added
`timer_shutdown_sync()`, which teardown code should use because it also
prevents a timer from being re-armed. API differences inside the supported
range are handled in [`src/include/lkl_compat.h`](../src/include/lkl_compat.h)
by **feature detection**, not `LINUX_VERSION_CODE` comparisons, which go
wrong on distribution kernels that backport APIs:

| Change | Release | Shim |
|---|---|---|
| `from_timer()` renamed `timer_container_of()` | 6.16 | `lkl_timer_container_of()`, chosen with `#ifdef timer_container_of` |

## Kernels actually tested

Only kernels that a passing test run has used are listed. Entries are
added from CI logs.

| Kernel | Where | Build | Host tests | QEMU tests | Evidence |
|---|---|---|---|---|---|
| 6.8.0-1070-azure | GitHub Actions, Ubuntu 22.04 runner | passed (`-Werror`, W=1, sparse) | passed: M1 106 checks; M2 C suite 17/17 + shell test | not run | CI runs 45386256336 (M1), 45403541128 (M2) |
| 7.0.0-1017-azure | GitHub Actions, Ubuntu 24.04 runner | passed (`-Werror`, W=1, sparse) | passed: same | passed under KVM: same | CI runs 45386256336 (M1), 45403541128 (M2) |

Untested so far: anything below 6.8, including the 6.2 floor. The floor
follows from the APIs used, not from a test run. A QEMU job booting an
older kernel would close that gap.

## Build and test dependencies

| Tool | Used for | Ubuntu package |
|---|---|---|
| gcc matching the kernel's compiler | building modules | `gcc-<N>` (`scripts/kernel-cc.sh` prints N) |
| kernel headers | building modules | `linux-headers-$(uname -r)` |
| sparse | `make sparse` | `sparse` |
| checkpatch.pl | style checks | fetched from upstream at a pinned tag by `scripts/checkpatch.sh --fetch` |
| Python 3.8+ | `tests/lib/kmsg.py` | `python3` |
| shellcheck | script lint | `shellcheck` |
| QEMU | isolated kernel testing | `qemu-system-x86` |
| virtme-ng | boots a kernel in QEMU with the host filesystem shared | `pip install virtme-ng` |

## WSL2

WSL2 runs Microsoft's kernel, which has no `linux-headers` package.
Build against a matching tree from the `microsoft/WSL2-Linux-Kernel`
repository and pass `KDIR=`. The procedure is the same as in the `kmsgq`
project's `docs/WSL2.md`. For isolated testing on WSL2, `scripts/qemu-test.sh`
can boot a kernel you built yourself:

```bash
scripts/qemu-test.sh ~/linux/arch/x86/boot/bzImage ~/linux
```
