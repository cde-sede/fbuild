#ifndef AST_H
#define AST_H

#include <stddef.h>
#include "cparser.h"

/* AST for the full ABNF grammar. */
typedef enum {
	AST_PROGRAM,    /* kids = statements                           */
	AST_ASSIGN,     /* tok = name id, kids[0] = value              */
	AST_EAGER,      /* tok = name id, kids[0] = value; "~=" eager  */
	AST_STORE,      /* tok = "<-", kids[0]=lvalue (field chain),   */
	                /* kids[1]=rhs; writes into a _GLOBALS slot     */
	AST_FORCE,      /* tok = "~", kids[0]=operand; re-eval, no memo */
	AST_TUPLE,      /* kids = elements (>= 2)                      */
	AST_LISTLIT,    /* kids = elements; List<e, ...> -> a real List */
	AST_MAPLIT,     /* kids = entries, each AST_TUPLE(key,value)   */
	AST_PARSER,     /* kids[0] = schema map; Parser<schema>(argv)  */
	AST_LAMBDA,     /* kids[0]=AST_PARAMS, kids[1]=body            */
	AST_PARAMS,     /* kids = parameters                           */
	AST_SEQ,        /* kids = operands of ">>"                     */
	AST_PIPE,       /* kids = operands of "|>"                     */
	AST_GUARD,      /* kids[0]=body, [1]=where|NULL, [2]=when|NULL */
	AST_WHERE,      /* kids = bindings                             */
	AST_BINDING,    /* tok = name id, kids[0] = value              */
	AST_WHEN,       /* kids[0] = condition                         */
	AST_COMPARE,    /* tok = op token, kids[0]=left, [1]=right     */
	AST_ARITH,      /* tok = +|-|*|/|%, kids[0]=left, [1]=right    */
	AST_NEG,        /* tok = '-', kids[0]=operand; unary negation  */
	AST_LOGICAL,    /* tok = "and"/"or", kids[0]=left, [1]=right   */
	AST_NOT,        /* tok = "not", kids[0]=operand               */
	AST_MORPHISM,   /* tok = "//", kids[0]=left, [1]=right         */

	AST_OBJNEW,     /* tok = class name, kids[0] = fields body; tags obj */
	AST_METHODDEF,  /* tok = method name, kids[0]=class id, kids[1]=body */
	AST_METHODCOPY, /* method NEW.* = OLD; kids[0]=dest class id, [1]=src id */

	AST_CALL,       /* kids[0]=callee, kids[1..]=args              */
	AST_FIELD,      /* tok = field id/int, kids[0]=base;            */
	                /* dynamic ".{expr}": tok=NULL, kids[1]=index   */
	AST_CONSTRUCT,  /* tok = type id, kids = constructor fields    */
	AST_CFIELD,     /* tok = name (named) | NULL (splat "*"),      */
	                /* kids[0] = value                             */

	AST_SPLAT,      /* kids[0] = expression                        */
	AST_METADATA,   /* tok = "&", kids[0] = primary                */
	AST_CAPTURE,    /* tok = "@", kids[0]=id|absent                */
	AST_PROJECTION, /* tok = ".x"/".0" target (id or int)         */
	AST_COMMAND,    /* kids = parts (CMDTEXT or interpolation expr)*/
	AST_CMDTEXT,    /* tok = command word slice                    */

	AST_ID,         /* tok; as a param decl: kids[0] = optional        */
	                /* AST_TYPEEXPR annotation (parens-only, see       */
	                /* parse_parameter's allow_type)                   */
	AST_INT,        /* tok                                         */
	AST_FLOAT,      /* tok                                         */
	AST_BOOL,       /* tok = "true" / "false"                      */
	AST_NIL,        /* tok = "nil" | "(" ; the Unit value           */
	AST_STRING,     /* tok (plain) | kids = parts (interpolated)   */
	AST_STRTEXT,    /* tok = literal text chunk in an interp string*/
	AST_PATH,       /* tok                                         */
	AST_TYPEEXPR,   /* tok = base type name; kids = generic args,  */
	                /* e.g. List<Int> -> tok "List", kids[0]="Int" */
} ast_kind;


typedef enum {
	RET_NONE = 0,

	RET_INT,
	RET_FLOAT,
	RET_STRING,
	RET_BOOL,
	RET_NUMERIC,    /* Int or Float, unresolved which -- same informal notion  */
	                /* arithmetic operand errors already use in prose; unifies */
	                /* with RET_INT/RET_FLOAT/itself, nothing else            */

	RET_FILE,       /* a path/file: a record of name/stem/.../mtime/size  */
	RET_COMMAND,    /* a build action produced by a `...` command literal */

	RET_LIST,       /* homogeneous, length UNKNOWN: elem = children[0]   */
	RET_TUPLE,      /* fixed arity, heterogeneous: children[0..nchildren)*/
	RET_FUNCTOR,    /* function: children[0]=param, children[1]=result   */

	RET_OBJECT,     /* type NAME(...) = ... instance; class_name = NAME. Field
	                 * shapes (fields/children, RET_FILE-record depth) are
	                 * tracked when the fields body is a statically-shaped map
	                 * literal (walk.c's infer_objnew_schema) or a
	                 * Parser<schema> record (infer_parser_schema) -- NULL
	                 * fields for any other/dynamic shape, in which case this
	                 * exists so hover/errors can still name the class instead
	                 * of falling back to "?". Methods are never tracked here
	                 * regardless (separate namespace, no static checking --
	                 * see walk.c's class_has_method/g_methodsyms). */

	RET_VAR,        /* not yet resolved (inference placeholder)          */
} ret_type;

/* A type is a tree. Scalars have no children. RET_LIST has exactly one
 * child (the element type) and an unknown length. RET_TUPLE has a fixed
 * number of children, each with its own (possibly different) type. */
typedef struct obj_type {
	ret_type          ret;
	struct obj_type **children;
	size_t            nchildren;
	token           **fields;   /* record (RET_FILE) field names, parallel
	                             * to children; NULL for non-records       */
	token            *class_name;   /* RET_OBJECT only: the `type NAME(...)` name */
	const char       *var_label;    /* RET_VAR only: cosmetic display name ("a",
	                                 * "b", ...) for a hand-written generic
	                                 * builtin signature -- purely for hover
	                                 * (walk.c's core_generic_signature), never
	                                 * consulted by inference/unify; the SAME
	                                 * obj_type* reused at every occurrence of
	                                 * one type variable in one signature is
	                                 * what makes them print as the same label */
	int               is_variadic;  /* RET_LIST only: display-only marker for a
	                                 * capture (@rest) param's slot -- prints as
	                                 * "*T" (T = the element type) instead of
	                                 * "List<T>", to read as "zero or more T"
	                                 * distinctly from an ordinary List-typed
	                                 * param. Never consulted by inference. */
} obj_type;

typedef struct ast {
	ast_kind     kind;
	token       *tok;       /* principal token (leaf/op/name), or NULL */
	struct ast **kids;
	size_t       nkids;
	size_t       cap;
	obj_type    *type;      /* inferred type, NULL until inference runs */

	/* eval caches (run.c). Nodes are arena-zeroed, so 0 == "not cached". */
	long         ival;      /* AST_INT: parsed literal (ival_set != 0)      */
	int          ival_set;
	const void  *rg_val;    /* AST_CALL: memoized fb_Reg* (may cache a miss) */
	void        *rg_state;  /* owning fb_State the memo is valid for         */
	int          rg_gen;    /* registration generation the memo saw          */
} ast;

extern const char *ast_name[];

ast *parse_program(void);   /* parses the global token stream from cur=0 */
void ast_print(ast *n, int depth);
void ast_free(ast *n);

/* Structured diagnostics (positions are 1-based, matching token.start/end).
 * Used by tools (e.g. an LSP daemon) that need type errors as data instead
 * of stderr text. */
typedef enum { FB_SEV_ERROR = 1, FB_SEV_WARNING = 2 } fb_Severity;
typedef struct {
	int line, col, end_line, end_col;   /* 1-based; end == start if unknown */
	fb_Severity severity;
	char message[256];
} fb_Diag;

/* type inference (walk.c) */
obj_type *infer_type(ast *node);
int       infer_program(ast *prog);   /* returns type-error count; prints to stderr */
/* Same as infer_program, but appends each error as an fb_Diag instead of
 * printing it. Writes at most max_diags entries; returns the total error
 * count (may exceed max_diags). */
int       infer_program_collect(ast *prog, fb_Diag *diags, int max_diags);
void      obj_type_print(obj_type *t);
char     *obj_type_sprint(obj_type *t);   /* string variant, arena-allocated */

/* ---- source-position tooling (walk.c): hover / go-to-definition --------
 * Read-only over an already-parsed, already-inferred `prog` (i.e. call after
 * infer_program()/infer_program_collect() succeeded) -- no execution. */

typedef struct { int line, col; } fb_Pos;
typedef struct { fb_Pos start, end; } fb_Span;   /* 1-based; {0,0}/{0,0} = empty/unknown */

fb_Span ast_span(const ast *n);   /* source range covering n and all its descendants */

typedef enum { FB_DEF_LOCAL = 0, FB_DEF_TOPLEVEL = 1, FB_DEF_METHOD = 2 } fb_DefKind;
typedef struct { fb_Span def_span; fb_DefKind kind; } fb_Definition;

/* What's at (line, col) in prog, 1-based, resolves to. Writes at most
 * max_out entries (>1 only for an ambiguous method name across classes,
 * FB_DEF_METHOD). Returns the count written, or 0 if there's no definition
 * site in this file (builtin, or unresolved). */
int fb_find_definition(ast *prog, int line, int col, fb_Definition *out, int max_out);

/* `markdown` is 1 when `text` uses markdown formatting (a builtin's
 * fleshed-out doc: fenced signature + prose sections) -- 0 for a plain
 * inferred-type string, so a caller can pick the right LSP MarkupKind. */
typedef struct { char *text; fb_Span span; int markdown; } fb_HoverInfo;

/* Inferred type of the node at (line, col), rendered as a string (see
 * obj_type_sprint). Returns 0 if no node covers that position. */
int fb_hover_at(ast *prog, int line, int col, fb_HoverInfo *out);

/* ---- completion (walk.c) -------------------------------------------------
 * v1: bare-identifier completion only, no dot-triggered field/method
 * completion (the parser has no error recovery, so "obj." doesn't parse). */

typedef enum {
	FB_COMP_BUILTIN      = 0,
	FB_COMP_TOPLEVEL_FN  = 1,   /* top-level def/method whose RHS is a lambda */
	FB_COMP_TOPLEVEL_VAR = 2,   /* top-level def whose RHS is a plain value   */
	FB_COMP_LOCAL        = 3,   /* lambda param / where-binding in scope      */
	FB_COMP_FIELD        = 4,   /* record field (RET_FILE) of a base.<here>   */
} fb_CompKind;
typedef struct {
	const char *name;    /* NUL-terminated, arena-owned */
	fb_CompKind kind;
	const char *detail;  /* signature/type string, or NULL if none available */
} fb_CompItem;

/* Candidates for a bare-identifier position (line, col), 1-based: every
 * known builtin, every top-level def/method name, and (if a scope can be
 * located at line/col) every local in scope there. No prefix filtering --
 * caller/client filters against what's already typed. Writes at most
 * max_out entries into out, returns the count written. `prog` may be NULL
 * (last parse was a syntax error) -- still returns builtins; the
 * top-level/local tiers just contribute nothing. */
int fb_complete_at(ast *prog, int line, int col, fb_CompItem *out, int max_out);

/* Field/method candidates for a base.<here> expression at (line, col),
 * 1-based, in an already-successfully-parsed `prog` -- i.e. "base.partial"
 * with at least the dot already parsed (a bare trailing "base." with
 * nothing typed after the dot is a syntax error under this grammar and
 * won't produce a prog at all; the caller is expected to splice a
 * placeholder identifier and reparse before calling this, see fbuildd's
 * handle_completion). Returns -1 if (line, col) isn't a base.field
 * position at all (caller should fall back to fb_complete_at); returns a
 * count >= 0 (possibly 0, if the receiver's type has no known
 * fields/methods, e.g. a dynamic Map) otherwise -- 0 is a real answer, not
 * "try something else". */
int fb_complete_field_at(ast *prog, int line, int col, fb_CompItem *out, int max_out);

/* ---- document symbols (walk.c) ------------------------------------------
 * Outline of every top-level def/method in `prog`, for
 * textDocument/documentSymbol. */

typedef enum {
	FB_SYM_FUNCTION = 0,   /* top-level def whose RHS is a lambda           */
	FB_SYM_VARIABLE = 1,   /* top-level def whose RHS is a plain value      */
	FB_SYM_CLASS    = 2,   /* `type X(...) = ...` constructor def           */
	FB_SYM_METHOD   = 3,   /* `method Class.name = ...`                     */
} fb_SymKind;
typedef struct {
	const char *name;        /* NUL-terminated, arena-owned */
	fb_SymKind  kind;
	fb_Span     span;        /* the whole def/method statement */
	const char *container;   /* class name for FB_SYM_METHOD, else NULL */
} fb_DocSymbol;

/* Writes at most max_out entries into out, returns the count written. */
int fb_document_symbols(ast *prog, fb_DocSymbol *out, int max_out);

/* ---- signature help (walk.c) --------------------------------------------
 * Not tied to any specific AST node -- callers locate the enclosing call
 * and active argument index themselves (a text-level scan is enough, no
 * AST/reparse needed, so this works even mid-typing an unclosed call). */

/* Best-effort functor signature for callee `name` (len bytes, raw text --
 * not a token). `prog` may be NULL: builtins still resolve; a user-defined
 * top-level function only resolves if `prog` is set. NULL if unknown. */
obj_type *fb_lookup_signature(ast *prog, const char *name, size_t len);

/* Individual parameter types of a RET_FUNCTOR (flattening a Tuple-shaped
 * param slot into one entry per element). Writes at most max_out into out,
 * returns the count written (0 if `t` isn't RET_FUNCTOR). */
int fb_functor_params(obj_type *t, obj_type **out, int max_out);

/* executor (run.c). argc/argv are the program's CLI args; args[2..] become
 * the language-level `args` list. */
void      run_program(ast *prog, int argc, char **argv);

/* version string (run.c, embedded via build.h metadata). */
const char *fbuild_version(void);

#endif /* AST_H */
