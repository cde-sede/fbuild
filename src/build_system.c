#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/syscall.h>

#include "cparser.h"
#include "ast.h"
#include "fbuild.h"
#include "graph.h"

#define die(message, ...) do { fprintf(stderr, "error:" message, ##__VA_ARGS__); exit(1); } while (0)

void lex(char *s)
{
	int line = 1;
	int col  = 1;

	/* command-literal state: backticks switch the lexer into a
	 * posix-bash-ish word mode. cmd != 0 while inside `...`. depth
	 * counts open {/( inside the command: depth 0 = bare command text
	 * (bash words), depth > 0 = an interpolation expression `{ ... }`
	 * lexed with the normal token rules (so `...` is TOK_SPLAT there). */
	int cmd   = 0;
	int depth = 0;

	/* interpolated-string mode: str != 0 while inside "..."/'...' that
	 * contains a `{ }`. strq holds the opening quote. depth is shared with
	 * command mode (it counts open {/( of the interpolation expression). */
	int str  = 0;
	char strq = 0;

	/* normal-mode paren nesting: a newline inside ( ... ) is insignificant
	 * (an expression continuation), so it is not emitted as a statement
	 * separator. Statements at depth 0 still end on a newline. */
	int pdepth = 0;

	for (size_t i = 0; s[i];) {
		size_t start = i;
		int sl = line;
		int sc = col;

		unsigned char c = s[i];

		/* ---- command / interpolated-string mode ---------------- */
		if (cmd || str) {
			/* depth > 0: inside an interpolation expression. ws is
			 * insignificant; track {/}/(/) nesting and fall through
			 * to the normal token rules for everything else. */
			if (depth > 0) {
				if (c == ' ' || c == '\t' || c == '\r') {
					i++;
					col++;
					continue;
				}

				if (c == '\n') {
					i++;
					line++;
					col = 1;
					continue;
				}

				if (c == '{') {
					emit(TOK_LBRACE, s + i, 1,
					     line, col, line, col);
					depth++;
					i++;
					col++;
					continue;
				}

				if (c == '}') {
					emit(TOK_RBRACE, s + i, 1,
					     line, col, line, col);
					depth--;
					i++;
					col++;
					continue;
				}

				if (c == '(') {
					emit(TOK_LPAREN, s + i, 1,
					     line, col, line, col);
					depth++;
					i++;
					col++;
					continue;
				}

				if (c == ')') {
					emit(TOK_RPAREN, s + i, 1,
					     line, col, line, col);
					depth--;
					i++;
					col++;
					continue;
				}

				/* fall through to normal lexing below */
			} else if (cmd) {
				/* command text: ws/newlines never emit tokens */
				if (c == ' ' || c == '\t' || c == '\r') {
					i++;
					col++;
					continue;
				}

				if (c == '\n') {
					i++;
					line++;
					col = 1;
					continue;
				}

				/* closing backtick ends the command */
				if (c == '`') {
					emit(TOK_BACKTICK, s + i, 1,
					     line, col, line, col);
					cmd = 0;
					i++;
					col++;
					continue;
				}

				/* interpolation open: `{ expr }` */
				if (c == '{') {
					emit(TOK_LBRACE, s + i, 1,
					     line, col, line, col);
					depth++;
					i++;
					col++;
					continue;
				}

				/* otherwise: a bash word (quote-aware) up to
				 * the next separator / interpolation / ` */
				start = i;
				sc = col;

				while (s[i]) {
					char ch = s[i];

					if (ch == ' ' || ch == '\t' ||
					    ch == '\r' || ch == '\n' ||
					    ch == '`' || ch == '{')
						break;

					if (ch == '\'' || ch == '"') {
						char q = ch;
						i++;
						col++;
						while (s[i] && s[i] != q) {
							if (s[i] == '\\' &&
							    s[i+1]) {
								i += 2;
								col += 2;
								continue;
							}
							if (s[i] == '\n')
								die("newline in command string");
							i++;
							col++;
						}
						if (!s[i])
							die("unterminated string in command");
						i++; /* closing quote */
						col++;
						continue;
					}

					i++;
					col++;
				}

				emit(TOK_COMMAND,
				     s + start,
				     i - start,
				     sl, sc,
				     line, col - 1);
				continue;
			} else {
				/* interpolated-string text: ws is significant. */
				if (c == strq) {
					emit(TOK_STR_END, s + i, 1,
					     line, col, line, col);
					str = 0;
					i++;
					col++;
					continue;
				}

				if (c == '{') {
					emit(TOK_LBRACE, s + i, 1,
					     line, col, line, col);
					depth++;
					i++;
					col++;
					continue;
				}

				start = i;
				sc = col;
				while (s[i] && s[i] != strq && s[i] != '{') {
					if (s[i] == '\\' && s[i+1]) {
						i += 2;
						col += 2;
						continue;
					}
					if (s[i] == '\n')
						die("newline in string");
					i++;
					col++;
				}
				if (!s[i])
					die("unterminated string");

				emit(TOK_STRTEXT,
				     s + start,
				     i - start,
				     sl, sc,
				     line, col - 1);
				continue;
			}
		}

		/* whitespace */
		if (c == ' ' || c == '\t' || c == '\r') {
			i++;
			col++;
			continue;
		}

		/* newline */
		if (c == '\n') {
			if (pdepth == 0)            /* not inside (...) -> separator */
				emit(TOK_NEWLINE, s + i, 1,
				     line, col, line, col);

			i++;
			line++;
			col = 1;
			continue;
		}

		/* comments */
		if (c == '#') {
			while (s[i] && s[i] != '\n') {
				i++;
				col++;
			}
			continue;
		}

		/* command literals: `gcc -o {...OBJ}` */
		if (c == '`') {
			emit(TOK_BACKTICK, s + i, 1,
			     line, col, line, col);
			cmd   = 1;
			depth = 0;
			i++;
			col++;
			continue;
		}

		/* path literals: [src/_*.c] */
		if (c == '[') {
			i++;
			col++;

			start = i;
			sc = col;

			while (s[i] && s[i] != ']') {
				if (s[i] == '\n')
					die("newline inside path literal");

				i++;
				col++;
			}

			if (!s[i])
				die("unterminated path literal");

			emit(TOK_PATH,
			     s + start,
			     i - start,
			     sl, sc,
			     line, col);

			i++; /* closing ] */
			col++;
			continue;
		}

		/* strings */
		if (c == '"' || c == '\'') {
			char q = c;

			/* scan ahead: a DOUBLE-quoted string with an unescaped `{`
			 * becomes an interpolated string (str mode). Single-quoted
			 * strings are raw (no interpolation), so JSON and other text
			 * with braces is literal. */
			int interp = 0;
			if (q == '"')
				for (size_t j = i + 1; s[j] && s[j] != q; j++) {
					if (s[j] == '\\' && s[j+1]) { j++; continue; }
					if (s[j] == '\n') break;
					if (s[j] == '{') { interp = 1; break; }
				}

			if (interp) {
				emit(TOK_STR_BEG, s + i, 1,
				     line, col, line, col);
				str  = 1;
				strq = q;
				depth = 0;
				i++;
				col++;
				continue;
			}

			i++;
			col++;

			start = i;
			sc = col;

			while (s[i] && s[i] != q) {
				if (s[i] == '\\' && s[i+1]) {
					i += 2;
					col += 2;
					continue;
				}

				if (s[i] == '\n')
					die("newline in string");

				i++;
				col++;
			}

			if (!s[i])
				die("unterminated string");

			emit(TOK_STRING,
			     s + start,
			     i - start,
			     sl, sc,
			     line, col);

			i++; /* closing quote */
			col++;
			continue;
		}

		/* longest-match operators first */

		if (!strncmp(s + i, "...", 3)) {
			emit(TOK_SPLAT, s + i, 3,
			     sl, sc, line, col + 2);
			i += 3;
			col += 3;
			continue;
		}

		if (!strncmp(s + i, "->", 2)) {
			emit(TOK_ARROW, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		if (!strncmp(s + i, "~=", 2)) {
			emit(TOK_TILDE_ASSIGN, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		if (!strncmp(s + i, "|>", 2)) {
			emit(TOK_PIPE, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		if (!strncmp(s + i, "||", 2)) {
			emit(TOK_OROR, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		if (!strncmp(s + i, "//", 2)) {
			emit(TOK_MORPH, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		if (!strncmp(s + i, "<=", 2)) {
			emit(TOK_LE, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		if (!strncmp(s + i, ">>", 2)) {
			emit(TOK_SEQ, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		if (!strncmp(s + i, ">=", 2)) {
			emit(TOK_GE, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		if (!strncmp(s + i, "==", 2)) {
			emit(TOK_EQ, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		if (!strncmp(s + i, "!=", 2)) {
			emit(TOK_NE, s + i, 2,
			     sl, sc, line, col + 1);
			i += 2;
			col += 2;
			continue;
		}

		/* numbers */

		if (isdigit(c)) {
			while (isdigit((unsigned char)s[i])) {
				i++;
				col++;
			}

			if (s[i] == '.' &&
			    isdigit((unsigned char)s[i+1])) {

				i++;
				col++;

				while (isdigit((unsigned char)s[i])) {
					i++;
					col++;
				}

				emit(TOK_FLOAT,
				     s + start,
				     i - start,
				     sl, sc,
				     line, col - 1);
			} else {
				emit(TOK_INT,
				     s + start,
				     i - start,
				     sl, sc,
				     line, col - 1);
			}

			continue;
		}

		/* identifiers and keywords */

		if (isalpha(c) || c == '_') {
			while (isalnum((unsigned char)s[i]) ||
			       s[i] == '_') {
				i++;
				col++;
			}

			size_t len = i - start;
			int kind = TOK_ID;

			if (len == 4 &&
			    !strncmp(s + start, "when", 4))
				kind = TOK_WHEN;

			else if (len == 5 &&
			         !strncmp(s + start, "where", 5))
				kind = TOK_WHERE;

			else if (len == 3 &&
			         !strncmp(s + start, "and", 3))
				kind = TOK_AND;

			else if (len == 2 &&
			         !strncmp(s + start, "or", 2))
				kind = TOK_OR;

			else if (len == 3 &&
			         !strncmp(s + start, "not", 3))
				kind = TOK_NOT;

			else if (len == 4 &&
			         !strncmp(s + start, "true", 4))
				kind = TOK_BOOL;

			else if (len == 5 &&
			         !strncmp(s + start, "false", 5))
				kind = TOK_BOOL;

			emit(kind,
			     s + start,
			     len,
			     sl, sc,
			     line, col - 1);

			continue;
		}

		/* single-character tokens */

		switch (c) {
		case '=':
			emit(TOK_ASSIGN, s+i, 1,
			     sl, sc, line, col);
			break;

		case '~':
			emit(TOK_TILDE, s+i, 1,
			     sl, sc, line, col);
			break;

		case '.':
			emit(TOK_DOT, s+i, 1,
			     sl, sc, line, col);
			break;

		case ',':
			emit(TOK_COMMA, s+i, 1,
			     sl, sc, line, col);
			break;

		case ':':
			emit(TOK_COLON, s+i, 1,
			     sl, sc, line, col);
			break;

		case '*':
			emit(TOK_STAR, s+i, 1,
			     sl, sc, line, col);
			break;

		case '+':
			emit(TOK_PLUS, s+i, 1,
			     sl, sc, line, col);
			break;

		case '-':
			emit(TOK_MINUS, s+i, 1,
			     sl, sc, line, col);
			break;

		case '/':
			emit(TOK_SLASH, s+i, 1,
			     sl, sc, line, col);
			break;

		case '%':
			emit(TOK_PERCENT, s+i, 1,
			     sl, sc, line, col);
			break;

		case '&':
			emit(TOK_AMP, s+i, 1,
			     sl, sc, line, col);
			break;

		case '@':
			emit(TOK_AT, s+i, 1,
			     sl, sc, line, col);
			break;

		case '(':
			pdepth++;
			emit(TOK_LPAREN, s+i, 1,
			     sl, sc, line, col);
			break;

		case ')':
			if (pdepth > 0)
				pdepth--;
			emit(TOK_RPAREN, s+i, 1,
			     sl, sc, line, col);
			break;

		case '<':
			emit(TOK_LT, s+i, 1,
			     sl, sc, line, col);
			break;

		case '>':
			emit(TOK_GT, s+i, 1,
			     sl, sc, line, col);
			break;

		case '{':                       /* interpolation / .{expr} */
			pdepth++;
			emit(TOK_LBRACE, s+i, 1,
			     sl, sc, line, col);
			break;

		case '}':
			if (pdepth > 0)
				pdepth--;
			emit(TOK_RBRACE, s+i, 1,
			     sl, sc, line, col);
			break;

		default:
			die("unexpected character '%c' at %d:%d",
			    c, line, col);
		}

		i++;
		col++;
	}

	if (cmd)
		die("unterminated command literal");
	if (str)
		die("unterminated string literal");

	emit(TOK_EOF, s, 0, line, col, line, col);
}


static inline int printable(tok_kind k) {
	return k != NONE
	    && k != TOK_EOF
	    && k != TOK_NEWLINE;
}


/* ===========================================================================
 * Parser: recursive descent over the ABNF precedence ladder + atoms.
 * Deferred: lambda, tuple, constructor, function-call, field, morphism.
 * ======================================================================== */

const char *ast_name[] = {
	[AST_PROGRAM]="program",
	[AST_ASSIGN]="assign",
	[AST_EAGER]="eager",
	[AST_FORCE]="force",
	[AST_TUPLE]="tuple",
	[AST_MAPLIT]="maplit",
	[AST_PARSER]="parser",
	[AST_LAMBDA]="lambda",
	[AST_PARAMS]="params",
	[AST_SEQ]="seq",
	[AST_PIPE]="pipe",
	[AST_GUARD]="guard",
	[AST_WHERE]="where",
	[AST_BINDING]="binding",
	[AST_WHEN]="when",
	[AST_COMPARE]="compare",
	[AST_ARITH]="arith",
	[AST_LOGICAL]="logical",
	[AST_NOT]="not",
	[AST_MORPHISM]="morphism",
	[AST_OBJNEW]="objnew",
	[AST_METHODDEF]="methoddef",
	[AST_CALL]="call",
	[AST_FIELD]="field",
	[AST_CONSTRUCT]="construct",
	[AST_CFIELD]="cfield",
	[AST_SPLAT]="splat",
	[AST_METADATA]="metadata",
	[AST_CAPTURE]="capture",
	[AST_PROJECTION]="projection",
	[AST_COMMAND]="command",
	[AST_CMDTEXT]="cmdtext",
	[AST_ID]="id",
	[AST_INT]="int",
	[AST_FLOAT]="float",
	[AST_BOOL]="bool",
	[AST_STRING]="string",
	[AST_STRTEXT]="strtext",
	[AST_PATH]="path",
};

static ast *node(ast_kind k) {
	ast *n = a_alloc(sizeof *n);
	n->kind = k;
	return n;
}

static void push(ast *parent, ast *child) {
	if (parent->nkids == parent->cap) {
		size_t nc = parent->cap ? parent->cap * 2 : 4;
		parent->kids = a_grow(parent->kids,
		                      parent->nkids * sizeof *parent->kids,
		                      nc * sizeof *parent->kids);
		parent->cap = nc;
	}
	parent->kids[parent->nkids++] = child;
}

static int at_eof(void) {
	token *t = tok_cur();
	return !t || t->kind == TOK_EOF;     /* TOK_EOF == NONE == 0 */
}

static int is_compare_op(tok_kind k) {
	return k == TOK_LT || k == TOK_GT
	    || k == TOK_LE || k == TOK_GE
	    || k == TOK_EQ || k == TOK_NE;
}

/* "ignore newlines unless they break syntax": skip newlines wherever the
 * grammar needs another token (after a committed operator/delimiter). */
static void skip_nl(void) {
	while (peek(.kind = TOK_NEWLINE))
		tok_advance();
}

/* kind of the next non-newline token at/after index `from`, without consuming.
 * Lets a continuation operator on the following line attach to the current
 * expression. */
static tok_kind sig_from(size_t from) {
	for (size_t i = from; i < p->pos; i++)
		if (p->tokens[i].kind != TOK_NEWLINE)
			return p->tokens[i].kind;
	return TOK_EOF;
}
static tok_kind sig_kind(void) { return sig_from(cur); }

static ast *parse_expression(void);
static ast *parse_expr_no_tuple(void);
static ast *parse_command(void);
static ast *parse_string(void);
static ast *parse_postfix(void);
static ast *parse_comparison(void);
static ast *parse_guarded(void);
static ast *parse_pipe(void);
static ast *parse_lambda(void);
static int  lambda_ahead(int multi);

/* a lambda body extends across commas (forming a tuple) only at statement /
 * paren level; inside a call-argument or tuple element a comma separates, so
 * the body must stop. This flag tracks "we are in a no-tuple context". */
static int g_no_tuple = 0;

/* operand of |> / >>: a single-parameter lambda needs no parens here
 * (`x |> f -> body`), otherwise a guarded expression. */
static ast *parse_pipe_rhs(void) {
	return lambda_ahead(0) ? parse_lambda() : parse_guarded();
}

/* atom: the leaf-ish productions (primary-expression minus the postfix
 * forms call/field/constructor/morphism, which parse_postfix layers on). */
static ast *parse_atom(void) {
	token *t = tok_cur();
	if (!t || t->kind == TOK_EOF)
		die("unexpected end of input in expression");

	switch (t->kind) {
	case TOK_AT: {                       /* capture: "@" / "@" id */
		ast *n = node(AST_CAPTURE);
		n->tok = t;
		tok_advance();
		if (peek(.kind = TOK_ID)) {
			ast *id = node(AST_ID);
			id->tok = tok_cur();
			tok_advance();
			push(n, id);
		}
		return n;
	}
	case TOK_DOT: {                      /* projection: ".id" / ".int" */
		ast *n = node(AST_PROJECTION);
		tok_advance();
		if (peek(.kind = TOK_ID) || peek(.kind = TOK_INT))
			n->tok = tok_cur();
		else
			die("expected name or index after '.' at %d:%d",
			    t->start.line, t->start.col);
		tok_advance();
		return n;
	}
	case TOK_AMP: {                      /* metadata: "&" primary */
		ast *n = node(AST_METADATA);
		n->tok = t;
		tok_advance();
		push(n, parse_postfix());
		return n;
	}
	case TOK_TILDE: {                    /* force re-eval: "~" postfix */
		ast *n = node(AST_FORCE);
		n->tok = t;
		tok_advance();
		push(n, parse_postfix());
		return n;
	}
	case TOK_SPLAT: {                    /* splat: "..." pipe-expr */
		/* spread the whole following expression up to pipe level, so
		 * `...xs |> f` means `...(xs |> f)` and `...g(x) when c` means
		 * `...(g(x) when c)`. Stops at "," / ">>" / lambda / assignment. */
		ast *n = node(AST_SPLAT);
		n->tok = t;
		tok_advance();
		push(n, parse_pipe());
		return n;
	}
	case TOK_BACKTICK:
		return parse_command();

	case TOK_STR_BEG:
		return parse_string();

	case TOK_ID:    { ast *n = node(AST_ID);     n->tok = t; tok_advance(); return n; }
	case TOK_INT:   { ast *n = node(AST_INT);    n->tok = t; tok_advance(); return n; }
	case TOK_FLOAT: { ast *n = node(AST_FLOAT);  n->tok = t; tok_advance(); return n; }
	case TOK_BOOL:  { ast *n = node(AST_BOOL);   n->tok = t; tok_advance(); return n; }
	case TOK_STRING:{ ast *n = node(AST_STRING); n->tok = t; tok_advance(); return n; }
	case TOK_PATH:  { ast *n = node(AST_PATH);   n->tok = t; tok_advance(); return n; }

	case TOK_LBRACE: {                   /* map literal: { key: val, ... } */
		ast *n = node(AST_MAPLIT);
		tok_advance();                   /* "{" */
		skip_nl();
		if (!peek(.kind = TOK_RBRACE)) {
			do {
				skip_nl();
				if (peek(.kind = TOK_RBRACE)) break;   /* trailing comma */
				ast *entry = node(AST_TUPLE);
				push(entry, parse_expr_no_tuple());    /* key */
				skip_nl();
				expect(.kind = TOK_COLON);
				skip_nl();
				push(entry, parse_expr_no_tuple());    /* value */
				push(n, entry);
				skip_nl();
			} while (accept(.kind = TOK_COMMA));
		}
		skip_nl();
		expect(.kind = TOK_RBRACE);
		return n;
	}

	case TOK_LPAREN: {                   /* "(" expression ")" */
		tok_advance();
		ast *n = parse_expression();
		expect(.kind = TOK_RPAREN);
		if (n->kind == AST_SPLAT) {  /* (...x): materialize the spread tuple */
			ast *tup = node(AST_TUPLE);
			push(tup, n);
			n = tup;
		}
		return n;
	}
	default:
		die("unexpected token '%.*s' at %d:%d",
		    (int)t->len, t->string, t->start.line, t->start.col);
	}
	return NULL; /* unreachable */
}

/* command: "`" *( command-word / interpolation ) "`" */
static ast *parse_command(void) {
	expect(.kind = TOK_BACKTICK);
	ast *n = node(AST_COMMAND);

	while (!at_eof() && !peek(.kind = TOK_BACKTICK)) {
		if (peek(.kind = TOK_COMMAND)) {
			ast *w = node(AST_CMDTEXT);
			w->tok = tok_cur();
			tok_advance();
			push(n, w);
		} else if (accept(.kind = TOK_LBRACE)) {
			push(n, parse_expression());   /* interpolation body */
			expect(.kind = TOK_RBRACE);
		} else {
			token *t = tok_cur();
			die("unexpected token in command at %d:%d",
			    t->start.line, t->start.col);
		}
	}

	expect(.kind = TOK_BACKTICK);
	return n;
}

/* interpolated string: STR_BEG *( STRTEXT / "{" expression "}" ) STR_END.
 * Produces an AST_STRING whose kids are text chunks and interpolated exprs. */
static ast *parse_string(void) {
	expect(.kind = TOK_STR_BEG);
	ast *n = node(AST_STRING);

	while (!at_eof() && !peek(.kind = TOK_STR_END)) {
		if (peek(.kind = TOK_STRTEXT)) {
			ast *w = node(AST_STRTEXT);
			w->tok = tok_cur();
			tok_advance();
			push(n, w);
		} else if (accept(.kind = TOK_LBRACE)) {
			push(n, parse_expression());   /* interpolation body */
			expect(.kind = TOK_RBRACE);
		} else {
			token *t = tok_cur();
			die("unexpected token in string at %d:%d",
			    t->start.line, t->start.col);
		}
	}

	expect(.kind = TOK_STR_END);
	return n;
}

/* function-call: callee "(" [ argument-list ] ")" */
static ast *parse_call(ast *callee) {
	expect(.kind = TOK_LPAREN);
	ast *n = node(AST_CALL);
	push(n, callee);
	if (!peek(.kind = TOK_RPAREN)) {
		do {
			push(n, parse_expr_no_tuple());   /* args, not a tuple */
		} while (accept(.kind = TOK_COMMA));
	}
	expect(.kind = TOK_RPAREN);
	return n;
}

/* field access: base "." ( identifier / integer / "{" expression "}" ) */
static ast *parse_field(ast *base) {
	expect(.kind = TOK_DOT);
	ast *n = node(AST_FIELD);
	if (accept(.kind = TOK_LBRACE)) {           /* dynamic ".{expr}" */
		push(n, base);                          /* kids[0] = base   */
		push(n, parse_expression());            /* kids[1] = index  */
		expect(.kind = TOK_RBRACE);
		return n;
	}
	if (peek(.kind = TOK_ID) || peek(.kind = TOK_INT))
		n->tok = tok_cur();
	else
		die("expected field name or index after '.'");
	tok_advance();
	push(n, base);
	return n;
}

/* "id <" begins a constructor (not a comparison) only when the next token
 * looks like a constructor-field: ">", "*", or "identifier =". */
static int constructor_ahead(void) {
	if (!peek(.kind = TOK_LT))
		return 0;
	token *t1 = tok_at(1);
	if (!t1)
		return 0;
	if (t1->kind == TOK_GT || t1->kind == TOK_STAR)
		return 1;
	if (t1->kind == TOK_ID) {
		token *t2 = tok_at(2);
		return t2 && t2->kind == TOK_ASSIGN;
	}
	return 0;
}

/* constructor: identifier "<" [ constructor-fields ] ">"
 * field values stay below comparison so a "<" / ">" cannot be mistaken for
 * the closing bracket. */
static ast *parse_constructor(ast *base) {
	ast *n = node(AST_CONSTRUCT);
	n->tok = base->tok;   /* type name */
	ast_free(base);

	expect(.kind = TOK_LT);
	if (!peek(.kind = TOK_GT)) {
		do {
			ast *f = node(AST_CFIELD);
			if (accept(.kind = TOK_STAR)) {       /* "*" expression */
				push(f, parse_postfix());
			} else {                              /* name "=" value */
				f->tok = expect(.kind = TOK_ID);
				expect(.kind = TOK_ASSIGN);
				push(f, parse_postfix());
			}
			push(n, f);
		} while (accept(.kind = TOK_COMMA));
	}
	expect(.kind = TOK_GT);
	return n;
}

/* Tuple<e, ..., *...spread>: build a tuple from positional values and splats.
 * Reuses AST_TUPLE, so eval/infer treat it exactly like a tuple literal. */
static ast *parse_tuple_ctor(ast *base) {
	ast *n = node(AST_TUPLE);
	ast_free(base);
	expect(.kind = TOK_LT);
	if (!peek(.kind = TOK_GT)) {
		do {
			skip_nl();
			if (accept(.kind = TOK_SPLAT)) {
				ast *s = node(AST_SPLAT);
				push(s, parse_postfix());
				push(n, s);
			} else {
				push(n, parse_postfix());
			}
			skip_nl();
		} while (accept(.kind = TOK_COMMA));
	}
	expect(.kind = TOK_GT);
	return n;
}

/* Parser<schema>: schema is a single map expression. The result is applied to
 * an args list (a following call), or to the program's args when used bare. */
static ast *parse_parser_ctor(ast *base) {
	ast *n = node(AST_PARSER);
	ast_free(base);
	expect(.kind = TOK_LT);
	skip_nl();
	push(n, parse_postfix());   /* schema map (postfix, so '>' isn't a compare) */
	skip_nl();
	expect(.kind = TOK_GT);
	return n;
}

/* Map<(k,v), (k2,v2), ...>: each element is a (key,value) pair expression.
 * Reuses AST_MAPLIT, identical eval to the `{ k: v }` literal. */
static ast *parse_map_ctor(ast *base) {
	ast *n = node(AST_MAPLIT);
	ast_free(base);
	expect(.kind = TOK_LT);
	if (!peek(.kind = TOK_GT)) {
		do {
			skip_nl();
			if (peek(.kind = TOK_GT)) break;   /* trailing comma */
			push(n, parse_postfix());
			skip_nl();
		} while (accept(.kind = TOK_COMMA));
	}
	expect(.kind = TOK_GT);
	return n;
}

/* postfix-expression: atom *( call / field / constructor / morphism ) */
static ast *parse_postfix(void) {
	ast *base = parse_atom();

	for (;;) {
		if (peek(.kind = TOK_LPAREN)) {
			base = parse_call(base);
		} else if (peek(.kind = TOK_DOT)) {
			base = parse_field(base);
		} else if (base->kind == AST_ID && peek(.kind = TOK_LT) &&
		           base->tok->len == 5 &&
		           !strncmp(base->tok->string, "Tuple", 5)) {
			base = parse_tuple_ctor(base);    /* Tuple<...> -> a tuple */
		} else if (base->kind == AST_ID && peek(.kind = TOK_LT) &&
		           base->tok->len == 3 &&
		           !strncmp(base->tok->string, "Map", 3)) {
			base = parse_map_ctor(base);      /* Map<(k,v), ...> -> a map */
		} else if (base->kind == AST_ID && peek(.kind = TOK_LT) &&
		           base->tok->len == 6 &&
		           !strncmp(base->tok->string, "Parser", 6)) {
			base = parse_parser_ctor(base);   /* Parser<schema> -> arg parser */
		} else if (base->kind == AST_ID && constructor_ahead()) {
			base = parse_constructor(base);
		} else if (sig_kind() == TOK_MORPH) {     /* path "//" path */
			skip_nl();
			ast *m = node(AST_MORPHISM);
			m->tok = tok_cur();
			tok_advance();
			skip_nl();
			push(m, base);
			push(m, parse_atom());
			base = m;
		} else {
			break;
		}
	}
	return base;
}

/* multiplicative: postfix *( ("*"|"/"|"%") postfix ), left-assoc */
static ast *parse_multiplicative(void) {
	ast *left = parse_postfix();
	for (tok_kind k = sig_kind();
	     k == TOK_STAR || k == TOK_SLASH || k == TOK_PERCENT;
	     k = sig_kind()) {
		skip_nl();
		ast *n = node(AST_ARITH);
		n->tok = tok_cur();
		tok_advance();
		skip_nl();
		push(n, left);
		push(n, parse_postfix());
		left = n;
	}
	return left;
}

/* additive: multiplicative *( ("+"|"-") multiplicative ), left-assoc */
static ast *parse_additive(void) {
	ast *left = parse_multiplicative();
	for (tok_kind k = sig_kind(); k == TOK_PLUS || k == TOK_MINUS; k = sig_kind()) {
		skip_nl();
		ast *n = node(AST_ARITH);
		n->tok = tok_cur();
		tok_advance();
		skip_nl();
		push(n, left);
		push(n, parse_multiplicative());
		left = n;
	}
	return left;
}

/* comparison-expression: additive *( compare-op additive ), left-assoc.
 * A compare-op on the next line continues the expression. */
static ast *parse_comparison(void) {
	ast *left = parse_additive();
	while (is_compare_op(sig_kind())) {
		skip_nl();
		ast *n = node(AST_COMPARE);
		n->tok = tok_cur();
		tok_advance();
		skip_nl();
		push(n, left);
		push(n, parse_additive());
		left = n;
	}
	return left;
}

/* logical negation: "not" not-expr | comparison (binds tighter than and/or,
 * looser than comparison: `not a == b` is `not (a == b)`) */
static ast *parse_not(void) {
	if (sig_kind() == TOK_NOT) {
		skip_nl();
		ast *n = node(AST_NOT);
		n->tok = tok_cur();
		tok_advance();
		skip_nl();
		push(n, parse_not());
		return n;
	}
	return parse_comparison();
}

/* logical conjunction: not-expr *( "and" not-expr ), left-assoc */
static ast *parse_and(void) {
	ast *left = parse_not();
	while (sig_kind() == TOK_AND) {
		skip_nl();
		ast *n = node(AST_LOGICAL);
		n->tok = tok_cur();
		tok_advance();
		skip_nl();
		push(n, left);
		push(n, parse_not());
		left = n;
	}
	return left;
}

/* logical disjunction: and-expr *( "or" and-expr ), left-assoc (binds looser
 * than `and`, both looser than comparison) */
static ast *parse_logical(void) {
	ast *left = parse_and();
	while (sig_kind() == TOK_OR || sig_kind() == TOK_OROR) {
		skip_nl();
		ast *n = node(AST_LOGICAL);
		n->tok = tok_cur();
		tok_advance();
		skip_nl();
		push(n, left);
		push(n, parse_and());
		left = n;
	}
	return left;
}

/* guarded-expression: logical [ where-clause ] [ when-clause ].
 * where/when may begin on the next line. */
static ast *parse_guarded(void) {
	ast *body  = parse_logical();
	ast *where = NULL;
	ast *whn   = NULL;

	if (sig_kind() == TOK_WHERE) {
		skip_nl();
		tok_advance();
		skip_nl();
		where = node(AST_WHERE);
		do {
			skip_nl();
			token *name = expect(.kind = TOK_ID);
			skip_nl();
			expect(.kind = TOK_ASSIGN);
			skip_nl();
			ast *b = node(AST_BINDING);
			b->tok = name;
			push(b, parse_logical());      /* below guard tail */
			push(where, b);
		} while (sig_kind() == TOK_COMMA && (skip_nl(), accept(.kind = TOK_COMMA)));
	}

	if (sig_kind() == TOK_WHEN) {
		skip_nl();
		tok_advance();
		skip_nl();
		whn = node(AST_WHEN);
		push(whn, parse_expression());
	}

	if (!where && !whn)
		return body;

	ast *g = node(AST_GUARD);
	push(g, body);
	push(g, where);   /* may be NULL */
	push(g, whn);     /* may be NULL */
	return g;
}

/* pipe-expression: guarded *( "|>" guarded ) */
static ast *parse_pipe(void) {
	ast *left = parse_guarded();
	if (sig_kind() != TOK_PIPE)
		return left;

	ast *n = node(AST_PIPE);
	push(n, left);
	while (sig_kind() == TOK_PIPE) {
		skip_nl();
		accept(.kind = TOK_PIPE);
		skip_nl();
		push(n, parse_pipe_rhs());   /* allows `|> f -> body` without parens */
	}
	return n;
}

/* sequence-expression: pipe *( ">>" pipe ) */
static ast *parse_sequence(void) {
	ast *left = parse_pipe();
	if (sig_kind() != TOK_SEQ)
		return left;

	ast *n = node(AST_SEQ);
	push(n, left);
	while (sig_kind() == TOK_SEQ) {
		skip_nl();
		accept(.kind = TOK_SEQ);
		skip_nl();
		push(n, parse_pipe());
	}
	return n;
}

/* Look past a parameter-list for "->" without consuming, to tell a lambda
 * apart from a tuple/expression that merely starts the same way. When `multi`
 * is 0, stop at a comma (a comma there is an argument/tuple separator, not a
 * lambda parameter separator) -- so only a single-parameter lambda matches. */
static int lambda_ahead(int multi) {
	size_t i = cur;
#define LA_SKIPNL() while (i < p->pos && p->tokens[i].kind == TOK_NEWLINE) i++
	for (;;) {
		LA_SKIPNL();
		token *t = i < p->pos ? &p->tokens[i] : NULL;
		if (!t)
			return 0;

		if (t->kind == TOK_AT) {                 /* "@" / "@" id */
			i++; LA_SKIPNL();
			if (i < p->pos && p->tokens[i].kind == TOK_ID)
				i++;
		} else if (t->kind == TOK_DOT) {         /* ".id" / ".int" */
			i++; LA_SKIPNL();
			if (i >= p->pos || (p->tokens[i].kind != TOK_ID && p->tokens[i].kind != TOK_INT))
				return 0;
			i++;
		} else if (t->kind == TOK_ID) {          /* identifier */
			i++;
		} else {
			return 0;
		}

		LA_SKIPNL();
		token *a = i < p->pos ? &p->tokens[i] : NULL;
		if (!a)
			return 0;
		if (a->kind == TOK_ARROW)
			return 1;
		if (a->kind == TOK_COMMA && multi) {
			i++;
			continue;
		}
		return 0;
	}
#undef LA_SKIPNL
}

/* parameter: identifier / projection / capture */
static ast *parse_parameter(void) {
	token *t = tok_cur();
	if (t->kind == TOK_AT) {                 /* capture */
		ast *n = node(AST_CAPTURE);
		n->tok = t;
		tok_advance();
		if (peek(.kind = TOK_ID)) {
			ast *id = node(AST_ID);
			id->tok = tok_cur();
			tok_advance();
			push(n, id);
		}
		return n;
	}
	if (t->kind == TOK_DOT) {                /* projection */
		ast *n = node(AST_PROJECTION);
		tok_advance();
		if (peek(.kind = TOK_ID) || peek(.kind = TOK_INT))
			n->tok = tok_cur();
		else
			die("expected name or index after '.' in parameter");
		tok_advance();
		return n;
	}
	ast *n = node(AST_ID);                   /* identifier */
	n->tok = expect(.kind = TOK_ID);
	return n;
}

/* lambda: parameter-list "->" expression */
static ast *parse_lambda(void) {
	ast *params = node(AST_PARAMS);
	do {
		skip_nl();
		push(params, parse_parameter());
	} while (sig_kind() == TOK_COMMA && (skip_nl(), accept(.kind = TOK_COMMA)));
	skip_nl();
	expect(.kind = TOK_ARROW);
	skip_nl();

	ast *n = node(AST_LAMBDA);
	push(n, params);
	/* in a no-tuple context the comma separates args/elements, so the body
	 * stops; otherwise the body may itself be a tuple (`x -> a, b`). */
	push(n, g_no_tuple ? parse_expr_no_tuple() : parse_expression());
	return n;
}

/* tuple/argument element: a single-parameter lambda (its params can't cross
 * the element-separating comma) or a sequence-expression. */
static ast *parse_expr_no_tuple(void) {
	int save = g_no_tuple;
	g_no_tuple = 1;
	ast *r = lambda_ahead(0) ? parse_lambda() : parse_sequence();
	g_no_tuple = save;
	return r;
}

/* expression: assignment / lambda / tuple / sequence-expression */
static ast *parse_expression(void) {
	int save = g_no_tuple;
	g_no_tuple = 0;                  /* tuple-allowing context */
	ast *res;

	/* type NAME(params) = body : a constructor for a tagged object. Desugars to
	 * `NAME = params -> <object of class NAME with `body` as its fields>`.
	 * Contextual: `type` stays a normal identifier unless it starts this shape. */
	if (peek(.kind = TOK_ID, .string = "type")
	 && _match((struct obj){ .kind = TOK_ID }, cur + 1)
	 && _match((struct obj){ .kind = TOK_LPAREN }, cur + 2)) {
		tok_advance();                              /* 'type' */
		token *nm = expect(.kind = TOK_ID);         /* class name */
		expect(.kind = TOK_LPAREN);
		ast *params = node(AST_PARAMS);
		skip_nl();
		if (!peek(.kind = TOK_RPAREN)) {
			do { skip_nl(); push(params, parse_parameter()); }
			while (sig_kind() == TOK_COMMA && (skip_nl(), accept(.kind = TOK_COMMA)));
			skip_nl();
		}
		expect(.kind = TOK_RPAREN);
		skip_nl();
		expect(.kind = TOK_ASSIGN);
		skip_nl();
		ast *obj = node(AST_OBJNEW);
		obj->tok = nm;
		push(obj, parse_expression());              /* fields body */
		ast *lam = node(AST_LAMBDA);
		push(lam, params);
		push(lam, obj);
		ast *asn = node(AST_ASSIGN);
		asn->tok = nm;
		push(asn, lam);
		g_no_tuple = save;
		return asn;
	}

	/* method NAME.MNAME = body : attach a method to class NAME. */
	if (peek(.kind = TOK_ID, .string = "method")
	 && _match((struct obj){ .kind = TOK_ID },     cur + 1)
	 && _match((struct obj){ .kind = TOK_DOT },    cur + 2)
	 && _match((struct obj){ .kind = TOK_ID },     cur + 3)
	 && _match((struct obj){ .kind = TOK_ASSIGN }, cur + 4)) {
		tok_advance();                              /* 'method' */
		token *cname = expect(.kind = TOK_ID);
		expect(.kind = TOK_DOT);
		token *mname = expect(.kind = TOK_ID);
		skip_nl();
		expect(.kind = TOK_ASSIGN);
		skip_nl();
		ast *cid = node(AST_ID);
		cid->tok = cname;
		ast *md = node(AST_METHODDEF);
		md->tok = mname;
		push(md, cid);
		push(md, parse_expression());               /* the method (a lambda) */
		g_no_tuple = save;
		return md;
	}

	if (peek(.kind = TOK_ID) && sig_from(cur + 1) == TOK_ASSIGN) {
		ast *a = node(AST_ASSIGN);
		a->tok = tok_cur();
		tok_advance();   /* identifier */
		skip_nl();
		tok_advance();   /* "=" */
		skip_nl();
		push(a, parse_expression());
		res = a;
	} else if (peek(.kind = TOK_ID) && sig_from(cur + 1) == TOK_TILDE_ASSIGN) {
		ast *a = node(AST_EAGER);
		a->tok = tok_cur();
		tok_advance();   /* identifier */
		skip_nl();
		tok_advance();   /* "~=" */
		skip_nl();
		push(a, parse_expression());
		res = a;
	} else {
		/* a multi-parameter lambda (commas in its param list) is only
		 * recognized here at the top level, not inside an argument/tuple
		 * element. The lambda may still be one element of a tuple, so fall
		 * through to the comma check. */
		ast *first = lambda_ahead(1) ? parse_lambda() : parse_expr_no_tuple();
		if (sig_kind() != TOK_COMMA) {
			res = first;
		} else {
			ast *n = node(AST_TUPLE);          /* tuple: e *( "," e ) */
			push(n, first);
			while (sig_kind() == TOK_COMMA) {
				skip_nl();
				accept(.kind = TOK_COMMA);
				skip_nl();
				push(n, parse_expr_no_tuple());
			}
			res = n;
		}
	}

	g_no_tuple = save;
	return res;
}

/* program: *( statement *newline ) */
ast *parse_program(void) {
	cur = 0;
	ast *prog = node(AST_PROGRAM);

	while (accept(.kind = TOK_NEWLINE))
		;

	while (!at_eof()) {
		push(prog, parse_expression());

		if (!at_eof() && !peek(.kind = TOK_NEWLINE)) {
			token *t = tok_cur();
			die("expected newline after statement at %d:%d",
			    t->start.line, t->start.col);
		}
		while (accept(.kind = TOK_NEWLINE))
			;
	}
	return prog;
}

/* ---- AST pretty printer (tree connectors + optional color) ------------- */

static int ast_color;   /* set from isatty at print time */

#define C_DIM  (ast_color ? "\033[2m"   : "")   /* connectors      */
#define C_KIND (ast_color ? "\033[1;36m": "")   /* node kind       */
#define C_TOK  (ast_color ? "\033[0;33m": "")   /* token text      */
#define C_OFF  (ast_color ? "\033[0m"   : "")

/* print "<kind> <tok>" with color, no leading prefix */
static void ast_label(ast *n) {
	printf("%s%s%s", C_KIND, ast_name[n->kind], C_OFF);
	if (n->tok && n->tok->len)
		printf(" %s%.*s%s",
		       C_TOK, (int)n->tok->len, n->tok->string, C_OFF);
	putchar('\n');
}

static void ast_kids(ast *n, const char *prefix) {
	size_t total = 0;
	for (size_t i = 0; i < n->nkids; i++)
		if (n->kids[i])
			total++;            /* skip NULL slots (e.g. guard) */

	size_t seen = 0;
	for (size_t i = 0; i < n->nkids; i++) {
		ast *c = n->kids[i];
		if (!c)
			continue;

		int last = (++seen == total);
		printf("%s%s%s%s", C_DIM, prefix, last ? "└─ " : "├─ ", C_OFF);
		ast_label(c);

		char *cp = a_alloc(strlen(prefix) + 8);
		sprintf(cp, "%s%s", prefix, last ? "   " : "│  ");
		ast_kids(c, cp);
	}
}

void ast_print(ast *n, int depth) {
	(void)depth;
	if (!n)
		return;
	ast_color = isatty(fileno(stdout));
	ast_label(n);           /* root, no connector */
	ast_kids(n, "");
}

/* AST lives in the program arena; nothing to free per node. */
void ast_free(ast *n) { (void)n; }
