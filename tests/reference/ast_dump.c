/* Reference AST dump from the C interpreter (~/probes/cparser), for diffing the
 * C++ parser against it: tests/reference/build.sh links it with cparser's sources. */
#include <stdio.h>
#include <stdlib.h>
#include "cparser.h"
#include "ast.h"
#include "fbuild.h"

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	fb_State *S = fb_new();
	(void)S;
	char *src = a_alloc(n + 1);
	if (fread(src, 1, n, f) != (size_t)n) return 1;
	src[n] = 0;
	fclose(f);
	p = a_alloc(sizeof *p + sizeof(token) * MAX_TOKENS);
	lex(src);
	ast_print(parse_program(), 0);
	return 0;
}
