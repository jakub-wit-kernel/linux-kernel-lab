# eBPF and XDP (M7)

Three eBPF programs, built with clang and loaded with libbpf, plus a
command-line tool that attaches them and reads their maps.

| Program | Hook | What it does |
|---|---|---|
| [`xdp_filter`](progs/xdp_filter.bpf.c) | XDP | Drops IPv4 packets by source prefix (an LPM trie, so the longest prefix wins) or by UDP destination port (a hash map). Counts packets and bytes per XDP action, and packets per IP protocol, in per-CPU arrays. |
| [`tc_dscp`](progs/tc_dscp.bpf.c) | tc egress (tcx) | Rewrites the DSCP of UDP datagrams to one port. It keeps the ECN bits and fixes the IPv4 header checksum incrementally (`bpf_l3_csum_replace()`). |
| [`napi_hist`](progs/napi_hist.bpf.c) | tracepoint `napi:napi_poll` | Counts NAPI polls per network device and how many packets each poll processed. Uses CO-RE. |

Map layouts shared between the programs and user space are in
[`include/lklbpf.h`](include/lklbpf.h).

## Building

You need clang (it has the BPF target built in) and libbpf 1.0 or newer.
CMake builds the programs into `build/ebpf/*.bpf.o` and the tool into
`build/lklbpf`:

```sh
sudo apt install clang libelf-dev zlib1g-dev
scripts/build-libbpf.sh                  # pinned libbpf release in .tools/libbpf
cmake -S . -B build -DLKL_LIBBPF_DIR=$PWD/.tools/libbpf
cmake --build build
```

A distribution libbpf 1.0 or newer also works: leave out
`LKL_LIBBPF_DIR` and CMake finds it with pkg-config. Ubuntu 22.04 ships
libbpf 0.5, which is why `scripts/build-libbpf.sh` exists. Without
libbpf, CMake skips these targets and says why; the eBPF tests then skip
too.

## lklbpf

```sh
sudo mount -t bpf bpf /sys/fs/bpf        # usually already mounted
sudo build/lklbpf xdp attach eth1        # native if the driver supports XDP, else generic
sudo build/lklbpf xdp deny-port 9999
sudo build/lklbpf xdp deny-src 192.0.2.0/24
sudo build/lklbpf xdp stats              # pass=... drop=... ipproto_17=... port_9999=...
sudo build/lklbpf xdp detach

sudo build/lklbpf tc attach eth1 --port 5004 --dscp 46
sudo build/lklbpf napi attach && sudo build/lklbpf napi stats eth1
```

- **Pinning.** `attach` pins the program's link and maps under
  `/sys/fs/bpf/lkl/<feature>/` (change it with `--pin DIR`), then exits;
  the program stays attached. Map commands work through those pins.
  `detach` removes the pins, and releasing the last reference to the
  link detaches the program.
- **XDP modes.** `--mode native` requires driver support: veth has it,
  `lkl_vnet` does not, so attaching there in native mode fails with
  `EOPNOTSUPP`. `--mode generic` runs the program in the core receive
  path, for any device, after an `sk_buff` has been built. The default
  (`auto`) lets the kernel choose. `stats` and `ip link` show which mode
  is in use.
- **Namespaces.** `attach` finds the device in the caller's network
  namespace, so run it under `ip netns exec`. A BPF filesystem mounted
  outside `/sys` stays visible there; the tests mount a private one.

## CO-RE without vmlinux.h

`napi_hist` reads `ctx->napi->dev->name` and `ctx->work`. Their offsets
depend on the kernel version and configuration. Instead of a generated
`vmlinux.h`, the program declares only those fields, in structs marked
`preserve_access_index`. clang records each field access as a
relocation in the object's `.BTF.ext` section. When loading, libbpf
looks the types up by name in the running kernel's BTF
(`/sys/kernel/btf/vmlinux`) and patches the offsets. So one `.o` works
on kernels with different struct layouts;
[docs/TESTING.md](../docs/TESTING.md) lists the kernels it was actually
loaded on. The kernel needs
`CONFIG_DEBUG_INFO_BTF`; without it, `napi attach` refuses and the test
skips.

## Kernel requirements

| Feature | Needs |
|---|---|
| `xdp_filter`, generic mode | `CONFIG_BPF_SYSCALL` and XDP (any device) |
| `xdp_filter`, native mode | a driver with XDP support (veth here) |
| `tc_dscp` | tcx links: Linux 6.6+ |
| `napi_hist` | kernel BTF (`CONFIG_DEBUG_INFO_BTF`), tracepoints |

## Tests

`tests/ebpf/` runs everything between two network namespaces over
`lkl_vnet` and veth pairs:

- `test_xdp.sh`: generic and native modes, port and prefix drops, and
  longest-prefix matching. Counters must equal the traffic sent.
- `test_tc.sh`: all rewritten datagrams arrive (which shows the checksum
  is right) with the new TOS byte, and the ECN bits survive.
- `test_napi_trace.sh`: the tracepoint program's per-device totals equal
  `lkl_vnet`'s own counters exactly: one event per NAPI poll, and work
  summing to the packets received.

`benchmarks/xdp/run.sh` compares the cost of dropping at XDP (native and
generic), netfilter, and the UDP socket lookup.
