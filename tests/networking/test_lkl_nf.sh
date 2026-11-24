#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_nf: a netfilter hook per network namespace. Rules and counters are
# per namespace, new namespaces get a hook, unloading removes every hook.
# Traffic runs over an lkl_vnet pair between two namespaces.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
# shellcheck source=tests/lib/net.sh
source "$(dirname "$0")/../lib/net.sh"
require_root
net_preflight

M=lkl_nf
T=$(mktemp -d)
trap 'ns_cleanup; rm -rf "$T"' EXIT
ns_cleanup
unload $M
unload lkl_vnet
m=$(mark)
must_load lkl_vnet
must_load $M
check "load logged" kmsg_expect "$m" "^$M: loaded"

# nfstat NS NAME: a field of NS's /proc/net/lkl_nf
nfstat() { kv "$2" "$(nsx "$1" cat /proc/net/lkl_nf)"; }
# shellcheck disable=SC2317 # only called through check/check_fails
nfset() { nsx "$1" bash -c "echo '$2' > /proc/net/lkl_nf"; }

# udp_count FROM TO IP PORT N: send N datagrams from namespace FROM to
# IP:PORT, received in namespace TO; prints how many arrived.
udp_count() {
	local pid
	rm -f "$T/ready"
	nsx "$2" "${NETTOOL[@]}" udp-recv "$4" --ready "$T/ready" --wait 2 \
		--idle 0.5 --max "$5" >"$T/out" &
	pid=$!
	if ! wait_file "$T/ready"; then
		kill "$pid"
		return 1
	fi
	nsx "$1" "${NETTOOL[@]}" udp-send "$3" "$4" "$5" --size 32 >/dev/null
	wait "$pid"
	kv received "$(cat "$T/out")"
}

check "/proc/net/lkl_nf in the initial namespace" test -r /proc/net/lkl_nf
check "lkl_vnet pair between $NS0 and $NS1" pair_into_ns lklvnet0 lklvnet1
check "$NS1 has its own /proc/net/lkl_nf" nsx $NS1 test -w /proc/net/lkl_nf
check_eq "no rule by default" "0" "$(nfstat $NS1 drop_udp_port)"
check "ping across" nsx $NS0 ping -c 1 -W 2 $IP1

check_eq "UDP to 9999 delivered: no rule yet" "20" "$(udp_count $NS0 $NS1 $IP1 9999 20)"
check "set drop_udp_port 9999 in $NS1" nfset $NS1 "drop_udp_port 9999"
check_eq "rule visible in $NS1" "9999" "$(nfstat $NS1 drop_udp_port)"
before=$(nfstat $NS1 dropped)
check_eq "UDP to 9999 dropped in $NS1" "0" "$(udp_count $NS0 $NS1 $IP1 9999 20)"
check_eq "each drop counted" "$((before + 20))" "$(nfstat $NS1 dropped)"
check_eq "UDP to 9998 still delivered" "20" "$(udp_count $NS0 $NS1 $IP1 9998 20)"
check "ping unaffected" nsx $NS0 ping -c 1 -W 2 $IP1
check "seen counts every local IPv4 packet in $NS1" test "$(nfstat $NS1 seen)" -ge 61

# --- isolation -----------------------------------------------------------------------
check_eq "$NS0 has its own rule: none" "0" "$(nfstat $NS0 drop_udp_port)"
check_eq "$NS0 dropped nothing" "0" "$(nfstat $NS0 dropped)"
check_eq "UDP to 9999 into $NS0 delivered" "20" "$(udp_count $NS1 $NS0 $IP0 9999 20)"
check_eq "the initial namespace has no rule" "0" "$(nfstat - drop_udp_port)"

# --- configuration -------------------------------------------------------------------------
check_fails "port 65536 rejected" nfset $NS1 "drop_udp_port 65536"
check_fails "trailing junk rejected" nfset $NS1 "drop_udp_port 99x"
check_fails "unknown command rejected" nfset $NS1 "allow everything"
check_eq "rejected writes left the rule alone" "9999" "$(nfstat $NS1 drop_udp_port)"
check "reset" nfset $NS1 reset
check_eq "reset zeroes the counters" "0" "$(nfstat $NS1 dropped)"
check_eq "reset keeps the rule" "9999" "$(nfstat $NS1 drop_udp_port)"

ns_prepare $NS2
check "a namespace created after loading gets a hook" nsx $NS2 test -r /proc/net/lkl_nf
check_eq "and starts without a rule" "0" "$(nfstat $NS2 drop_udp_port)"

# --- unload with a rule active ---------------------------------------------------------------
check "rmmod with a rule active in $NS1" rmmod $M
check_fails "/proc/net/lkl_nf gone from $NS1" nsx $NS1 test -e /proc/net/lkl_nf
check_fails "and from the initial namespace" test -e /proc/net/lkl_nf
check_eq "UDP to 9999 delivered again" "20" "$(udp_count $NS0 $NS1 $IP1 9999 20)"

check "rmmod lkl_vnet" rmmod lkl_vnet
check "unload logged" kmsg_expect "$m" "^$M: unloaded$"
check "no kernel warnings" kmsg_clean "$m"
finish
