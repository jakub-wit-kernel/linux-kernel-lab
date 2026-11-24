#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build a small kernel with the bug detectors on, for testing the lab's
# modules: lockdep (PROVE_LOCKING), KASAN, DEBUG_ATOMIC_SLEEP, DEBUG_LIST,
# kmemleak, and dynamic debug; plus network namespaces and netfilter for
# the networking tests.
#
#   scripts/build-debug-kernel.sh 6.12 kernel
#
# Downloads linux-VERSION from kernel.org, configures it with virtme-ng's
# minimal VM config plus the debug options, and builds bzImage and the
# module build infrastructure. Object files are removed afterwards; the
# tree can still build external modules (KDIR=OUTDIR) and boot in QEMU.
# Requires: curl, xz, gcc, flex, bison, bc, libelf-dev, libssl-dev, vng.
set -euo pipefail

ver=${1:?usage: build-debug-kernel.sh VERSION OUTDIR}
out=${2:?usage: build-debug-kernel.sh VERSION OUTDIR}
major=${ver%%.*}

rm -rf "$out"
mkdir -p "$out"
curl -fsSL --retry 6 --retry-delay 10 --retry-all-errors \
	"https://cdn.kernel.org/pub/linux/kernel/v$major.x/linux-$ver.tar.xz" |
	tar -xJ --strip-components=1 -C "$out"
cd "$out"

vng --kconfig
scripts/config \
	-e MODULES -e MODULE_UNLOAD \
	-e DEBUG_KERNEL -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP \
	-e KASAN -e KASAN_GENERIC -e KASAN_INLINE \
	-e DEBUG_LIST -e KALLSYMS -e KALLSYMS_ALL \
	-e DEBUG_KMEMLEAK -d DEBUG_KMEMLEAK_DEFAULT_OFF -e DYNAMIC_DEBUG \
	-e NET -e INET -e NAMESPACES -e NET_NS -e NETFILTER \
	-e DEBUG_FS -e DMI -e DMIID -e SYSFS -e PROC_FS \
	-e DEBUG_INFO_NONE -d DEBUG_INFO_DWARF5 -d DEBUG_INFO_BTF
make olddefconfig

for opt in PROVE_LOCKING KASAN DEBUG_ATOMIC_SLEEP DEBUG_KMEMLEAK DYNAMIC_DEBUG \
	NET_NS NETFILTER MODULES; do
	grep -q "^CONFIG_$opt=y" .config ||
		{ echo "build-debug-kernel: CONFIG_$opt did not stick" >&2; exit 1; }
done

make -j"$(nproc)" bzImage modules
# Keep only what booting and building external modules needs.
find . \( -name '*.o' -o -name '*.ko' -o -name '*.a' \) \
	-not -path './scripts/*' -not -path './tools/*' -delete
rm -rf Documentation
echo "build-debug-kernel: $(make -s kernelrelease) ready in $out"
