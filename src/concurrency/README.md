# Concurrency experiments (`lkl_sync`)

`lkl_sync` runs one concurrency experiment at a time on request. Each one
starts N kernel threads behind a start barrier, runs a workload, and
checks an invariant that a synchronization bug would break.

```sh
sudo insmod src/concurrency/lkl_sync.ko
echo "spin 4 200000" | sudo tee /sys/kernel/lkl_sync/run   # name threads iterations
cat /sys/kernel/lkl_sync/result
cat /sys/kernel/lkl_sync/detectors      # lockdep / KASAN / DEBUG_ATOMIC_SLEEP built in?
```

The write blocks until the experiment finishes. It fails with `EIO` if
the invariant was violated, `EINVAL` for bad input, and `ETIMEDOUT` if
the run took over 120 s (the threads are then aborted). The `result`
line is written in every case.

| Experiment | What runs | Invariant |
|---|---|---|
| `spin` | threads increment under `spin_lock_bh`; a timer increments the same counter under `spin_lock` in softirq context | counter = thread increments + timer ticks |
| `atomic` | `atomic64_inc` | exact total |
| `racy` | `WRITE_ONCE(x, READ_ONCE(x) + 1)` with no lock | none: reports how many updates were lost |
| `mutex` | increments under a mutex, with an occupancy counter | exact total, never two holders |
| `rcu` | thread 0 publishes new `{a, b}` versions with `rcu_replace_pointer` and frees old ones with `kfree_rcu`; the others read under `rcu_read_lock` | `a + b` is constant in every read |
| `waitq` | half the threads produce `1..N` into a 16-slot ring, the rest consume; two wait queues | sum consumed = sum produced |
| `workqueue` | N work items on a private unbound workqueue, flushed; then 10 delayed works, every other one cancelled with `cancel_delayed_work_sync` | all N ran; exactly 5 cancelled, the other 5 ran |

Limits: 1–32 threads, 1–10,000,000 iterations (work items: 100,000).
`rcu` and `waitq` need at least 2 threads.

## Detector demonstrations

Two experiments contain a deliberate bug. They run only when the
matching detector is built into the kernel, and are refused with
`EOPNOTSUPP` otherwise: on a normal kernel they could really deadlock or
corrupt memory.

| Experiment | Bug | Expected report |
|---|---|---|
| `lockdep_softirq` | takes the lock the timer uses in softirq context with plain `spin_lock` in process context | lockdep: `inconsistent {IN-SOFTIRQ-W} -> {SOFTIRQ-ON-W} usage` (needs `CONFIG_PROVE_LOCKING`) |
| `kasan_rcu_uaf` | the `rcu` experiment with `kfree` instead of `kfree_rcu`, so readers touch freed memory | `BUG: KASAN: slab-use-after-free` (needs `CONFIG_KASAN`) |

`tests/debug/test_detectors.sh` triggers both and requires the reports.
It runs after every other test, because lockdep turns itself off after
its first report and KASAN taints the kernel.

## Debug kernel

`scripts/build-debug-kernel.sh 6.12 kernel` downloads Linux 6.12 and
builds it with virtme-ng's VM config plus lockdep, `DEBUG_ATOMIC_SLEEP`,
KASAN and `DEBUG_LIST`. Then:

```sh
LKL_EXPECT_DETECTORS=1 LKL_QEMU_MEM=2G \
    scripts/qemu-test.sh kernel/arch/x86/boot/bzImage "$PWD/kernel"
```

CI does this in the `test-debug-kernel` job (the kernel build is cached).
