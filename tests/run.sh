#!/bin/sh
# make test: differential checks against the C fbuild (built on demand into
# build/reference from ~/probes/cparser; see tests/reference/build.sh).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
cd "$root"
[ -x build/reference/ast_dump ] || sh tests/reference/build.sh build/reference
FBUILD=${FBUILD:-build/clang++-debug/fbuild} sh tests/parse_diff.sh tests/parse/*.fb \
	$(find "$HOME/probes/cparser" "$HOME/final" -name "*.fb" -not -path "*/.git/*" -not -path "*/build/*" -not -path "$HOME/final/fbuild/*")
FBUILD=${FBUILD:-build/clang++-debug/fbuild} sh tests/run_diff.sh
FBUILD=${FBUILD:-build/clang++-debug/fbuild} bash tests/signals.sh
FBUILD=${FBUILD:-build/clang++-debug/fbuild} sh tests/plugins.sh
FBUILD=${FBUILD:-build/clang++-debug/fbuild} sh tests/stdlib/run.sh
FBUILD=${FBUILD:-build/clang++-debug/fbuild} sh tests/check.sh
