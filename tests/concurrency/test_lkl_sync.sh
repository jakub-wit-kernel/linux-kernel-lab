#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_sync: every concurrency experiment, its invariant, input validation,
# and refusal of the detector demonstrations on kernels without detectors.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_sync
S=/sys/kernel/$M
unload $M
m=$(mark)
must_load $M
check_eq "result before any run" "none" "$(cat $S/result)"
detectors=$(cat $S/detectors)
diag "detectors: $detectors"

# run "spec" -> runs an experiment; prints the result line
run() { echo "$1" > $S/run && cat $S/result; }
field() { grep -oE "(^| )$1=[^ ]+" <<<"$2" | cut -d= -f2; }

# --- experiments and their invariants -------------------------------------------
r=$(run "spin 4 200000")
check "spin: lock held across softirq timer, no lost updates" test "$(field ok "$r")" = 1
check_eq "spin: counter == thread increments + timer ticks" "$(field expected "$r")" "$(field counter "$r")"
check "spin: the timer really ran during the test" test "$(field timer_ticks "$r")" -gt 0

r=$(run "atomic 4 1000000")
check_eq "atomic: exact total" "4000000" "$(field counter "$r")"

r=$(run "racy 4 1000000")
check "racy: completes and reports" test "$(field ok "$r")" = 1
diag "racy: lost $(field lost "$r") of 4000000 unlocked increments"

r=$(run "mutex 4 100000")
check_eq "mutex: exact total" "400000" "$(field counter "$r")"
check_eq "mutex: never two holders" "0" "$(field exclusion_violations "$r")"

r=$(run "rcu 4 20000")
check_eq "rcu: readers never saw an inconsistent config" "0" "$(field invariant_violations "$r")"
check "rcu: readers actually read" test "$(field reads "$r")" -gt 0

r=$(run "waitq 4 100000")
check_eq "waitq: checksum of consumed values" "$(field expected_sum "$r")" "$(field sum "$r")"
r=$(run "waitq 7 5000")
check_eq "waitq: uneven 3 producers / 4 consumers" "$(field expected_sum "$r")" "$(field sum "$r")"

r=$(run "workqueue 1 20000")
check_eq "workqueue: every work item ran" "20000" "$(field executed "$r")"
check_eq "workqueue: cancel_delayed_work_sync cancelled 5" "5" "$(field cancelled "$r")"
check_eq "workqueue: exactly the other 5 ran" "5" "$(field delayed_executed "$r")"

# --- input validation -----------------------------------------------------------------
check_fails "unknown experiment rejected" run "teleport 1 1"
check_fails "0 threads rejected" run "atomic 0 10"
check_fails "33 threads rejected" run "atomic 33 10"
check_fails "0 iterations rejected" run "atomic 1 0"
check_fails "rcu needs a writer and a reader" run "rcu 1 10"
check_fails "waitq needs a producer and a consumer" run "waitq 1 10"
check_fails "workqueue item count capped" run "workqueue 1 200000"
check_fails "malformed command rejected" run "spin four"
check_fails "result is read-only" bash -c "echo x > $S/result"

# --- detector demonstrations need detectors --------------------------------------------
if [[ $detectors == *lockdep=0* ]]; then
	check_fails "lockdep_softirq refused without lockdep" run "lockdep_softirq 1 1"
	check "refusal reported as EOPNOTSUPP" grep -q "error=-95" $S/result
fi
if [[ $detectors == *kasan=0* ]]; then
	check_fails "kasan_rcu_uaf refused without KASAN" run "kasan_rcu_uaf 2 10"
fi

check "rmmod" rmmod $M
check "every run logged" kmsg_expect "$m" "experiment=spin .* ok=1" "experiment=workqueue .* ok=1"
check "no kernel warnings (also checks lockdep/KASAN when enabled)" kmsg_clean "$m"
finish
