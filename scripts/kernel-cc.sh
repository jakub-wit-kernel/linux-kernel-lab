#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Print the gcc that built the running kernel (e.g. "gcc-13"), so modules
# can be built with a matching compiler: make CC="$(scripts/kernel-cc.sh)"
set -euo pipefail
if [[ $(cat /proc/version) =~ gcc-([0-9]+) ]]; then
	echo "gcc-${BASH_REMATCH[1]}"
else
	echo gcc
fi
