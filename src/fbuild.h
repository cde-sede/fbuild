#ifndef FBUILD_H
#define FBUILD_H

/* ============================================================================
 * fbuild extension interface (S0 foundation)
 * ----------------------------------------------------------------------------
 * The stable C surface that host programs (embedding) and plugins (.so) use to
 * extend the language: register new builtins and construct/inspect values. In
 * S0 the interpreter is a single implicit state per process; every entry point
 * still takes an fb_State* so a future multi-state build stays source-compatible.
 *
 * Values (fb_Value) are opaque handles into the interpreter's arena. Do NOT
 * free them: they live until the interpreter tears its arena down. Never store
 * an fb_Value past the call that produced it beyond the current run.
 * ============================================================================ */

#include <stddef.h>
#include "ast.h"   /* ast, fb_Diag/fb_Severity -- needed by fb_check() below */

#define FBUILD_ABI 5              /* bump on any incompatible ABI change */

typedef struct fb_State fb_State; /* opaque interpreter state            */
typedef struct fb_val  *fb_Value; /* opaque value handle (arena-owned)   */

/* value kinds returned by fb_kind() */
typedef enum {
	FB_NIL = 0,
	FB_INT,
	FB_FLOAT,
	FB_STR,
	FB_BOOL,
	FB_FILE,
	FB_LIST,
	FB_TUPLE,
	FB_CMD,
	FB_MAP,
	FB_LAMBDA,
	FB_OBJECT,
	FB_OTHER,
} fb_Kind;

/* declared return type of a registered builtin, for the type checker.
 * FB_RET_ANY = unknown (unifies with everything). */
typedef enum {
	FB_RET_ANY = 0,
	FB_RET_INT,
	FB_RET_FLOAT,
	FB_RET_STR,
	FB_RET_BOOL,
	FB_RET_FILE,
	FB_RET_LIST,
	FB_RET_MAP,
	FB_RET_UNIT,
	FB_RET_LIST_STR,   /* List<String> (FB_RET_LIST is an untyped list) */
	FB_RET_NUM,        /* Int or Float, unresolved which -- for a param_types
	                    * entry (below) as much as `ret` itself */
} fb_Ret;

/* An arbitrary union of >= 2 fb_Ret members for one return/param slot, e.g.
 * { FB_RET_INT, FB_RET_STR } for a language-level `Int|String`. FB_RET_NUM
 * stays the common Int|Float shorthand for a single slot (no allocation
 * needed); reach for fb_RetUnion when the union isn't just Int|Float, or
 * has more than two members. Members are plain fb_Ret (no nested unions,
 * matching the language-level grammar, which flattens A|B|C too). Point a
 * static const array at `members` -- fb_Reg only borrows the pointer. */
typedef struct {
	const fb_Ret *members;
	int           nmembers;
} fb_RetUnion;

/* A builtin: by default receives its already-evaluated arguments (splats
 * flattened). With FB_LAZY the args arrive UNEVALUATED as thunks (splats are
 * NOT flattened, one thunk per source argument) and the builtin decides what to
 * evaluate, and when, by calling fb_force() -- this is how you write control
 * forms (a custom `bench`, lazy defaults, short-circuiting helpers).
 * Return a value made with the fb_* constructors, or fb_error() to abort. */
typedef fb_Value (*fb_CFunction)(fb_State *S, fb_Value *args, int nargs);

/* fb_Reg.flags */
#define FB_LAZY   1u  /* args are thunks; force them with fb_force() */
#define FB_GLOBAL 2u  /* name denotes a bound global value (fb_global_bind),
                       * not a callable -- the checker types a bare reference
                       * to it from `ret` directly instead of RET_VAR. */

/* Registration record. `max < 0` means variadic. `ret` drives type inference.
 * `doc` is optional (NULL is fine, existing designated-initializer callsites
 * that don't set it get NULL for free) -- a one-line human-readable summary
 * shown by tools (e.g. an LSP daemon's hover) instead of falling back to the
 * bare inferred type; has no effect on inference or the runtime itself.
 * `param_types`/`nparams` are likewise optional (NULL/0: every param shows
 * as "?" in a synthesized signature, today's behavior) -- when set, the
 * checker/hover build precise per-argument types instead of generic ones.
 * `nparams` need not match `min`/`max`: shorter than the real arity is fine
 * (only the typed prefix shows precisely, the rest still "?" / a variadic
 * tail), it's just display/checking precision, not a hard contract.
 * `ret_class` is optional (NULL: `ret` drives the return type as usual) --
 * set it to a `fb_class(S, name)` class's own name to say "this returns an
 * instance of that native class" instead, letting the checker/hover treat
 * it like a language-level `type NAME(...)` instance (nameable, and its
 * fb_method()-registered methods become recognized field accesses) rather
 * than a bare `ret`-typed value. Takes priority over `ret` when set.
 * `ret_union`/`param_type_unions` are optional, for a slot that's a real
 * A|B|C union rather than one fb_Ret: `ret_union` (non-NULL) takes priority
 * over both `ret` and `ret_class` for the return type. `param_type_unions`,
 * if set, is a `nparams`-length array parallel to `param_types`; a NULL
 * entry means "use param_types[i]" as usual, a non-NULL entry overrides
 * that slot with the union.
 * A per-argument counterpart to `ret_class` (a param that must be an
 * instance of a native class, e.g. a Pipeline/Buffer/Shader param) is NOT a
 * field here -- see fb_reg_param_class() below instead, a separate call
 * keyed by this registration's own `name` string, so growing this struct
 * (an ABI break needing an FBUILD_ABI bump + every plugin recompiled) isn't
 * needed just to add one more optional per-param annotation. */
typedef struct {
	const char   *name;
	fb_CFunction  fn;
	int           min, max;
	fb_Ret        ret;
	unsigned      flags;   /* 0, or a bitwise-or of FB_* flags */
	const char   *doc;         /* optional: one-line description, or NULL */
	const fb_Ret *param_types; /* optional: per-argument types, or NULL */
	int           nparams;     /* length of param_types; ignored if NULL */
	const char   *ret_class;   /* optional: native class name, or NULL */
	const fb_RetUnion            *ret_union;          /* optional, see above */
	const fb_RetUnion *const     *param_type_unions;  /* optional, see above */
} fb_Reg;

/* Evaluate a thunk argument passed to an FB_LAZY builtin (returns it unchanged
 * if it is already a value). Safe to call more than once (re-evaluates). */
fb_Value fb_force(fb_State *S, fb_Value thunk);

/* ---- lifecycle / embedding -------------------------------------------- */
/* Register the interpreter's own ABI builtins. Called by fb_new(); host code
 * rarely needs it directly. Idempotent. */
void          fb_init(void);

/* Create / destroy an interpreter. Single implicit state (S0): fb_new()
 * initializes the shared arena and returns the process interpreter; fb_close()
 * tears the arena down. One live state at a time. */
fb_State *fb_new(void);
void      fb_close(fb_State *S);

/* Args exposed to the script as the `args` list (argv[i] for i in [1,argc);
 * a `:::` separator, if present, forwards only what follows). */
void      fb_set_args(fb_State *S, int argc, char **argv);

/* Type-check and run a program. Return 0 on success, non-zero on a type or
 * runtime error (message in fb_error_msg). fb_dofile reads `path` first. */
int       fb_dostring(fb_State *S, const char *src, const char *chunkname);
int       fb_dofile  (fb_State *S, const char *path);
const char *fb_error_msg(fb_State *S);

/* Read a top-level def by name after a successful run (nil if absent). */
fb_Value  fb_global(fb_State *S, const char *name);

/* Mutable key/value stores, the supported replacement for host-side static
 * slots (the old l_save/l_restore pattern). `_global` is the active state's
 * store (visible in scripts as `_GLOBALS`); `_shared` is process-global,
 * shared across states and plugins (script name `_SHARED`). get returns nil
 * when the key is absent. Values are stored as handed in; do not pass a value
 * whose lifetime ends before the store's. */
fb_Value  fb_global_get(fb_State *S, const char *key);
void      fb_global_set(fb_State *S, const char *key, fb_Value v);
fb_Value  fb_shared_get(fb_State *S, const char *key);
void      fb_shared_set(fb_State *S, const char *key, fb_Value v);

/* ---- custom objects (classes + methods) ------------------------------- */
/* A class is a nominal tag plus a method table; an object carries its class,
 * a field map (build with fb_set / read with fb_field / fb_key), and optional
 * opaque native data. Methods are called from the language as `obj.name(args)`,
 * dispatched to the registered fb_CFunction with the object as the first (self)
 * argument. The language `type`/`method` forms build on the same machinery. */
typedef struct fb_class *fb_Class;

fb_Class fb_class (fb_State *S, const char *name);   /* find or create a class */
/* `meta` is optional (NULL: bind only, no type/hover info). When given, only
 * min/max/ret/doc/param_types/nparams/ret_class/ret_union/param_type_unions
 * are read (name/fn/flags ignored --
 * name/fn are already the two params right before it; a method's a plain
 * callable, not GLOBAL). To ALSO type a param as an instance of a native
 * class (fb_reg_param_class's job), call it after this fb_method with the
 * shadow-registered name this builds: "ClassName.methodname".
 * If `c`'s own name equals the identifier this class's instance ends up
 * bound to via fb_global_bind (the singleton-namespace pattern, e.g. `math`/
 * `regex` -- see fb_global_bind below), fb_method ALSO registers a shadow
 * "ClassName.method" entry with fb_register so the type checker and hover
 * can find it as `base.method` -- inert at runtime (never dispatched; the
 * real call still goes through the class's own method table), min/max
 * counted the same way script-level `obj.method(args)` counts args (self
 * excluded). If `c`'s name and the eventual global-bind name differ (or
 * there's no global bind at all -- e.g. fbstream.c's per-instance `Stream`
 * class), the shadow entry still gets registered but nothing will ever look
 * it up by that name -- harmless, just inert. */
void     fb_method(fb_State *S, fb_Class c, const char *name, fb_CFunction fn,
                    const fb_Reg *meta);
fb_Value fb_object(fb_State *S, fb_Class c, void *data); /* instance + native data */
void    *fb_object_data(fb_Value obj);               /* the native data (or NULL) */

/* Declare a plain DATA field (an fb_set() field written at construction, not
 * a method) on a native class, e.g. stream()'s "path" or an App's "title" --
 * so the checker recognizes `obj.field` on that class instead of flagging a
 * legitimate field as unknown. Without this, any class with >=1 fb_method()
 * becomes "closed" for the checker (fb_reg_has_class true) and a plain
 * fb_set() field on an instance of it fails to type-check as a missing
 * field -- this is the fix for that gap, register every fb_set() field a
 * constructor writes. `type` is a plain fb_Ret (FB_RET_ANY if you don't want
 * to commit to one); `ret_class`, if non-NULL, types the field as an
 * instance of that native class instead (same convention as fb_Reg.ret_class,
 * and takes priority over `type` when both would apply). Order/timing vs.
 * fb_method doesn't matter. Last declaration for a given (class, name) wins. */
void     fb_class_field(fb_State *S, fb_Class c, const char *name, fb_Ret type,
                         const char *ret_class);

/* Bind `v` to a bare top-level identifier so scripts can reference it by name
 * with no construction step, e.g. a stateless namespace object exposing only
 * methods: fb_global_bind(S, "regex", fb_object(S, RegexCls, NULL)) makes
 * `regex.findall(...)` work directly. Checked by identifier lookup ahead of
 * the builtin registry; a script-level `def` of the same name still shadows
 * it (defs are resolved first). Also register the name with fb_register (any
 * fb_CFunction, e.g. one that fb_errors) so the type checker recognizes it —
 * fb_global_bind alone does not make an identifier known to the checker.
 * Last bind for a given name wins. */
void     fb_global_bind(fb_State *S, const char *name, fb_Value v);

/* ---- plugins ---------------------------------------------------------- */
/* dlopen a plugin shared object and let it register builtins. The plugin must
 * export:
 *     int fbuild_abi(void);            // return FBUILD_ABI
 *     int fbuild_open(fb_State *S);    // call fb_register(...); return 0 on ok
 * Load plugins BEFORE fb_dostring/fb_dofile so the type checker sees the names.
 * Returns 0 on success, non-zero on error (message in fb_error_msg). */
int       fb_load_plugin(fb_State *S, const char *path);

/* Load $FBUILD_PLUGINS (colon-separated .so paths), then every *.so directly
 * inside each colon-separated directory of $FBUILD_PLUGIN_PATH (sorted by name,
 * non-recursive). Idempotent per process. Normally triggered automatically by
 * fb_dostring/fb_dofile before type-checking; call it directly to force env
 * plugins to load without running a script (e.g. to list them). Returns 0 on
 * success, non-zero on error (message in fb_error_msg). */
int       fb_load_env_plugins(fb_State *S);

/* Enumerate plugins loaded into S so far (via fb_load_plugin, directly or via
 * fb_load_env_plugins), in load order. fb_plugin_path returns NULL if i is
 * out of range. */
int         fb_plugin_count(fb_State *S);
const char *fb_plugin_path (fb_State *S, int i);

/* Signatures a plugin's exported entry points must match. */
typedef int (*fb_abi_fn)(void);
typedef int (*fb_open_fn)(fb_State *S);

/* ---- registration ----------------------------------------------------- */
void          fb_register(fb_State *S, const fb_Reg *reg);
const fb_Reg *fb_reg_find(const char *name);              /* NUL-terminated */
const fb_Reg *fb_reg_findn(const char *name, size_t len); /* slice-friendly */
/* Does any registered entry look like "class_name.<anything>" (an
 * fb_method()-shadowed native method, see fb_method's doc)? Used by the
 * checker to tell a genuinely native, closed-method-set class (Stream: a
 * field/method miss is a real error) from an ordinary object of unknown
 * shape (permissive: might have a field/method this build can't see). */
int fb_reg_has_class(const char *class_name);
/* Every "class_name.<method>" entry (fb_method()'s shadow registrations) --
 * writes at most max_out into out, returns the count found (may exceed
 * max_out; a full buffer should be treated as possibly truncated). Used by
 * completion (fb_complete_field_at) to list a native class's methods the
 * same way it already lists a language class's (see collect_symbols'
 * g_methodsyms) -- fb_reg_has_class above only answers yes/no. */
int fb_reg_class_methods(const char *class_name, const fb_Reg **out, int max_out);
/* Enumerate every currently-registered builtin (core + host + plugin) on S,
 * in registration order. Reflects whatever's been fb_register'd so far
 * (e.g. after loading workspace plugins), same as fb_reg_find/fb_reg_findn.
 * fb_reg_at returns NULL if i is out of range. */
int           fb_reg_count(fb_State *S);
const fb_Reg *fb_reg_at   (fb_State *S, int i);

/* Was class_name.field_name declared via fb_class_field()? Fills *out_ret /
 * *out_ret_class (either may be NULL) and returns 1 if so, else returns 0
 * and leaves them untouched. Used by the checker to let a closed native
 * class's legitimate fb_set() fields through record_field() misses. */
int fb_native_field_type(const char *class_name, const char *field_name,
                          fb_Ret *out_ret, const char **out_ret_class);

/* Declare that parameter `index` (0-based, self excluded -- same counting as
 * fb_Reg.min/max and fb_method's own arity) of a builtin/method registered
 * under `reg_name` (its plain fb_register `name`, or fb_method's shadow
 * "ClassName.method" string) must be an instance of native class
 * `class_name`. Checker-only (signature/hover + call-site arg checking);
 * no runtime effect, and no ordering requirement vs. the fb_register/
 * fb_method call it annotates. This is how a param gets the per-argument
 * equivalent of `ret_class` without fb_Reg itself growing a field for it
 * (which would force an FBUILD_ABI bump + recompiling every plugin just to
 * add one more optional per-param annotation -- not worth it for something
 * checker-only). Process-global like `_SHARED` (not per-fb_State): purely
 * checker metadata, never consulted at runtime, so cross-state visibility
 * is harmless. */
void fb_reg_param_class(const char *reg_name, int index, const char *class_name);
/* class_name for reg_name's parameter `index`, or NULL if never declared. */
const char *fb_reg_param_class_at(const char *reg_name, int index);

/* ---- value constructors ----------------------------------------------- */
fb_Value fb_nil (fb_State *S);
fb_Value fb_int (fb_State *S, long v);
fb_Value fb_float(fb_State *S, double v);
fb_Value fb_bool(fb_State *S, int v);
fb_Value fb_str (fb_State *S, const char *s);
fb_Value fb_file(fb_State *S, const char *path);
fb_Value fb_list(fb_State *S);
void     fb_push(fb_State *S, fb_Value list, fb_Value e);
fb_Value fb_tuple(fb_State *S);                              /* an empty tuple */
void     fb_tuple_push(fb_State *S, fb_Value tuple, fb_Value e);
fb_Value fb_map (fb_State *S);
void     fb_set (fb_State *S, fb_Value map, const char *key, fb_Value v);
fb_Value fb_cmd (fb_State *S);                       /* an empty command (argv) */
void     fb_cmd_push(fb_State *S, fb_Value cmd, fb_Value word); /* append a word */

/* ---- value inspection ------------------------------------------------- */
fb_Kind     fb_kind (fb_Value v);
long        fb_toint(fb_Value v);
double      fb_tofloat(fb_Value v);
const char *fb_tostr(fb_State *S, fb_Value v);   /* coerces int/bool/file  */
int         fb_len  (fb_Value v);                /* list/tuple/map size    */
fb_Value    fb_at   (fb_Value v, int i);         /* list/tuple/map value #i */
fb_Value    fb_field(fb_Value v, const char *k); /* map value by key (nil) */
const char *fb_key  (fb_Value map, int i);       /* map key #i (NULL if oob) */

/* ---- calling back into the language ----------------------------------- */
fb_Value fb_apply(fb_State *S, fb_Value fn, fb_Value *args, int nargs);

/* ---- check without executing -------------------------------------------
 * Parse + type-check `src`, but never call run_program() -- no shell command,
 * file write, or other side effect ever happens. For tools (e.g. an LSP
 * daemon) that want live diagnostics on an in-progress edit without running
 * the script.
 *
 * On a syntax error: writes exactly one diagnostic, *out_prog = NULL.
 * On a clean parse: type-checks, writes up to max_diags diagnostics (one per
 * type error), *out_prog = the parsed program (arena-owned by S; valid until
 * S is closed or reused by another fb_dostring/fb_dofile/fb_check call).
 * Returns the total diagnostic count (may exceed max_diags). Loads env
 * plugins first, same as fb_dostring. */
int fb_check(fb_State *S, const char *src, const char *chunkname,
              fb_Diag *diags, int max_diags, ast **out_prog);

/* ---- error signalling ------------------------------------------------- */
/* Aborts the current builtin. Under a protected boundary (embedding) it
 * unwinds to that boundary; otherwise it prints and exits. Never returns
 * (return type is fb_Value only so `return fb_error(...);` type-checks). */
fb_Value fb_error(fb_State *S, const char *fmt, ...);
/* Same as fb_error, but also records a 1-based source position (0,0 = none)
 * that a caller can recover via fb_error_pos() after the call unwinds. */
fb_Value fb_error_at(fb_State *S, int line, int col, const char *fmt, ...);
/* Position recorded by the most recent fb_error_at on S (0,0 if unset/unknown). */
void fb_error_pos(fb_State *S, int *line, int *col);

#endif /* FBUILD_H */
