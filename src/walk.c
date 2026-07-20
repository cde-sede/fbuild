#include "ast.h"
#include "fbuild.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>


#define UNREACHABLE(message, ...) do {                                                             \
		fprintf(stderr, "%s:%d: UNREACHABLE: " message "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
		abort();                                                                                   \
	} while(0)

char *AST_NODE_NAMES[] = {

	[AST_PROGRAM] = "AST_PROGRAM",
	[AST_ASSIGN] = "AST_ASSIGN",
	[AST_EAGER] = "AST_EAGER",
	[AST_FORCE] = "AST_FORCE",
	[AST_TUPLE] = "AST_TUPLE",
	[AST_LISTLIT] = "AST_LISTLIT",
	[AST_MAPLIT] = "AST_MAPLIT",
	[AST_PARSER] = "AST_PARSER",
	[AST_LAMBDA] = "AST_LAMBDA",
	[AST_PARAMS] = "AST_PARAMS",
	[AST_SEQ] = "AST_SEQ",
	[AST_PIPE] = "AST_PIPE",
	[AST_GUARD] = "AST_GUARD",
	[AST_WHERE] = "AST_WHERE",
	[AST_BINDING] = "AST_BINDING",
	[AST_WHEN] = "AST_WHEN",
	[AST_COMPARE] = "AST_COMPARE",
	[AST_ARITH] = "AST_ARITH",
	[AST_NEG] = "AST_NEG",
	[AST_LOGICAL] = "AST_LOGICAL",
	[AST_NOT] = "AST_NOT",
	[AST_MORPHISM] = "AST_MORPHISM",

	[AST_OBJNEW] = "AST_OBJNEW",
	[AST_METHODDEF] = "AST_METHODDEF",
	[AST_METHODCOPY] = "AST_METHODCOPY",
	[AST_CALL] = "AST_CALL",
	[AST_FIELD] = "AST_FIELD",
	[AST_CONSTRUCT] = "AST_CONSTRUCT",
	[AST_CFIELD] = "AST_CFIELD",

	[AST_SPLAT] = "AST_SPLAT",
	[AST_METADATA] = "AST_METADATA",
	[AST_CAPTURE] = "AST_CAPTURE",
	[AST_PROJECTION] = "AST_PROJECTION",
	[AST_COMMAND] = "AST_COMMAND",
	[AST_CMDTEXT] = "AST_CMDTEXT",

	[AST_ID] = "AST_ID",
	[AST_INT] = "AST_INT",
	[AST_FLOAT] = "AST_FLOAT",
	[AST_BOOL] = "AST_BOOL",
	[AST_NIL] = "AST_NIL",
	[AST_STRING] = "AST_STRING",
	[AST_STRTEXT] = "AST_STRTEXT",
	[AST_PATH] = "AST_PATH",

};


static int tok_is(token *t, const char *s);
static int tok_eq(token *a, token *b);

/* ---- type constructors ------------------------------------------------ */

static obj_type *t_new(ret_type r) {
	obj_type *t = a_alloc(sizeof *t);
	t->ret = r;
	return t;
}

/* take ownership of a children array */
static obj_type *t_compound(ret_type r, size_t n, obj_type **kids) {
	obj_type *t = t_new(r);
	t->children  = kids;
	t->nchildren = n;
	return t;
}

static obj_type *t_list(obj_type *elem) {
	obj_type **k = a_alloc(sizeof *k);
	k[0] = elem;
	return t_compound(RET_LIST, 1, k);
}

static obj_type *t_functor(obj_type *param, obj_type *ret) {
	obj_type **k = a_alloc(2 * sizeof *k);
	k[0] = param;
	k[1] = ret;
	return t_compound(RET_FUNCTOR, 2, k);
}

/* append a named field to a record type (RET_FILE) */
static void rec_push(obj_type *rec, token *name, obj_type *ft) {
	size_t n = rec->nchildren + 1;
	rec->children = a_grow(rec->children, rec->nchildren * sizeof *rec->children,
	                       n * sizeof *rec->children);
	rec->fields   = a_grow(rec->fields,   rec->nchildren * sizeof *rec->fields,
	                       n * sizeof *rec->fields);
	rec->children[rec->nchildren] = ft;
	rec->fields[rec->nchildren]   = name;
	rec->nchildren = n;
}

/* A File and a Path are the same thing: a record with these standard
 * fields, auto-filled from the path. name/stem/suffix/parent are strings;
 * mtime/size come from stat(). */
#define FTOK(s) { .string = (char *)s, .len = sizeof(s) - 1 }
static token FF_name   = FTOK("name");
static token FF_stem   = FTOK("stem");
static token FF_suffix = FTOK("suffix");
static token FF_parent = FTOK("parent");
static token FF_mtime  = FTOK("mtime");
static token FF_size   = FTOK("size");
#undef FTOK

static obj_type *t_file(void) {
	obj_type *f = t_new(RET_FILE);
	rec_push(f, &FF_name,   t_new(RET_STRING));
	rec_push(f, &FF_stem,   t_new(RET_STRING));
	rec_push(f, &FF_suffix, t_new(RET_STRING));
	rec_push(f, &FF_parent, t_new(RET_STRING));
	rec_push(f, &FF_mtime,  t_new(RET_INT));
	rec_push(f, &FF_size,   t_new(RET_INT));
	return f;
}

/* Parser<schema>(...) result, when `schema` is a literal map (the common
 * case): a record with one field per schema entry, typed from its
 * "bool"/"int"/"str" spec (the "type"-only bareword form, or the explicit
 * { "type": ..., ... } form -- see SKILLS.md "cli args"), plus "rest"
 * (leftover positionals, always present). NULL if schema isn't a literal
 * (e.g. `Parser<some_var>`) -- stays RET_VAR/"?" in that case, same as
 * before this existed; there's nothing to statically read a shape from. */
static token PS_rest = { .string = (char *)"rest",   .len = 4 };
static token PS_name = { .string = (char *)"Parser", .len = 6 };

static obj_type *infer_parser_schema(ast *schema) {
	if (schema->kind != AST_MAPLIT)
		return NULL;
	obj_type *rec = t_new(RET_OBJECT);
	rec->class_name = &PS_name;
	for (size_t i = 0; i < schema->nkids; i++) {
		ast *entry = schema->kids[i];       /* AST_TUPLE(key, value) */
		if (entry->kind != AST_TUPLE || entry->nkids != 2)
			continue;
		ast *key = entry->kids[0], *val = entry->kids[1];
		if (key->kind != AST_STRING || !key->tok)
			continue;
		token *type_tok = NULL;
		if (val->kind == AST_STRING && val->tok) {
			type_tok = val->tok;                     /* "force": "bool" */
		} else if (val->kind == AST_MAPLIT) {         /* "jobs": { "type": "int", ... } */
			for (size_t j = 0; j < val->nkids; j++) {
				ast *e2 = val->kids[j];
				if (e2->kind == AST_TUPLE && e2->nkids == 2
				 && e2->kids[0]->kind == AST_STRING && e2->kids[0]->tok
				 && tok_is(e2->kids[0]->tok, "type")
				 && e2->kids[1]->kind == AST_STRING && e2->kids[1]->tok)
					type_tok = e2->kids[1]->tok;
			}
		}
		ret_type rt = RET_STRING;   /* default, matches "str" and anything unrecognized */
		if (type_tok) {
			if (tok_is(type_tok, "bool")) rt = RET_BOOL;
			else if (tok_is(type_tok, "int")) rt = RET_INT;
		}
		rec_push(rec, key->tok, t_new(rt));
	}
	rec_push(rec, &PS_rest, t_list(t_new(RET_STRING)));
	return rec;
}

/* map a registered builtin's declared return type (fbuild.h) to a checker type.
 * Map/Json and ANY are RET_VAR (unknown): field access stays permissive. */
static obj_type *fb_ret_type(fb_Ret r) {
	switch (r) {
	case FB_RET_INT:  return t_new(RET_INT);
	case FB_RET_FLOAT: return t_new(RET_FLOAT);
	case FB_RET_STR:  return t_new(RET_STRING);
	case FB_RET_BOOL: return t_new(RET_BOOL);
	case FB_RET_FILE: return t_file();
	case FB_RET_LIST: return t_list(t_new(RET_VAR));
	case FB_RET_LIST_STR: return t_list(t_new(RET_STRING));
	case FB_RET_UNIT: return t_new(RET_NONE);
	case FB_RET_MAP:
	case FB_RET_ANY:
	default:          return t_new(RET_VAR);
	}
}

/* ---- error reporting + unification ------------------------------------ */

static int g_type_errors;

/* Optional sink: when set, type_err() appends structured diagnostics here
 * instead of printing to stderr. Set/cleared around a single
 * infer_program_collect() call; NULL means the legacy stderr behavior. */
static fb_Diag *g_diag_sink;
static int      g_diag_cap, g_diag_n;

static void type_err(token *where, const char *fmt, ...) {
	g_type_errors++;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	if (g_diag_sink) {
		if (g_diag_n < g_diag_cap) {
			fb_Diag *d = &g_diag_sink[g_diag_n];
			d->line = where ? where->start.line : 0;
			d->col  = where ? where->start.col  : 0;
			d->end_line = where ? where->end.line : d->line;
			d->end_col  = where ? where->end.col  : d->col;
			d->severity = FB_SEV_ERROR;
			snprintf(d->message, sizeof d->message, "%s", buf);
		}
		g_diag_n++;
		return;
	}

	fprintf(stderr, "type error");
	if (where)
		fprintf(stderr, " at %d:%d", where->start.line, where->start.col);
	fprintf(stderr, ": %s\n", buf);
}

/* structural compatibility; RET_VAR (unknown) unifies with anything.
 * RET_OBJECT does too -- field/method shapes aren't tracked (see ast.h), so
 * treating it as strictly-typed here would false-positive on every object
 * passed where some other type was expected. */
static int unify(obj_type *a, obj_type *b) {
	if (!a || !b)
		return 1;
	if (a->ret == RET_VAR || b->ret == RET_VAR)
		return 1;
	if (a->ret == RET_OBJECT || b->ret == RET_OBJECT)
		return 1;
	if ((a->ret == RET_INT || a->ret == RET_FLOAT) &&
	    (b->ret == RET_INT || b->ret == RET_FLOAT))
		return 1;                    /* Int/Float are mutually numeric */
	if (a->ret != b->ret)
		return 0;
	if (a->nchildren != b->nchildren)
		return 0;
	for (size_t i = 0; i < a->nchildren; i++)
		if (!unify(a->children[i], b->children[i]))
			return 0;
	return 1;
}

/* ---- helpers ---------------------------------------------------------- */

static int tok_is(token *t, const char *s) {
	size_t n = strlen(s);
	return t && t->len == n && !memcmp(t->string, s, n);
}

static int tok_eq(token *a, token *b) {
	return a->len == b->len && !memcmp(a->string, b->string, a->len);
}

static obj_type *elem_of(obj_type *t) {
	if (t && t->ret == RET_LIST && t->nchildren)
		return t->children[0];
	return t_new(RET_VAR);   /* tuples are heterogeneous: element type is ? */
}

/* lists and tuples are both ordered sequences; builtins accept either (a ?
 * passes too, since it may resolve to a sequence). */
static int is_seq_t(obj_type *t) {
	return !t || t->ret == RET_LIST || t->ret == RET_TUPLE || t->ret == RET_VAR;
}

/* token as a non-negative index (".0", ".1"), or -1 */
static long tok_index(token *t) {
	if (!t || !t->len)
		return -1;
	long v = 0;
	for (size_t i = 0; i < t->len; i++) {
		if (t->string[i] < '0' || t->string[i] > '9')
			return -1;
		v = v * 10 + (t->string[i] - '0');
	}
	return v;
}

/* positional element of a destructured argument */
static obj_type *positional(obj_type *t, size_t i) {
	if (t && t->ret == RET_TUPLE && i < t->nchildren)
		return t->children[i];
	if (t && t->ret == RET_LIST)
		return elem_of(t);
	return t_new(RET_VAR);
}

/* bounds-safe arg_lams[i] lookup, mirroring positional()'s own bounds check
 * (arg_lams, when non-NULL, is exactly as long as arg->nchildren -- see
 * specialize_call) */
static ast *positional_lam(ast **arg_lams, obj_type *arg, size_t i) {
	if (arg_lams && arg && arg->ret == RET_TUPLE && i < arg->nchildren)
		return arg_lams[i];
	return NULL;
}

/* named field of a record (RET_FILE, or a RET_OBJECT with fields tracked --
 * e.g. a Parser<schema> result, see infer_parser_schema), or NULL if absent */
static obj_type *record_field(obj_type *rec, token *name) {
	if (rec && (rec->ret == RET_FILE || rec->ret == RET_OBJECT) && rec->fields)
		for (size_t i = 0; i < rec->nchildren; i++)
			if (rec->fields[i] && tok_eq(rec->fields[i], name))
				return rec->children[i];
	return NULL;
}

/* define a record field: a new name is added, an existing one is locked to
 * its type (the new value must unify with it). `where` locates the error. */
static int unify(obj_type *a, obj_type *b);
static obj_type *record_field(obj_type *rec, token *name);
static void type_err(token *where, const char *fmt, ...);
static const char *ret_kind_name(ret_type r);

static void rec_define(obj_type *rec, token *name, obj_type *ft, token *where) {
	obj_type *cur = record_field(rec, name);
	if (cur) {
		if (!unify(cur, ft))
			type_err(where, "field '%.*s' is %s, cannot set to %s",
			         (int)name->len, name->string,
			         ret_kind_name(cur->ret), ret_kind_name(ft->ret));
	} else {
		rec_push(rec, name, ft);
	}
}

static const char *ret_kind_name(ret_type r) {
	switch (r) {
	case RET_NONE:    return "None";
	case RET_INT:     return "Int";
	case RET_FLOAT:   return "Float";
	case RET_STRING:  return "String";
	case RET_BOOL:    return "Bool";
	case RET_FILE:    return "File";
	case RET_COMMAND: return "Command";
	case RET_LIST:    return "List";
	case RET_TUPLE:   return "Tuple";
	case RET_FUNCTOR: return "Functor";
	case RET_OBJECT:  return "Object";   /* no class_name here -- see obj_type_print for the named form */
	case RET_VAR:     return "?";
	}
	return "?";
}

/* ---- type scope (lambda params + implicit argument) ------------------- */

/* `lam` (only ever set by specialize_call, hover-only): when this binding's
 * value is a direct reference to a named top-level lambda (e.g. param `f`
 * bound to `compile` at a call site `create_cmd(compile, ...)`), the def's
 * own AST_LAMBDA node -- so a later call *through* this binding (`f(...)`
 * inside create_cmd's body) can specialize `compile` itself against that
 * inner call's real arguments, instead of showing compile's generic
 * signature. NULL for an ordinary binding with no such provenance. */
typedef struct tbind { token *name; obj_type *type; ast *lam; struct tbind *next; } tbind;

typedef struct scope {
	tbind        *binds;
	obj_type     *arg;        /* type of the implicit lambda argument */
	struct scope *parent;
} scope;

static obj_type *scope_get(scope *s, token *name) {
	for (; s; s = s->parent)
		for (tbind *b = s->binds; b; b = b->next)
			if (tok_eq(b->name, name))
				return b->type;
	return NULL;
}

static void scope_put_lam(scope *s, token *name, obj_type *t, ast *lam) {
	tbind *b = a_alloc(sizeof *b);
	b->name = name;
	b->type = t;
	b->lam  = lam;
	b->next = s->binds;
	s->binds = b;
}

static void scope_put(scope *s, token *name, obj_type *t) {
	scope_put_lam(s, name, t, NULL);
}

/* ---- inference -------------------------------------------------------- */

static ast      *g_program;   /* top-level scope for resolving identifiers */
static obj_type *infer(ast *node, scope *sc);

/* Hover-only: when set, infer_call() re-specializes a call to a named
 * top-level lambda against this call site's actual argument types instead
 * of using the def's own cached, context-free (unknown-param) type. Applies
 * at every depth of the re-inference it's set around (nested calls inside a
 * specialized lambda's body specialize too), so it must never be set during
 * the normal infer_program() pass -- only fb_hover_at() toggles it, briefly,
 * around one read-only re-inference. See find_toplevel_lambda/infer_apply. */
static int g_specialize_calls;
static ast *find_toplevel_lambda(ast *prog, token *name);
static tbind *scope_find(scope *s, token *name);
static obj_type *infer_apply(ast *fn, obj_type *arg, scope *sc);
static obj_type *infer_apply_lam(ast *fn, obj_type *arg, ast **arg_lams, scope *sc);
static obj_type *specialize_call(ast *node, size_t na, obj_type *a0, obj_type *a1,
                                  scope *sc, ast *lam, ast *callee);
static obj_type *tuple_of_n_var(size_t n);
static obj_type *t_variadic(obj_type *elem);

static int is_builtin_name(token *t) {
	static const char *names[] = {
		"glob", "zip", "subst", "stale", "run",
		"env", "shell", "which", "exists", "mkdir", "read", "write",
		"move", "copy", "symlink", "remove", "touch", "is_dir", "is_file",
		"args", "contains",
		"filter", "flatten", "concat", "fold", "length", "first", "last",
		"take", "drop", "print", "debug",
		"sort", "unique", "join", "split", "replace",
		"cat", "upper", "lower", "trim", "starts_with", "ends_with",
		"capitalize", "title", "lstrip", "rstrip", "strip_prefix", "strip_suffix",
		"pad_left", "pad_right", "center",
		"parent", "name", "stem", "suffix", "with_suffix", "with_name",
		"path", "abs",
		"json_parse", "json_read", "json_str", "json_pretty", "json_write",
		"get", "has", "keys", "values", "items",
		"range", "enumerate", "slice", "index", "reverse", "sum", "min", "max",
		"count", "find", "to_int", "to_str", "to_float", "repeat", "chars",
		"clock", "time", "sleep", "bench", "config",
		"flag", "opt", "positionals",
		NULL,
	};
	for (size_t i = 0; names[i]; i++)
		if (tok_is(t, names[i])) return 1;
	return fb_reg_findn(t->string, t->len) != NULL;   /* registered builtin */
}

/* resolve an identifier not bound in any lambda/where scope: a top-level def,
 * a known builtin, or an error. */
static obj_type *infer_id(token *name) {
	if (g_program) {
		for (size_t i = 0; i < g_program->nkids; i++) {
			ast *s = g_program->kids[i];
			if ((s->kind == AST_ASSIGN || s->kind == AST_EAGER)
			    && tok_eq(s->tok, name))
				return infer(s->kids[0], NULL);   /* defs are context-free */
		}
	}
	{
		const fb_Reg *rg = fb_reg_findn(name->string, name->len);
		if (rg && (rg->flags & FB_GLOBAL))   /* bound global value: type from `ret` */
			return fb_ret_type(rg->ret);
	}
	if (is_builtin_name(name))
		return t_new(RET_VAR);
	type_err(name, "unknown symbol '%.*s'",
	         (int)name->len, name->string);
	return t_new(RET_VAR);
}

/* core builtins' {min, max} arity (-1 max = variadic). File-scope (not just
 * inside infer_call_impl) so infer_call's wrapper can also read it, to give
 * a callee like "zip"/"print" a real functor shape for hover instead of a
 * bare "?" -- these are dispatched by this name-match chain with early
 * returns, never reaching the "user function value" callee->type = ft
 * annotation used by everything else. */
static const struct { const char *name; int min, max; } g_core_arity[] = {
	{"glob",1,1}, {"subst",3,3}, {"stale",1,-1}, {"zip",2,-1},
	{"env",1,1}, {"shell",1,1}, {"which",1,1}, {"exists",1,1},
	{"mkdir",1,1}, {"read",1,1}, {"write",2,2},
	{"move",2,2}, {"copy",2,2}, {"symlink",2,2}, {"remove",1,1},
	{"touch",1,1}, {"is_dir",1,1}, {"is_file",1,1},
	{"filter",2,2}, {"contains",2,2}, {"flatten",1,1}, {"concat",0,-1},
	{"fold",3,3}, {"length",1,1}, {"first",1,1}, {"last",1,1},
	{"take",2,2}, {"drop",2,2}, {"sort",1,1}, {"unique",1,1},
	{"join",2,2}, {"split",2,2}, {"replace",3,3},
	{"cat",0,-1}, {"upper",1,1}, {"lower",1,1}, {"trim",1,1},
	{"starts_with",2,2}, {"ends_with",2,2},
	{"capitalize",1,1}, {"title",1,1}, {"lstrip",1,1}, {"rstrip",1,1},
	{"strip_prefix",2,2}, {"strip_suffix",2,2},
	{"pad_left",2,3}, {"pad_right",2,3}, {"center",2,3},
	{"parent",1,1}, {"name",1,1}, {"stem",1,1}, {"suffix",1,1},
	{"with_suffix",2,2}, {"with_name",2,2}, {"path",0,-1}, {"abs",1,1},
	{"run",1,-1}, {"print",0,-1}, {"debug",0,-1},
	{"json_parse",1,1}, {"json_read",1,1}, {"json_str",1,1},
	{"json_pretty",1,1}, {"json_write",2,2},
	{"get",2,2}, {"has",2,2},
	{"keys",1,1}, {"values",1,1}, {"items",1,1},
	{"range",1,2}, {"enumerate",1,1}, {"slice",3,3}, {"index",2,3},
	{"reverse",1,1}, {"sum",1,-1}, {"min",1,-1}, {"max",1,-1},
	{"count",2,2}, {"find",2,3}, {"to_int",1,1}, {"to_str",1,1}, {"to_float",1,1},
	{"repeat",2,2}, {"chars",1,1},
	{"clock",0,0}, {"time",0,0}, {"sleep",1,1}, {"bench",2,2},
	{"config",1,1}, {"flag",1,-1}, {"opt",2,2}, {"positionals",0,0},
};
#define N_CORE_ARITY (sizeof g_core_arity / sizeof g_core_arity[0])

static obj_type *tvar(const char *label) {
	obj_type *t = t_new(RET_VAR);
	t->var_label = label;
	return t;
}

/* Hand-written structural signatures (with shared, cosmetically-labeled type
 * variables -- see obj_type.var_label) for core builtins whose SKILLS.md
 * documented type is genuinely generic/higher-order, e.g.
 *   filter : List<a> -> (a -> Bool) -> List<a>
 * Consulted only as a fallback for a builtin callee's hover with no call
 * site to specialize from (infer_call's wrapper, below); a real call always
 * overrides this with the call's own actual types via the normal dispatch.
 * NULL for anything not curated here -- falls back further to the plain
 * arity-shaped ("?, ? -> T") signature built from g_core_arity. */
static obj_type *core_generic_signature(token *name) {
	if (tok_is(name, "zip")) {
		obj_type *a = tvar("a"), *b = tvar("b");
		obj_type **k = a_alloc(2 * sizeof *k);
		k[0] = t_list(a); k[1] = t_list(b);
		obj_type **rk = a_alloc(2 * sizeof *rk);
		rk[0] = a; rk[1] = b;
		return t_functor(t_compound(RET_TUPLE, 2, k),
		                  t_list(t_compound(RET_TUPLE, 2, rk)));
	}
	if (tok_is(name, "filter")) {
		obj_type *a = tvar("a");
		obj_type **k = a_alloc(2 * sizeof *k);
		k[0] = t_list(a); k[1] = t_functor(a, t_new(RET_BOOL));
		return t_functor(t_compound(RET_TUPLE, 2, k), t_list(a));
	}
	if (tok_is(name, "fold")) {
		obj_type *a = tvar("a"), *b = tvar("b");
		obj_type **cbk = a_alloc(2 * sizeof *cbk);
		cbk[0] = b; cbk[1] = a;
		obj_type **k = a_alloc(3 * sizeof *k);
		k[0] = t_list(a); k[1] = b; k[2] = t_functor(t_compound(RET_TUPLE, 2, cbk), b);
		return t_functor(t_compound(RET_TUPLE, 3, k), b);
	}
	if (tok_is(name, "flatten")) {
		obj_type *a = tvar("a");
		return t_functor(t_list(t_list(a)), t_list(a));
	}
	if (tok_is(name, "concat")) {
		obj_type *a = tvar("a");
		return t_functor(t_variadic(t_list(a)), t_list(a));
	}
	if (tok_is(name, "cat")) {
		return t_functor(t_variadic(t_new(RET_STRING)), t_new(RET_STRING));
	}
	if (tok_is(name, "run")) {
		return t_functor(t_variadic(t_new(RET_COMMAND)), t_new(RET_NONE));
	}
	if (tok_is(name, "stale")) {
		obj_type **k = a_alloc(2 * sizeof *k);
		k[0] = t_file(); k[1] = t_variadic(t_file());
		return t_functor(t_compound(RET_TUPLE, 2, k), t_new(RET_BOOL));
	}
	if (tok_is(name, "sum") || tok_is(name, "min") || tok_is(name, "max")) {
		return t_functor(t_variadic(t_new(RET_INT)), t_new(RET_INT));
	}
	if (tok_is(name, "flag")) {
		return t_functor(t_variadic(t_new(RET_STRING)), t_new(RET_BOOL));
	}
	if (tok_is(name, "first") || tok_is(name, "last")) {
		obj_type *a = tvar("a");
		return t_functor(t_list(a), a);
	}
	if (tok_is(name, "take") || tok_is(name, "drop")) {
		obj_type *a = tvar("a");
		obj_type **k = a_alloc(2 * sizeof *k);
		k[0] = t_list(a); k[1] = t_new(RET_INT);
		return t_functor(t_compound(RET_TUPLE, 2, k), t_list(a));
	}
	if (tok_is(name, "sort") || tok_is(name, "unique") || tok_is(name, "reverse")) {
		obj_type *a = tvar("a");
		return t_functor(t_list(a), t_list(a));
	}
	if (tok_is(name, "slice")) {
		obj_type *a = tvar("a");
		obj_type **k = a_alloc(3 * sizeof *k);
		k[0] = t_list(a); k[1] = t_new(RET_INT); k[2] = t_new(RET_INT);
		return t_functor(t_compound(RET_TUPLE, 3, k), t_list(a));
	}
	if (tok_is(name, "index")) {
		obj_type *a = tvar("a"), *b = tvar("b");
		obj_type **k = a_alloc(2 * sizeof *k);
		k[0] = t_list(a); k[1] = b;
		return t_functor(t_compound(RET_TUPLE, 2, k), t_new(RET_INT));
	}
	if (tok_is(name, "count")) {
		obj_type *a = tvar("a");
		obj_type **k = a_alloc(2 * sizeof *k);
		k[0] = t_list(a); k[1] = a;
		return t_functor(t_compound(RET_TUPLE, 2, k), t_new(RET_INT));
	}
	return NULL;
}

static obj_type *infer_call_impl(ast *node, scope *sc) {
	ast *callee = node->kids[0];

	/* always visit every argument so errors inside them surface */
	size_t na = node->nkids - 1;
	obj_type *a0 = na >= 1 ? infer(node->kids[1], sc) : NULL;
	obj_type *a1 = na >= 2 ? infer(node->kids[2], sc) : NULL;
	for (size_t i = 3; i < node->nkids; i++)
		infer(node->kids[i], sc);

	if (callee->kind == AST_ID) {
		/* arity check for builtins: -1 max means variadic. A bad arg count
		 * would otherwise read past node->kids at run time (segfault). */
		for (size_t i = 0; i < N_CORE_ARITY; i++) {
			if (!tok_is(callee->tok, g_core_arity[i].name)) continue;
			int mn = g_core_arity[i].min, mx = g_core_arity[i].max;
			if ((int)na < mn || (mx >= 0 && (int)na > mx)) {
				if (mx < 0)
					type_err(callee->tok, "%s expects at least %d argument%s, got %zu",
					         g_core_arity[i].name, mn, mn == 1 ? "" : "s", na);
				else if (mn == mx)
					type_err(callee->tok, "%s expects %d argument%s, got %zu",
					         g_core_arity[i].name, mn, mn == 1 ? "" : "s", na);
				else
					type_err(callee->tok, "%s expects %d to %d arguments, got %zu",
					         g_core_arity[i].name, mn, mx, na);
			}
			break;
		}

		/* glob(pattern) -> List<File> */
		if (tok_is(callee->tok, "glob"))
			return t_list(t_file());

		/* subst(s, from, to) -> String */
		if (tok_is(callee->tok, "subst"))
			return t_new(RET_STRING);

		/* stale(a, b) -> Bool ; run(...) -> Bool (true if it had work) */
		if (tok_is(callee->tok, "stale") || tok_is(callee->tok, "run"))
			return t_new(RET_BOOL);

		/* print(...) / debug(...) -> None (side effect only) */
		if (tok_is(callee->tok, "print") || tok_is(callee->tok, "debug"))
			return t_new(RET_NONE);

		/* time intrinsics -> Int */
		if (tok_is(callee->tok, "clock") || tok_is(callee->tok, "time"))
			return t_new(RET_INT);
		/* sleep(ms) -> () */
		if (tok_is(callee->tok, "sleep"))
			return t_new(RET_NONE);
		/* config(map) -> () */
		if (tok_is(callee->tok, "config"))
			return t_new(RET_NONE);
		/* CLI arg helpers */
		if (tok_is(callee->tok, "flag"))   return t_new(RET_BOOL);
		if (tok_is(callee->tok, "opt"))    return t_new(RET_STRING);
		if (tok_is(callee->tok, "positionals")) return t_list(t_new(RET_STRING));
		/* bench(label, expr) -> expr's value (dynamic) */
		if (tok_is(callee->tok, "bench"))
			return t_new(RET_VAR);

		/* json: values are dynamic (?). */
		if (tok_is(callee->tok, "json_parse") || tok_is(callee->tok, "json_read")
		 || tok_is(callee->tok, "get"))
			return t_new(RET_VAR);
		if (tok_is(callee->tok, "json_str") || tok_is(callee->tok, "json_pretty"))
			return t_new(RET_STRING);
		if (tok_is(callee->tok, "json_write")) return t_file();
		if (tok_is(callee->tok, "has"))        return t_new(RET_BOOL);
		if (tok_is(callee->tok, "keys"))       return t_list(t_new(RET_STRING));
		if (tok_is(callee->tok, "values"))     return t_list(t_new(RET_VAR));
		if (tok_is(callee->tok, "items")) {
			obj_type **kv = a_alloc(2 * sizeof *kv);
			kv[0] = t_new(RET_STRING);
			kv[1] = t_new(RET_VAR);
			return t_list(t_compound(RET_TUPLE, 2, kv));
		}

		/* python-style list/string ops */
		if (tok_is(callee->tok, "range")) return t_list(t_new(RET_INT));
		if (tok_is(callee->tok, "enumerate")) {
			obj_type **kv = a_alloc(2 * sizeof *kv);
			kv[0] = t_new(RET_INT);
			kv[1] = elem_of(a0);
			return t_list(t_compound(RET_TUPLE, 2, kv));
		}
		if (tok_is(callee->tok, "slice") || tok_is(callee->tok, "reverse"))
			return a0 ? a0 : t_list(t_new(RET_VAR));
		if (tok_is(callee->tok, "index") || tok_is(callee->tok, "sum")
		 || tok_is(callee->tok, "min") || tok_is(callee->tok, "max")
		 || tok_is(callee->tok, "count") || tok_is(callee->tok, "find")
		 || tok_is(callee->tok, "to_int"))
			return t_new(RET_INT);
		if (tok_is(callee->tok, "to_float")) return t_new(RET_FLOAT);
		if (tok_is(callee->tok, "to_str") || tok_is(callee->tok, "repeat"))
			return t_new(RET_STRING);
		if (tok_is(callee->tok, "chars")) return t_list(t_new(RET_STRING));

		/* zip(seq, seq, ...) -> List<Tuple<elem, elem, ...>> */
		if (tok_is(callee->tok, "zip")) {
			obj_type **elems = na ? a_alloc(na * sizeof *elems) : NULL;
			for (size_t i = 0; i < na; i++) {
				obj_type *ai = infer(node->kids[i + 1], sc);
				if (!is_seq_t(ai))
					type_err(callee->tok, "zip argument %zu must be a List or Tuple, got %s",
					         i + 1, ret_kind_name(ai->ret));
				elems[i] = elem_of(ai);
			}
			return t_list(t_compound(RET_TUPLE, na, elems));
		}

		/* config / process / fs */
		if (tok_is(callee->tok, "env")    || tok_is(callee->tok, "read"))
			return t_new(RET_STRING);
		/* shell(cmd|tuple|list) -> captured String, or nested tuple/list thereof */
		if (tok_is(callee->tok, "shell")) return t_new(RET_VAR);
		if (tok_is(callee->tok, "which"))  return t_file();
		if (tok_is(callee->tok, "exists")) return t_new(RET_BOOL);
		if (tok_is(callee->tok, "mkdir"))  return t_file();
		if (tok_is(callee->tok, "write"))  return a0 ? a0 : t_file();
		if (tok_is(callee->tok, "move") || tok_is(callee->tok, "copy")
		 || tok_is(callee->tok, "symlink") || tok_is(callee->tok, "touch"))
			return t_file();
		if (tok_is(callee->tok, "remove") || tok_is(callee->tok, "is_dir")
		 || tok_is(callee->tok, "is_file"))
			return t_new(RET_BOOL);

		/* lists */
		if (tok_is(callee->tok, "filter") || tok_is(callee->tok, "sort")
		 || tok_is(callee->tok, "unique")) return a0 ? a0 : t_list(t_new(RET_VAR));
		if (tok_is(callee->tok, "flatten"))return t_list(elem_of(elem_of(a0)));
		if (tok_is(callee->tok, "concat")) return t_list(elem_of(a0));
		if (tok_is(callee->tok, "fold"))   return a1 ? a1 : t_new(RET_VAR);
		if (tok_is(callee->tok, "contains")) return t_new(RET_BOOL);
		if (tok_is(callee->tok, "length")) return t_new(RET_INT);
		if (tok_is(callee->tok, "first") || tok_is(callee->tok, "last"))
			return elem_of(a0);

		/* strings */
		if (tok_is(callee->tok, "join") || tok_is(callee->tok, "replace")
		 || tok_is(callee->tok, "cat")  || tok_is(callee->tok, "upper")
		 || tok_is(callee->tok, "lower")|| tok_is(callee->tok, "trim")
		 || tok_is(callee->tok, "capitalize") || tok_is(callee->tok, "title")
		 || tok_is(callee->tok, "lstrip") || tok_is(callee->tok, "rstrip")
		 || tok_is(callee->tok, "strip_prefix") || tok_is(callee->tok, "strip_suffix")
		 || tok_is(callee->tok, "pad_left") || tok_is(callee->tok, "pad_right")
		 || tok_is(callee->tok, "center"))
			return t_new(RET_STRING);
		if (tok_is(callee->tok, "split"))  return t_list(t_new(RET_STRING));
		if (tok_is(callee->tok, "starts_with") || tok_is(callee->tok, "ends_with"))
			return t_new(RET_BOOL);

		/* paths */
		if (tok_is(callee->tok, "parent") || tok_is(callee->tok, "name")
		 || tok_is(callee->tok, "stem")   || tok_is(callee->tok, "suffix"))
			return t_new(RET_STRING);
		if (tok_is(callee->tok, "with_suffix") || tok_is(callee->tok, "with_name")
		 || tok_is(callee->tok, "path") || tok_is(callee->tok, "abs"))
			return t_file();

		/* registered (host/plugin) builtin not handled above: arity + return
		 * type from the shared registry (fbuild.h). Consulted last so core
		 * names keep their precise inference. */
		const fb_Reg *rg = fb_reg_findn(callee->tok->string, callee->tok->len);
		if (rg) {
			if ((int)na < rg->min || (rg->max >= 0 && (int)na > rg->max)) {
				if (rg->max < 0)
					type_err(callee->tok, "%s expects at least %d argument%s, got %zu",
					         rg->name, rg->min, rg->min == 1 ? "" : "s", na);
				else if (rg->min == rg->max)
					type_err(callee->tok, "%s expects %d argument%s, got %zu",
					         rg->name, rg->min, rg->min == 1 ? "" : "s", na);
				else
					type_err(callee->tok, "%s expects %d to %d arguments, got %zu",
					         rg->name, rg->min, rg->max, na);
			}
			return fb_ret_type(rg->ret);
		}
	}

	/* otherwise: applying a user function value */
	tbind *lb = callee->kind == AST_ID ? scope_find(sc, callee->tok) : NULL;
	int local = lb != NULL;
	obj_type *ft = (callee->kind == AST_ID)
	             ? (local ? lb->type : infer_id(callee->tok))
	             : infer(callee, sc);
	/* callee is resolved directly above (not via infer()), so it never gets
	 * annotated the way every other node does -- do it here so hover on a
	 * call's callee shows the function's own type, not just its call result. */
	if (callee->kind == AST_ID)
		callee->type = ft;

	/* Hover-only re-specialization against this call's real, already-inferred
	 * argument types, instead of a cached generic/context-free signature
	 * (g_specialize_calls stays set through the whole walk, so a call inside
	 * a specialized body specializes too). Left off during the normal
	 * infer_program() pass. Two ways `lam` (the def to specialize) is found:
	 *   - callee is a *named* top-level function (find_toplevel_lambda)
	 *   - callee is a *local* param that's itself a direct alias for one
	 *     (lb->lam, set by an OUTER specialize_call when binding this param
	 *     -- e.g. create_cmd's `f`, bound to `compile` at a call site, lets
	 *     `f(...)` inside create_cmd's body specialize `compile` itself
	 *     against THAT inner call's real args, not just show compile's
	 *     generic (?, ? -> Command)) */
	if (g_specialize_calls && callee->kind == AST_ID) {
		ast *lam = local ? lb->lam : find_toplevel_lambda(g_program, callee->tok);
		if (lam)
			return specialize_call(node, na, a0, a1, sc, lam, callee);
	}

	if (ft && ft->ret == RET_FUNCTOR) {
		if (na == 1 && !unify(ft->children[0], a0))
			type_err(callee->kind == AST_ID ? callee->tok : NULL,
			         "argument is %s, function expects %s",
			         ret_kind_name(a0 ? a0->ret : RET_VAR),
			         ret_kind_name(ft->children[0]->ret));
		return ft->nchildren > 1 ? ft->children[1] : t_new(RET_VAR);
	}
	return t_new(RET_VAR);
}

static obj_type *infer_call(ast *node, scope *sc) {
	ast *callee = node->kids[0];
	obj_type *result = infer_call_impl(node, sc);

	/* A builtin callee (the core name-match chain above, or a registered
	 * host/plugin builtin) is dispatched via early returns that never reach
	 * the "user function value" callee->type = ft annotation -- so without
	 * this, hovering e.g. "zip" or "print" directly shows a bare "?" even
	 * though the call itself infers fine. Annotate uniformly here instead:
	 * whatever arity is known (the core table, else the registry) for the
	 * param shape, this call's own already-computed result for the return
	 * type. Only steps in when there's nothing more specific already --
	 * NULL (the common case above), or a bare RET_VAR: a builtin that's
	 * documented (g_core_arity/is_builtin_name) but not name-matched in
	 * infer_call_impl's dispatch AND not currently registered (e.g. a
	 * plugin-provided name like take/drop when that plugin isn't loaded)
	 * falls through infer_id to the generic "user function value" path,
	 * which sets callee->type to a bare RET_VAR -- worth upgrading too. */
	if (callee->kind == AST_ID && (!callee->type || callee->type->ret == RET_VAR)) {
		obj_type *generic = core_generic_signature(callee->tok);
		if (generic) {
			/* a curated signature is a complete, self-consistent generic
			 * description on its own (e.g. filter's List<a> result already
			 * shares "a" with its List<a> param) -- use it as-is rather than
			 * grafting on `result`, which without a real call site is just
			 * whatever the plain dispatch above defaulted to, strictly less
			 * informative than the curated shape. */
			callee->type = generic;
		} else {
			int mn = -1, mx = -1;
			for (size_t i = 0; i < N_CORE_ARITY; i++)
				if (tok_is(callee->tok, g_core_arity[i].name)) {
					mn = g_core_arity[i].min; mx = g_core_arity[i].max;
					break;
				}
			if (mn < 0) {
				const fb_Reg *rg = fb_reg_findn(callee->tok->string, callee->tok->len);
				if (rg) { mn = rg->min; mx = rg->max; }
			}
			if (mn >= 0) {
				obj_type *param;
				if (mx < 0) {
					/* variadic (max<0): `mn` fixed "?" slots (none if
					 * mn==0) followed by a trailing "*?" absorbing the
					 * rest -- same "*" a capture param's own generic slot
					 * uses (infer_param_type_from_usage), since this is
					 * the same shape: zero or more of an unknown type. */
					if (mn == 0) {
						param = t_variadic(t_new(RET_VAR));
					} else {
						obj_type **k = a_alloc(((size_t)mn + 1) * sizeof *k);
						for (int j = 0; j < mn; j++) k[j] = t_new(RET_VAR);
						k[mn] = t_variadic(t_new(RET_VAR));
						param = t_compound(RET_TUPLE, (size_t)mn + 1, k);
					}
				} else {
					size_t n = (size_t)mx;
					param = n <= 1 ? t_new(RET_VAR) : tuple_of_n_var(n);
				}
				callee->type = t_functor(param, result);
			}
		}
	}
	return result;
}

static obj_type *tuple_of_n_var(size_t n) {
	obj_type **k = a_alloc(n * sizeof *k);
	for (size_t i = 0; i < n; i++)
		k[i] = t_new(RET_VAR);
	return t_compound(RET_TUPLE, n, k);
}

/* the token a param slot binds its name from, or NULL for a slot with no
 * single stable name (a nested destructure pattern, AST_TUPLE) */
static token *param_name_tok(ast *p) {
	switch (p->kind) {
	case AST_ID:
	case AST_PROJECTION: return p->tok;
	case AST_CAPTURE:    return p->nkids ? p->kids[0]->tok : NULL;
	default:             return NULL;
	}
}

/* first call `name(...)` found anywhere in `node`, syntactically -- its
 * arity, or -1 if `name` is never called there. Used only to guess a
 * standalone lambda's own param types from how the body uses them (see
 * infer_param_type_from_usage); a real call site's actual argument type,
 * via specialization, always wins over this guess when one's available. */
static int param_call_arity(ast *node, token *name) {
	if (!node) return -1;
	if (node->kind == AST_CALL && node->kids[0]->kind == AST_ID
	    && tok_eq(node->kids[0]->tok, name))
		return (int)(node->nkids - 1);
	for (size_t i = 0; i < node->nkids; i++) {
		int r = param_call_arity(node->kids[i], name);
		if (r >= 0) return r;
	}
	return -1;
}

/* is `name` ever spread (`...name`) anywhere in `node`, syntactically? A
 * splat only makes sense on a sequence (SKILLS.md: "spread into a tuple/
 * args/command/map"), so this is as telling as param_call_arity is for a
 * called param -- see infer_param_type_from_usage. */
static int param_is_splatted(ast *node, token *name) {
	if (!node) return 0;
	if (node->kind == AST_SPLAT && node->kids[0]->kind == AST_ID
	    && tok_eq(node->kids[0]->tok, name))
		return 1;
	for (size_t i = 0; i < node->nkids; i++)
		if (param_is_splatted(node->kids[i], name)) return 1;
	return 0;
}

/* a display-only "*T" (zero or more T) marker -- see obj_type.is_variadic */
static obj_type *t_variadic(obj_type *elem) {
	obj_type *t = t_list(elem);
	t->is_variadic = 1;
	return t;
}

/* A standalone lambda's own params have no call site to type them from, so
 * they default to "?" (RET_VAR) -- except:
 *   - a capture param (@rest) structurally *is* "zero or more of whatever
 *     follows", regardless of how the body uses it, so it always shows as
 *     "*?" instead of a bare "?".
 *   - if the body calls a param as a function (`f(...)`), that param is
 *     evidently itself a function, so show it as a functor of that call's
 *     arity instead of a bare "?".
 *   - if the body ever spreads a param (`...s`), it's evidently a sequence,
 *     so show it as List<?> instead of a bare "?".
 * Narrow, syntactic, single-purpose (these usage shapes only) -- not a
 * general unification solver. */
static obj_type *infer_param_type_from_usage(ast *p, ast *body) {
	if (p->kind == AST_CAPTURE)
		return t_variadic(t_new(RET_VAR));
	token *name = param_name_tok(p);
	if (name) {
		int arity = param_call_arity(body, name);
		if (arity >= 0)
			return t_functor(arity == 1 ? t_new(RET_VAR) : tuple_of_n_var((size_t)arity),
			                  t_new(RET_VAR));
		if (param_is_splatted(body, name))
			return t_list(t_new(RET_VAR));
	}
	return t_new(RET_VAR);
}

/* bind a lambda's parameters against the argument it is applied to. A lambda
 * takes one argument; multiple params destructure it positionally.
 *
 * `annotate` controls the ONLY side effect that isn't scope construction:
 * writing each param declaration's own node->type, for hover. Real inference
 * (infer()'s AST_LAMBDA case, infer_apply) passes 1 -- the `arg` there is a
 * real type, worth showing. locate_scope() (goto-def's/hover's structural,
 * scope-shape-only walk) passes 0: it calls this with a placeholder RET_VAR
 * `arg` for every lambda it structurally walks through on the way to a
 * target -- often lambdas unrelated to what's actually being hovered -- so
 * annotating there would stomp a real, already-correct type (or a
 * specialized one from a previous hover) with a meaningless placeholder.
 *
 * `arg_lams` (may be NULL) parallels `arg`'s flattened slots (see
 * specialize_call): arg_lams[i] is the top-level lambda node that slot i's
 * value directly aliases, if any, so a param bound to that slot carries the
 * alias forward (see tbind.lam) -- letting a later call *through* the param
 * specialize the aliased function too, not just show its generic signature. */
static void bind_params_lam(scope *sc, ast *params, obj_type *arg, ast **arg_lams, int annotate) {
	sc->arg = arg;
	size_t P = params->nkids;
	for (size_t i = 0; i < P; i++) {
		ast *p = params->kids[i];
		obj_type *pt = (P == 1) ? arg : positional(arg, i);
		switch (p->kind) {
		case AST_ID:                       /* x         */
		case AST_PROJECTION:               /* .src / .0 */
			if (p->tok) {
				ast *lam = (P == 1) ? (arg_lams ? arg_lams[0] : NULL)
				                    : positional_lam(arg_lams, arg, i);
				scope_put_lam(sc, p->tok, pt, lam);
			}
			if (annotate) p->type = pt;   /* declaration site, for hover */
			break;
		case AST_CAPTURE:                  /* @t -> rest of the args (i..end) */
			if (p->nkids) {
				obj_type *rest;
				if (arg && arg->ret == RET_TUPLE) {
					size_t r = i < arg->nchildren ? arg->nchildren - i : 0;
					obj_type **k = r ? a_alloc(r * sizeof *k) : NULL;
					for (size_t j = 0; j < r; j++)
						k[j] = arg->children[i + j];
					rest = t_compound(RET_TUPLE, r, k);
				} else if (i == 0) {
					rest = arg;            /* single arg / list: whole */
				} else {
					rest = t_new(RET_VAR);
				}
				scope_put(sc, p->kids[0]->tok, rest);
				if (annotate) p->kids[0]->type = rest;   /* declaration site, for hover */
			}
			break;
		case AST_TUPLE:
			/* (a, b): destructure this one positional argument by field name.
			 * Static field shapes aren't tracked for maps/objects (RET_VAR /
			 * RET_OBJECT, see ast.h), so record_field() -- meaningful only for
			 * RET_FILE records -- falls back to RET_VAR here, same permissive
			 * behavior as an ordinary named field access on an object. */
			for (size_t j = 0; j < p->nkids; j++) {
				ast *sub = p->kids[j];
				if (!sub->tok) continue;
				obj_type *ft = record_field(pt, sub->tok);
				obj_type *bound = ft ? ft : t_new(RET_VAR);
				scope_put(sc, sub->tok, bound);
				if (annotate) sub->type = bound;   /* declaration site, for hover */
			}
			break;
		default:
			break;
		}
	}
}

static void bind_params(scope *sc, ast *params, obj_type *arg, int annotate) {
	bind_params_lam(sc, params, arg, NULL, annotate);
}

/* apply a function node to an argument type, returning the result type.
 * arg_lams: see bind_params_lam. */
static obj_type *infer_apply_lam(ast *fn, obj_type *arg, ast **arg_lams, scope *sc) {
	if (fn->kind == AST_LAMBDA) {
		scope inner = { .parent = sc };
		bind_params_lam(&inner, fn->kids[0], arg, arg_lams, 1);
		obj_type *res = infer(fn->kids[1], &inner);
		fn->type = t_functor(arg, res);   /* annotate for display */
		return res;
	}
	if (fn->kind == AST_MORPHISM) {    /* File -> File */
		fn->type = t_functor(t_file(), t_file());
		if (arg && arg->ret != RET_FILE && arg->ret != RET_VAR)
			type_err(fn->tok, "morphism expects a File, got %s",
			         ret_kind_name(arg->ret));
		return t_file();
	}

	obj_type *ft = infer(fn, sc);
	if (ft && ft->ret == RET_FUNCTOR && ft->nchildren > 1) {
		if (!unify(ft->children[0], arg))
			type_err(NULL, "function expects %s, got %s",
			         ret_kind_name(ft->children[0]->ret),
			         ret_kind_name(arg ? arg->ret : RET_VAR));
		return ft->children[1];
	}
	return t_new(RET_VAR);
}

static obj_type *infer_apply(ast *fn, obj_type *arg, scope *sc) {
	return infer_apply_lam(fn, arg, NULL, sc);
}

/* Hover-only. Specialize `lam` (a top-level lambda, reached either by name
 * or through a local alias -- see infer_call) against this call's real
 * argument types, and (if `callee` is non-NULL, i.e. this call was reached
 * via a named/aliased AST_ID callee) annotate that callee node's own hover
 * type from the specialized param bindings, not just the call's result. */
/* structural equality (same shape, not just same top-level kind) -- used
 * only to decide whether a specialized capture param's bound values are
 * uniform enough to collapse into "*T", see collapse_variadic. */
static int types_structurally_equal(obj_type *a, obj_type *b) {
	if (a == b) return 1;
	if (!a || !b) return 0;
	if (a->ret != b->ret) return 0;
	if (a->ret == RET_OBJECT)
		return a->class_name == b->class_name
		    || (a->class_name && b->class_name && tok_eq(a->class_name, b->class_name));
	if (a->nchildren != b->nchildren) return 0;
	for (size_t i = 0; i < a->nchildren; i++)
		if (!types_structurally_equal(a->children[i], b->children[i])) return 0;
	return 1;
}

/* A specialized capture param is bound to a real Tuple<...> (the actual
 * trailing args at this call site, see bind_params_lam's AST_CAPTURE case).
 * If every element turned out the same type, "*T" (zero or more T) reads
 * better than spelling out "Tuple<T, T, T>" -- but only then: a genuinely
 * mixed tail (Tuple<String, Command>) is kept as an explicit tuple, since
 * "*" would misrepresent it as uniform. Non-tuples (already collapsed, or
 * the "no call site" placeholder) and empty tuples pass through unchanged. */
static obj_type *collapse_variadic(obj_type *t) {
	if (!t || t->ret != RET_TUPLE || t->nchildren == 0) return t;
	for (size_t i = 1; i < t->nchildren; i++)
		if (!types_structurally_equal(t->children[0], t->children[i])) return t;
	return t_variadic(t->children[0]);
}

static obj_type *specialize_call(ast *node, size_t na, obj_type *a0, obj_type *a1,
                                  scope *sc, ast *lam, ast *callee) {
	/* one type per syntactic argument (a0/a1 already computed by the caller) */
	obj_type **argt = na ? a_alloc(na * sizeof *argt) : NULL;
	if (na >= 1) argt[0] = a0;
	if (na >= 2) argt[1] = a1;
	for (size_t i = 2; i < na; i++)
		argt[i] = infer(node->kids[i + 1], sc);

	/* Flatten: a splat of a known tuple contributes its own elements as
	 * separate positional slots instead of one opaque tuple slot -- e.g.
	 * `f(x, ...pair)` where pair:Tuple<A,B> is 3 slots (x, A, B), not 2
	 * (x, Tuple<A,B>), so a callee param bound to one of pair's elements
	 * gets that element's real type instead of collapsing to "?". */
	size_t total = 0;
	for (size_t i = 0; i < na; i++) {
		ast *argn = node->kids[i + 1];
		total += (argn->kind == AST_SPLAT && argt[i] && argt[i]->ret == RET_TUPLE)
		       ? argt[i]->nchildren : 1;
	}
	obj_type *arg = NULL;
	ast **arg_lams = total ? a_alloc(total * sizeof *arg_lams) : NULL;
	if (total >= 1) {
		obj_type **k = a_alloc(total * sizeof *k);
		size_t j = 0;
		for (size_t i = 0; i < na; i++) {
			ast *argn = node->kids[i + 1];
			if (argn->kind == AST_SPLAT && argt[i] && argt[i]->ret == RET_TUPLE) {
				for (size_t e = 0; e < argt[i]->nchildren; e++) {
					k[j] = argt[i]->children[e];
					arg_lams[j] = NULL;   /* a spread element's own provenance isn't tracked */
					j++;
				}
			} else {
				k[j] = argt[i];
				/* this slot's own alias, if it has one -- a local that's
				 * itself an alias (recorded by an outer specialize_call), or
				 * a direct reference to a named top-level lambda */
				arg_lams[j] = NULL;
				if (argn->kind == AST_ID) {
					tbind *b = scope_find(sc, argn->tok);
					arg_lams[j] = b ? b->lam : find_toplevel_lambda(g_program, argn->tok);
				}
				j++;
			}
		}
		arg = (total == 1) ? k[0] : t_compound(RET_TUPLE, total, k);
	}

	obj_type *result = infer_apply_lam(lam, arg, arg_lams, NULL);

	/* infer_apply_lam's bind_params_lam (annotate=1) just annotated each of
	 * the def's own param declarations with the real type (and alias, if
	 * any) bound at THIS call -- read the types back so the callee's own
	 * hover (at this specific call site, not its bare/generic definition)
	 * also reflects what it looks like as used here, not just the call's
	 * result. Each of these ast* nodes belongs to `lam` (the shared def), so
	 * this only affects hovering "callee" at call sites -- hovering the
	 * def's own name still re-infers generically (no caller to inform it).
	 *
	 * For a param that carries an alias (arg_lams[i] set, e.g. create_cmd's
	 * `f` bound to `compile`): the alias target's OWN ->type, not the
	 * param's, since infer_apply_lam (just above) already fully inferred
	 * `lam`'s body -- which, if the body calls that param (`f(...)`), just
	 * specialized the aliased def itself via this same mechanism recursively
	 * and left the refined signature on ITS node. The param's own ->type is
	 * only what it looked like on the way IN (compile's generic signature);
	 * the alias's ->type is what it turned out to mean once used. */
	if (callee) {
		ast *plist = lam->kids[0];
		size_t PP = plist->nkids;
		if (PP >= 1) {
			obj_type **pk = a_alloc(PP * sizeof *pk);
			for (size_t i = 0; i < PP; i++) {
				ast *p = plist->kids[i];
				ast *p_lam = (i < total) ? arg_lams[i] : NULL;
				pk[i] = p_lam ? p_lam->type
				     : (p->kind == AST_CAPTURE && p->nkids) ? collapse_variadic(p->kids[0]->type) : p->type;
			}
			callee->type = t_functor(PP == 1 ? pk[0] : t_compound(RET_TUPLE, PP, pk), result);
		}
	}
	return result;
}

/* sentinel parked on node->type while a context-free node is mid-inference,
 * so a self-referential def resolves to ? instead of recursing forever. */
static obj_type T_INPROGRESS = { .ret = RET_VAR };

static obj_type *infer(ast *node, scope *sc) {
	if (!node)
		return t_new(RET_VAR);
	/* Cycle guard: always active when !sc, specializing or not -- a
	 * self-referential def must not recurse forever just because hover asked
	 * for a fresher answer. Separate from the cache-value reuse right below,
	 * which IS skipped under g_specialize_calls (hover): that pass exists
	 * specifically to get a fresher, call-site-specific answer than whatever
	 * infer_program()'s original pass already cached here. */
	if (!sc && node->type == &T_INPROGRESS)
		return t_new(RET_VAR);
	if (!sc && node->type && !g_specialize_calls)   /* context-free memo (shared ids -> DAG) */
		return node->type;
	if (!sc)
		node->type = &T_INPROGRESS;

	obj_type *t;
	switch (node->kind) {

	case AST_INT:    t = t_new(RET_INT);    break;
	case AST_FLOAT:  t = t_new(RET_FLOAT);  break;
	case AST_BOOL:   t = t_new(RET_BOOL);   break;
	case AST_NIL:    t = t_new(RET_VAR);    break;   /* unifies with anything */
	case AST_STRTEXT: t = t_new(RET_STRING); break;
	case AST_STRING:                                 /* visit {expr} parts */
		for (size_t i = 0; i < node->nkids; i++)
			infer(node->kids[i], sc);
		t = t_new(RET_STRING);
		break;
	case AST_PATH:   t = t_file();          break;   /* a path is a File */

	case AST_ID: {
		obj_type *bt = scope_get(sc, node->tok);
		t = bt ? bt : infer_id(node->tok);
		break;
	}

	case AST_CALL:    t = infer_call(node, sc); break;

	case AST_OBJNEW:                                 /* object of unknown shape */
		infer(node->kids[0], sc);                    /* check the fields body    */
		t = t_new(RET_OBJECT);
		t->class_name = node->tok;                   /* class name, e.g. "Point" */
		break;

	case AST_METHODDEF:                              /* method Class.name = fn   */
		infer(node->kids[1], sc);   /* the lambda; kids[0] is the class name id, */
		t = t_new(RET_NONE);        /* not a variable, so don't infer it         */
		break;

	case AST_METHODCOPY:            /* method New.m = Old.m2 / New.* = Old.*   */
		t = t_new(RET_NONE);        /* kids are class/method name ids, not     */
		break;                      /* variables -- nothing to infer           */

	case AST_COMPARE: {
		obj_type *l = infer(node->kids[0], sc);
		obj_type *r = infer(node->kids[1], sc);
		if (!unify(l, r))
			type_err(node->tok, "cannot compare %s with %s",
			         ret_kind_name(l->ret), ret_kind_name(r->ret));
		t = t_new(RET_BOOL);
		break;
	}

	case AST_ARITH: {     /* + - * / % : (Int|Float) x (Int|Float) -> Int|Float */
		obj_type *l = infer(node->kids[0], sc);
		obj_type *r = infer(node->kids[1], sc);
		if (l->ret != RET_INT && l->ret != RET_FLOAT && l->ret != RET_VAR)
			type_err(node->tok, "arithmetic operand must be Int or Float, got %s",
			         ret_kind_name(l->ret));
		if (r->ret != RET_INT && r->ret != RET_FLOAT && r->ret != RET_VAR)
			type_err(node->tok, "arithmetic operand must be Int or Float, got %s",
			         ret_kind_name(r->ret));
		t = t_new(l->ret == RET_FLOAT || r->ret == RET_FLOAT ? RET_FLOAT : RET_INT);
		break;
	}

	case AST_NEG: {                      /* unary "-" : Int|Float -> Int|Float */
		obj_type *o = infer(node->kids[0], sc);
		if (o->ret != RET_INT && o->ret != RET_FLOAT && o->ret != RET_VAR)
			type_err(node->tok, "'-' operand must be numeric, got %s",
			         ret_kind_name(o->ret));
		t = t_new(o->ret == RET_FLOAT ? RET_FLOAT : RET_INT);
		break;
	}

	case AST_LOGICAL: {                  /* and/or/|| : any type, runtime truthy() decides */
		infer(node->kids[0], sc);
		infer(node->kids[1], sc);
		/* ||  is value-or (keeps operand type); and/or coerce to Bool at runtime */
		t = t_new(node->tok->kind == TOK_OROR ? RET_VAR : RET_BOOL);
		break;
	}

	case AST_NOT: {                      /* not : any -> Bool (runtime truthy()) */
		infer(node->kids[0], sc);
		t = t_new(RET_BOOL);
		break;
	}

	case AST_TUPLE: {
		obj_type **k = node->nkids ? a_alloc(node->nkids * sizeof *k) : NULL;
		for (size_t i = 0; i < node->nkids; i++)
			k[i] = infer(node->kids[i], sc);
		t = t_compound(RET_TUPLE, node->nkids, k);
		break;
	}

	case AST_LISTLIT: {              /* List<e, ...> : homogeneous element type */
		obj_type *elem = t_new(RET_VAR);
		for (size_t i = 0; i < node->nkids; i++) {
			obj_type *ei = infer(node->kids[i], sc);
			elem = (i == 0) ? ei : (unify(elem, ei) ? elem : t_new(RET_VAR));
		}
		t = t_list(elem);
		break;
	}

	case AST_MORPHISM:               /* [*.c] // [*.o] : File -> File */
		t = t_functor(t_file(), t_file());
		break;

	case AST_LAMBDA: {               /* standalone: arg type(s) unknown, refined a little from usage */
		scope inner = { .parent = sc };
		ast *plist = node->kids[0];
		size_t P = plist->nkids;
		obj_type *param;
		if (P <= 1) {
			param = P == 1 ? infer_param_type_from_usage(plist->kids[0], node->kids[1])
			               : t_new(RET_VAR);
		} else {
			/* one slot per param (not a bare "?") so a multi-param
			 * standalone lambda's own signature shows its arity, e.g.
			 * "(?, ?, ? -> ?)" instead of collapsing to "(? -> ?)". */
			obj_type **k = a_alloc(P * sizeof *k);
			for (size_t i = 0; i < P; i++)
				k[i] = infer_param_type_from_usage(plist->kids[i], node->kids[1]);
			param = t_compound(RET_TUPLE, P, k);
		}
		bind_params(&inner, plist, param, 1);
		t = t_functor(param, infer(node->kids[1], &inner));
		break;
	}

	case AST_PIPE: {                 /* in |> fn: map element-wise over seqs */
		t = infer(node->kids[0], sc);
		for (size_t i = 1; i < node->nkids; i++) {
			int over_seq = t && (t->ret == RET_LIST || t->ret == RET_TUPLE);
			obj_type *arg = over_seq ? elem_of(t) : t;
			obj_type *res = infer_apply(node->kids[i], arg, sc);
			t = over_seq ? t_list(res) : res;
		}
		break;
	}

	case AST_SEQ:                    /* visit every step; value is the last */
		t = t_new(RET_NONE);
		for (size_t i = 0; i < node->nkids; i++)
			t = infer(node->kids[i], sc);
		break;

	case AST_GUARD: {               /* when/where don't change the type */
		scope gs = { .parent = sc, .arg = sc ? sc->arg : NULL };
		ast *where = node->kids[1];
		ast *whenc = node->kids[2];
		if (where)
			for (size_t i = 0; i < where->nkids; i++) {
				ast *bd = where->kids[i];   /* AST_BINDING */
				scope_put(&gs, bd->tok, infer(bd->kids[0], &gs));
			}
		scope *bsc = where ? &gs : sc;
		if (whenc)                          /* any type: runtime truthy() decides */
			infer(whenc->kids[0], bsc);
		t = infer(node->kids[0], bsc);
		break;
	}

	case AST_FIELD: {               /* base.field / base.0 / base.{expr} */
		obj_type *base = infer(node->kids[0], sc);
		if (!node->tok && node->nkids == 2) {   /* dynamic ".{expr}" */
			infer(node->kids[1], sc);           /* index expr */
			t = t_new(RET_VAR);                 /* element type unknown */
			break;
		}
		long idx = tok_index(node->tok);
		if (idx >= 0) {
			t = positional(base, (size_t)idx);
		} else {
			obj_type *ft = record_field(base, node->tok);
			if (!ft && base && base->ret != RET_VAR && base->ret != RET_OBJECT)
				type_err(node->tok, "%s has no field '%.*s'",
				         ret_kind_name(base->ret),
				         (int)node->tok->len, node->tok->string);
			t = ft ? ft : t_new(RET_VAR);
		}
		break;
	}

	case AST_PROJECTION: {          /* .name / .idx on the implicit arg */
		obj_type *bt = node->tok ? scope_get(sc, node->tok) : NULL;
		if (bt) {
			t = bt;
		} else {
			long idx = tok_index(node->tok);
			if (idx >= 0)
				t = positional(sc ? sc->arg : NULL, (size_t)idx);
			else {
				/* named projection: a field of the implicit argument */
				obj_type *ft = record_field(sc ? sc->arg : NULL, node->tok);
				t = ft ? ft : t_new(RET_VAR);
			}
		}
		break;
	}

	case AST_CAPTURE: {             /* @ / @id : the implicit argument */
		obj_type *bt = node->nkids ? scope_get(sc, node->kids[0]->tok) : NULL;
		t = bt ? bt : (sc && sc->arg ? sc->arg : t_new(RET_VAR));
		break;
	}

	case AST_SPLAT:                 /* ...x : spreads, same element type */
		t = infer(node->kids[0], sc);
		break;

	case AST_CONSTRUCT: {           /* File<...>: record with typed fields */
		if (!tok_is(node->tok, "File")) {
			t = t_new(RET_VAR);
			break;
		}
		obj_type *rec = t_file();               /* standard fields seeded */
		for (size_t i = 0; i < node->nkids; i++) {
			ast *f = node->kids[i];                 /* AST_CFIELD */
			obj_type *fv = infer(f->kids[0], sc);
			if (f->tok) {                           /* name = value */
				rec_define(rec, f->tok, fv, f->tok);
			} else if (fv->ret == RET_FILE) {       /* *splat: inherit fields */
				for (size_t j = 0; j < fv->nchildren; j++)
					rec_define(rec, fv->fields[j], fv->children[j],
					           node->tok);
			} else if (fv->ret != RET_VAR) {
				type_err(node->tok,
				         "cannot splat %s into File (expected File)",
				         ret_kind_name(fv->ret));
			}
		}
		t = rec;
		break;
	}

	case AST_COMMAND:               /* a `...` command literal is an action */
		for (size_t i = 0; i < node->nkids; i++)
			if (node->kids[i]->kind != AST_CMDTEXT)
				infer(node->kids[i], sc);   /* visit interpolations */
		t = t_new(RET_COMMAND);
		break;

	case AST_METADATA:              /* &x : metadata (mtime-ish) */
		infer(node->kids[0], sc);
		t = t_new(RET_VAR);
		break;

	case AST_FORCE:                 /* ~x : re-eval x, same type as x */
		t = infer(node->kids[0], sc);
		break;

	case AST_STORE:                 /* lvalue <- rhs : yields rhs's type */
		infer(node->kids[0], sc);   /* type-check the lvalue chain */
		t = infer(node->kids[1], sc);
		break;

	case AST_MAPLIT:                /* { k: v } / Map<...> : dynamic map */
		for (size_t i = 0; i < node->nkids; i++)
			infer(node->kids[i], sc);   /* type-check entries */
		t = t_new(RET_VAR);
		break;

	case AST_PARSER: {              /* Parser<schema> : record, if schema is a literal map */
		infer(node->kids[0], sc);
		obj_type *rec = infer_parser_schema(node->kids[0]);
		t = rec ? rec : t_new(RET_VAR);
		break;
	}

	/* structural nodes carry no value type of their own */
	case AST_PROGRAM:
	case AST_ASSIGN:
	case AST_EAGER:
	case AST_PARAMS:
	case AST_WHERE:
	case AST_BINDING:
	case AST_WHEN:
	case AST_CFIELD:
	case AST_CMDTEXT:
		t = t_new(RET_NONE);
		break;

	default:
		UNREACHABLE("ast_kind: unknown: %d", node->kind);
	}

	node->type = t;     /* annotate (scoped pass overwrites with resolved) */
	return t;
}

obj_type *infer_type(ast *node) { return infer(node, NULL); }

/* ---- printing --------------------------------------------------------- */

void obj_type_print(obj_type *t) {
	if (!t) { printf("?"); return; }
	switch (t->ret) {
	case RET_NONE:   printf("None");   break;
	case RET_INT:    printf("Int");    break;
	case RET_FLOAT:  printf("Float");  break;
	case RET_STRING:  printf("String");  break;
	case RET_BOOL:    printf("Bool");    break;
	case RET_FILE:    printf("File");    break;   /* fields are standard */
	case RET_COMMAND: printf("Command"); break;
	case RET_VAR:     printf("%s", t->var_label ? t->var_label : "?"); break;

	case RET_OBJECT:
		if (t->class_name) printf("%.*s", (int)t->class_name->len, t->class_name->string);
		else                printf("Object");
		break;

	case RET_LIST:
		if (t->is_variadic) {
			printf("*");
			obj_type_print(t->nchildren ? t->children[0] : NULL);
		} else {
			printf("List<");
			obj_type_print(t->nchildren ? t->children[0] : NULL);
			printf(">");
		}
		break;

	case RET_TUPLE:
		printf("Tuple<");
		for (size_t i = 0; i < t->nchildren; i++) {
			if (i) printf(", ");
			obj_type_print(t->children[i]);
		}
		printf(">");
		break;

	case RET_FUNCTOR: {
		printf("(");
		/* a multi-param functor's own param slot is a RET_TUPLE (one entry
		 * per param, see infer()'s AST_LAMBDA case) -- show it flattened as
		 * "a, b, c -> r", not wrapped as "Tuple<a, b, c> -> r". */
		obj_type *p = t->nchildren > 0 ? t->children[0] : NULL;
		if (p && p->ret == RET_TUPLE) {
			for (size_t i = 0; i < p->nchildren; i++) {
				if (i) printf(", ");
				obj_type_print(p->children[i]);
			}
		} else {
			obj_type_print(p);
		}
		printf(" -> ");
		obj_type_print(t->nchildren > 1 ? t->children[1] : NULL);
		printf(")");
		break;
	}
	}
}

/* Recursion must be bounded: every self-call has to sit under a `when` guard
 * (the base case). An unconditional self-call always loops, so flag it. This
 * is syntactic - it proves a guard exists, not that the guard ever stops. */
static void check_recursion(token *name, ast *node, int guarded) {
	if (!node)
		return;

	if (node->kind == AST_CALL && node->nkids &&
	    node->kids[0]->kind == AST_ID && tok_eq(node->kids[0]->tok, name)) {
		if (!guarded)
			type_err(name, "unbounded recursion: '%.*s' calls itself "
			               "with no 'when' guard (no base case)",
			         (int)name->len, name->string);
		/* still descend into the arguments below */
	}

	if (node->kind == AST_GUARD) {              /* value is gated by `when` */
		int g = guarded || node->kids[2] != NULL;
		check_recursion(name, node->kids[0], g);
		if (node->kids[1])                      /* where bindings */
			check_recursion(name, node->kids[1], guarded);
		if (node->kids[2])                      /* when condition */
			check_recursion(name, node->kids[2], guarded);
		return;
	}

	/* `(a when c) || b`: reaching b proves c (and thus a) didn't fire -- the
	 * language's documented if/else idiom (SKILLS.md). Treat the right side
	 * of || as guarded the same way a `when` body is, so a self-call in the
	 * "else" branch of that idiom doesn't false-positive. */
	if (node->kind == AST_LOGICAL && node->tok->kind == TOK_OROR) {
		check_recursion(name, node->kids[0], guarded);
		check_recursion(name, node->kids[1], 1);
		return;
	}

	for (size_t i = 0; i < node->nkids; i++)
		check_recursion(name, node->kids[i], guarded);
}

/* shared body: g_type_errors is reset here so repeated calls in one process
 * (e.g. a daemon re-checking an edited document) don't accumulate across
 * calls. g_diag_sink must be set/cleared by the caller. */
static int infer_program_impl(ast *prog) {
	g_program = prog;
	g_type_errors = 0;
	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		int is_def = (s->kind == AST_ASSIGN || s->kind == AST_EAGER);
		obj_type *t = infer_type(is_def ? s->kids[0] : s);  /* type-check */
		if (is_def) {
			/* infer_type() above types the RHS; the AST_ASSIGN/AST_EAGER node
			 * itself (whose tok is the def's own name, at its declaration
			 * site) is never otherwise visited by infer() -- annotate it here
			 * so hovering the LHS name shows the def's type, not "?". */
			s->type = t;
			check_recursion(s->tok, s->kids[0], 0);
		}
	}
	return g_type_errors;
}

int infer_program(ast *prog) {
	g_diag_sink = NULL;
	int errs = infer_program_impl(prog);
	if (errs)
		fprintf(stderr, "%d type error%s\n", errs, errs == 1 ? "" : "s");
	return errs;
}

int infer_program_collect(ast *prog, fb_Diag *diags, int max_diags) {
	g_diag_sink = diags;
	g_diag_cap  = max_diags;
	g_diag_n    = 0;
	int errs = infer_program_impl(prog);
	g_diag_sink = NULL;
	return errs;
}

/* ============================================================================
 * Source-position tooling (hover / goto-definition), for embedders like an
 * LSP daemon. Everything below is read-only over an already-parsed,
 * already-inferred `ast *prog` -- no execution, no type_err()/diagnostics.
 * ========================================================================== */

/* ---- spans -------------------------------------------------------------- */

static int pos_before(fb_Pos a, fb_Pos b) {
	return a.line < b.line || (a.line == b.line && a.col < b.col);
}
static int pos_after(fb_Pos a, fb_Pos b) {
	return a.line > b.line || (a.line == b.line && a.col > b.col);
}
static int span_empty(fb_Span s) {
	return s.start.line == 0 && s.start.col == 0 && s.end.line == 0 && s.end.col == 0;
}

fb_Span ast_span(const ast *n) {
	fb_Span s = { .start = {0, 0}, .end = {0, 0} };
	if (!n) return s;
	int have = 0;
	if (n->tok) {
		s.start = (fb_Pos){ n->tok->start.line, n->tok->start.col };
		s.end   = (fb_Pos){ n->tok->end.line,   n->tok->end.col };
		have = 1;
	}
	for (size_t i = 0; i < n->nkids; i++) {
		if (!n->kids[i]) continue;   /* guard/where/when slots may be absent */
		fb_Span c = ast_span(n->kids[i]);
		if (span_empty(c)) continue;
		if (!have || pos_before(c.start, s.start)) s.start = c.start;
		if (!have || pos_after(c.end, s.end))       s.end   = c.end;
		have = 1;
	}
	return s;
}

static int span_contains(fb_Span s, int line, int col) {
	if (span_empty(s)) return 0;
	fb_Pos p = { line, col };
	return !pos_before(p, s.start) && !pos_after(p, s.end);
}

/* innermost node whose span covers (line, col), or NULL if none does. */
static ast *find_node(ast *n, int line, int col) {
	if (!n) return NULL;
	if (!span_contains(ast_span(n), line, col)) return NULL;
	for (size_t i = 0; i < n->nkids; i++) {
		ast *hit = find_node(n->kids[i], line, col);
		if (hit) return hit;
	}
	return n;
}

/* ---- symbol tables (top-level defs, methods) ---------------------------- */

typedef struct { token *name; ast *site; } fb_DefSym;
typedef struct { token *class_name, *method_name; ast *site; } fb_MethodSym;

#define FB_MAX_DEFSYMS    4096
#define FB_MAX_METHODSYMS 4096
static fb_DefSym    g_defsyms[FB_MAX_DEFSYMS];
static int          g_ndefsyms;
static fb_MethodSym g_methodsyms[FB_MAX_METHODSYMS];
static int          g_nmethodsyms;

/* One flat pass over prog's top-level statements. Class constructors
 * (`type Name(...) = ...`) desugar to plain AST_ASSIGN at parse time so
 * they're already covered by the AST_ASSIGN/AST_EAGER case -- no special
 * handling needed. Wildcard method copies (`method New.* = Old.*`, tok ==
 * NULL) name no single method and are skipped. */
static void collect_symbols(ast *prog) {
	g_ndefsyms = 0;
	g_nmethodsyms = 0;
	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		if ((s->kind == AST_ASSIGN || s->kind == AST_EAGER) && s->tok) {
			if (g_ndefsyms < FB_MAX_DEFSYMS) {
				g_defsyms[g_ndefsyms].name = s->tok;
				g_defsyms[g_ndefsyms].site = s;
				g_ndefsyms++;
			}
		} else if (s->kind == AST_METHODDEF && s->tok) {
			if (g_nmethodsyms < FB_MAX_METHODSYMS) {
				g_methodsyms[g_nmethodsyms].class_name  = s->kids[0]->tok;
				g_methodsyms[g_nmethodsyms].method_name = s->tok;
				g_methodsyms[g_nmethodsyms].site = s;
				g_nmethodsyms++;
			}
		} else if (s->kind == AST_METHODCOPY && s->tok) {   /* NULL tok = wildcard, skip */
			if (g_nmethodsyms < FB_MAX_METHODSYMS) {
				g_methodsyms[g_nmethodsyms].class_name  = s->kids[0]->tok;
				g_methodsyms[g_nmethodsyms].method_name = s->tok;
				g_methodsyms[g_nmethodsyms].site = s;
				g_nmethodsyms++;
			}
		}
	}
}

/* ---- scope-threading walk (locals: lambda params / where-bindings) ------ */

static tbind *scope_find(scope *s, token *name) {
	for (; s; s = s->parent)
		for (tbind *b = s->binds; b; b = b->next)
			if (tok_eq(b->name, name))
				return b;
	return NULL;
}

/* Recurse from `n` down to `target`, threading the same scope-introducing
 * rules infer() uses (AST_LAMBDA binds params, AST_GUARD's `where` binds),
 * and return the scope in effect at `target`. Unlike infer(), the scope
 * frames here must outlive this call (the caller reads them afterward), so
 * they're arena-allocated rather than stack-allocated like infer()'s. Only
 * structural scope shape matters -- bound types are placeholders (RET_VAR),
 * never read by callers of this walk. */
static scope *locate_scope(ast *n, ast *target, scope *sc) {
	if (!n) return NULL;
	if (n == target) return sc;
	switch (n->kind) {
	case AST_LAMBDA: {
		scope *inner = a_alloc(sizeof *inner);
		inner->parent = sc;
		bind_params(inner, n->kids[0], t_new(RET_VAR), 0);
		scope *hit = locate_scope(n->kids[0], target, sc);   /* params themselves: outer scope */
		if (hit) return hit;
		return locate_scope(n->kids[1], target, inner);
	}
	case AST_GUARD: {
		scope *gs = a_alloc(sizeof *gs);
		gs->parent = sc;
		gs->arg = sc ? sc->arg : NULL;
		ast *where = n->kids[1];
		ast *whenc = n->kids[2];
		if (where)
			for (size_t i = 0; i < where->nkids; i++) {
				ast *bd = where->kids[i];   /* AST_BINDING */
				scope_put(gs, bd->tok, t_new(RET_VAR));
			}
		scope *bsc = where ? gs : sc;
		if (where) {
			scope *hit = locate_scope(where, target, bsc);
			if (hit) return hit;
		}
		if (whenc) {
			scope *hit = locate_scope(whenc, target, bsc);
			if (hit) return hit;
		}
		return locate_scope(n->kids[0], target, bsc);
	}
	default:
		for (size_t i = 0; i < n->nkids; i++) {
			scope *hit = locate_scope(n->kids[i], target, sc);
			if (hit) return hit;
		}
		return NULL;
	}
}

/* ---- go-to-definition ---------------------------------------------------- */

int fb_find_definition(ast *prog, int line, int col, fb_Definition *out, int max_out) {
	if (!prog || !out || max_out <= 0) return 0;
	collect_symbols(prog);

	ast *n = find_node(prog, line, col);
	if (!n) return 0;

	/* base.method_name(...) / base.method_name : resolve by method name
	 * across every class (no receiver-type narrowing in v1 -- the receiver's
	 * static type is RET_VAR for any AST_OBJNEW-produced object, so the
	 * class isn't statically knowable). Ambiguous name -> multiple results. */
	if (n->kind == AST_FIELD && n->tok) {
		int nout = 0;
		for (int i = 0; i < g_nmethodsyms && nout < max_out; i++)
			if (tok_eq(g_methodsyms[i].method_name, n->tok)) {
				out[nout].def_span = ast_span(g_methodsyms[i].site);
				out[nout].kind = FB_DEF_METHOD;
				nout++;
			}
		return nout;
	}

	if (n->kind != AST_ID) return 0;

	scope *sc = locate_scope(prog, n, NULL);
	tbind *b = sc ? scope_find(sc, n->tok) : NULL;
	if (b) {
		out[0].def_span = (fb_Span){
			.start = { b->name->start.line, b->name->start.col },
			.end   = { b->name->end.line,   b->name->end.col },
		};
		out[0].kind = FB_DEF_LOCAL;
		return 1;
	}

	for (int i = 0; i < g_ndefsyms; i++)
		if (tok_eq(g_defsyms[i].name, n->tok)) {
			out[0].def_span = ast_span(g_defsyms[i].site);
			out[0].kind = FB_DEF_TOPLEVEL;
			return 1;
		}

	return 0;   /* builtin (no site in this file) or genuinely unresolved */
}

/* ---- hover ---------------------------------------------------------------
 * node->type is already populated by infer_program_collect()/infer_program()
 * (every visited node gets one, walk.c:~930) -- hover just has to find the
 * node and print it. */

static void obj_type_sprint_into(obj_type *t, char *buf, size_t bufsz, size_t *pos) {
#define APPEND(...) do { \
		if (*pos < bufsz) { \
			int _w = snprintf(buf + *pos, bufsz - *pos, __VA_ARGS__); \
			if (_w > 0) *pos += ((size_t)_w < bufsz - *pos) ? (size_t)_w : bufsz - *pos; \
		} \
	} while (0)
	if (!t) { APPEND("?"); return; }
	switch (t->ret) {
	case RET_NONE:    APPEND("None");    break;
	case RET_INT:     APPEND("Int");     break;
	case RET_FLOAT:   APPEND("Float");   break;
	case RET_STRING:  APPEND("String");  break;
	case RET_BOOL:    APPEND("Bool");    break;
	case RET_FILE:    APPEND("File");    break;
	case RET_COMMAND: APPEND("Command"); break;
	case RET_VAR:     APPEND("%s", t->var_label ? t->var_label : "?"); break;
	case RET_OBJECT:
		if (t->class_name) APPEND("%.*s", (int)t->class_name->len, t->class_name->string);
		else                APPEND("Object");
		break;
	case RET_LIST:
		if (t->is_variadic) {
			APPEND("*");
			obj_type_sprint_into(t->nchildren ? t->children[0] : NULL, buf, bufsz, pos);
		} else {
			APPEND("List<");
			obj_type_sprint_into(t->nchildren ? t->children[0] : NULL, buf, bufsz, pos);
			APPEND(">");
		}
		break;
	case RET_TUPLE:
		APPEND("Tuple<");
		for (size_t i = 0; i < t->nchildren; i++) {
			if (i) APPEND(", ");
			obj_type_sprint_into(t->children[i], buf, bufsz, pos);
		}
		APPEND(">");
		break;
	case RET_FUNCTOR: {
		APPEND("(");
		/* see obj_type_print's identical case for why: flatten a multi-param
		 * functor's Tuple<...> param slot into "a, b, c -> r". */
		obj_type *p = t->nchildren > 0 ? t->children[0] : NULL;
		if (p && p->ret == RET_TUPLE) {
			for (size_t i = 0; i < p->nchildren; i++) {
				if (i) APPEND(", ");
				obj_type_sprint_into(p->children[i], buf, bufsz, pos);
			}
		} else {
			obj_type_sprint_into(p, buf, bufsz, pos);
		}
		APPEND(" -> ");
		obj_type_sprint_into(t->nchildren > 1 ? t->children[1] : NULL, buf, bufsz, pos);
		APPEND(")");
		break;
	}
	}
#undef APPEND
}

char *obj_type_sprint(obj_type *t) {
	char buf[512];
	size_t pos = 0;
	obj_type_sprint_into(t, buf, sizeof buf, &pos);
	return a_dup(buf);
}

/* top-level `NAME = params -> body` (or ~=) whose RHS is a lambda, or NULL.
 * Used to re-specialize a call's result type against its actual argument
 * types, since infer_id's cached type for a standalone def always used an
 * unknown (RET_VAR) param -- there's no single call site to inform it when
 * it's inferred context-free. */
static ast *find_toplevel_lambda(ast *prog, token *name) {
	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		if ((s->kind == AST_ASSIGN || s->kind == AST_EAGER) && s->tok
		    && tok_eq(s->tok, name) && s->kids[0]->kind == AST_LAMBDA)
			return s->kids[0];
	}
	return NULL;
}

static int ast_contains(ast *n, const ast *target) {
	if (!n) return 0;
	if (n == target) return 1;
	for (size_t i = 0; i < n->nkids; i++)
		if (ast_contains(n->kids[i], target)) return 1;
	return 0;
}

/* which top-level statement's subtree `target` lives in, or NULL */
static ast *find_enclosing_toplevel(ast *prog, const ast *target) {
	for (size_t i = 0; i < prog->nkids; i++)
		if (ast_contains(prog->kids[i], target)) return prog->kids[i];
	return NULL;
}

int fb_hover_at(ast *prog, int line, int col, fb_HoverInfo *out) {
	if (!prog || !out) return 0;
	ast *n = find_node(prog, line, col);
	if (!n) return 0;
	out->span = ast_span(n);

	/* A registered name (builtin or FB_GLOBAL-bound plugin value, e.g. the
	 * "math" namespace) with an optional doc string takes priority over the
	 * bare inferred type -- these are frequently RET_VAR/"?" (no static
	 * shape tracked), so a doc string is the only useful thing to show. */
	const fb_Reg *rg = (n->kind == AST_ID && n->tok)
	                  ? fb_reg_findn(n->tok->string, n->tok->len) : NULL;
	if (rg && rg->doc) {
		out->text = a_dup(rg->doc);
		return 1;
	}

	/* Re-infer the WHOLE enclosing top-level statement's RHS from scratch,
	 * with call-site specialization on (g_specialize_calls, see infer_call):
	 * a call to a named top-level function anywhere in it, at any depth,
	 * resolves against its *real* argument types instead of the def's own
	 * generic/context-free signature.
	 *
	 * This must walk from the enclosing STATEMENT, not from `n` itself --
	 * locate_scope() (used for goto-def) only reconstructs scope *structure*
	 * with placeholder RET_VAR args, since it has no real argument types to
	 * work with; using it here would seed e.g. a pipe-lambda's param with a
	 * placeholder instead of the pipe's real element type, silently
	 * corrupting specialization deeper in (found the hard way: a capture
	 * param downstream of one came out an empty tuple instead of real
	 * types). Re-inferring from the statement's RHS with sc=NULL instead
	 * threads real types through every local binding the normal way (e.g.
	 * AST_PIPE calling infer_apply with the pipe's real element type). Side
	 * effects of that walk (n->type / a call's callee->type / a param
	 * declaration's ->type) land on `n` itself as a natural byproduct of
	 * reaching it, however deep it is -- so just read n->type back after.
	 *
	 * Exception: when `n` IS the enclosing statement (hovering a def's own
	 * LHS name), it's the ASSIGN/EAGER wrapper, never a descendant of its
	 * own RHS -- inferring the RHS can't land on it as a side effect. Write
	 * the result back explicitly, same as infer_program_impl's main-pass
	 * convention (s->type = t) for exactly this node. */
	ast *stmt = find_enclosing_toplevel(prog, n);
	if (stmt) {
		int is_def = stmt->kind == AST_ASSIGN || stmt->kind == AST_EAGER;
		ast *rhs = is_def ? stmt->kids[0] : stmt;
		g_specialize_calls = 1;
		obj_type *t = infer(rhs, NULL);
		if (is_def)
			stmt->type = t;
		g_specialize_calls = 0;
	}
	out->text = obj_type_sprint(n->type);
	return 1;
}
