# SPDX-License-Identifier: GPL-2.0
# shellcheck shell=bash
#
# eBPF test helpers. Sourced after common.sh and net.sh.
#
# Without lklbpf, or on a kernel without BPF support, a test script skips
# as a whole and says why. LKL_EXPECT_BPF=1 (set by CI where the eBPF
# parts are built) turns that skip into a failure, so CI cannot pass by
# skipping.

LKLBPF=${LKL_USER_BUILD:-$LKL_ROOT/build}/lklbpf
BPF_PIN=
BPF_PROBE=

bpf_unavailable() {
	if [[ -n ${LKL_EXPECT_BPF:-} ]]; then
		bail "LKL_EXPECT_BPF is set, but $1"
	fi
	ok "eBPF tests # SKIP $1"
	finish
}

# bpf_preflight: lklbpf exists and the kernel runs XDP programs.
bpf_preflight() {
	[[ -x $LKLBPF ]] ||
		bpf_unavailable "lklbpf is not built (needs clang and libbpf >= 1.0; see ebpf/README.md)"
	BPF_PROBE=$("$LKLBPF" probe) || bpf_unavailable "lklbpf probe failed"
	[[ $BPF_PROBE == *xdp=1* ]] ||
		bpf_unavailable "the kernel does not run BPF programs ($BPF_PROBE)"
	diag "lklbpf probe: $BPF_PROBE"
}

# bpf_mount DIR: a private BPF filesystem for this script's pins. It is a
# plain mount, so it is still visible inside `ip netns exec`, which
# remounts only /sys.
bpf_mount() {
	mkdir -p "$1" || return 1
	mount -t bpf bpf "$1" || return 1
	BPF_PIN=$1/lkl
}

bpf_umount() {
	[[ -n $BPF_PIN ]] && umount "${BPF_PIN%/lkl}" 2>/dev/null
	return 0
}

# lklbpf ARGS: run lklbpf with this script's pin directory.
lklbpf() { "$LKLBPF" --pin "$BPF_PIN" "$@"; }

# lklbpf_ns NS ARGS: the same, inside network namespace NS (for attach).
lklbpf_ns() {
	local ns=$1
	shift
	nsx "$ns" "$LKLBPF" --pin "$BPF_PIN" "$@"
}

# xdp_mode NS DEV: "native", "generic" or "none", from ip link.
xdp_mode() {
	local line
	line=$(nsx "$1" ip -o link show dev "$2")
	if [[ $line == *" xdpgeneric"* ]]; then
		echo generic
	elif [[ $line == *" xdp"* ]]; then
		echo native
	else
		echo none
	fi
}
