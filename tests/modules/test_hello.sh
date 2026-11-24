#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_hello: load, metadata, unload.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_hello
unload $M
m=$(mark)

must_load $M
check "module is listed in /proc/modules" is_loaded $M
check "init logged with release and HZ" \
	kmsg_expect "$m" "^$M: loaded on Linux $(uname -r | sed 's/[.+]/\\&/g') \(HZ=[0-9]+\)$"
check "init message is KERN_INFO" kmsg_level "$m" "^$M: loaded" info

info=$(modinfo "$(ko_path $M)")
check_eq "modinfo license" "GPL" "$(sed -n 's/^license: *//p' <<<"$info")"
check_eq "modinfo version" "1.0" "$(sed -n 's/^version: *//p' <<<"$info")"
check "modinfo vermagic matches running kernel" \
	grep -q "^vermagic: *$(uname -r) " <<<"$info"

check_fails "loading it twice fails (EEXIST)" load $M
check "rmmod succeeds" rmmod $M
check "module is gone" bash -c "! grep -q '^$M ' /proc/modules"
check "exit logged with uptime" kmsg_expect "$m" "^$M: unloaded after [0-9]+ ms$"
check "no kernel warnings" kmsg_clean "$m"
finish
