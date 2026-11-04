#!/bin/bash
# Signals: each case starts fbuild in its own session, signals it (the whole group, as a
# terminal's Ctrl-C does, or fbuild alone), and checks the exit status, that the cursor
# was shown again after a live display, and that no child outlived it.
FB=$(realpath "${FBUILD:-build/clang++-debug/fbuild}")
cd "$(dirname "$0")/signals" || exit 1
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
check() {  # script signal target want_exit want_cursor
	setsid "$FB" --script "$1" > "$tmp/out" 2>&1 &
	local pid=$!
	sleep 0.5
	if [ "$3" = group ]; then kill -"$2" -- -"$pid"; else kill -"$2" "$pid"; fi
	wait "$pid"; local code=$?
	sleep 0.2
	local cursor=0; grep -q $'\e\\[?25h' "$tmp/out" && cursor=1
	local left=0; pgrep -f 'sleep 4[0-2]' > /dev/null && left=1 && pkill -f 'sleep 4[0-2]'
	if [ "$code" = "$4" ] && [ "$cursor" = "$5" ] && [ "$left" = 0 ] && grep -q 'interrupted by user' "$tmp/out"; then
		pass=$((pass+1))
	else
		fail=$((fail+1)); echo "FAIL $1 $2 $3: exit $code (want $4), cursor $cursor (want $5), leftover $left"
	fi
}
check int.fb INT group 130 1
check int.fb TERM self 143 1
check cap.fb INT group 130 0
check cap.fb TERM self 143 0
check busy.fb INT group 130 0
echo "signals: $pass ok, $fail failed"
[ $fail -eq 0 ]
