# Changelog

All notable changes to this project are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Milestones are listed when they are merged.

## [Unreleased]

### Added
- M6 (in progress): `lkl_vnet` virtual Ethernet pairs with a NAPI
  receive path, `lkl_nf` per-namespace netfilter hook, namespace-based
  networking tests and `tests/lib/nettool.py`.

## [0.5.0] - 2026-11-24

### Added
- M5 (PR #9): `lkl_mem`, memory experiments run through sysfs
  (`kmalloc` size classes and alignment, a constructed and aligned
  `kmem_cache`, buddy-block alignment, `vmalloc` physical layout);
  `/dev/lkl_mmap`, a buffer mapped into user space by a fault handler,
  with an 11-case C suite; kmemleak and KASAN demonstrations.
- The debug kernel also enables kmemleak and dynamic debug, and boots
  with `kasan_multi_shot`.

### Fixed
- The `lkl_sync` RCU use-after-free demonstration could miss its race
  and go unreported (seen once in CI); readers now hold the stale
  pointer for 50 us in that demonstration only.

## [0.4.0] - 2026-11-24

### Added
- M4 (PR #7): `lkl_sync`, concurrency experiments run through sysfs
  (softirq-safe spinlocks, atomics, an unlocked racy counter, mutexes,
  RCU, a wait-queue bounded buffer, workqueues and delayed work), each
  checking an invariant in the kernel; detector demonstrations for
  lockdep and KASAN; `scripts/build-debug-kernel.sh` and a CI job that
  runs the whole suite on a lockdep + KASAN 6.12 kernel in QEMU.

### Changed
- checkpatch ignores `ALLOC_WITH_SIZEOF`: the suggested `kzalloc_obj()`
  does not exist on 6.x kernels.

## [0.3.0] - 2026-11-24

### Added
- M3 (PR #5): `lkl_events` with a `seq_file` listing paged by
  sequence number, a binary snapshot frozen at open, a procfs control
  file, validated sysfs attributes, a pollable `alert` attribute driven
  by `sysfs_notify()`, a debug attribute group gated by `is_visible`,
  and debugfs internals; a C interface suite and a shell test.

## [0.2.0] - 2026-11-24

### Added
- M2 (PR #3): `lkl_buf` seekable buffer character device with
  extensible, versioned ioctl structs; `liblklbuf` and the `lklbuf` CLI
  (CMake); a C device test suite including torn-read and concurrent
  writer checks; library unit tests that run without a kernel.

## [0.1.0] - 2026-11-24

### Added
- M1 (PR #1): kernel module fundamentals. Four modules (`lkl_hello`,
  `lkl_params`, `lkl_logging`, `lkl_lifecycle`), a compatibility header
  for 6.2/6.16 API changes, a TAP test harness with structured
  `/dev/kmsg` assertions (`tests/lib/kmsg.py`), QEMU testing via
  virtme-ng, checkpatch/sparse/W=1 checks and GitHub Actions CI.
- Repository foundation: license (GPL-2.0), contribution guide, roadmap,
  directory layout.
