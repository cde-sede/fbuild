/* Lazy build-graph evaluator.
 *
 * Lowers the AST into an in-memory DAG of build nodes. Top-level defs are
 * thunks: forced on first reference and memoized, so shared names become
 * shared nodes (a real DAG, order-independent). Evaluation builds node
 * structure only -- it never globs the filesystem, expands a map over a
 * list, beta-reduces a lambda, or spawns a command. The single effectful
 * builtin, run(), is lowered to a V_RUN node and left unspawned: that is
 * the deferred stage ("everything but run"). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ast.h"
#include "graph.h"

#define fatal(...) do { fprintf(stderr, "eval: " __VA_ARGS__); \
                        fputc('\n', stderr); exit(1); } while (0)

/* ---- values / graph nodes --------------------------------------------- */

typedef enum {
	V_THUNK,        /* unforced: expr + env                          */
	V_INT, V_FLOAT, V_STRING,
	V_PATH,         /* path/glob pattern, e.g. src then star then c  */
	V_BUILTIN,      /* free symbol: glob, zip, subst, executable ... */
	V_LAMBDA,       /* closure: params + body (opaque until run)     */
	V_TUPLE,        /* a, b, c                                       */
	V_RECORD,       /* Type<...> constructor instance                */
	V_FIELDV,       /* named/splat field of a record                 */
	V_CMD,          /* `gcc ... {interp}`                            */
	V_MORPH,        /* [*.c] // [*.o]                                */
	V_PIPE,         /* input |> fn                                   */
	V_SEQ,          /* a >> b >> c                                   */
	V_CALL,         /* f(args...)                                    */
	V_RUN,          /* run(...)  -- terminal, NOT spawned            */
	V_GUARD,        /* body [where ...] [when ...]                   */
	V_WHEREV,
	V_COMPARE,
	V_FIELD,        /* base.field                                    */
	V_PROJ,         /* .field / .0  (standalone projection)          */
	V_CAPTURE,      /* @ / @id                                       */
	V_META,         /* &x                                            */
	V_SPLAT,        /* ...x                                          */
} vkind;

static const char *vkind_name[] = {
	[V_THUNK]="thunk", [V_INT]="int", [V_FLOAT]="float", [V_STRING]="string",
	[V_PATH]="path", [V_BUILTIN]="builtin", [V_LAMBDA]="lambda",
	[V_TUPLE]="tuple", [V_RECORD]="record", [V_FIELDV]="field",
	[V_CMD]="cmd", [V_MORPH]="morph", [V_PIPE]="pipe", [V_SEQ]="seq",
	[V_CALL]="call", [V_RUN]="run", [V_GUARD]="guard", [V_WHEREV]="where",
	[V_COMPARE]="compare", [V_FIELD]="field-access", [V_PROJ]="proj",
	[V_CAPTURE]="capture", [V_META]="meta", [V_SPLAT]="splat",
};

typedef struct value value;
typedef struct env env;

struct value {
	vkind        kind;
	token       *tok;          /* leaf slice / op / type / field name */

	/* labelled children (graph edges) */
	value      **kids;
	const char **labels;
	size_t       nkids, cap;

	/* V_THUNK */
	ast         *expr;
	env         *tenv;
	value       *forced;

	/* V_LAMBDA: opaque body kept as AST */
	ast         *body;

	obj_type    *type;         /* inferred type of this node (from walk.c) */
};

struct binding { token *name; value *val; struct binding *next; };
struct env     { struct binding *head; env *parent; };

/* ---- helpers ---------------------------------------------------------- */

static value *vnew(vkind k) {
	value *v = calloc(1, sizeof *v);
	if (!v) fatal("out of memory");
	v->kind = k;
	return v;
}

static void vpush(value *p, const char *label, value *c) {
	if (p->nkids == p->cap) {
		p->cap = p->cap ? p->cap * 2 : 4;
		p->kids   = realloc(p->kids,   p->cap * sizeof *p->kids);
		p->labels = realloc(p->labels, p->cap * sizeof *p->labels);
		if (!p->kids || !p->labels) fatal("out of memory");
	}
	p->labels[p->nkids] = label;
	p->kids[p->nkids++] = c;
}

static value *vleaf(vkind k, token *t) { value *v = vnew(k); v->tok = t; return v; }

static int tok_eq(token *a, token *b) {
	return a->len == b->len && !memcmp(a->string, b->string, a->len);
}

static env *env_new(env *parent) {
	env *e = calloc(1, sizeof *e);
	if (!e) fatal("out of memory");
	e->parent = parent;
	return e;
}

static void env_bind(env *e, token *name, value *val) {
	struct binding *b = calloc(1, sizeof *b);
	if (!b) fatal("out of memory");
	b->name = name;
	b->val  = val;
	b->next = e->head;
	e->head = b;
}

static value *env_lookup(env *e, token *name) {
	for (; e; e = e->parent)
		for (struct binding *b = e->head; b; b = b->next)
			if (tok_eq(b->name, name))
				return b->val;
	return NULL;
}

/* ---- evaluation ------------------------------------------------------- */

static value *eval_inner(ast *n, env *e);

/* stamp each freshly-built node with the type inference computed for its
 * source AST node, so the graph can print "node : Type" */
static value *eval(ast *n, env *e) {
	value *v = eval_inner(n, e);
	if (v && !v->type)
		v->type = n->type;
	return v;
}

static value *force(value *v) {
	if (v && v->kind == V_THUNK) {
		if (!v->forced)
			v->forced = eval(v->expr, v->tenv);
		return force(v->forced);
	}
	return v;
}

static int is_run_call(ast *n) {
	return n->kind == AST_CALL
	    && n->kids[0]->kind == AST_ID
	    && n->kids[0]->tok->len == 3
	    && !memcmp(n->kids[0]->tok->string, "run", 3);
}

static value *eval_inner(ast *n, env *e) {
	switch (n->kind) {
	case AST_INT:    return vleaf(V_INT, n->tok);
	case AST_FLOAT:  return vleaf(V_FLOAT, n->tok);
	case AST_STRING: return vleaf(V_STRING, n->tok);
	case AST_PATH:   return vleaf(V_PATH, n->tok);

	case AST_ID: {
		value *b = env_lookup(e, n->tok);
		if (b) return force(b);          /* shared -> DAG node */
		return vleaf(V_BUILTIN, n->tok); /* free symbol: glob/zip/... */
	}

	case AST_PROJECTION: return vleaf(V_PROJ, n->tok);

	case AST_CAPTURE: {
		value *v = vleaf(V_CAPTURE, n->tok);
		if (n->nkids) vpush(v, "name", eval(n->kids[0], e));
		return v;
	}

	case AST_METADATA: {
		value *v = vnew(V_META);
		vpush(v, "of", eval(n->kids[0], e));
		return v;
	}

	case AST_SPLAT: {
		value *v = vnew(V_SPLAT);
		vpush(v, "of", eval(n->kids[0], e));
		return v;
	}

	case AST_FIELD: {
		value *v = vleaf(V_FIELD, n->tok);
		vpush(v, "base", eval(n->kids[0], e));
		return v;
	}

	case AST_LAMBDA: {                    /* opaque closure */
		value *v = vnew(V_LAMBDA);
		ast *params = n->kids[0];
		for (size_t i = 0; i < params->nkids; i++)
			vpush(v, "param", eval(params->kids[i], e));
		v->body = n->kids[1];
		v->tenv = e;
		return v;
	}

	case AST_TUPLE: {
		value *v = vnew(V_TUPLE);
		for (size_t i = 0; i < n->nkids; i++)
			vpush(v, "elem", eval(n->kids[i], e));
		return v;
	}

	case AST_CONSTRUCT: {
		value *v = vleaf(V_RECORD, n->tok);   /* tok = type name */
		for (size_t i = 0; i < n->nkids; i++) {
			ast *f = n->kids[i];              /* AST_CFIELD */
			value *fv = vleaf(V_FIELDV, f->tok); /* tok=name or NULL */
			vpush(fv, "value", eval(f->kids[0], e));
			vpush(v, f->tok ? "field" : "splat", fv);
		}
		return v;
	}

	case AST_COMMAND: {
		value *v = vnew(V_CMD);
		for (size_t i = 0; i < n->nkids; i++) {
			ast *part = n->kids[i];
			if (part->kind == AST_CMDTEXT)
				vpush(v, "arg", vleaf(V_STRING, part->tok));
			else
				vpush(v, "interp", eval(part, e));
		}
		return v;
	}

	case AST_MORPHISM: {
		value *v = vnew(V_MORPH);
		vpush(v, "from", eval(n->kids[0], e));
		vpush(v, "to",   eval(n->kids[1], e));
		return v;
	}

	case AST_PIPE: {                       /* left-assoc: in |> fn |> fn */
		value *acc = eval(n->kids[0], e);
		for (size_t i = 1; i < n->nkids; i++) {
			value *p = vnew(V_PIPE);
			vpush(p, "in", acc);
			vpush(p, "fn", eval(n->kids[i], e));
			acc = p;
		}
		return acc;
	}

	case AST_SEQ: {
		value *v = vnew(V_SEQ);
		for (size_t i = 0; i < n->nkids; i++)
			vpush(v, "step", eval(n->kids[i], e));
		return v;
	}

	case AST_COMPARE: {
		value *v = vleaf(V_COMPARE, n->tok);
		vpush(v, "lhs", eval(n->kids[0], e));
		vpush(v, "rhs", eval(n->kids[1], e));
		return v;
	}

	case AST_CALL: {
		/* run() is the deferred terminal: build the node, never spawn */
		int run = is_run_call(n);
		value *v = vnew(run ? V_RUN : V_CALL);
		if (!run)
			vpush(v, "fn", eval(n->kids[0], e));
		for (size_t i = 1; i < n->nkids; i++)
			vpush(v, "arg", eval(n->kids[i], e));
		return v;
	}

	case AST_GUARD: {
		ast *body  = n->kids[0];
		ast *where = n->kids[1];
		ast *whenc = n->kids[2];

		env *ge = e;
		value *wv = NULL;
		if (where) {
			ge = env_new(e);
			wv = vnew(V_WHEREV);
			for (size_t i = 0; i < where->nkids; i++) {
				ast *bd = where->kids[i];        /* AST_BINDING */
				value *val = eval(bd->kids[0], ge);
				env_bind(ge, bd->tok, val);
				vpush(wv, "bind", val);          /* label by name below */
			}
		}

		value *v = vnew(V_GUARD);
		vpush(v, "body", eval(body, ge));
		if (wv)    vpush(v, "where", wv);
		if (whenc) vpush(v, "when", eval(whenc->kids[0], ge)); /* the condition */
		return v;
	}

	case AST_ASSIGN:
		/* an assignment used as a value: bind, value is the rhs */
		return eval(n->kids[0], e);

	default:
		fatal("cannot lower node '%s'", ast_name[n->kind]);
	}
	return NULL;
}

/* ---- printer ---------------------------------------------------------- */

static int gcolor;
#define G_DIM  (gcolor ? "\033[2m"    : "")
#define G_KIND (gcolor ? "\033[1;35m" : "")
#define G_LBL  (gcolor ? "\033[2;37m" : "")
#define G_TOK  (gcolor ? "\033[0;33m" : "")
#define G_OFF  (gcolor ? "\033[0m"    : "")

static void label_line(const char *label, value *v) {
	if (label)
		printf("%s%s:%s ", G_LBL, label, G_OFF);
	printf("%s%s%s", G_KIND, vkind_name[v->kind], G_OFF);
	if (v->tok && v->tok->len)
		printf(" %s%.*s%s", G_TOK, (int)v->tok->len, v->tok->string, G_OFF);
	if (v->kind == V_LAMBDA && v->body)
		printf(" %s-> <%s>%s", G_DIM, ast_name[v->body->kind], G_OFF);
	if (v->type) {
		printf(" %s: %s", G_DIM, G_OFF);
		obj_type_print(v->type);
	}
	putchar('\n');
}

static void print_kids(value *v, const char *prefix) {
	for (size_t i = 0; i < v->nkids; i++) {
		value *c = v->kids[i];
		int last = (i + 1 == v->nkids);
		printf("%s%s%s%s", G_DIM, prefix, last ? "└─ " : "├─ ", G_OFF);

		/* graph DAGs can re-share nodes; print fully (acyclic by build) */
		if (c) {
			label_line(v->labels[i], c);
			char *cp = malloc(strlen(prefix) + 8);
			if (!cp) fatal("out of memory");
			sprintf(cp, "%s%s", prefix, last ? "   " : "│  ");
			print_kids(c, cp);
			free(cp);
		} else {
			printf("%s<null>%s\n", G_DIM, G_OFF);
		}
	}
}

static void print_root(const char *label, value *v) {
	label_line(label, v);
	print_kids(v, "");
}

/* ---- driver ----------------------------------------------------------- */

void graph_run_program(ast *prog) {
	gcolor = isatty(fileno(stdout));
	env *global = env_new(NULL);

	/* pass 1: bind every top-level definition as a thunk (lazy) */
	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		if (s->kind == AST_ASSIGN) {
			value *th = vnew(V_THUNK);
			th->expr = s->kids[0];
			th->tenv = global;
			env_bind(global, s->tok, th);
		}
	}

	/* pass 2: force defs (for display) then roots (e.g. run(...)) */
	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		if (s->kind != AST_ASSIGN)
			continue;
		printf("%s%.*s%s = ", G_KIND, (int)s->tok->len, s->tok->string, G_OFF);
		print_root(NULL, force(env_lookup(global, s->tok)));
	}

	for (size_t i = 0; i < prog->nkids; i++) {
		ast *s = prog->kids[i];
		if (s->kind == AST_ASSIGN)
			continue;
		print_root("root", eval(s, global));
	}
}
