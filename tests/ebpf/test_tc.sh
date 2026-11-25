#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# tc_dscp: a tcx egress program rewrites the DSCP of UDP datagrams to one
# port. The receiver sees the new TOS byte with the ECN bits kept, and
# receives every datagram, which shows the IPv4 checksum was fixed.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
# shellcheck source=tests/lib/net.sh
source "$(dirname "$0")/../lib/net.sh"
# shellcheck source=tests/lib/bpf.sh
source "$(dirname "$0")/../lib/bpf.sh"
require_root
net_preflight
bpf_preflight
IFS=. read -r kmaj kmin _ <<<"$(uname -r)"
if ((kmaj < 6 || (kmaj == 6 && kmin < 6))); then
	bpf_unavailable "tcx needs Linux 6.6 or later (running $(uname -r))"
fi

T=$(mktemp -d)
cleanup() {
	ns_cleanup
	bpf_umount
	rm -rf "$T"
}
trap cleanup EXIT
ns_cleanup
unload lkl_nf
unload lkl_vnet
m=$(mark)
must_load lkl_vnet
bpf_mount "$T/bpf" || bail "cannot mount a BPF filesystem"
check "lkl_vnet pair between $NS0 and $NS1" pair_into_ns lklvnet0 lklvnet1
check "ping across" nsx $NS0 ping -c 1 -W 2 $IP1

r=$(udp_recv_send $NS0 $NS1 $IP1 9100 20 --tos)
check_eq "before: TOS 0 on the wire" "0:20" "$(kv tos "$r")"

out=$(lklbpf_ns $NS0 tc attach lklvnet0 --port 9100 --dscp 46)
diag "$out"
check "attached at lklvnet0 egress" grep -q "attached to lklvnet0 egress" <<<"$out"
check_fails "attaching twice refused" lklbpf_ns $NS0 tc attach lklvnet0 --port 9100 --dscp 46

r=$(udp_recv_send $NS0 $NS1 $IP1 9100 50 --tos)
check_eq "all 50 datagrams to 9100 arrived: the checksum is right" "50" "$(kv received "$r")"
check_eq "each carries DSCP 46 (TOS 184)" "184:50" "$(kv tos "$r")"
r=$(udp_recv_send $NS0 $NS1 $IP1 9101 50 --tos)
check_eq "other ports keep TOS 0" "0:50" "$(kv tos "$r")"
r=$(UDP_SEND_TOS=1 udp_recv_send $NS0 $NS1 $IP1 9100 20 --tos)
check_eq "ECN bits kept: TOS 0x01 becomes 185" "185:20" "$(kv tos "$r")"

s=$(lklbpf tc stats)
diag "tc stats: $s"
check_eq "matched: 70 datagrams to 9100" "70" "$(kvn matched "$s")"
check_eq "rewritten: all 70" "70" "$(kvn rewritten "$s")"
check_eq "no helper errors" "0" "$(kvn errors "$s")"
check "seen counts every egress packet" test "$(kvn seen "$s")" -ge 120

check "detach" lklbpf tc detach
r=$(udp_recv_send $NS0 $NS1 $IP1 9100 20 --tos)
check_eq "after detach: TOS 0 again" "0:20" "$(kv tos "$r")"
check_fails "DSCP 64 rejected" lklbpf_ns $NS0 tc attach lklvnet0 --port 9100 --dscp 64
check_fails "port 0 rejected" lklbpf_ns $NS0 tc attach lklvnet0 --port 0 --dscp 46
check_fails "unknown device rejected" lklbpf_ns $NS0 tc attach nosuchdev --port 9100 --dscp 46

check "rmmod lkl_vnet" rmmod lkl_vnet
check "no kernel warnings" kmsg_clean "$m"
finish
