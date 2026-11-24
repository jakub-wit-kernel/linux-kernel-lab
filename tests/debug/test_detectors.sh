#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Prove that the kernel's bug detectors are live by triggering one known
# bug for each and requiring the report. Runs LAST (tests/run_all.sh puts
# tests/debug/ at the end): lockdep switches itself off after its first
# report, and KASAN taints the kernel.
#
# Skips on kernels without the detectors, unless LKL_EXPECT_DETECTORS=1
# (set by the debug-kernel CI job), in which case missing detectors fail.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_sync
S=/sys/kernel/$M
unload $M
must_load $M
detectors=$(cat $S/detectors)
diag "detectors: $detectors"

if [[ $detectors == *lockdep=0*kasan=0* ]]; then
	if [[ -n ${LKL_EXPECT_DETECTORS:-} ]]; then
		not_ok "debug kernel expected, but lockdep and KASAN are both off"
	else
		ok "detector demonstrations # SKIP kernel without lockdep and KASAN"
	fi
	rmmod $M
	finish
fi

if [[ $detectors == *lockdep=1* ]]; then
	m=$(mark)
	echo "lockdep_softirq 1 1" > $S/run
	check "lockdep reports the softirq-unsafe lock use" \
		kmsg_expect "$m" "inconsistent lock state" "inconsistent \{IN-SOFTIRQ-W\} -> \{SOFTIRQ-ON-W\} usage"
	# Function names may be inlined away; the module name is always there.
	check "the report is about lkl_sync" kmsg_expect "$m" "inconsistent lock state" "lkl_sync"
elif [[ -n ${LKL_EXPECT_DETECTORS:-} ]]; then
	not_ok "debug kernel expected lockdep (CONFIG_PROVE_LOCKING)"
fi

if [[ $detectors == *kasan=1* ]]; then
	m=$(mark)
	echo "kasan_rcu_uaf 4 20000" > $S/run || true	# may report violations
	check "KASAN reports the RCU use-after-free" \
		kmsg_expect "$m" "BUG: KASAN: [a-z-]*use-after-free"
	check "the faulting access is by an lkl_sync thread" \
		kmsg_expect "$m" "use-after-free" "by task lkl_sync/"
elif [[ -n ${LKL_EXPECT_DETECTORS:-} ]]; then
	not_ok "debug kernel expected KASAN (CONFIG_KASAN)"
fi

rmmod $M || true
finish
