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
#include <stdint.h>

#include "fbuild.h"

#define P(v) fb_tostr(S, (v))

static char *sdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }

/* a bare byte count from a script Int (repeat/pad width) can be arbitrarily
 * large without overflowing size_t -- malloc still fails (or the OOM killer
 * fires) well before that. Refuse past a sane cap instead of segfaulting on
 * the NULL malloc() would otherwise return. */
#define FBSTR_MAX_ALLOC ((size_t)1 << 30)   /* 1 GiB */

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

/* find / rfind / occurrences / repeat / chars / replace -------------------- */
/* find(s, sub[, from]) -> Int: first index of sub at/after byte offset
 * `from` (default 0), or -1. Negative/out-of-range `from` clamps to [0,len]. */
static fb_Value bi_find(fb_State *S, fb_Value *a, int n) {
	const char *s = P(a[0]), *sub = P(a[1]);
	long len = (long)strlen(s);
	long from = n >= 3 ? fb_toint(a[2]) : 0;
	if (from < 0) from += len;
	if (from < 0) from = 0;
	if (from > len) return fb_int(S, -1);
	const char *p = strstr(s + from, sub);
	return fb_int(S, p ? (long)(p - s) : -1);
}
/* rfind(s, sub) -> Int: last index of sub, or -1. */
static fb_Value bi_rfind(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]), *sub = P(a[1]);
	if (!*sub) return fb_int(S, (long)strlen(s));
	const char *last = NULL;
	for (const char *p = s; (p = strstr(p, sub)); p++) last = p;
	return fb_int(S, last ? (long)(last - s) : -1);
}
/* occurrences(s, sub) -> Int: count of non-overlapping matches of sub in s. */
static fb_Value bi_occurrences(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]), *sub = P(a[1]);
	size_t lf = strlen(sub);
	if (!lf) return fb_int(S, 0);
	long cnt = 0;
	for (const char *p = s; (p = strstr(p, sub)); p += lf) cnt++;
	return fb_int(S, cnt);
}
static fb_Value bi_repeat(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]); long k = fb_toint(a[1]);
	if (k < 0) k = 0;
	size_t l = strlen(s);
	if (l && (size_t)k > (SIZE_MAX - 1) / l)
		return fb_error(S, "repeat: result too large (%s * %ld)", s, k);
	size_t total = l * (size_t)k;
	if (total > FBSTR_MAX_ALLOC)
		return fb_error(S, "repeat: result too large (%zu bytes)", total);
	char *b = malloc(total + 1);
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
	if (start > len) start = len;
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
	if ((size_t)width > FBSTR_MAX_ALLOC)
		return fb_error(S, "pad: width too large (%ld)", width);
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

/* words(s) -> List<String>: whitespace-run-separated tokens, no empties
 * (unlike split(s, " "), collapses runs and ignores leading/trailing space). */
static fb_Value bi_words(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]);
	fb_Value list = fb_list(S);
	while (*s) {
		while (isspace((unsigned char)*s)) s++;
		if (!*s) break;
		const char *start = s;
		while (*s && !isspace((unsigned char)*s)) s++;
		char *w = malloc((size_t)(s - start) + 1);
		memcpy(w, start, (size_t)(s - start)); w[s - start] = '\0';
		fb_push(S, list, fb_str(S, w)); free(w);
	}
	return list;
}
/* partition(s, sep) -> (before, sep, after): splits on the first occurrence
 * of sep; if sep isn't found, (s, "", ""). */
static fb_Value bi_partition(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]), *sep = P(a[1]);
	fb_Value t = fb_tuple(S);
	const char *p = *sep ? strstr(s, sep) : NULL;
	if (!p) {
		fb_tuple_push(S, t, fb_str(S, s));
		fb_tuple_push(S, t, fb_str(S, ""));
		fb_tuple_push(S, t, fb_str(S, ""));
		return t;
	}
	size_t lb = (size_t)(p - s);
	char *before = malloc(lb + 1); memcpy(before, s, lb); before[lb] = '\0';
	fb_tuple_push(S, t, fb_str(S, before)); free(before);
	fb_tuple_push(S, t, fb_str(S, sep));
	fb_tuple_push(S, t, fb_str(S, p + strlen(sep)));
	return t;
}

/* is_alpha / is_digit / is_alnum / is_space / is_upper / is_lower --------- */
/* true iff s is non-empty and every byte satisfies `pred`. */
static fb_Value all_of(fb_State *S, const char *s, int (*pred)(int)) {
	if (!*s) return fb_bool(S, 0);
	for (; *s; s++) if (!pred((unsigned char)*s)) return fb_bool(S, 0);
	return fb_bool(S, 1);
}
static fb_Value bi_is_alpha(fb_State *S, fb_Value *a, int n) { (void)n; return all_of(S, P(a[0]), isalpha); }
static fb_Value bi_is_digit(fb_State *S, fb_Value *a, int n) { (void)n; return all_of(S, P(a[0]), isdigit); }
static fb_Value bi_is_alnum(fb_State *S, fb_Value *a, int n) { (void)n; return all_of(S, P(a[0]), isalnum); }
static fb_Value bi_is_space(fb_State *S, fb_Value *a, int n) { (void)n; return all_of(S, P(a[0]), isspace); }
/* is_upper/is_lower (Python semantics): true iff there's at least one cased
 * letter and no letter of the opposite case (digits/punctuation don't count). */
static fb_Value bi_is_upper(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]); int seen = 0;
	for (; *s; s++) {
		if (islower((unsigned char)*s)) return fb_bool(S, 0);
		if (isupper((unsigned char)*s)) seen = 1;
	}
	return fb_bool(S, seen);
}
static fb_Value bi_is_lower(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]); int seen = 0;
	for (; *s; s++) {
		if (isupper((unsigned char)*s)) return fb_bool(S, 0);
		if (islower((unsigned char)*s)) seen = 1;
	}
	return fb_bool(S, seen);
}

/* ord(s) -> Int: byte value of s's first character. chr(i) -> String: the
 * one-byte string for code i (0-255). ASCII/byte-oriented, like the rest of
 * this file -- no multibyte/UTF-8 decoding. */
static fb_Value bi_ord(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[0]);
	if (!*s) return fb_error(S, "ord: empty string");
	return fb_int(S, (unsigned char)s[0]);
}
static fb_Value bi_chr(fb_State *S, fb_Value *a, int n) {
	(void)n; long v = fb_toint(a[0]);
	if (v < 0 || v > 255) return fb_error(S, "chr: %ld out of byte range", v);
	char c[2] = { (char)v, '\0' };
	return fb_str(S, c);
}

int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	static const fb_Reg regs[] = {
		{ "upper", bi_upper, 1, 1, FB_RET_STR, 0, NULL },
		{ "lower", bi_lower, 1, 1, FB_RET_STR, 0, NULL },
		{ "capitalize", bi_capitalize, 1, 1, FB_RET_STR, 0, NULL },
		{ "title", bi_title, 1, 1, FB_RET_STR, 0, NULL },
		{ "trim", bi_trim, 1, 1, FB_RET_STR, 0, NULL },
		{ "lstrip", bi_lstrip, 1, 1, FB_RET_STR, 0, NULL },
		{ "rstrip", bi_rstrip, 1, 1, FB_RET_STR, 0, NULL },
		{ "starts_with", bi_starts_with, 2, 2, FB_RET_BOOL, 0, NULL },
		{ "ends_with", bi_ends_with, 2, 2, FB_RET_BOOL, 0, NULL },
		{ "strip_prefix", bi_strip_prefix, 2, 2, FB_RET_STR, 0, NULL },
		{ "strip_suffix", bi_strip_suffix, 2, 2, FB_RET_STR, 0, NULL },
		{ "find", bi_find, 2, 3, FB_RET_INT, 0, NULL },
		{ "rfind", bi_rfind, 2, 2, FB_RET_INT, 0, NULL },
		{ "occurrences", bi_occurrences, 2, 2, FB_RET_INT, 0, NULL },
		{ "repeat", bi_repeat, 2, 2, FB_RET_STR, 0, NULL },
		{ "chars", bi_chars, 1, 1, FB_RET_LIST, 0, NULL },
		{ "words", bi_words, 1, 1, FB_RET_LIST, 0, NULL },
		{ "partition", bi_partition, 2, 2, FB_RET_ANY, 0, NULL },
		{ "substr", bi_substr, 2, 3, FB_RET_STR, 0, NULL },
		{ "replace", bi_replace, 3, 3, FB_RET_STR, 0, NULL },
		{ "pad_left", bi_pad_left, 2, 3, FB_RET_STR, 0, NULL },
		{ "pad_right", bi_pad_right, 2, 3, FB_RET_STR, 0, NULL },
		{ "center", bi_center, 2, 3, FB_RET_STR, 0, NULL },
		{ "is_alpha", bi_is_alpha, 1, 1, FB_RET_BOOL, 0, NULL },
		{ "is_digit", bi_is_digit, 1, 1, FB_RET_BOOL, 0, NULL },
		{ "is_alnum", bi_is_alnum, 1, 1, FB_RET_BOOL, 0, NULL },
		{ "is_space", bi_is_space, 1, 1, FB_RET_BOOL, 0, NULL },
		{ "is_upper", bi_is_upper, 1, 1, FB_RET_BOOL, 0, NULL },
		{ "is_lower", bi_is_lower, 1, 1, FB_RET_BOOL, 0, NULL },
		{ "ord", bi_ord, 1, 1, FB_RET_INT, 0, NULL },
		{ "chr", bi_chr, 1, 1, FB_RET_STR, 0, NULL },
	};
	for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
		fb_register(S, &regs[i]);
	return 0;
}
