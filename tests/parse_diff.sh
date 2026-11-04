#!/bin/sh
# Parses every .fb given (or found under ~/probes/cparser) with both the C reference
# (build/reference/ast_dump) and the C++ fbuild (--dump-ast), and reports mismatches.
FB=${FBUILD:-build/clang++-debug/fbuild}
REF=${REF:-build/reference/ast_dump}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ $# -eq 0 ]; then
	set -- $(find "$HOME/probes/cparser" -name '*.fb' -not -path '*/node_modules/*' -not -path '*/.git/*')
fi
pass=0; fail=0
for f in "$@"; do
	"$REF" "$f" > "$tmp/ref" 2>&1; r=$?
	"$FB" --dump-ast "$f" > "$tmp/got" 2>&1; g=$?
	if [ $r -ne 0 ] && [ $g -ne 0 ]; then pass=$((pass+1)); continue; fi
	if [ $r -eq $g ] && cmp -s "$tmp/ref" "$tmp/got"; then pass=$((pass+1)); else
		fail=$((fail+1)); echo "MISMATCH $f (ref $r, got $g)"
		diff "$tmp/ref" "$tmp/got" | head -${DIFF_LINES:-6}
	fi
done
echo "parse_diff: $pass ok, $fail mismatched"
[ $fail -eq 0 ]
