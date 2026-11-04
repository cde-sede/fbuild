#!/bin/sh
# Builds the C reference tools from ~/probes/cparser (CPARSER overrides) into $1.
set -e
C=${CPARSER:-$HOME/probes/cparser}
out=${1:-build/reference}
mkdir -p "$out"
here=$(cd "$(dirname "$0")" && pwd)
gcc -O1 -w -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -I"$C/src" -I"$C/cds-lib" \
	"$C/src/build_system.c" "$C/src/parser.c" "$C/src/walk.c" "$C/src/run.c" "$C/src/eval.c" \
	"$C/src/run_builtins.c" "$C/src/build_impl.c" "$here/ast_dump.c" \
	-o "$out/ast_dump" -rdynamic -ldl -lm -lpthread
