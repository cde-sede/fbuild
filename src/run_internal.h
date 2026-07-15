/* Shared internals for the executor, split across translation units:
 *   run.c          - core: eval, apply, environment, execution, fb_* API
 *   run_builtins.c - the builtin library (do_*), their wrappers, registration
 *   build_impl.c   - BUILD_IMPLEMENTATION for build.h (compiled once, in parallel)
 *
 * This header carries the runtime types, the tiny value constructors, and the
 * cross-TU declarations so run.c and run_builtins.c agree on the ABI. It is a
 * header-only consumer of build.h: whichever includer wants the metadata block
 * defines METADATA_LIST first (run.c does); build_impl.c defines
 * BUILD_IMPLEMENTATION and does NOT include this header. */

#ifndef FBUILD_RUN_INTERNAL_H
#define FBUILD_RUN_INTERNAL_H

#define _GNU_SOURCE

/* cds-lib runtime preamble (must match build_impl.c so build.h sees the same
 * generic-DA config). */
#define BUILD_DA_GENERIC_EXTRA(func)   \
    DA_DECL_GENERIC(char, func)        \
    DA_DECL_GENERIC(voidptr, func)     \
    DA_DECL_GENERIC(int, func)
#define BUILD_TAIL_LINES 5
#define CORO_STACK_SIZE (512 * 1024)
#include "cds_lib/build.h"

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <string.h>

typedef void* voidptr;
typedef DECL_DA(voidptr) t_da_voidptr;
DECL_DA_FUNCTIONS(voidptr, 128, realloc, free);

typedef DECL_DA(char) t_string;
DECL_DA_FUNCTIONS(char, 1024, realloc, free);

typedef DECL_DA(int) t_da_int;
DECL_DA_FUNCTIONS(int, 256, realloc, free);

#include "connex.h"

/* build__buf is defined inside build.h's implementation block (build_impl.c);
 * mirror its ABI here so capture_cmd/pkgconf_run can read a group's per-command
 * output buffer. build_ui_cleanup() is likewise impl-only -> use fb_ui_cleanup. */
typedef struct { char *items; size_t count; size_t capacity; } build__buf;
extern void fb_ui_cleanup(void);

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
	RV_NONE, RV_INT, RV_FLOAT, RV_STR, RV_BOOL,
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

/* The kind-specific payloads overlap (anonymous union): sequence-likes
 * (LIST/TUPLE/CMD/MAP/OBJECT) never carry lambda fields and vice versa, so
 * the struct shrinks 120 -> 80 bytes -- every rval is arena-allocated and
 * zeroed, so this directly cuts the allocator + memset hot path. `i` and `s`
 * stay standalone: RV_STR uses both (json numeric marker). All access sites
 * are kind-guarded (is_seq / switch on ->k) before touching union members. */
struct rval {
	rkind   k;
	long    i;          /* int / bool; RV_STR: json numeric marker */
	double  f;          /* float                                    */
	char   *s;          /* string / file path / builtin name       */
	union {
		struct {
			rval  **items;      /* list / tuple / cmd args; map values */
			char  **keys;       /* map keys, parallel to items         */
			size_t  n;
			size_t  cap;        /* allocated slots in items            */
		};
		struct {
			ast    *params;     /* lambda                              */
			ast    *body;       /* lambda / thunk / deferred seq       */
			renv   *env;        /* lambda closure                      */
		};
		struct { char *from, *to; };  /* morphism patterns             */
	};
	char   *label;      /* def name, for group display         */
	void   *cls;        /* RV_OBJECT: its class (struct fb_class*) */
	void   *nat;        /* RV_OBJECT: native data; RV_CFUN: fb_CFunction */
};

struct rbind { token *name; rval *val; struct rbind *next; };
/* Two inline bind slots cover the common lambda frame (<= 2 params) without a
 * separate rbind allocation each; extra binds fall back to the arena. renv is
 * always zero-initialized (arena or static), so nslots starts at 0. */
struct renv  { struct rbind *head; rval *arg; renv *parent;
               struct rbind slots[2]; int nslots; };

/* tagged-object class (RV_OBJECT). Shared so repr() (run_builtins.c) can name
 * an object; the class table itself is managed in run.c. */
typedef struct fb_class {
	char *name;
	struct { char *name; rval *val; } *methods;
	size_t nmethods, cap;
	struct fb_class *next;
} fb_class_t;

/* the shared unit value; single definition lives in run.c */
extern rval RV_UNIT;

/* All runtime allocations come from the shared program arena (a_alloc / a_grow
 * / a_dup, declared in cparser.h, created by parser.c). Nothing is freed per
 * object. The tiny constructors are inline so both TUs share them. */
static inline rval *rv(rkind k) {
	rval *v = a_alloc(sizeof *v);
	v->k = k;
	return v;
}
static inline rval *rv_int(long i)  { rval *v = rv(RV_INT);  v->i = i; return v; }
static inline rval *rv_float(double d) { rval *v = rv(RV_FLOAT); v->f = d; return v; }
static inline rval *rv_bool(int b)  { rval *v = rv(RV_BOOL); v->i = !!b; return v; }
static inline rval *rv_str(char *s) { rval *v = rv(RV_STR);  v->s = s; return v; }
static inline rval *rv_file(char *s){ rval *v = rv(RV_FILE); v->s = s; return v; }

static inline void rv_push(rval *list, rval *e) {
	if (list->n == list->cap) {
		size_t nc = list->cap ? list->cap * 2 : 4;
		list->items = a_grow(list->items, list->n * sizeof *list->items,
		                     nc * sizeof *list->items);
		list->cap = nc;
	}
	list->items[list->n++] = e;
}

/* ---- cross-TU globals (defined in run.c) ------------------------------ */
extern t_env  g_env;
extern int    g_bench;
extern rval  *g_spinner;
extern rval  *g_args;
extern rval  *g_cwd;
extern rval  *g_globals;
extern rval  *g_shared;
extern void  *g_scratch;     /* per-iteration scratch arena (two-arena model) */
extern struct fb_State *g_active;

/* ---- core helpers used by run_builtins.c (defined in run.c) ----------- */
char       *subst_a(const char *from, const char *to, const char *s);
void        map_set(rval *m, char *key, rval *val);
rval       *map_get(rval *m, const char *key);
char       *as_cstr(rval *v);
char       *fmt_double(double d);
const char *pstr(rval *v);
int         truthy(rval *v);
int         is_seq(rval *v);
rval       *eval(ast *n, renv *e);
void        print_emit(char *line);
rval       *store_globals(void);
rval       *store_shared(void);
rval       *file_field_s(rval *f, const char *name);
rval       *file_field(rval *f, token *name);
char       *path_dir(const char *p);
char       *path_base(const char *p);
char       *path_stem(const char *p);
char       *path_suffix(const char *p);
void        joinp(char *buf, size_t sz, const char *rel, const char *name);
rval       *do_glob(const char *pat);
rval       *do_zip(rval **args, size_t k);
rval       *do_stale(rval **args, size_t n);
rval       *apply(rval *fn, rval *arg);
rval       *apply_scratch(rval *fn, rval *arg);
void        collect(rval *v, t_cmd *cmds);
int         has_cmd(rval *v);
void        run_tree(rval *root);
const fb_Reg *fb_reg_find(const char *name);
rval       *fb_dispatch(const fb_Reg *rg, rval **as, size_t k);

/* ---- builtins used by run.c (defined in run_builtins.c) --------------- */
rval *do_parse(rval *schema, rval *argv_list);
int   is_builtin(const char *s);
rval *call_builtin1(const char *n, rval *a);
void  fb_open_base(fb_State *S);

#endif /* FBUILD_RUN_INTERNAL_H */
