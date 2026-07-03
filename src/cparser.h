#ifndef CPARSER_H
#define CPARSER_H

#include <stddef.h>

#define MAX_TOKENS 1024

/* Program-lifetime arena (backed by cds_lib/build.h, implemented in run.c,
 * owned/created by parser.c). Every allocation in the front-end and the executor
 * comes from it; nothing is freed individually, the whole arena is released at
 * exit. See arena_init / arena_fini. */
void  arena_init(void);
void  arena_fini(void);
void *a_alloc(size_t n);
void *a_grow(void *old, size_t oldn, size_t newn);
char *a_dup(const char *s);
char *a_dupn(const char *s, size_t n);

#define peek(...)   _peek((struct obj){__VA_ARGS__})
#define accept(...) _accept((struct obj){__VA_ARGS__})
#define expect(...) _expect((struct obj){__VA_ARGS__})

typedef enum {
	NONE = 0,
	TOK_EOF = 0,
	TOK_NEWLINE,

	TOK_ID,
	TOK_INT,
	TOK_FLOAT,
	TOK_BOOL,       // true / false
	TOK_STRING,
	TOK_BACKTICK,   // `  command literal delimiter
	TOK_COMMAND,
	TOK_PATH,

	TOK_WHEN,
	TOK_WHERE,
	TOK_AND,        // and
	TOK_OR,         // or
	TOK_OROR,       // ||  value-or (left if truthy else right)
	TOK_NOT,        // not

	TOK_ASSIGN,     // =
	TOK_TILDE_ASSIGN, // ~=  eager / re-definable assign
	TOK_TILDE,      // ~   force re-eval (ignore memoization)
	TOK_ARROW,      // ->
	TOK_PIPE,       // |>
	TOK_MORPH,      // //
	TOK_SPLAT,      // ...
	TOK_SEQ,        // >>  sequential

	TOK_LT,
	TOK_GT,
	TOK_LE,
	TOK_GE,
	TOK_EQ,
	TOK_NE,

	TOK_DOT,        // .
	TOK_COMMA,      // ,
	TOK_COLON,      // :
	TOK_STAR,       // *  (also multiply)
	TOK_AMP,        // &
	TOK_AT,         // @
	TOK_PLUS,       // +
	TOK_MINUS,      // -
	TOK_SLASH,      // /  (single; // is morphism)
	TOK_PERCENT,    // %

	TOK_LPAREN,
	TOK_RPAREN,
	TOK_LBRACE,     // {  interpolation open
	TOK_RBRACE,     // }  interpolation close
	TOK_LBRACK,
	TOK_RBRACK,
	TOK_LANGLE,
	TOK_RANGLE,
	TOK_STR_BEG,    // opening quote of an interpolated string
	TOK_STR_END,    // closing quote of an interpolated string
	TOK_STRTEXT,    // literal text chunk inside an interpolated string
} tok_kind;

typedef struct {
	tok_kind kind;
	struct { int col, line; } start;
	struct { int col, line; } end;
	char  *string;   /* slice into input, NOT NUL-terminated */
	size_t len;
	char  *owned;    /* lazy NUL-terminated copy, NULL until tok_str() */
} token;

struct parser {
	char  *src;      /* borrowed input the tokens slice into */
	size_t pos;      /* token count */
	token  tokens[];
};

/* match spec: unset fields (kind==NONE, string==NULL) mean "any" */
struct obj { int kind; char *string; };

extern struct parser *p;
extern size_t         cur;
extern const char    *kind_name[];

void   lexer(char *restrict s);
void   lex(char *s);          /* the funcbuild lexer (build_system.c) */
void   emit(int kind, char *str, size_t len, int sl, int sc, int el, int ec);

int    _match(struct obj want, size_t idx);
int    _peek(struct obj want);
int    _accept(struct obj want);
token *_expect(struct obj want);

char  *tok_str(token *t);
void   tok_free(token *t);
void   parser_free(struct parser *pp);

/* raw cursor, for hand-written (recursive-descent / precedence) parsers */
token *tok_cur(void);
token *tok_at(size_t off);
void   tok_advance(void);
void   tok_back(void);

#endif /* CPARSER_H */
