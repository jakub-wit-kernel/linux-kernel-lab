#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Run every module test against the running kernel. Needs root and the
# modules built for this kernel (`make`).
#
#   sudo tests/run_all.sh            all tests
#   sudo tests/run_all.sh params     only tests whose name contains "params"
#
# Exit status: number of failed test scripts (0 = all passed).
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

if [[ $EUID -ne 0 ]]; then
	echo "run_all.sh: needs root (insmod, /dev/kmsg)" >&2
	exit 1
fi

filter=${1:-}
failed=0
ran=0
vendor=$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null || echo unknown)
echo "# kernel $(uname -r), $(nproc) CPUs, platform: $vendor"
# Set by scripts/qemu-test.sh: prove we really are inside the QEMU guest.
if [[ -n ${LKL_EXPECT_VM:-} && $vendor != QEMU ]]; then
	echo "run_all.sh: expected to run under QEMU, platform is '$vendor'" >&2
	exit 1
fi
# tests/debug/ runs last: its detector demonstrations disable lockdep
# and taint the kernel for anything that would come after.
for t in tests/modules/test_*.sh tests/drivers/test_*.sh \
	 tests/concurrency/test_*.sh tests/memory/test_*.sh \
	 tests/networking/test_*.sh tests/debug/test_*.sh; do
	name=$(basename "$t" .sh)
	[[ -n $filter && $name != *"$filter"* ]] && continue
	ran=$((ran + 1))
	echo "# === $name"
	if bash "$t"; then
		echo "# --- $name: PASS"
	else
		echo "# --- $name: FAIL"
		failed=$((failed + 1))
	fi
done
echo "# $ran test scripts, $failed failed"
exit "$failed"
