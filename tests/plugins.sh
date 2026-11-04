#!/bin/sh
# Plugins: loading through --plugin, $FBUILD_PLUGINS and $FBUILD_PLUGIN_PATH, --list-plugins,
# and the errors of a missing file, a shared object without entry points and a failing open().
FB=$(realpath "${FBUILD:-build/clang++-debug/fbuild}")
DIR=$(realpath "${PLUGINS:-build/clang++-debug/plugins}")
TESTS=$(realpath "${TEST_PLUGINS:-build/clang++-debug/test-plugins}")
CXXCMD=${PLUGIN_CXX:-}
cd "$(dirname "$0")/plugins" || exit 1
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
expect() {  # name want_exit want_text command...
	name=$1; want=$2; text=$3; shift 3
	out=$("$@" 2>&1); code=$?
	if [ "$code" = "$want" ] && printf '%s' "$out" | grep -qF -- "$text"; then pass=$((pass+1)); else
		fail=$((fail+1)); echo "FAIL $name: exit $code (want $want), output: $out"; fi
}
expect flag 0 "OK hello, world 1" "$FB" --plugin "$DIR/hello.so" --script use.fb
expect flag= 0 "OK hello, world 1" "$FB" --plugin="$DIR/hello.so" --script use.fb
expect env 0 "OK hello, world 1" env FBUILD_PLUGINS="$DIR/hello.so" "$FB" --script use.fb
mkdir "$tmp/dir" && cp "$DIR/hello.so" "$tmp/dir/"
expect path 0 "OK hello, world 1" env FBUILD_PLUGIN_PATH="$tmp/dir" "$FB" --script use.fb
expect list 0 "$tmp/dir/hello.so" env FBUILD_PLUGIN_PATH="$tmp/dir" "$FB" --list-plugins
expect missing 1 "none.so: cannot open" "$FB" --plugin "$tmp/none.so" --script use.fb
expect no_entry 1 "missing fb::plugin::api()" "$FB" --plugin "$TESTS/no_entry.so" --script use.fb
expect bad_open 1 "needs a configuration it does not have" "$FB" --plugin "$TESTS/bad_open.so" --script use.fb
expect bad_dir 1 "FBUILD_PLUGIN_PATH: cannot open" env FBUILD_PLUGIN_PATH="$tmp/nowhere" "$FB" --script use.fb
echo "plugins: $pass ok, $fail failed"
[ $fail -eq 0 ]
