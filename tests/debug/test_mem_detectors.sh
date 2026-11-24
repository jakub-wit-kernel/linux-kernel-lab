#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Prove that kmemleak and KASAN catch lkl_mem's deliberate memory bugs.
# Runs after every non-debug test (see tests/run_all.sh). A second KASAN
# report in one boot needs the kasan_multi_shot boot parameter, which the
# debug-kernel CI job passes.
#
# Skips on kernels without the detectors, unless LKL_EXPECT_DETECTORS=1.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_mem
S=/sys/kernel/$M
KML=/sys/kernel/debug/kmemleak
unload $M
must_load $M
detectors=$(cat $S/detectors)
diag "detectors: $detectors"

if [[ $detectors == *kmemleak=0*kasan=0* ]]; then
	if [[ -n ${LKL_EXPECT_DETECTORS:-} ]]; then
		not_ok "debug kernel expected, but kmemleak and KASAN are both off"
	else
		ok "memory detector demonstrations # SKIP kernel without kmemleak and KASAN"
	fi
	rmmod $M
	finish
fi

if [[ $detectors == *kmemleak=1* ]]; then
	mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
	if [[ ! -w $KML ]]; then
		not_ok "kmemleak built in but $KML is not writable (disabled at boot?)"
	else
		echo clear > $KML		# forget leaks reported so far
		echo "leak 8" > $S/run
		# kmemleak ignores objects younger than 5 s; scan twice so
		# stale register or stack copies of the pointers are gone.
		sleep 6
		echo scan > $KML
		sleep 1
		echo scan > $KML
		report=$(cat $KML)
		n=$(grep -c "LKLLEAK-" <<<"$report")
		diag "kmemleak reported $n of 8 leaked objects"
		check "kmemleak reports leaked objects by their contents" test "$n" -ge 1
		check "the backtrace names the leaking function" grep -q "lkl_mem_leak_one" <<<"$report"
	fi
elif [[ -n ${LKL_EXPECT_DETECTORS:-} ]]; then
	not_ok "debug kernel expected kmemleak (CONFIG_DEBUG_KMEMLEAK)"
fi

if [[ $detectors == *kasan=1* ]]; then
	m=$(mark)
	echo "kasan_oob 1" > $S/run
	check "KASAN reports the out-of-bounds write" \
		kmsg_expect "$m" "BUG: KASAN: slab-out-of-bounds"
	check "it is a 1-byte write by an lkl_mem path" \
		kmsg_expect "$m" "slab-out-of-bounds" "Write of size 1"
elif [[ -n ${LKL_EXPECT_DETECTORS:-} ]]; then
	not_ok "debug kernel expected KASAN (CONFIG_KASAN)"
fi

rmmod $M || true
finish
