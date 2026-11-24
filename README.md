# linux-kernel-lab

A hands-on Linux kernel development lab: kernel modules, drivers, kernel
interfaces, concurrency, memory management, networking and eBPF. It is
built milestone by milestone, and every piece is tested on real kernels
and inside QEMU.

**Status:** started 2026-11-24. See [docs/ROADMAP.md](docs/ROADMAP.md)
for each milestone's status. Only milestones marked *Completed* there
are claimed as done.

## Quick start

```bash
make                                  # build all modules for the running kernel
cmake -S . -B build && cmake --build build   # user-space libraries, CLIs, tests
sudo tests/run_all.sh                 # load each module and run its tests
scripts/qemu-test.sh                  # the same tests inside a QEMU VM
python3 -m unittest discover -s tests/lib
```

See [docs/TESTING.md](docs/TESTING.md) for what each check covers and
[docs/KERNEL_VERSIONS.md](docs/KERNEL_VERSIONS.md) for supported and
tested kernels.

## Layout

| Path | Contents | Stage |
|---|---|---|
| `src/modules/` | Kernel module fundamentals | 1 |
| `drivers/` | Character devices, procfs and sysfs | 2 |
| `src/concurrency/` | Locking, RCU, wait queues, workqueues (M4) | 3 |
| `src/memory/` | Allocators, `vmalloc`, `mmap` to user space (M5) | 3 |
| `networking/` | Virtual Ethernet pairs with NAPI, a per-namespace netfilter hook (M6, in progress) | 4 |
| `ebpf/` | eBPF and XDP programs | 4 |
| `benchmarks/` | Performance experiments | 3–5 |
| `tests/` | Test harness and per-module tests | all |
| `scripts/` | Build, QEMU and static-analysis tooling | all |
| `docs/` | Roadmap, testing, kernel versions | all |

## License

GPL-2.0. See [LICENSE](LICENSE). Kernel modules declare
`MODULE_LICENSE("GPL")`.
