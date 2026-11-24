# Testing

## Layers

| Layer | Command | Needs |
|---|---|---|
| Test-tool unit tests | `python3 -m unittest discover -s tests/lib` | Python 3.8+ only |
| Shell lint | `shellcheck scripts/*.sh tests/*.sh tests/*/*.sh` | shellcheck |
| Build | `make` (warnings are errors) | headers for the target kernel |
| Extra warnings | `make w1` | as above |
| sparse | `make sparse` | sparse |
| Coding style | `scripts/checkpatch.sh --fetch && scripts/checkpatch.sh` | network once (fetches upstream `checkpatch.pl` at a pinned tag) |
| Module tests, host kernel | `sudo tests/run_all.sh` | root, modules built for the running kernel |
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

| Check | Dev sandbox (no module support, no network) | CI: Ubuntu 22.04 runner | CI: Ubuntu 24.04 runner | CI: QEMU guest |
|---|---|---|---|---|
| kmsg.py unit tests | passed (16 tests) | see PR | see PR | n/a |
| Shell syntax (`bash -n`) | passed | n/a | n/a | n/a |
| Test harness refuses to run without module support | passed (bails out) | n/a | n/a | n/a |
| Build, W=1, sparse, checkpatch | not possible (no kernel headers) | see PR | see PR | n/a |
| Module tests | not possible | see PR | see PR | see PR |

The CI columns are filled in from the milestone pull request's actual CI
results.
