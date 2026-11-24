# Stage 1 / M1: kernel module fundamentals

Four small modules, each focused on one topic, each with a test script
that loads it, asserts on its behaviour through `/sys` and the kernel log,
and unloads it.

| Module | Topic | What the tests prove |
|---|---|---|
| [`hello/lkl_hello.c`](hello/lkl_hello.c) | Module skeleton | `__init`/`__exit`, `pr_fmt` prefix, `KERN_INFO` level, `modinfo` metadata and vermagic, `EEXIST` on double load, clean unload |
| [`params/lkl_params.c`](params/lkl_params.c) | Module parameters | `int`/`charp`/`bool`/array parameters; sysfs permissions (0444 vs 0644); custom `kernel_param_ops` that reject out-of-range or unknown values at load time *and* at runtime, leaving the old value intact; init-time validation of array elements; defaults |
| [`logging/lkl_logging.c`](logging/lkl_logging.c) | Kernel logging | Each `pr_*` lands at the right syslog level (checked from `/dev/kmsg` records); `pr_debug` silent until `dyndbg=+p`; `pr_info_once` fires once; `print_hex_dump` format; a private `ratelimit_state` lets exactly 5 of 20 messages through, and a runtime burst through a write-only parameter is fully suppressed |
| [`lifecycle/lkl_lifecycle.c`](lifecycle/lkl_lifecycle.c) | Init and cleanup | Four resources (memory, array, sysfs group, self-rearming timer) acquired in order and released in reverse; `fail_at=N` forces step *N* to fail, and the tests check, for every N, that exactly steps 1..N-1 are released in reverse order, the load fails with `-ENOMEM`, and nothing is left in sysfs; 20 load/unload cycles with a live timer; no kernel warnings |

## Design points worth reading

- **Validated parameters** (`lkl_params.c`). `module_param_cb` with a
  custom `.set` validates before delegating the store to
  `param_set_uint`. The kernel serializes sysfs parameter writes
  (`kernel_param_lock`), so the setter needs no lock of its own; readers
  that can race with a write use `READ_ONCE`. The setter never modifies
  its `const char *val` input, not even to strip the newline `echo` adds.
- **Deterministic rate limiting** (`lkl_logging.c`). A private
  `DEFINE_RATELIMIT_STATE` keeps other kernel activity from consuming the
  budget, which is what makes "exactly 5 of 20" testable.
- **Teardown order** (`lkl_lifecycle.c`).
  - The timer is shut down with `timer_shutdown_sync()` (6.2+, the
    supported floor), which also prevents it from re-arming, before the
    memory it writes is freed.
  - The sysfs group is removed before the state its `show()` callbacks
    read.
  - `kmalloc_array` checks the size multiplication for overflow.
- **API drift** is isolated in [`../include/lkl_compat.h`](../include/lkl_compat.h)
  and handled by feature detection (`#ifdef timer_container_of` for the
  6.16 rename), not version-number checks.

## Run

```bash
make                                   # from the repository root
sudo tests/run_all.sh                  # all module tests on this kernel
sudo tests/run_all.sh lifecycle        # one test script
scripts/qemu-test.sh                   # the same tests inside a QEMU VM
```
