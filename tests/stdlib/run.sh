#!/bin/sh
# The standard plugins against cparser's C ones: each script runs with both binaries and
# their plugins (REF_FBUILD / REF_PLUGINS); stdout, stderr and the exit status must match.
FB=$(realpath "${FBUILD:-build/clang++-debug/fbuild}")
DIR=$(realpath "${PLUGINS:-build/clang++-debug/plugins}")
REF=${REF_FBUILD:-$HOME/probes/cparser/fbuild}
REFDIR=${REF_PLUGINS:-$HOME/probes/cparser/build}
cd "$(dirname "$0")" || exit 1
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
check() {  # script plugins...
	script=$1; shift
	ref=""; got=""
	for p in "$@"; do ref="$ref --plugin $REFDIR/$p.so"; got="$got --plugin $DIR/$p.so"; done
	$REF $ref --script "$script" > "$tmp/want" 2>&1; echo "exit $?" >> "$tmp/want"
	$FB $got --script "$script" > "$tmp/got" 2>&1; echo "exit $?" >> "$tmp/got"
	if cmp -s "$tmp/want" "$tmp/got"; then pass=$((pass+1)); else
		fail=$((fail+1)); echo "MISMATCH $script"; diff "$tmp/want" "$tmp/got" | head -10; fi
}
check math.fb fbmath
check stdlib.fb fbloop fbstr
check fs.fb fbfs
check regex.fb fbregex
check opt.fb fbopt fbfs
echo "stdlib: $pass ok, $fail mismatched"
[ $fail -eq 0 ]
