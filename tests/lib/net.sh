# SPDX-License-Identifier: GPL-2.0
# shellcheck shell=bash
#
# Network namespace helpers for the networking tests. Sourced after
# common.sh. Each test runs its traffic between two namespaces so the
# host's own networking is never involved.

NS0=lkl_ns0
NS1=lkl_ns1
NS2=lkl_ns2
IP0=10.99.0.1
IP1=10.99.0.2
# shellcheck disable=SC2034 # used by the scripts that source this file
NETTOOL=(python3 "$LKL_ROOT/tests/lib/nettool.py")

net_preflight() {
	local c
	for c in ip ping ethtool; do
		command -v "$c" >/dev/null ||
			bail "required command '$c' not found (packages: iproute2, iputils-ping, ethtool)"
	done
	if ! ip netns add lkl_probe 2>/dev/null; then
		bail "cannot create network namespaces (ip netns add)"
	fi
	ip netns del lkl_probe
}

# nsx NS CMD...: run CMD inside namespace NS ("-": the initial namespace).
nsx() {
	local ns=$1
	shift
	if [[ $ns == - ]]; then
		"$@"
	else
		ip netns exec "$ns" "$@"
	fi
}

ns_cleanup() {
	local ns
	for ns in "$NS0" "$NS1" "$NS2"; do
		ip netns del "$ns" 2>/dev/null
	done
	return 0
}

# ns_prepare NS: create it, loopback up, IPv6 off for every device that
# arrives later (keeps the packet counts to what the test itself sends).
ns_prepare() {
	ip netns add "$1" || return 1
	nsx "$1" ip link set lo up
	if [[ -d /proc/sys/net/ipv6 ]]; then
		nsx "$1" sysctl -qw net.ipv6.conf.all.disable_ipv6=1 \
			net.ipv6.conf.default.disable_ipv6=1
	fi
}

# pair_into_ns DEV0 DEV1: DEV0 -> NS0 as IP0, DEV1 -> NS1 as IP1, both up.
pair_into_ns() {
	ns_prepare "$NS0" || return 1
	ns_prepare "$NS1" || return 1
	ip link set "$1" netns "$NS0" || return 1
	ip link set "$2" netns "$NS1" || return 1
	nsx "$NS0" ip addr add "$IP0/24" dev "$1" || return 1
	nsx "$NS1" ip addr add "$IP1/24" dev "$2" || return 1
	nsx "$NS0" ip link set "$1" up || return 1
	nsx "$NS1" ip link set "$2" up
}

# devstat NS DEV COUNTER: standard counter from sysfs (dev_get_stats()).
devstat() { nsx "$1" cat "/sys/class/net/$2/statistics/$3"; }

# ethstat NS DEV NAME: driver counter from ethtool -S.
ethstat() {
	nsx "$1" ethtool -S "$2" | awk -v k="$3:" '$1 == k { print $2 }'
}

# flow_ok NS_A DEV_A NS_B DEV_B: every frame A sent was received, dropped
# or is still queued at B.
flow_ok() {
	local tx rx drop q
	tx=$(devstat "$1" "$2" tx_packets)
	rx=$(devstat "$3" "$4" rx_packets)
	drop=$(devstat "$3" "$4" rx_dropped)
	q=$(ethstat "$3" "$4" rx_queue_len)
	diag "$2 -> $4: tx=$tx rx=$rx rx_dropped=$drop queued=$q"
	((tx == rx + drop + q))
}

# wait_file PATH: wait up to 10 s for a file to appear.
wait_file() {
	local i
	for ((i = 0; i < 100; i++)); do
		[[ -e $1 ]] && return 0
		sleep 0.1
	done
	return 1
}

# wait_link [NS] DEV: wait up to 15 s for DEV to appear (namespace
# teardown is asynchronous).
wait_link() {
	local i cmd=(ip link show)
	if (($# == 2)); then
		cmd=(ip netns exec "$1" ip link show)
		shift
	fi
	for ((i = 0; i < 150; i++)); do
		"${cmd[@]}" "$1" >/dev/null 2>&1 && return 0
		sleep 0.1
	done
	return 1
}

# kv NAME "k=v k=v ...": print the value of NAME (empty if absent).
kv() { grep -oE "(^| )$1=[^ ]*" <<<"$2" | head -n 1 | cut -d= -f2; }

# kvn NAME "k=v ...": like kv, but 0 when absent (for arithmetic).
kvn() {
	local v
	v=$(kv "$1" "$2")
	echo "${v:-0}"
}

# udp_recv_send FROM TO IP PORT N [udp-recv options...]: start a receiver
# for N datagrams in namespace TO, send N from namespace FROM to IP:PORT,
# print the receiver's report ("received=... bytes=... [tos=...]"). The
# receiver gives up 2 s after the last datagram (or without any). Uses
# the caller's temporary directory $T.
udp_recv_send() {
	local from=$1 to=$2 ip=$3 port=$4 n=$5 pid
	shift 5
	rm -f "${T:?}/udp.ready"
	nsx "$to" "${NETTOOL[@]}" udp-recv "$port" --ready "$T/udp.ready" \
		--wait 2 --idle 0.5 --max "$n" "$@" >"$T/udp.out" &
	pid=$!
	if ! wait_file "$T/udp.ready"; then
		kill "$pid"
		return 1
	fi
	nsx "$from" "${NETTOOL[@]}" udp-send "$ip" "$port" "$n" --size 32 \
		${UDP_SEND_TOS:+--tos "$UDP_SEND_TOS"} >/dev/null
	wait "$pid"
	cat "$T/udp.out"
}

# udp_count FROM TO IP PORT N: how many of N datagrams arrived.
udp_count() { kvn received "$(udp_recv_send "$@")"; }
