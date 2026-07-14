/* fbopt -- optimization-focused builtins for fbuild, shipped as a plugin (.so).
 *
 * Build:  cc -shared -fPIC -Isrc plugins/fbopt.c -o build/fbopt.so
 *         (needs only fbuild.h; the fb_* symbols resolve at load time from the
 *          -rdynamic fbuild host, so the plugin links against nothing.)
 * Load:   FBUILD_PLUGINS=build/fbopt.so ./fbuild
 *         (colon-separate several; loaded before the script so the type checker
 *          sees the names.)
 *
 * Theme: filesystem-metadata memoization. stale() re-stats every argument on
 * every call, so `stale(.o, ...s, ...HDRS)` run once per object re-stats the
 * whole shared header list N times -- O(objects x headers) syscalls. Every
 * builtin here shares one process-lifetime mtime cache, so each distinct path
 * is stat'd once and reused.
 *
 * Snapshot semantics: a path's mtime is frozen at first access. Intended for
 * inputs that do not change during the build (sources, headers) and targets
 * each checked once. If a cached path is rebuilt and must be re-checked in the
 * same run, call stat_reset() first.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "fbuild.h"

/* ---- shared memoized stat cache --------------------------------------- */
typedef struct { char *path; struct timespec mt; int missing; } ent;
static ent   *cache;
static size_t cache_n, cache_cap;

/* stat `path` once; subsequent calls return the cached entry. The returned
 * pointer is invalidated by the next stat_memo() (the array may realloc), so
 * copy any field you need to keep across another call. */
static const ent *stat_memo(const char *path) {
	for (size_t i = 0; i < cache_n; i++)
		if (!strcmp(cache[i].path, path)) return &cache[i];
	if (cache_n == cache_cap) {
		cache_cap = cache_cap ? cache_cap * 2 : 64;
		cache = realloc(cache, cache_cap * sizeof *cache);
	}
	ent *e = &cache[cache_n++];
	e->path = strdup(path);          /* process-lifetime; freed only by stat_reset */
	struct stat st;
	if (stat(path, &st) != 0) { e->missing = 1; e->mt = (struct timespec){0}; }
	else                      { e->missing = 0; e->mt = st.st_mtim; }
	return e;
}

/* strict "a modified more recently than b", nanosecond resolution */
static int newer(struct timespec a, struct timespec b) {
	if (a.tv_sec != b.tv_sec) return a.tv_sec > b.tv_sec;
	return a.tv_nsec > b.tv_nsec;
}

/* ---- builtins --------------------------------------------------------- */

/* stalem(target, ...deps) -> Bool: does target need rebuilding? True if target
 * is missing, or any dep is missing, or any dep is newer than target. Memoized;
 * nanosecond comparison (unlike whole-second build-level checks). */
static fb_Value bi_stalem(fb_State *S, fb_Value *a, int n) {
	if (n == 0) return fb_bool(S, 0);
	const ent *t = stat_memo(fb_tostr(S, a[0]));
	if (t->missing) return fb_bool(S, 1);
	struct timespec tmt = t->mt;            /* copy: cache may realloc below */
	for (int i = 1; i < n; i++) {
		const ent *d = stat_memo(fb_tostr(S, a[i]));
		if (d->missing || newer(d->mt, tmt)) return fb_bool(S, 1);
	}
	return fb_bool(S, 0);
}

/* mtimem(path) -> Int: mtime in nanoseconds since the epoch, or -1 if the path
 * is missing. Memoized. */
static fb_Value bi_mtimem(fb_State *S, fb_Value *a, int n) {
	if (n < 1) return fb_int(S, -1);
	const ent *e = stat_memo(fb_tostr(S, a[0]));
	if (e->missing) return fb_int(S, -1);
	return fb_int(S, (long)e->mt.tv_sec * 1000000000L + e->mt.tv_nsec);
}

/* newest(...paths) -> File: the path with the latest mtime (nil if none exist).
 * Memoized -- e.g. `stalem(target, newest(...deps))` checks against one file. */
static fb_Value bi_newest(fb_State *S, fb_Value *a, int n) {
	int best = -1;
	struct timespec bmt = {0};
	for (int i = 0; i < n; i++) {
		const ent *e = stat_memo(fb_tostr(S, a[i]));
		if (e->missing) continue;
		struct timespec m = e->mt;          /* copy before the next stat_memo */
		if (best < 0 || newer(m, bmt)) { best = i; bmt = m; }
	}
	if (best < 0) return fb_nil(S);
	return fb_file(S, fb_tostr(S, a[best]));
}

/* stat_reset() -> Bool: drop the mtime cache. Call between build phases if a
 * cached path was rebuilt and must be re-stat'd. */
static fb_Value bi_stat_reset(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n;
	for (size_t i = 0; i < cache_n; i++) free(cache[i].path);
	free(cache);
	cache = NULL; cache_n = cache_cap = 0;
	return fb_bool(S, 1);
}

/* ---- plugin entry points (dlsym'd by fb_load_plugin) ------------------ */
int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	static const fb_Reg regs[] = {
		{ "stalem",     bi_stalem,     1, -1, FB_RET_BOOL, 0 },
		{ "mtimem",     bi_mtimem,     1,  1, FB_RET_INT,  0 },
		{ "newest",     bi_newest,     1, -1, FB_RET_FILE, 0 },
		{ "stat_reset", bi_stat_reset, 0,  0, FB_RET_BOOL, 0 },
	};
	for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
		fb_register(S, &regs[i]);
	return 0;
}
