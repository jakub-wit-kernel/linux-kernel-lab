# Changelog

All notable changes to this project are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Milestones are listed when they are merged.

## [Unreleased]

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
