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

Notes:
- The first CI run (45385836101) passed every module test but failed
  checkpatch and shellcheck. The fixes are in the PR's history:
  checkpatch was scanning generated `*.mod.c` files, and version-number
  comparisons were replaced with feature detection.
- On Ubuntu 22.04, sparse 0.6.4 prints many warnings from the kernel's
  own headers; `make sparse` fails only on findings in `src/`.
