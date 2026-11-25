#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build a pinned libbpf release as a static library, with its headers and
# the UAPI headers it ships, for the eBPF part of the lab:
#
#   scripts/build-libbpf.sh [VERSION [PREFIX]]    (default v1.6.3 .tools/libbpf)
#   cmake -S . -B build -DLKL_LIBBPF_DIR=$PWD/.tools/libbpf
#
# Distribution packages work too if they are libbpf 1.0 or newer (CMake
# finds them with pkg-config). Ubuntu 22.04 ships 0.5, hence this script.
# Requires: git, make, a C compiler, libelf-dev, zlib1g-dev.
set -euo pipefail

ver=${1:-v1.6.3}
prefix=${2:-.tools/libbpf}
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)

if [[ -f $prefix/lib/libbpf.a && $(cat "$prefix/VERSION" 2>/dev/null) == "$ver" ]]; then
	echo "build-libbpf: $ver already in $prefix"
	exit 0
fi

src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
git clone -q --depth 1 --branch "$ver" https://github.com/libbpf/libbpf "$src/libbpf"
make -s -C "$src/libbpf/src" -j"$(nproc)" BUILD_STATIC_ONLY=1 \
	OBJDIR="$src/build" DESTDIR="$prefix" PREFIX= LIBDIR=/lib \
	INCLUDEDIR=/include UAPIDIR=/include install install_uapi_headers
echo "$ver" >"$prefix/VERSION"
echo "build-libbpf: $ver installed in $prefix"
