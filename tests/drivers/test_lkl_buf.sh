#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# lkl_buf: device nodes, the C test suite, the CLI, and module lifecycle.
# Needs the user-space programs built: cmake -S . -B build && cmake --build build
# shellcheck source=tests/lib/common.sh
source "$(dirname "$0")/../lib/common.sh"
require_root

M=lkl_buf
BUILD=${LKL_USER_BUILD:-$LKL_ROOT/build}
CLI=$BUILD/lklbuf
CTEST=$BUILD/lklbuf_test
[[ -x $CLI && -x $CTEST ]] || bail "user-space tools not built in $BUILD (cmake --build build)"
unload $M

m=$(mark)
must_load $M nr_devs=2 capacity=65536
check "load logged" kmsg_expect "$m" "^$M: loaded: 2 devices, major [0-9]+, capacity 65536 bytes$"
check "/dev/lkl_buf0 is a character device" test -c /dev/lkl_buf0
check "/dev/lkl_buf1 is a character device" test -c /dev/lkl_buf1
check "no third device" test ! -e /dev/lkl_buf2
check "sysfs class lists both" test -e /sys/class/$M/lkl_buf0 -a -e /sys/class/$M/lkl_buf1
read -r maj min < <(stat -c '%t %T' /dev/lkl_buf0)	# hex major/minor
check_eq "device numbers match sysfs" \
	"$(cat /sys/class/$M/lkl_buf0/dev)" "$((16#$maj)):$((16#$min))"

# The C suite does the heavy lifting; relay its TAP output as diagnostics.
out=$("$CTEST" 2>&1)
status=$?
sed 's/^/#   /' <<<"$out"
check_eq "C device test suite passes (exit status)" "0" "$status"
check "C device test suite ran every case" grep -q "^# [0-9]* run, [0-9]* passed, 0 failed" <<<"$out"

# CLI round trip.
"$CLI" clear
check "CLI write" "$CLI" write 0 "hello, kernel"
check_eq "CLI read" "hello, kernel" "$("$CLI" read 0 64)"
check "CLI fill" "$CLI" fill 0 5 0x58
check_eq "CLI read after fill" "XXXXX, kernel" "$("$CLI" read 0 64)"
check_eq "CLI info size" "size 13" "$("$CLI" info | grep '^size')"
check "CLI truncate" "$CLI" truncate 5
check_eq "CLI read after truncate" "XXXXX" "$("$CLI" read 0 64)"
check "CLI write from stdin with K suffix offset" bash -c "printf 'abc' | '$CLI' write 1K -"
check_eq "data landed at 1024" "abc" "$("$CLI" read 1024 3)"
check_fails "CLI rejects a bad size" "$CLI" truncate 10X
check_fails "CLI rejects truncate beyond capacity" "$CLI" truncate 1M

# dd: plain file tools work on the device.
"$CLI" clear
check "dd writes 32 KiB" dd if=/dev/urandom of=/tmp/lkl_buf.in bs=4K count=8 status=none
check "dd copies into the device" dd if=/tmp/lkl_buf.in of=/dev/lkl_buf0 bs=4K status=none
check "cmp reads back identical data" cmp -n 32768 /tmp/lkl_buf.in /dev/lkl_buf0
rm -f /tmp/lkl_buf.in

# Lifecycle: an open file pins the module.
exec 7</dev/lkl_buf0
check_fails "rmmod refused while a file is open" rmmod $M
exec 7<&-
m=$(mark)
check "rmmod after close" rmmod $M
check "unload logged" kmsg_expect "$m" "^$M: unloaded$"
check "device nodes removed" test ! -e /dev/lkl_buf0

check_fails "nr_devs=0 rejected" load $M nr_devs=0
check_fails "capacity below 4 KiB rejected" load $M capacity=100
check "no kernel warnings" kmsg_clean "$m"
finish
