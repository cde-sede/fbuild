#!/bin/sh
# Type checker: each tests/check/NAME.fb through fbuild --dump-types (each top-level
# definition's type, then the diagnostics), compared with NAME.expected. The plugins in
# $PLUGINS are loaded, so their builtins and namespace objects are known.
FB=$(realpath "${FBUILD:-build/clang++-debug/fbuild}")
PLUGINS=$(realpath "${PLUGINS:-build/clang++-debug/plugins}")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
for f in tests/check/*.fb; do
	base=$(basename "$f" .fb)
	(cd tests/check && FBUILD_PLUGIN_PATH="$PLUGINS" "$FB" --dump-types --script "$base.fb" > "$tmp/got" 2>&1; echo "exit $?" >> "$tmp/got")
	if cmp -s "tests/check/$base.expected" "$tmp/got"; then pass=$((pass+1)); else
		fail=$((fail+1)); echo "MISMATCH $f"; diff "tests/check/$base.expected" "$tmp/got" | head -${DIFF_LINES:-10}
	fi
done
echo "check: $pass ok, $fail mismatched"
[ $fail -eq 0 ]
