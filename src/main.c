/* cparser library: slice-based lexer + token matcher/cursor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cparser.h"

/* arena declarations only (implementation lives in run.c via BUILD_IMPLEMENTATION) */
#include "cds_lib/build.h"

struct parser *p   = NULL;
size_t         cur = 0;

/* ---- program arena ---------------------------------------------------- */

static mem_arena *g_arena;

void arena_init(void) {
	g_arena = arena_create(GiB(4), MiB(1));   /* 4 GiB reserved, 1 MiB committed */
	if (!g_arena) { fprintf(stderr, "out of memory\n"); exit(1); }
}
void arena_fini(void) {
	if (g_arena) arena_destroy(g_arena);
	g_arena = NULL;
}
void *a_alloc(size_t n) {
	void *q = arena_push(g_arena, n, 0);   /* zeroed */
	if (!q) { fprintf(stderr, "out of memory\n"); exit(1); }
	return q;
}
void *a_grow(void *old, size_t oldn, size_t newn) {
	void *q = arena_push(g_arena, newn, 1);
	if (!q) { fprintf(stderr, "out of memory\n"); exit(1); }
	if (old && oldn) memcpy(q, old, oldn < newn ? oldn : newn);
	return q;
}
char *a_dupn(const char *s, size_t n) {
	char *q = a_alloc(n + 1);
	memcpy(q, s, n);
	q[n] = '\0';
	return q;
}
char *a_dup(const char *s) { return a_dupn(s, strlen(s)); }


const char *kind_name[] = {
	[TOK_EOF]="TOK_EOF",
	[TOK_NEWLINE]="TOK_NEWLINE",

	[TOK_ID]="TOK_ID",
	[TOK_INT]="TOK_INT",
	[TOK_FLOAT]="TOK_FLOAT",
	[TOK_BOOL]="TOK_BOOL",
	[TOK_STRING]="TOK_STRING",
	[TOK_BACKTICK]="TOK_BACKTICK",
	[TOK_COMMAND]="TOK_COMMAND",
	[TOK_PATH]="TOK_PATH",

	[TOK_WHEN]="TOK_WHEN",
	[TOK_WHERE]="TOK_WHERE",
	[TOK_AND]="TOK_AND",
	[TOK_OR]="TOK_OR",
	[TOK_OROR]="TOK_OROR",
	[TOK_NOT]="TOK_NOT",

	[TOK_ASSIGN]="TOK_ASSIGN",
	[TOK_ARROW]="TOK_ARROW",
	[TOK_PIPE]="TOK_PIPE",
	[TOK_MORPH]="TOK_MORPH",
	[TOK_SPLAT]="TOK_SPLAT",
	[TOK_SEQ]="TOK_SEQ",

	[TOK_LT]="TOK_LT",
	[TOK_GT]="TOK_GT",
	[TOK_LE]="TOK_LE",
	[TOK_GE]="TOK_GE",
	[TOK_EQ]="TOK_EQ",
	[TOK_NE]="TOK_NE",

	[TOK_DOT]="TOK_DOT",
	[TOK_COMMA]="TOK_COMMA",
	[TOK_COLON]="TOK_COLON",
	[TOK_STAR]="TOK_STAR",
	[TOK_AMP]="TOK_AMP",
	[TOK_AT]="TOK_AT",
	[TOK_PLUS]="TOK_PLUS",
	[TOK_MINUS]="TOK_MINUS",
	[TOK_SLASH]="TOK_SLASH",
	[TOK_PERCENT]="TOK_PERCENT",

	[TOK_LPAREN]="TOK_LPAREN",
	[TOK_RPAREN]="TOK_RPAREN",
	[TOK_LBRACE]="TOK_LBRACE",
	[TOK_RBRACE]="TOK_RBRACE",
	[TOK_LBRACK]="TOK_LBRACK",
	[TOK_RBRACK]="TOK_RBRACK",
	[TOK_LANGLE]="TOK_LANGLE",
	[TOK_RANGLE]="TOK_RANGLE",
	[TOK_STR_BEG]="TOK_STR_BEG",
	[TOK_STR_END]="TOK_STR_END",
	[TOK_STRTEXT]="TOK_STRTEXT"
};

int _match(struct obj want, size_t idx) {
	if (idx >= p->pos)
		return 0;
	token *t = &p->tokens[idx];
	if (want.kind != NONE && (int)t->kind != want.kind)
		return 0;
	if (want.string && (strlen(want.string) != t->len || memcmp(want.string, t->string, t->len)))
		return 0;
	return 1;
}

int _peek(struct obj want) { return _match(want, cur); }

int _accept(struct obj want) {
	if (!_match(want, cur))
		return 0;
	cur++;
	return 1;
}

/* consume + return matched token, or die */
token *_expect(struct obj want) {
	if (!_match(want, cur)) {
		token *t = cur < p->pos ? &p->tokens[cur] : NULL;
		fprintf(stderr, "parse error at token %zu: expected", cur);
		if (want.kind != NONE) fprintf(stderr, " kind %s", kind_name[want.kind]);
		if (want.string)       fprintf(stderr, " \"%s\"", want.string);
		if (t) fprintf(stderr, ", got \"%.*s\"\n", (int)t->len, t->string);
		else   fprintf(stderr, ", got <EOF>\n");
		exit(1);
	}
	return &p->tokens[cur++];
}

token *tok_cur(void)        { return cur < p->pos ? &p->tokens[cur] : NULL; }
token *tok_at(size_t off)   { size_t i = cur + off; return i < p->pos ? &p->tokens[i] : NULL; }
void   tok_advance(void)    { cur++; }
void   tok_back(void)       { cur--; }

/* opt-in NUL-terminated copy, cached in the token, allocated once (arena) */
char *tok_str(token *t) {
	if (!t->owned)
		t->owned = a_dupn(t->string, t->len);
	return t->owned;
}

/* arena-backed: nothing to free per token / per parser. */
void tok_free(token *t)            { (void)t; }
void parser_free(struct parser *pp) { (void)pp; }

void emit(int kind, char *str, size_t len, int sl, int sc, int el, int ec) {
	if (p->pos >= MAX_TOKENS)
		return;
	p->tokens[p->pos++] = (token){
		.kind = kind, .start = {sc, sl}, .end = {ec, el},
		.string = str, .len = len};
}

void lexer(char *restrict s) { (void)s; }
