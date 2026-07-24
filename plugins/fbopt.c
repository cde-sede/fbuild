/* fbopt -- optimization-focused builtins for fbuild, shipped as a plugin (.so).
 *
 * Build:  cc -shared -fPIC -Isrc plugins/fbopt.c -o build/fbopt.so
 *         (needs only fbuild.h; the fb_* symbols resolve at load time from the
 *          -rdynamic fbuild host, so the .so links against nothing.)
 * Load:   FBUILD_PLUGINS=build/fbopt.so ./fbuild        (or `./fbuild plugins`)
 *
 * Theme: filesystem-metadata memoization + precise/content-based staleness.
 * stale() re-stats every argument on every call; these builtins share one
 * process-lifetime cache so each distinct path is stat'd (and, for hashing,
 * read) at most once.
 *
 * Builtins:
 *   stalem(t, ...deps)  Bool   memoized stale(); nanosecond mtime compare
 *   mtimem(p)           Int    mtime ns since epoch, -1 if missing
 *   newest(...p)        File   path with latest mtime (nil if none)
 *   oldest(...p)        File   path with earliest mtime (nil if none)
 *   existsm(p)          Bool   memoized existence
 *   is_filem(p)         Bool   memoized regular-file test
 *   is_dirm(p)          Bool   memoized directory test
 *   globm(pat)          List   memoized single-dir glob "dir/​*.ext"
 *   needed(units,...d)  List   subset of units [obj,src] that are stale
 *   depsm(objfile)      List   parse `cc -MMD` sibling .d -> real header deps
 *   hashm(p)            Int    memoized FNV-1a content hash (0 if missing)
 *   stale_hash(t,...d)  Bool   rebuild only if a dep's CONTENT changed
 *   cache_load(file)    Bool   load the stale_hash baseline (path -> hash)
 *   cache_save(file)    Bool   write current dep hashes as the new baseline
 *   stat_reset()        Bool   drop the in-memory stat/glob caches
 *
 * Snapshot semantics: a path's mtime/hash is frozen at first access. Intended
 * for inputs that don't change during a run and targets each checked once; call
 * stat_reset() between phases if a cached path was rebuilt.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <time.h>

#include "fbuild.h"

/* ---- shared memoized stat/hash cache ---------------------------------- */
typedef struct {
	char              *path;
	struct timespec    mt;
	mode_t             mode;
	int                missing;
	uint64_t           hash;    /* FNV-1a of contents; computed lazily */
	int                hashed;
} ent;
static ent   *cache;
static size_t cache_n, cache_cap;

/* stat `path` once; the returned pointer is invalidated by the next stat_memo()
 * (the array may realloc), so copy anything you keep across another call. */
static ent *stat_memo(const char *path) {
	for (size_t i = 0; i < cache_n; i++)
		if (!strcmp(cache[i].path, path)) return &cache[i];
	if (cache_n == cache_cap) {
		cache_cap = cache_cap ? cache_cap * 2 : 64;
		cache = realloc(cache, cache_cap * sizeof *cache);
	}
	ent *e = &cache[cache_n++];
	e->path = strdup(path);          /* process-lifetime; freed by stat_reset */
	e->hash = 0; e->hashed = 0;
	struct stat st;
	if (stat(path, &st) != 0) { e->missing = 1; e->mt = (struct timespec){0}; e->mode = 0; }
	else                      { e->missing = 0; e->mt = st.st_mtim; e->mode = st.st_mode; }
	return e;
}

/* strict "a modified more recently than b", nanosecond resolution */
static int newer(struct timespec a, struct timespec b) {
	if (a.tv_sec != b.tv_sec) return a.tv_sec > b.tv_sec;
	return a.tv_nsec > b.tv_nsec;
}

/* FNV-1a 64 over the file's bytes; 0 if missing/unreadable. Memoized. */
static uint64_t hash_of(ent *e) {
	if (e->hashed) return e->hash;
	e->hashed = 1;
	if (e->missing) return e->hash = 0;
	FILE *f = fopen(e->path, "rb");
	if (!f) return e->hash = 0;
	uint64_t h = 1469598103934665603ULL;
	unsigned char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
	fclose(f);
	return e->hash = h;
}

/* ---- persistent baseline (path -> hash at last cache_save) ------------ */
typedef struct { char *path; uint64_t hash; } base_ent;
static base_ent *base;
static size_t    base_n, base_cap;

static int base_get(const char *path, uint64_t *out) {
	for (size_t i = 0; i < base_n; i++)
		if (!strcmp(base[i].path, path)) { *out = base[i].hash; return 1; }
	return 0;
}

/* ---- helper: fb_Value path -> C string (File/Str) --------------------- */
#define P(v) fb_tostr(S, (v))

/* =======================================================================
 *                              builtins
 * ======================================================================= */

/* stalem(target, ...deps) -> Bool */
static fb_Value bi_stalem(fb_State *S, fb_Value *a, int n) {
	if (n == 0) return fb_bool(S, 0);
	ent *t = stat_memo(P(a[0]));
	if (t->missing) return fb_bool(S, 1);
	struct timespec tmt = t->mt;
	for (int i = 1; i < n; i++) {
		ent *d = stat_memo(P(a[i]));
		if (d->missing || newer(d->mt, tmt)) return fb_bool(S, 1);
	}
	return fb_bool(S, 0);
}

/* mtimem(path) -> Int (ns since epoch, -1 if missing) */
static fb_Value bi_mtimem(fb_State *S, fb_Value *a, int n) {
	if (n < 1) return fb_int(S, -1);
	ent *e = stat_memo(P(a[0]));
	if (e->missing) return fb_int(S, -1);
	return fb_int(S, (long)e->mt.tv_sec * 1000000000L + e->mt.tv_nsec);
}

/* newest/oldest(...paths) -> File (nil if none exist) */
static fb_Value bi_extreme(fb_State *S, fb_Value *a, int n, int want_newest) {
	int best = -1;
	struct timespec bmt = {0};
	for (int i = 0; i < n; i++) {
		ent *e = stat_memo(P(a[i]));
		if (e->missing) continue;
		struct timespec m = e->mt;
		if (best < 0 || (want_newest ? newer(m, bmt) : newer(bmt, m))) { best = i; bmt = m; }
	}
	if (best < 0) return fb_nil(S);
	return fb_file(S, P(a[best]));
}
static fb_Value bi_newest(fb_State *S, fb_Value *a, int n) { return bi_extreme(S, a, n, 1); }
static fb_Value bi_oldest(fb_State *S, fb_Value *a, int n) { return bi_extreme(S, a, n, 0); }

/* existsm / is_filem / is_dirm(path) -> Bool */
static fb_Value bi_existsm (fb_State *S, fb_Value *a, int n) {
	(void)n; return fb_bool(S, !stat_memo(P(a[0]))->missing);
}
static fb_Value bi_is_filem(fb_State *S, fb_Value *a, int n) {
	(void)n; ent *e = stat_memo(P(a[0])); return fb_bool(S, !e->missing && S_ISREG(e->mode));
}
static fb_Value bi_is_dirm (fb_State *S, fb_Value *a, int n) {
	(void)n; ent *e = stat_memo(P(a[0])); return fb_bool(S, !e->missing && S_ISDIR(e->mode));
}

/* ---- memoized single-directory glob ----------------------------------- */
typedef struct { char *pat; char **items; size_t n; } glob_ent;
static glob_ent *globs;
static size_t    globs_n, globs_cap;

static int cmpstr(const void *x, const void *y) {
	return strcmp(*(char *const *)x, *(char *const *)y);
}

/* glob "dir/<wild>" once (single level); cache the resulting path strings. */
static const glob_ent *glob_memo(const char *pat) {
	for (size_t i = 0; i < globs_n; i++)
		if (!strcmp(globs[i].pat, pat)) return &globs[i];
	if (globs_n == globs_cap) {
		globs_cap = globs_cap ? globs_cap * 2 : 16;
		globs = realloc(globs, globs_cap * sizeof *globs);
	}
	glob_ent *g = &globs[globs_n++];
	g->pat = strdup(pat); g->items = NULL; g->n = 0;

	const char *slash = strrchr(pat, '/');
	char dir[4096]; const char *wild;
	if (slash) {
		size_t dl = (size_t)(slash - pat);
		if (dl >= sizeof dir) dl = sizeof dir - 1;
		memcpy(dir, pat, dl); dir[dl] = '\0'; wild = slash + 1;
	} else { dir[0] = '.'; dir[1] = '\0'; wild = pat; }

	DIR *d = opendir(dir);
	if (!d) return g;
	size_t cap = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2])))
			continue;                                   /* skip . and .. */
		if (fnmatch(wild, de->d_name, 0) != 0) continue;
		if (g->n == cap) { cap = cap ? cap * 2 : 16; g->items = realloc(g->items, cap * sizeof *g->items); }
		char full[8192];
		snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
		g->items[g->n++] = strdup(full);
	}
	closedir(d);
	qsort(g->items, g->n, sizeof *g->items, cmpstr);
	return g;
}

/* globm(pattern) -> List<File>. Single directory level (dir/<wildcard>). */
static fb_Value bi_globm(fb_State *S, fb_Value *a, int n) {
	(void)n;
	const glob_ent *g = glob_memo(P(a[0]));
	fb_Value list = fb_list(S);
	for (size_t i = 0; i < g->n; i++) fb_push(S, list, fb_file(S, g->items[i]));
	return list;
}

/* needed(units, ...deps) -> List: the units [obj, src, ...] that are stale.
 * A unit is stale if its obj (element 0) is missing or older than its src
 * (element 1) or any shared dep. Reuses the memoized mtime cache. */
static fb_Value bi_needed(fb_State *S, fb_Value *a, int n) {
	fb_Value out = fb_list(S);
	if (n == 0) return out;
	fb_Value units = a[0];
	int ulen = fb_len(units);
	for (int u = 0; u < ulen; u++) {
		fb_Value unit = fb_at(units, u);
		if (fb_len(unit) < 1) continue;
		ent *obj = stat_memo(P(fb_at(unit, 0)));
		int stale = obj->missing;
		if (!stale) {
			struct timespec omt = obj->mt;
			if (fb_len(unit) >= 2) {                    /* src = element 1 */
				ent *src = stat_memo(P(fb_at(unit, 1)));
				if (src->missing || newer(src->mt, omt)) stale = 1;
			}
			for (int i = 1; !stale && i < n; i++) {     /* shared deps */
				ent *d = stat_memo(P(a[i]));
				if (d->missing || newer(d->mt, omt)) stale = 1;
			}
		}
		if (stale) fb_push(S, out, unit);
	}
	return out;
}

/* depsm(objfile) -> List<File>: parse the sibling `cc -MMD` depfile
 * (<obj without .o>.d) into the real prerequisite list. Empty if no depfile
 * (e.g. first build) -- the object is missing then anyway, so stale() rebuilds. */
static fb_Value bi_depsm(fb_State *S, fb_Value *a, int n) {
	(void)n;
	fb_Value out = fb_list(S);
	const char *obj = P(a[0]);
	size_t ol = strlen(obj);
	char df[4096];
	if (ol + 1 >= sizeof df) return out;
	memcpy(df, obj, ol + 1);
	if (ol >= 2 && !strcmp(df + ol - 2, ".o")) { df[ol - 1] = 'd'; }  /* x.o -> x.d */
	else { if (ol + 2 >= sizeof df) return out; strcpy(df + ol, ".d"); }

	FILE *f = fopen(df, "rb");
	if (!f) return out;
	/* whole file, then tokenize past the first ':' on whitespace, dropping the
	 * '\\' line-continuations. */
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	if (sz <= 0) { fclose(f); return out; }
	char *buf = malloc((size_t)sz + 1);
	size_t got = fread(buf, 1, (size_t)sz, f); fclose(f);
	buf[got] = '\0';

	char *colon = strchr(buf, ':');
	char *p = colon ? colon + 1 : buf;
	char *tok = NULL;
	for (; *p; p++) {
		char c = *p;
		if (c == '\\' && (p[1] == '\n' || p[1] == '\r')) { c = ' '; }  /* continuation */
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\\') {
			if (tok) { *p = '\0'; if (*tok) fb_push(S, out, fb_file(S, tok)); tok = NULL; }
		} else if (!tok) tok = p;
	}
	if (tok && *tok) fb_push(S, out, fb_file(S, tok));
	free(buf);
	return out;
}

/* hashm(path) -> Int (FNV-1a of contents; 0 if missing). Memoized. */
static fb_Value bi_hashm(fb_State *S, fb_Value *a, int n) {
	(void)n; return fb_int(S, (long)hash_of(stat_memo(P(a[0]))));
}

/* stale_hash(target, ...deps) -> Bool: rebuild if target missing, or any dep's
 * content hash differs from the loaded baseline (cache_load). Content-based, so
 * immune to mtime churn (touch / checkout). Run cache_save() after building to
 * record the new baseline. With no baseline loaded, everything is stale. */
static fb_Value bi_stale_hash(fb_State *S, fb_Value *a, int n) {
	if (n == 0) return fb_bool(S, 0);
	if (stat_memo(P(a[0]))->missing) return fb_bool(S, 1);
	for (int i = 1; i < n; i++) {
		ent *d = stat_memo(P(a[i]));
		if (d->missing) return fb_bool(S, 1);
		uint64_t h = hash_of(d), b;
		if (!base_get(d->path, &b) || b != h) return fb_bool(S, 1);
	}
	return fb_bool(S, 0);
}

/* cache_load(file) -> Bool: read "hexhash path" lines into the baseline. */
static fb_Value bi_cache_load(fb_State *S, fb_Value *a, int n) {
	(void)n;
	FILE *f = fopen(P(a[0]), "rb");
	if (!f) return fb_bool(S, 0);
	char line[8192];
	while (fgets(line, sizeof line, f)) {
		char *sp = strchr(line, ' ');
		if (!sp) continue;
		*sp = '\0';
		char *path = sp + 1;
		size_t pl = strlen(path);
		while (pl && (path[pl-1] == '\n' || path[pl-1] == '\r')) path[--pl] = '\0';
		if (!pl) continue;
		if (base_n == base_cap) { base_cap = base_cap ? base_cap * 2 : 64; base = realloc(base, base_cap * sizeof *base); }
		base[base_n].hash = strtoull(line, NULL, 16);
		base[base_n].path = strdup(path);
		base_n++;
	}
	fclose(f);
	return fb_bool(S, 1);
}

/* cache_save(file) -> Bool: write the current content hash of every file hashed
 * this run (i.e. every stale_hash/hashm dep) as the next baseline. */
static fb_Value bi_cache_save(fb_State *S, fb_Value *a, int n) {
	(void)n;
	FILE *f = fopen(P(a[0]), "wb");
	if (!f) return fb_bool(S, 0);
	for (size_t i = 0; i < cache_n; i++)
		if (cache[i].hashed && !cache[i].missing)
			fprintf(f, "%016llx %s\n", (unsigned long long)cache[i].hash, cache[i].path);
	fclose(f);
	return fb_bool(S, 1);
}

/* stat_reset() -> Bool: drop the in-memory stat + glob caches (not the loaded
 * baseline). Call between phases if a cached path was rebuilt. */
static fb_Value bi_stat_reset(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n;
	for (size_t i = 0; i < cache_n; i++) free(cache[i].path);
	free(cache); cache = NULL; cache_n = cache_cap = 0;
	for (size_t i = 0; i < globs_n; i++) {
		for (size_t j = 0; j < globs[i].n; j++) free(globs[i].items[j]);
		free(globs[i].items); free(globs[i].pat);
	}
	free(globs); globs = NULL; globs_n = globs_cap = 0;
	return fb_bool(S, 1);
}

/* Hover documentation, same convention as src/builtin_docs.h (core's
 * centralized table): summary + **Params** + **Returns** + **Example**.
 * Plugins are separate translation units so this lives locally instead of
 * sharing that header -- same fb_Reg.doc ABI field either way. */

#define FBDOC_stalem \
"Memoized `stale()`: a path's mtime is stat'd once per run and reused for every check, instead of stat'd again on each call.\n" \
"\n" \
"**Params**\n" \
"- `target` — the output `File`\n" \
"- `deps...` — zero or more dependency `File`s\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"run(cmd) when stalem(obj, src, ...HDRS)\n" \
"```"

#define FBDOC_mtimem \
"Reads a path's modification time (memoized -- one stat per path per run).\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `Int` nanoseconds since epoch, or `-1` if missing.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"mtimem('build/app')\n" \
"```"

#define FBDOC_newest \
"Finds the most recently modified of several paths (memoized stats).\n" \
"\n" \
"**Params**\n" \
"- `paths...` — one or more `Path`s\n" \
"\n" \
"**Returns** the newest `File`, or `nil` if none of them exist.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"newest(...glob('src/*.c'))\n" \
"```"

#define FBDOC_oldest \
"Finds the least recently modified of several paths (memoized stats).\n" \
"\n" \
"**Params**\n" \
"- `paths...` — one or more `Path`s\n" \
"\n" \
"**Returns** the oldest `File`, or `nil` if none of them exist.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"oldest(...glob('src/*.c'))\n" \
"```"

#define FBDOC_existsm \
"Memoized `exists()` -- one stat per path per run.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"existsm('build/app')\n" \
"```"

#define FBDOC_is_filem \
"Memoized regular-file test -- one stat per path per run.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"is_filem('build/app')\n" \
"```"

#define FBDOC_is_dirm \
"Memoized directory test -- one stat per path per run.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"is_dirm('build')\n" \
"```"

#define FBDOC_globm \
"Memoized single-directory glob (`dir/<wildcard>` -- one level, not recursive `**`): each distinct pattern is only read from disk once per run.\n" \
"\n" \
"**Params**\n" \
"- `pattern` — a glob pattern, e.g. `'src/*.c'`\n" \
"\n" \
"**Returns** `List<File>`, sorted.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"globm('src/*.c')\n" \
"```"

#define FBDOC_needed \
"Filters a list of [obj, src] (or [obj, src, ...] ) unit pairs down to the ones that are stale, sharing one memoized stat cache across every unit and dep -- avoids the O(units x deps) restat a plain `stale()` loop would do.\n" \
"\n" \
"**Params**\n" \
"- `units` — `List` of `[obj, src, ...]` tuples/lists (obj = element 0, src = element 1)\n" \
"- `deps...` — shared dependencies checked against every unit (e.g. common headers)\n" \
"\n" \
"**Returns** the subset of `units` that are stale.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"todo = needed(zip(objs, srcs), ...HDRS)\n" \
"```"

#define FBDOC_depsm \
"Parses a compiler-generated `.d` depfile (`cc -MMD`, sibling of the given object file) into its real prerequisite list. Returns an empty list if there's no depfile yet (first build -- the object is missing then anyway, so `stale()` rebuilds regardless).\n" \
"\n" \
"**Params**\n" \
"- `objfile` — the object `Path` (its sibling `.d` is read, e.g. `a.o` -> `a.d`)\n" \
"\n" \
"**Returns** `List<File>`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"hdrs = depsm(obj)\n" \
"```"

#define FBDOC_hashm \
"Content hash of a file (FNV-1a over its bytes), memoized -- read from disk at most once per run.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `Int`, or `0` if missing.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"hashm('src/main.c')\n" \
"```"

#define FBDOC_stale_hash \
"Content-based staleness: rebuild if the target is missing, or if any dep's content hash differs from the loaded baseline (see `cache_load`). Immune to mtime churn (touch, checkout, clock skew) -- unlike `stale()`/`stalem()`. With no baseline loaded, everything is stale.\n" \
"\n" \
"**Params**\n" \
"- `target` — the output `File`\n" \
"- `deps...` — zero or more dependency `File`s\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"cache_load('build/.hashes')\n" \
"run(cmd) when stale_hash(obj, src)\n" \
"cache_save('build/.hashes')\n" \
"```"

#define FBDOC_cache_load \
"Loads a `stale_hash` baseline (path -> content hash) from a file written by an earlier `cache_save`.\n" \
"\n" \
"**Params**\n" \
"- `path` — the baseline `File`\n" \
"\n" \
"**Returns** `Bool` -- `false` if the file couldn't be opened (no prior baseline; everything counts as stale).\n" \
"\n" \
"**Example**\n" \
"```\n" \
"cache_load('build/.hashes')\n" \
"```"

#define FBDOC_cache_save \
"Writes the content hash of every file hashed so far this run (via `stale_hash`/`hashm`) as the next baseline. Call after building, so the next run's `stale_hash` compares against what was actually just built.\n" \
"\n" \
"**Params**\n" \
"- `path` — where to write the baseline\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"cache_save('build/.hashes')\n" \
"```"

#define FBDOC_stat_reset \
"Drops the in-memory stat/glob caches (not the loaded `stale_hash` baseline) -- call between phases if a path this run already stat'd was then rebuilt, so the next check re-stats it instead of reusing a stale snapshot.\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"stat_reset()\n" \
"```"

/* ---- plugin entry points (dlsym'd by fb_load_plugin) ------------------ */
int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	static const fb_Reg regs[] = {
		{ "stalem", bi_stalem, 1, -1, FB_RET_BOOL, 0, FBDOC_stalem },
		{ "mtimem", bi_mtimem, 1, 1, FB_RET_INT, 0, FBDOC_mtimem },
		{ "newest", bi_newest, 1, -1, FB_RET_FILE, 0, FBDOC_newest },
		{ "oldest", bi_oldest, 1, -1, FB_RET_FILE, 0, FBDOC_oldest },
		{ "existsm", bi_existsm, 1, 1, FB_RET_BOOL, 0, FBDOC_existsm },
		{ "is_filem", bi_is_filem, 1, 1, FB_RET_BOOL, 0, FBDOC_is_filem },
		{ "is_dirm", bi_is_dirm, 1, 1, FB_RET_BOOL, 0, FBDOC_is_dirm },
		{ "globm", bi_globm, 1, 1, FB_RET_LIST, 0, FBDOC_globm },
		{ "needed", bi_needed, 1, -1, FB_RET_LIST, 0, FBDOC_needed },
		{ "depsm", bi_depsm, 1, 1, FB_RET_LIST, 0, FBDOC_depsm },
		{ "hashm", bi_hashm, 1, 1, FB_RET_INT, 0, FBDOC_hashm },
		{ "stale_hash", bi_stale_hash, 1, -1, FB_RET_BOOL, 0, FBDOC_stale_hash },
		{ "cache_load", bi_cache_load, 1, 1, FB_RET_BOOL, 0, FBDOC_cache_load },
		{ "cache_save", bi_cache_save, 1, 1, FB_RET_BOOL, 0, FBDOC_cache_save },
		{ "stat_reset", bi_stat_reset, 0, 0, FB_RET_BOOL, 0, FBDOC_stat_reset },
	};
	for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
		fb_register(S, &regs[i]);
	return 0;
}
