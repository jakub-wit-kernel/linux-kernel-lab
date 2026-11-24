#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_logging: severities, once, hex dump, rate limiting, dynamic debug.
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_logging
P=/sys/module/$M/parameters
unload $M

m=$(mark)
must_load $M burst=20

check "error line is KERN_ERR"       kmsg_level "$m" "level demo: error$" err
check "warning line is KERN_WARNING" kmsg_level "$m" "level demo: warning$" warning
check "notice line is KERN_NOTICE"   kmsg_level "$m" "level demo: notice$" notice
check "info line is KERN_INFO"       kmsg_level "$m" "level demo: info$" info
check "every line carries the pr_fmt prefix" \
	kmsg_expect "$m" "^$M: level demo: error$"
check "pr_debug is silent without dynamic debug" \
	kmsg_absent "$m" "level demo: debug"
check_eq "pr_info_once printed exactly once" "1" \
	"$(kmsg_count "$m" "printed once although called 3 times")"
check "hex dump shows offset, bytes and ASCII" \
	kmsg_expect "$m" "sample: 00000000: 6c 6b 6c 00 de ad be ef 01 02 03 04 05 06 07 08  lkl\.\.\.\.\.\.\.\.\.\.\.\.\.$" \
		"sample: 00000010: 68 65 78 2d 64 75 6d 70 +hex-dump$"
check_eq "rate limit lets exactly 5 of 20 through" "5" \
	"$(kmsg_count "$m" "burst message [0-9]+ of 20$")"
check "burst summary counts the suppressed ones" \
	kmsg_expect "$m" "burst done: requested=20 emitted=5 suppressed=15$"

# The budget (5 per 60 s) is spent, so a runtime burst emits nothing.
m=$(mark)
check "runtime trigger via write-only parameter" bash -c "echo 7 > $P/emit"
check "runtime burst fully suppressed inside the interval" \
	kmsg_expect "$m" "burst done: requested=7 emitted=0 suppressed=7$"
check_fails "trigger is write-only (reading fails)" cat $P/emit
check_fails "trigger rejects huge bursts" bash -c "echo 99999 > $P/emit"
check "rmmod" rmmod $M

# Dynamic debug: enable pr_debug for this module at load time.
if [[ -e /proc/dynamic_debug/control ]] ||
   grep -q '^CONFIG_DYNAMIC_DEBUG=y' "/boot/config-$(uname -r)" 2>/dev/null; then
	m=$(mark)
	check "insmod with dyndbg=+p" load $M burst=0 dyndbg=+p
	check "pr_debug visible once enabled, at KERN_DEBUG" \
		kmsg_level "$m" "level demo: debug \(dynamic debug is on\)$" debug
	check "rmmod" rmmod $M
else
	ok "pr_debug with dynamic debug # SKIP kernel built without CONFIG_DYNAMIC_DEBUG"
fi
check "no kernel warnings" kmsg_clean "$m"
finish
