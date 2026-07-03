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

#define FBUILD_ABI 1              /* bump on any incompatible ABI change */

typedef struct fb_State fb_State; /* opaque interpreter state            */
typedef struct fb_val  *fb_Value; /* opaque value handle (arena-owned)   */

/* value kinds returned by fb_kind() */
typedef enum {
	FB_NIL = 0,
	FB_INT,
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
	FB_RET_STR,
	FB_RET_BOOL,
	FB_RET_FILE,
	FB_RET_LIST,
	FB_RET_MAP,
	FB_RET_UNIT,
} fb_Ret;

/* A builtin: by default receives its already-evaluated arguments (splats
 * flattened). With FB_LAZY the args arrive UNEVALUATED as thunks (splats are
 * NOT flattened, one thunk per source argument) and the builtin decides what to
 * evaluate, and when, by calling fb_force() -- this is how you write control
 * forms (a custom `bench`, lazy defaults, short-circuiting helpers).
 * Return a value made with the fb_* constructors, or fb_error() to abort. */
typedef fb_Value (*fb_CFunction)(fb_State *S, fb_Value *args, int nargs);

/* fb_Reg.flags */
#define FB_LAZY 1u    /* args are thunks; force them with fb_force() */

/* Registration record. `max < 0` means variadic. `ret` drives type inference. */
typedef struct {
	const char  *name;
	fb_CFunction fn;
	int          min, max;
	fb_Ret       ret;
	unsigned     flags;   /* 0, or a bitwise-or of FB_* flags */
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

/* ---- custom objects (classes + methods) ------------------------------- */
/* A class is a nominal tag plus a method table; an object carries its class,
 * a field map (build with fb_set / read with fb_field / fb_key), and optional
 * opaque native data. Methods are called from the language as `obj.name(args)`,
 * dispatched to the registered fb_CFunction with the object as the first (self)
 * argument. The language `type`/`method` forms build on the same machinery. */
typedef struct fb_class *fb_Class;

fb_Class fb_class (fb_State *S, const char *name);   /* find or create a class */
void     fb_method(fb_State *S, fb_Class c, const char *name, fb_CFunction fn);
fb_Value fb_object(fb_State *S, fb_Class c, void *data); /* instance + native data */
void    *fb_object_data(fb_Value obj);               /* the native data (or NULL) */

/* ---- plugins ---------------------------------------------------------- */
/* dlopen a plugin shared object and let it register builtins. The plugin must
 * export:
 *     int fbuild_abi(void);            // return FBUILD_ABI
 *     int fbuild_open(fb_State *S);    // call fb_register(...); return 0 on ok
 * Load plugins BEFORE fb_dostring/fb_dofile so the type checker sees the names.
 * Returns 0 on success, non-zero on error (message in fb_error_msg). */
int       fb_load_plugin(fb_State *S, const char *path);

/* Signatures a plugin's exported entry points must match. */
typedef int (*fb_abi_fn)(void);
typedef int (*fb_open_fn)(fb_State *S);

/* ---- registration ----------------------------------------------------- */
void          fb_register(fb_State *S, const fb_Reg *reg);
const fb_Reg *fb_reg_find(const char *name);              /* NUL-terminated */
const fb_Reg *fb_reg_findn(const char *name, size_t len); /* slice-friendly */

/* ---- value constructors ----------------------------------------------- */
fb_Value fb_nil (fb_State *S);
fb_Value fb_int (fb_State *S, long v);
fb_Value fb_bool(fb_State *S, int v);
fb_Value fb_str (fb_State *S, const char *s);
fb_Value fb_file(fb_State *S, const char *path);
fb_Value fb_list(fb_State *S);
void     fb_push(fb_State *S, fb_Value list, fb_Value e);
fb_Value fb_map (fb_State *S);
void     fb_set (fb_State *S, fb_Value map, const char *key, fb_Value v);
fb_Value fb_cmd (fb_State *S);                       /* an empty command (argv) */
void     fb_cmd_push(fb_State *S, fb_Value cmd, fb_Value word); /* append a word */

/* ---- value inspection ------------------------------------------------- */
fb_Kind     fb_kind (fb_Value v);
long        fb_toint(fb_Value v);
const char *fb_tostr(fb_State *S, fb_Value v);   /* coerces int/bool/file  */
int         fb_len  (fb_Value v);                /* list/tuple/map size    */
fb_Value    fb_at   (fb_Value v, int i);         /* list/tuple/map value #i */
fb_Value    fb_field(fb_Value v, const char *k); /* map value by key (nil) */
const char *fb_key  (fb_Value map, int i);       /* map key #i (NULL if oob) */

/* ---- calling back into the language ----------------------------------- */
fb_Value fb_apply(fb_State *S, fb_Value fn, fb_Value *args, int nargs);

/* ---- error signalling ------------------------------------------------- */
/* Aborts the current builtin. Under a protected boundary (embedding) it
 * unwinds to that boundary; otherwise it prints and exits. Never returns
 * (return type is fb_Value only so `return fb_error(...);` type-checks). */
fb_Value fb_error(fb_State *S, const char *fmt, ...);

#endif /* FBUILD_H */
