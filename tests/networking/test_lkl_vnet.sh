#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_vnet: device pairs and carrier, traffic between two namespaces,
# NAPI receive and flow control, MTU, accounting, namespace teardown,
# and rmmod with a device still inside a namespace.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
# shellcheck source=tests/lib/net.sh
source "$(dirname "$0")/../lib/net.sh"
require_root
net_preflight

M=lkl_vnet
D0=lklvnet0
D1=lklvnet1
T=$(mktemp -d)
trap 'ns_cleanup; rm -rf "$T"' EXIT
ns_cleanup
unload lkl_nf
unload $M
m=$(mark)
# The smallest ring: a 64 KiB TCP send (about 45 frames after GSO) fills
# it, so the flow-control path runs.
must_load $M rx_ring=16
check "load logged" kmsg_expect "$m" "^$M: loaded: 1 pair\(s\), rx_ring 16, $D0\.\.$D1$"

# --- the pair ----------------------------------------------------------------------------
check "$D0 exists" test -e /sys/class/net/$D0
check "$D1 exists" test -e /sys/class/net/$D1
check_fails "exactly one pair" test -e /sys/class/net/lklvnet2
check_eq "ethtool names the driver" "driver: $M" "$(ethtool -i $D0 | head -n 1)"
check_eq "$D0's peer is $D1" "$(cat /sys/class/net/$D1/ifindex)" "$(ethstat - $D0 peer_ifindex)"
check_eq "$D1's peer is $D0" "$(cat /sys/class/net/$D0/ifindex)" "$(ethstat - $D1 peer_ifindex)"
check_eq "stable, locally administered MAC" "02:6c:6b:00:00:01" "$(cat /sys/class/net/$D0/address)"
check_fails "ip link del refused: the module owns the devices" ip link del $D0

ip link set $D0 up
check_eq "one end up: no carrier" "0" "$(cat /sys/class/net/$D0/carrier)"
ip link set $D1 up
check_eq "both ends up: carrier on $D0" "1" "$(cat /sys/class/net/$D0/carrier)"
check_eq "both ends up: carrier on $D1" "1" "$(cat /sys/class/net/$D1/carrier)"
ip link set $D1 down
check_eq "peer down: carrier off" "0" "$(cat /sys/class/net/$D0/carrier)"
ip link set $D0 down

# --- traffic between two namespaces ----------------------------------------------------
check "pair moved into $NS0/$NS1 and configured" pair_into_ns $D0 $D1
check_fails "$D0 left the initial namespace" test -e /sys/class/net/$D0
check "ping $IP1 from $NS0" nsx $NS0 ping -c 3 -i 0.2 -W 2 $IP1
sleep 0.3
check "accounting $D0 -> $D1" flow_ok $NS0 $D0 $NS1 $D1
check "accounting $D1 -> $D0" flow_ok $NS1 $D1 $NS0 $D0

nsx $NS1 "${NETTOOL[@]}" udp-recv 7000 --ready "$T/udp" --wait 5 --idle 1 >"$T/udp.out" &
rx=$!
wait_file "$T/udp" || bail "UDP receiver did not start"
sent=$(nsx $NS0 "${NETTOOL[@]}" udp-send $IP1 7000 50000 --size 64)
wait $rx
got=$(cat "$T/udp.out")
diag "UDP flood: $sent, $got"
check "UDP flood reached the receiver" test "$(kv received "$got")" -gt 0

nsx $NS1 "${NETTOOL[@]}" tcp-recv 7001 --ready "$T/tcp" >"$T/tcp.out" &
rx=$!
wait_file "$T/tcp" || bail "TCP receiver did not start"
sent=$(nsx $NS0 "${NETTOOL[@]}" tcp-send $IP1 7001 33554432)
wait $rx
got=$(cat "$T/tcp.out")
check_eq "TCP: 32 MiB arrived" "33554432" "$(kv bytes "$got")"
check_eq "TCP: contents intact (SHA-256)" "$(kv sha256 "$sent")" "$(kv sha256 "$got")"

sleep 0.5
check "accounting $D0 -> $D1 after bulk traffic" flow_ok $NS0 $D0 $NS1 $D1
check "accounting $D1 -> $D0 after bulk traffic" flow_ok $NS1 $D1 $NS0 $D0
check_eq "$D1's ring is empty at rest" "0" "$(ethstat $NS1 $D1 rx_queue_len)"
polls=$(ethstat $NS1 $D1 napi_polls)
stops=$(ethstat $NS0 $D0 queue_stops)
wakes=$(ethstat $NS0 $D0 queue_wakes)
diag "$D1 NAPI: polls=$polls budget_exhausted=$(ethstat $NS1 $D1 napi_budget_exhausted)"
diag "$D0 tx queue: stops=$stops wakes=$wakes"
check "NAPI polled on $D1" test "$polls" -gt 0
check "flow control engaged: $D0's queue was stopped" test "$stops" -gt 0
check_eq "every stop was followed by a wake" "$stops" "$wakes"

# --- MTU ------------------------------------------------------------------------------
nsx $NS0 ip link set $D0 mtu 9000
dropped=$(devstat $NS1 $D1 rx_dropped)
check_fails "4000-byte ping fails while $D1 has MTU 1500" nsx $NS0 ping -c 1 -W 1 -s 4000 $IP1
check "the frame was dropped at $D1 for its MTU" test "$(ethstat $NS1 $D1 rx_dropped_mtu)" -gt 0
check "and counted in $D1's rx_dropped" test "$(devstat $NS1 $D1 rx_dropped)" -gt "$dropped"
nsx $NS1 ip link set $D1 mtu 9000
check "4000-byte ping works with MTU 9000 at both ends" nsx $NS0 ping -c 1 -W 2 -s 4000 $IP1
check_fails "MTU 9001 rejected" nsx $NS0 ip link set $D0 mtu 9001
nsx $NS0 ip link set $D0 mtu 1500
nsx $NS1 ip link set $D1 mtu 1500

# --- the peer goes away and comes back -------------------------------------------------
nsx $NS1 ip link set $D1 down
check_eq "peer down in $NS1: no carrier on $D0 in $NS0" "0" "$(nsx $NS0 cat /sys/class/net/$D0/carrier)"
check_fails "no ping while the peer is down" nsx $NS0 ping -c 1 -W 1 $IP1
nsx $NS1 ip link set $D1 up
check "ping again once the peer is back" nsx $NS0 ping -c 2 -i 0.2 -W 2 $IP1
sleep 0.3
check "accounting $D0 -> $D1 after the outage" flow_ok $NS0 $D0 $NS1 $D1
check "accounting $D1 -> $D0 after the outage" flow_ok $NS1 $D1 $NS0 $D0

# --- teardown ---------------------------------------------------------------------------
ip netns del $NS1
check "deleting $NS1 returns $D1 to the initial namespace" wait_link $D1
ip netns del $NS0
check "deleting $NS0 returns $D0 as well" wait_link $D0

ns_prepare $NS2
ip link set $D1 netns $NS2
check "rmmod with $D1 inside $NS2" rmmod $M
check_fails "$D1 is gone from $NS2" nsx $NS2 ip link show $D1
check_fails "$D0 is gone" test -e /sys/class/net/$D0
ip netns del $NS2

# --- parameters ------------------------------------------------------------------------
check_fails "pairs=0 rejected" load $M pairs=0
check_fails "pairs=5 rejected" load $M pairs=5
check_fails "rx_ring=8 rejected" load $M rx_ring=8
check "pairs=2 loads" load $M pairs=2
check_eq "lklvnet2's peer is lklvnet3" "$(cat /sys/class/net/lklvnet3/ifindex)" "$(ethstat - lklvnet2 peer_ifindex)"
check "rmmod" rmmod $M
check "unload logged" kmsg_expect "$m" "^$M: unloaded$"
check "no kernel warnings" kmsg_clean "$m"
finish
