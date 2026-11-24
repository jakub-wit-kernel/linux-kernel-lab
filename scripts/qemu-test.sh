#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Run the module test suite inside a throwaway QEMU VM using virtme-ng.
# The VM boots a kernel image and sees this checkout through a shared
# filesystem, so a buggy module can crash the guest, never the host.
#
#   scripts/qemu-test.sh                         boot the running kernel
#   scripts/qemu-test.sh /path/to/bzImage KDIR   boot another kernel (the
#                                                modules are built for KDIR)
#
# Requires: qemu-system-x86_64, virtme-ng (pip install virtme-ng), a
# readable kernel image. KVM is used when /dev/kvm is accessible; without
# it QEMU falls back to software emulation (slower, still correct).
set -euo pipefail
cd "$(dirname "$0")/.."

KIMG=${1:-/boot/vmlinuz-$(uname -r)}
KDIR=${2:-/lib/modules/$(uname -r)/build}

die() { echo "qemu-test: $*" >&2; exit 2; }
command -v vng >/dev/null || die "virtme-ng not found (pip install virtme-ng)"
command -v qemu-system-x86_64 >/dev/null || die "qemu-system-x86_64 not found"
[[ -r $KIMG ]] || die "$KIMG not readable (try: sudo chmod 0644 $KIMG)"

# CC on the command line: kbuild ignores a CC environment variable.
make -s modules KDIR="$KDIR" ${CC:+CC="$CC"}
# User-space tools and test programs (run inside the guest from the share).
cmake -S . -B build >/dev/null
cmake --build build -j >/dev/null

kvm_note="KVM"
if [[ ! -w /dev/kvm ]]; then
	kvm_note="software emulation (no writable /dev/kvm)"
fi
echo "qemu-test: booting $KIMG with $kvm_note"

# --user root: insmod and /dev/kmsg need root inside the guest.
# LKL_EXPECT_VM makes run_all.sh verify it really runs under QEMU.
# LKL_EXPECT_DETECTORS (debug-kernel CI) makes the detector test fail
# instead of skipping if lockdep or KASAN turn out to be missing.
# LKL_QEMU_APPEND: extra kernel command line (e.g. kasan_multi_shot).
vng --run "$KIMG" ${LKL_QEMU_APPEND:+--append "$LKL_QEMU_APPEND"} --user root --cpus 2 --memory "${LKL_QEMU_MEM:-1G}" \
	--rwdir "$PWD" \
	--exec "cd '$PWD' && LKL_EXPECT_VM=1 LKL_EXPECT_DETECTORS=${LKL_EXPECT_DETECTORS:-} tests/run_all.sh"
