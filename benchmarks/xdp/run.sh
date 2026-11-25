#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Where is the cheapest place to drop a packet? One CPU sends UDP
# datagrams over a veth pair from one namespace to another, and the
# receiver drops them at one of four points:
#
#   xdp-native   XDP_DROP in veth's NAPI poll, before an sk_buff exists
#   xdp-generic  XDP_DROP in the core receive path, after the sk_buff
#   netfilter    NF_DROP at IPv4 LOCAL_IN (the lab's lkl_nf module)
#   stack        no filter and no listener: dropped at the UDP socket
#                lookup (counted as Udp NoPorts)
#
# veth runs the receive side on the sending CPU (NAPI, or the backlog in
# softirq), so the send rate includes the cost of receiving up to the
# drop point. Numbers depend on the machine; compare rows, not runs.
#
#   sudo benchmarks/xdp/run.sh [PACKETS] [CPU]    (default 1000000, CPU 0)
#
# Needs the modules (make) and the user-space build with eBPF (cmake).
# Exits non-zero if a mode's drop point counted nothing.
LKL_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=tests/lib/common.sh
source "$LKL_ROOT/tests/lib/common.sh"
# shellcheck source=tests/lib/net.sh
source "$LKL_ROOT/tests/lib/net.sh"
# shellcheck source=tests/lib/bpf.sh
source "$LKL_ROOT/tests/lib/bpf.sh"
# No `set -e`: the helpers return non-zero on purpose (ns_cleanup when
# there is nothing to delete). Every step that matters checks itself.

N=${1:-1000000}
CPU=${2:-0}
FLOOD=${LKL_USER_BUILD:-$LKL_ROOT/build}/udpflood
SRC=10.97.0.1
DST=10.97.0.2
PORT=9999

[[ $EUID -eq 0 ]] || bail "run as root"
[[ -x $FLOOD ]] || bail "$FLOOD not built (cmake --build build)"
[[ -x $LKLBPF ]] || bail "$LKLBPF not built (needs libbpf; see ebpf/README.md)"
ko_path lkl_nf >/dev/null || bail "lkl_nf.ko not built (make)"
net_preflight

T=$(mktemp -d)
# shellcheck disable=SC2317 # called by the EXIT trap
cleanup() {
	lklbpf xdp detach >/dev/null 2>&1
	unload lkl_nf
	ns_cleanup
	bpf_umount
	rm -rf "$T"
}
trap cleanup EXIT
ns_cleanup
unload lkl_nf
bpf_mount "$T/bpf" || bail "cannot mount a BPF filesystem"
# setup: two namespaces joined by a veth pair, $SRC <-> $DST.
setup() {
	ns_prepare $NS0 || return 1
	ns_prepare $NS1 || return 1
	ip link add lklbench0 netns $NS0 type veth peer name lklbench1 netns $NS1 ||
		return 1
	nsx $NS0 ip addr add $SRC/24 dev lklbench0 || return 1
	nsx $NS1 ip addr add $DST/24 dev lklbench1 || return 1
	nsx $NS0 ip link set lklbench0 up || return 1
	nsx $NS1 ip link set lklbench1 up || return 1
	nsx $NS0 ping -c 1 -W 2 $DST >/dev/null
}
setup || bail "cannot set up the veth pair between $NS0 and $NS1"

# flood: send N datagrams from CPU $CPU in NS0; print udpflood's report.
flood() {
	if ! nsx $NS0 taskset -c "$CPU" "$FLOOD" $DST $PORT "$N"; then
		echo "run.sh: udpflood failed" >&2
		return 1
	fi
}

# Udp NoPorts in NS1 (the second Udp: line holds the values).
noports() { nsx $NS1 awk '/^Udp:/ { if (h) print $3; h = 1 }' /proc/net/snmp; }

rows=()
failed=0
record() {	# MODE "udpflood report" DROPPED
	local mode=$1 rep=$2 dropped=$3
	rows+=("$(printf '| %-11s | %9s | %7s | %6.2f | %9s |' "$mode" \
		"$(kv sent "$rep")" "$(kv seconds "$rep")" \
		"$(awk -v p="$(kv pps "$rep")" 'BEGIN { print p / 1e6 }')" "$dropped")")
	if ((dropped == 0)); then
		echo "run.sh: $mode: the drop point counted nothing" >&2
		failed=1
	fi
}

for mode in native generic; do
	lklbpf_ns $NS1 xdp attach lklbench1 --mode $mode >/dev/null ||
		bail "cannot attach XDP in $mode mode"
	lklbpf xdp deny-port $PORT || bail "cannot deny port $PORT"
	rep=$(flood) || exit 1
	record "xdp-$mode" "$rep" "$(kvn port_$PORT "$(lklbpf xdp stats)")"
	lklbpf xdp detach >/dev/null || bail "cannot detach XDP"
done

load lkl_nf || bail "cannot load lkl_nf"
nsx $NS1 bash -c "echo 'drop_udp_port $PORT' > /proc/net/lkl_nf" ||
	bail "cannot set the lkl_nf rule"
rep=$(flood) || exit 1
record netfilter "$rep" "$(kvn dropped "$(nsx $NS1 cat /proc/net/lkl_nf)")"
rmmod lkl_nf

before=$(noports)
rep=$(flood) || exit 1
after=$(noports)
record stack "$rep" "$((after - before))"

echo "# XDP drop benchmark: $N datagrams of 64 bytes per mode, sender on CPU $CPU"
echo "# kernel $(uname -r), $(nproc) CPUs, $(awk -F': ' '/model name/ { print $2; exit }' /proc/cpuinfo)"
echo
echo "| mode        |      sent | seconds |   Mpps | dropped   |"
echo "|-------------|-----------|---------|--------|-----------|"
printf '%s\n' "${rows[@]}"
exit "$failed"
