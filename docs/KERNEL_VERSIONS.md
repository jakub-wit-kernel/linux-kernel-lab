# Kernel versions and dependencies

## Supported range

The code targets **Linux 5.15 and later** (5.15 is the oldest LTS still
common in distributions). API differences inside that range are handled
in [`src/include/lkl_compat.h`](../src/include/lkl_compat.h):

| Change | Release | Shim |
|---|---|---|
| `timer_shutdown_sync()` added | 6.2 | `lkl_timer_shutdown_sync()` falls back to `del_timer_sync()` |
| `from_timer()` renamed `timer_container_of()` | 6.16 | `lkl_timer_container_of()` |

## Kernels actually tested

Only kernels that a passing test run has used are listed. Entries are
added from CI logs.

| Kernel | Where | Build | Host tests | QEMU tests | Evidence |
|---|---|---|---|---|---|
| (pending first CI run of milestone 1) | | | | | |

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
