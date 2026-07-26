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
	[AST_TYPEEXPR] = "AST_TYPEEXPR",
	[AST_TYPEUNION] = "AST_TYPEUNION",

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

/* the canonical "Int or Float, unresolved which" union -- what a bare
 * arithmetic result / FB_RET_NUM used to spell as the standalone RET_NUMERIC
 * scalar, now just the two-member case of the general RET_UNION. */
static obj_type *t_numeric(void) {
	obj_type **k = a_alloc(2 * sizeof *k);
	k[0] = t_new(RET_INT);
	k[1] = t_new(RET_FLOAT);
	return t_compound(RET_UNION, 2, k);
}

static obj_type *t_union(size_t n, obj_type **kids) {
	return t_compound(RET_UNION, n, kids);
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
	case FB_RET_NUM:  return t_numeric();
	case FB_RET_MAP:
	case FB_RET_ANY:
	default:          return t_new(RET_VAR);
	}
}

/* an fb_RetUnion (see fbuild.h) -> the same RET_UNION a language-level
 * A|B|C annotation resolves to; each member goes through fb_ret_type(). */
static obj_type *fb_ret_union_type(const fb_RetUnion *u) {
	obj_type **k = u->nmembers ? a_alloc(u->nmembers * sizeof *k) : NULL;
	for (int i = 0; i < u->nmembers; i++)
		k[i] = fb_ret_type(u->members[i]);
	return t_union((size_t)u->nmembers, k);
}

/* a `token` naming a native class (e.g. "Stream"), for obj_type.class_name
 * -- `name` is a stable string (a plugin's own string literal, living for
 * the process), so no need to a_dup the text itself, just the token struct
 * that points at it (this one DOES need arena storage: unlike the various
 * short-lived synthetic tokens elsewhere in this file, used only within one
 * function call, an obj_type.class_name is often cached on a long-lived
 * ast->type -- a stack token here would dangle). */
static token *native_class_token(const char *name) {
	token *t = a_alloc(sizeof *t);
	*t = (token){0};
	t->string = (char *)name;
	t->len = strlen(name);
	return t;
}

/* an instance-of-native-class type, e.g. "Pipeline" for a Pipeline param/
 * return slot -- shared by fb_reg_ret_type (ret_class) and builtin_signature
 * (param_classes[i]). */
static obj_type *t_native_class(const char *name) {
	obj_type *t = t_new(RET_OBJECT);
	t->class_name = native_class_token(name);
	return t;
}

/* a registered function/method's return type -- `ret_union` (an arbitrary
 * A|B|C union) takes priority over both `ret_class` and `ret`; `ret_class`
 * (an instance of a native class, e.g. stream()'s "Stream") takes priority
 * over plain `ret` when set, same as fbuild.h documents. */
static obj_type *fb_reg_ret_type(const fb_Reg *rg) {
	if (rg->ret_union)
		return fb_ret_union_type(rg->ret_union);
	if (rg->ret_class)
		return t_native_class(rg->ret_class);
	return fb_ret_type(rg->ret);
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

static obj_type *record_field(obj_type *rec, token *name);

/* true for a concrete Int/Float, or a union whose every member is
 * (recursively) numericish, e.g. the {Int,Float} union t_numeric() builds.
 * A union with a non-numeric member (Int|String) is NOT numericish -- it
 * can't stand in for an arithmetic operand regardless of which alternative
 * a given value happens to be. */
static int is_numericish(obj_type *t) {
	if (!t)
		return 0;
	if (t->ret == RET_INT || t->ret == RET_FLOAT)
		return 1;
	if (t->ret == RET_UNION) {
		if (t->nchildren == 0)
			return 0;
		for (size_t i = 0; i < t->nchildren; i++)
			if (!is_numericish(t->children[i]))
				return 0;
		return 1;
	}
	return 0;
}

/* structural compatibility; RET_VAR (unknown) unifies with anything.
 * RET_UNION unifies with a concrete type iff any member does, and with
 * another union iff any pair of members does (the recursive call on a
 * RET_UNION `o` re-enters this same branch from the other side, so both
 * directions get checked without a separate union-vs-union case).
 * RET_OBJECT: two objects with DIFFERENT known class_name never unify,
 * regardless of field-shape tracking -- a Buffer is never a Shader even if
 * neither has a tracked shape (native ret_class/param-class types never do,
 * see ast.h). Same class_name (or either side missing one, which shouldn't
 * happen for a real RET_OBJECT but is tolerated) falls through to the field
 * check, UNLESS both sides have a known field shape (populated by
 * infer_objnew_schema/infer_parser_schema/rec_define) -- an unshaped object
 * (spread of something dynamic, or a non-map fields body) still unifies
 * with anything of the same class to avoid false positives, same as before
 * this field tracking existed (see ast.h). Fields are compared by name, not
 * position: map/spread field order isn't semantically meaningful, unlike a
 * Tuple's. */
static int unify(obj_type *a, obj_type *b) {
	if (!a || !b)
		return 1;
	if (a->ret == RET_VAR || b->ret == RET_VAR)
		return 1;
	if (a->ret == RET_UNION || b->ret == RET_UNION) {
		obj_type *u = a->ret == RET_UNION ? a : b;
		obj_type *o = a->ret == RET_UNION ? b : a;
		for (size_t i = 0; i < u->nchildren; i++)
			if (unify(u->children[i], o))
				return 1;
		return 0;
	}
	if (a->ret == RET_OBJECT || b->ret == RET_OBJECT) {
		if (a->ret != b->ret)
			return 1;
		if (a->class_name && b->class_name && !tok_eq(a->class_name, b->class_name))
			return 0;
		if (!a->fields || !b->fields)
			return 1;
		for (size_t i = 0; i < a->nchildren; i++) {
			obj_type *bf = record_field(b, a->fields[i]);
			if (bf && !unify(a->children[i], bf))
				return 0;
		}
		return 1;
	}
	/* Int and Float mutually unify even as two concrete (non-union) types --
	 * long-standing leniency predating RET_UNION (was "is_numericish(a) &&
	 * is_numericish(b)", back when RET_NUMERIC was numericish too; the union
	 * branch above already covers anything union-shaped, so this only still
	 * needs to handle the plain-Int-vs-plain-Float pair). */
	if ((a->ret == RET_INT || a->ret == RET_FLOAT) && (b->ret == RET_INT || b->ret == RET_FLOAT))
		return 1;
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
static const char *ret_kind_name(obj_type *t);

static void rec_define(obj_type *rec, token *name, obj_type *ft, token *where) {
	obj_type *cur = record_field(rec, name);
	if (cur) {
		if (!unify(cur, ft))
			type_err(where, "field '%.*s' is %s, cannot set to %s",
			         (int)name->len, name->string,
			         ret_kind_name(cur), ret_kind_name(ft));
	} else {
		rec_push(rec, name, ft);
	}
}

/* short kind name for error messages; a union joins its members' own short
 * names with "|" (e.g. "Int|Float", "Int|String|Point"), arena-allocated via
 * a_dup since it's built at call time instead of being a string literal. */
static const char *ret_kind_name(obj_type *t) {
	if (!t)
		return "?";
	switch (t->ret) {
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
	case RET_OBJECT:
		return t->class_name ? a_dupn(t->class_name->string, t->class_name->len) : "Object";
	case RET_VAR:     return "?";
	case RET_UNION: {
		char buf[256];
		size_t pos = 0;
		for (size_t i = 0; i < t->nchildren; i++) {
			const char *m = ret_kind_name(t->children[i]);
			size_t mlen = strlen(m);
			if (i && pos < sizeof buf - 1)
				buf[pos++] = '|';
			if (pos + mlen >= sizeof buf)
				mlen = sizeof buf - 1 - pos;
			memcpy(buf + pos, m, mlen);
			pos += mlen;
		}
		buf[pos] = 0;
		return a_dup(buf);
	}
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
static obj_type *resolve_type_expr(ast *te);
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
		if (rg && (rg->flags & FB_GLOBAL))   /* bound global value: type from `ret`/`ret_union` */
			return rg->ret_union ? fb_ret_union_type(rg->ret_union) : fb_ret_type(rg->ret);
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

/* Best available functor signature for builtin NAME, or NULL if NAME is
 * neither curated (core_generic_signature) nor known-arity (g_core_arity /
 * the fb_reg registry) -- i.e. genuinely unknown. `result_type` is the
 * return type to use when only arity is known (a real call site's own
 * inferred result, or NULL when there's no call to specialize from, e.g.
 * bare-name completion/hover with no arguments -- falls back to RET_VAR).
 * Order of preference: curated structural signature > registry/table
 * arity-shaped functor > NULL. Shared by infer_call's callee-annotation
 * wrapper and completion's builtin tier, so both stay in sync. */
static obj_type *builtin_signature(token *name, obj_type *result_type) {
	obj_type *generic = core_generic_signature(name);
	if (generic) return generic;

	int mn = -1, mx = -1;
	const fb_Reg *rg = NULL;
	for (size_t i = 0; i < N_CORE_ARITY; i++)
		if (tok_is(name, g_core_arity[i].name)) {
			mn = g_core_arity[i].min; mx = g_core_arity[i].max;
			break;
		}
	if (mn < 0) {
		rg = fb_reg_findn(name->string, name->len);
		if (rg) { mn = rg->min; mx = rg->max; }
	}
	if (mn < 0) return NULL;

	/* slot `i`: rg's own declared param_type_unions[i] if set (a real A|B|C
	 * union), else an fb_reg_param_class()-declared native class for this
	 * slot (e.g. a Pipeline/Buffer param -- kept OUTSIDE fb_Reg, see
	 * fbuild.h), else param_types[i], else the generic "?" every slot used
	 * to be unconditionally. g_core_arity-only names (rg stays NULL) always
	 * fall to "?" -- core builtins aren't fb_Reg-registered, so there's
	 * nothing to type them from here. */
#define PSLOT(i) ((rg && rg->param_type_unions && (int)(i) < rg->nparams \
                   && rg->param_type_unions[i]) \
                  ? fb_ret_union_type(rg->param_type_unions[i]) \
                  : (rg && fb_reg_param_class_at(rg->name, (int)(i))) \
                  ? t_native_class(fb_reg_param_class_at(rg->name, (int)(i))) \
                  : (rg && rg->param_types && (int)(i) < rg->nparams) \
                  ? fb_ret_type(rg->param_types[i]) : t_new(RET_VAR))

	obj_type *param;
	if (mx < 0) {
		/* variadic (max<0): `mn` fixed slots (none if mn==0) followed by a
		 * trailing "*T" absorbing the rest -- same "*" a capture param's own
		 * generic slot uses (infer_param_type_from_usage). The variadic
		 * tail's element type reuses the last typed fixed slot (e.g.
		 * math.min/max: one-or-more, all Int|Float) when available. */
		if (mn == 0) {
			param = t_variadic(PSLOT(0));
		} else {
			obj_type **k = a_alloc(((size_t)mn + 1) * sizeof *k);
			for (int j = 0; j < mn; j++) k[j] = PSLOT(j);
			k[mn] = t_variadic(PSLOT(mn - 1));
			param = t_compound(RET_TUPLE, (size_t)mn + 1, k);
		}
	} else {
		size_t n = (size_t)mx;
		if (n <= 1) {
			param = PSLOT(0);
		} else {
			obj_type **k = a_alloc(n * sizeof *k);
			for (size_t j = 0; j < n; j++) k[j] = PSLOT(j);
			param = t_compound(RET_TUPLE, n, k);
		}
	}
#undef PSLOT
	return t_functor(param, result_type ? result_type : t_new(RET_VAR));
}

/* math.sqrt-style native method: `field` is an AST_FIELD whose base is a
 * plain identifier (a plugin namespace object, e.g. `math`) and whose own
 * name is one of that class's fb_method()s. fb_method() shadow-registers a
 * "ClassName.method" entry in the flat fb_Reg registry specifically for
 * this lookup (see run.c) -- reuses builtin_signature()'s existing generic-
 * signature builder (arity-shaped "?" params, real return type) rather than
 * a new formatter. NULL if base isn't a plain id or no such shadow entry
 * exists (an ordinary field access, or a method-less/unregistered plugin). */
static char *dotted_key(token *a, token *b, size_t *outlen) {
	size_t len = a->len + 1 + b->len;
	char *buf = a_alloc(len + 1);
	snprintf(buf, len + 1, "%.*s.%.*s", (int)a->len, a->string, (int)b->len, b->string);
	*outlen = len;
	return buf;
}

/* "a.b" registry lookup -- fb_method's shadow "ClassName.method" entries
 * (see run.c), the SAME string built two ways depending on the caller:
 * either a bound global's own identifier text (math.sqrt: `a` = the "math"
 * token, no static shape tracked, base->ret stays RET_VAR) or a value's
 * actual class name (a Stream instance: `a` = its class_name token, RET_
 * OBJECT). Shared by native_method_type, class_native_method_type below,
 * and fb_hover_at's doc lookup. */
static obj_type *dotted_reg_type(token *a, token *b) {
	size_t len; char *buf = dotted_key(a, b, &len);
	const fb_Reg *rg = fb_reg_findn(buf, len);
	if (!rg) return NULL;
	token synth = *b; synth.string = buf; synth.len = len;
	obj_type *sig = builtin_signature(&synth, fb_reg_ret_type(rg));
	return sig ? sig : fb_reg_ret_type(rg);
}

static obj_type *native_method_type(ast *field) {
	if (field->kind != AST_FIELD || !field->tok
	 || field->kids[0]->kind != AST_ID || !field->kids[0]->tok)
		return NULL;
	return dotted_reg_type(field->kids[0]->tok, field->tok);
}

/* does native class `class_name` (e.g. "Stream") have a method
 * `method_name`, per an fb_method()-registered shadow entry? Parallels
 * class_has_method (language-level `type`/`method` classes) for a native
 * one -- see AST_FIELD's is_method check. */
static int class_has_native_method(token *class_name, token *method_name) {
	size_t len; char *buf = dotted_key(class_name, method_name, &len);
	return fb_reg_findn(buf, len) != NULL;
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
					         i + 1, ret_kind_name(ai));
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
			return fb_reg_ret_type(rg);
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
		/* na==1: children[0] IS the one param's type. na>=2: children[0] is
		 * a RET_TUPLE, one slot per param (see declared_or_guessed_param_type
		 * in the AST_LAMBDA case) -- check whichever of the call's own
		 * arguments this function already has types for (a0/a1; args past
		 * the 2nd are only visited for their own internal errors above, not
		 * kept around as a value here, so a 3+-arg call's 3rd+ arguments
		 * aren't checked against an annotation -- a real gap, not attempted
		 * here). */
		token *where = callee->kind == AST_ID ? callee->tok : NULL;
		if (na == 1) {
			if (!unify(ft->children[0], a0))
				type_err(where, "argument is %s, function expects %s",
				         ret_kind_name(a0),
				         ret_kind_name(ft->children[0]));
		} else if (na >= 2 && ft->children[0] && ft->children[0]->ret == RET_TUPLE) {
			obj_type *p0 = positional(ft->children[0], 0);
			obj_type *p1 = positional(ft->children[0], 1);
			if (!unify(p0, a0))
				type_err(where, "argument 1 is %s, function expects %s",
				         ret_kind_name(a0), ret_kind_name(p0));
			if (!unify(p1, a1))
				type_err(where, "argument 2 is %s, function expects %s",
				         ret_kind_name(a1), ret_kind_name(p1));
		}
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
		obj_type *sig = builtin_signature(callee->tok, result);
		if (sig) callee->type = sig;
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

/* infer_param_type_from_usage, but a `: Type` annotation (AST_ID param
 * only) wins outright over the usage guess. Used wherever a standalone
 * lambda's own PARAM type is computed for its functor signature (not just
 * the body's internal scope binding, which bind_params_lam already handles
 * separately) -- callers that skip this and use infer_param_type_from_usage
 * directly would show/report an annotated param as "?" in the def's own
 * cached signature, which is exactly what infer_id() and every real call
 * site's argument check reads, so the annotation would never actually be
 * enforced anywhere. */
static obj_type *declared_or_guessed_param_type(ast *p, ast *body) {
	if (p->kind == AST_ID && p->nkids)
		return resolve_type_expr(p->kids[0]);
	return infer_param_type_from_usage(p, body);
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
				/* `: Type` annotation (AST_ID param only, see
				 * parse_parameter's allow_type): check the real argument
				 * against it -- catches a mismatch at every real call site,
				 * since infer_apply_lam's direct-lambda fast path funnels
				 * here with the call's actual arg type as `pt`; harmless
				 * during a def's own generic pass, where `pt` is RET_VAR-
				 * ish and unifies with anything -- then bind to the
				 * DECLARED type, stronger than `pt` for checking the body. */
				if (p->kind == AST_ID && p->nkids) {
					obj_type *decl_t = resolve_type_expr(p->kids[0]);
					if (!unify(decl_t, pt))
						type_err(p->tok, "parameter '%.*s' declared %s, got %s",
						         (int)p->tok->len, p->tok->string,
						         ret_kind_name(decl_t), ret_kind_name(pt));
					pt = decl_t;
				}
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
			 * record_field() resolves a real type when `pt` is a RET_FILE
			 * record or a known-shape RET_OBJECT (infer_objnew_schema); a
			 * dynamic Map or unshaped object falls back to RET_VAR here,
			 * same permissive behavior as an ordinary named field access. */
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
			         ret_kind_name(arg));
		return t_file();
	}

	obj_type *ft = infer(fn, sc);
	if (ft && ft->ret == RET_FUNCTOR && ft->nchildren > 1) {
		if (!unify(ft->children[0], arg))
			type_err(NULL, "function expects %s, got %s",
			         ret_kind_name(ft->children[0]),
			         ret_kind_name(arg));
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

/* ---- symbol tables (top-level defs, methods) ----------------------------
 * Declared here (ahead of infer()'s AST_FIELD case, which needs
 * class_has_method) -- collect_symbols() itself is defined further down,
 * near the other source-position/LSP tooling it's shared with. */

typedef struct { token *name; ast *site; } fb_DefSym;
typedef struct { token *class_name, *method_name; ast *site; } fb_MethodSym;

#define FB_MAX_DEFSYMS    4096
#define FB_MAX_METHODSYMS 4096
static fb_DefSym    g_defsyms[FB_MAX_DEFSYMS];
static int          g_ndefsyms;
static fb_MethodSym g_methodsyms[FB_MAX_METHODSYMS];
static int          g_nmethodsyms;

static void collect_symbols(ast *prog);

/* Does class `class_name` have a method `method_name`, per the last
 * collect_symbols() pass? Methods live in a separate namespace from fields
 * (ast.h explicitly leaves method shapes unchecked), so this only answers
 * "is this identifier a method" -- no arity/return-type checking, same as
 * before object field shapes were tracked. */
static int class_has_method(token *class_name, token *method_name) {
	if (!class_name) return 0;
	for (int i = 0; i < g_nmethodsyms; i++)
		if (tok_eq(g_methodsyms[i].class_name, class_name)
		 && tok_eq(g_methodsyms[i].method_name, method_name))
			return 1;
	return 0;
}

/* Best-effort hover/signature type for a known method (class_has_method
 * already true) -- direct `method Class.name = lambda` defs resolve to that
 * lambda's own already-inferred type (kids[1]->type, set when its
 * AST_METHODDEF statement was itself visited by infer_program_impl's
 * top-level loop -- NULL if that hasn't happened yet, e.g. a forward
 * reference, or if the method is only known via a copy (AST_METHODCOPY
 * site has no lambda of its own to point at -- tracing through a copy/
 * wildcard chain to the original definition is unsupported today; caller
 * falls back to RET_VAR "?" same as before this existed). */
static obj_type *class_method_type(token *class_name, token *method_name) {
	for (int i = 0; i < g_nmethodsyms; i++)
		if (tok_eq(g_methodsyms[i].class_name, class_name)
		 && tok_eq(g_methodsyms[i].method_name, method_name)) {
			ast *site = g_methodsyms[i].site;
			if (site->kind == AST_METHODDEF && site->nkids > 1)
				return site->kids[1]->type;
			return NULL;
		}
	return NULL;
}

/* Generic instance type of `class_name` (the class's own constructor lambda,
 * `type NAME(...) = { ... }`, inferred with unspecialized/RET_VAR params --
 * same shape self would have for ANY instance, since a method's receiver
 * isn't tied to one particular call site). NULL if the class isn't a known
 * top-level `type` def, or its own top-level statement hasn't been inferred
 * yet (a method def forward-referencing its class, ahead of the `type`
 * statement -- unusual, but not a language error, so just fall back to the
 * old unhinted guess rather than erroring). */
static obj_type *method_self_type(token *class_name) {
	ast *ctor = find_toplevel_lambda(g_program, class_name);
	if (!ctor || ctor->kids[1]->kind != AST_OBJNEW)
		return NULL;
	return ctor->kids[1]->type;
}

/* Resolve a parsed `: Type` param annotation (AST_TYPEEXPR/AST_TYPEUNION,
 * see build_system.c's parse_type_expr) to a checker type. Scalars map
 * directly; List<T>/Tuple<T,...> recurse one level (matches the parser,
 * which only accepts these two generics); any other name is looked up as a
 * `type NAME(...)` class -- same lookup as method_self_type, returning
 * RET_VAR (not an error) if the class exists but hasn't been inferred yet
 * (forward reference, tolerated same as everywhere else in this checker).
 * A name that's neither a scalar keyword nor any known top-level def is a
 * real type_err, falling back to RET_VAR so the rest of inference proceeds.
 * A|B|C (AST_TYPEUNION) resolves each member and builds a RET_UNION --
 * members are never themselves unions (the parser already flattens). */
static obj_type *resolve_type_expr_uncached(ast *te) {
	if (te->kind == AST_TYPEUNION) {
		obj_type **k = te->nkids ? a_alloc(te->nkids * sizeof *k) : NULL;
		for (size_t i = 0; i < te->nkids; i++)
			k[i] = resolve_type_expr(te->kids[i]);
		return t_union(te->nkids, k);
	}
	token *name = te->tok;
	if (te->nkids == 0) {
		if (tok_is(name, "Int"))     return t_new(RET_INT);
		if (tok_is(name, "Float"))   return t_new(RET_FLOAT);
		if (tok_is(name, "String"))  return t_new(RET_STRING);
		if (tok_is(name, "Bool"))    return t_new(RET_BOOL);
		if (tok_is(name, "Path"))    return t_file();
		if (tok_is(name, "File"))    return t_file();
		if (tok_is(name, "Command")) return t_new(RET_COMMAND);
		if (tok_is(name, "Unit"))    return t_new(RET_NONE);
		ast *cls = find_toplevel_lambda(g_program, name);
		if (cls)
			return (cls->kids[1]->kind == AST_OBJNEW && cls->kids[1]->type)
			     ? cls->kids[1]->type : t_new(RET_VAR);
		type_err(name, "unknown type '%.*s'", (int)name->len, name->string);
		return t_new(RET_VAR);
	}
	if (tok_is(name, "List") && te->nkids == 1)
		return t_list(resolve_type_expr(te->kids[0]));
	if (tok_is(name, "Tuple") && te->nkids >= 1) {
		obj_type **k = a_alloc(te->nkids * sizeof *k);
		for (size_t i = 0; i < te->nkids; i++)
			k[i] = resolve_type_expr(te->kids[i]);
		return t_compound(RET_TUPLE, te->nkids, k);
	}
	type_err(name, "unknown generic type '%.*s<...>'", (int)name->len, name->string);
	return t_new(RET_VAR);
}

/* memoized on the AST_TYPEEXPR node's own ->type -- a param's annotation is
 * resolved twice per pass (once for the def's own functor signature via
 * declared_or_guessed_param_type, once more inside bind_params_lam's
 * per-call check), and without this an unknown-type typo would report
 * twice for the same annotation. */
static obj_type *resolve_type_expr(ast *te) {
	if (te->type)
		return te->type;
	return te->type = resolve_type_expr_uncached(te);
}

/* type NAME(...) = { ... } : build a field-typed record from a statically-
 * shaped fields body, same idioms as run.c's objnew_keys_static (literal,
 * non-interpolated string/bareword/shorthand keys) plus AST_SPLAT
 * composition (`{ ...Point(x, y), z }`, see .tests/class.fb) -- the checker
 * cares about static shape, not run.c's runtime-caching fast path, so a
 * spread of a known-shape object/file is fine here even though
 * objnew_keys_static rejects any spread outright. Returns NULL (caller
 * falls back to a bare, unshaped RET_OBJECT) if the body isn't a map
 * literal, a key isn't statically known, or a spread source's own shape
 * isn't tracked -- in every such case there's no way to be sure we've seen
 * every field, so an incomplete schema would just cause false positives. */
static obj_type *infer_objnew_schema(ast *body, scope *sc) {
	if (body->kind != AST_MAPLIT)
		return NULL;
	obj_type *rec = t_new(RET_OBJECT);
	for (size_t i = 0; i < body->nkids; i++) {
		ast *entry = body->kids[i];
		if (entry->kind == AST_SPLAT) {
			obj_type *src = infer(entry->kids[0], sc);
			if ((src->ret != RET_OBJECT && src->ret != RET_FILE) || !src->fields)
				return NULL;
			for (size_t j = 0; j < src->nchildren; j++)
				rec_define(rec, src->fields[j], src->children[j], entry->kids[0]->tok);
			continue;
		}
		if (entry->kind != AST_TUPLE)
			return NULL;
		ast *key = entry->kids[0], *val = entry->kids[1];
		if (key->kind != AST_STRING || key->nkids != 0 || !key->tok)
			return NULL;
		rec_define(rec, key->tok, infer(val, sc), key->tok);
	}
	return rec;
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

	case AST_OBJNEW: {                               /* object, field-typed if body is a static map */
		infer(node->kids[0], sc);                    /* check the fields body    */
		obj_type *rec = infer_objnew_schema(node->kids[0], sc);
		t = rec ? rec : t_new(RET_OBJECT);
		t->class_name = node->tok;                   /* class name, e.g. "Point" */
		break;
	}

	case AST_METHODDEF: {                            /* method Class.name = fn   */
		/* kids[0] is the class name id, kids[1] the lambda (self -> body, or
		 * a multi-param destructure-by-field-name form -- see SYNTAX.md).
		 * Single-param form: self IS the whole receiver -- bind_params_lam
		 * already knows how to bind a single param to a whole `arg`, same
		 * as an ordinary call's self-argument would be. Multi-param form:
		 * each param is bound BY NAME from self's fields (run.c's actual
		 * dispatch rule, SYNTAX.md), not positionally like an ordinary
		 * multi-param lambda -- record_field() per param, a field with no
		 * match typed Unit (matches run.c: "no matching field gets ()"),
		 * only when self's own shape is known (otherwise "?", same as any
		 * other unshaped-object field access). */
		ast *lam = node->kids[1];
		obj_type *resolved = method_self_type(node->kids[0]->tok);
		/* node->kids[0] is the class-id token (`Point` in `method Point.repr
		 * = ...`) -- annotate it too, so hovering the CLASS name shows the
		 * receiver shape (same value `self` is bound to below), not just
		 * the method name. Falls back to a bare class_name-only type (still
		 * names the class for hover, see obj_type_print) when the class's
		 * own fields aren't statically known -- same fallback AST_OBJNEW
		 * itself uses. */
		if (resolved) {
			node->kids[0]->type = resolved;
		} else {
			obj_type *bare = t_new(RET_OBJECT);
			bare->class_name = node->kids[0]->tok;
			node->kids[0]->type = bare;
		}
		obj_type *self_t = (lam->kind == AST_LAMBDA && lam->kids[0]->nkids >= 1)
		                  ? resolved : NULL;
		if (self_t) {
			ast *plist = lam->kids[0];
			scope inner = { .parent = sc };
			obj_type *param_t;
			if (plist->nkids == 1) {
				bind_params_lam(&inner, plist, self_t, NULL, 1);
				param_t = self_t;
			} else {
				inner.arg = self_t;
				size_t P = plist->nkids;
				obj_type **k = a_alloc(P * sizeof *k);
				for (size_t i = 0; i < P; i++) {
					ast *p = plist->kids[i];
					token *name = param_name_tok(p);
					obj_type *bound = t_new(RET_VAR);
					if (name) {
						obj_type *ft = self_t->fields ? record_field(self_t, name) : NULL;
						bound = ft ? ft : self_t->fields ? t_new(RET_NONE) : t_new(RET_VAR);
						scope_put(&inner, name, bound);
						p->type = bound;   /* declaration site, for hover */
					}
					k[i] = bound;
				}
				param_t = t_compound(RET_TUPLE, P, k);
			}
			lam->type = t_functor(param_t, infer(lam->kids[1], &inner));
		} else {
			infer(lam, sc);
		}
		/* node->tok is the method's own name (`method Class.name = ...`) --
		 * take the lambda's just-computed signature as this statement's
		 * own type too (same reasoning as infer_program_impl's `s->type = t`
		 * for a top-level def), so hovering the method NAME itself shows
		 * its real signature instead of a bare "None". */
		t = lam->type;
		break;
	}

	case AST_METHODCOPY:            /* method New.m = Old.m2 / New.* = Old.*   */
		t = t_new(RET_NONE);        /* kids are class/method name ids, not     */
		break;                      /* variables -- nothing to infer           */

	case AST_COMPARE: {
		obj_type *l = infer(node->kids[0], sc);
		obj_type *r = infer(node->kids[1], sc);
		if (!unify(l, r))
			type_err(node->tok, "cannot compare %s with %s",
			         ret_kind_name(l), ret_kind_name(r));
		t = t_new(RET_BOOL);
		break;
	}

	case AST_ARITH: {     /* + - * / % : (Int|Float) x (Int|Float) -> Int|Float */
		obj_type *l = infer(node->kids[0], sc);
		obj_type *r = infer(node->kids[1], sc);
		if (!is_numericish(l) && l->ret != RET_VAR)
			type_err(node->tok, "arithmetic operand must be Int or Float, got %s",
			         ret_kind_name(l));
		if (!is_numericish(r) && r->ret != RET_VAR)
			type_err(node->tok, "arithmetic operand must be Int or Float, got %s",
			         ret_kind_name(r));
		/* an unresolved {Int,Float} union on either side (see t_numeric())
		 * keeps the result that same union -- collapsing it to RET_INT here
		 * would be a silent, wrong guess (it might be Float at runtime). */
		obj_type *rt = (l->ret == RET_FLOAT || r->ret == RET_FLOAT) ? t_new(RET_FLOAT)
		             : (l->ret == RET_UNION || r->ret == RET_UNION) ? t_numeric()
		             : t_new(RET_INT);
		t = rt;
		break;
	}

	case AST_NEG: {                      /* unary "-" : Int|Float -> Int|Float */
		obj_type *o = infer(node->kids[0], sc);
		if (!is_numericish(o) && o->ret != RET_VAR)
			type_err(node->tok, "'-' operand must be numeric, got %s",
			         ret_kind_name(o));
		t = o->ret == RET_FLOAT ? t_new(RET_FLOAT) : o->ret == RET_UNION ? t_numeric() : t_new(RET_INT);
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
			param = P == 1 ? declared_or_guessed_param_type(plist->kids[0], node->kids[1])
			               : t_new(RET_VAR);
		} else {
			/* one slot per param (not a bare "?") so a multi-param
			 * standalone lambda's own signature shows its arity, e.g.
			 * "(?, ?, ? -> ?)" instead of collapsing to "(? -> ?)". */
			obj_type **k = a_alloc(P * sizeof *k);
			for (size_t i = 0; i < P; i++)
				k[i] = declared_or_guessed_param_type(plist->kids[i], node->kids[1]);
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
			int is_method = base && base->ret == RET_OBJECT && base->class_name
			              && (class_has_method(base->class_name, node->tok)
			                  || class_has_native_method(base->class_name, node->tok));
			/* a native class (Stream: fb_reg_has_class matches, at least one
			 * "Stream.<method>" shadow entry exists) has a FULLY known,
			 * closed method set. It can still carry plain fb_set() data
			 * fields (e.g. stream()'s "path") though -- a plugin declares
			 * those explicitly via fb_class_field() so the checker can tell
			 * a legitimate field from a real typo (is_native_field below);
			 * anything neither a known method nor a declared field IS a
			 * real error, same strictness as RET_FILE. */
			fb_Ret nf_ret = FB_RET_ANY; const char *nf_ret_class = NULL;
			int is_native_field = !ft && base && base->ret == RET_OBJECT && base->class_name
			                     && fb_native_field_type(tok_str(base->class_name), tok_str(node->tok),
			                                             &nf_ret, &nf_ret_class);
			int native_closed = base && base->ret == RET_OBJECT && base->class_name
			                   && fb_reg_has_class(tok_str(base->class_name));
			/* stay permissive for an object with an unknown field shape
			 * (and not a closed native class), or when the miss is actually
			 * a method (separate namespace, not statically checked -- see
			 * class_has_method/class_has_native_method: a language
			 * `type`/`method` class, or a native one, via fb_method()'s
			 * shadow registry entries), or a declared native field. */
			int permissive_obj = base && base->ret == RET_OBJECT
			                    && ((!base->fields && !native_closed) || is_method || is_native_field);
			if (!ft && base && base->ret != RET_VAR && !permissive_obj)
				type_err(node->tok, "%s has no field '%.*s'",
				         ret_kind_name(base),
				         (int)node->tok->len, node->tok->string);
			if (!ft && is_native_field) {
				fb_Reg synth = { .ret = nf_ret, .ret_class = nf_ret_class };
				ft = fb_reg_ret_type(&synth);
			}
			if (!ft && is_method) {
				ft = class_method_type(base->class_name, node->tok);
				if (!ft) ft = dotted_reg_type(base->class_name, node->tok);
			}
			if (!ft && base && base->ret == RET_VAR)
				ft = native_method_type(node);
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
				         ret_kind_name(fv));
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
		/* fields tracked when the body is a statically-shaped map literal
		 * (infer_objnew_schema) or for a Parser<schema> record
		 * (infer_parser_schema) -- absent for any other/dynamic shape. */
		if (t->fields && t->nchildren) {
			printf("<{ ");
			for (size_t i = 0; i < t->nchildren; i++) {
				if (i) printf(", ");
				if (t->fields[i]) printf("%.*s: ", (int)t->fields[i]->len, t->fields[i]->string);
				obj_type_print(t->children[i]);
			}
			printf(" }>");
		}
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

	case RET_UNION:
		for (size_t i = 0; i < t->nchildren; i++) {
			if (i) printf("|");
			obj_type_print(t->children[i]);
		}
		break;
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
	collect_symbols(prog);   /* populate g_methodsyms so AST_FIELD can tell a
	                          * method-name miss from a real missing field */
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

/* ---- symbol tables (top-level defs, methods) ----------------------------
 * (types/globals declared earlier, near class_has_method -- infer()'s
 * AST_FIELD case needs g_methodsyms before this point in the file) */

/* One flat pass over prog's top-level statements. Class constructors
 * (`type Name(...) = ...`) desugar to plain AST_ASSIGN at parse time so
 * they're already covered by the AST_ASSIGN/AST_EAGER case -- no special
 * handling needed. A wildcard method copy (`method New.* = Old.*`, tok ==
 * NULL) names no single method -- instead of recording it directly, expand
 * it against whatever Old.* already resolved to SO FAR (program order,
 * matching run.c's eager/no-live-rebinding copy semantics), so a later
 * lookup on New sees the same methods Old had at the copy site. Processing
 * top to bottom means a chain of copies (New2.* = New.*, New.* = Old.*)
 * expands transitively for free -- New's entries already reflect Old's by
 * the time New2's copy runs. */
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
		} else if (s->kind == AST_METHODCOPY && s->tok) {   /* single: New.m = Old.m2 */
			if (g_nmethodsyms < FB_MAX_METHODSYMS) {
				g_methodsyms[g_nmethodsyms].class_name  = s->kids[0]->tok;
				g_methodsyms[g_nmethodsyms].method_name = s->tok;
				g_methodsyms[g_nmethodsyms].site = s;
				g_nmethodsyms++;
			}
		} else if (s->kind == AST_METHODCOPY) {             /* wildcard: New.* = Old.* */
			token *dst = s->kids[0]->tok, *src = s->kids[1]->tok;
			int n = g_nmethodsyms;   /* snapshot: don't also copy entries added below */
			for (int j = 0; j < n && g_nmethodsyms < FB_MAX_METHODSYMS; j++)
				if (tok_eq(g_methodsyms[j].class_name, src)) {
					g_methodsyms[g_nmethodsyms].class_name  = dst;
					g_methodsyms[g_nmethodsyms].method_name = g_methodsyms[j].method_name;
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

/* Every binding visible at scope `s` (all frames, walking ->parent), most
 * inner first, each name appearing once (an outer frame's binding of the
 * same name is shadowed and skipped). For completion, which needs the full
 * local-variable list rather than one name's lookup. Writes at most
 * max_out entries into out; returns the count written. */
static int scope_collect(scope *s, tbind **out, int max_out) {
	int n = 0;
	for (; s && n < max_out; s = s->parent)
		for (tbind *b = s->binds; b && n < max_out; b = b->next) {
			int shadowed = 0;
			for (int i = 0; i < n; i++)
				if (tok_eq(out[i]->name, b->name)) { shadowed = 1; break; }
			if (!shadowed) out[n++] = b;
		}
	return n;
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
		if (t->fields && t->nchildren) {
			APPEND("<{ ");
			for (size_t i = 0; i < t->nchildren; i++) {
				if (i) APPEND(", ");
				if (t->fields[i]) APPEND("%.*s: ", (int)t->fields[i]->len, t->fields[i]->string);
				obj_type_sprint_into(t->children[i], buf, bufsz, pos);
			}
			APPEND(" }>");
		}
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
	case RET_UNION:
		for (size_t i = 0; i < t->nchildren; i++) {
			if (i) APPEND("|");
			obj_type_sprint_into(t->children[i], buf, bufsz, pos);
		}
		break;
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
	 * "math" namespace, or a native method shadow-registered by fb_method
	 * as "math.sqrt" -- see native_method_type) with an optional doc string
	 * takes priority over the bare inferred type -- these are frequently
	 * RET_VAR/"?" (no static shape tracked), so a doc string is the only
	 * useful thing to show. `disp` is what gets printed in the fenced
	 * signature block: the bare name for AST_ID, "base.method" for a native
	 * method field so the shown signature reads as it's actually called.
	 * Two ways an AST_FIELD's base names a native class for this lookup:
	 * its own identifier text (math.sqrt: math is RET_VAR, no shape to
	 * check) or its inferred type's class_name (a Stream instance under any
	 * local variable name: s.write, base->type is RET_OBJECT{"Stream"}). */
	const fb_Reg *rg = NULL;
	token disp = {0};
	if (n->kind == AST_ID && n->tok) {
		rg = fb_reg_findn(n->tok->string, n->tok->len);
		if (rg) disp = *n->tok;
	} else if (n->kind == AST_FIELD && n->tok && n->kids[0]) {
		token *name = n->tok;
		token *base = (n->kids[0]->kind == AST_ID && n->kids[0]->tok) ? n->kids[0]->tok
		            : (n->kids[0]->type && n->kids[0]->type->ret == RET_OBJECT)
		              ? n->kids[0]->type->class_name : NULL;
		if (base) {
			size_t len;
			char *buf = dotted_key(base, name, &len);
			rg = fb_reg_findn(buf, len);
			if (rg) { disp = *name; disp.string = buf; disp.len = len; }
		}
	}
	if (rg && rg->doc) {
		/* Prepend the computed signature as a fenced code block ahead of
		 * the doc's own prose sections (see builtin_docs.h for the
		 * convention) -- a doc string alone omits arity/types entirely.
		 * Skip this for FB_GLOBAL (a bound value, not a callable) -- its
		 * min/max in the arity table describes fb_global_bind's own
		 * plumbing, not a call shape, so a synthesized "name(...)" would
		 * be actively misleading; the doc's own "**Type**" line already
		 * says what it is. */
		obj_type *sig = (rg->flags & FB_GLOBAL) ? NULL : builtin_signature(&disp, NULL);
		if (sig) {
			char *sigstr = obj_type_sprint(sig);
			size_t cap = disp.len + strlen(sigstr) + strlen(rg->doc) + 32;
			char *buf = a_alloc(cap);
			snprintf(buf, cap, "```funcbuild\n%.*s%s\n```\n\n%s",
			         (int)disp.len, disp.string, sigstr, rg->doc);
			out->text = buf;
		} else {
			out->text = a_dup(rg->doc);
		}
		out->markdown = 1;
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
	out->markdown = 0;
	return 1;
}

int fb_complete_at(ast *prog, int line, int col, fb_CompItem *out, int max_out) {
	if (!out || max_out <= 0) return 0;
	int n = 0;

	/* 1. builtins: core table first, then anything registered (host/plugin)
	 * not already covered by the core table. */
	for (size_t i = 0; i < N_CORE_ARITY && n < max_out; i++) {
		token tmp = {0};
		tmp.string = (char *)g_core_arity[i].name;
		tmp.len = strlen(g_core_arity[i].name);
		/* A FB_GLOBAL name (args, cwd, ...) is a bound value, not a
		 * callable -- its arity-table entry describes fb_global_bind's own
		 * plumbing, not a call shape, so synthesizing "name(...)" here
		 * would be misleading. Use the doc's own summary line instead. */
		const fb_Reg *grg = fb_reg_findn(tmp.string, tmp.len);
		int is_global = grg && (grg->flags & FB_GLOBAL);
		obj_type *sig = is_global ? NULL : builtin_signature(&tmp, NULL);
		out[n].name = g_core_arity[i].name;
		out[n].kind = FB_COMP_BUILTIN;
		if (sig) out[n].detail = obj_type_sprint(sig);
		else if (is_global && grg->doc) out[n].detail = a_dupn(grg->doc, strcspn(grg->doc, "\n"));
		else out[n].detail = NULL;
		n++;
	}
	for (int i = 0; i < fb_reg_count(NULL) && n < max_out; i++) {
		const fb_Reg *rg = fb_reg_at(NULL, i);
		int dup = 0;
		for (size_t j = 0; j < N_CORE_ARITY; j++)
			if (!strcmp(rg->name, g_core_arity[j].name)) { dup = 1; break; }
		if (dup) continue;
		out[n].name = rg->name;
		out[n].kind = FB_COMP_BUILTIN;
		/* Just the summary line: a fully fleshed-out doc (see
		 * builtin_docs.h) is multiple paragraphs, too long for a
		 * completion item's single-line detail -- full text is still
		 * available via hover. */
		out[n].detail = rg->doc ? a_dupn(rg->doc, strcspn(rg->doc, "\n")) : NULL;
		n++;
	}

	/* 2. top-level defs/methods */
	if (prog) {
		collect_symbols(prog);
		for (int i = 0; i < g_ndefsyms && n < max_out; i++) {
			ast *rhs = g_defsyms[i].site->kids[0];
			out[n].name = tok_str(g_defsyms[i].name);
			out[n].kind = (rhs && rhs->kind == AST_LAMBDA)
			            ? FB_COMP_TOPLEVEL_FN : FB_COMP_TOPLEVEL_VAR;
			out[n].detail = g_defsyms[i].site->type
			              ? obj_type_sprint(g_defsyms[i].site->type) : NULL;
			n++;
		}
		for (int i = 0; i < g_nmethodsyms && n < max_out; i++) {
			out[n].name = tok_str(g_methodsyms[i].method_name);
			out[n].kind = FB_COMP_TOPLEVEL_FN;
			out[n].detail = NULL;
			n++;
		}
	}

	/* 3. locals in scope at (line, col) -- omitted if the cursor isn't over
	 * any node (a gap) or prog is NULL (last check was a syntax error). */
	if (prog) {
		ast *cursor = find_node(prog, line, col);
		if (cursor) {
			scope *sc = locate_scope(prog, cursor, NULL);
			if (sc) {
				tbind *binds[256];
				int nb = scope_collect(sc, binds, 256);
				for (int i = 0; i < nb && n < max_out; i++) {
					out[n].name = tok_str(binds[i]->name);
					out[n].kind = FB_COMP_LOCAL;
					out[n].detail = binds[i]->type ? obj_type_sprint(binds[i]->type) : NULL;
					n++;
				}
			}
		}
	}

	return n;
}

/* Field/method candidates for receiver type `base`. Two field-bearing
 * shapes: RET_FILE records (File<>, standard fields) and RET_OBJECT with
 * `fields` actually populated -- infer_parser_schema (Parser<schema>
 * results) and infer_objnew_schema (a `type X(...) = { ... }` instance
 * whose fields body is a statically-shaped map literal, see ast.h/walk.c)
 * both populate this. A `type X(...)` instance ALSO offers its methods
 * (matched by class name against g_methodsyms, dynamic dispatch, no static
 * checking, same resolution infer()'s own AST_FIELD case uses) alongside
 * any known fields -- the two aren't mutually exclusive on an object.
 * Anything else (RET_VAR, a dynamic Map, List, ...) has no statically-known
 * members: 0 candidates, correctly. */
static int complete_receiver_fields(obj_type *base, fb_CompItem *out, int max_out) {
	int n = 0;
	if (!base) return 0;
	if ((base->ret == RET_FILE || base->ret == RET_OBJECT) && base->fields) {
		for (size_t i = 0; i < base->nchildren && n < max_out; i++) {
			/* NOT tok_str(): File/Parser<schema> field-name tokens (FF_name,
			 * PS_rest, ...) are process-wide statics shared across every
			 * fb_State ever created -- tok_str's lazy ->owned cache would
			 * capture a pointer into whichever fb_State's arena happened to
			 * call it first, which dangles once that state is fb_close'd.
			 * A fresh, uncached copy into the CURRENT arena is required. */
			out[n].name = a_dupn(base->fields[i]->string, base->fields[i]->len);
			out[n].kind = FB_COMP_FIELD;
			out[n].detail = base->children[i] ? obj_type_sprint(base->children[i]) : NULL;
			n++;
		}
	}
	if (base->ret == RET_OBJECT && base->class_name) {
		for (int i = 0; i < g_nmethodsyms && n < max_out; i++)
			if (tok_eq(g_methodsyms[i].class_name, base->class_name)) {
				out[n].name = tok_str(g_methodsyms[i].method_name);
				out[n].kind = FB_COMP_TOPLEVEL_FN;
				out[n].detail = NULL;
				n++;
			}
		/* native class methods (fb_method()'s shadow "ClassName.method"
		 * registry entries, e.g. Stream) -- a completely separate source
		 * from g_methodsyms (language `type`/`method` defs), not mutually
		 * exclusive with it. */
		const fb_Reg *nregs[64];
		int nn = fb_reg_class_methods(tok_str(base->class_name), nregs, 64);
		for (int i = 0; i < nn && n < max_out; i++) {
			const char *dot = strchr(nregs[i]->name, '.');
			out[n].name = a_dup(dot ? dot + 1 : nregs[i]->name);
			out[n].kind = FB_COMP_TOPLEVEL_FN;
			out[n].detail = nregs[i]->doc ? a_dupn(nregs[i]->doc, strcspn(nregs[i]->doc, "\n")) : NULL;
			n++;
		}
	}
	return n;
}

int fb_complete_field_at(ast *prog, int line, int col, fb_CompItem *out, int max_out) {
	if (!prog || !out || max_out <= 0) return -1;
	/* An LSP cursor sits right after the last typed character, i.e. one
	 * column past a token's own span (find_node/ast_span require the
	 * position to be ON a covered character, correct for hover but not
	 * here) -- if the exact position misses, retry one column back before
	 * giving up. */
	ast *n = find_node(prog, line, col);
	if ((!n || n->kind != AST_FIELD) && col > 1)
		n = find_node(prog, line, col - 1);
	if (!n || n->kind != AST_FIELD || !n->kids[0]) return -1;
	collect_symbols(prog);   /* populate g_methodsyms for the RET_OBJECT case */
	return complete_receiver_fields(n->kids[0]->type, out, max_out);
}

int fb_document_symbols(ast *prog, fb_DocSymbol *out, int max_out) {
	if (!prog || !out || max_out <= 0) return 0;
	collect_symbols(prog);
	int n = 0;
	for (int i = 0; i < g_ndefsyms && n < max_out; i++) {
		ast *rhs = g_defsyms[i].site->kids[0];
		fb_SymKind kind = FB_SYM_VARIABLE;
		if (rhs && rhs->kind == AST_LAMBDA) {
			ast *body = rhs->nkids > 1 ? rhs->kids[1] : NULL;
			kind = (body && body->kind == AST_OBJNEW) ? FB_SYM_CLASS : FB_SYM_FUNCTION;
		}
		out[n].name = tok_str(g_defsyms[i].name);
		out[n].kind = kind;
		out[n].span = ast_span(g_defsyms[i].site);
		out[n].container = NULL;
		n++;
	}
	for (int i = 0; i < g_nmethodsyms && n < max_out; i++) {
		out[n].name = tok_str(g_methodsyms[i].method_name);
		out[n].kind = FB_SYM_METHOD;
		out[n].span = ast_span(g_methodsyms[i].site);
		out[n].container = tok_str(g_methodsyms[i].class_name);
		n++;
	}
	return n;
}

/* Best-effort functor signature for a callee named `name` (a raw text
 * slice, len bytes -- not tied to any specific AST node, so this works
 * even when `prog` doesn't parse, e.g. mid-typing an unclosed call): a
 * builtin (curated or arity-based, via builtin_signature) takes priority,
 * else a top-level user function's own already-inferred type in `prog`
 * (the generic, context-free signature -- same one a bare hover on the
 * def's own name shows, e.g. "((?, ? -> ?), ?, *? -> ?)"). `prog` may be
 * NULL (only builtins resolve then). Returns NULL if nothing is known. */
obj_type *fb_lookup_signature(ast *prog, const char *name, size_t len) {
	token tmp = {0};
	tmp.string = (char *)name;
	tmp.len = len;
	obj_type *sig = builtin_signature(&tmp, NULL);
	if (sig) return sig;
	if (!prog) return NULL;
	collect_symbols(prog);
	for (int i = 0; i < g_ndefsyms; i++)
		if (g_defsyms[i].name->len == len && !strncmp(g_defsyms[i].name->string, name, len))
			return g_defsyms[i].site->type;
	return NULL;
}

/* Individual parameter types of a RET_FUNCTOR, flattening a Tuple-shaped
 * param slot into one entry per element (the same rule
 * obj_type_sprint_into's RET_FUNCTOR case renders by) -- e.g. for signature
 * help, which needs each argument's own type/label rather than one string
 * for the whole param list. Writes at most max_out into out, returns the
 * count written (0 if `t` isn't RET_FUNCTOR). */
int fb_functor_params(obj_type *t, obj_type **out, int max_out) {
	if (!t || t->ret != RET_FUNCTOR || max_out <= 0) return 0;
	obj_type *p = t->nchildren > 0 ? t->children[0] : NULL;
	if (!p) return 0;
	if (p->ret == RET_TUPLE) {
		int n = 0;
		for (size_t i = 0; i < p->nchildren && n < max_out; i++) out[n++] = p->children[i];
		return n;
	}
	out[0] = p;
	return 1;
}
