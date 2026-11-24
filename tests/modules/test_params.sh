#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_params: load-time values, sysfs permissions, runtime validation.
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_params
P=/sys/module/$M/parameters
unload $M

# --- load-time values -------------------------------------------------------
m=$(mark)
must_load $M count=7 name=probe verbose=1 threshold=80 mode=fast ports=22,80,443
check "init summary reflects every parameter" \
	kmsg_expect "$m" "^$M: loaded: name=probe count=7 verbose=1 threshold=80 mode=fast nports=3$"
check "array elements logged in order" \
	kmsg_expect "$m" "port\[0\]=22$" "port\[1\]=80$" "port\[2\]=443$"
check_eq "sysfs count" "7" "$(cat $P/count)"
check_eq "sysfs name" "probe" "$(cat $P/name)"
check_eq "sysfs verbose" "Y" "$(cat $P/verbose)"
check_eq "sysfs ports" "22,80,443" "$(cat $P/ports)"
check_eq "sysfs mode (custom get)" "fast" "$(cat $P/mode)"

# --- permissions ---------------------------------------------------------------
check_eq "name is read-only (0444)" "444" "$(stat -c %a $P/name)"
check_eq "threshold is writable (0644)" "644" "$(stat -c %a $P/threshold)"

# --- runtime validation through custom kernel_param_ops ------------------------
m=$(mark)
check "threshold accepts 1 (lower bound)" bash -c "echo 1 > $P/threshold"
check "threshold accepts 100 (upper bound)" bash -c "echo 100 > $P/threshold"
check_fails "threshold rejects 0" bash -c "echo 0 > $P/threshold"
check_fails "threshold rejects 101" bash -c "echo 101 > $P/threshold"
check_fails "threshold rejects non-numbers" bash -c "echo ten > $P/threshold"
check_eq "rejected writes left the value alone" "100" "$(cat $P/threshold)"
check "rejection logged at KERN_WARNING" \
	kmsg_level "$m" "threshold 0 rejected: must be 1\.\.100" warning
check "verbose mode logs accepted changes" \
	kmsg_expect "$m" "threshold 80 -> 1$" "threshold 1 -> 100$"

check "mode accepts 'debug' with trailing newline" bash -c "echo debug > $P/mode"
check_eq "mode reads back" "debug" "$(cat $P/mode)"
check_fails "mode rejects unknown names" bash -c "echo turbo > $P/mode"
check "rejected mode is logged without the newline" \
	kmsg_expect "$m" "mode 'turbo' rejected: expected fast, safe or debug$"
check "count (plain int) accepts runtime writes" bash -c "echo 42 > $P/count"

m=$(mark)
check "rmmod" rmmod $M
check "unload summary shows runtime values" \
	kmsg_expect "$m" "^$M: unloaded: final count=42 threshold=100 mode=debug$"

# --- load-time validation ---------------------------------------------------------
m=$(mark)
check_fails "out-of-range threshold at load is rejected" load $M threshold=500
check_fails "out-of-range port fails init (-EINVAL)" load $M ports=22,70000
check "port failure explained in the log" \
	kmsg_expect "$m" "ports\[1\]=70000 out of range 1\.\.65535$"
check_fails "more than 8 ports is rejected" load $M ports=1,2,3,4,5,6,7,8,9
check "module not left loaded after failures" bash -c "! grep -q '^$M ' /proc/modules"

# --- defaults --------------------------------------------------------------------------
check "insmod with defaults" load $M
check_eq "default threshold" "50" "$(cat $P/threshold)"
check_eq "default mode" "safe" "$(cat $P/mode)"
check_eq "default ports is empty" "" "$(cat $P/ports)"
check "rmmod" rmmod $M
check "no kernel warnings" kmsg_clean "$m"
finish
