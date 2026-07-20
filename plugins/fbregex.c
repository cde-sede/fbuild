/* fbregex -- POSIX extended regex utilities for fbuild, as a plugin (.so).
 *
 * Build: cc -shared -fPIC -Isrc plugins/fbregex.c -o build/fbregex.so
 * Load:  FBUILD_PLUGINS=build/fbregex.so ./fbuild
 *
 * Exposes a single stateless namespace object bound to the bare identifier
 * `regex` (fb_global_bind, no construction needed):
 *
 *     print(regex.findall("a1 b22 c333", "[0-9]+"))   # [1, 22, 333]
 *
 * Wraps <regex.h> (glibc POSIX ERE). Patterns are always REG_EXTENDED.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>

#include "fbuild.h"

#define P(v) fb_tostr(S, (v))

/* fb_error() never returns; on failure this aborts the builtin, so callers
 * can assume `re` is valid once rx_compile() returns. */
static void rx_compile(fb_State *S, regex_t *re, const char *pat) {
	int rc = regcomp(re, pat, REG_EXTENDED);
	if (rc != 0) {
		char buf[256];
		regerror(rc, re, buf, sizeof buf);
		fb_error(S, "regex: invalid pattern '%s': %s", pat, buf);
	}
}

/* expand \0-\9 backreferences in `repl` against match `m` (ng entries) taken
 * against subject `s`. Caller frees the result. */
static char *rx_expand(const char *repl, const char *s, regmatch_t *m, size_t ng, size_t *outlen) {
	size_t cap = strlen(repl) + 64, len = 0;
	char *b = malloc(cap);
	for (const char *p = repl; *p; p++) {
		if (*p == '\\' && isdigit((unsigned char)p[1])) {
			size_t idx = (size_t)(p[1] - '0'); p++;
			if (idx < ng && m[idx].rm_so >= 0) {
				size_t l = (size_t)(m[idx].rm_eo - m[idx].rm_so);
				if (len + l + 1 > cap) { cap = (len + l + 1) * 2; b = realloc(b, cap); }
				memcpy(b + len, s + m[idx].rm_so, l); len += l;
			}
			continue;
		}
		if (len + 2 > cap) { cap *= 2; b = realloc(b, cap); }
		b[len++] = *p;
	}
	b[len] = '\0';
	if (outlen) *outlen = len;
	return b;
}

/* regex.match(s, pattern) -> Bool ------------------------------------------ */
static fb_Value m_match(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[1]), *pat = P(a[2]);
	regex_t re; rx_compile(S, &re, pat);
	int rc = regexec(&re, s, 0, NULL, 0);
	regfree(&re);
	return fb_bool(S, rc == 0);
}

/* regex.find(s, pattern) -> String | nil (first match) --------------------- */
static fb_Value m_find(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[1]), *pat = P(a[2]);
	regex_t re; rx_compile(S, &re, pat);
	regmatch_t m;
	int rc = regexec(&re, s, 1, &m, 0);
	regfree(&re);
	if (rc != 0) return fb_nil(S);
	size_t l = (size_t)(m.rm_eo - m.rm_so);
	char *b = malloc(l + 1); memcpy(b, s + m.rm_so, l); b[l] = '\0';
	fb_Value r = fb_str(S, b); free(b); return r;
}

/* regex.findall(s, pattern) -> List<String> (all non-overlapping matches) -- */
static fb_Value m_findall(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[1]), *pat = P(a[2]);
	regex_t re; rx_compile(S, &re, pat);
	fb_Value list = fb_list(S);
	const char *p = s; int eflags = 0;
	for (;;) {
		regmatch_t m;
		if (regexec(&re, p, 1, &m, eflags) != 0) break;
		size_t l = (size_t)(m.rm_eo - m.rm_so);
		char *b = malloc(l + 1); memcpy(b, p + m.rm_so, l); b[l] = '\0';
		fb_push(S, list, fb_str(S, b)); free(b);
		if (m.rm_eo == m.rm_so) {
			if (p[m.rm_eo] == '\0') break;
			p += m.rm_eo + 1;
		} else {
			p += m.rm_eo;
		}
		eflags = REG_NOTBOL;
	}
	regfree(&re);
	return list;
}

/* regex.groups(s, pattern) -> List<String|nil> (group 0 = whole match, first
 * match only; empty list if no match; nil for a group that did not
 * participate) */
static fb_Value m_groups(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[1]), *pat = P(a[2]);
	regex_t re; rx_compile(S, &re, pat);
	size_t ng = re.re_nsub + 1;
	regmatch_t *m = malloc(ng * sizeof *m);
	int rc = regexec(&re, s, ng, m, 0);
	fb_Value list = fb_list(S);
	if (rc == 0) {
		for (size_t i = 0; i < ng; i++) {
			if (m[i].rm_so < 0) { fb_push(S, list, fb_nil(S)); continue; }
			size_t l = (size_t)(m[i].rm_eo - m[i].rm_so);
			char *b = malloc(l + 1); memcpy(b, s + m[i].rm_so, l); b[l] = '\0';
			fb_push(S, list, fb_str(S, b)); free(b);
		}
	}
	free(m); regfree(&re);
	return list;
}

/* regex.replace(s, pattern, repl) -> String (first match only, \0-\9 backrefs) */
static fb_Value m_replace(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[1]), *pat = P(a[2]), *repl = P(a[3]);
	regex_t re; rx_compile(S, &re, pat);
	size_t ng = re.re_nsub + 1;
	regmatch_t *m = malloc(ng * sizeof *m);
	int rc = regexec(&re, s, ng, m, 0);
	if (rc != 0) { free(m); regfree(&re); return fb_str(S, s); }
	size_t rl; char *rep = rx_expand(repl, s, m, ng, &rl);
	size_t pre = (size_t)m[0].rm_so;
	const char *tail = s + m[0].rm_eo;
	size_t total = pre + rl + strlen(tail);
	char *out = malloc(total + 1);
	memcpy(out, s, pre);
	memcpy(out + pre, rep, rl);
	strcpy(out + pre + rl, tail);
	free(rep); free(m); regfree(&re);
	fb_Value r = fb_str(S, out); free(out); return r;
}

/* regex.replaceall(s, pattern, repl) -> String (all matches, \0-\9 backrefs) */
static fb_Value m_replaceall(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[1]), *pat = P(a[2]), *repl = P(a[3]);
	regex_t re; rx_compile(S, &re, pat);
	size_t ng = re.re_nsub + 1;
	regmatch_t *m = malloc(ng * sizeof *m);
	size_t cap = strlen(s) * 2 + 64, len = 0;
	char *out = malloc(cap);
	const char *p = s; int eflags = 0;
	for (;;) {
		if (regexec(&re, p, ng, m, eflags) != 0) break;
		size_t pre = (size_t)m[0].rm_so;
		if (len + pre + 1 > cap) { cap = (len + pre + 1) * 2 + 64; out = realloc(out, cap); }
		memcpy(out + len, p, pre); len += pre;
		size_t rl; char *rep = rx_expand(repl, p, m, ng, &rl);
		if (len + rl + 1 > cap) { cap = (len + rl + 1) * 2 + 64; out = realloc(out, cap); }
		memcpy(out + len, rep, rl); len += rl; free(rep);
		if (m[0].rm_eo == m[0].rm_so) {
			if (p[m[0].rm_eo] == '\0') { p += m[0].rm_eo; break; }
			if (len + 2 > cap) { cap = (len + 2) * 2; out = realloc(out, cap); }
			out[len++] = p[m[0].rm_eo];
			p += m[0].rm_eo + 1;
		} else {
			p += m[0].rm_eo;
		}
		eflags = REG_NOTBOL;
	}
	size_t rest = strlen(p);
	if (len + rest + 1 > cap) { cap = len + rest + 1; out = realloc(out, cap); }
	memcpy(out + len, p, rest); len += rest; out[len] = '\0';
	free(m); regfree(&re);
	fb_Value r = fb_str(S, out); free(out); return r;
}

/* regex.split(s, pattern) -> List<String> (pieces between non-overlapping
 * matches) */
static fb_Value m_split(fb_State *S, fb_Value *a, int n) {
	(void)n; const char *s = P(a[1]), *pat = P(a[2]);
	regex_t re; rx_compile(S, &re, pat);
	fb_Value list = fb_list(S);
	const char *p = s; int eflags = 0;
	for (;;) {
		regmatch_t m;
		if (regexec(&re, p, 1, &m, eflags) != 0) break;
		if (m.rm_so == m.rm_eo) {
			if (!p[0]) break;
			p++; eflags = REG_NOTBOL;
			continue;
		}
		size_t l = (size_t)m.rm_so;
		char *b = malloc(l + 1); memcpy(b, p, l); b[l] = '\0';
		fb_push(S, list, fb_str(S, b)); free(b);
		p += m.rm_eo;
		eflags = REG_NOTBOL;
	}
	fb_push(S, list, fb_str(S, p));
	regfree(&re);
	return list;
}

/* calling the bare namespace object as a function (`regex(...)`) is a
 * mistake -- point at the method form instead of segfaulting on a[0]/a[1]. */
static fb_Value bi_regex_noncallable(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n;
	return fb_error(S, "regex is a namespace object; call e.g. "
	                    "regex.match(s, pattern), not regex(...)");
}

int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	fb_Class cls = fb_class(S, "Regex");
	fb_method(S, cls, "match",      m_match);
	fb_method(S, cls, "find",       m_find);
	fb_method(S, cls, "findall",    m_findall);
	fb_method(S, cls, "groups",     m_groups);
	fb_method(S, cls, "replace",    m_replace);
	fb_method(S, cls, "replaceall", m_replaceall);
	fb_method(S, cls, "split",      m_split);

	/* registered so the type checker recognizes the bare identifier `regex`
	 * (FB_GLOBAL: a bound value, not a callable); the real value comes from
	 * fb_global_bind below, which eval() resolves first, so this fn only runs
	 * if someone calls `regex(...)` directly. */
	fb_register(S, &(fb_Reg){ "regex", bi_regex_noncallable, 0, -1, FB_RET_ANY, FB_GLOBAL,
	            "regex namespace (fbregex): match, find, findall, groups, "
	            "replace, replaceall, split" });
	fb_global_bind(S, "regex", fb_object(S, cls, NULL));
	return 0;
}
