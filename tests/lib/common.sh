# SPDX-License-Identifier: GPL-2.0
# shellcheck shell=bash
#
# Shared helpers for module tests. Sourced, not executed.
#
# Output is TAP: "ok N - description" / "not ok N - description". Each
# test script ends with `finish`, which prints the plan and exits non-zero
# if anything failed.

set -uo pipefail

LKL_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
KMSG="python3 $LKL_ROOT/tests/lib/kmsg.py"
_n=0
_failed=0

ok()     { _n=$((_n + 1)); echo "ok $_n - $*"; }
not_ok() { _n=$((_n + 1)); _failed=$((_failed + 1)); echo "not ok $_n - $*"; }
diag()   { echo "# $*"; }

# check "description" command...  -> ok if the command succeeds
check() {
	local desc=$1
	shift
	if "$@"; then ok "$desc"; else not_ok "$desc"; fi
}

# check_fails "description" command...  -> ok if the command fails
check_fails() {
	local desc=$1
	shift
	if "$@" 2>/dev/null; then not_ok "$desc"; else ok "$desc"; fi
}

# check_eq "description" expected actual
check_eq() {
	if [[ "$2" == "$3" ]]; then
		ok "$1"
	else
		not_ok "$1"
		diag "expected '$2', got '$3'"
	fi
}

finish() {
	echo "1..$_n"
	if ((_failed)); then
		diag "$_failed of $_n checks failed"
		exit 1
	fi
	exit 0
}

# Abort the whole script: later checks would be meaningless.
bail() { echo "Bail out! $*"; exit 1; }

# Fail loudly instead of letting negative checks ("write is rejected",
# "module is not loaded") pass vacuously on a system that cannot load
# modules at all.
preflight() {
	local c
	for c in insmod rmmod modinfo python3; do
		command -v "$c" >/dev/null || bail "required command '$c' not found"
	done
	[[ -r /proc/modules ]] || bail "/proc/modules missing: kernel without module support?"
	[[ -r /dev/kmsg && -w /dev/kmsg ]] || bail "/dev/kmsg not accessible"
}

require_root() {
	if [[ $EUID -ne 0 ]]; then
		echo "1..0 # SKIP needs root (insmod, /dev/kmsg)"
		exit 0
	fi
	preflight
}

# Path of a built module: ko_path lkl_hello
ko_path() {
	local ko
	ko=$(find "$LKL_ROOT/src" "$LKL_ROOT/drivers" -name "$1.ko" -print -quit)
	if [[ -z $ko ]]; then
		echo "missing $1.ko: run 'make' first" >&2
		return 1
	fi
	echo "$ko"
}

is_loaded() { [[ -d /sys/module/$1 ]] && grep -q "^$1 " /proc/modules; }

load() {
	local mod=$1
	shift
	insmod "$(ko_path "$mod")" "$@"
}

unload() { if is_loaded "$1"; then rmmod "$1"; fi; }

# First load of a script: if it fails, nothing after it can be trusted.
must_load() {
	if load "$@"; then
		ok "insmod $*"
	else
		bail "cannot load $1 (built for this kernel? see 'make')"
	fi
}

# New marker in the kernel log; tests assert only on what follows it.
mark() { $KMSG mark; }

kmsg_expect() { local m=$1; shift; $KMSG expect "$m" "$@"; }
kmsg_absent() { $KMSG absent "$1" "$2"; }
kmsg_count()  { $KMSG count "$1" "$2"; }
kmsg_level()  { $KMSG level "$1" "$2" "$3"; }
kmsg_clean()  { $KMSG clean "$1"; }

# Show the log after a marker when a test needs debugging.
kmsg_show()   { $KMSG show "$1" | sed 's/^/#   /'; }
