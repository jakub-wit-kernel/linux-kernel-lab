# Contributing

## Workflow

1. Branch from `main`: `feature/<milestone>-<topic>` (for example
   `feature/m1-kernel-modules`) or `fix/<topic>`.
2. Keep commits focused. Use the kernel's subject style:
   `area: imperative summary` (for example
   `modules/params: validate log level with custom param ops`).
   Explain *why* in the body.
3. Open a pull request against `main`. Describe the scope, how it was
   tested, and anything that could not be tested.
4. CI must pass before merging. `main` is kept buildable and green.
5. Update `CHANGELOG.md` (under *Unreleased*) and, when a milestone
   completes, `docs/ROADMAP.md`.

## Local checks

```bash
make                       # build all modules against the running kernel
make KDIR=/path/to/tree    # or against another kernel tree
make check                 # checkpatch (if available), W=1 and sparse builds
sudo tests/run_all.sh      # load each module and run its tests (needs root)
scripts/qemu-test.sh       # the same tests inside a QEMU VM (virtme-ng)
python3 -m unittest discover -s tests/lib   # tests for the test tooling
```

`docs/TESTING.md` explains what each check covers and what needs root, a
matching kernel, or KVM.

## Code style

- Kernel code follows `Documentation/process/coding-style.rst`. Run
  `scripts/checkpatch.sh` before sending a PR; CI runs it on every change.
- Every module uses `pr_fmt`, checks every allocation, and unwinds
  failures in reverse order with `goto` labels.
- Shell scripts use `set -euo pipefail`. Python targets 3.8+ and uses the
  standard library only.

## Honesty rules for docs and PRs

- Never mark a test as passed unless it actually ran and passed.
- Report benchmark numbers together with the machine they came from.
- Roadmap entries stay *Planned* until the code is merged.
