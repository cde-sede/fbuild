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


/* Executor core: eval, apply, environment, execution, the fb_* embedding API.
 * Runtime types and the tiny value constructors live in run_internal.h; the
 * builtin library lives in run_builtins.c; build.h's implementation lives in
 * build_impl.c. */

/* This TU owns the single static build_info (.my_metadata) that
 * fbuild_version() reads back, so it defines METADATA_LIST before the header
 * pulls in build.h. */
#define FBUILD_VERSION "0.3.1"
#define _version FBUILD_VERSION
#define METADATA_LIST(X) X(version, FBUILD_VERSION)

#include "run_internal.h"
#include "subst.c"   /* _subst: its single definition lives in this TU */

/* shared globals (declared extern in run_internal.h) */
rval RV_UNIT = { .k = RV_NONE };
rval *g_spinner;     /* custom spinner glyph fn; saved/restored per state */

/* _subst (subst.c) returns a libc malloc'd string; move it into the arena. */
char *subst_a(const char *from, const char *to, const char *s) {
	char *r = _subst(from, to, s);
	char *a = a_dup(r ? r : "");
	free(r);
	return a;
}

/* map (JSON object) helpers: insertion-ordered key->value, linear lookup. */
void map_set(rval *m, char *key, rval *val) {
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
rval *map_get(rval *m, const char *key) {
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

/* render a double the way funcbuild prints Floats: shortest round-trip form,
 * but always with a '.' (or exponent) so it stays visually distinct from an
 * Int (2.0, never bare 2). */
char *fmt_double(double d) {
	char *b = a_alloc(32);
	snprintf(b, 32, "%.15g", d);
	if (!strpbrk(b, ".en")) {           /* no '.', no exponent, no nan/inf */
		size_t n = strlen(b);
		b = a_grow(b, n + 1, n + 3);
		b[n] = '.'; b[n+1] = '0'; b[n+2] = '\0';
	}
	return b;
}

/* decimal render of a long, arena-backed (hot path: no snprintf) */
static char *ltoa_a(long x) {
	char tmp[24];
	char *p = tmp + sizeof tmp;
	unsigned long u = x < 0 ? -(unsigned long)x : (unsigned long)x;
	do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
	if (x < 0) *--p = '-';
	return a_dupn(p, (size_t)(tmp + sizeof tmp - p));
}

/* a runtime value rendered as a command-line argument */
char *as_cstr(rval *v) {
	if (!v) return "";
	switch (v->k) {
	case RV_STR:
	case RV_FILE: return v->s ? v->s : "";
	case RV_INT:
	case RV_BOOL: return ltoa_a(v->i);
	case RV_FLOAT: return fmt_double(v->f);
	default:      return "";
	}
}

/* numeric value as a double, promoting Int/Bool; used by arithmetic/compare
 * once either operand is a Float. */
static double as_dbl(rval *v) { return v->k == RV_FLOAT ? v->f : (double)v->i; }

/* ---- environment ------------------------------------------------------ */

static void env_put(renv *e, token *name, rval *val) {
	struct rbind *b = e->nslots < 2 ? &e->slots[e->nslots++]
	                                : a_alloc(sizeof *b);
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
t_env  g_env;
rval  *g_args;          /* the `args` builtin variable (List<String>) */
rval  *g_cwd;           /* the `cwd` builtin variable (String) */
rval  *g_globals;       /* `_GLOBALS`: per-state mutable store (RV_MAP)  */
rval  *g_shared;        /* `_SHARED`: process-global store, NOT snapshot */
/* Two-arena model: g_main holds durable data (parse tree, top-level defs, the
 * stores, promoted results); g_scratch holds per-iteration temporaries that are
 * reclaimed after each element. Both point into the active state. During an
 * iteration the current arena (what a_alloc targets) is switched to g_scratch;
 * results are promoted back to g_main with rv_clone before the scratch is
 * popped. Set by fb_activate. */
static void  *g_main;
void  *g_scratch;

/* print/debug must not write while a run owns the terminal (the cds-lib runtime
 * is drawing the build graph). Output produced during a run is queued and
 * flushed once the event loop finishes. */
static int    g_in_run;
int    g_bench;          /* --bench on the CLI: emit bench() timings */
static char **g_print_q;       /* arena-backed; lines are arena strings */
static size_t g_print_n, g_print_cap;

/* line must be arena-owned (see do_print). */
void print_emit(char *line) {
	if (g_in_run) {
		/* The queue is flushed after the run, but a run step may execute inside
		 * a scratch region (reclaimed >> step). Keep both the queue and each
		 * line in the durable arena so a scratch pop can't dangle them. */
		void *cur = arena_current();
		arena_use(g_main);
		if (arena_owns(g_scratch, line)) line = a_dup(line);
		if (g_print_n == g_print_cap) {
			size_t nc = g_print_cap ? g_print_cap * 2 : 8;
			g_print_q = a_grow(g_print_q, g_print_n * sizeof *g_print_q,
			                   nc * sizeof *g_print_q);
			g_print_cap = nc;
		}
		g_print_q[g_print_n++] = line;
		arena_use(cur);
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

rval *eval(ast *n, renv *e);

/* Mutable stores backing `_GLOBALS`/`_SHARED` and the fb_global / fb_shared
 * C API. Insertion-ordered maps, created on first use. Phase 1: values are
 * stored by reference in the active arena (safe while the arena is permanent);
 * a later phase gives these their own arena + deep-copy-on-write (rv_clone). */
rval *store_globals(void) {
	if (!g_globals) g_globals = rv(RV_MAP);
	return g_globals;
}
rval *store_shared(void) {
	if (!g_shared) g_shared = rv(RV_MAP);   /* NOTE: in the creating state's arena */
	return g_shared;
}

/* --- generational promotion (scratch -> durable) ---------------------------
 * rv_clone deep-copies the parts of `v` that live in the scratch arena into the
 * CURRENT arena (callers set it to g_main first), sharing any node that is
 * already durable. So promoting an iteration result copies only the fresh
 * temporaries; references to top-level defs / globals are shared, not
 * duplicated. env_clone does the same for a closure's captured frames: the
 * per-call frame (in scratch) is copied, the enclosing global frames shared,
 * which is what keeps closures escaping a loop cheap and, more importantly,
 * valid after the scratch is popped. A value already durable clones to itself. */
static rval *rv_clone(rval *v);

static char *str_promote(char *s) {   /* dup a string only if it's scratch-owned */
	return (s && arena_owns(g_scratch, s)) ? a_dup(s) : s;
}

static renv *env_clone(renv *e) {
	if (!e || !arena_owns(g_scratch, e)) return e;   /* durable frame: share */
	renv *c = a_alloc(sizeof *c);
	c->arg    = rv_clone(e->arg);
	c->parent = env_clone(e->parent);
	c->head   = NULL;
	struct rbind **tail = &c->head;                  /* preserve binding order */
	for (struct rbind *b = e->head; b; b = b->next) {
		struct rbind *nb = a_alloc(sizeof *nb);
		nb->name = b->name;                          /* token: permanent arena */
		nb->val  = rv_clone(b->val);
		nb->next = NULL;
		*tail = nb; tail = &nb->next;
	}
	return c;
}

static rval *rv_clone(rval *v) {
	if (!v || !arena_owns(g_scratch, v)) return v;   /* durable / NULL: share */
	rval *c = rv(v->k);
	*c = *v;                                         /* shallow, then fix owned */
	c->label = str_promote(c->label);
	switch (v->k) {
	case RV_STR: case RV_FILE: case RV_BUILTIN:
		c->s = str_promote(v->s);
		break;
	case RV_LIST: case RV_TUPLE: case RV_CMD:
		c->items = NULL; c->n = 0; c->cap = 0;
		for (size_t i = 0; i < v->n; i++) rv_push(c, rv_clone(v->items[i]));
		break;
	case RV_MAP: case RV_OBJECT:
		c->items = NULL; c->keys = NULL; c->n = 0; c->cap = 0;
		for (size_t i = 0; i < v->n; i++)
			map_set(c, str_promote(v->keys[i]), rv_clone(v->items[i]));
		/* cls (registry) and nat (host data) are durable/shared as-is */
		break;
	case RV_LAMBDA: case RV_THUNK:
		c->env = env_clone(v->env);   /* params/body AST live in permanent arena */
		break;
	case RV_MORPH:
		c->from = str_promote(v->from);
		c->to   = str_promote(v->to);
		break;
	default:                          /* RV_INT/BOOL/NONE etc: shallow copy ok */
		break;
	}
	return c;
}

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
			if (!truthy(c))                     /* gated out */
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
			/* A def is program-lifetime and may be memoized, so force it in the
			 * durable arena even when the reference happens inside a scratch
			 * region (e.g. a |> element): a scratch-allocated value would be
			 * freed at the next pop and dangle on the next lookup. */
			void *cur = arena_current();
			arena_use(g_main);
			rval *v = eval(d->rhs, &g_top);
			arena_use(cur);
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
			void *cur = arena_current();          /* force durably (see def_lookup) */
			arena_use(g_main);
			rval *v = eval(d->rhs, &g_top);
			arena_use(cur);
			d->doing = 0;
			if (v && !v->label) v->label = dup_tok(name);
			return v;
		}
	return NULL;
}

/* ---- file fields ------------------------------------------------------ */

rval *file_field_s(rval *f, const char *name) {
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

rval *file_field(rval *f, token *name) {
	if (!name) return &RV_UNIT;
	char buf[64];
	size_t k = name->len < sizeof buf - 1 ? name->len : sizeof buf - 1;
	memcpy(buf, name->string, k);
	buf[k] = '\0';
	return file_field_s(f, buf);
}

/* path component helpers (return heap strings) for File<...> construction */
char *path_dir(const char *p) {
	const char *s = strrchr(p, '/');
	return s ? a_dupn(p, s - p) : a_dup(".");
}
char *path_base(const char *p) {
	const char *s = strrchr(p, '/');
	return a_dup(s ? s + 1 : p);
}
char *path_stem(const char *p) {
	char *b = path_base(p);
	char *d = strrchr(b, '.');
	if (d) *d = '\0';
	return b;
}
char *path_suffix(const char *p) {
	char *b = path_base(p);
	char *d = strrchr(b, '.');
	char *r = a_dup(d ? d : "");
	return r;
}

/* ---- builtins --------------------------------------------------------- */

/* join an output-relative dir with a child name (no leading "./", no "//") */
void joinp(char *buf, size_t sz, const char *rel, const char *name) {
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
int is_seq(rval *v) { return v && (v->k == RV_LIST || v->k == RV_TUPLE); }

rval *do_glob(const char *pat) {
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
rval *do_zip(rval **args, size_t k) {
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
rval *do_stale(rval **args, size_t n) {
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

/* tuple with exact capacity, one items allocation (vs rv_push doubling) */
static rval *rv_tuple_n(rval **items, size_t k) {
	rval *t = rv(RV_TUPLE);
	if (k) {
		t->items = a_alloc(k * sizeof *t->items);
		memcpy(t->items, items, k * sizeof *t->items);
		t->n = t->cap = k;
	}
	return t;
}

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
		case AST_TUPLE:
			/* (a, b): destructure this one positional argument by field name
			 * (map or object), same lookup as method self-destructuring. */
			for (size_t j = 0; j < p->nkids; j++) {
				ast *sub = p->kids[j];
				if (sub->tok) env_put(e, sub->tok, map_get(pt, dup_tok(sub->tok)));
			}
			break;
		default:
			break;
		}
	}
}


/* ---- multiple interpreters (fb_State) --------------------------------------
 * The interpreter keeps its working set in the g_* globals above/below. Each
 * fb_State owns its own arena plus a saved snapshot of those globals; switching
 * the active state (fb_activate) saves the outgoing state's globals into its
 * snapshot and loads the incoming state's, and points the arena at the new one.
 * So several independent interpreters can coexist (they don't run interleaved,
 * matching the single-threaded coroutine model). Declared before apply()
 * because apply dispatches native callables (RV_CFUN) through g_active. */
#define FB_MAX_REG 10240   /* core builtins + host/plugin (full raylib ~660) */
#define FB_MAX_BINDS 64    /* plugin-bound global identifiers (fb_global_bind) */

typedef struct { const char *name; rval *v; } fb_bind;

typedef struct {
	def    defs[512]; size_t ndefs;
	renv   top; t_env env;
	rval  *args, *cwd, *globals;
	int    in_run, bench;
	char **print_q; size_t print_n, print_cap;
	fb_Reg regs[FB_MAX_REG]; int nreg;
	fb_bind binds[FB_MAX_BINDS]; int nbinds;
	rval  *classes;
	rval  *spinner;
} fb_globals;

#define FB_MAX_PLUGINS 64

struct fb_State {
	jmp_buf   err_jmp;      /* protected-call boundary                     */
	int       err_set;      /* 1 when a boundary is installed              */
	char      err_msg[512];
	int       err_line, err_col;   /* position for the current err_msg, if any (0 = unknown) */
	int       argc;         /* args handed to the script (fb_set_args)     */
	char    **argv;
	void     *arena;        /* this interpreter's durable arena (g_main)   */
	void     *scratch;      /* per-iteration scratch arena (reclaimed)     */
	fb_globals snap;        /* globals held while this state is inactive   */
	const char *plugins[FB_MAX_PLUGINS]; int nplugins; /* fb_load_plugin() log */
	int       env_plugins_done;   /* fb_load_env_plugins() ran for this state */
};
struct fb_State *g_active;   /* the interpreter whose globals are live */
static void fb_activate(struct fb_State *S);   /* make S's globals live */

rval *apply(rval *fn, rval *arg) {
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

/* apply fn to arg inside a scratch mark, promoting the result to the durable
 * arena and reclaiming the call's temporaries. Use in per-element loops where
 * only the (promoted) result is kept. No-op wrapper when there's no scratch or
 * we're already nested in one (the outer frame owns that reclamation). Caller
 * must be in the durable arena. */
rval *apply_scratch(rval *fn, rval *arg) {
	if (!g_scratch || arena_current() == g_scratch)
		return apply(fn, arg);
	void  *durable = arena_current();
	size_t mark    = arena_pos(g_scratch);
	arena_use(g_scratch);
	rval *r = apply(fn, arg);
	arena_use(durable);
	r = rv_clone(r);
	arena_reset_to(g_scratch, mark);
	return r;
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
static int    g_reg_gen = 1;        /* bumped on every registration; never 0 so
                                     * zeroed ast memo fields read as stale */

void fb_register(fb_State *S, const fb_Reg *reg) {
	fb_activate(S);
	if (!reg || !reg->name || !reg->fn) return;
	g_reg_gen++;
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
int fb_reg_has_class(const char *class_name) {
	size_t len = strlen(class_name);
	for (int i = 0; i < g_nreg; i++)
		if (!strncmp(g_regs[i].name, class_name, len) && g_regs[i].name[len] == '.')
			return 1;
	return 0;
}
int fb_reg_class_methods(const char *class_name, const fb_Reg **out, int max_out) {
	size_t len = strlen(class_name);
	int n = 0;
	for (int i = 0; i < g_nreg; i++)
		if (!strncmp(g_regs[i].name, class_name, len) && g_regs[i].name[len] == '.') {
			if (n < max_out) out[n] = &g_regs[i];
			n++;
		}
	return n;
}
int fb_reg_count(fb_State *S) { (void)S; return g_nreg; }
const fb_Reg *fb_reg_at(fb_State *S, int i) {
	(void)S;
	return (i >= 0 && i < g_nreg) ? &g_regs[i] : NULL;
}
static const fb_Reg *fb_reg_find_tok(token *t) {
	return t ? fb_reg_findn(t->string, t->len) : NULL;
}

static fb_bind g_binds[FB_MAX_BINDS];   /* live plugin globals (part of active state) */
static int     g_nbinds;

/* Bind `v` to a bare top-level identifier `name` (checked by AST_ID eval
 * ahead of the builtin-registry fallback, so `name` reads as a plain value,
 * e.g. a namespace object: `name.method(...)` with no script-side
 * construction). Last bind wins if called again with the same name. */
void fb_global_bind(fb_State *S, const char *name, fb_Value v) {
	fb_activate(S);
	for (int i = 0; i < g_nbinds; i++)
		if (!strcmp(g_binds[i].name, name)) { g_binds[i].v = (rval *)v; return; }
	if (g_nbinds < FB_MAX_BINDS) {
		g_binds[g_nbinds].name = a_dup(name);
		g_binds[g_nbinds].v = (rval *)v;
		g_nbinds++;
	} else {
		fprintf(stderr, "fbuild: global bind table full (%d), dropped '%s'\n",
		        FB_MAX_BINDS, name);
	}
}
static rval *fb_bind_lookup(token *t) {
	for (int i = 0; i < g_nbinds; i++)
		if (tok_is(t, g_binds[i].name)) return g_binds[i].v;
	return NULL;
}

/* value constructors */
fb_Value fb_nil (fb_State *S)              { (void)S; return (fb_Value)&RV_UNIT; }
fb_Value fb_int (fb_State *S, long v)      { (void)S; return (fb_Value)rv_int(v); }
fb_Value fb_float(fb_State *S, double v)   { (void)S; return (fb_Value)rv_float(v); }
fb_Value fb_bool(fb_State *S, int v)       { (void)S; return (fb_Value)rv_bool(v); }
fb_Value fb_str (fb_State *S, const char *s){ (void)S; return (fb_Value)rv_str(a_dup(s ? s : "")); }
fb_Value fb_file(fb_State *S, const char *p){ (void)S; return (fb_Value)rv_file(a_dup(p ? p : "")); }
fb_Value fb_list(fb_State *S)              { (void)S; return (fb_Value)rv(RV_LIST); }
fb_Value fb_tuple(fb_State *S)             { (void)S; return (fb_Value)rv(RV_TUPLE); }
fb_Value fb_map (fb_State *S)              { (void)S; return (fb_Value)rv(RV_MAP); }
fb_Value fb_cmd (fb_State *S)              { (void)S; return (fb_Value)rv(RV_CMD); }
void     fb_push(fb_State *S, fb_Value l, fb_Value e) { (void)S; rv_push((rval*)l, (rval*)e); }
void     fb_tuple_push(fb_State *S, fb_Value t, fb_Value e) { (void)S; rv_push((rval*)t, (rval*)e); }
void     fb_cmd_push(fb_State *S, fb_Value c, fb_Value w) { (void)S; rv_push((rval*)c, (rval*)w); }
void     fb_set (fb_State *S, fb_Value m, const char *k, fb_Value v) {
	(void)S; map_set((rval*)m, a_dup(k ? k : ""), (rval*)v);
}

/* Mutable stores: _GLOBALS is the active state's store; _SHARED is process-
 * global. get returns nil when absent. Operate on the active state (call after
 * fb_activate / inside a run); the language sees the same maps as `_GLOBALS`
 * and `_SHARED`. This is the supported replacement for host-side static slots. */
fb_Value fb_global_get(fb_State *S, const char *k)              { (void)S; return (fb_Value)map_get(store_globals(), k ? k : ""); }
void     fb_global_set(fb_State *S, const char *k, fb_Value v)  { (void)S; map_set(store_globals(), a_dup(k ? k : ""), (rval*)v); }
fb_Value fb_shared_get(fb_State *S, const char *k)             { (void)S; return (fb_Value)map_get(store_shared(),  k ? k : ""); }
void     fb_shared_set(fb_State *S, const char *k, fb_Value v) { (void)S; map_set(store_shared(),  a_dup(k ? k : ""), (rval*)v); }

/* value inspection */
fb_Kind fb_kind(fb_Value v) {
	rval *r = (rval*)v;
	if (!r) return FB_NIL;
	switch (r->k) {
	case RV_NONE:   return FB_NIL;
	case RV_INT:    return FB_INT;
	case RV_FLOAT:  return FB_FLOAT;
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
long        fb_toint(fb_Value v)             { rval *r=(rval*)v; if (!r) return 0; return r->k == RV_FLOAT ? (long)r->f : r->i; }
double      fb_tofloat(fb_Value v)           { rval *r=(rval*)v; if (!r) return 0.0; return r->k == RV_FLOAT ? r->f : (double)r->i; }
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
		arg = rv_tuple_n((rval **)args, nargs < 0 ? 0 : (size_t)nargs);
	}
	return (fb_Value)apply((rval*)fn, arg);
}

static fb_Value fb_error_vat(fb_State *S, int line, int col, const char *fmt, va_list ap) {
	if (!S) S = g_active;
	vsnprintf(S->err_msg, sizeof S->err_msg, fmt, ap);
	S->err_line = line; S->err_col = col;
	if (S->err_set) longjmp(S->err_jmp, 1);   /* embedding: unwind to boundary */
	fprintf(stderr, "error: %s\n", S->err_msg);
	exit(1);                                   /* CLI: no boundary installed  */
}

fb_Value fb_error(fb_State *S, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	fb_Value r = fb_error_vat(S, 0, 0, fmt, ap);
	va_end(ap);
	return r;
}

fb_Value fb_error_at(fb_State *S, int line, int col, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	fb_Value r = fb_error_vat(S, line, col, fmt, ap);
	va_end(ap);
	return r;
}

void fb_error_pos(fb_State *S, int *line, int *col) {
	if (!S) S = g_active;
	if (line) *line = S->err_line;
	if (col)  *col  = S->err_col;
}

/* Dispatch a registered builtin: enforce arity, then call. Args are already
 * evaluated (splats flattened). */
rval *fb_dispatch(const fb_Reg *rg, rval **as, size_t k) {
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

static rval *g_classes;   /* RV_CLASS list, linked via ->cls (see rkind) */

static rval *class_find(const char *name) {
	for (rval *c = g_classes; c; c = (rval *)c->cls)
		if (!strcmp(c->s, name)) return c;
	return NULL;
}
static rval *class_ensure(const char *name) {
	rval *c = class_find(name);
	if (c) return c;
	c = rv(RV_CLASS);
	c->s = a_dup(name);
	c->cls = g_classes;   /* intrusive next-ptr, not a class (this IS one) */
	g_classes = c;
	return c;
}
/* methods are stored map-shaped (keys/items), so this is just map_set. */
static void class_add_method(rval *c, const char *name, rval *val) {
	map_set(c, a_dup(name), val);
}
static rval *class_method(rval *c, const char *name) {
	if (!c) return NULL;
	for (size_t i = 0; i < c->n; i++)
		if (!strcmp(c->keys[i], name)) return c->items[i];
	return NULL;
}

/* fb_class_field: declared native DATA fields (fb_set() fields, not methods)
 * live in the SAME class rval's map slots as methods (class_add_method),
 * under a key no real identifier can ever collide with ('\x01' isn't a valid
 * identifier byte) -- piggybacks the class rval's existing per-fb_State
 * snapshot/restore (g_classes) for free, no separate global table needed.
 * Value stored is a 2-tuple (Int ret, ret_class Str|Unit), read back by
 * fb_native_field_type(). */
static void class_add_field(rval *c, const char *name, fb_Ret ret, const char *ret_class) {
	char *key = a_alloc(strlen(name) + 4);
	sprintf(key, "\x01f:%s", name);
	rval *info = rv(RV_TUPLE);
	rv_push(info, rv_int((long)ret));
	rv_push(info, ret_class ? rv_str(a_dup(ret_class)) : rv(RV_NONE));
	class_add_method(c, key, info);   /* same map-slot storage, distinct key namespace */
}
static rval *class_field_info(rval *c, const char *name) {
	if (!c) return NULL;
	char stackkey[128];
	char *key = stackkey;
	size_t need = strlen(name) + 4;
	if (need > sizeof stackkey) key = a_alloc(need);
	sprintf(key, "\x01f:%s", name);
	return class_method(c, key);
}

/* true when every entry in a `{ "k": v, ... }` fields body is a plain
 * AST_TUPLE(key, value) with a literal, non-interpolated string key -- a
 * bareword or shorthand key desugars to this shape too (see TOK_LBRACE in
 * build_system.c). A `...spread` entry is a bare AST_SPLAT, not a tuple, and
 * its source is only known at runtime, so it always fails this check. Such a
 * key can't depend on args/env, so the key set is identical on every
 * evaluation of this body, letting AST_OBJNEW capture it once per class
 * instead of re-evaluating/re-allocating it on every construction. */
static int objnew_keys_static(ast *body) {
	for (size_t i = 0; i < body->nkids; i++) {
		ast *entry = body->kids[i];
		if (entry->kind != AST_TUPLE) return 0;   /* e.g. a ...spread entry */
		ast *key = entry->kids[0];
		if (key->kind != AST_STRING || key->nkids != 0) return 0;
	}
	return 1;
}
/* build an object of class c: share `fields`' map slots (if any) + native data */
static rval *make_object(rval *c, rval *fields, void *nat) {
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

/* ABI: define classes, attach native methods, instantiate native objects.
 * fb_Class is a distinct opaque type from fb_Value at the ABI boundary (so the
 * compiler still catches a class passed where a value is expected), but is
 * internally the same rval arena representation (kind RV_CLASS). */
fb_Class fb_class(fb_State *S, const char *name) {
	fb_activate(S); return (fb_Class)class_ensure(name);
}
void fb_method(fb_State *S, fb_Class c, const char *name, fb_CFunction fn,
               const fb_Reg *meta) {
	fb_activate(S);
	class_add_method((rval *)c, name, make_cfun(fn));
	if (!meta) return;
	/* Shadow flat-registry entry "ClassName.method" so walk.c's checker/
	 * hover can find this method's shape (see native_method_type there) --
	 * only ever looked up by that composite string (never a valid bare
	 * identifier token), so it's unreachable through ordinary call dispatch
	 * despite reusing the real `fn` pointer (simpler than a dummy stub, and
	 * fb_register rejects a NULL fn outright). min/max/ret/doc/param_types/
	 * nparams/ret_class/ret_union/param_type_unions are the only fields
	 * actually read; name/fn are already this call's own params, and a
	 * method is never FB_GLOBAL. */
	rval *cls = (rval *)c;
	size_t cn = strlen(cls->s), mn = strlen(name);
	char *key = a_alloc(cn + 1 + mn + 1);
	sprintf(key, "%s.%s", cls->s, name);
	fb_register(S, &(fb_Reg){ .name = key, .fn = fn, .min = meta->min,
	                          .max = meta->max, .ret = meta->ret,
	                          .doc = meta->doc, .param_types = meta->param_types,
	                          .nparams = meta->nparams, .ret_class = meta->ret_class,
	                          .ret_union = meta->ret_union,
	                          .param_type_unions = meta->param_type_unions });
}
fb_Value fb_object(fb_State *S, fb_Class c, void *data) {
	fb_activate(S); return (fb_Value)make_object((rval *)c, NULL, data);
}
void *fb_object_data(fb_Value v) {
	rval *r = (rval *)v;
	return (r && r->k == RV_OBJECT) ? r->nat : NULL;
}
void fb_class_field(fb_State *S, fb_Class c, const char *name, fb_Ret type,
                     const char *ret_class) {
	fb_activate(S);
	class_add_field((rval *)c, name, type, ret_class);
}
int fb_native_field_type(const char *class_name, const char *field_name,
                          fb_Ret *out_ret, const char **out_ret_class) {
	rval *c = class_find(class_name);
	rval *info = c ? class_field_info(c, field_name) : NULL;
	if (!info) return 0;
	if (out_ret) *out_ret = (fb_Ret)info->items[0]->i;
	if (out_ret_class) *out_ret_class = info->items[1]->k == RV_STR ? info->items[1]->s : NULL;
	return 1;
}

/* fb_reg_param_class: checker-only per-param native-class annotations, kept
 * OUTSIDE fb_Reg (see fbuild.h) -- a small process-global list (deliberately
 * not per-fb_State/g_classes-hung like class_add_field: it's not reachable
 * from any class rval, only from a plain registry-name string). */
typedef struct fb_pclass_ent {
	char *reg_name;
	int   index;
	char *class_name;
	struct fb_pclass_ent *next;
} fb_pclass_ent;
static fb_pclass_ent *g_pclasses;

void fb_reg_param_class(const char *reg_name, int index, const char *class_name) {
	fb_pclass_ent *e = a_alloc(sizeof *e);
	e->reg_name = a_dup(reg_name);
	e->index = index;
	e->class_name = a_dup(class_name);
	e->next = g_pclasses;
	g_pclasses = e;
}
const char *fb_reg_param_class_at(const char *reg_name, int index) {
	for (fb_pclass_ent *e = g_pclasses; e; e = e->next)
		if (e->index == index && !strcmp(e->reg_name, reg_name)) return e->class_name;
	return NULL;
}

/* ---- more builtins ---------------------------------------------------- */

int truthy(rval *v) {
	if (!v || v->k == RV_NONE) return 0;
	if (v->k == RV_BOOL || v->k == RV_INT) return v->i != 0;
	if (v->k == RV_FLOAT) return v->f != 0.0;
	return 1;
}

/* path of a file/string argument */
const char *pstr(rval *v) {
	return (v && (v->k == RV_FILE || v->k == RV_STR) && v->s) ? v->s : "";
}

/* config / process ----------------------------------------------------- */

/* monotonic clock in nanoseconds; subtract two reads to time a section. */

/* ---- command collection + execution ----------------------------------- */

/* append every command reachable from v into the cds-lib t_cmd (argv list with a
 * NULL after each command, as CMD() produces). */
void collect(rval *v, t_cmd *cmds) {
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
int has_cmd(rval *v) {
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
		/* No scratch reclamation here: the runtime executes commands
		 * asynchronously (exec_run schedules onto the event loop and returns
		 * before children finish), so the argv strings must stay live past this
		 * scope -- popping a scratch per step would free them mid-run. */
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
	CRETURN(NULL);   /* RETURN alias is impl-only; CRETURN is the public macro */
}

void run_tree(rval *root) {
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

/* numeric fast path: evaluate an arithmetic subtree without boxing Int
 * intermediates (an rval per +|-|*|/|% node is the allocation-heavy part of
 * hot loops), while staying Float-correct -- a Float anywhere in the subtree
 * (only possible at a non-arithmetic leaf; AST_INT/AST_ARITH are never
 * Float) flips the whole expression to double math from that point up. Each
 * leaf is still evaluated exactly once via eval(), so this is safe for
 * effectful operands. */
typedef struct { long i; double f; int is_float; } numval;

static numval eval_num(ast *n, renv *e) {
	switch (n->kind) {
	case AST_INT:
		if (!n->ival_set) { n->ival = strtol(n->tok->string, NULL, 10); n->ival_set = 1; }
		return (numval){ .i = n->ival };
	case AST_ARITH: {
		numval a = eval_num(n->kids[0], e);
		numval b = eval_num(n->kids[1], e);
		if (a.is_float || b.is_float) {
			double x = a.is_float ? a.f : (double)a.i;
			double y = b.is_float ? b.f : (double)b.i;
			double r;
			switch (n->tok->kind) {
			case TOK_PLUS:  r = x + y; break;
			case TOK_MINUS: r = x - y; break;
			case TOK_STAR:  r = x * y; break;
			case TOK_SLASH: r = y != 0.0 ? x / y : 0.0; break;
			default:        r = y != 0.0 ? fmod(x, y) : 0.0; break;   /* TOK_PERCENT */
			}
			return (numval){ .f = r, .is_float = 1 };
		}
		long x = a.i, y = b.i, r;
		switch (n->tok->kind) {
		case TOK_PLUS:  r = x + y; break;
		case TOK_MINUS: r = x - y; break;
		case TOK_STAR:  r = x * y; break;
		case TOK_SLASH: r = y ? x / y : 0; break;
		default:        r = y ? x % y : 0; break;   /* TOK_PERCENT */
		}
		return (numval){ .i = r };
	}
	default: {
		rval *v = eval(n, e);
		if (v->k == RV_FLOAT) return (numval){ .f = v->f, .is_float = 1 };
		return (numval){ .i = v->i };
	}
	}
}

rval *eval(ast *n, renv *e) {
	switch (n->kind) {
	case AST_INT:
		if (!n->ival_set) { n->ival = strtol(n->tok->string, NULL, 10); n->ival_set = 1; }
		return rv_int(n->ival);
	case AST_FLOAT:  return rv_float(strtod(a_dupn(n->tok->string, n->tok->len), NULL));
	case AST_BOOL:   return rv_bool(n->tok->len == 4);   /* "true" vs "false" */
	case AST_NIL:    return &RV_UNIT;
	case AST_STRTEXT: return rv_str(str_unescape(n->tok->string, n->tok->len));
	case AST_STRING: {
		if (n->nkids == 0)                       /* plain string */
			return rv_str(str_unescape(n->tok->string, n->tok->len));
		/* interpolated: concatenate text chunks and {expr} values. as_cstr
		 * results are arena-stable, so keep pointers and memcpy once. */
		size_t tot = 0;
		char **parts = a_alloc(n->nkids * sizeof *parts);
		size_t *lens = a_alloc(n->nkids * sizeof *lens);
		for (size_t i = 0; i < n->nkids; i++) {
			parts[i] = as_cstr(eval(n->kids[i], e));
			lens[i]  = strlen(parts[i]);
			tot += lens[i];
		}
		char *b = a_alloc(tot + 1), *w = b;
		for (size_t i = 0; i < n->nkids; i++) {
			memcpy(w, parts[i], lens[i]);
			w += lens[i];
		}
		*w = '\0';
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

	case AST_STORE: {
		/* "lvalue <- rhs": mutate a slot of a store map in place. lvalue is a
		 * field/index chain; its base must evaluate to a map (typically
		 * _GLOBALS/_SHARED, or a nested map). The write is eager. */
		ast *lv = n->kids[0];
		rval *rhs = eval(n->kids[1], e);
		if (lv->kind != AST_FIELD)
			return (rval *)fb_error(g_active, "<- target is not a writable field");
		rval *base = eval(lv->kids[0], e);
		if (!base || (base->k != RV_MAP && base->k != RV_OBJECT))
			return (rval *)fb_error(g_active, "<- target base is not a map");
		const char *ks = (!lv->tok && lv->nkids == 2)   /* dynamic ".{expr}" */
		               ? as_cstr(eval(lv->kids[1], e)) : NULL;
		/* The store outlives the current scope, so commit key + value into the
		 * durable arena -- a scratch-allocated rhs would dangle after a pop. */
		void *cur = arena_current();
		arena_use(g_main);
		char *key   = ks ? a_dup(ks) : dup_tok(lv->tok);
		rval *stored = rv_clone(rhs);
		map_set(base, key, stored);
		arena_use(cur);
		return stored;
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
		rval *bv = fb_bind_lookup(n->tok);   /* plugin global (fb_global_bind) */
		if (bv) return bv;
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

	case AST_LISTLIT: {                    /* List<e, ...> : a real list literal */
		rval *t = rv(RV_LIST);
		for (size_t i = 0; i < n->nkids; i++)
			tuple_add(t, n->kids[i], e);
		return t;
	}

	case AST_MAPLIT: {                     /* { k: v, ...spread } and Map<(k,v),...> */
		rval *m = rv(RV_MAP);
		for (size_t i = 0; i < n->nkids; i++) {
			ast *entry = n->kids[i];
			if (entry->kind == AST_SPLAT) {
				rval *src = eval(entry->kids[0], e);
				if (!src || (src->k != RV_MAP && src->k != RV_OBJECT))
					return (rval *)fb_error(g_active,
						"...: cannot spread a non-map/object value into a map literal");
				for (size_t j = 0; j < src->n; j++)
					map_set(m, a_dup(src->keys[j]), src->items[j]);
				continue;
			}
			rval *kv = eval(entry, e);  /* each entry: a 2-tuple */
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
				rval *out = rv(RV_LIST);       /* durable; items promoted in */
				for (size_t j = 0; j < acc->n; j++)
					rv_push(out, apply_scratch(fn, acc->items[j]));
				acc = out;
			} else {
				acc = apply_scratch(fn, acc);   /* scalar pipe: reclaim temps */
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
			if (!truthy(c))                    /* gate */
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
			rval *m = class_method((rval *)base->cls, dup_tok(n->tok));
			return m ? m : map_get(base, dup_tok(n->tok));
		}
		if (base->k == RV_MAP)                 /* m.key -> map lookup */
			return map_get(base, dup_tok(n->tok));
		long idx = tok_index(n->tok);
		if (idx >= 0) return positional(base, (size_t)idx);
		return file_field(base, n->tok);
	}

	case AST_OBJNEW: {                     /* tag a fields map as an object */
		rval *c = class_ensure(dup_tok(n->tok));
		ast *body = n->kids[0];
		if (body->kind == AST_MAPLIT && objnew_keys_static(body)) {
			/* static schema: (re)capture the key array on the class only
			 * when it doesn't already match this body's field count -- the
			 * common case (one `type` decl per class name) captures once
			 * and every later construction skips straight to evaluating
			 * values. */
			if (c->i != (long)body->nkids) {
				char **schema = a_alloc(body->nkids * sizeof *schema);
				for (size_t i = 0; i < body->nkids; i++) {
					token *kt = body->kids[i]->kids[0]->tok;
					schema[i] = str_unescape(kt->string, kt->len);
				}
				c->nat = schema;
				c->i = (long)body->nkids;
			}
			rval *o = rv(RV_OBJECT);
			o->cls = c;
			o->keys = (char **)c->nat;
			o->n = o->cap = (size_t)c->i;
			o->items = a_alloc((size_t)c->i * sizeof *o->items);
			for (size_t i = 0; i < body->nkids; i++)
				o->items[i] = eval(body->kids[i]->kids[1], e);
			return o;
		}
		rval *fields = eval(body, e);
		return make_object(c, fields, NULL);
	}

	case AST_METHODDEF: {                  /* method Class.name = lambda */
		rval *c = class_ensure(dup_tok(n->kids[0]->tok));
		rval *m = eval(n->kids[1], e);
		class_add_method(c, dup_tok(n->tok), m);
		return &RV_UNIT;
	}

	case AST_METHODCOPY: {                 /* method New.m = Old.m2 / New.* = Old.* */
		char *dst_name = dup_tok(n->kids[0]->tok);
		char *src_name = dup_tok(n->kids[1]->tok);
		rval *dst = class_ensure(dst_name);
		rval *src = class_ensure(src_name);
		if (n->tok) {                       /* single: copy one named method */
			char *dm = dup_tok(n->tok);
			char *sm = dup_tok(n->kids[2]->tok);
			rval *m = class_method(src, sm);
			if (!m)
				return (rval *)fb_error(g_active, "method %s.%s = %s.%s: %s has no method '%s'",
					dst_name, dm, src_name, sm, src_name, sm);
			class_add_method(dst, dm, m);
		} else {                            /* wildcard: copy every method src has now */
			for (size_t i = 0; i < src->n; i++)
				class_add_method(dst, src->keys[i], src->items[i]);
		}
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
		int lnum = l->k == RV_INT || l->k == RV_BOOL || l->k == RV_FLOAT;
		int rnum = r->k == RV_INT || r->k == RV_BOOL || r->k == RV_FLOAT;
		if (lnum && rnum && (l->k == RV_FLOAT || r->k == RV_FLOAT)) {
			double a = as_dbl(l), b = as_dbl(r);
			switch (n->tok->kind) {
			case TOK_LT: res = a <  b; break;
			case TOK_GT: res = a >  b; break;
			case TOK_LE: res = a <= b; break;
			case TOK_GE: res = a >= b; break;
			case TOK_EQ: res = a == b; break;
			default:     res = a != b; break;   /* TOK_NE */
			}
		} else if (lnum && rnum) {
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

	case AST_ARITH: {                    /* + - * / %, Int or Float (unboxed inner) */
		numval r = eval_num(n, e);
		return r.is_float ? rv_float(r.f) : rv_int(r.i);
	}

	case AST_NEG: {                      /* unary "-", Int or Float */
		rval *v = eval(n->kids[0], e);
		return v->k == RV_FLOAT ? rv_float(-v->f) : rv_int(-v->i);
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
		 * `m`, apply the method to (self, ...args) -- unless `m`'s param list
		 * has more than one name (`method Class.m = a, b -> ...`, parens
		 * around it are just grouping, same AST either way), in which case
		 * those names destructure self's fields BY NAME instead: each param
		 * whose name matches one of self's keys gets that field's value.
		 * Otherwise fall through (the field may hold a plain callable). */
		if (callee->kind == AST_FIELD && callee->tok && callee->nkids == 1) {
			rval *self = eval(callee->kids[0], e);
			if (self->k == RV_OBJECT) {
				rval *m = class_method((rval *)self->cls, dup_tok(callee->tok));
				if (m) {
					size_t k; rval **as = eval_args(n, e, &k);
					rval *arg;
					if (m->k == RV_LAMBDA && m->params->nkids > 1) {
						arg = rv(RV_TUPLE);
						for (size_t i = 0; i < m->params->nkids; i++) {
							ast *p = m->params->kids[i];
							rv_push(arg, (p->kind == AST_ID && p->tok)
							             ? map_get(self, dup_tok(p->tok)) : &RV_UNIT);
						}
					} else if (k == 0) {
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
			 * gets evaluated, splat-flattened args. Dispatch through the ABI.
			 * The lookup (a linear registry scan, hits AND misses) is memoized
			 * per call site, invalidated by state switch or registration. */
			const fb_Reg *rg;
			if (n->rg_state == (void *)g_active && n->rg_gen == g_reg_gen) {
				rg = (const fb_Reg *)n->rg_val;
			} else {
				rg = fb_reg_find_tok(callee->tok);
				n->rg_val   = rg;
				n->rg_state = g_active;
				n->rg_gen   = g_reg_gen;
			}
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
		rval *arg = (k == 1) ? as[0] : rv_tuple_n(as, k);
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

	/* publish for bare-identifier lookup (fb_bind_lookup, checked in AST_ID
	 * eval) the same way a plugin exposes a namespace object -- `args`/`cwd`/
	 * `_GLOBALS`/`_SHARED` are core state, not CLI-only, so the bind happens
	 * here rather than in main.c (see fb_open_base for their FB_GLOBAL
	 * registration). store_globals()/store_shared() lazily create their map
	 * on first call, so binding here (before any script code runs) forces
	 * that creation up front; later map_set mutations are in-place so the
	 * bound reference stays live. */
	fb_global_bind(g_active, "args", (fb_Value)g_args);
	fb_global_bind(g_active, "cwd", (fb_Value)g_cwd);
	fb_global_bind(g_active, "_GLOBALS", (fb_Value)store_globals());
	fb_global_bind(g_active, "_SHARED", (fb_Value)store_shared());

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
	g->globals = g_globals;   /* _SHARED (g_shared) is process-global: not saved */
	g->in_run = g_in_run; g->bench = g_bench;
	g->print_q = g_print_q; g->print_n = g_print_n; g->print_cap = g_print_cap;
	memcpy(g->regs, g_regs, sizeof g_regs); g->nreg = g_nreg;
	memcpy(g->binds, g_binds, sizeof g_binds); g->nbinds = g_nbinds;
	g->classes = g_classes; g->spinner = g_spinner;
}
/* load S's snapshot into the live globals */
static void fb_gload(struct fb_State *S) {
	fb_globals *g = &S->snap;
	memcpy(g_defs, g->defs, sizeof g_defs); g_ndefs = g->ndefs;
	g_top = g->top; g_env = g->env; g_args = g->args; g_cwd = g->cwd;
	g_globals = g->globals;
	g_in_run = g->in_run; g_bench = g->bench;
	g_print_q = g->print_q; g_print_n = g->print_n; g_print_cap = g->print_cap;
	memcpy(g_regs, g->regs, sizeof g_regs); g_nreg = g->nreg;
	memcpy(g_binds, g->binds, sizeof g_binds); g_nbinds = g->nbinds;
	g_classes = g->classes; g_spinner = g->spinner;
}
static void fb_activate(struct fb_State *S) {
	if (g_active == S) return;
	if (g_active) fb_gsave(g_active);
	g_active = S;
	fb_gload(S);
	g_main    = S->arena;
	g_scratch = S->scratch;
	arena_use(S->arena);
}

fb_State *fb_new(void) {
	struct fb_State *S = calloc(1, sizeof *S);
	if (!S) { fprintf(stderr, "out of memory\n"); exit(1); }
	S->arena   = arena_new();
	S->scratch = arena_new();
	fb_activate(S);   /* loads S's (zeroed) globals + points the arena at S */
	fb_init();        /* register core builtins into S's registry */
	return S;
}

void fb_close(fb_State *S) {
	fb_ui_cleanup();                       /* release the shared draw buffer */
	arena_free(S->scratch);
	arena_free(S->arena);
	if (g_active == S) g_active = NULL;   /* live globals now dangle until next activate */
	free(S);
}

void fb_set_args(fb_State *S, int argc, char **argv) {
	S->argc = argc;
	S->argv = argv;
}

const char *fb_error_msg(fb_State *S) { return S->err_msg; }

int fb_load_env_plugins(fb_State *S);   /* public: see fbuild.h */

int fb_dostring(fb_State *S, const char *src, const char *chunkname) {
	(void)chunkname;
	fb_activate(S);
	S->err_msg[0] = '\0';
	fb_init();
	if (fb_load_env_plugins(S)) return 1;   /* $FBUILD_PLUGINS, before check */

	/* Boundary installed before lex/parse: die()/_expect() (routed through
	 * fb_error/fb_error_at) unwind here too, not just runtime errors -- a
	 * malformed script no longer kills a long-lived host. */
	if (setjmp(S->err_jmp)) { S->err_set = 0; return 1; }
	S->err_set = 1;

	p = a_alloc(sizeof *p + sizeof(token) * MAX_TOKENS);
	lex(a_dup(src));                 /* lexer wants a writable, arena-owned buf */
	ast *prog = parse_program();
	if (getenv("DUMP_AST")) ast_print(prog, 0);

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

/* Parse + type-check only -- never calls run_program(). See fbuild.h. */
int fb_check(fb_State *S, const char *src, const char *chunkname,
             fb_Diag *diags, int max_diags, ast **out_prog) {
	(void)chunkname;
	fb_activate(S);
	S->err_msg[0] = '\0';
	fb_init();
	if (out_prog) *out_prog = NULL;
	if (fb_load_env_plugins(S)) {
		if (diags && max_diags > 0) {
			diags[0] = (fb_Diag){0};
			diags[0].severity = FB_SEV_ERROR;
			snprintf(diags[0].message, sizeof diags[0].message, "%.*s", (int)sizeof diags[0].message - 1, S->err_msg);
		}
		return 1;
	}

	if (setjmp(S->err_jmp)) {
		S->err_set = 0;
		if (diags && max_diags > 0) {
			diags[0] = (fb_Diag){0};
			diags[0].line = diags[0].end_line = S->err_line;
			diags[0].col  = diags[0].end_col  = S->err_col;
			diags[0].severity = FB_SEV_ERROR;
			snprintf(diags[0].message, sizeof diags[0].message, "%.*s", (int)sizeof diags[0].message - 1, S->err_msg);
		}
		return 1;
	}
	S->err_set = 1;

	p = a_alloc(sizeof *p + sizeof(token) * MAX_TOKENS);
	lex(a_dup(src));                 /* lexer wants a writable, arena-owned buf */
	ast *prog = parse_program();
	if (getenv("DUMP_AST")) ast_print(prog, 0);

	int errs = infer_program_collect(prog, diags, max_diags);
	S->err_set = 0;
	if (out_prog) *out_prog = prog;
	return errs;
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
	if (S->nplugins < FB_MAX_PLUGINS) S->plugins[S->nplugins++] = a_dup(path);
	return 0;
}

int fb_plugin_count(fb_State *S) { return S->nplugins; }
const char *fb_plugin_path(fb_State *S, int i) {
	return (i >= 0 && i < S->nplugins) ? S->plugins[i] : NULL;
}

/* Load every *.so in `dir` (non-recursive, sorted for determinism). Reads the
 * directory with getdents64, matching glob_walk's convention. */
static int fb_load_dir_plugins(fb_State *S, const char *dir) {
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) { snprintf(S->err_msg, sizeof S->err_msg, "FBUILD_PLUGIN_PATH: cannot open '%s'", dir); return 1; }
	char *names[256]; int nn = 0;
	char buf[8192];
	for (;;) {
		long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
		if (n <= 0) break;
		for (long off = 0; off < n; ) {
			struct glob_dirent64 *e = (struct glob_dirent64 *)(buf + off);
			off += e->d_reclen;
			size_t l = strlen(e->d_name);
			if (l > 3 && !strcmp(e->d_name + l - 3, ".so") && nn < 256)
				names[nn++] = a_dup(e->d_name);
		}
	}
	close(fd);
	for (int i = 0; i < nn; i++)
		for (int j = i + 1; j < nn; j++)
			if (strcmp(names[i], names[j]) > 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
	for (int i = 0; i < nn; i++) {
		char path[4096];
		joinp(path, sizeof path, dir, names[i]);
		if (fb_load_plugin(S, path)) return 1;
	}
	return 0;
}

/* Load colon-separated plugin paths from $FBUILD_PLUGINS, then every *.so
 * under colon-separated directories in $FBUILD_PLUGIN_PATH (once, in that order). */
int fb_load_env_plugins(fb_State *S) {
	if (S->env_plugins_done) return 0;
	S->env_plugins_done = 1;
	const char *env = getenv("FBUILD_PLUGINS");
	if (env && *env) {
		char *list = a_dup(env), *save = NULL;
		for (char *t = strtok_r(list, ":", &save); t; t = strtok_r(NULL, ":", &save))
			if (fb_load_plugin(S, t)) return 1;
	}
	const char *dirs = getenv("FBUILD_PLUGIN_PATH");
	if (dirs && *dirs) {
		char *list = a_dup(dirs), *save = NULL;
		for (char *t = strtok_r(list, ":", &save); t; t = strtok_r(NULL, ":", &save))
			if (fb_load_dir_plugins(S, t)) return 1;
	}
	return 0;
}
