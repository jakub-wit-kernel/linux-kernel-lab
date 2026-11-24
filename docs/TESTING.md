# Testing

## Layers

| Layer | Command | Needs |
|---|---|---|
| Test-tool unit tests | `python3 -m unittest discover -s tests/lib` | Python 3.8+ only |
| User-space build and unit tests | `cmake -S . -B build && cmake --build build && (cd build && ctest)` | CMake, a C compiler |
| Shell lint | `shellcheck scripts/*.sh tests/*.sh tests/*/*.sh` | shellcheck |
| Build | `make` (warnings are errors) | headers for the target kernel |
| Extra warnings | `make w1` | as above |
| sparse | `make sparse` | sparse |
| Coding style | `scripts/checkpatch.sh --fetch && scripts/checkpatch.sh` | network once (fetches upstream `checkpatch.pl` at a pinned tag) |
| Module and driver tests, host kernel | `sudo tests/run_all.sh` | root, modules built for the running kernel, user-space tools built (`build/`) |
| Module tests, QEMU | `scripts/qemu-test.sh` | qemu-system-x86, virtme-ng, readable kernel image; KVM optional |

## How module tests work

Each `tests/modules/test_*.sh` writes a unique marker into the kernel log
(`/dev/kmsg`), loads its module, and asserts only on records after that
marker. Assertions go through [`tests/lib/kmsg.py`](../tests/lib/kmsg.py),
which reads structured `/dev/kmsg` records rather than `dmesg` text, so a
test can also check each message's **log level**. Output is TAP.

Safeguards against false passes:
- **Preflight.** A missing `insmod`/`rmmod`/`modinfo`, `/proc/modules` or
  `/dev/kmsg` aborts with `Bail out!`. Without that, negative checks such
  as "the invalid write is rejected" would pass on a machine that can't
  load modules at all. That happened in this project's own development
  sandbox, and is why the guard exists.
- **First load must succeed.** If a script's first load fails, the script
  bails instead of continuing.
- **Kernel health.** Every script ends with `kmsg.py clean`, which fails
  on `BUG:`, `WARNING:`, `Oops`, KASAN/UBSAN/KCSAN reports and similar.
- **Really in QEMU.** `scripts/qemu-test.sh` sets `LKL_EXPECT_VM=1`, and
  `run_all.sh` then refuses to run unless the DMI vendor is `QEMU`.

## Verification record

What has actually run, and where. A cell says *passed* only when the run
completed successfully.

| Check | Dev sandbox (no module support, no network) | CI: Ubuntu 22.04 runner, Linux 6.8.0-1070-azure | CI: Ubuntu 24.04 runner, Linux 7.0.0-1017-azure | CI: QEMU guest (KVM), Linux 7.0.0-1017-azure |
|---|---|---|---|---|
| kmsg.py unit tests | passed (16) | n/a | passed (16) | n/a |
| Shell syntax / shellcheck | `bash -n` passed | n/a | shellcheck passed | n/a |
| Harness refuses to run without module support | passed (all scripts bail out) | n/a | n/a | n/a |
| Build with `-Werror` | not possible (no headers) | passed | passed | n/a (same modules) |
| W=1, sparse (findings in our sources) | not possible | passed (0) | passed (0) | n/a |
| checkpatch `--strict` (upstream v6.12) | not possible (no network) | passed: 0 errors, 0 warnings, 0 checks | passed: same | n/a |
| Module tests (4 scripts, 106 checks) | not possible | passed | passed | passed, platform verified as QEMU |

Evidence: CI run 45386256336 on PR #1 (`feature/m1-kernel-modules`).

### M2 (PR #3, CI run 45403541128)

The "Linux 7.0" columns of the M2–M4 tables are not one kernel. In these
runs the 24.04 host-test jobs and the QEMU guest ran 7.0.0-1014-aws, and
the 24.04 build job used 7.0.0-1017-azure (M2) or 7.0.0-1014-aws (M3,
M4). See docs/KERNEL_VERSIONS.md.

| Check | Dev sandbox | Ubuntu 22.04, Linux 6.8 | Ubuntu 24.04, Linux 7.0 | QEMU guest (KVM), Linux 7.0 |
|---|---|---|---|---|
| User-space build (gcc, clang, `-Werror`) and library unit tests | passed, also under ASan+UBSan | n/a | passed (gcc, clang, ASan+UBSan) | n/a |
| Driver build, W=1, sparse | not possible | passed | passed | n/a |
| checkpatch `--strict` (7 files) | not possible | passed: 0 errors, 0 warnings | passed: same | n/a |
| C device suite (17 cases) | correctly skips without a device | passed | passed | passed |
| Torn-read check (concurrent block reads, 0 torn) | not possible | 520,389 reads | 404,480 reads | 313,468 reads |
| Driver shell test (nodes, sysfs, CLI, dd, rmmod-while-open) | not possible | passed | passed | passed |

checkpatch's three `--strict` CHECK notes (blank lines in the UAPI
header) were fixed after this run; the fix is in the PR.

### M3 (PR #5, CI run 45411395148)

| Check | Dev sandbox | Ubuntu 22.04, Linux 6.8 | Ubuntu 24.04, Linux 7.0 | QEMU guest (KVM), Linux 7.0 |
|---|---|---|---|---|
| Driver build, W=1, sparse | not possible | passed | passed | n/a |
| checkpatch `--strict` (9 files) | not possible | 0 errors, 0 warnings, 0 checks | same | n/a |
| C interface suite (9 cases) | correctly skips without the module | passed | passed | passed |
| Paged read under concurrent adds (lines read, all strictly increasing) | not possible | 781 | 578 | 565 |
| Shell test (modes, validation, debugfs, `is_visible`, removal) | not possible | passed | passed | passed |
| All earlier suites (M1, M2) | | passed | passed | passed |

Notes:
- The first CI run (45385836101) passed every module test but failed
  checkpatch and shellcheck. The fixes are in the PR's history:
  checkpatch was scanning generated `*.mod.c` files, and version-number
  comparisons were replaced with feature detection.
- On Ubuntu 22.04, sparse 0.6.4 prints many warnings from the kernel's
  own headers; `make sparse` fails only on findings in `src/`.

### M4 (PR #7, CI run 45416452233)

The new `test-debug-kernel` job boots Linux 6.12.0, built by
`scripts/build-debug-kernel.sh` with `PROVE_LOCKING`, `KASAN`
(generic), `DEBUG_ATOMIC_SLEEP` and `DEBUG_LIST`, in QEMU. The CI log
shows each option set to `y`. The whole suite runs there.

| Check | Dev sandbox | Ubuntu 22.04, Linux 6.8 | Ubuntu 24.04, Linux 7.0 | QEMU (KVM), Linux 7.0 | QEMU (KVM), Linux 6.12 debug |
|---|---|---|---|---|---|
| `lkl_sync` build, W=1, sparse | not possible | passed | passed | n/a | built against the debug tree |
| checkpatch `--strict` (10 files) | not possible | 0 errors, 0 warnings | 0 errors, 0 warnings, 3 checks (long format strings) | n/a | n/a |
| `test_lkl_sync` (28 checks: 7 experiments' invariants, input validation) | not possible | passed | passed | passed | passed, no lockdep/KASAN reports |
| Unlocked `racy` counter: updates lost of 4,000,000 | not possible | 3,116,980 | 753,687 | 2,230,885 | 1,128,072 |
| Detector demonstrations | not possible | skipped (no detectors) | skipped | skipped | lockdep `inconsistent lock state` and `KASAN: use-after-free` both reported |
| All earlier suites (M1–M3) | | passed | passed | passed | passed, with no lockdep, KASAN or atomic-sleep reports |

Notes:
- The first run (45413625126) had every test job green, including the
  debug kernel, but failed checkpatch. The 7.0 checkpatch on the 24.04
  runner suggests `kzalloc_obj()`, which 6.x kernels lack, so
  `ALLOC_WITH_SIZEOF` is ignored.
- The debug kernel has no `CONFIG_DYNAMIC_DEBUG`, so one `test_logging`
  check is skipped there. It passes on the runner kernels.
- `racy` loses a different number of updates on every run. The numbers
  show that the race is real, not how big it is.
- KCSAN was not run: it cannot be combined with KASAN.

### M5 (PR #9, CI run 45425305054)

The debug kernel is now Linux 6.12.0 built with `PROVE_LOCKING`, `KASAN`,
`DEBUG_ATOMIC_SLEEP`, `DEBUG_LIST`, `DEBUG_KMEMLEAK` and
`DYNAMIC_DEBUG`. The CI log shows each one set to `y`. It boots with
`kasan_multi_shot`.

| Check | Dev sandbox | Ubuntu 22.04, Linux 6.8.0-1070-azure | Ubuntu 24.04, Linux 7.0.0-1014-aws | QEMU (KVM), Linux 7.0.0-1017-azure | QEMU (KVM), Linux 6.12 debug |
|---|---|---|---|---|---|
| `lkl_mem` build, W=1, sparse | not possible | passed | passed | n/a | built against the debug tree |
| checkpatch `--strict` (11 files) | not possible | 0 errors, 0 warnings, 6 checks (long format strings) | same | n/a | n/a |
| `lkl_mmap_test` C suite (11 cases) | compiles with gcc and clang `-Werror`; skips without the device | passed | passed | passed | passed |
| Faults on first touch / second touch of 8 mapped pages | not possible | 8 / 0 | 8 / 0 | 8 / 0 | 8 / 0 |
| `test_lkl_mem` (39 checks: experiments, validation, rmmod while open) | not possible | passed | passed | passed | passed, no KASAN reports |
| Physical runs behind an 8 MiB `vmalloc` (2048 pages) | not possible | 290 | 1 | 1 | 1104 |
| kmemleak: leaked objects reported (of 8), by content and backtrace | not possible | skipped | skipped | skipped | 4 |
| KASAN: `slab-out-of-bounds`, 1-byte write | not possible | skipped | skipped | skipped | reported |
| All earlier suites (M1–M4), including the M4 detector test | | passed | passed | passed | passed; the `test_logging` dynamic-debug check now runs here too |

`kmalloc` size classes were the same on every kernel: 1→8, 13→16,
100→128, 200→256, 5000→8192, 200000→262144. Powers of two map to
themselves.

Notes:
- The first run (45422185673) failed `test_lkl_mem` everywhere. The
  test assumed `kmem_cache_size()` includes alignment padding; it
  returns the object size (60). The test was fixed.
- In the same run, the M4 RCU use-after-free demonstration went
  unreported on the debug kernel. It passed in the next run without
  changes, so it was a race window that sometimes stayed shut. The
  demonstration now widens the window deliberately. The fix was
  verified in 45425305054.
- kmemleak reported 4 of the 8 leaked objects. The others were still
  referenced from somewhere kmemleak scans when it looked (for example
  a stale stack slot); the test requires at least one.
- Correction, made during M6: this note used to say the 24.04 runner
  image changed between M4 and M5. It did not. GitHub assigns
  `ubuntu-24.04` jobs to more than one image (`7.0.0-1017-azure` and
  `7.0.0-1014-aws`), job by job. docs/KERNEL_VERSIONS.md lists which job
  ran which kernel in every milestone run.

### M6 (PR #11, CI run 45453186271)

The debug kernel now also enables `NET_NS` and `NETFILTER`; the CI log
shows both set to `y`. All traffic runs between two network namespaces
over an `lkl_vnet` pair (`10.99.0.1` ↔ `10.99.0.2`).

| Check | Dev sandbox | Ubuntu 22.04, Linux 6.8.0-1070-azure | Ubuntu 24.04, Linux 7.0.0-1014-aws | QEMU (KVM), Linux 7.0.0-1017-azure | QEMU (KVM), Linux 6.12 debug |
|---|---|---|---|---|---|
| `lkl_vnet`, `lkl_nf` build, W=1, sparse | not possible | passed | passed | n/a | built against the debug tree |
| checkpatch `--strict` (13 files) | not possible | 0 errors, 0 warnings, 6 checks (long format strings) | same | n/a | n/a |
| `nettool.py` unit tests (UDP and TCP over loopback) | passed | n/a | passed | n/a | n/a |
| `test_lkl_vnet` (51 checks) | not possible | passed | passed | passed | passed, no lockdep/KASAN reports |
| UDP flood: datagrams received of 50,000 sent | not possible | 50,000 | 50,000 | 50,000 | 50,000 |
| TCP: 32 MiB with matching SHA-256 | not possible | passed | passed | passed | passed |
| Flow control (`rx_ring=16`): sender queue stops / wakes | not possible | 1,023 / 1,023 | 1,022 / 1,022 | 1,022 / 1,022 | 1,022 / 1,022 |
| Per-direction accounting identity after every phase | not possible | held | held | held | held |
| `test_lkl_nf` (36 checks: drops, per-namespace isolation, validation, new namespaces, unload with a rule active) | not possible | passed | passed | passed | passed |
| All earlier suites (M1–M5) and both detector tests | | passed (detector tests skipped) | passed (skipped) | passed (skipped) | passed, both reports required and seen |

Notes:
- The first run (45448471995) passed every test job but failed
  shellcheck. It flagged a variable defined for sourcing scripts and a
  function called only through `check`. Both now have a targeted
  `disable` with a comment.
- `napi_budget_exhausted` stayed 0 everywhere. That is expected with
  `rx_ring=16`: a poll can find at most 16 frames, and the budget is 64.
  The counter can only move with `rx_ring` above 64.
- One frame was counted as `rx_dropped` in every run. It is the
  4000-byte ping in the MTU test, also seen in `rx_dropped_mtu`.
