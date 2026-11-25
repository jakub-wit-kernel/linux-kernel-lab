#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# xdp_filter: generic XDP on lkl_vnet (no driver support) and native XDP on
# veth; drops by UDP port and by source prefix (longest match); counters
# that match the traffic sent; map updates while attached; detach.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
# shellcheck source=tests/lib/net.sh
source "$(dirname "$0")/../lib/net.sh"
# shellcheck source=tests/lib/bpf.sh
source "$(dirname "$0")/../lib/bpf.sh"
require_root
net_preflight
bpf_preflight

T=$(mktemp -d)
# shellcheck disable=SC2317 # called by the EXIT trap
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
check "ping before XDP" nsx $NS0 ping -c 1 -W 2 $IP1

# --- generic XDP on lkl_vnet ---------------------------------------------------------------
check_fails "native mode refused: lkl_vnet has no XDP support" \
	lklbpf_ns $NS1 xdp attach lklvnet1 --mode native
check_eq "native attempt left nothing attached" "none" "$(xdp_mode $NS1 lklvnet1)"
out=$(lklbpf_ns $NS1 xdp attach lklvnet1)
diag "$out"
check "auto mode attaches generic XDP" grep -q "mode=generic" <<<"$out"
check_eq "ip link shows xdpgeneric" "generic" "$(xdp_mode $NS1 lklvnet1)"
check_fails "attaching twice refused" lklbpf_ns $NS1 xdp attach lklvnet1

s0=$(lklbpf xdp stats)
check_eq "UDP to 9000 delivered through XDP_PASS" "20" "$(udp_count $NS0 $NS1 $IP1 9000 20)"
s1=$(lklbpf xdp stats)
check_eq "XDP counted exactly those 20 UDP packets" "20" \
	"$(($(kvn ipproto_17 "$s1") - $(kvn ipproto_17 "$s0")))"
check "and passed them" test $(($(kvn pass "$s1") - $(kvn pass "$s0"))) -ge 20

rx0=$(devstat $NS1 lklvnet1 rx_packets)
check "deny UDP port 9999" lklbpf xdp deny-port 9999
check_eq "UDP to 9999 dropped" "0" "$(udp_count $NS0 $NS1 $IP1 9999 20)"
s2=$(lklbpf xdp stats)
diag "after deny-port: $s2"
check_eq "the port's counter saw all 20" "20" "$(kvn port_9999 "$s2")"
check_eq "XDP_DROP rose by 20" "20" "$(($(kvn drop "$s2") - $(kvn drop "$s1")))"
check "generic XDP runs above the driver: lklvnet1 still received them" \
	test $(($(devstat $NS1 lklvnet1 rx_packets) - rx0)) -ge 20
check_eq "UDP to 9998 still delivered" "20" "$(udp_count $NS0 $NS1 $IP1 9998 20)"
check "allow port 9999 again" lklbpf xdp allow-port 9999
check_eq "UDP to 9999 delivered again" "20" "$(udp_count $NS0 $NS1 $IP1 9999 20)"
check_fails "allowing a port that is not denied fails" lklbpf xdp allow-port 9999
check_fails "port 0 rejected" lklbpf xdp deny-port 0
check_fails "port 65536 rejected" lklbpf xdp deny-port 65536

check "deny source 10.99.0.0/24" lklbpf xdp deny-src 10.99.0.0/24
check_fails "ping from $IP0 dropped" nsx $NS0 ping -c 2 -i 0.2 -W 1 $IP1
s3=$(lklbpf xdp stats)
check "the /24 entry counted the pings" test "$(kvn src_10.99.0.0/24 "$s3")" -ge 2
check "deny $IP0/32 as well" lklbpf xdp deny-src $IP0/32
nsx $NS0 ping -c 1 -W 1 $IP1 >/dev/null
s4=$(lklbpf xdp stats)
diag "after deny-src: $s4"
check "longest prefix wins: the /32 counted the next ping" test "$(kvn src_10.99.0.1/32 "$s4")" -ge 1
check_eq "and the /24 did not" "$(kvn src_10.99.0.0/24 "$s3")" "$(kvn src_10.99.0.0/24 "$s4")"
check "allow the /32" lklbpf xdp allow-src $IP0/32
check "allow the /24" lklbpf xdp allow-src 10.99.0.0/24
check "ping works again" nsx $NS0 ping -c 1 -W 2 $IP1
check_fails "prefix length 33 rejected" lklbpf xdp deny-src 10.99.0.0/33
check_fails "malformed address rejected" lklbpf xdp deny-src 10.99.0

check "detach" lklbpf xdp detach
check_eq "ip link shows no XDP program" "none" "$(xdp_mode $NS1 lklvnet1)"
check_fails "stats fail once detached" lklbpf xdp stats
check "traffic flows without XDP" nsx $NS0 ping -c 1 -W 2 $IP1

# --- native XDP on veth ---------------------------------------------------------------------
if ip link add lklveth0 netns $NS0 type veth peer name lklveth1 netns $NS1 2>/dev/null; then
	nsx $NS0 ip addr add 10.98.0.1/24 dev lklveth0
	nsx $NS1 ip addr add 10.98.0.2/24 dev lklveth1
	nsx $NS0 ip link set lklveth0 up
	nsx $NS1 ip link set lklveth1 up
	out=$(lklbpf_ns $NS1 xdp attach lklveth1)
	diag "$out"
	check "veth: auto mode attaches native XDP" grep -q "mode=native" <<<"$out"
	check_eq "ip link shows xdp (native)" "native" "$(xdp_mode $NS1 lklveth1)"
	check "veth: deny UDP port 9999" lklbpf xdp deny-port 9999
	check_eq "veth: UDP to 9999 dropped in the driver" "0" "$(udp_count $NS0 $NS1 10.98.0.2 9999 20)"
	check_eq "veth: the port's counter saw all 20" "20" "$(kvn port_9999 "$(lklbpf xdp stats)")"
	check_eq "veth: other ports delivered (XDP_PASS)" "20" "$(udp_count $NS0 $NS1 10.98.0.2 9998 20)"
	check "veth: detach" lklbpf xdp detach
	out=$(lklbpf_ns $NS1 xdp attach lklveth1 --mode generic)
	check "veth: generic mode can be forced" grep -q "mode=generic" <<<"$out"
	check_eq "ip link shows xdpgeneric" "generic" "$(xdp_mode $NS1 lklveth1)"
	check "veth: detach generic" lklbpf xdp detach
elif [[ -n ${LKL_EXPECT_BPF:-} ]]; then
	not_ok "veth native XDP: cannot create a veth pair"
else
	ok "veth native XDP # SKIP cannot create a veth pair (CONFIG_VETH?)"
fi

check "rmmod lkl_vnet" rmmod lkl_vnet
check "no kernel warnings" kmsg_clean "$m"
finish
