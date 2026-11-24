#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_mem: allocation experiments and their invariants, input validation,
# the /dev/lkl_mmap C suite, and refusal of the detector demonstrations
# on kernels without detectors.
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_mem
S=/sys/kernel/$M
CTEST=${LKL_USER_BUILD:-$LKL_ROOT/build}/lkl_mmap_test
[[ -x $CTEST ]] || bail "lkl_mmap_test not built (cmake --build build)"

unload $M
m=$(mark)
must_load $M mmap_pages=8
check "load logged with the buffer size" kmsg_expect "$m" "^$M: loaded: /dev/lkl_mmap 32768 bytes"
check "/dev/lkl_mmap is a character device" test -c /dev/lkl_mmap
check_eq "result before any run" "none" "$(cat $S/result)"
detectors=$(cat $S/detectors)
diag "detectors: $detectors"

run() { echo "$1" > $S/run && cat $S/result; }
field() { grep -oE "(^| )$1=[^ ]+" <<<"$2" | cut -d= -f2; }

# --- experiments ----------------------------------------------------------------------
r=$(run "kmalloc 1048576")
check_eq "kmalloc: sizes, alignment, contents" "0" "$(field violations "$r")"
classes=$(field size:class "$r")
diag "kmalloc size:class $classes"
check "kmalloc(13) comes from the 16-byte class" grep -q "13:16" <<<"$classes"
check "kmalloc(100) comes from the 128-byte class" grep -q "100:128" <<<"$classes"
check "kmalloc(5000) comes from the 8192-byte class" grep -q "5000:8192" <<<"$classes"

r=$(run "cache 20000")
check "cache: ok" test "$(field ok "$r")" = 1
check_eq "cache: every object 64-byte aligned" "0" "$(field misaligned "$r")"
check_eq "cache: constructor state on every allocation" "0" "$(field not_constructed "$r")"
check "cache: object padded to the alignment" test "$(field object_size "$r")" -ge 64

for order in 0 3 8; do
	r=$(run "pages $order")
	check_eq "pages order $order: blocks aligned to their size" "0" "$(field misaligned "$r")"
	check_eq "pages order $order: contents intact" "0" "$(field corrupt "$r")"
done

r=$(run "vmalloc 8388608")
check "vmalloc: ok" test "$(field ok "$r")" = 1
check_eq "vmalloc: 8 MiB is 2048 pages" "2048" "$(field pages "$r")"
diag "vmalloc: 2048 virtually contiguous pages in $(field physical_runs "$r") physical runs"

# --- input validation -------------------------------------------------------------------
check_fails "unknown experiment rejected" run "teleport 1"
check_fails "kmalloc above 1 MiB rejected" run "kmalloc 1048577"
check_fails "cache of 0 objects rejected" run "cache 0"
check_fails "pages order 9 rejected" run "pages 9"
check_fails "vmalloc above 64 MiB rejected" run "vmalloc 67108865"
check_fails "missing argument rejected" run "cache"

if [[ $detectors == *kmemleak=0* ]]; then
	check_fails "leak refused without kmemleak" run "leak 1"
	check "refusal reported as EOPNOTSUPP" grep -q "error=-95" $S/result
fi
if [[ $detectors == *kasan=0* ]]; then
	check_fails "kasan_oob refused without KASAN" run "kasan_oob 1"
fi

# --- /dev/lkl_mmap --------------------------------------------------------------------
out=$("$CTEST" 2>&1)
status=$?
while IFS= read -r line; do echo "#   $line"; done <<<"$out"
check_eq "mmap C suite passes (exit status)" "0" "$status"
check "mmap C suite ran every case" grep -q "^# [0-9]* run, [0-9]* passed, 0 failed" <<<"$out"

exec 3</dev/lkl_mmap
check_fails "rmmod refused while the device is open" rmmod $M
exec 3<&-
check "rmmod" rmmod $M
check "/dev/lkl_mmap removed" test ! -e /dev/lkl_mmap
# Only now, with a successful load behind us, can a failed insmod mean
# "rejected" rather than "not built".
check_fails "mmap_pages=0 rejected" load $M mmap_pages=0
check_fails "mmap_pages=1025 rejected" load $M mmap_pages=1025
check "every run logged" kmsg_expect "$m" "experiment=kmalloc .* ok=1" "experiment=vmalloc .* ok=1"
check "unload logged" kmsg_expect "$m" "^$M: unloaded after [0-9]+ mmap faults"
check "no kernel warnings (also checks KASAN when enabled)" kmsg_clean "$m"
finish
