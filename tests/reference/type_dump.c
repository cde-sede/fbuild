/* Reference type check from the C interpreter (~/probes/cparser): each top-level
 * definition's inferred type, then each type error, for comparing the C++ checker
 * (fbuild --dump-types) against it. tests/reference/build.sh links it with cparser's
 * sources. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cparser.h"
#include "ast.h"
#include "fbuild.h"

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	fb_State *S = fb_new();
	/* the C plugins (FBUILD_REF_PLUGINS, colon-separated), so their names are known */
	const char *list = getenv("FBUILD_REF_PLUGINS");
	if (list) {
		char *copy = a_dup(list);
		for (char *part = strtok(copy, ":"); part; part = strtok(NULL, ":"))
			fb_load_plugin(S, part);
	}
	char *src = a_alloc(n + 1);
	if (fread(src, 1, n, f) != (size_t)n) return 1;
	src[n] = 0;
	fclose(f);
	p = a_alloc(sizeof *p + sizeof(token) * MAX_TOKENS);
	lex(src);
	ast *prog = parse_program();
	static fb_Diag diags[256];
	int errs = infer_program_collect(prog, diags, 256);
	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		if (s->kind == AST_ASSIGN || s->kind == AST_EAGER)
			printf("%.*s : %s\n", (int)s->tok->len, s->tok->string, obj_type_sprint(s->type));
	}
	for (int i = 0; i < errs && i < 256; i++)
		printf("error %d:%d: %s\n", diags[i].line, diags[i].col, diags[i].message);
	return errs ? 1 : 0;
}
