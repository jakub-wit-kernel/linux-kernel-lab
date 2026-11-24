# Networking (M6)

| Module | What it shows |
|---|---|
| [`vnet/lkl_vnet`](vnet/lkl_vnet.c) | A pair of virtual Ethernet devices: `net_device_ops`, a NAPI receive path, transmit flow control, carrier that follows the peer, `ethtool` statistics, frames handed across network namespaces |
| [`netfilter/lkl_nf`](netfilter/lkl_nf.c) | A netfilter hook in every network namespace (`pernet_operations`), with its own rule, counters and `/proc/net/lkl_nf` file in each |

The tests (`tests/networking/`) never touch the host's own networking.
They move the two ends of a pair into two network namespaces, give them
`10.99.0.1` and `10.99.0.2`, and send traffic between them with
`tests/lib/nettool.py`. They need `ip`, `ping` and `ethtool`.

## `lkl_vnet`

```sh
sudo insmod networking/vnet/lkl_vnet.ko pairs=1 rx_ring=256
ip link show lklvnet0        # peer: lklvnet1
ethtool -S lklvnet0
```

Parameters: `pairs` (1–4) and `rx_ring` (16–4096 frames per device).

**Transmit** (`ndo_start_xmit` on A). If B is down, the frame is
dropped (A's `tx_dropped`). Otherwise the frame goes through what
`__dev_forward_skb()` does: a check against B's MTU, a scrub when it
crosses into another namespace, and `eth_type_trans()` for B. It is then
queued on B's receive ring, and B's NAPI is scheduled. When B's ring is
full, A stops its own transmit queue. The qdisc above A then holds the
backlog instead of the driver dropping it.

**Receive** (NAPI poll on B, in softirq). B dequeues up to `budget`
frames and passes them to `napi_gro_receive()`. It wakes A's queue once
its ring has drained to half. A stop and a poll can race: a barrier pair
plus a re-check after stopping (`smp_mb__after_atomic()` in transmit,
`smp_mb()` in poll) guarantees the queue is never left stopped with an
empty ring.

**Carrier** is on only while both ends are up, so `ip link` shows
`NO-CARRIER` on one end when the other is down, even if the other end is
in a different namespace.

**Lifetime.** The module owns the devices. `ip link del` is refused,
deleting a namespace hands the device back to the initial namespace, and
rmmod removes the devices wherever they are.

**Accounting**, per direction, at any moment:

    A.tx_packets == B.rx_packets + B.rx_dropped + B.rx_queue_len

`rx_dropped` counts frames B refused because of its MTU and frames
flushed from its ring when it went down. The tests check this identity
after each traffic phase.

`ethtool -S` counters: `peer_ifindex`, `rx_queue_len`, `rx_dropped_mtu`,
`napi_polls`, `napi_budget_exhausted`, `queue_stops`, `queue_wakes`.

## `lkl_nf`

```sh
sudo insmod networking/netfilter/lkl_nf.ko
cat /proc/net/lkl_nf                              # drop_udp_port=0 seen=0 dropped=0
echo "drop_udp_port 9999" | sudo tee /proc/net/lkl_nf
echo reset | sudo tee /proc/net/lkl_nf            # zero the counters
```

`register_pernet_subsys()` runs the module's init for every existing
network namespace, and again for every namespace created later. Each
namespace gets an IPv4 `LOCAL_IN` hook with its own state
(`net_generic()`) and its own `/proc/net/lkl_nf`
(`proc_create_net_single_write()`). The hook counts every local IPv4
packet. It drops UDP datagrams to the configured port, reading the UDP
header with `skb_header_pointer()` because it may not be in the linear
part of the buffer. A rule set in one namespace has no effect in any
other. Unloading unregisters every namespace's hook.
