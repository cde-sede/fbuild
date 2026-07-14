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
	[AST_LOGICAL] = "AST_LOGICAL",
	[AST_NOT] = "AST_NOT",
	[AST_MORPHISM] = "AST_MORPHISM",

	[AST_OBJNEW] = "AST_OBJNEW",
	[AST_METHODDEF] = "AST_METHODDEF",
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

/* map a registered builtin's declared return type (fbuild.h) to a checker type.
 * Map/Json and ANY are RET_VAR (unknown): field access stays permissive. */
static obj_type *fb_ret_type(fb_Ret r) {
	switch (r) {
	case FB_RET_INT:  return t_new(RET_INT);
	case FB_RET_STR:  return t_new(RET_STRING);
	case FB_RET_BOOL: return t_new(RET_BOOL);
	case FB_RET_FILE: return t_file();
	case FB_RET_LIST: return t_list(t_new(RET_VAR));
	case FB_RET_UNIT: return t_new(RET_NONE);
	case FB_RET_MAP:
	case FB_RET_ANY:
	default:          return t_new(RET_VAR);
	}
}

/* ---- error reporting + unification ------------------------------------ */

static int g_type_errors;

static void type_err(token *where, const char *fmt, ...) {
	g_type_errors++;
	fprintf(stderr, "type error");
	if (where)
		fprintf(stderr, " at %d:%d", where->start.line, where->start.col);
	fprintf(stderr, ": ");
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* structural compatibility; RET_VAR (unknown) unifies with anything */
static int unify(obj_type *a, obj_type *b) {
	if (!a || !b)
		return 1;
	if (a->ret == RET_VAR || b->ret == RET_VAR)
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

/* named field of a record (RET_FILE), or NULL if absent */
static obj_type *record_field(obj_type *rec, token *name) {
	if (rec && rec->ret == RET_FILE && rec->fields)
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
	case RET_VAR:     return "?";
	}
	return "?";
}

/* ---- type scope (lambda params + implicit argument) ------------------- */

typedef struct tbind { token *name; obj_type *type; struct tbind *next; } tbind;

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

static void scope_put(scope *s, token *name, obj_type *t) {
	tbind *b = a_alloc(sizeof *b);
	b->name = name;
	b->type = t;
	b->next = s->binds;
	s->binds = b;
}

/* ---- inference -------------------------------------------------------- */

static ast      *g_program;   /* top-level scope for resolving identifiers */
static obj_type *infer(ast *node, scope *sc);

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
		"count", "find", "to_int", "to_str", "repeat", "chars",
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
	if (tok_is(name, "args"))          /* CLI args */
		return t_list(t_new(RET_STRING));
	if (tok_is(name, "cwd"))           /* builtin cwd */
		return t_new(RET_STRING);
	if (tok_is(name, "_GLOBALS") || tok_is(name, "_SHARED"))
		return t_new(RET_VAR);         /* mutable store: dynamic map */
	if (is_builtin_name(name))
		return t_new(RET_VAR);
	type_err(name, "unknown symbol '%.*s'",
	         (int)name->len, name->string);
	return t_new(RET_VAR);
}

static obj_type *infer_call(ast *node, scope *sc) {
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
		static const struct { const char *name; int min, max; } arity[] = {
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
			{"count",2,2}, {"find",2,2}, {"to_int",1,1}, {"to_str",1,1},
			{"repeat",2,2}, {"chars",1,1},
			{"clock",0,0}, {"time",0,0}, {"sleep",1,1}, {"bench",2,2},
			{"config",1,1}, {"flag",1,-1}, {"opt",2,2}, {"positionals",0,0},
		};
		for (size_t i = 0; i < sizeof arity / sizeof arity[0]; i++) {
			if (!tok_is(callee->tok, arity[i].name)) continue;
			int mn = arity[i].min, mx = arity[i].max;
			if ((int)na < mn || (mx >= 0 && (int)na > mx)) {
				if (mx < 0)
					type_err(callee->tok, "%s expects at least %d argument%s, got %zu",
					         arity[i].name, mn, mn == 1 ? "" : "s", na);
				else if (mn == mx)
					type_err(callee->tok, "%s expects %d argument%s, got %zu",
					         arity[i].name, mn, mn == 1 ? "" : "s", na);
				else
					type_err(callee->tok, "%s expects %d to %d arguments, got %zu",
					         arity[i].name, mn, mx, na);
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
	obj_type *ft = (callee->kind == AST_ID)
	             ? (scope_get(sc, callee->tok) ? scope_get(sc, callee->tok)
	                                           : infer_id(callee->tok))
	             : infer(callee, sc);
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

/* bind a lambda's parameters against the argument it is applied to. A lambda
 * takes one argument; multiple params destructure it positionally. */
static void bind_params(scope *sc, ast *params, obj_type *arg) {
	sc->arg = arg;
	size_t P = params->nkids;
	for (size_t i = 0; i < P; i++) {
		ast *p = params->kids[i];
		obj_type *pt = (P == 1) ? arg : positional(arg, i);
		switch (p->kind) {
		case AST_ID:                       /* x         */
		case AST_PROJECTION:               /* .src / .0 */
			if (p->tok) scope_put(sc, p->tok, pt);
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
			}
			break;
		default:
			break;
		}
	}
}

/* apply a function node to an argument type, returning the result type */
static obj_type *infer_apply(ast *fn, obj_type *arg, scope *sc) {
	if (fn->kind == AST_LAMBDA) {
		scope inner = { .parent = sc };
		bind_params(&inner, fn->kids[0], arg);
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

/* sentinel parked on node->type while a context-free node is mid-inference,
 * so a self-referential def resolves to ? instead of recursing forever. */
static obj_type T_INPROGRESS = { .ret = RET_VAR };

static obj_type *infer(ast *node, scope *sc) {
	if (!node)
		return t_new(RET_VAR);
	if (!sc && node->type) {      /* context-free memo (shared ids -> DAG) */
		if (node->type == &T_INPROGRESS)   /* recursive re-entry */
			return t_new(RET_VAR);
		return node->type;
	}
	if (!sc)
		node->type = &T_INPROGRESS;

	obj_type *t;
	switch (node->kind) {

	case AST_INT:    t = t_new(RET_INT);    break;
	case AST_FLOAT:  t = t_new(RET_FLOAT);  break;
	case AST_BOOL:   t = t_new(RET_BOOL);   break;
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
		t = t_new(RET_VAR);
		break;

	case AST_METHODDEF:                              /* method Class.name = fn   */
		infer(node->kids[1], sc);   /* the lambda; kids[0] is the class name id, */
		t = t_new(RET_NONE);        /* not a variable, so don't infer it         */
		break;

	case AST_COMPARE: {
		obj_type *l = infer(node->kids[0], sc);
		obj_type *r = infer(node->kids[1], sc);
		if (!unify(l, r))
			type_err(node->tok, "cannot compare %s with %s",
			         ret_kind_name(l->ret), ret_kind_name(r->ret));
		t = t_new(RET_BOOL);
		break;
	}

	case AST_ARITH: {                    /* + - * / % : Int x Int -> Int */
		obj_type *l = infer(node->kids[0], sc);
		obj_type *r = infer(node->kids[1], sc);
		if (l->ret != RET_INT && l->ret != RET_VAR)
			type_err(node->tok, "arithmetic operand must be Int, got %s",
			         ret_kind_name(l->ret));
		if (r->ret != RET_INT && r->ret != RET_VAR)
			type_err(node->tok, "arithmetic operand must be Int, got %s",
			         ret_kind_name(r->ret));
		t = t_new(RET_INT);
		break;
	}

	case AST_LOGICAL: {                  /* and/or: Bool x Bool -> Bool;  ||: value-or */
		obj_type *l = infer(node->kids[0], sc);
		obj_type *r = infer(node->kids[1], sc);
		if (node->tok->kind == TOK_OROR) {   /* left if truthy else right: any -> ? */
			t = t_new(RET_VAR);
			break;
		}
		if (l->ret != RET_BOOL && l->ret != RET_VAR)
			type_err(node->tok, "'%.*s' operand must be Bool, got %s",
			         (int)node->tok->len, node->tok->string, ret_kind_name(l->ret));
		if (r->ret != RET_BOOL && r->ret != RET_VAR)
			type_err(node->tok, "'%.*s' operand must be Bool, got %s",
			         (int)node->tok->len, node->tok->string, ret_kind_name(r->ret));
		t = t_new(RET_BOOL);
		break;
	}

	case AST_NOT: {                      /* not : Bool -> Bool */
		obj_type *o = infer(node->kids[0], sc);
		if (o->ret != RET_BOOL && o->ret != RET_VAR)
			type_err(node->tok, "'not' operand must be Bool, got %s",
			         ret_kind_name(o->ret));
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

	case AST_MORPHISM:               /* [*.c] // [*.o] : File -> File */
		t = t_functor(t_file(), t_file());
		break;

	case AST_LAMBDA: {               /* standalone: arg type unknown */
		scope inner = { .parent = sc };
		obj_type *param = t_new(RET_VAR);
		bind_params(&inner, node->kids[0], param);
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
		if (whenc) {                        /* condition must be Bool */
			obj_type *ct = infer(whenc->kids[0], bsc);
			if (ct->ret != RET_BOOL && ct->ret != RET_VAR)
				type_err(NULL, "when condition must be Bool, got %s",
				         ret_kind_name(ct->ret));
		}
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
			if (!ft && base && base->ret != RET_VAR)
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

	case AST_PARSER:               /* Parser<schema> : dynamic map of results */
		infer(node->kids[0], sc);
		t = t_new(RET_VAR);
		break;

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
	case RET_VAR:     printf("?");       break;

	case RET_LIST:
		printf("List<");
		obj_type_print(t->nchildren ? t->children[0] : NULL);
		printf(">");
		break;

	case RET_TUPLE:
		printf("Tuple<");
		for (size_t i = 0; i < t->nchildren; i++) {
			if (i) printf(", ");
			obj_type_print(t->children[i]);
		}
		printf(">");
		break;

	case RET_FUNCTOR:
		printf("(");
		obj_type_print(t->nchildren > 0 ? t->children[0] : NULL);
		printf(" -> ");
		obj_type_print(t->nchildren > 1 ? t->children[1] : NULL);
		printf(")");
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

	for (size_t i = 0; i < node->nkids; i++)
		check_recursion(name, node->kids[i], guarded);
}

int infer_program(ast *prog) {
	g_program = prog;
	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		int is_def = (s->kind == AST_ASSIGN || s->kind == AST_EAGER);
		infer_type(is_def ? s->kids[0] : s);  /* type-check */
		if (is_def)
			check_recursion(s->tok, s->kids[0], 0);
	}
	if (g_type_errors)
		fprintf(stderr, "%d type error%s\n",
		        g_type_errors, g_type_errors == 1 ? "" : "s");
	return g_type_errors;
}
