#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# napi_hist: a CO-RE tracepoint program on napi:napi_poll. Its per-device
# totals must agree exactly with lkl_vnet's own counters: one event per
# NAPI poll, and the polls' work adding up to the packets received.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
# shellcheck source=tests/lib/net.sh
source "$(dirname "$0")/../lib/net.sh"
# shellcheck source=tests/lib/bpf.sh
source "$(dirname "$0")/../lib/bpf.sh"
require_root
net_preflight
bpf_preflight
[[ $BPF_PROBE == *btf=1* ]] ||
	bpf_unavailable "no kernel BTF (/sys/kernel/btf/vmlinux): CO-RE needs CONFIG_DEBUG_INFO_BTF"
[[ $BPF_PROBE == *tracepoint=1* ]] ||
	bpf_unavailable "the kernel does not run tracepoint programs"
if ! mountpoint -q /sys/kernel/tracing; then
	mount -t tracefs nodev /sys/kernel/tracing || bail "cannot mount tracefs"
fi

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

check "attach napi_hist (CO-RE relocations resolved at load)" lklbpf napi attach
check_fails "attaching twice refused" lklbpf napi attach
sleep 0.2
polls0=$(ethstat $NS1 lklvnet1 napi_polls)
rx0=$(devstat $NS1 lklvnet1 rx_packets)
b0=$(lklbpf napi stats lklvnet1)

check "ping" nsx $NS0 ping -c 5 -i 0.1 -W 2 $IP1
check_eq "a UDP burst arrives" "2000" "$(udp_count $NS0 $NS1 $IP1 9200 2000)"
nsx $NS1 "${NETTOOL[@]}" tcp-recv 9201 --ready "$T/tcp" >"$T/tcp.out" &
rx=$!
wait_file "$T/tcp" || bail "TCP receiver did not start"
nsx $NS0 "${NETTOOL[@]}" tcp-send $IP1 9201 4194304 >/dev/null
wait $rx
sleep 0.5

polls1=$(ethstat $NS1 lklvnet1 napi_polls)
rx1=$(devstat $NS1 lklvnet1 rx_packets)
b1=$(lklbpf napi stats lklvnet1)
diag "lkl_vnet: napi_polls +$((polls1 - polls0)), rx_packets +$((rx1 - rx0))"
diag "napi_hist: $b1"
check "the tracepoint fired for lklvnet1" test "$(kvn polls "$b1")" -gt "$(kvn polls "$b0")"
check_eq "one event per NAPI poll of lklvnet1" "$((polls1 - polls0))" \
	"$(($(kvn polls "$b1") - $(kvn polls "$b0")))"
check_eq "the polls' work adds up to the packets lklvnet1 received" "$((rx1 - rx0))" \
	"$(($(kvn work "$b1") - $(kvn work "$b0")))"
check "the histogram has entries" test -n "$(kv hist "$b1")"
check_eq "no events for a device that does not exist" "polls=0 work=0 hist=" "$(lklbpf napi stats nosuchdev)"

check "detach" lklbpf napi detach
check_fails "stats fail once detached" lklbpf napi stats lklvnet1
check "rmmod lkl_vnet" rmmod lkl_vnet
check "no kernel warnings" kmsg_clean "$m"
finish
