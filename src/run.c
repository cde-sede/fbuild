/* Executor: a tree-walking interpreter that runs the build through the cds-lib
 * runtime (cds_lib/build.h + connex.h + subst.c).
 *
 * The earlier stages stay lazy/symbolic; this one forces values and causes
 * side effects. glob() walks the filesystem, a morphism rewrites a path with
 * _subst(), `|>` maps a function over a list, a lambda beta-reduces
 * (destructuring its tuple argument), a `...` command interpolates its parts
 * into an argv, stale() defers to build_need_rebuild(), a guard gates on its
 * `when`, `>>` sequences, and run(x) forces x and feeds every command it
 * reaches to build_seq_visual() (which drives the connex event loop). */

#define _GNU_SOURCE

/* cds-lib runtime preamble (mirrors cds-lib/build.c) */
#define BUILD_DA_GENERIC_EXTRA(func)   \
    DA_DECL_GENERIC(char, func)        \
    DA_DECL_GENERIC(voidptr, func)     \
    DA_DECL_GENERIC(int, func)
#define BUILD_TAIL_LINES 5
#define BUILD_IMPLEMENTATION

/* embed a version string in the .my_metadata section via build.h's METADATA
 * mechanism; build_info.version is read back by fbuild_version(). */
#define FBUILD_VERSION "0.1.13"
#define _version FBUILD_VERSION
#define METADATA_LIST(X) X(version, FBUILD_VERSION)
#include "cds_lib/build.h"

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>

typedef void* voidptr;
typedef DECL_DA(voidptr) t_da_voidptr;
DECL_DA_FUNCTIONS(voidptr, 128, realloc, free);

typedef DECL_DA(char) t_string;
DECL_DA_FUNCTIONS(char, 1024, realloc, free);

typedef DECL_DA(int) t_da_int;
DECL_DA_FUNCTIONS(int, 256, realloc, free);

#include "subst.c"
#include "connex.h"

#include "ast.h"
#include "cparser.h"
#include "fbuild.h"

#include <setjmp.h>
#include <stdarg.h>
#include <dlfcn.h>

#define fatal(...) do { fprintf(stderr, "run: " __VA_ARGS__); \
                        fputc('\n', stderr); exit(1); } while (0)

extern char **environ;

/* ---- runtime values --------------------------------------------------- */

typedef enum {
	RV_NONE, RV_INT, RV_STR, RV_BOOL,
	RV_FILE,        /* a path (.s) + lazily derived fields                */
	RV_LIST,        /* independent items (glob / |> map) -> parallel      */
	RV_SEQ,         /* ordered dependency (>>) -> sequential              */
	RV_TUPLE,
	RV_CMD,         /* an argv (.items) ready to spawn                    */
	RV_LAMBDA, RV_MORPH,
	RV_BUILTIN,     /* a builtin referenced as a value (.s = name)        */
	RV_MAP,         /* JSON object: keys[] -> items[] (insertion order)   */
	RV_THUNK,       /* unevaluated arg to an FB_LAZY builtin (body + env)  */
	RV_OBJECT,      /* tagged object: cls + fields(keys/items) + nat data  */
	RV_CFUN,        /* native callable (nat = fb_CFunction); a method impl */
} rkind;

typedef struct rval rval;
typedef struct renv renv;

struct rval {
	rkind   k;
	long    i;          /* int / bool                          */
	char   *s;          /* string / file path                  */
	rval  **items;      /* list / tuple / cmd args; map values */
	char  **keys;       /* map keys, parallel to items         */
	size_t  n;
	size_t  cap;        /* allocated slots in items            */
	ast    *params;     /* lambda                              */
	ast    *body;
	renv   *env;        /* lambda closure                      */
	char   *from, *to;  /* morphism patterns                   */
	char   *label;      /* def name, for group display         */
	void   *cls;        /* RV_OBJECT: its class (struct fb_class*) */
	void   *nat;        /* RV_OBJECT: native data; RV_CFUN: fb_CFunction */
};

struct rbind { token *name; rval *val; struct rbind *next; };
struct renv  { struct rbind *head; rval *arg; renv *parent; };

static rval RV_UNIT = { .k = RV_NONE };

/* All runtime allocations come from the shared program arena (a_alloc / a_grow
 * / a_dup, declared in cparser.h, created by parser.c). Nothing is freed per
 * object. The only non-arena memory is libc-owned (open_memstream / realpath)
 * and the cds-lib dynamic-array argv, which keep their own malloc/free. */

/* ---- helpers ---------------------------------------------------------- */

static rval *rv(rkind k) {
	rval *v = a_alloc(sizeof *v);
	v->k = k;
	return v;
}
static inline rval *rv_int(long i)  { rval *v = rv(RV_INT);  v->i = i; return v; }
static inline rval *rv_bool(int b)  { rval *v = rv(RV_BOOL); v->i = !!b; return v; }
static inline rval *rv_str(char *s) { rval *v = rv(RV_STR);  v->s = s; return v; }
static inline rval *rv_file(char *s){ rval *v = rv(RV_FILE); v->s = s; return v; }

static void rv_push(rval *list, rval *e) {
	if (list->n == list->cap) {
		size_t nc = list->cap ? list->cap * 2 : 4;
		list->items = a_grow(list->items, list->n * sizeof *list->items,
		                     nc * sizeof *list->items);
		list->cap = nc;
	}
	list->items[list->n++] = e;
}

/* _subst (subst.c) returns a libc malloc'd string; move it into the arena. */
static char *subst_a(const char *from, const char *to, const char *s) {
	char *r = _subst(from, to, s);
	char *a = a_dup(r ? r : "");
	free(r);
	return a;
}

/* map (JSON object) helpers: insertion-ordered key->value, linear lookup. */
static void map_set(rval *m, char *key, rval *val) {
	for (size_t i = 0; i < m->n; i++)
		if (!strcmp(m->keys[i], key)) { m->items[i] = val; return; }
	if (m->n == m->cap) {
		size_t nc = m->cap ? m->cap * 2 : 4;
		m->items = a_grow(m->items, m->n * sizeof *m->items, nc * sizeof *m->items);
		m->keys  = a_grow(m->keys,  m->n * sizeof *m->keys,  nc * sizeof *m->keys);
		m->cap = nc;
	}
	m->keys[m->n] = key;
	m->items[m->n] = val;
	m->n++;
}
static rval *map_get(rval *m, const char *key) {
	if (!m || (m->k != RV_MAP && m->k != RV_OBJECT)) return &RV_UNIT;
	for (size_t i = 0; i < m->n; i++)
		if (!strcmp(m->keys[i], key)) return m->items[i];
	return &RV_UNIT;
}

static char *dup_tok(token *t) {
	return a_dupn(t->string, t->len);
}

/* decode C-style escape sequences in a string literal's body (\n \t \\ ...) */
static char *str_unescape(const char *src, size_t len) {
	char *out = a_alloc(len + 1);
	size_t j = 0;
	for (size_t i = 0; i < len; i++) {
		if (src[i] != '\\' || i + 1 >= len) { out[j++] = src[i]; continue; }
		switch (src[++i]) {
		case 'n':  out[j++] = '\n'; break;
		case 't':  out[j++] = '\t'; break;
		case 'r':  out[j++] = '\r'; break;
		case '0':  out[j++] = '\0'; break;
		case 'a':  out[j++] = '\a'; break;
		case 'b':  out[j++] = '\b'; break;
		case 'f':  out[j++] = '\f'; break;
		case 'v':  out[j++] = '\v'; break;
		default:   out[j++] = src[i]; break;   /* \\ \' \" and unknown: literal */
		}
	}
	out[j] = '\0';
	return out;
}

static int tok_eq(token *a, token *b) {
	return a->len == b->len && !memcmp(a->string, b->string, a->len);
}
static int tok_is(token *t, const char *s) {
	size_t n = strlen(s);
	return t && t->len == n && !memcmp(t->string, s, n);
}
static long tok_index(token *t) {
	if (!t || !t->len) return -1;
	long v = 0;
	for (size_t i = 0; i < t->len; i++) {
		if (t->string[i] < '0' || t->string[i] > '9') return -1;
		v = v * 10 + (t->string[i] - '0');
	}
	return v;
}

/* a runtime value rendered as a command-line argument */
static char *as_cstr(rval *v) {
	if (!v) return "";
	switch (v->k) {
	case RV_STR:
	case RV_FILE: return v->s ? v->s : "";
	case RV_INT:
	case RV_BOOL: { char *b = a_alloc(24); snprintf(b, 24, "%ld", v->i); return b; }
	default:      return "";
	}
}

/* ---- environment ------------------------------------------------------ */

static void env_put(renv *e, token *name, rval *val) {
	struct rbind *b = a_alloc(sizeof *b);
	b->name = name; b->val = val; b->next = e->head; e->head = b;
}
static rval *env_get(renv *e, token *name) {
	for (; e; e = e->parent)
		for (struct rbind *b = e->head; b; b = b->next)
			if (tok_eq(b->name, name)) return b->val;
	return NULL;
}
static rval *cur_arg(renv *e) {
	for (; e; e = e->parent)
		if (e->arg) return e->arg;
	return NULL;
}

/* ---- top-level defs (lazy, memoized) ---------------------------------- */

typedef struct { token *name; ast *rhs; rval *val; int doing; int pure; } def;
static def    g_defs[512];
static size_t g_ndefs;
static renv   g_top;
static t_env  g_env;
static rval  *g_args;          /* the `args` builtin variable (List<String>) */
static rval  *g_cwd;           /* the `cwd` builtin variable (String) */

/* print/debug must not write while a run owns the terminal (the cds-lib runtime
 * is drawing the build graph). Output produced during a run is queued and
 * flushed once the event loop finishes. */
static int    g_in_run;
static int    g_bench;          /* --bench on the CLI: emit bench() timings */
static char **g_print_q;       /* arena-backed; lines are arena strings */
static size_t g_print_n, g_print_cap;

/* line must be arena-owned (see do_print). */
static void print_emit(char *line) {
	if (g_in_run) {
		if (g_print_n == g_print_cap) {
			size_t nc = g_print_cap ? g_print_cap * 2 : 8;
			g_print_q = a_grow(g_print_q, g_print_n * sizeof *g_print_q,
			                   nc * sizeof *g_print_q);
			g_print_cap = nc;
		}
		g_print_q[g_print_n++] = line;
	} else {
		fputs(line, stdout);
		fputc('\n', stdout);
	}
}

static void print_flush(void) {
	for (size_t i = 0; i < g_print_n; i++) {
		fputs(g_print_q[i], stdout);
		fputc('\n', stdout);
	}
	g_print_n = 0;
}

static rval *eval(ast *n, renv *e);
static rval *do_shell(rval *v);
static rval *do_pkgconfig(const char *name);

/* Add one tuple-literal element, flattening splats and dropping elements whose
 * `when` guard is false (so `(...RANGE(n-1) when n>0, n)` collapses cleanly). */
static void tuple_add(rval *t, ast *el, renv *e) {
	if (el->kind == AST_GUARD) {
		ast *where = el->kids[1];
		ast *whenc = el->kids[2];
		/* arena-allocate the where-scope: a bound value may be a lambda or a
		 * deferred `>>` that captures this env and outlives this frame. */
		renv *bsc = e;
		if (where) {
			renv *ge = a_alloc(sizeof *ge);
			ge->parent = e;
			ge->arg    = cur_arg(e);
			for (size_t i = 0; i < where->nkids; i++) {
				ast *bd = where->kids[i];
				env_put(ge, bd->tok, eval(bd->kids[0], ge));
			}
			bsc = ge;
		}
		if (whenc) {
			rval *c = eval(whenc->kids[0], bsc);
			if (!(c->k == RV_BOOL && c->i))     /* gated out */
				return;
		}
		tuple_add(t, el->kids[0], bsc);
		return;
	}
	if (el->kind == AST_SPLAT) {
		rval *v = eval(el->kids[0], e);
		if (v->k == RV_LIST || v->k == RV_TUPLE)
			for (size_t j = 0; j < v->n; j++) rv_push(t, v->items[j]);
		else if (v->k != RV_NONE)
			rv_push(t, v);
		return;
	}
	rval *v = eval(el, e);
	if (v->k != RV_NONE)
		rv_push(t, v);
}

/* effectful builtins must re-run on every use, so a def whose evaluation
 * reaches one is never memoized. (Lambda bodies are skipped: their effects
 * happen at apply, not when the def is evaluated to a closure.) */
static int effectful_call(ast *n) {
	if (n->kind != AST_CALL || n->kids[0]->kind != AST_ID) return 0;
	token *c = n->kids[0]->tok;
	return tok_is(c, "shell") || tok_is(c, "run")
	    || tok_is(c, "mkdir") || tok_is(c, "write")
	    || tok_is(c, "move")  || tok_is(c, "copy")
	    || tok_is(c, "symlink") || tok_is(c, "remove") || tok_is(c, "touch");
}
static int ast_pure(ast *n) {
	if (!n) return 1;
	if (n->kind == AST_FORCE) return 0;         /* ~x re-evals: never memoize */
	if (effectful_call(n)) return 0;
	if (n->kind == AST_LAMBDA) return 1;        /* body deferred to apply */
	for (size_t i = 0; i < n->nkids; i++)
		if (!ast_pure(n->kids[i])) return 0;
	return 1;
}

static rval *def_lookup(token *name) {
	for (size_t i = 0; i < g_ndefs; i++)
		if (tok_eq(g_defs[i].name, name)) {
			def *d = &g_defs[i];
			if (d->pure && d->val) return d->val;   /* memoized (pure only) */
			if (d->doing) fatal("cyclic definition '%.*s'",
			                    (int)name->len, name->string);
			d->doing = 1;
			rval *v = eval(d->rhs, &g_top);
			d->doing = 0;
			if (v && !v->label)
				v->label = dup_tok(name);           /* group label */
			if (d->pure) d->val = v;                /* cache pure defs only */
			return v;
		}
	return NULL;
}

/* like def_lookup but ignores (and does not populate) the memo cache: the rhs
 * is re-evaluated every call. Backs the "~x" force operator. */
static rval *def_force(token *name) {
	for (size_t i = 0; i < g_ndefs; i++)
		if (tok_eq(g_defs[i].name, name)) {
			def *d = &g_defs[i];
			if (d->doing) fatal("cyclic definition '%.*s'",
			                    (int)name->len, name->string);
			d->doing = 1;
			rval *v = eval(d->rhs, &g_top);
			d->doing = 0;
			if (v && !v->label) v->label = dup_tok(name);
			return v;
		}
	return NULL;
}

/* ---- file fields ------------------------------------------------------ */

static rval *file_field_s(rval *f, const char *name) {
	if (!f || f->k != RV_FILE) return &RV_UNIT;
	const char *p = f->s ? f->s : "";
	const char *slash = strrchr(p, '/');
	const char *base  = slash ? slash + 1 : p;
	const char *dot   = strrchr(base, '.');

	if (!strcmp(name, "name"))
		return rv_str(a_dup(base));
	if (!strcmp(name, "parent"))
		return rv_str(slash ? a_dupn(p, slash - p) : a_dup("."));
	if (!strcmp(name, "stem"))
		return rv_str(dot ? a_dupn(base, dot - base) : a_dup(base));
	if (!strcmp(name, "suffix"))
		return rv_str(a_dup(dot ? dot : ""));

	struct stat st;
	int ok = stat(p, &st) == 0;
	if (!strcmp(name, "mtime")) return rv_int(ok ? (long)st.st_mtime : 0);
	if (!strcmp(name, "size"))  return rv_int(ok ? (long)st.st_size  : 0);
	return &RV_UNIT;
}

static rval *file_field(rval *f, token *name) {
	if (!name) return &RV_UNIT;
	char buf[64];
	size_t k = name->len < sizeof buf - 1 ? name->len : sizeof buf - 1;
	memcpy(buf, name->string, k);
	buf[k] = '\0';
	return file_field_s(f, buf);
}

/* path component helpers (return heap strings) for File<...> construction */
static char *path_dir(const char *p) {
	const char *s = strrchr(p, '/');
	return s ? a_dupn(p, s - p) : a_dup(".");
}
static char *path_base(const char *p) {
	const char *s = strrchr(p, '/');
	return a_dup(s ? s + 1 : p);
}
static char *path_stem(const char *p) {
	char *b = path_base(p);
	char *d = strrchr(b, '.');
	if (d) *d = '\0';
	return b;
}
static char *path_suffix(const char *p) {
	char *b = path_base(p);
	char *d = strrchr(b, '.');
	char *r = a_dup(d ? d : "");
	return r;
}

/* ---- builtins --------------------------------------------------------- */

/* join an output-relative dir with a child name (no leading "./", no "//") */
static void joinp(char *buf, size_t sz, const char *rel, const char *name) {
	if (!rel[0])               snprintf(buf, sz, "%s", name);
	else if (!strcmp(rel, "/")) snprintf(buf, sz, "/%s", name);
	else                       snprintf(buf, sz, "%s/%s", rel, name);
}

/* one directory entry from getdents64 (kernel ABI; no portable header) */
struct glob_dirent64 {
	unsigned long long d_ino;
	long long          d_off;
	unsigned short     d_reclen;
	unsigned char      d_type;
	char               d_name[];
};

static void glob_walk(const char *rel, char *const *comps, size_t ncomp,
                      size_t idx, rval *out);

/* is `child` a directory? trust the getdents d_type, fall back to stat() only
 * when the filesystem reports DT_UNKNOWN. */
static int glob_isdir(const char *child, unsigned char dtype) {
	if (dtype == DT_DIR) return 1;
	if (dtype != DT_UNKNOWN) return 0;
	struct stat st;
	return stat(child, &st) == 0 && S_ISDIR(st.st_mode);
}

/* recursive glob over path components. `rel` is the matched-so-far output path
 * ("" = cwd). "**" matches zero or more directories. Reads directories with
 * getdents64 into a stack buffer (no libc opendir/readdir heap buffer). */
static void glob_walk(const char *rel, char *const *comps, size_t ncomp,
                      size_t idx, rval *out) {
	if (idx >= ncomp) return;
	const char *dir  = rel[0] ? rel : ".";
	const char *comp = comps[idx];
	int last = (idx + 1 == ncomp);
	int star = !strcmp(comp, "**");

	if (star)
		glob_walk(rel, comps, ncomp, idx + 1, out);   /* "**" = zero dirs */

	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) return;

	char buf[8192];
	for (;;) {
		long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
		if (n <= 0) break;
		for (long off = 0; off < n; ) {
			struct glob_dirent64 *e = (struct glob_dirent64 *)(buf + off);
			off += e->d_reclen;
			const char *name = e->d_name;
			if (name[0] == '.') continue;

			char child[4096];
			joinp(child, sizeof child, rel, name);

			if (star) {                            /* descend, keep "**" */
				if (glob_isdir(child, e->d_type))
					glob_walk(child, comps, ncomp, idx, out);
				continue;
			}
			if (fnmatch(comp, name, 0) != 0) continue;
			if (last)
				rv_push(out, rv_file(a_dup(child)));
			else if (glob_isdir(child, e->d_type))
				glob_walk(child, comps, ncomp, idx + 1, out);
		}
	}
	close(fd);
}

/* lists and tuples are both ordered sequences; most builtins accept either. */
static int is_seq(rval *v) { return v && (v->k == RV_LIST || v->k == RV_TUPLE); }

static rval *do_glob(const char *pat) {
	rval *list = rv(RV_LIST);
	char *dup = a_dup(pat);
	char *comps[64];
	size_t nc = 0;
	char *t = strtok(dup, "/");
	for (; t && nc < 64; t = strtok(NULL, "/"))
		comps[nc++] = t;
	if (t) fprintf(stderr, "warning: glob '%s' has >64 path components; truncated\n", pat);
	glob_walk(pat[0] == '/' ? "/" : "", comps, nc, 0, list);
	return list;
}

/* zip(a, b, ...): list of k-tuples, length = shortest input. */
static rval *do_zip(rval **args, size_t k) {
	rval *list = rv(RV_LIST);
	if (k == 0) return list;
	size_t len = SIZE_MAX;
	for (size_t j = 0; j < k; j++) {
		if (!is_seq(args[j])) return list;        /* non-seq -> empty */
		if (args[j]->n < len) len = args[j]->n;
	}
	for (size_t i = 0; i < len; i++) {
		rval *t = rv(RV_TUPLE);
		for (size_t j = 0; j < k; j++)
			rv_push(t, args[j]->items[i]);
		rv_push(list, t);
	}
	return list;
}

/* stale(target, deps...): true if target is out of date vs ANY dep (missing
 * target, or older than some dep). N-ary; first arg is the target. */
static rval *do_stale(rval **args, size_t n) {
	if (n == 0) return rv_bool(0);
	rval *target = args[0];
	if (!target || target->k != RV_FILE) return rv_bool(1);

	size_t ndep = n - 1;
	const char **deps = ndep ? a_alloc(ndep * sizeof *deps) : NULL;
	for (size_t i = 0; i < ndep; i++)
		deps[i] = (args[i + 1] && args[i + 1]->k == RV_FILE) ? args[i + 1]->s : "";

	int r = build_need_rebuild(target->s, deps, ndep);
	return rv_bool(r != 0);
}

/* evaluate a call's arguments (node->kids[1..]) into a flat array, expanding a
 * `...list` splat argument into its elements. the array is arena-allocated. */
static rval **eval_args(ast *call, renv *e, size_t *out_n) {
	rval **arr = NULL;
	size_t n = 0, cap = 0;
	for (size_t i = 1; i < call->nkids; i++) {
		ast *a = call->kids[i];
		rval *single = NULL, *splat = NULL;
		if (a->kind == AST_SPLAT) splat = eval(a->kids[0], e);
		else                      single = eval(a, e);

		int spread = splat && (splat->k == RV_LIST || splat->k == RV_TUPLE);
		size_t add = spread ? splat->n : 1;
		if (n + add > cap) {
			size_t nc = n + add > cap * 2 ? n + add : cap * 2;
			arr = a_grow(arr, n * sizeof *arr, nc * sizeof *arr);
			cap = nc;
		}
		if (spread)
			for (size_t j = 0; j < splat->n; j++) arr[n++] = splat->items[j];
		else
			arr[n++] = splat ? splat : single;
	}
	*out_n = n;
	return arr;
}

/* ---- application ------------------------------------------------------ */

static rval *positional(rval *arg, size_t i) {
	if (arg && (arg->k == RV_TUPLE || arg->k == RV_LIST) && i < arg->n)
		return arg->items[i];
	return &RV_UNIT;
}

static void bind_params(renv *e, ast *params, rval *arg) {
	e->arg = arg;
	size_t P = params->nkids;
	for (size_t i = 0; i < P; i++) {
		ast *p = params->kids[i];
		rval *pt = (P == 1) ? arg : positional(arg, i);
		switch (p->kind) {
		case AST_ID:
		case AST_PROJECTION:
			if (p->tok) env_put(e, p->tok, pt);
			break;
		case AST_CAPTURE:
			/* @id is a rest-capture: it collects the arguments the named
			 * params before it did not take (positions i..end). */
			if (p->nkids) {
				rval *rest;
				if (is_seq(arg)) {
					rest = rv(RV_TUPLE);
					for (size_t j = i; j < arg->n; j++)
						rv_push(rest, arg->items[j]);
				} else {
					rest = (i == 0) ? arg : &RV_UNIT;
				}
				env_put(e, p->kids[0]->tok, rest);
			}
			break;
		default:
			break;
		}
	}
}

static rval *call_builtin1(const char *name, rval *arg);  /* defined below */

/* ---- multiple interpreters (fb_State) --------------------------------------
 * The interpreter keeps its working set in the g_* globals above/below. Each
 * fb_State owns its own arena plus a saved snapshot of those globals; switching
 * the active state (fb_activate) saves the outgoing state's globals into its
 * snapshot and loads the incoming state's, and points the arena at the new one.
 * So several independent interpreters can coexist (they don't run interleaved,
 * matching the single-threaded coroutine model). Declared before apply()
 * because apply dispatches native callables (RV_CFUN) through g_active. */
struct fb_class;
#define FB_MAX_REG 10240   /* core builtins + host/plugin (full raylib ~660) */

typedef struct {
	def    defs[512]; size_t ndefs;
	renv   top; t_env env;
	rval  *args, *cwd;
	int    in_run, bench;
	char **print_q; size_t print_n, print_cap;
	fb_Reg regs[FB_MAX_REG]; int nreg;
	struct fb_class *classes;
	rval  *spinner;
} fb_globals;

struct fb_State {
	jmp_buf   err_jmp;      /* protected-call boundary                     */
	int       err_set;      /* 1 when a boundary is installed              */
	char      err_msg[512];
	int       argc;         /* args handed to the script (fb_set_args)     */
	char    **argv;
	void     *arena;        /* this interpreter's allocation arena         */
	fb_globals snap;        /* globals held while this state is inactive   */
};
static struct fb_State *g_active;   /* the interpreter whose globals are live */
static void fb_activate(struct fb_State *S);   /* make S's globals live */

static rval *apply(rval *fn, rval *arg) {
	if (!fn) return &RV_UNIT;
	if (fn->k == RV_BUILTIN) return call_builtin1(fn->s, arg);
	if (fn->k == RV_LAMBDA) {
		/* arena-allocate the call frame: the body may return a lambda or a
		 * deferred `>>` sequence that captures this env (v->env) and outlives
		 * this stack frame -- a stack renv would dangle (use-after-return). */
		renv *e = a_alloc(sizeof *e);
		e->parent = fn->env;
		bind_params(e, fn->params, arg);
		return eval(fn->body, e);
	}
	if (fn->k == RV_MORPH) {
		if (arg->k != RV_FILE) return &RV_UNIT;
		return rv_file(subst_a(fn->from, fn->to, arg->s));
	}
	if (fn->k == RV_CFUN) {                 /* native method/callable */
		fb_CFunction f = (fb_CFunction)fn->nat;
		if (arg->k == RV_TUPLE)
			return (rval *)f(g_active, (fb_Value *)arg->items, (int)arg->n);
		rval *one[1] = { arg };
		return (rval *)f(g_active, (fb_Value *)one, 1);
	}
	return &RV_UNIT;
}

/* ============================================================================
 * Extension ABI (fbuild.h) — single implicit state (S0).
 * A registry of host/plugin builtins, thin value constructors/accessors over
 * rval + the arena, and an error boundary. Core builtins still live in the
 * eval() if-chains; the registry is consulted for anything not matched there,
 * so registered names extend the language without touching core.
 * ========================================================================== */

static fb_Reg g_regs[FB_MAX_REG];   /* live registry (part of the active state) */
static int    g_nreg;

void fb_register(fb_State *S, const fb_Reg *reg) {
	fb_activate(S);
	if (!reg || !reg->name || !reg->fn) return;
	for (int i = 0; i < g_nreg; i++)          /* last registration wins */
		if (!strcmp(g_regs[i].name, reg->name)) { g_regs[i] = *reg; return; }
	if (g_nreg < FB_MAX_REG) g_regs[g_nreg++] = *reg;
	else fprintf(stderr, "fbuild: builtin registry full (%d), dropped '%s'\n",
	             FB_MAX_REG, reg->name);
}
const fb_Reg *fb_reg_findn(const char *name, size_t len) {
	for (int i = 0; i < g_nreg; i++)
		if (!strncmp(g_regs[i].name, name, len) && g_regs[i].name[len] == '\0')
			return &g_regs[i];
	return NULL;
}
const fb_Reg *fb_reg_find(const char *name) {
	return fb_reg_findn(name, strlen(name));
}
static const fb_Reg *fb_reg_find_tok(token *t) {
	return t ? fb_reg_findn(t->string, t->len) : NULL;
}

/* value constructors */
fb_Value fb_nil (fb_State *S)              { (void)S; return (fb_Value)&RV_UNIT; }
fb_Value fb_int (fb_State *S, long v)      { (void)S; return (fb_Value)rv_int(v); }
fb_Value fb_bool(fb_State *S, int v)       { (void)S; return (fb_Value)rv_bool(v); }
fb_Value fb_str (fb_State *S, const char *s){ (void)S; return (fb_Value)rv_str(a_dup(s ? s : "")); }
fb_Value fb_file(fb_State *S, const char *p){ (void)S; return (fb_Value)rv_file(a_dup(p ? p : "")); }
fb_Value fb_list(fb_State *S)              { (void)S; return (fb_Value)rv(RV_LIST); }
fb_Value fb_map (fb_State *S)              { (void)S; return (fb_Value)rv(RV_MAP); }
fb_Value fb_cmd (fb_State *S)              { (void)S; return (fb_Value)rv(RV_CMD); }
void     fb_push(fb_State *S, fb_Value l, fb_Value e) { (void)S; rv_push((rval*)l, (rval*)e); }
void     fb_cmd_push(fb_State *S, fb_Value c, fb_Value w) { (void)S; rv_push((rval*)c, (rval*)w); }
void     fb_set (fb_State *S, fb_Value m, const char *k, fb_Value v) {
	(void)S; map_set((rval*)m, a_dup(k ? k : ""), (rval*)v);
}

/* value inspection */
fb_Kind fb_kind(fb_Value v) {
	rval *r = (rval*)v;
	if (!r) return FB_NIL;
	switch (r->k) {
	case RV_NONE:   return FB_NIL;
	case RV_INT:    return FB_INT;
	case RV_STR:    return FB_STR;
	case RV_BOOL:   return FB_BOOL;
	case RV_FILE:   return FB_FILE;
	case RV_LIST:   return FB_LIST;
	case RV_TUPLE:  return FB_TUPLE;
	case RV_CMD:    return FB_CMD;
	case RV_MAP:    return FB_MAP;
	case RV_LAMBDA: return FB_LAMBDA;
	case RV_OBJECT: return FB_OBJECT;
	default:        return FB_OTHER;
	}
}
long        fb_toint(fb_Value v)             { rval *r=(rval*)v; return r ? r->i : 0; }
const char *fb_tostr(fb_State *S, fb_Value v){ (void)S; return as_cstr((rval*)v); }
int         fb_len  (fb_Value v)             { rval *r=(rval*)v; return r ? (int)r->n : 0; }
fb_Value    fb_at   (fb_Value v, int i) {
	rval *r=(rval*)v;
	if (!r || i < 0 || (size_t)i >= r->n) return (fb_Value)&RV_UNIT;
	return (fb_Value)r->items[i];
}
fb_Value    fb_field(fb_Value v, const char *k) { return (fb_Value)map_get((rval*)v, k ? k : ""); }
const char *fb_key(fb_Value v, int i) {
	rval *r = (rval*)v;
	if (!r || r->k != RV_MAP || i < 0 || (size_t)i >= r->n) return NULL;
	return r->keys[i];
}

fb_Value fb_force(fb_State *S, fb_Value thunk) {
	(void)S;
	rval *r = (rval*)thunk;
	if (r && r->k == RV_THUNK) return (fb_Value)eval(r->body, r->env);
	return thunk;
}

fb_Value fb_apply(fb_State *S, fb_Value fn, fb_Value *args, int nargs) {
	(void)S;
	rval *arg;
	if (nargs == 1) {
		arg = (rval*)args[0];
	} else {
		arg = rv(RV_TUPLE);
		for (int i = 0; i < nargs; i++) rv_push(arg, (rval*)args[i]);
	}
	return (fb_Value)apply((rval*)fn, arg);
}

fb_Value fb_error(fb_State *S, const char *fmt, ...) {
	if (!S) S = g_active;
	va_list ap; va_start(ap, fmt);
	vsnprintf(S->err_msg, sizeof S->err_msg, fmt, ap);
	va_end(ap);
	if (S->err_set) longjmp(S->err_jmp, 1);   /* embedding: unwind to boundary */
	fprintf(stderr, "error: %s\n", S->err_msg);
	exit(1);                                   /* CLI: no boundary installed  */
}

/* Dispatch a registered builtin: enforce arity, then call. Args are already
 * evaluated (splats flattened). */
static rval *fb_dispatch(const fb_Reg *rg, rval **as, size_t k) {
	if ((int)k < rg->min || (rg->max >= 0 && (int)k > rg->max))
		fb_error(g_active, "%s: expects %d..%d args, got %zu",
		         rg->name, rg->min, rg->max, k);
	return (rval *)rg->fn(g_active, (fb_Value *)as, (int)k);
}

/* ---- custom objects: classes + methods -------------------------------- */
/* A class is a name plus a method table (each method is an rval callable: a
 * language RV_LAMBDA or a native RV_CFUN). An object (RV_OBJECT) carries its
 * class in ->cls, its fields in the map slots (keys/items), and optional native
 * data in ->nat. Method call `obj.m(args)` applies the method to (self, args).
 * Both the language (`type`/`method`) and the ABI (fb_class/fb_method) build on
 * this. */
typedef struct fb_class {
	char *name;
	struct { char *name; rval *val; } *methods;
	size_t nmethods, cap;
	struct fb_class *next;
} fb_class_t;

static fb_class_t *g_classes;

static fb_class_t *class_find(const char *name) {
	for (fb_class_t *c = g_classes; c; c = c->next)
		if (!strcmp(c->name, name)) return c;
	return NULL;
}
static fb_class_t *class_ensure(const char *name) {
	fb_class_t *c = class_find(name);
	if (c) return c;
	c = a_alloc(sizeof *c);
	c->name = a_dup(name);
	c->methods = NULL; c->nmethods = c->cap = 0;
	c->next = g_classes; g_classes = c;
	return c;
}
static void class_add_method(fb_class_t *c, const char *name, rval *val) {
	for (size_t i = 0; i < c->nmethods; i++)
		if (!strcmp(c->methods[i].name, name)) { c->methods[i].val = val; return; }
	if (c->nmethods == c->cap) {
		size_t nc = c->cap ? c->cap * 2 : 4;
		c->methods = a_grow(c->methods, c->cap * sizeof *c->methods, nc * sizeof *c->methods);
		c->cap = nc;
	}
	c->methods[c->nmethods].name = a_dup(name);
	c->methods[c->nmethods].val  = val;
	c->nmethods++;
}
static rval *class_method(fb_class_t *c, const char *name) {
	if (!c) return NULL;
	for (size_t i = 0; i < c->nmethods; i++)
		if (!strcmp(c->methods[i].name, name)) return c->methods[i].val;
	return NULL;
}
/* build an object of class c: share `fields`' map slots (if any) + native data */
static rval *make_object(fb_class_t *c, rval *fields, void *nat) {
	rval *o = rv(RV_OBJECT);
	o->cls = c; o->nat = nat;
	if (fields && (fields->k == RV_MAP || fields->k == RV_OBJECT)) {
		o->keys = fields->keys; o->items = fields->items;
		o->n = fields->n; o->cap = fields->cap;
	}
	return o;
}
static rval *make_cfun(fb_CFunction fn) {
	rval *v = rv(RV_CFUN); v->nat = (void *)fn; return v;
}

/* ABI: define classes, attach native methods, instantiate native objects. */
fb_Class fb_class(fb_State *S, const char *name) {
	fb_activate(S); return (fb_Class)class_ensure(name);
}
void fb_method(fb_State *S, fb_Class c, const char *name, fb_CFunction fn) {
	fb_activate(S); class_add_method((fb_class_t *)c, name, make_cfun(fn));
}
fb_Value fb_object(fb_State *S, fb_Class c, void *data) {
	fb_activate(S); return (fb_Value)make_object((fb_class_t *)c, NULL, data);
}
void *fb_object_data(fb_Value v) {
	rval *r = (rval *)v;
	return (r && r->k == RV_OBJECT) ? r->nat : NULL;
}

/* ---- more builtins ---------------------------------------------------- */

static int truthy(rval *v) {
	if (!v || v->k == RV_NONE) return 0;
	if (v->k == RV_BOOL || v->k == RV_INT) return v->i != 0;
	return 1;
}

/* path of a file/string argument */
static const char *pstr(rval *v) {
	return (v && (v->k == RV_FILE || v->k == RV_STR) && v->s) ? v->s : "";
}

/* config / process ----------------------------------------------------- */

/* monotonic clock in nanoseconds; subtract two reads to time a section. */
static rval *do_clock(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return rv_int((long)ts.tv_sec * 1000000000L + ts.tv_nsec);
}
/* wall-clock unix epoch, in seconds. */
static rval *do_time(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return rv_int((long)ts.tv_sec);
}
/* sleep for the given number of milliseconds; returns (). */
static rval *do_sleep(rval *ms) {
	long m = (ms && ms->k == RV_INT) ? ms->i : 0;
	if (m > 0) {
		struct timespec ts;
		ts.tv_sec  = m / 1000;
		ts.tv_nsec = (m % 1000) * 1000000L;
		nanosleep(&ts, NULL);
	}
	return &RV_UNIT;
}

/* config spinner: a language lambda (Int -> String). The build runtime calls
 * spin_trampoline() each refresh with this command's frame counter; we apply the
 * lambda and hand back its string (arena-owned, valid until the next call). */
static rval *g_spinner;
static const char *spin_trampoline(int n) {
	if (!g_spinner) return "";
	const char *s = as_cstr(apply(g_spinner, rv_int(n)));
	return s ? s : "";
}

/* config(map): apply build-script settings. Recognized keys:
 *   jobs       : Int           cap concurrent child procs in PARALLEL groups
 *   verbose    : Bool          show every command's output (no collapse)
 *   keep_going : Bool          run all tasks, report failures at the end
 *   silent     : Bool          suppress the live UI
 *   color      : Bool          force the live UI on (true) / off (false)
 *   spinner_ms : Int           display refresh period in ms
 *   tail_lines : Int           live-output lines under a running command
 *   spinner    : (Int->String) custom spinner glyph per frame
 * Unknown keys are ignored so scripts stay forward-compatible. */
static rval *do_config(rval *m) {
	if (!m || m->k != RV_MAP) return &RV_UNIT;
	rval *v;
	if ((v = map_get(m, "jobs"))       && v->k == RV_INT)    build_set_jobs((int)v->i);
	if ((v = map_get(m, "verbose"))    && v->k != RV_NONE)   build_set_verbose(truthy(v));
	if ((v = map_get(m, "keep_going")) && v->k != RV_NONE)   build_set_keep_going(truthy(v));
	if ((v = map_get(m, "silent"))     && v->k != RV_NONE)   build_set_silent(truthy(v));
	if ((v = map_get(m, "color"))      && v->k != RV_NONE)   build_set_color(truthy(v) ? 1 : 2);
	if ((v = map_get(m, "spinner_ms")) && v->k == RV_INT)    build_set_spinner_ms(v->i);
	if ((v = map_get(m, "tail_lines")) && v->k == RV_INT)    build_set_tail_lines((int)v->i);
	if ((v = map_get(m, "spinner"))    && v->k == RV_LAMBDA) {
		g_spinner = v;
		build_set_spinner_fn(spin_trampoline);
	}
	return &RV_UNIT;
}

static rval *do_env(const char *name) {
	char *v = getenv(name);
	if (!v) return &RV_UNIT;            /* unset -> () (falsy), so `env(x) || d` works */
	return rv_str(a_dup(v));
}

/* ---- lightweight CLI arg helpers (over the `args` list) --------------- */

/* flag(name...) -> Bool: true if any of the given flag names appears in args. */
static rval *do_flag(rval **a, size_t n) {
	for (size_t k = 0; k < n; k++) {
		const char *name = as_cstr(a[k]);
		for (size_t i = 0; i < g_args->n; i++)
			if (!strcmp(as_cstr(g_args->items[i]), name)) return rv_bool(1);
	}
	return rv_bool(0);
}

/* opt(name, default) -> String: value of `name v` or `name=v` in args, else
 * default. A function default is invoked lazily -- only when the option is
 * absent -- so `opt('-x', _ -> expensive())` never runs expensive() when -x is
 * supplied. */
static rval *do_opt(rval *name, rval *deflt) {
	const char *nm = as_cstr(name);
	size_t nl = strlen(nm);
	for (size_t i = 0; i < g_args->n; i++) {
		const char *cur = as_cstr(g_args->items[i]);
		if (!strcmp(cur, nm)) {                          /* `name value` */
			if (i + 1 < g_args->n) return rv_str(a_dup(as_cstr(g_args->items[i + 1])));
			break;
		}
		if (!strncmp(cur, nm, nl) && cur[nl] == '=')     /* `name=value` */
			return rv_str(a_dup(cur + nl + 1));
	}
	return deflt->k == RV_LAMBDA ? apply(deflt, &RV_UNIT) : deflt;
}

/* positionals() -> List<String>: args that aren't flags (don't start with '-').
 * Lightweight: does not skip values consumed by opt(); use Parser for that. */
static rval *do_positionals(void) {
	rval *l = rv(RV_LIST);
	for (size_t i = 0; i < g_args->n; i++) {
		const char *cur = as_cstr(g_args->items[i]);
		if (cur[0] != '-') rv_push(l, rv_str(a_dup(cur)));
	}
	return l;
}

/* Parser<schema>(argv): drive build.h's args.h with a schema map. Each schema
 * entry is name -> (type string | spec map). Returns a map of name -> typed
 * value (with schema/spec defaults applied), plus `rest` = leftover positionals.
 * `-h`/`--help` is handled by args.h (prints help from the `help` fields). */
static rval *do_parse(rval *schema, rval *argv_list) {
	if (!schema || schema->k != RV_MAP) return rv(RV_MAP);
	size_t ne = schema->n;

	t_arg_rule *rules = a_alloc((ne + 2) * sizeof *rules);
	memset(rules, 0, (ne + 2) * sizeof *rules);

	for (size_t i = 0; i < ne; i++) {
		const char *name = schema->keys[i];
		rval *spec = schema->items[i];
		rval *sm = (spec && spec->k == RV_MAP) ? spec : NULL;
		const char *tstr = sm ? "str" : as_cstr(spec);
		const char *longh = NULL, *shorth = NULL, *help = NULL;
		if (sm) {
			rval *v;
			if ((v = map_get(sm, "type"))->k  != RV_NONE) tstr   = as_cstr(v);
			if ((v = map_get(sm, "long"))->k  != RV_NONE) longh  = as_cstr(v);
			if ((v = map_get(sm, "short"))->k != RV_NONE) shorth = as_cstr(v);
			if ((v = map_get(sm, "help"))->k  != RV_NONE) help   = as_cstr(v);
		}
		if (!longh) { char *l = a_alloc(strlen(name) + 3); sprintf(l, "--%s", name); longh = l; }
		if (!shorth) {
			/* auto short = -<first letter>, only if unique among names and not -h */
			int dup = (name[0] == 'h');
			for (size_t j = 0; j < ne && !dup; j++)
				if (j != i && schema->keys[j][0] == name[0]) dup = 1;
			if (!dup) { char *s = a_alloc(3); s[0] = '-'; s[1] = name[0]; s[2] = 0; shorth = s; }
		}
		t_arg_type ty = ARG_STRING;
		if (!strcmp(tstr, "bool")) ty = ARG_BOOL;
		else if (!strcmp(tstr, "int") || !strcmp(tstr, "integer")) ty = ARG_INTEGER;
		rules[i].shorthand = shorth;
		rules[i].longhand  = longh;
		rules[i].flags     = 0;
		rules[i].type      = ty;
		rules[i].help      = help;
	}
	rules[ne].longhand = "rest";              /* capture leftover positionals */
	rules[ne].flags    = ARG_POSITIONAL | ARG_ARRAY;
	rules[ne].type     = ARG_STR_ARRAY;

	size_t na = (argv_list && is_seq(argv_list)) ? argv_list->n : 0;
	char **av = a_alloc((na + 1) * sizeof *av);
	av[0] = "fbuild";
	for (size_t i = 0; i < na; i++) av[i + 1] = (char *)as_cstr(argv_list->items[i]);

	t_argparser parser = { .rules = rules };
	if (args_parse(&parser, (int)(na + 1), av) == -2) {  /* -h/--help shown */
		args_free(&parser);
		exit(0);
	}

	rval *res = rv(RV_MAP);
	for (size_t i = 0; i < ne; i++) {
		const char *q = rules[i].longhand;
		rval *val;
		switch (rules[i].type) {
		case ARG_BOOL:    val = rv_bool(args_get_bool(&parser, q)); break;
		case ARG_INTEGER: val = rv_int(args_get_int(&parser, q));   break;
		default: { const char *s = args_get_str(&parser, q); val = rv_str(a_dup(s ? s : "")); }
		}
		if (!args_present(&parser, q) && schema->items[i]->k == RV_MAP) {
			rval *d = map_get(schema->items[i], "default");
			/* A function default is invoked lazily -- only here, when the arg
			 * was not supplied -- so an expensive default (e.g. shelling out)
			 * never runs when a value is given on the command line. */
			if (d->k == RV_LAMBDA) d = apply(d, &RV_UNIT);
			if (d->k != RV_NONE)   val = d;      /* spec default overrides type zero */
		}
		map_set(res, a_dup(schema->keys[i]), val);
	}
	rval *rest = rv(RV_LIST);
	t_arg_str_array *arr = args_get_array(&parser, "rest");
	if (arr) for (size_t i = 0; i < arr->count; i++) rv_push(rest, rv_str(a_dup(arr->items[i])));
	map_set(res, a_dup("rest"), rest);

	args_free(&parser);
	return res;
}

/* run a command via the shell, capture stdout, trim trailing newlines */
static rval *do_which(const char *name) {
	if (strchr(name, '/'))
		return access(name, X_OK) == 0 ? rv_file(a_dup(name)) : rv_str(a_dup(""));
	char *path = getenv("PATH");
	if (!path) return rv_str(a_dup(""));
	char *p = a_dup(path), *save = NULL;
	for (char *t = strtok_r(p, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
		char full[4096];
		snprintf(full, sizeof full, "%s/%s", t, name);
		if (access(full, X_OK) == 0) return rv_file(a_dup(full));
	}
	return rv_str(a_dup(""));
}

/* filesystem ----------------------------------------------------------- */

static rval *do_exists(rval *f) { return rv_bool(access(pstr(f), F_OK) == 0); }

static rval *do_mkdir(rval *d) {               /* mkdir -p */
	const char *p = pstr(d);
	char tmp[4096];
	snprintf(tmp, sizeof tmp, "%s", p);
	for (char *s = tmp + 1; *s; s++)
		if (*s == '/') { *s = '\0'; mkdir(tmp, 0755); *s = '/'; }
	mkdir(tmp, 0755);
	return rv_file(a_dup(p));
}

static rval *do_read(rval *f) {
	FILE *fp = fopen(pstr(f), "rb");
	if (!fp) return rv_str(a_dup(""));
	size_t cap = 256, n = 0;
	char *b = a_alloc(cap);
	int c;
	while ((c = fgetc(fp)) != EOF) {
		if (n + 1 >= cap) { b = a_grow(b, n, cap * 2); cap *= 2; }
		b[n++] = (char)c;
	}
	fclose(fp);
	b[n] = '\0';
	return rv_str(b);
}

static rval *do_write(rval *f, rval *content) {
	FILE *fp = fopen(pstr(f), "wb");
	if (fp) { fputs(as_cstr(content), fp); fclose(fp); }
	return (f && f->k == RV_FILE) ? f : rv_file(a_dup(pstr(f)));
}

/* raw byte copy of src -> dst, preserving src's mode. */
static int file_copy(const char *src, const char *dst) {
	int in = open(src, O_RDONLY);
	if (in < 0) return 0;
	struct stat st;
	mode_t mode = (fstat(in, &st) == 0) ? st.st_mode & 0777 : 0644;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) { close(in); return 0; }
	char buf[1 << 16];
	ssize_t r;
	int ok = 1;
	while ((r = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, (size_t)r) != r) { ok = 0; break; }
	if (r < 0) ok = 0;
	close(in);
	close(out);
	return ok;
}

static rval *do_move(rval *a, rval *b) {       /* rename, fallback copy+unlink */
	const char *s = pstr(a), *d = pstr(b);
	if (rename(s, d) != 0 && file_copy(s, d)) unlink(s);
	return rv_file(a_dup(d));
}
static rval *do_copy(rval *a, rval *b) {
	const char *s = pstr(a), *d = pstr(b);
	file_copy(s, d);
	return rv_file(a_dup(d));
}
static rval *do_symlink(rval *target, rval *link) {
	const char *t = pstr(target), *l = pstr(link);
	unlink(l);                                 /* replace an existing link */
	symlink(t, l);
	return rv_file(a_dup(l));
}
static rval *do_remove(rval *f) {              /* unlink file, rmdir directory */
	const char *p = pstr(f);
	if (unlink(p) == 0) return rv_bool(1);
	return rv_bool(rmdir(p) == 0);
}
static rval *do_touch(rval *f) {
	const char *p = pstr(f);
	int fd = open(p, O_WRONLY | O_CREAT, 0644);
	if (fd >= 0) close(fd);
	utimensat(AT_FDCWD, p, NULL, 0);           /* bump mtime to now */
	return rv_file(a_dup(p));
}
static rval *do_is_dir(rval *f) {
	struct stat st;
	return rv_bool(stat(pstr(f), &st) == 0 && S_ISDIR(st.st_mode));
}
static rval *do_is_file(rval *f) {
	struct stat st;
	return rv_bool(stat(pstr(f), &st) == 0 && S_ISREG(st.st_mode));
}

/* lists ---------------------------------------------------------------- */

static rval *do_filter(rval *list, rval *fn) {
	rval *out = rv(RV_LIST);
	if (!is_seq(list)) return out;
	for (size_t i = 0; i < list->n; i++)
		if (truthy(apply(fn, list->items[i])))
			rv_push(out, list->items[i]);
	return out;
}

static rval *do_flatten(rval *list) {
	rval *out = rv(RV_LIST);
	if (!is_seq(list)) return out;
	for (size_t i = 0; i < list->n; i++) {
		rval *e = list->items[i];
		if (is_seq(e))
			for (size_t j = 0; j < e->n; j++) rv_push(out, e->items[j]);
		else
			rv_push(out, e);
	}
	return out;
}

static rval *do_concat(rval **args, size_t n) {
	rval *out = rv(RV_LIST);
	for (size_t i = 0; i < n; i++) {
		rval *a = args[i];
		if (is_seq(a))
			for (size_t j = 0; j < a->n; j++) rv_push(out, a->items[j]);
		else if (a->k != RV_NONE)             /* skip () (e.g. a gated-off def) */
			rv_push(out, a);
	}
	return out;
}

static rval *do_fold(rval *list, rval *init, rval *fn) {
	rval *acc = init;
	if (is_seq(list))
		for (size_t i = 0; i < list->n; i++) {
			rval *t = rv(RV_TUPLE);
			rv_push(t, acc);
			rv_push(t, list->items[i]);
			acc = apply(fn, t);
		}
	return acc;
}

static rval *do_contains(rval *list, rval *x) {
	if (!is_seq(list)) return rv_bool(0);
	const char *s = as_cstr(x);
	for (size_t i = 0; i < list->n; i++)
		if (!strcmp(as_cstr(list->items[i]), s)) return rv_bool(1);
	return rv_bool(0);
}

static rval *do_length(rval *list) {
	return rv_int(is_seq(list) ? (long)list->n : 0);
}
static rval *do_first(rval *list) {
	return (is_seq(list) && list->n) ? list->items[0] : &RV_UNIT;
}
static rval *do_last(rval *list) {
	return (is_seq(list) && list->n) ? list->items[list->n - 1] : &RV_UNIT;
}

/* take(list, n): first n elements. drop(list, n): all but the first n.
 * n is clamped to [0, len]; negative n counts from the end. */
static rval *do_take(rval *list, rval *count) {
	rval *out = rv(RV_LIST);
	if (!is_seq(list)) return out;
	long c = count->k == RV_INT ? count->i : 0;
	if (c < 0) c += (long)list->n;
	if (c < 0) c = 0;
	if (c > (long)list->n) c = (long)list->n;
	for (long i = 0; i < c; i++) rv_push(out, list->items[i]);
	return out;
}
static rval *do_drop(rval *list, rval *count) {
	rval *out = rv(RV_LIST);
	if (!is_seq(list)) return out;
	long c = count->k == RV_INT ? count->i : 0;
	if (c < 0) c += (long)list->n;
	if (c < 0) c = 0;
	if (c > (long)list->n) c = (long)list->n;
	for (long i = c; i < (long)list->n; i++) rv_push(out, list->items[i]);
	return out;
}

static int cmp_rv(const void *a, const void *b) {
	return strcmp(as_cstr(*(rval *const *)a), as_cstr(*(rval *const *)b));
}
static rval *do_sort(rval *list) {
	rval *out = rv(RV_LIST);
	if (!is_seq(list)) return out;
	for (size_t i = 0; i < list->n; i++) rv_push(out, list->items[i]);
	if (out->n) qsort(out->items, out->n, sizeof *out->items, cmp_rv);
	return out;
}
static rval *do_unique(rval *list) {
	rval *out = rv(RV_LIST);
	if (!is_seq(list)) return out;
	for (size_t i = 0; i < list->n; i++) {
		const char *s = as_cstr(list->items[i]);
		int seen = 0;
		for (size_t j = 0; j < out->n; j++)
			if (strcmp(as_cstr(out->items[j]), s) == 0) { seen = 1; break; }
		if (!seen) rv_push(out, list->items[i]);
	}
	return out;
}

/* python-style list ops -------------------------------------------------- */

static rval *do_range(long start, long stop) {       /* [start, stop) */
	rval *l = rv(RV_LIST);
	for (long i = start; i < stop; i++) rv_push(l, rv_int(i));
	return l;
}
static rval *do_enumerate(rval *list) {              /* [(0,x0), (1,x1), ...] */
	rval *l = rv(RV_LIST);
	if (!is_seq(list)) return l;
	for (size_t i = 0; i < list->n; i++) {
		rval *t = rv(RV_TUPLE);
		rv_push(t, rv_int((long)i));
		rv_push(t, list->items[i]);
		rv_push(l, t);
	}
	return l;
}
static rval *do_slice(rval *list, long s, long e) {  /* list[s:e], neg from end */
	rval *l = rv(RV_LIST);
	if (!is_seq(list)) return l;
	long n = (long)list->n;
	if (s < 0) s += n;
	if (e < 0) e += n;
	if (s < 0) s = 0;
	if (e > n) e = n;
	for (long i = s; i < e; i++) rv_push(l, list->items[i]);
	return l;
}
/* first index where (key ? key(elem) : elem) equals val, or -1. */
static rval *do_index(rval *list, rval *val, rval *key) {
	if (!is_seq(list)) return rv_int(-1);
	const char *s = as_cstr(val);
	for (size_t i = 0; i < list->n; i++) {
		rval *e = key ? apply(key, list->items[i]) : list->items[i];
		if (!strcmp(as_cstr(e), s)) return rv_int((long)i);
	}
	return rv_int(-1);
}
static rval *do_reverse(rval *list) {
	rval *l = rv(RV_LIST);
	if (!is_seq(list)) return l;
	for (size_t i = list->n; i > 0; i--) rv_push(l, list->items[i - 1]);
	return l;
}
/* sum/min/max accept either a single sequence or varargs: sum(L) == sum(1,2,3). */
static void agg_items(rval **args, size_t n, rval ***it, size_t *cnt) {
	if (n == 1 && is_seq(args[0])) { *it = args[0]->items; *cnt = args[0]->n; }
	else                           { *it = args;           *cnt = n; }
}
static rval *do_sum(rval **args, size_t n) {
	rval **it; size_t c; agg_items(args, n, &it, &c);
	long t = 0;
	for (size_t i = 0; i < c; i++) t += it[i]->i;
	return rv_int(t);
}
static rval *do_min(rval **args, size_t n) {
	rval **it; size_t c; agg_items(args, n, &it, &c);
	if (!c) return rv_int(0);
	long m = it[0]->i;
	for (size_t i = 1; i < c; i++) if (it[i]->i < m) m = it[i]->i;
	return rv_int(m);
}
static rval *do_max(rval **args, size_t n) {
	rval **it; size_t c; agg_items(args, n, &it, &c);
	if (!c) return rv_int(0);
	long m = it[0]->i;
	for (size_t i = 1; i < c; i++) if (it[i]->i > m) m = it[i]->i;
	return rv_int(m);
}
static rval *do_count(rval *list, rval *val) {       /* occurrences of val */
	long c = 0;
	if (is_seq(list)) {
		const char *s = as_cstr(val);
		for (size_t i = 0; i < list->n; i++)
			if (!strcmp(as_cstr(list->items[i]), s)) c++;
	}
	return rv_int(c);
}

/* strings -------------------------------------------------------------- */

static rval *do_join(rval *list, const char *sep) {
	if (!is_seq(list)) return rv_str(a_dup(""));
	size_t tot = 1;
	for (size_t i = 0; i < list->n; i++)
		tot += strlen(as_cstr(list->items[i])) + strlen(sep);
	char *b = a_alloc(tot);
	b[0] = '\0';
	for (size_t i = 0; i < list->n; i++) {
		if (i) strcat(b, sep);
		strcat(b, as_cstr(list->items[i]));
	}
	return rv_str(b);
}

static rval *do_split(const char *s, const char *sep) {
	rval *out = rv(RV_LIST);
	if (!*sep) { rv_push(out, rv_str(a_dup(s))); return out; }
	size_t sl = strlen(sep);
	const char *p = s, *q;
	while ((q = strstr(p, sep))) { rv_push(out, rv_str(a_dupn(p, q - p))); p = q + sl; }
	rv_push(out, rv_str(a_dup(p)));
	return out;
}

static rval *do_replace(const char *s, const char *a, const char *b) {
	if (!*a) return rv_str(a_dup(s));
	size_t la = strlen(a), lb = strlen(b), cnt = 0;
	for (const char *p = s; (p = strstr(p, a)); p += la) cnt++;
	char *out = a_alloc(strlen(s) + (lb > la ? cnt * (lb - la) : 0) + 1), *w = out;
	const char *p = s, *q;
	while ((q = strstr(p, a))) {
		memcpy(w, p, q - p); w += q - p;
		memcpy(w, b, lb);    w += lb;
		p = q + la;
	}
	strcpy(w, p);
	return rv_str(out);
}

/* paths ---------------------------------------------------------------- */

static rval *do_with_suffix(rval *f, const char *ext) {
	const char *p = pstr(f);
	char *dir = path_dir(p), *st = path_stem(p);
	const char *dot = (ext[0] == '.') ? "" : ".";
	char *name = a_alloc(strlen(st) + strlen(dot) + strlen(ext) + 1);
	sprintf(name, "%s%s%s", st, dot, ext);
	char *out;
	if (dir[0] && strcmp(dir, ".") != 0) {
		out = a_alloc(strlen(dir) + 1 + strlen(name) + 1);
		sprintf(out, "%s/%s", dir, name);
	} else out = a_dup(name);
	return rv_file(out);
}

static rval *do_with_name(rval *f, const char *nm) {
	char *dir = path_dir(pstr(f));
	char *out;
	if (dir[0] && strcmp(dir, ".") != 0) {
		out = a_alloc(strlen(dir) + 1 + strlen(nm) + 1);
		sprintf(out, "%s/%s", dir, nm);
	} else out = a_dup(nm);
	return rv_file(out);
}

/* more string ops --------------------------------------------------------- */

static rval *do_find(const char *s, const char *sub) {   /* byte index or -1 */
	const char *p = strstr(s, sub);
	return rv_int(p ? (long)(p - s) : -1);
}
static rval *do_to_int(rval *v) { return rv_int(strtol(as_cstr(v), NULL, 10)); }
static rval *do_to_str(rval *v) { return rv_str(a_dup(as_cstr(v))); }
static rval *do_repeat(const char *s, long n) {
	if (n < 0) n = 0;
	size_t l = strlen(s);
	char *b = a_alloc(l * (size_t)n + 1);
	for (long i = 0; i < n; i++) memcpy(b + (size_t)i * l, s, l);
	b[l * (size_t)n] = '\0';
	return rv_str(b);
}
static rval *do_chars(const char *s) {          /* string -> list of 1-char strings */
	rval *l = rv(RV_LIST);
	for (; *s; s++) { char *c = a_alloc(2); c[0] = *s; c[1] = '\0'; rv_push(l, rv_str(c)); }
	return l;
}

static rval *do_cat(rval **a, size_t n) {       /* concat strings */
	size_t tot = 1;
	for (size_t i = 0; i < n; i++) tot += strlen(as_cstr(a[i]));
	char *b = a_alloc(tot);
	b[0] = '\0';
	for (size_t i = 0; i < n; i++) strcat(b, as_cstr(a[i]));
	return rv_str(b);
}
static rval *do_upper(rval *v) {
	char *s = a_dup(as_cstr(v));
	for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
	return rv_str(s);
}
static rval *do_lower(rval *v) {
	char *s = a_dup(as_cstr(v));
	for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
	return rv_str(s);
}
static rval *do_trim(rval *v) {
	const char *s = as_cstr(v);
	while (isspace((unsigned char)*s)) s++;
	size_t l = strlen(s);
	while (l && isspace((unsigned char)s[l - 1])) l--;
	return rv_str(a_dupn(s, l));
}
static rval *do_starts_with(rval *v, rval *p) {
	const char *s = as_cstr(v), *pre = as_cstr(p);
	return rv_bool(strncmp(s, pre, strlen(pre)) == 0);
}
static rval *do_ends_with(rval *v, rval *p) {
	const char *s = as_cstr(v), *suf = as_cstr(p);
	size_t ls = strlen(s), lf = strlen(suf);
	return rv_bool(lf <= ls && strcmp(s + ls - lf, suf) == 0);
}
static rval *do_capitalize(rval *v) {           /* first upper, rest lower */
	char *s = a_dup(as_cstr(v));
	if (s[0]) {
		s[0] = (char)toupper((unsigned char)s[0]);
		for (char *p = s + 1; *p; p++) *p = (char)tolower((unsigned char)*p);
	}
	return rv_str(s);
}
static rval *do_title(rval *v) {                 /* capitalize each word */
	char *s = a_dup(as_cstr(v));
	int start = 1;
	for (char *p = s; *p; p++) {
		if (isspace((unsigned char)*p)) { start = 1; continue; }
		*p = start ? (char)toupper((unsigned char)*p) : (char)tolower((unsigned char)*p);
		start = 0;
	}
	return rv_str(s);
}
static rval *do_lstrip(rval *v) {
	const char *s = as_cstr(v);
	while (isspace((unsigned char)*s)) s++;
	return rv_str(a_dup(s));
}
static rval *do_rstrip(rval *v) {
	const char *s = as_cstr(v);
	size_t l = strlen(s);
	while (l && isspace((unsigned char)s[l - 1])) l--;
	return rv_str(a_dupn(s, l));
}
static rval *do_strip_prefix(rval *v, rval *p) {
	const char *s = as_cstr(v), *pre = as_cstr(p);
	size_t lp = strlen(pre);
	return rv_str(a_dup(strncmp(s, pre, lp) == 0 ? s + lp : s));
}
static rval *do_strip_suffix(rval *v, rval *p) {
	const char *s = as_cstr(v), *suf = as_cstr(p);
	size_t ls = strlen(s), lf = strlen(suf);
	if (lf <= ls && strcmp(s + ls - lf, suf) == 0) return rv_str(a_dupn(s, ls - lf));
	return rv_str(a_dup(s));
}
/* pad s to width with fill; side: -1 pad left, +1 pad right, 0 center */
static rval *pad_to(const char *s, long width, char fill, int side) {
	size_t l = strlen(s);
	if (width < 0 || (size_t)width <= l) return rv_str(a_dupn(s, l));
	size_t total = (size_t)width, padn = total - l;
	size_t lpad = side < 0 ? padn : side > 0 ? 0 : padn / 2;
	char *b = a_alloc(total + 1);
	memset(b, fill, lpad);
	memcpy(b + lpad, s, l);
	memset(b + lpad + l, fill, total - lpad - l);
	b[total] = '\0';
	return rv_str(b);
}

/* more path ops ----------------------------------------------------------- */

static rval *do_path(rval **a, size_t n) {      /* join components with "/" */
	char *out = a_alloc(1);
	out[0] = '\0';
	size_t len = 0, cap = 1;
	int first = 1;
	for (size_t i = 0; i < n; i++) {
		const char *p = as_cstr(a[i]);
		if (!*p) continue;
		size_t pl = strlen(p);
		if (len + pl + 2 > cap) { out = a_grow(out, len + 1, len + pl + 2); cap = len + pl + 2; }
		if (!first) out[len++] = '/';
		memcpy(out + len, p, pl);
		len += pl; out[len] = '\0';
		first = 0;
	}
	return rv_file(out);
}
static rval *do_abs(rval *v) {                   /* realpath, fallback to as-is */
	char *r = realpath(pstr(v), NULL);
	if (!r) return rv_file(a_dup(pstr(v)));
	rval *res = rv_file(a_dup(r));   /* realpath buffer is libc-owned */
	free(r);
	return res;
}

/* print / debug rendering ------------------------------------------------- */

/* a growable, arena-backed string builder (avoids per-print open_memstream). */
typedef struct { char *buf; size_t len, cap; } sbuf;

static void sb_ensure(sbuf *s, size_t add) {
	if (s->len + add + 1 > s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 64;
		while (nc < s->len + add + 1) nc *= 2;
		s->buf = a_grow(s->buf, s->len, nc);
		s->cap = nc;
	}
}
static void sb_putc(sbuf *s, char c) { sb_ensure(s, 1); s->buf[s->len++] = c; s->buf[s->len] = '\0'; }
static void sb_puts(sbuf *s, const char *p) {
	size_t n = strlen(p);
	sb_ensure(s, n);
	memcpy(s->buf + s->len, p, n);
	s->len += n;
	s->buf[s->len] = '\0';
}
static void sb_putl(sbuf *s, long v) { char t[24]; snprintf(t, sizeof t, "%ld", v); sb_puts(s, t); }

/* repr: a plain, readable rendering of any value, any shape. */
static void repr(sbuf *s, rval *v) {
	if (!v) { sb_puts(s, "()"); return; }
	switch (v->k) {
	case RV_NONE:  sb_puts(s, "()");                       break;
	case RV_INT:   sb_putl(s, v->i);                       break;
	case RV_BOOL:  sb_puts(s, v->i ? "true" : "false");    break;
	case RV_STR:   sb_puts(s, v->s ? v->s : "");           break;
	case RV_FILE:  sb_puts(s, v->s ? v->s : "");           break;
	case RV_LIST:
		sb_putc(s, '[');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			repr(s, v->items[i]);
		}
		sb_putc(s, ']');
		break;
	case RV_SEQ:   sb_puts(s, "<seq>");                    break;
	case RV_TUPLE:
		sb_putc(s, '(');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			repr(s, v->items[i]);
		}
		sb_putc(s, ')');
		break;
	case RV_CMD:
		sb_putc(s, '`');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_putc(s, ' ');
			repr(s, v->items[i]);
		}
		sb_putc(s, '`');
		break;
	case RV_MAP:
		sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			sb_puts(s, v->keys[i]);
			sb_puts(s, ": ");
			repr(s, v->items[i]);
		}
		sb_putc(s, '}');
		break;
	case RV_LAMBDA:  sb_puts(s, "<lambda>");                            break;
	case RV_MORPH:   sb_puts(s, "<"); sb_puts(s, v->from);
	                 sb_puts(s, " // "); sb_puts(s, v->to); sb_puts(s, ">"); break;
	case RV_BUILTIN: sb_puts(s, "<builtin "); sb_puts(s, v->s); sb_puts(s, ">"); break;
	case RV_THUNK:   sb_puts(s, "<thunk>");                     break;
	case RV_CFUN:    sb_puts(s, "<method>");                    break;
	case RV_OBJECT:
		sb_puts(s, v->cls ? ((fb_class_t *)v->cls)->name : "object");
		sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			sb_puts(s, v->keys[i]); sb_puts(s, ": "); repr(s, v->items[i]);
		}
		sb_putc(s, '}');
		break;
	}
}

/* debug: a typed, structural rendering (kind tags + quoting). */
static void debug_repr(sbuf *s, rval *v) {
	if (!v) { sb_puts(s, "None"); return; }
	switch (v->k) {
	case RV_NONE:  sb_puts(s, "None");                          break;
	case RV_INT:   sb_puts(s, "Int(");  sb_putl(s, v->i); sb_putc(s, ')'); break;
	case RV_BOOL:  sb_puts(s, v->i ? "Bool(true)" : "Bool(false)"); break;
	case RV_STR:   sb_puts(s, "Str(\""); sb_puts(s, v->s ? v->s : ""); sb_puts(s, "\")"); break;
	case RV_FILE:  sb_puts(s, "File("); sb_puts(s, v->s ? v->s : ""); sb_putc(s, ')'); break;
	case RV_LIST:
		sb_puts(s, "List#"); sb_putl(s, (long)v->n); sb_putc(s, '[');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, ']');
		break;
	case RV_SEQ:   sb_puts(s, "Seq");                          break;
	case RV_TUPLE:
		sb_puts(s, "Tuple#"); sb_putl(s, (long)v->n); sb_putc(s, '(');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, ')');
		break;
	case RV_CMD:
		sb_puts(s, "Cmd#"); sb_putl(s, (long)v->n); sb_putc(s, '[');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, ']');
		break;
	case RV_MAP:
		sb_puts(s, "Map#"); sb_putl(s, (long)v->n); sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			sb_putc(s, '"'); sb_puts(s, v->keys[i]); sb_puts(s, "\": ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, '}');
		break;
	case RV_LAMBDA:  sb_puts(s, "Lambda("); sb_putl(s, (long)(v->params ? v->params->nkids : 0)); sb_puts(s, " params)"); break;
	case RV_MORPH:   sb_puts(s, "Morph("); sb_puts(s, v->from); sb_puts(s, " // "); sb_puts(s, v->to); sb_putc(s, ')'); break;
	case RV_BUILTIN: sb_puts(s, "Builtin("); sb_puts(s, v->s); sb_putc(s, ')'); break;
	case RV_THUNK:   sb_puts(s, "Thunk");                       break;
	case RV_CFUN:    sb_puts(s, "Method");                      break;
	case RV_OBJECT:
		sb_puts(s, v->cls ? ((fb_class_t *)v->cls)->name : "Object");
		sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			sb_putc(s, '"'); sb_puts(s, v->keys[i]); sb_puts(s, "\": ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, '}');
		break;
	}
}

/* print(...) / debug(...): render every argument (space-separated) and emit,
 * deferring until the active run finishes if one is in progress. */
static rval *do_print(rval **a, size_t n, int dbg) {
	sbuf s = {0};
	for (size_t i = 0; i < n; i++) {
		if (i) sb_putc(&s, ' ');
		if (dbg) debug_repr(&s, a[i]);
		else     repr(&s, a[i]);
	}
	print_emit(s.buf ? s.buf : a_dup(""));   /* arena-owned line */
	return &RV_UNIT;
}

/* ---- JSON ------------------------------------------------------------- */
/* Values map: object -> RV_MAP, array -> RV_LIST, string -> RV_STR,
 * integer -> RV_INT, real -> RV_STR flagged numeric (i==1, no float type),
 * true/false -> RV_BOOL, null -> (). All arena-allocated. */

static rval *json_num(char *text, int isreal) {
	if (isreal) { rval *v = rv_str(text); v->i = 1; return v; }  /* i=1: numeric */
	return rv_int(strtol(text, NULL, 10));
}

static void json_ws(const char **p) {
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

/* parse a JSON string literal at *p (which points at the opening quote). */
static char *json_string(const char **p) {
	(*p)++;                                   /* skip opening quote */
	size_t cap = 16, len = 0;
	char *b = a_alloc(cap);
	while (**p && **p != '"') {
		char c = **p;
		if (c == '\\') {
			(*p)++;
			char e = **p;
			switch (e) {
			case 'n': c = '\n'; break;  case 't': c = '\t'; break;
			case 'r': c = '\r'; break;  case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;  case '/': c = '/';  break;
			case '"': c = '"';  break;  case '\\': c = '\\'; break;
			case 'u': {                 /* \uXXXX -> UTF-8 (BMP) */
				unsigned cp = 0;
				for (int k = 0; k < 4 && isxdigit((unsigned char)(*p)[1]); k++) {
					char h = *++(*p);
					cp = cp * 16 + (h <= '9' ? h - '0'
					              : (h | 32) - 'a' + 10);
				}
				if (len + 4 >= cap) { b = a_grow(b, len, cap *= 2); }
				if (cp < 0x80) b[len++] = (char)cp;
				else if (cp < 0x800) {
					b[len++] = (char)(0xC0 | (cp >> 6));
					b[len++] = (char)(0x80 | (cp & 0x3F));
				} else {
					b[len++] = (char)(0xE0 | (cp >> 12));
					b[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
					b[len++] = (char)(0x80 | (cp & 0x3F));
				}
				(*p)++;
				continue;
			}
			default: c = e; break;
			}
		}
		if (len + 1 >= cap) { b = a_grow(b, len, cap *= 2); }
		b[len++] = c;
		(*p)++;
	}
	if (**p == '"') (*p)++;
	b[len] = '\0';
	return b;
}

static rval *json_value(const char **p);

static rval *json_value(const char **p) {
	json_ws(p);
	char c = **p;
	if (c == '{') {
		(*p)++;
		rval *m = rv(RV_MAP);
		json_ws(p);
		if (**p == '}') { (*p)++; return m; }
		for (;;) {
			json_ws(p);
			if (**p != '"') break;
			char *k = json_string(p);
			json_ws(p);
			if (**p == ':') (*p)++;
			map_set(m, k, json_value(p));
			json_ws(p);
			if (**p == ',') { (*p)++; continue; }
			break;
		}
		json_ws(p);
		if (**p == '}') (*p)++;
		return m;
	}
	if (c == '[') {
		(*p)++;
		rval *l = rv(RV_LIST);
		json_ws(p);
		if (**p == ']') { (*p)++; return l; }
		for (;;) {
			rv_push(l, json_value(p));
			json_ws(p);
			if (**p == ',') { (*p)++; continue; }
			break;
		}
		json_ws(p);
		if (**p == ']') (*p)++;
		return l;
	}
	if (c == '"') return rv_str(json_string(p));
	if (!strncmp(*p, "true", 4))  { *p += 4; return rv_bool(1); }
	if (!strncmp(*p, "false", 5)) { *p += 5; return rv_bool(0); }
	if (!strncmp(*p, "null", 4))  { *p += 4; return &RV_UNIT; }
	if (c == '-' || isdigit((unsigned char)c)) {     /* number */
		const char *start = *p;
		int isreal = 0;
		if (**p == '-') (*p)++;
		while (isdigit((unsigned char)**p)) (*p)++;
		if (**p == '.') { isreal = 1; (*p)++; while (isdigit((unsigned char)**p)) (*p)++; }
		if (**p == 'e' || **p == 'E') {
			isreal = 1; (*p)++;
			if (**p == '+' || **p == '-') (*p)++;
			while (isdigit((unsigned char)**p)) (*p)++;
		}
		return json_num(a_dupn(start, *p - start), isreal);
	}
	(*p)++;                                   /* skip unknown byte */
	return &RV_UNIT;
}

static rval *do_json_parse(const char *s) {
	const char *p = s;
	return json_value(&p);
}

/* serialize a value as JSON into the string builder. */
static void json_emit(sbuf *s, rval *v, int pretty, int depth);

static void json_indent(sbuf *s, int depth) {
	sb_putc(s, '\n');
	for (int i = 0; i < depth; i++) sb_puts(s, "  ");
}

static void json_emit_str(sbuf *s, const char *p) {
	sb_putc(s, '"');
	for (; *p; p++) {
		switch (*p) {
		case '"':  sb_puts(s, "\\\""); break;
		case '\\': sb_puts(s, "\\\\"); break;
		case '\n': sb_puts(s, "\\n");  break;
		case '\t': sb_puts(s, "\\t");  break;
		case '\r': sb_puts(s, "\\r");  break;
		default:   sb_putc(s, *p);     break;
		}
	}
	sb_putc(s, '"');
}

static void json_emit(sbuf *s, rval *v, int pretty, int depth) {
	if (!v) { sb_puts(s, "null"); return; }
	switch (v->k) {
	case RV_NONE:  sb_puts(s, "null");                       break;
	case RV_INT:   sb_putl(s, v->i);                         break;
	case RV_BOOL:  sb_puts(s, v->i ? "true" : "false");      break;
	case RV_STR:
		if (v->i) sb_puts(s, v->s ? v->s : "0");      /* numeric (i==1) */
		else      json_emit_str(s, v->s ? v->s : "");
		break;
	case RV_FILE:  json_emit_str(s, v->s ? v->s : "");       break;
	case RV_LIST:
	case RV_TUPLE:
		sb_putc(s, '[');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_putc(s, ',');
			if (pretty) json_indent(s, depth + 1);
			json_emit(s, v->items[i], pretty, depth + 1);
		}
		if (pretty && v->n) json_indent(s, depth);
		sb_putc(s, ']');
		break;
	case RV_MAP:
		sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_putc(s, ',');
			if (pretty) json_indent(s, depth + 1);
			json_emit_str(s, v->keys[i]);
			sb_putc(s, ':');
			if (pretty) sb_putc(s, ' ');
			json_emit(s, v->items[i], pretty, depth + 1);
		}
		if (pretty && v->n) json_indent(s, depth);
		sb_putc(s, '}');
		break;
	default:       sb_puts(s, "null");                       break;
	}
}

static rval *do_json_str(rval *v, int pretty) {
	sbuf s = {0};
	json_emit(&s, v, pretty, 0);
	return rv_str(s.buf ? s.buf : a_dup(""));
}

/* map(items): build a Map from a list/tuple of (key, value) pairs. */
static rval *do_keys(rval *m) {
	rval *l = rv(RV_LIST);
	if (m && m->k == RV_MAP)
		for (size_t i = 0; i < m->n; i++) rv_push(l, rv_str(m->keys[i]));
	return l;
}
static rval *do_values(rval *m) {
	rval *l = rv(RV_LIST);
	if (m && m->k == RV_MAP)
		for (size_t i = 0; i < m->n; i++) rv_push(l, m->items[i]);
	return l;
}
static rval *do_items(rval *m) {
	rval *l = rv(RV_LIST);
	if (m && m->k == RV_MAP)
		for (size_t i = 0; i < m->n; i++) {
			rval *t = rv(RV_TUPLE);
			rv_push(t, rv_str(m->keys[i]));
			rv_push(t, m->items[i]);
			rv_push(l, t);
		}
	return l;
}
static rval *do_has(rval *m, rval *key) {
	if (!m || m->k != RV_MAP) return rv_bool(0);
	const char *k = as_cstr(key);
	for (size_t i = 0; i < m->n; i++)
		if (!strcmp(m->keys[i], k)) return rv_bool(1);
	return rv_bool(0);
}

/* is `name` a builtin (any arity)? used to make a bare reference a value */
static int is_builtin(const char *s) {
	static const char *names[] = {
		"glob","zip","subst","stale","run","env","shell","which","exists",
		"mkdir","read","write","move","copy","symlink","remove","touch",
		"is_dir","is_file","filter","contains","flatten","concat","fold","length",
		"first","last","take","drop","sort","unique","join","split","replace",
		"cat","upper","lower","trim","starts_with","ends_with",
		"parent","name","stem","suffix","with_suffix","with_name","path","abs",
		"print","debug",
		"json_parse","json_read","json_str","json_pretty","json_write",
		"get","has","keys","values","items",
		"range","enumerate","slice","index","reverse","sum","min","max","count",
		"find","to_int","to_str","repeat","chars",
		"flag","opt","positionals", NULL };
	for (size_t i = 0; names[i]; i++) if (!strcmp(s, names[i])) return 1;
	return fb_reg_find(s) != NULL;   /* registered host/plugin builtin */
}

/* apply a unary builtin to one argument (e.g. used in `list |> name`). Builtins
 * that need more than one argument can't be used bare and yield unit. */
static rval *call_builtin1(const char *n, rval *a) {
	const fb_Reg *rg = fb_reg_find(n);
	if (rg && rg->min <= 1 && (rg->max < 0 || rg->max >= 1)) {
		rval *as[1] = { a };
		return fb_dispatch(rg, as, 1);
	}
	return &RV_UNIT;   /* not a builtin, or one that can't be applied unary */
}

/* ---- command collection + execution ----------------------------------- */

/* append every command reachable from v into the cds-lib t_cmd (argv list with a
 * NULL after each command, as CMD() produces). */
static void collect(rval *v, t_cmd *cmds) {
	if (!v) return;
	switch (v->k) {
	case RV_CMD:
		for (size_t i = 0; i < v->n; i++)
			DA_APPEND(*cmds, as_cstr(v->items[i]));
		DA_APPEND(*cmds, NULL);
		break;
	case RV_LIST:            /* flatten composite into a command sequence */
	case RV_TUPLE:           /* a tuple of commands flattens the same way */
		for (size_t i = 0; i < v->n; i++)
			collect(v->items[i], cmds);
		break;
	case RV_SEQ:             /* deferred: evaluate each step now, in order */
		for (size_t i = 0; i < v->body->nkids; i++)
			collect(eval(v->body->kids[i], v->env), cmds);
		break;
	default:                  /* RV_NONE (skipped guard) / values: nop */
		break;
	}
}

/* a short label for a parallel task: the program name (argv[0]); else the
 * value's def label. */
static char *task_label(rval *v) {
	if (v->k == RV_CMD && v->n) return as_cstr(v->items[0]);
	if (v->label) return v->label;
	return "job";
}

/* does this value carry any command to run? */
static int has_cmd(rval *v) {
	if (!v) return 0;
	if (v->k == RV_CMD) return v->n > 0;
	if (v->k == RV_SEQ) return 1;   /* deferred: assume work until evaluated */
	if (v->k == RV_LIST || v->k == RV_TUPLE) {
		for (size_t i = 0; i < v->n; i++)
			if (has_cmd(v->items[i])) return 1;
	}
	return 0;
}

/* build_parallel_run reuses get_current_loop(); exec runs inside a coroutine
 * parked on the main loop, so run parallel on its own loop instead (from
 * cds-lib/build.c). */
static int run_parallel_isolated(t_parallel_ctx *par, t_env env) {
	t_loop *saved = coro_get_current_loop();
	coro_set_current_loop(NULL);
	int rc = build_parallel_run(par, env);
	coro_set_current_loop(saved);
	return rc;
}

/* execute a value, mirroring its structure onto the cds-lib runtime:
 *   RV_SEQ        -> a GROUP whose children run in order (>> dependency)
 *   RV_LIST/TUPLE -> a PARALLEL group of independent items
 *   RV_CMD        -> a GROUP running the single command
 * Each level (depth) is its own named group. Must run inside a coroutine on
 * one event loop (see run_coro). */
static void exec_run(rval *v) {
	if (!has_cmd(v)) return;
	char *label = v->label ? v->label : "run";

	switch (v->k) {
	case RV_SEQ:                            /* deferred: eval each step late */
		GROUP_NAMED(label) {
			ast *seqn = v->body;
			for (size_t i = 0; i < seqn->nkids; i++)
				exec_run(eval(seqn->kids[i], v->env));
		}
		break;

	case RV_LIST:                          /* the parallel IS the group */
	case RV_TUPLE:                         /* a tuple of commands -> parallel */
		PARALLEL_NAMED(label) {
			for (size_t i = 0; i < v->n; i++) {
				rval *it = v->items[i];
				if (!has_cmd(it)) continue;
				t_cmd c = {0};
				collect(it, &c);
				PARALLEL_SEQ(_par, task_label(it), c);
			}
			run_parallel_isolated(&_par, g_env);
		}
		break;

	case RV_CMD:
		GROUP_NAMED(label) {
			t_cmd c = {0};
			collect(v, &c);
			build_seq_visual(&c, g_env, &_grp);
		}
		break;

	default:
		break;
	}
}

/* run the whole tree inside one coroutine on one event loop, mirroring
 * cds-lib/build.c: nested GROUP/PARALLEL only render correctly when driven from a
 * coroutine parked on a single loop. */
CORO_DEF(run_coro, args, { rval *root; })
{
	exec_run(args->root);
	RETURN(NULL);
}

static void run_tree(rval *root) {
	if (!has_cmd(root)) return;
	/* silent build: echo each command as it starts (make-style), since the live
	 * UI that would show it is suppressed. shell() capture turns this off. */
	build_set_echo_start(build_get_silent());
	t_loop *loop = create_loop();
	g_in_run = 1;             /* RUN() may execute the coro body synchronously */
	RUN(loop, run_coro, .root = root);
	loop_run(loop);
	destroy_loop(loop);
	g_in_run = 0;
	print_flush();            /* emit anything print/debug queued during the run */
}

/* shell(v): like run() but silent (no UI), returning each child's captured
 * output as a String instead of streaming it. Reuses the library's capturing
 * runner (build_seq_visual into a silent group); we read back the per-command
 * build__buf and free what the group would otherwise hand to build_flush_outputs. */
static rval *capture_cmd(rval *v) {
	t_group_ctx grp = { .opts = { .silent = OPT_TRUE } };
	t_cmd c = {0};
	collect(v, &c);                       /* one argv (+ NULL terminator) */

	build_set_echo_start(0);              /* capture is quiet: no make-style echo */
	build_seq_visual(&c, g_env, &grp);    /* runs silent, captures into grp */

	size_t total = 0;
	for (size_t i = 0; i < grp.cmd_count; i++) {
		build__buf *b = (build__buf *)grp.commands[i].output;
		if (b) total += b->count;
	}
	char *out = a_alloc(total + 1);
	size_t n = 0;
	for (size_t i = 0; i < grp.cmd_count; i++) {
		build__buf *b = (build__buf *)grp.commands[i].output;
		if (b) {
			if (b->count) { memcpy(out + n, b->items, b->count); n += b->count; }
			free(b->items);
			free(b);
		}
		free(grp.commands[i].repr);
	}
	free(grp.commands);

	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;   /* trim trailing newline */
	out[n] = '\0';
	return rv_str(out);
}

/* run `pkg-config <args...>` (argv[0] must be "pkg-config"), capturing trimmed
 * stdout via the library's silent runner; sets *code to the child exit status.
 * Same capture/free pattern as capture_cmd, but keeps the exit code. */
static char *pkgconf_run(char **argv, size_t argc, int *code) {
	t_group_ctx grp = { .opts = { .silent = OPT_TRUE } };
	t_cmd c = {0};
	for (size_t i = 0; i < argc; i++) DA_APPEND(c, argv[i]);
	DA_APPEND(c, NULL);

	build_set_echo_start(0);
	build_seq_visual(&c, g_env, &grp);

	size_t total = 0;
	for (size_t i = 0; i < grp.cmd_count; i++) {
		build__buf *b = (build__buf *)grp.commands[i].output;
		if (b) total += b->count;
	}
	char *out = a_alloc(total + 1);
	size_t n = 0;
	int ec = 0;
	for (size_t i = 0; i < grp.cmd_count; i++) {
		build__buf *b = (build__buf *)grp.commands[i].output;
		if (b) {
			if (b->count) { memcpy(out + n, b->items, b->count); n += b->count; }
			free(b->items);
			free(b);
		}
		ec = grp.commands[i].exit_code;
		free(grp.commands[i].repr);
	}
	free(grp.commands);

	while (n && (out[n-1]=='\n' || out[n-1]=='\r' || out[n-1]==' ' || out[n-1]=='\t')) n--;
	out[n] = '\0';
	if (code) *code = ec;
	return out;
}

/* split on runs of whitespace, dropping empty tokens -> List<String> */
static rval *split_ws(const char *s) {
	rval *out = rv(RV_LIST);
	while (*s) {
		while (*s==' ' || *s=='\t' || *s=='\n' || *s=='\r') s++;
		if (!*s) break;
		const char *b = s;
		while (*s && *s!=' ' && *s!='\t' && *s!='\n' && *s!='\r') s++;
		rv_push(out, rv_str(a_dupn(b, s - b)));
	}
	return out;
}

/* pkgconfig(name) -> { found, cflags:List, libs:List, version } by shelling out
 * to pkg-config. `found` is pkg-config --exists succeeding; when absent the
 * lists are empty and version "". */
static rval *do_pkgconfig(const char *name) {
	char *nm = a_dup(name);
	rval *m = rv(RV_MAP);

	int code = 0;
	char *ex[] = { "pkg-config", "--exists", nm };
	(void)pkgconf_run(ex, 3, &code);
	int found = (code == 0);
	map_set(m, a_dup("found"), rv_bool(found));

	if (!found) {
		map_set(m, a_dup("cflags"),  rv(RV_LIST));
		map_set(m, a_dup("libs"),    rv(RV_LIST));
		map_set(m, a_dup("version"), rv_str(a_dup("")));
		return m;
	}

	char *cf[] = { "pkg-config", "--cflags", nm };
	char *lf[] = { "pkg-config", "--libs", nm };
	char *vf[] = { "pkg-config", "--modversion", nm };
	map_set(m, a_dup("cflags"),  split_ws(pkgconf_run(cf, 3, NULL)));
	map_set(m, a_dup("libs"),    split_ws(pkgconf_run(lf, 3, NULL)));
	map_set(m, a_dup("version"), rv_str(pkgconf_run(vf, 3, NULL)));
	return m;
}

static rval *do_shell(rval *v) {
	if (!v) return rv_str(a_dup(""));
	switch (v->k) {
	case RV_TUPLE: {                       /* nest captured strings per structure */
		rval *t = rv(RV_TUPLE);
		for (size_t i = 0; i < v->n; i++) rv_push(t, do_shell(v->items[i]));
		return t;
	}
	case RV_LIST: {
		rval *l = rv(RV_LIST);
		for (size_t i = 0; i < v->n; i++) rv_push(l, do_shell(v->items[i]));
		return l;
	}
	case RV_SEQ: {
		rval *t = rv(RV_TUPLE);
		for (size_t i = 0; i < v->body->nkids; i++)
			rv_push(t, do_shell(eval(v->body->kids[i], v->env)));
		return t;
	}
	case RV_CMD:
		return capture_cmd(v);
	default:
		return rv_str(a_dup(""));
	}
}

/* ---- command building ------------------------------------------------- */

/* an RV_CMD holds the argv built from the command's parts */
static rval *build_command(ast *node, renv *e) {
	rval *cmd = rv(RV_CMD);
	for (size_t i = 0; i < node->nkids; i++) {
		ast *part = node->kids[i];
		if (part->kind == AST_CMDTEXT) {
			rv_push(cmd, rv_str(dup_tok(part->tok)));
		} else if (part->kind == AST_SPLAT) {
			rval *lst = eval(part->kids[0], e);   /* {...OBJ} */
			if (lst->k == RV_LIST || lst->k == RV_TUPLE)
				for (size_t j = 0; j < lst->n; j++)
					rv_push(cmd, lst->items[j]);
			else
				rv_push(cmd, lst);
		} else {
			rv_push(cmd, eval(part, e));          /* {expr} */
		}
	}
	return cmd;
}

/* ---- evaluator -------------------------------------------------------- */

static rval *eval(ast *n, renv *e) {
	switch (n->kind) {
	case AST_INT:    return rv_int(strtol(n->tok->string, NULL, 10));
	case AST_BOOL:   return rv_bool(n->tok->len == 4);   /* "true" vs "false" */
	case AST_STRTEXT: return rv_str(str_unescape(n->tok->string, n->tok->len));
	case AST_STRING: {
		if (n->nkids == 0)                       /* plain string */
			return rv_str(str_unescape(n->tok->string, n->tok->len));
		/* interpolated: concatenate text chunks and {expr} values */
		size_t tot = 0;
		char **parts = a_alloc(n->nkids * sizeof *parts);
		for (size_t i = 0; i < n->nkids; i++) {
			parts[i] = a_dup(as_cstr(eval(n->kids[i], e)));
			tot += strlen(parts[i]);
		}
		char *b = a_alloc(tot + 1);
		b[0] = '\0';
		for (size_t i = 0; i < n->nkids; i++)
			strcat(b, parts[i]);
		return rv_str(b);
	}
	case AST_PATH:   return rv_file(dup_tok(n->tok));

	case AST_EAGER: {
		/* "~=": evaluate now and bind into the current scope, shadowing any
		 * lazy `=` def of the same name (env_get is consulted before defs). */
		rval *v = eval(n->kids[0], e);
		if (v && !v->label) v->label = dup_tok(n->tok);
		env_put(e, n->tok, v);
		return v;
	}

	case AST_FORCE: {
		/* "~x": re-evaluate x ignoring memoization. For a lazy top-level def
		 * that means re-running its rhs; an env-bound value is already final. */
		ast *operand = n->kids[0];
		if (operand->kind == AST_ID) {
			rval *b = env_get(e, operand->tok);
			if (b) return b;
			rval *d = def_force(operand->tok);
			if (d) return d;
		}
		return eval(operand, e);
	}

	case AST_ID: {
		rval *b = env_get(e, n->tok);
		if (b) return b;
		rval *d = def_lookup(n->tok);
		if (d) return d;
		if (tok_is(n->tok, "args")) return g_args;   /* CLI args list */
		if (tok_is(n->tok, "cwd")) return g_cwd;   /* CLI args list */
		char *nm = dup_tok(n->tok);
		if (is_builtin(nm)) {             /* a builtin used as a value */
			rval *v = rv(RV_BUILTIN);
			v->s = nm;
			return v;
		}
		return rv_str(nm);                /* bare symbol: literal word */
	}

	case AST_LAMBDA: {
		rval *v = rv(RV_LAMBDA);
		v->params = n->kids[0];
		v->body   = n->kids[1];
		v->env    = e;
		return v;
	}

	case AST_MORPHISM: {
		rval *v = rv(RV_MORPH);
		v->from = dup_tok(n->kids[0]->tok);
		v->to   = dup_tok(n->kids[1]->tok);
		return v;
	}

	case AST_TUPLE: {
		rval *t = rv(RV_TUPLE);
		for (size_t i = 0; i < n->nkids; i++)
			tuple_add(t, n->kids[i], e);
		return t;
	}

	case AST_MAPLIT: {                     /* { k: v } and Map<(k,v),...> */
		rval *m = rv(RV_MAP);
		for (size_t i = 0; i < n->nkids; i++) {
			rval *kv = eval(n->kids[i], e);  /* each entry: a 2-tuple */
			if (kv && (kv->k == RV_TUPLE || kv->k == RV_LIST) && kv->n >= 2)
				map_set(m, a_dup(as_cstr(kv->items[0])), kv->items[1]);
		}
		return m;
	}

	case AST_PARSER:                       /* bare Parser<schema> -> parse args */
		return do_parse(eval(n->kids[0], e), g_args);

	case AST_PIPE: {
		rval *acc = eval(n->kids[0], e);
		for (size_t i = 1; i < n->nkids; i++) {
			rval *fn = eval(n->kids[i], e);
			if (is_seq(acc)) {                 /* map element-wise */
				rval *out = rv(RV_LIST);
				for (size_t j = 0; j < acc->n; j++)
					rv_push(out, apply(fn, acc->items[j]));
				acc = out;
			} else {
				acc = apply(fn, acc);
			}
		}
		return acc;
	}

	case AST_SEQ: {
		/* deferred: a step's guards must see the filesystem AFTER prior
		 * steps run (>> is a real dependency), so keep the AST + env and
		 * evaluate each step lazily at exec time (see exec_run). */
		rval *v = rv(RV_SEQ);
		v->body = n;
		v->env  = e;
		return v;
	}

	case AST_GUARD: {
		ast *where = n->kids[1];
		ast *whenc = n->kids[2];
		/* arena-allocate the where-scope: a bound value may be a lambda or a
		 * deferred `>>` that captures this env and outlives this frame. */
		renv *bsc = e;
		if (where) {
			renv *ge = a_alloc(sizeof *ge);
			ge->parent = e;
			ge->arg    = cur_arg(e);
			for (size_t i = 0; i < where->nkids; i++) {
				ast *bd = where->kids[i];
				env_put(ge, bd->tok, eval(bd->kids[0], ge));
			}
			bsc = ge;
		}
		if (whenc) {
			rval *c = eval(whenc->kids[0], bsc);
			if (!(c->k == RV_BOOL && c->i))    /* gate */
				return &RV_UNIT;
		}
		return eval(n->kids[0], bsc);
	}

	case AST_FIELD: {
		rval *base = eval(n->kids[0], e);
		if (!n->tok && n->nkids == 2) {       /* dynamic ".{expr}" */
			rval *k = eval(n->kids[1], e);
			if (base->k == RV_MAP || base->k == RV_OBJECT) return map_get(base, as_cstr(k));
			if (k->k == RV_INT) return positional(base, (size_t)k->i);
			return file_field_s(base, as_cstr(k));
		}
		if (base->k == RV_OBJECT) {            /* method (unbound) or field */
			rval *m = class_method((fb_class_t *)base->cls, dup_tok(n->tok));
			return m ? m : map_get(base, dup_tok(n->tok));
		}
		if (base->k == RV_MAP)                 /* m.key -> map lookup */
			return map_get(base, dup_tok(n->tok));
		long idx = tok_index(n->tok);
		if (idx >= 0) return positional(base, (size_t)idx);
		return file_field(base, n->tok);
	}

	case AST_OBJNEW: {                     /* tag a fields map as an object */
		fb_class_t *c = class_ensure(dup_tok(n->tok));
		rval *fields = eval(n->kids[0], e);
		return make_object(c, fields, NULL);
	}

	case AST_METHODDEF: {                  /* method Class.name = lambda */
		fb_class_t *c = class_ensure(dup_tok(n->kids[0]->tok));
		rval *m = eval(n->kids[1], e);
		class_add_method(c, dup_tok(n->tok), m);
		return &RV_UNIT;
	}

	case AST_PROJECTION: {
		rval *b = n->tok ? env_get(e, n->tok) : NULL;
		if (b) return b;
		long idx = tok_index(n->tok);
		if (idx >= 0) return positional(cur_arg(e), (size_t)idx);
		return file_field(cur_arg(e), n->tok);
	}

	case AST_CAPTURE: {
		rval *b = n->nkids ? env_get(e, n->kids[0]->tok) : NULL;
		if (b) return b;
		rval *a = cur_arg(e);
		return a ? a : &RV_UNIT;
	}

	case AST_CONSTRUCT: {            /* File<*base, name=..., ...> -> a path */
		if (!tok_is(n->tok, "File"))
			return &RV_UNIT;
		rval *base = NULL;
		char *o_name = NULL, *o_stem = NULL, *o_suffix = NULL, *o_parent = NULL;
		for (size_t i = 0; i < n->nkids; i++) {
			ast *f = n->kids[i];                 /* AST_CFIELD */
			rval *fv = eval(f->kids[0], e);
			if (!f->tok) {                       /* *splat: inherit a File */
				if (fv->k == RV_FILE) base = fv;
			} else if (tok_is(f->tok, "name"))   o_name   = as_cstr(fv);
			  else if (tok_is(f->tok, "stem"))   o_stem   = as_cstr(fv);
			  else if (tok_is(f->tok, "suffix")) o_suffix = as_cstr(fv);
			  else if (tok_is(f->tok, "parent")) o_parent = as_cstr(fv);
			  /* unknown/custom fields don't affect the path */
		}
		const char *bp = (base && base->s) ? base->s : "";
		char *parent = o_parent ? a_dup(o_parent) : path_dir(bp);
		char *bname;
		if (o_name) {
			bname = a_dup(o_name);
		} else if (o_stem || o_suffix) {
			char *st = o_stem   ? a_dup(o_stem)   : path_stem(bp);
			char *su = o_suffix ? a_dup(o_suffix) : path_suffix(bp);
			bname = a_alloc(strlen(st) + strlen(su) + 1);
			sprintf(bname, "%s%s", st, su);
		} else {
			bname = path_base(bp);
		}
		char *path;
		if (parent[0] && strcmp(parent, ".") != 0) {
			path = a_alloc(strlen(parent) + 1 + strlen(bname) + 1);
			sprintf(path, "%s/%s", parent, bname);
		} else {
			path = a_dup(bname);
		}
		return rv_file(path);
	}

	case AST_COMPARE: {
		rval *l = eval(n->kids[0], e);
		rval *r = eval(n->kids[1], e);
		int res;
		if ((l->k == RV_INT || l->k == RV_BOOL) &&
		    (r->k == RV_INT || r->k == RV_BOOL)) {
			long a = l->i, b = r->i;
			switch (n->tok->kind) {
			case TOK_LT: res = a <  b; break;
			case TOK_GT: res = a >  b; break;
			case TOK_LE: res = a <= b; break;
			case TOK_GE: res = a >= b; break;
			case TOK_EQ: res = a == b; break;
			default:     res = a != b; break;   /* TOK_NE */
			}
		} else {
			int c = strcmp(as_cstr(l), as_cstr(r));
			switch (n->tok->kind) {
			case TOK_LT: res = c <  0; break;
			case TOK_GT: res = c >  0; break;
			case TOK_LE: res = c <= 0; break;
			case TOK_GE: res = c >= 0; break;
			case TOK_EQ: res = c == 0; break;
			default:     res = c != 0; break;
			}
		}
		return rv_bool(res);
	}

	case AST_ARITH: {                    /* + - * / %, integer */
		long a = eval(n->kids[0], e)->i;
		long b = eval(n->kids[1], e)->i;
		switch (n->tok->kind) {
		case TOK_PLUS:    return rv_int(a + b);
		case TOK_MINUS:   return rv_int(a - b);
		case TOK_STAR:    return rv_int(a * b);
		case TOK_SLASH:   return rv_int(b ? a / b : 0);
		default:          return rv_int(b ? a % b : 0);   /* TOK_PERCENT */
		}
	}

	case AST_LOGICAL: {                  /* and / or / ||, short-circuit */
		rval *l = eval(n->kids[0], e);
		if (n->tok->kind == TOK_OROR)   /* value-or: left if truthy, else right */
			return truthy(l) ? l : eval(n->kids[1], e);
		if (n->tok->kind == TOK_AND)
			return truthy(l) ? rv_bool(truthy(eval(n->kids[1], e)))
			                 : rv_bool(0);
		/* TOK_OR */
		return truthy(l) ? rv_bool(1)
		                 : rv_bool(truthy(eval(n->kids[1], e)));
	}

	case AST_NOT:
		return rv_bool(!truthy(eval(n->kids[0], e)));

	case AST_COMMAND:
		return build_command(n, e);

	case AST_CALL: {
		ast *callee = n->kids[0];
		size_t na = n->nkids - 1;
		if (callee->kind == AST_PARSER) {     /* Parser<schema>(argv) */
			rval *schema = eval(callee->kids[0], e);
			rval *argv = na >= 1 ? eval(n->kids[1], e) : g_args;
			return do_parse(schema, argv);
		}
		/* method call obj.m(args): if the base is an object whose class defines
		 * `m`, apply the method to (self, ...args). Otherwise fall through (the
		 * field may hold a plain callable). */
		if (callee->kind == AST_FIELD && callee->tok && callee->nkids == 1) {
			rval *self = eval(callee->kids[0], e);
			if (self->k == RV_OBJECT) {
				rval *m = class_method((fb_class_t *)self->cls, dup_tok(callee->tok));
				if (m) {
					size_t k; rval **as = eval_args(n, e, &k);
					rval *arg;
					if (k == 0) {
						arg = self;
					} else {
						arg = rv(RV_TUPLE);
						rv_push(arg, self);
						for (size_t i = 0; i < k; i++) rv_push(arg, as[i]);
					}
					return apply(m, arg);
				}
			}
		}
		if (callee->kind == AST_ID) {
			/* registered builtin (core + host/plugin). FB_LAZY builtins get
			 * their args as unevaluated thunks (no splat flatten); everyone else
			 * gets evaluated, splat-flattened args. Dispatch through the ABI. */
			const fb_Reg *rg = fb_reg_find_tok(callee->tok);
			if (rg) {
				if (rg->flags & FB_LAZY) {
					size_t na2 = n->nkids - 1;
					rval **th = na2 ? a_alloc(na2 * sizeof *th) : NULL;
					for (size_t i = 0; i < na2; i++) {
						rval *t = rv(RV_THUNK);
						t->body = n->kids[i + 1];
						t->env  = e;
						th[i] = t;
					}
					return fb_dispatch(rg, th, na2);
				}
				size_t k; rval **as = eval_args(n, e, &k);
				return fb_dispatch(rg, as, k);
			}
		}
		/* user function: one arg, or a tuple of args (splats flatten) */
		rval *fn = eval(callee, e);
		size_t k;
		rval **as = eval_args(n, e, &k);
		rval *arg;
		if (k == 1) {
			arg = as[0];
		} else {
			arg = rv(RV_TUPLE);
			for (size_t i = 0; i < k; i++)
				rv_push(arg, as[i]);
		}
		return apply(fn, arg);
	}

	default:
		fatal("cannot execute node '%s'", ast_name[n->kind]);
	}
	return &RV_UNIT;
}

/* ============================================================================
 * Builtin registration: every core builtin is exposed through the extension
 * ABI (fbuild.h). Thin wrappers marshal already-evaluated fb_Value args to the
 * existing do_* implementations; the registry table below is the single source
 * of truth for names, arity, and (for the checker, on extern names) return type.
 * `bench` is registered FB_LAZY (its args stay unevaluated); everything else is
 * eager. No builtin is special-cased in eval() anymore.
 * ========================================================================== */

#define AR(i) ((rval *)a[i])
#define AC(i) as_cstr((rval *)a[i])
#define AI(k) (((rval *)a[k])->i)
#define RV(x) ((fb_Value)(x))

/* generators for the common wrapper shapes */
#define WV1(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(fn(AR(0)));}
#define WV2(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(fn(AR(0),AR(1)));}
#define WC1(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(fn(AC(0)));}
#define WC2(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(fn(AC(0),AC(1)));}
#define WVN(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;return RV(fn((rval**)a,(size_t)n));}

WC1(w_glob, do_glob)          WVN(w_zip, do_zip)          WVN(w_stale, do_stale)
WC1(w_env, do_env)            WV1(w_shell, do_shell)      WC1(w_which, do_which)
WV1(w_exists, do_exists)      WV1(w_mkdir, do_mkdir)      WV1(w_read, do_read)
WV2(w_write, do_write)        WV2(w_move, do_move)        WV2(w_copy, do_copy)
WV2(w_symlink, do_symlink)    WV1(w_remove, do_remove)    WV1(w_touch, do_touch)
WV1(w_is_dir, do_is_dir)      WV1(w_is_file, do_is_file)
WV2(w_filter, do_filter)      WV2(w_contains, do_contains)WV1(w_flatten, do_flatten)
WV1(w_length, do_length)      WV1(w_first, do_first)      WV1(w_last, do_last)
WV2(w_take, do_take)          WV2(w_drop, do_drop)        WV1(w_sort, do_sort)
WV1(w_unique, do_unique)      WV1(w_enumerate, do_enumerate) WV1(w_reverse, do_reverse)
WV2(w_count, do_count)        WVN(w_concat, do_concat)    WVN(w_cat, do_cat)
WC2(w_split, do_split)        WV1(w_upper, do_upper)      WV1(w_lower, do_lower)
WV1(w_trim, do_trim)          WV1(w_capitalize, do_capitalize) WV1(w_title, do_title)
WV1(w_lstrip, do_lstrip)      WV1(w_rstrip, do_rstrip)
WV2(w_strip_prefix, do_strip_prefix) WV2(w_strip_suffix, do_strip_suffix)
WV2(w_starts_with, do_starts_with)   WV2(w_ends_with, do_ends_with)
WC2(w_find, do_find)          WV1(w_to_int, do_to_int)    WV1(w_to_str, do_to_str)
WC1(w_chars, do_chars)        WV1(w_abs, do_abs)          WVN(w_path, do_path)
WV2(w_has, do_has)            WV1(w_keys, do_keys)        WV1(w_values, do_values)
WV1(w_items, do_items)        WV1(w_sleep, do_sleep)      WV1(w_config, do_config)
WV2(w_opt, do_opt)            WVN(w_sum, do_sum)          WVN(w_min, do_min)
WVN(w_max, do_max)

/* wrappers with argument reshaping / mixed types */
static fb_Value w_subst(fb_State*S,fb_Value*a,int n){(void)S;(void)n;
	return RV(rv_str(subst_a(AC(1), AC(2), AC(0)))); }        /* subst(s,from,to) */
static fb_Value w_join(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_join(AR(0), AC(1)));}
static fb_Value w_replace(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_replace(AC(0),AC(1),AC(2)));}
static fb_Value w_repeat(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_repeat(AC(0), AI(1)));}
static fb_Value w_with_suffix(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_with_suffix(AR(0), AC(1)));}
static fb_Value w_with_name(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_with_name(AR(0), AC(1)));}
static fb_Value w_parent(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(rv_str(path_dir(pstr(AR(0)))));}
static fb_Value w_name(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(rv_str(path_base(pstr(AR(0)))));}
static fb_Value w_stem(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(rv_str(path_stem(pstr(AR(0)))));}
static fb_Value w_suffix(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(rv_str(path_suffix(pstr(AR(0)))));}
static fb_Value w_fold(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_fold(AR(0),AR(1),AR(2)));}
static fb_Value w_range(fb_State*S,fb_Value*a,int n){(void)S;
	return RV(n == 1 ? do_range(0, AI(0)) : do_range(AI(0), AI(1))); }
static fb_Value w_slice(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_slice(AR(0), AI(1), AI(2)));}
static fb_Value w_index(fb_State*S,fb_Value*a,int n){(void)S;return RV(do_index(AR(0), AR(1), n >= 3 ? AR(2) : NULL));}
static fb_Value w_get(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(map_get(AR(0), AC(1)));}
static fb_Value w_clock(fb_State*S,fb_Value*a,int n){(void)S;(void)a;(void)n;return RV(do_clock());}
static fb_Value w_time(fb_State*S,fb_Value*a,int n){(void)S;(void)a;(void)n;return RV(do_time());}
static fb_Value w_positionals(fb_State*S,fb_Value*a,int n){(void)S;(void)a;(void)n;return RV(do_positionals());}
static fb_Value w_json_parse(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_json_parse(AC(0)));}
static fb_Value w_json_read(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_json_parse(as_cstr(do_read(AR(0)))));}
static fb_Value w_json_str(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_json_str(AR(0), 0));}
static fb_Value w_json_pretty(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_json_str(AR(0), 1));}
static fb_Value w_json_write(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_write(AR(0), do_json_str(AR(1), 0)));}
static fb_Value w_print(fb_State*S,fb_Value*a,int n){(void)S;return RV(do_print((rval**)a,(size_t)n,0));}
static fb_Value w_debug(fb_State*S,fb_Value*a,int n){(void)S;return RV(do_print((rval**)a,(size_t)n,1));}
static fb_Value w_flag(fb_State*S,fb_Value*a,int n){(void)S;return RV(do_flag((rval**)a,(size_t)n));}
static fb_Value l_pkgconfig(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_pkgconfig(AC(0)));}
static fb_Value w_pad(fb_State*S,fb_Value*a,int n,int side){(void)S;
	char fill = ' ';
	if (n >= 3) { const char *f = AC(2); if (f[0]) fill = f[0]; }
	return RV(pad_to(AC(0), AI(1), fill, side)); }
static fb_Value w_pad_left (fb_State*S,fb_Value*a,int n){return w_pad(S,a,n,-1);}
static fb_Value w_pad_right(fb_State*S,fb_Value*a,int n){return w_pad(S,a,n, 1);}
static fb_Value w_center   (fb_State*S,fb_Value*a,int n){return w_pad(S,a,n, 0);}
/* bench(label, expr): FB_LAZY -- times evaluation of the (unevaluated) expr
 * between two clock reads, prints elapsed when --bench is set, returns expr's
 * value so it stays transparent in a pipeline. The reference lazy builtin. */
static fb_Value w_bench(fb_State*S,fb_Value*a,int n){(void)n;
	const char *label = as_cstr((rval *)fb_force(S, a[0]));
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rval *r = (rval *)fb_force(S, a[1]);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (g_bench) {
		double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3
		          + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
		size_t cap = strlen(label) + 64;
		char *line = a_alloc(cap);
		snprintf(line, cap, "[bench] %s: %.3f ms", label, ms);
		print_emit(line);
	}
	return (fb_Value)r; }
/* run(targets...): build a target (single, or a tuple = parallel), run it,
 * return whether it carried any work. Eager: args are evaluated commands/seqs. */
static fb_Value w_run(fb_State*S,fb_Value*a,int n){(void)S;
	rval *target;
	if (n == 1) target = AR(0);
	else { target = rv(RV_TUPLE); for (int i = 0; i < n; i++) rv_push(target, AR(i)); }
	int ran = has_cmd(target);
	run_tree(target);
	return RV(rv_bool(ran)); }

/* Register the interpreter's ABI builtins. Idempotent (fb_register dedups). */
static void fb_open_base(fb_State *S) {
	static const fb_Reg base[] = {
		/* io / process */
		{"env",w_env,1,1,FB_RET_STR,0}, {"shell",w_shell,1,1,FB_RET_ANY,0},
		{"which",w_which,1,1,FB_RET_FILE,0}, {"pkgconfig",l_pkgconfig,1,1,FB_RET_MAP,0},
		{"run",w_run,1,-1,FB_RET_BOOL,0}, {"print",w_print,0,-1,FB_RET_UNIT,0},
		{"debug",w_debug,0,-1,FB_RET_UNIT,0}, {"config",w_config,1,1,FB_RET_UNIT,0},
		/* time */
		{"clock",w_clock,0,0,FB_RET_INT,0}, {"time",w_time,0,0,FB_RET_INT,0},
		{"sleep",w_sleep,1,1,FB_RET_UNIT,0}, {"bench",w_bench,2,2,FB_RET_ANY,FB_LAZY},
		/* filesystem */
		{"glob",w_glob,1,1,FB_RET_LIST,0}, {"exists",w_exists,1,1,FB_RET_BOOL,0},
		{"mkdir",w_mkdir,1,1,FB_RET_FILE,0}, {"read",w_read,1,1,FB_RET_STR,0},
		{"write",w_write,2,2,FB_RET_FILE,0}, {"move",w_move,2,2,FB_RET_FILE,0},
		{"copy",w_copy,2,2,FB_RET_FILE,0}, {"symlink",w_symlink,2,2,FB_RET_FILE,0},
		{"remove",w_remove,1,1,FB_RET_BOOL,0}, {"touch",w_touch,1,1,FB_RET_FILE,0},
		{"is_dir",w_is_dir,1,1,FB_RET_BOOL,0}, {"is_file",w_is_file,1,1,FB_RET_BOOL,0},
		{"stale",w_stale,1,-1,FB_RET_BOOL,0},
		/* list */
		{"zip",w_zip,2,-1,FB_RET_LIST,0}, {"filter",w_filter,2,2,FB_RET_LIST,0},
		{"contains",w_contains,2,2,FB_RET_BOOL,0}, {"flatten",w_flatten,1,1,FB_RET_LIST,0},
		{"concat",w_concat,0,-1,FB_RET_LIST,0}, {"fold",w_fold,3,3,FB_RET_ANY,0},
		{"length",w_length,1,1,FB_RET_INT,0}, {"first",w_first,1,1,FB_RET_ANY,0},
		{"last",w_last,1,1,FB_RET_ANY,0}, {"take",w_take,2,2,FB_RET_LIST,0},
		{"drop",w_drop,2,2,FB_RET_LIST,0}, {"sort",w_sort,1,1,FB_RET_LIST,0},
		{"unique",w_unique,1,1,FB_RET_LIST,0}, {"range",w_range,1,2,FB_RET_LIST,0},
		{"enumerate",w_enumerate,1,1,FB_RET_LIST,0}, {"slice",w_slice,3,3,FB_RET_LIST,0},
		{"index",w_index,2,3,FB_RET_INT,0}, {"reverse",w_reverse,1,1,FB_RET_LIST,0},
		{"sum",w_sum,1,-1,FB_RET_INT,0}, {"min",w_min,1,-1,FB_RET_INT,0},
		{"max",w_max,1,-1,FB_RET_INT,0}, {"count",w_count,2,2,FB_RET_INT,0},
		/* string */
		{"cat",w_cat,0,-1,FB_RET_STR,0}, {"join",w_join,2,2,FB_RET_STR,0},
		{"split",w_split,2,2,FB_RET_LIST,0}, {"replace",w_replace,3,3,FB_RET_STR,0},
		{"subst",w_subst,3,3,FB_RET_STR,0}, {"upper",w_upper,1,1,FB_RET_STR,0},
		{"lower",w_lower,1,1,FB_RET_STR,0}, {"trim",w_trim,1,1,FB_RET_STR,0},
		{"capitalize",w_capitalize,1,1,FB_RET_STR,0}, {"title",w_title,1,1,FB_RET_STR,0},
		{"lstrip",w_lstrip,1,1,FB_RET_STR,0}, {"rstrip",w_rstrip,1,1,FB_RET_STR,0},
		{"strip_prefix",w_strip_prefix,2,2,FB_RET_STR,0}, {"strip_suffix",w_strip_suffix,2,2,FB_RET_STR,0},
		{"pad_left",w_pad_left,2,3,FB_RET_STR,0}, {"pad_right",w_pad_right,2,3,FB_RET_STR,0},
		{"center",w_center,2,3,FB_RET_STR,0}, {"starts_with",w_starts_with,2,2,FB_RET_BOOL,0},
		{"ends_with",w_ends_with,2,2,FB_RET_BOOL,0}, {"find",w_find,2,2,FB_RET_INT,0},
		{"repeat",w_repeat,2,2,FB_RET_STR,0}, {"chars",w_chars,1,1,FB_RET_LIST,0},
		{"to_int",w_to_int,1,1,FB_RET_INT,0}, {"to_str",w_to_str,1,1,FB_RET_STR,0},
		/* path */
		{"parent",w_parent,1,1,FB_RET_STR,0}, {"name",w_name,1,1,FB_RET_STR,0},
		{"stem",w_stem,1,1,FB_RET_STR,0}, {"suffix",w_suffix,1,1,FB_RET_STR,0},
		{"with_suffix",w_with_suffix,2,2,FB_RET_FILE,0}, {"with_name",w_with_name,2,2,FB_RET_FILE,0},
		{"path",w_path,0,-1,FB_RET_FILE,0}, {"abs",w_abs,1,1,FB_RET_FILE,0},
		/* json / map */
		{"json_parse",w_json_parse,1,1,FB_RET_ANY,0}, {"json_read",w_json_read,1,1,FB_RET_ANY,0},
		{"json_str",w_json_str,1,1,FB_RET_STR,0}, {"json_pretty",w_json_pretty,1,1,FB_RET_STR,0},
		{"json_write",w_json_write,2,2,FB_RET_FILE,0}, {"get",w_get,2,2,FB_RET_ANY,0},
		{"has",w_has,2,2,FB_RET_BOOL,0}, {"keys",w_keys,1,1,FB_RET_LIST,0},
		{"values",w_values,1,1,FB_RET_LIST,0}, {"items",w_items,1,1,FB_RET_LIST,0},
		/* cli args */
		{"flag",w_flag,1,-1,FB_RET_BOOL,0}, {"opt",w_opt,2,2,FB_RET_STR,0},
		{"positionals",w_positionals,0,0,FB_RET_LIST,0},
	};
	for (size_t i = 0; i < sizeof base / sizeof base[0]; i++)
		fb_register(S, &base[i]);
}

/* public init (fbuild.h): register ABI builtins before check/run. Idempotent. */
void fb_init(void) { fb_open_base(g_active); }

#undef AR
#undef AC
#undef AI
#undef RV
#undef WV1
#undef WV2
#undef WC1
#undef WC2
#undef WVN

/* ---- driver ----------------------------------------------------------- */

/* version string embedded in .my_metadata (see build_info above). */
const char *fbuild_version(void) { return build_info.version; }

void run_program(ast *prog, int argc, char **argv) {
	fb_init();   /* ensure ABI builtins are registered (idempotent) */

	/* language-level `args` = CLI args forwarded to the script (auto-discovered).
	 * If a `:::` separator is present, only args after it are forwarded (args
	 * before it are for fbuild itself); otherwise all args are forwarded. */
	int start = 1;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], ":::")) { start = i + 1; break; }

	g_args = rv(RV_LIST);
	for (int i = start; i < argc; i++) {
		rv_push(g_args, rv_str(a_dup(argv[i])));
		if (!strcmp(argv[i], "--bench")) g_bench = 1;
	}

	g_cwd = rv(RV_STR);
	g_cwd->s = a_alloc(1024);
	getcwd(g_cwd->s, 1024);

	g_ndefs = 0;
	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		if (s->kind == AST_ASSIGN && g_ndefs < 512) {
			g_defs[g_ndefs].name  = s->tok;
			g_defs[g_ndefs].rhs   = s->kids[0];
			g_defs[g_ndefs].val   = NULL;
			g_defs[g_ndefs].doing = 0;
			g_defs[g_ndefs].pure  = ast_pure(s->kids[0]);
			g_ndefs++;
		}
	}

	size_t ne = 0;
	while (environ[ne]) ne++;
	g_env = (t_env){ .items = environ, .count = ne, .capacity = 0 };

	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		if (s->kind != AST_ASSIGN)
			eval(s, &g_top);     /* run(...) statements execute here */
	}

	build_flush_outputs();
}

/* ============================================================================
 * Embedding API (fbuild.h) — single implicit state (S0/S1).
 * ========================================================================== */

/* save the live globals into S's snapshot */
static void fb_gsave(struct fb_State *S) {
	fb_globals *g = &S->snap;
	memcpy(g->defs, g_defs, sizeof g_defs); g->ndefs = g_ndefs;
	g->top = g_top; g->env = g_env; g->args = g_args; g->cwd = g_cwd;
	g->in_run = g_in_run; g->bench = g_bench;
	g->print_q = g_print_q; g->print_n = g_print_n; g->print_cap = g_print_cap;
	memcpy(g->regs, g_regs, sizeof g_regs); g->nreg = g_nreg;
	g->classes = g_classes; g->spinner = g_spinner;
}
/* load S's snapshot into the live globals */
static void fb_gload(struct fb_State *S) {
	fb_globals *g = &S->snap;
	memcpy(g_defs, g->defs, sizeof g_defs); g_ndefs = g->ndefs;
	g_top = g->top; g_env = g->env; g_args = g->args; g_cwd = g->cwd;
	g_in_run = g->in_run; g_bench = g->bench;
	g_print_q = g->print_q; g_print_n = g->print_n; g_print_cap = g->print_cap;
	memcpy(g_regs, g->regs, sizeof g_regs); g_nreg = g->nreg;
	g_classes = g->classes; g_spinner = g->spinner;
}
static void fb_activate(struct fb_State *S) {
	if (g_active == S) return;
	if (g_active) fb_gsave(g_active);
	g_active = S;
	fb_gload(S);
	arena_use(S->arena);
}

fb_State *fb_new(void) {
	struct fb_State *S = calloc(1, sizeof *S);
	if (!S) { fprintf(stderr, "out of memory\n"); exit(1); }
	S->arena = arena_new();
	fb_activate(S);   /* loads S's (zeroed) globals + points the arena at S */
	fb_init();        /* register core builtins into S's registry */
	return S;
}

void fb_close(fb_State *S) {
	build_ui_cleanup();                    /* release the shared draw buffer */
	arena_free(S->arena);
	if (g_active == S) g_active = NULL;   /* live globals now dangle until next activate */
	free(S);
}

void fb_set_args(fb_State *S, int argc, char **argv) {
	S->argc = argc;
	S->argv = argv;
}

const char *fb_error_msg(fb_State *S) { return S->err_msg; }

static int fb_load_env_plugins(fb_State *S);

int fb_dostring(fb_State *S, const char *src, const char *chunkname) {
	(void)chunkname;
	fb_activate(S);
	S->err_msg[0] = '\0';
	fb_init();
	if (fb_load_env_plugins(S)) return 1;   /* $FBUILD_PLUGINS, before check */
	p = a_alloc(sizeof *p + sizeof(token) * MAX_TOKENS);
	lex(a_dup(src));                 /* lexer wants a writable, arena-owned buf */
	ast *prog = parse_program();
	if (getenv("DUMP_AST")) ast_print(prog, 0);

	/* runtime fb_error() unwinds here (parse-time die() still exits: S1 limit). */
	if (setjmp(S->err_jmp)) { S->err_set = 0; return 1; }
	S->err_set = 1;

	int errs = infer_program(prog);
	if (errs) {
		S->err_set = 0;
		snprintf(S->err_msg, sizeof S->err_msg,
		         "%d type error%s", errs, errs == 1 ? "" : "s");
		return 1;
	}
	run_program(prog, S->argc, S->argv);
	S->err_set = 0;
	return 0;
}

int fb_dofile(fb_State *S, const char *path) {
	fb_activate(S);
	FILE *f = fopen(path, "rb");
	if (!f) { snprintf(S->err_msg, sizeof S->err_msg, "cannot open '%s'", path); return 1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	if (n < 0) { fclose(f); snprintf(S->err_msg, sizeof S->err_msg, "cannot read '%s'", path); return 1; }
	rewind(f);
	char *buf = a_alloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = '\0';
	return fb_dostring(S, buf, path);
}

fb_Value fb_global(fb_State *S, const char *name) {
	fb_activate(S);
	token t = { .string = (char *)name, .len = strlen(name) };
	rval *v = def_lookup(&t);
	return (fb_Value)(v ? v : &RV_UNIT);
}

int fb_load_plugin(fb_State *S, const char *path) {
	fb_activate(S);
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		snprintf(S->err_msg, sizeof S->err_msg, "plugin: %s", dlerror());
		return 1;
	}
	fb_abi_fn abi = (fb_abi_fn)dlsym(h, "fbuild_abi");
	if (!abi) {
		snprintf(S->err_msg, sizeof S->err_msg,
		         "plugin '%s': missing fbuild_abi", path);
		dlclose(h); return 1;
	}
	if (abi() != FBUILD_ABI) {
		snprintf(S->err_msg, sizeof S->err_msg,
		         "plugin '%s': ABI %d, expected %d", path, abi(), FBUILD_ABI);
		dlclose(h); return 1;
	}
	fb_open_fn open_fn = (fb_open_fn)dlsym(h, "fbuild_open");
	if (!open_fn) {
		snprintf(S->err_msg, sizeof S->err_msg,
		         "plugin '%s': missing fbuild_open", path);
		dlclose(h); return 1;
	}
	if (open_fn(S) != 0) {
		if (!S->err_msg[0])
			snprintf(S->err_msg, sizeof S->err_msg, "plugin '%s': open failed", path);
		dlclose(h); return 1;
	}
	/* keep h open for the process lifetime: registered fns point into it */
	return 0;
}

/* Load colon-separated plugin paths from $FBUILD_PLUGINS (once). */
static int fb_load_env_plugins(fb_State *S) {
	static int done = 0;
	if (done) return 0;
	done = 1;
	const char *env = getenv("FBUILD_PLUGINS");
	if (!env || !*env) return 0;
	char *list = a_dup(env), *save = NULL;
	for (char *t = strtok_r(list, ":", &save); t; t = strtok_r(NULL, ":", &save))
		if (fb_load_plugin(S, t)) return 1;
	return 0;
}
