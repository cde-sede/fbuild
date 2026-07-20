/* fbloop -- iteration + list-processing builtins for fbuild, as a plugin (.so).
 *
 * Build: cc -shared -fPIC -Isrc plugins/fbloop.c -o build/fbloop.so
 * Load:  FBUILD_PLUGINS=build/fbloop.so ./fbuild
 *
 * General loops:
 *   iter(n, ...args)   List  -> n elements; each is the args (tuple if >1, the
 *                             single arg, or the index i if no args). So
 *                             `iter(10, x) |> f` calls f(x) ten times;
 *                             `iter(10) |> f` calls f(0..9).
 *   while(cond, func)  Int  (FB_LAZY cond) -> re-evaluates cond each round;
 *                             while truthy, calls func(i) with the round index
 *                             (capped). No intermediary list -- func runs
 *                             directly each round, memory doesn't grow with
 *                             round count. Returns the number of rounds run.
 *                             Eager language: cond must change via
 *                             external/mutable state to terminate.
 *
 * List ops (moved out of core): slice index enumerate take drop reverse range
 * sort unique fold count sum min max flatten. The build-glue list ops (filter,
 * zip, concat, contains, length, first, last) stay in core.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "fbuild.h"

static int seq(fb_Value v) { fb_Kind k = fb_kind(v); return k == FB_LIST || k == FB_TUPLE; }
static int truthy(fb_Value v) {
	fb_Kind k = fb_kind(v);
	if (k == FB_NIL) return 0;
	if (k == FB_BOOL || k == FB_INT) return fb_toint(v) != 0;
	return 1;
}
/* sort/unique key: each element stringified ONCE. fb_tostr per qsort
 * comparison was O(n log n) arena allocations, and unique's rescan made it
 * O(n^2) stringify -- large lists blew the arena (OOM) and took seconds. */
typedef struct { const char *key; fb_Value val; } keyed;
static int cmp_keyed(const void *x, const void *y) {
	return strcmp(((keyed const *)x)->key, ((keyed const *)y)->key);
}
static unsigned long khash(const char *s) {          /* FNV-1a */
	unsigned long h = 1469598103934665603UL;
	while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211UL; }
	return h;
}

/* ---- general loops ----------------------------------------------------- */

/* one loop element from the payload args (tuple if >1, single arg, or index). */
static fb_Value payload(fb_State *S, fb_Value *args, int m, long idx) {
	if (m == 0) return fb_int(S, idx);
	if (m == 1) return args[0];
	fb_Value t = fb_tuple(S);
	for (int i = 0; i < m; i++) fb_tuple_push(S, t, args[i]);
	return t;
}

static fb_Value bi_iter(fb_State *S, fb_Value *a, int n) {
	long k = fb_toint(a[0]); if (k < 0) k = 0;
	fb_Value out = fb_list(S);
	for (long i = 0; i < k; i++) fb_push(S, out, payload(S, a + 1, n - 1, i));
	return out;
}

#define WHILE_CAP 10000000L
static fb_Value bi_while(fb_State *S, fb_Value *a, int n) {
	fb_Value func = n >= 2 ? fb_force(S, a[1]) : (fb_Value)0;
	long i;
	for (i = 0; i < WHILE_CAP; i++) {
		if (!truthy(fb_force(S, a[0]))) break;   /* re-evaluate cond each round */
		if (func) {
			fb_Value idx = fb_int(S, i);
			fb_apply(S, func, &idx, 1);          /* runs now -- no list built */
		}
	}
	return fb_int(S, i);
}

/* ---- list ops ---------------------------------------------------------- */

static fb_Value bi_range(fb_State *S, fb_Value *a, int n) {
	long start = n == 1 ? 0 : fb_toint(a[0]);
	long stop  = n == 1 ? fb_toint(a[0]) : fb_toint(a[1]);
	fb_Value l = fb_list(S);
	for (long i = start; i < stop; i++) fb_push(S, l, fb_int(S, i));
	return l;
}

static fb_Value bi_enumerate(fb_State *S, fb_Value *a, int n) {
	(void)n; fb_Value l = fb_list(S);
	if (!seq(a[0])) return l;
	int len = fb_len(a[0]);
	for (int i = 0; i < len; i++) {
		fb_Value t = fb_tuple(S);
		fb_tuple_push(S, t, fb_int(S, i));
		fb_tuple_push(S, t, fb_at(a[0], i));
		fb_push(S, l, t);
	}
	return l;
}

static fb_Value bi_slice(fb_State *S, fb_Value *a, int n) {
	(void)n; fb_Value l = fb_list(S);
	if (!seq(a[0])) return l;
	long len = fb_len(a[0]), s = fb_toint(a[1]), e = fb_toint(a[2]);
	if (s < 0) s += len;
	if (e < 0) e += len;
	if (s < 0) s = 0;
	if (e > len) e = len;
	for (long i = s; i < e; i++) fb_push(S, l, fb_at(a[0], (int)i));
	return l;
}

static fb_Value bi_take(fb_State *S, fb_Value *a, int n) {
	(void)n; fb_Value l = fb_list(S);
	if (!seq(a[0])) return l;
	long len = fb_len(a[0]), c = fb_toint(a[1]);
	if (c < 0) c += len;
	if (c < 0) c = 0;
	if (c > len) c = len;
	for (long i = 0; i < c; i++) fb_push(S, l, fb_at(a[0], (int)i));
	return l;
}
static fb_Value bi_drop(fb_State *S, fb_Value *a, int n) {
	(void)n; fb_Value l = fb_list(S);
	if (!seq(a[0])) return l;
	long len = fb_len(a[0]), c = fb_toint(a[1]);
	if (c < 0) c += len;
	if (c < 0) c = 0;
	if (c > len) c = len;
	for (long i = c; i < len; i++) fb_push(S, l, fb_at(a[0], (int)i));
	return l;
}

static fb_Value bi_reverse(fb_State *S, fb_Value *a, int n) {
	(void)n; fb_Value l = fb_list(S);
	if (!seq(a[0])) return l;
	for (int i = fb_len(a[0]); i > 0; i--) fb_push(S, l, fb_at(a[0], i - 1));
	return l;
}

static fb_Value bi_flatten(fb_State *S, fb_Value *a, int n) {
	(void)n; fb_Value out = fb_list(S);
	if (!seq(a[0])) return out;
	int len = fb_len(a[0]);
	for (int i = 0; i < len; i++) {
		fb_Value e = fb_at(a[0], i);
		if (seq(e)) { int el = fb_len(e); for (int j = 0; j < el; j++) fb_push(S, out, fb_at(e, j)); }
		else fb_push(S, out, e);
	}
	return out;
}

static fb_Value bi_sort(fb_State *S, fb_Value *a, int n) {
	(void)n; fb_Value out = fb_list(S);
	if (!seq(a[0])) return out;
	int len = fb_len(a[0]);
	keyed *arr = malloc((size_t)len * sizeof *arr);
	for (int i = 0; i < len; i++) {
		fb_Value e = fb_at(a[0], i);
		arr[i] = (keyed){ fb_tostr(S, e), e };
	}
	qsort(arr, (size_t)len, sizeof *arr, cmp_keyed);
	for (int i = 0; i < len; i++) fb_push(S, out, arr[i].val);
	free(arr);
	return out;
}

/* first occurrence wins, by string key; open-addressed set -> O(n). */
static fb_Value bi_unique(fb_State *S, fb_Value *a, int n) {
	(void)n; fb_Value out = fb_list(S);
	if (!seq(a[0])) return out;
	int len = fb_len(a[0]);
	size_t cap = 16;
	while (cap < (size_t)len * 2) cap *= 2;
	const char **slot = calloc(cap, sizeof *slot);
	for (int i = 0; i < len; i++) {
		fb_Value e = fb_at(a[0], i);
		const char *si = fb_tostr(S, e);
		size_t h = khash(si) & (cap - 1);
		int dup = 0;
		while (slot[h]) {
			if (!strcmp(slot[h], si)) { dup = 1; break; }
			h = (h + 1) & (cap - 1);
		}
		if (!dup) { slot[h] = si; fb_push(S, out, e); }
	}
	free(slot);
	return out;
}

static fb_Value bi_count(fb_State *S, fb_Value *a, int n) {
	(void)n; long c = 0;
	if (seq(a[0])) {
		const char *s = fb_tostr(S, a[1]); int len = fb_len(a[0]);
		for (int i = 0; i < len; i++) if (!strcmp(fb_tostr(S, fb_at(a[0], i)), s)) c++;
	}
	return fb_int(S, c);
}

/* sum/min/max accept a single sequence or varargs: sum(L) == sum(1,2,3). */
static int  agg_len(fb_Value *a, int n)        { return (n == 1 && seq(a[0])) ? fb_len(a[0]) : n; }
static long agg_val(fb_Value *a, int n, int i) { return fb_toint((n == 1 && seq(a[0])) ? fb_at(a[0], i) : a[i]); }

static fb_Value bi_sum(fb_State *S, fb_Value *a, int n) {
	long t = 0; int c = agg_len(a, n);
	for (int i = 0; i < c; i++) t += agg_val(a, n, i);
	return fb_int(S, t);
}
static fb_Value bi_min(fb_State *S, fb_Value *a, int n) {
	int c = agg_len(a, n); if (!c) return fb_int(S, 0);
	long m = agg_val(a, n, 0);
	for (int i = 1; i < c; i++) { long v = agg_val(a, n, i); if (v < m) m = v; }
	return fb_int(S, m);
}
static fb_Value bi_max(fb_State *S, fb_Value *a, int n) {
	int c = agg_len(a, n); if (!c) return fb_int(S, 0);
	long m = agg_val(a, n, 0);
	for (int i = 1; i < c; i++) { long v = agg_val(a, n, i); if (v > m) m = v; }
	return fb_int(S, m);
}

/* fold(list, init, fn): left fold, fn called as fn(acc, elem). */
static fb_Value bi_fold(fb_State *S, fb_Value *a, int n) {
	(void)n; fb_Value acc = a[1];
	if (seq(a[0])) {
		int len = fb_len(a[0]);
		for (int i = 0; i < len; i++) {
			fb_Value args[2] = { acc, fb_at(a[0], i) };
			acc = fb_apply(S, a[2], args, 2);
		}
	}
	return acc;
}

/* index(list, val [, key]) -> first index where (key ? key(elem) : elem) == val, else -1 */
static fb_Value bi_index(fb_State *S, fb_Value *a, int n) {
	if (!seq(a[0])) return fb_int(S, -1);
	const char *s = fb_tostr(S, a[1]);
	fb_Value key = n >= 3 ? a[2] : (fb_Value)0;
	int len = fb_len(a[0]);
	for (int i = 0; i < len; i++) {
		fb_Value item = fb_at(a[0], i);
		fb_Value e = key ? fb_apply(S, key, &item, 1) : item;
		if (!strcmp(fb_tostr(S, e), s)) return fb_int(S, i);
	}
	return fb_int(S, -1);
}

int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	static const fb_Reg regs[] = {
		{ "iter", bi_iter, 1, -1, FB_RET_LIST, 0, NULL },
		{ "while", bi_while, 2, 2, FB_RET_INT, FB_LAZY, NULL },
		{ "range", bi_range, 1, 2, FB_RET_LIST, 0, NULL },
		{ "enumerate", bi_enumerate, 1, 1, FB_RET_LIST, 0, NULL },
		{ "slice", bi_slice, 3, 3, FB_RET_LIST, 0, NULL },
		{ "take", bi_take, 2, 2, FB_RET_LIST, 0, NULL },
		{ "drop", bi_drop, 2, 2, FB_RET_LIST, 0, NULL },
		{ "reverse", bi_reverse, 1, 1, FB_RET_LIST, 0, NULL },
		{ "flatten", bi_flatten, 1, 1, FB_RET_LIST, 0, NULL },
		{ "sort", bi_sort, 1, 1, FB_RET_LIST, 0, NULL },
		{ "unique", bi_unique, 1, 1, FB_RET_LIST, 0, NULL },
		{ "count", bi_count, 2, 2, FB_RET_INT, 0, NULL },
		{ "sum", bi_sum, 1, -1, FB_RET_INT, 0, NULL },
		{ "min", bi_min, 1, -1, FB_RET_INT, 0, NULL },
		{ "max", bi_max, 1, -1, FB_RET_INT, 0, NULL },
		{ "fold", bi_fold, 3, 3, FB_RET_ANY, 0, NULL },
		{ "index", bi_index, 2, 3, FB_RET_INT, 0, NULL },
	};
	for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
		fb_register(S, &regs[i]);
	return 0;
}
