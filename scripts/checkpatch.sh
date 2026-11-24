#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Run the kernel's checkpatch.pl over every C source and header in src/.
#
# checkpatch.pl is looked up in $CHECKPATCH, then in $KDIR/scripts, then
# in .tools/ (where `scripts/checkpatch.sh --fetch` downloads it from the
# upstream kernel at a pinned tag, together with the data files it reads).
#
# Fails on ERROR and WARNING. CHECK-level (--strict) findings are printed
# but do not fail the run.
set -euo pipefail
cd "$(dirname "$0")/.."

TAG=${CHECKPATCH_TAG:-v6.12}
KDIR=${KDIR:-/lib/modules/$(uname -r)/build}
BASE="https://raw.githubusercontent.com/torvalds/linux/$TAG/scripts"

if [[ ${1:-} == --fetch ]]; then
	mkdir -p .tools
	for f in checkpatch.pl spelling.txt const_structs.checkpatch; do
		curl -fsSL "$BASE/$f" -o ".tools/$f"
	done
	chmod +x .tools/checkpatch.pl
	echo "checkpatch: fetched $TAG into .tools/"
	exit 0
fi

cp=${CHECKPATCH:-}
for c in "$cp" "$KDIR/scripts/checkpatch.pl" .tools/checkpatch.pl; do
	[[ -n $c && -x $c ]] && { cp=$c; break; }
done
if [[ -z $cp || ! -x $cp ]]; then
	echo "checkpatch: not found; run 'scripts/checkpatch.sh --fetch'" >&2
	exit 2
fi

# Tracked sources only: never the build's generated *.mod.c files.
mapfile -t files < <(git ls-files 'src/*.c' 'src/*.h')
out=$("$cp" --no-tree --strict --terse --show-types -f "${files[@]}" || true)
echo "$out"
errors=$(grep -c ': ERROR:' <<<"$out" || true)
warnings=$(grep -c ': WARNING:' <<<"$out" || true)
checks=$(grep -c ': CHECK:' <<<"$out" || true)
echo "checkpatch: ${#files[@]} files, $errors errors, $warnings warnings, $checks checks"
[[ $errors -eq 0 && $warnings -eq 0 ]]
