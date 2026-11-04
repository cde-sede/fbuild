#!/bin/sh
# Runs each tests/run/*.fb and compares stdout+stderr and the exit status with
# NAME.expected when it exists (intended divergences, see DIVERGENCES.md), or
# else with the C fbuild (REF_FBUILD, default ~/probes/cparser/fbuild). With a
# NAME.args, the script runs once per line of it, the line's words as arguments.
FB=$(realpath "${FBUILD:-build/clang++-debug/fbuild}")
REF=${REF_FBUILD:-$HOME/probes/cparser/fbuild}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# Runs binary on the script once per argument line into file, each run's exit status after it.
each() {  # binary script args_file out_file
	if [ -f "$3" ]; then
		: > "$4"
		while IFS= read -r line; do
			echo "== $line" >> "$4"
			# shellcheck disable=SC2086
			"$1" --script "$2" $line >> "$4" 2>&1; echo "exit $?" >> "$4"
		done < "$3"
	else
		"$1" --script "$2" > "$4" 2>&1; echo "exit $?" >> "$4"
	fi
}
pass=0; fail=0
for f in tests/run/*.fb; do
	dir=$(dirname "$f"); base=$(basename "$f" .fb)
	# Durations closing the runner's summary lines vary between runs: shown as T.
	(cd "$dir" && each "$FB" "$base.fb" "$base.args" "$tmp/raw")
	sed -E 's/  ([0-9]+ms|[0-9]+\.[0-9]+s|[0-9]+m[0-9]+s)$/  T/' "$tmp/raw" > "$tmp/got"
	if [ -f "$dir/$base.expected" ]; then
		cp "$dir/$base.expected" "$tmp/want"
	else
		(cd "$dir" && each "$REF" "$base.fb" "$base.args" "$tmp/want")
	fi
	if cmp -s "$tmp/want" "$tmp/got"; then pass=$((pass+1)); else
		fail=$((fail+1)); echo "MISMATCH $f"; diff "$tmp/want" "$tmp/got" | head -${DIFF_LINES:-10}
	fi
done
echo "run_diff: $pass ok, $fail mismatched"
[ $fail -eq 0 ]
