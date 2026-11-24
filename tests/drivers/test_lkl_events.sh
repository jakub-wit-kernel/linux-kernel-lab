#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_events: procfs, sysfs and debugfs entries, the C interface suite,
# parameter validation, attribute visibility and clean removal.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_events
P=/proc/lkl_lab
S=/sys/kernel/lkl_lab
CTEST=${LKL_USER_BUILD:-$LKL_ROOT/build}/lkl_events_test
[[ -x $CTEST ]] || bail "lkl_events_test not built (cmake --build build)"
unload $M

m=$(mark)
must_load $M init_capacity=64
check "load logged" kmsg_expect "$m" "^$M: loaded: capacity 64, debug attributes off$"

# --- entries and permissions --------------------------------------------------
for f in events:444 summary:444 control:200 snapshot:400; do
	check_eq "proc ${f%%:*} mode" "${f#*:}" "$(stat -c %a $P/${f%%:*})"
done
for f in count:444 capacity:644 threshold:644 alert:444; do
	check_eq "sysfs ${f%%:*} mode" "${f#*:}" "$(stat -c %a $S/${f%%:*})"
done
check "debug attributes hidden without debug_attrs" test ! -e $S/debug/dropped

# --- sysfs validation ------------------------------------------------------------
check_eq "capacity reflects the parameter" "64" "$(cat $S/capacity)"
check_fails "capacity 0 rejected" bash -c "echo 0 > $S/capacity"
check_fails "capacity 4097 rejected" bash -c "echo 4097 > $S/capacity"
check_fails "threshold must be a number" bash -c "echo lots > $S/threshold"
check_eq "rejected writes left capacity alone" "64" "$(cat $S/capacity)"

# --- control + summary -------------------------------------------------------------
check "control: add" bash -c "echo 'add hello world' > $P/control"
check "events lists it" grep -qE '^0 [0-9]+ hello world$' $P/events
check_eq "summary count" "count 1" "$(grep '^count' $P/summary)"
check_eq "sysfs count agrees" "1" "$(cat $S/count)"
check "control: clear" bash -c "echo clear > $P/control"
check_eq "events empty after clear" "" "$(cat $P/events)"

# --- the C interface suite --------------------------------------------------------------
out=$("$CTEST" 2>&1)
status=$?
while IFS= read -r line; do echo "#   $line"; done <<<"$out"
check_eq "C interface suite passes (exit status)" "0" "$status"
check "C interface suite ran every case" grep -q "^# [0-9]* run, [0-9]* passed, 0 failed" <<<"$out"

# --- debugfs (optional: only where debugfs is mounted) ---------------------------------
if mountpoint -q /sys/kernel/debug; then
	check "debugfs adds counter readable" grep -qE '^[0-9]+$' /sys/kernel/debug/lkl_lab/adds
	check "debugfs ring state readable" grep -q '^cap ' /sys/kernel/debug/lkl_lab/ring
else
	ok "debugfs entries # SKIP debugfs not mounted"
fi

# --- unload removes everything; reload with debug attributes ---------------------------
m=$(mark)
check "rmmod" rmmod $M
check "proc directory removed" test ! -e $P
check "sysfs directory removed" test ! -e $S
check "unload logged" kmsg_expect "$m" "^$M: unloaded$"

check "load with debug_attrs=1" load $M debug_attrs=1 init_capacity=4
check "debug group visible" test -r $S/debug/dropped -a -r $S/debug/last_seq
check_eq "last_seq with no events" "none" "$(cat $S/debug/last_seq)"
echo "burst 10" > $P/control
check_eq "debug dropped counts overwritten events" "6" "$(cat $S/debug/dropped)"
check_eq "debug last_seq" "9" "$(cat $S/debug/last_seq)"
check "rmmod" rmmod $M

check_fails "init_capacity=0 rejected" load $M init_capacity=0
check_fails "init_capacity=5000 rejected" load $M init_capacity=5000
check "no kernel warnings" kmsg_clean "$m"
finish
