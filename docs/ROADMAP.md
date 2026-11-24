# Roadmap

The lab grows in five stages. The stages describe the **order of learning
and building**. They are not a historical record: this repository was
created on **2026-11-24**, and its git history shows when each piece was
actually written. A milestone is marked **Completed** only once its code
is merged to `main` and its CI checks have actually passed.

Status values: *Planned* · *In progress* · *Completed (date, PR)*.

## Stage 1: Kernel fundamentals and modules

| Milestone | Scope | Status |
|---|---|---|
| **M1** | Basic module; module parameters (standard types, arrays, validated custom `kernel_param_ops`); kernel logging (levels, `pr_fmt`, rate limiting, `printk_once`, hex dumps, dynamic debug); init/cleanup with ordered unwinding and failure injection. Plus the repository foundation: Kbuild tree, test harness, QEMU testing via virtme-ng, checkpatch/sparse/W=1, GitHub Actions. | *Completed (2026-11-24, PR #1)* |

## Stage 2: Character drivers and kernel interfaces

| Milestone | Scope | Status |
|---|---|---|
| M2 | Character device: dynamic major/minor, `cdev`, `class`/`device`, read/write with `copy_*_user`, `llseek`, ioctl with a versioned UAPI header, a user-space library and CLI. (The standalone `kmsgq` project already covers a message-queue driver; M2 focuses on a seekable, buffer-backed device and the UAPI and versioning discipline.) | *Completed (2026-11-24, PR #3)* |
| M3 | procfs (`seq_file`, `proc_ops`, a binary snapshot), sysfs (attribute groups, `kobject`, `is_visible`, `sysfs_notify` + poll), plus debugfs for diagnostics. sysfs binary attributes were left out on purpose: their API changed repeatedly between 6.13 and 6.16, and procfs provides the binary interface instead. | *In progress* |

## Stage 3: Concurrency, memory management, performance

| Milestone | Scope | Status |
|---|---|---|
| M4 | Spinlocks (incl. IRQ-safe variants), mutexes, atomics and `refcount_t`, RCU basics, wait queues, workqueues and delayed work, kthreads. Tested under a QEMU kernel built with lockdep, `DEBUG_ATOMIC_SLEEP`, KASAN and KCSAN. | *Planned* |
| M5 | `kmalloc`/`kfree`, `vmalloc`, slab caches (`kmem_cache`), page allocation (`alloc_pages`), `mmap` of kernel memory to user space, memory accounting and leak checks with kmemleak. | *Planned* |

## Stage 4: Networking, eBPF and XDP

| Milestone | Scope | Status |
|---|---|---|
| M6 | Virtual network device (`net_device_ops`, NAPI-style RX), packet processing with `sk_buff`, network namespaces for isolated tests, a netfilter hook module. | *Planned* |
| M7 | eBPF with libbpf and CO-RE: XDP packet filtering and counting, tc and tracepoint programs, BPF maps for kernel/user communication, an XDP benchmark using veth pairs in namespaces. | *Planned* |

## Stage 5: Advanced optimization and production-quality testing

| Milestone | Scope | Status |
|---|---|---|
| M8 | ftrace and tracepoints, perf profiling, lock contention analysis (`lock_stat`), memory optimization, a reproducible benchmark suite; SmartNIC/offload concepts (AF_XDP, XDP offload models) documented with experiments where hardware-free emulation allows. | *Planned* |

## How a milestone is delivered

1. Work happens on a `feature/mN-*` branch.
2. A pull request is opened against `main` with the milestone's scope,
   test evidence and known limitations.
3. CI must pass: build, static analysis, tests on the runner kernel and in
   QEMU.
4. After review, the PR is merged. This file and `CHANGELOG.md` record the
   completion date and PR number.
