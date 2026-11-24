#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_lifecycle: normal life cycle, then a forced failure at every init step
# to prove each unwind path releases exactly what was acquired, in reverse.
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_lifecycle
S=/sys/kernel/$M
unload $M

# --- normal load / run / unload ------------------------------------------------
m=$(mark)
must_load $M tick_ms=20 history=16
check "init acquires steps 1-4 in order" \
	kmsg_expect "$m" "step 1: state allocated$" "step 2: 16 samples allocated$" \
		"step 3: sysfs group created$" "step 4: timer armed \(20 ms\)$" "^$M: loaded$"
check "sysfs directory exists" test -d $S
check_eq "samples attribute" "16" "$(cat $S/samples)"
t1=$(cat $S/ticks)
sleep 0.5
t2=$(cat $S/ticks)
check "timer is ticking (ticks $t1 -> $t2)" test "$t2" -gt "$t1"
check_fails "sysfs attributes are read-only" bash -c "echo 5 > $S/ticks"

m=$(mark)
check "rmmod" rmmod $M
check "exit releases 4..1 in reverse order" \
	kmsg_expect "$m" "undo 4: timer stopped after [0-9]+ ticks$" \
		"undo 3: sysfs group removed$" "undo 2: samples freed$" \
		"undo 1: state freed$" "^$M: unloaded$"
check "sysfs directory removed" test ! -e $S

# --- failure injection at each step -----------------------------------------------
for n in 1 2 3 4; do
	m=$(mark)
	check_fails "fail_at=$n: insmod fails" load $M fail_at=$n
	check "fail_at=$n: module not loaded" bash -c "! grep -q '^$M ' /proc/modules"
	check "fail_at=$n: no sysfs directory left behind" test ! -e $S

	# Expected log: steps 1..n-1, the injected failure, undo n-1..1.
	pats=()
	acquired=("step 1: state allocated$" "step 2: [0-9]+ samples allocated$"
		  "step 3: sysfs group created$")
	released=("" "undo 1: state freed$" "undo 2: samples freed$"
		  "undo 3: sysfs group removed$")
	for ((i = 1; i < n; i++)); do pats+=("${acquired[i - 1]}"); done
	pats+=("step $n: injected failure$")
	for ((i = n - 1; i >= 1; i--)); do pats+=("${released[i]}"); done
	pats+=("init failed: -12$")
	if ((n == 1)); then what="nothing to unwind"; else what="unwinds steps $((n - 1))..1 in reverse"; fi
	check "fail_at=$n: $what" \
		kmsg_expect "$m" "${pats[@]}"
	check_eq "fail_at=$n: step $((n + 1)) never started, step $n never undone" "0" \
		"$(kmsg_count "$m" "step $((n + 1)):|undo $n:")"
	check "fail_at=$n: no kernel warnings" kmsg_clean "$m"
done

# --- parameter validation -------------------------------------------------------------
check_fails "tick_ms below 10 rejected" load $M tick_ms=5
check_fails "fail_at above 4 rejected" load $M fail_at=9

# --- repeated load/unload cycles ----------------------------------------------------------
m=$(mark)
cycles_ok=1
for i in $(seq 20); do
	load $M tick_ms=10 && sleep 0.02 && rmmod $M || { cycles_ok=0; break; }
done
check "20 load/unload cycles with a live timer" test $cycles_ok -eq 1
check_eq "every cycle logged its full teardown" "20" \
	"$(kmsg_count "$m" "undo 1: state freed$")"
check "no kernel warnings across cycles" kmsg_clean "$m"
finish
