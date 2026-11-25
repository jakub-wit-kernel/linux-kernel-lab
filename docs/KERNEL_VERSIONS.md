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

| Kernel | Where | Build (`-Werror`; W=1 and sparse in the build jobs) | Host tests | QEMU tests (KVM) | Evidence (CI runs) |
|---|---|---|---|---|---|
| 6.8.0-1070-azure | Ubuntu 22.04 runner | passed in every run, M1–M6 | passed: every suite, M1–M7 | not run | 45386256336 (M1), 45403541128 (M2), 45411395148 (M3), 45416452233 (M4), 45425305054 (M5), 45453186271 (M6), 45520291266 (M7) |
| 7.0.0-1017-azure | Ubuntu 24.04 runner image, and the guest kernel booted from it | passed: M1, M2, M5 build jobs | passed: M1 | passed: M1, M5, M6 | host 45386256336; build 45403541128, 45425305054; QEMU 45386256336, 45425305054, 45453186271 |
| 7.0.0-1014-aws | Ubuntu 24.04 runner image, and the guest kernel booted from it | passed: M3, M4, M6, M7 build jobs, and every host test job below | passed: M2–M7 | passed: M2–M4, M7 | host 45403541128, 45411395148, 45416452233, 45425305054, 45453186271, 45520291266; QEMU 45403541128, 45411395148, 45416452233, 45520291266 |
| 6.12.0 (kernel.org, lockdep + KASAN + kmemleak) | QEMU guest, built by `scripts/build-debug-kernel.sh` | modules built against it | n/a | passed: every suite plus the detector demonstrations, M4–M7 (eBPF tests skipped: no libbpf in that job, no BTF in that kernel) | 45416452233 (M4), 45425305054 (M5), 45453186271 (M6), 45520291266 (M7) |

GitHub runs `ubuntu-24.04` jobs on more than one runner image, so jobs in
the same CI run can get different 7.0 kernels. Each test log starts with
`# kernel <release>`. The tables in [TESTING.md](TESTING.md) name the
kernel each job actually ran, where it was checked.

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
| Python 3.8+ | `tests/lib/kmsg.py`, `tests/lib/nettool.py` | `python3` |
| ip, ping, ethtool | networking tests (namespaces, traffic, driver counters) | `iproute2`, `iputils-ping`, `ethtool` |
| clang, libbpf ≥ 1.0 | eBPF programs and `lklbpf` | `clang`; libbpf from `scripts/build-libbpf.sh` (needs `libelf-dev`, `zlib1g-dev`) or a distro package ≥ 1.0 |
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
