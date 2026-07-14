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
	AST_LOGICAL,    /* tok = "and"/"or", kids[0]=left, [1]=right   */
	AST_NOT,        /* tok = "not", kids[0]=operand               */
	AST_MORPHISM,   /* tok = "//", kids[0]=left, [1]=right         */

	AST_OBJNEW,     /* tok = class name, kids[0] = fields body; tags obj */
	AST_METHODDEF,  /* tok = method name, kids[0]=class id, kids[1]=body */

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

	AST_ID,         /* tok                                         */
	AST_INT,        /* tok                                         */
	AST_FLOAT,      /* tok                                         */
	AST_BOOL,       /* tok = "true" / "false"                      */
	AST_STRING,     /* tok (plain) | kids = parts (interpolated)   */
	AST_STRTEXT,    /* tok = literal text chunk in an interp string*/
	AST_PATH,       /* tok                                         */
} ast_kind;


typedef enum {
	RET_NONE = 0,

	RET_INT,
	RET_FLOAT,
	RET_STRING,
	RET_BOOL,

	RET_FILE,       /* a path/file: a record of name/stem/.../mtime/size  */
	RET_COMMAND,    /* a build action produced by a `...` command literal */

	RET_LIST,       /* homogeneous, length UNKNOWN: elem = children[0]   */
	RET_TUPLE,      /* fixed arity, heterogeneous: children[0..nchildren)*/
	RET_FUNCTOR,    /* function: children[0]=param, children[1]=result   */

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

/* type inference (walk.c) */
obj_type *infer_type(ast *node);
int       infer_program(ast *prog);   /* returns type-error count */
void      obj_type_print(obj_type *t);

/* executor (run.c). argc/argv are the program's CLI args; args[2..] become
 * the language-level `args` list. */
void      run_program(ast *prog, int argc, char **argv);

/* version string (run.c, embedded via build.h metadata). */
const char *fbuild_version(void);

#endif /* AST_H */
