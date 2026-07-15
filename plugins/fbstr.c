/* fbstr -- string utilities for fbuild, as a plugin (.so).
 *
 * Build: cc -shared -fPIC -Isrc plugins/fbstr.c -o build/fbstr.so
 * Load:  FBUILD_PLUGINS=build/fbstr.so ./fbuild
 *
 * The build-glue string ops (cat, join, split, subst, to_int, to_str) stay in
 * core; the general text utilities live here. fb_str copies its argument, so a
 * malloc'd scratch buffer is fine.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "fbuild.h"

#define P(v) fb_tostr(S, (v))

static char *sdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }

/* upper / lower / capitalize / title -------------------------------------- */
static fb_Value bi_upper(fb_State *S, fb_Value *a, int n) {
	(void)n; char *s = sdup(P(a[0]));
	for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
	fb_Value r = fb_str(S, s); free(s); return r;
}
static fb_Value bi_lower(fb_State *S, fb_Value *a, int n) {
	(void)n; char *s = sdup(P(a[0]));
	for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
	fb_Value r = fb_str(S, s); free(s); return r;
}
static fb_Value bi_capitalize(fb_State *S, fb_Value *a, int n) {
	(void)n; char *s = sdup(P(a[0]));
	if (s[0]) { s[0] = (char)toupper((unsigned char)s[0]);
		for (char *p = s + 1; *p; p++) *p = (char)tolower((unsigned char)*p); }
	fb_Value r = fb_str(S, s); free(s); return r;
}
static fb_Value bi_title(fb_State *S, fb_Value *a, int n) {
	(void)n; char *s = sdup(P(a[0])); int start = 1;
	for (char *p = s; *p; p++) {
		if (isspace((unsigned char)*p)) { start = 1; continue; }
		*p = start ? (char)toupper((unsigned char)*p) : (char)tolower((unsigned char)*p);
		start = 0;
	}
	fb_Value r = fb_str(S, s); free(s); return r;
}

/* trim / lstrip / rstrip -------------------------------------------------- */
static fb_Value bi_trim(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]);
	while (isspace((unsigned char)*s)) s++;
	size_t l = strlen(s);
	while (l && isspace((unsigned char)s[l-1])) l--;
	char *b = malloc(l + 1); memcpy(b, s, l); b[l] = '\0';
	fb_Value r = fb_str(S, b); free(b); return r;
}
static fb_Value bi_lstrip(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]);
	while (isspace((unsigned char)*s)) s++;
	return fb_str(S, s);
}
static fb_Value bi_rstrip(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]); size_t l = strlen(s);
	while (l && isspace((unsigned char)s[l-1])) l--;
	char *b = malloc(l + 1); memcpy(b, s, l); b[l] = '\0';
	fb_Value r = fb_str(S, b); free(b); return r;
}

/* prefix / suffix tests + strips ------------------------------------------ */
static fb_Value bi_starts_with(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]), *pre = P(a[1]);
	return fb_bool(S, strncmp(s, pre, strlen(pre)) == 0);
}
static fb_Value bi_ends_with(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]), *suf = P(a[1]);
	size_t ls = strlen(s), lf = strlen(suf);
	return fb_bool(S, lf <= ls && strcmp(s + ls - lf, suf) == 0);
}
static fb_Value bi_strip_prefix(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]), *pre = P(a[1]); size_t lp = strlen(pre);
	return fb_str(S, strncmp(s, pre, lp) == 0 ? s + lp : s);
}
static fb_Value bi_strip_suffix(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]), *suf = P(a[1]);
	size_t ls = strlen(s), lf = strlen(suf);
	if (lf <= ls && strcmp(s + ls - lf, suf) == 0) {
		char *b = malloc(ls - lf + 1); memcpy(b, s, ls - lf); b[ls-lf] = '\0';
		fb_Value r = fb_str(S, b); free(b); return r;
	}
	return fb_str(S, s);
}

/* find / repeat / chars / replace ----------------------------------------- */
static fb_Value bi_find(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]), *sub = P(a[1]);
	const char *p = strstr(s, sub);
	return fb_int(S, p ? (long)(p - s) : -1);
}
static fb_Value bi_repeat(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]); long k = fb_toint(a[1]);
	if (k < 0) k = 0;
	size_t l = strlen(s); char *b = malloc(l * (size_t)k + 1);
	for (long i = 0; i < k; i++) memcpy(b + (size_t)i * l, s, l);
	b[l * (size_t)k] = '\0';
	fb_Value r = fb_str(S, b); free(b); return r;
}
static fb_Value bi_chars(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]);
	fb_Value list = fb_list(S);
	for (; *s; s++) { char c[2] = { *s, '\0' }; fb_push(S, list, fb_str(S, c)); }
	return list;
}
/* substr(s, start[, end]) -> String: [start,end) byte range, neg counts from
 * end (mirrors fbloop's slice()). end defaults to the string's length. */
static fb_Value bi_substr(fb_State *S, fb_Value *a, int n) {
	const char *s = P(a[0]);
	long len = (long)strlen(s);
	long start = fb_toint(a[1]);
	long end = n >= 3 ? fb_toint(a[2]) : len;
	if (start < 0) start += len;
	if (end < 0) end += len;
	if (start < 0) start = 0;
	if (end > len) end = len;
	if (end < start) end = start;
	size_t l = (size_t)(end - start);
	char *b = malloc(l + 1); memcpy(b, s + start, l); b[l] = '\0';
	fb_Value r = fb_str(S, b); free(b); return r;
}
static fb_Value bi_replace(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]), *from = P(a[1]), *to = P(a[2]);
	if (!*from) return fb_str(S, s);
	size_t lf = strlen(from), lt = strlen(to), cnt = 0;
	for (const char *p = s; (p = strstr(p, from)); p += lf) cnt++;
	char *out = malloc(strlen(s) + (lt > lf ? cnt * (lt - lf) : 0) + 1), *w = out;
	const char *p = s, *q;
	while ((q = strstr(p, from))) {
		memcpy(w, p, (size_t)(q - p)); w += q - p;
		memcpy(w, to, lt);             w += lt;
		p = q + lf;
	}
	strcpy(w, p);
	fb_Value r = fb_str(S, out); free(out); return r;
}

/* pad_left / pad_right / center(s, width [,fill]) ------------------------- */
static fb_Value pad(fb_State *S, fb_Value *a, int n, int side) {
	const char *s = P(a[0]); long width = fb_toint(a[1]);
	char fill = ' ';
	if (n >= 3) { const char *f = P(a[2]); if (f[0]) fill = f[0]; }
	size_t l = strlen(s);
	if (width < 0 || (size_t)width <= l) return fb_str(S, s);
	size_t total = (size_t)width, padn = total - l;
	size_t lpad = side < 0 ? padn : side > 0 ? 0 : padn / 2;
	char *b = malloc(total + 1);
	memset(b, fill, lpad); memcpy(b + lpad, s, l);
	memset(b + lpad + l, fill, total - lpad - l); b[total] = '\0';
	fb_Value r = fb_str(S, b); free(b); return r;
}
static fb_Value bi_pad_left (fb_State *S, fb_Value *a, int n) { return pad(S, a, n, -1); }
static fb_Value bi_pad_right(fb_State *S, fb_Value *a, int n) { return pad(S, a, n,  1); }
static fb_Value bi_center   (fb_State *S, fb_Value *a, int n) { return pad(S, a, n,  0); }

int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	static const fb_Reg regs[] = {
		{ "upper",        bi_upper,        1, 1, FB_RET_STR,  0 },
		{ "lower",        bi_lower,        1, 1, FB_RET_STR,  0 },
		{ "capitalize",   bi_capitalize,   1, 1, FB_RET_STR,  0 },
		{ "title",        bi_title,        1, 1, FB_RET_STR,  0 },
		{ "trim",         bi_trim,         1, 1, FB_RET_STR,  0 },
		{ "lstrip",       bi_lstrip,       1, 1, FB_RET_STR,  0 },
		{ "rstrip",       bi_rstrip,       1, 1, FB_RET_STR,  0 },
		{ "starts_with",  bi_starts_with,  2, 2, FB_RET_BOOL, 0 },
		{ "ends_with",    bi_ends_with,    2, 2, FB_RET_BOOL, 0 },
		{ "strip_prefix", bi_strip_prefix, 2, 2, FB_RET_STR,  0 },
		{ "strip_suffix", bi_strip_suffix, 2, 2, FB_RET_STR,  0 },
		{ "find",         bi_find,         2, 2, FB_RET_INT,  0 },
		{ "repeat",       bi_repeat,       2, 2, FB_RET_STR,  0 },
		{ "chars",        bi_chars,        1, 1, FB_RET_LIST, 0 },
		{ "substr",       bi_substr,       2, 3, FB_RET_STR,  0 },
		{ "replace",      bi_replace,      3, 3, FB_RET_STR,  0 },
		{ "pad_left",     bi_pad_left,     2, 3, FB_RET_STR,  0 },
		{ "pad_right",    bi_pad_right,    2, 3, FB_RET_STR,  0 },
		{ "center",       bi_center,       2, 3, FB_RET_STR,  0 },
	};
	for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
		fb_register(S, &regs[i]);
	return 0;
}
