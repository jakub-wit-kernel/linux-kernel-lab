# Stage 2 / M3: `lkl_events`, kernel-to-user interfaces

One event log (a ring of the most recent events), exposed through procfs,
sysfs and debugfs, so the three interfaces can be compared on the same
data.

| Interface | Entry | Mechanism | What it demonstrates |
|---|---|---|---|
| procfs | `/proc/lkl_lab/events` | `seq_file` iterator | Output larger than one page, served correctly across many small `read()`s |
| procfs | `/proc/lkl_lab/summary` | `proc_create_single` | The one-shot `single_open` pattern |
| procfs | `/proc/lkl_lab/control` | `proc_ops.proc_write` | Parsing commands from user space (`add TEXT`, `burst N`, `clear`) with `memdup_user_nul` and a size limit |
| procfs | `/proc/lkl_lab/snapshot` | `proc_ops` open/read/release | Binary records ([UAPI header](../../include/uapi/linux/lkl_events.h)) frozen at `open()` |
| sysfs | `/sys/kernel/lkl_lab/{count,capacity,threshold}` | `kobj_attribute` + attribute group | Validated read-write attributes |
| sysfs | `/sys/kernel/lkl_lab/alert` | `sysfs_notify()` | A **pollable** attribute: `poll(POLLPRI)` wakes when the alert changes |
| sysfs | `/sys/kernel/lkl_lab/debug/` | named group + `is_visible` | Attributes that exist only when `debug_attrs=1` |
| debugfs | `/sys/kernel/debug/lkl_lab/{adds,ring}` | `debugfs_create_u64`, `DEFINE_SHOW_ATTRIBUTE` | Developer-only internals, with no ABI promise |

## Design points

**Paging by sequence number.** The `seq_file` position is an event
*sequence number*, not a list index. When a read is split across calls
and events are added in between, the next call resumes at the next
unseen sequence number:
- an index-based iterator would show a shifted list, with duplicates
- events that were overwritten in the meantime are skipped, never repeated

A test reads the listing 5 bytes at a time while another thread keeps
adding, and requires strictly increasing sequence numbers.

**Consistent binary snapshot.** `/proc/lkl_lab/snapshot` copies the ring
under the lock in `open()` and serves reads from that private copy. Every
read on one file description therefore sees the same data, whatever
happens to the log in between. That is the usual way to give user space a
consistent view of changing kernel state.

**Pollable sysfs attribute.** User space opens `alert`, reads it once to
arm notification, then waits in `poll()` for `POLLPRI`. Any change of the
alert state (count crossing the threshold, a threshold write, a resize, a
clear) calls `sysfs_notify()`.

**Resizing.** Event `s` lives in slot `s % capacity`. A resize copies the
newest `min(count, new)` events into slot `s % new`, so the mapping holds
and readers in the middle of a listing continue correctly.

**Why there is no sysfs binary attribute.** The `bin_attribute` callback
signature changed several times between 6.13 and 6.16 (constification of
`struct bin_attribute`). A procfs file gives the same binary interface on
every supported kernel without version checks.

**Teardown order.** Every user-visible entry point (debugfs, procfs,
sysfs) is removed before the ring is freed. `proc_remove()` and
`debugfs_remove()` wait for callers already inside a handler.

## Run

```bash
make && cmake -S . -B build && cmake --build build
sudo tests/run_all.sh lkl_events
sudo insmod drivers/events/lkl_events.ko debug_attrs=1
echo "add hello" | sudo tee /proc/lkl_lab/control
cat /proc/lkl_lab/events /proc/lkl_lab/summary
```
