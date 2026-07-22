/* fbuildd: an LSP (Language Server Protocol) daemon for funcbuild (.fb)
 * scripts. Thin driver over libfbuild, same spirit as main.c: talks
 * JSON-RPC over stdio (the standard LSP transport), keeps one fb_State per
 * open document (see fb_check, src/run.c), and answers diagnostics/hover/
 * go-to-definition requests from the real parser/type-checker -- not a
 * separate hand-maintained grammar.
 *
 * v1 scope: textDocument/{didOpen,didChange,didClose,hover,definition,
 * documentSymbol,completion}, publishDiagnostics. Completion covers bare identifiers
 * (builtins/top-level defs/locals) and base.field/method access -- the
 * latter needs a placeholder-splice trick for a bare trailing "base."
 * (nothing typed after the dot yet), since the parser has no error
 * recovery and that's otherwise a syntax error; see handle_completion.
 *
 * Nothing but framed JSON-RPC messages may go to stdout; anything else
 * (errors, notices) goes to stderr. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>

#include "fbuild.h"

/* ============================================================================
 * Minimal JSON: just enough to read LSP requests and hand-serialize LSP
 * responses. Independent of libfbuild's own JSON codec (run_builtins.c),
 * which is internal, static, and built around the language's own rval*
 * value model -- not worth exposing across that boundary for this.
 * ========================================================================== */

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jkind;

typedef struct jval {
	jkind kind;
	int b;
	double num;
	char *str;
	struct jval **items; size_t n;    /* JV_ARR */
	char **keys; struct jval **vals; size_t nk;   /* JV_OBJ */
} jval;

typedef struct { const char *s; size_t pos, len; } jparser;

static jval *jnew(jkind k) { jval *v = calloc(1, sizeof *v); v->kind = k; return v; }

static void jfree(jval *v) {
	if (!v) return;
	switch (v->kind) {
	case JV_STR: free(v->str); break;
	case JV_ARR:
		for (size_t i = 0; i < v->n; i++) jfree(v->items[i]);
		free(v->items);
		break;
	case JV_OBJ:
		for (size_t i = 0; i < v->nk; i++) { free(v->keys[i]); jfree(v->vals[i]); }
		free(v->keys); free(v->vals);
		break;
	default: break;
	}
	free(v);
}

static void jskip_ws(jparser *p) {
	while (p->pos < p->len && strchr(" \t\r\n", p->s[p->pos])) p->pos++;
}

static jval *jparse_value(jparser *p);

/* assumes p->s[p->pos] == '"' */
static jval *jparse_string_raw(jparser *p) {
	p->pos++;
	size_t cap = 32, len = 0;
	char *buf = malloc(cap);
	while (p->pos < p->len && p->s[p->pos] != '"') {
		char c = p->s[p->pos++];
		if (c == '\\' && p->pos < p->len) {
			char e = p->s[p->pos++];
			if (e == 'u' && p->pos + 4 <= p->len) {
				unsigned cp = 0;
				for (int i = 0; i < 4; i++) {
					char h = p->s[p->pos + i];
					cp <<= 4;
					if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
				}
				p->pos += 4;
				if (len + 3 >= cap) { cap *= 2; buf = realloc(buf, cap); }
				if (cp < 0x80) {
					buf[len++] = (char)cp;
				} else if (cp < 0x800) {
					buf[len++] = (char)(0xC0 | (cp >> 6));
					buf[len++] = (char)(0x80 | (cp & 0x3F));
				} else {
					buf[len++] = (char)(0xE0 | (cp >> 12));
					buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
					buf[len++] = (char)(0x80 | (cp & 0x3F));
				}
				continue;
			}
			switch (e) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			default:  c = e;    break;   /* '"', '\\', '/' and anything else: literal */
			}
		}
		if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
		buf[len++] = c;
	}
	if (p->pos < p->len) p->pos++;   /* closing quote */
	buf[len] = 0;
	jval *v = jnew(JV_STR);
	v->str = buf;
	return v;
}

static jval *jparse_array(jparser *p) {
	p->pos++;   /* '[' */
	jval *v = jnew(JV_ARR);
	jskip_ws(p);
	if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return v; }
	for (;;) {
		jval *item = jparse_value(p);
		v->items = realloc(v->items, (v->n + 1) * sizeof *v->items);
		v->items[v->n++] = item;
		jskip_ws(p);
		if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; jskip_ws(p); continue; }
		break;
	}
	if (p->pos < p->len && p->s[p->pos] == ']') p->pos++;
	return v;
}

static jval *jparse_object(jparser *p) {
	p->pos++;   /* '{' */
	jval *v = jnew(JV_OBJ);
	jskip_ws(p);
	if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return v; }
	for (;;) {
		jskip_ws(p);
		jval *key = (p->pos < p->len && p->s[p->pos] == '"') ? jparse_string_raw(p) : jnew(JV_STR);
		if (!key->str) key->str = strdup("");
		jskip_ws(p);
		if (p->pos < p->len && p->s[p->pos] == ':') p->pos++;
		jval *val = jparse_value(p);
		v->keys = realloc(v->keys, (v->nk + 1) * sizeof *v->keys);
		v->vals = realloc(v->vals, (v->nk + 1) * sizeof *v->vals);
		v->keys[v->nk] = key->str;   /* steal */
		v->vals[v->nk] = val;
		v->nk++;
		free(key);
		jskip_ws(p);
		if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
		break;
	}
	if (p->pos < p->len && p->s[p->pos] == '}') p->pos++;
	return v;
}

static jval *jparse_value(jparser *p) {
	jskip_ws(p);
	if (p->pos >= p->len) return jnew(JV_NULL);
	char c = p->s[p->pos];
	if (c == '"') return jparse_string_raw(p);
	if (c == '{') return jparse_object(p);
	if (c == '[') return jparse_array(p);
	if (c == 't' && p->pos + 4 <= p->len && !strncmp(p->s + p->pos, "true", 4)) {
		p->pos += 4; jval *v = jnew(JV_BOOL); v->b = 1; return v;
	}
	if (c == 'f' && p->pos + 5 <= p->len && !strncmp(p->s + p->pos, "false", 5)) {
		p->pos += 5; jval *v = jnew(JV_BOOL); v->b = 0; return v;
	}
	if (c == 'n' && p->pos + 4 <= p->len && !strncmp(p->s + p->pos, "null", 4)) {
		p->pos += 4; return jnew(JV_NULL);
	}
	size_t start = p->pos;
	if (p->pos < p->len && p->s[p->pos] == '-') p->pos++;
	while (p->pos < p->len && (isdigit((unsigned char)p->s[p->pos]) || strchr(".eE+-", p->s[p->pos])))
		p->pos++;
	char tmp[64];
	size_t n = p->pos - start; if (n >= sizeof tmp) n = sizeof tmp - 1;
	memcpy(tmp, p->s + start, n); tmp[n] = 0;
	jval *v = jnew(JV_NUM);
	v->num = atof(tmp);
	return v;
}

static jval *jget(jval *obj, const char *key) {
	if (!obj || obj->kind != JV_OBJ) return NULL;
	for (size_t i = 0; i < obj->nk; i++)
		if (!strcmp(obj->keys[i], key)) return obj->vals[i];
	return NULL;
}
static const char *jstr(jval *v) { return (v && v->kind == JV_STR) ? v->str : NULL; }
static int jint(jval *v, int def) { return (v && v->kind == JV_NUM) ? (int)v->num : def; }

/* ---- growable output buffer + hand-written LSP-shape serializers -------- */

typedef struct { char *buf; size_t len, cap; } dbuf;

static void dbuf_init(dbuf *d) { d->cap = 4096; d->buf = malloc(d->cap); d->len = 0; d->buf[0] = 0; }
static void dbuf_ensure(dbuf *d, size_t extra) {
	if (d->len + extra + 1 > d->cap) {
		while (d->len + extra + 1 > d->cap) d->cap *= 2;
		d->buf = realloc(d->buf, d->cap);
	}
}
static void dbuf_putn(dbuf *d, const char *s, size_t n) {
	dbuf_ensure(d, n);
	memcpy(d->buf + d->len, s, n);
	d->len += n;
	d->buf[d->len] = 0;
}
static void dbuf_puts(dbuf *d, const char *s) { dbuf_putn(d, s, strlen(s)); }
static void dbuf_putf(dbuf *d, const char *fmt, ...) {
	char tmp[512];
	va_list ap; va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n > 0) dbuf_putn(d, tmp, (size_t)n);
}
static void dbuf_putjstr(dbuf *d, const char *s) {
	dbuf_puts(d, "\"");
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
		switch (*p) {
		case '"':  dbuf_puts(d, "\\\""); break;
		case '\\': dbuf_puts(d, "\\\\"); break;
		case '\n': dbuf_puts(d, "\\n");  break;
		case '\r': dbuf_puts(d, "\\r");  break;
		case '\t': dbuf_puts(d, "\\t");  break;
		default:
			if (*p < 0x20) dbuf_putf(d, "\\u%04x", *p);
			else { char c = (char)*p; dbuf_putn(d, &c, 1); }
		}
	}
	dbuf_puts(d, "\"");
}

/* 1-based fb_Pos/token positions -> 0-based LSP positions; 0 (unknown) clamps to 0 */
static int z(int x) { return x > 0 ? x - 1 : 0; }

/* ============================================================================
 * stdio transport: Content-Length-framed JSON-RPC (LSP base protocol).
 * ========================================================================== */

static void send_message(const char *json, size_t len) {
	printf("Content-Length: %zu\r\n\r\n", len);
	fwrite(json, 1, len, stdout);
	fflush(stdout);
}

static void write_id(dbuf *b, jval *id) {
	if (id && id->kind == JV_STR)      dbuf_putjstr(b, id->str);
	else if (id && id->kind == JV_NUM) dbuf_putf(b, "%d", (int)id->num);
	else                                dbuf_puts(b, "null");
}

static void send_result(jval *id, const char *result_json) {
	dbuf b; dbuf_init(&b);
	dbuf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
	write_id(&b, id);
	dbuf_puts(&b, ",\"result\":");
	dbuf_puts(&b, result_json);
	dbuf_puts(&b, "}");
	send_message(b.buf, b.len);
	free(b.buf);
}

static void send_notification(const char *method, const char *params_json) {
	dbuf b; dbuf_init(&b);
	dbuf_puts(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
	dbuf_putjstr(&b, method);
	dbuf_puts(&b, ",\"params\":");
	dbuf_puts(&b, params_json);
	dbuf_puts(&b, "}");
	send_message(b.buf, b.len);
	free(b.buf);
}

/* read "Header: value\r\n" lines up to the blank line; only Content-Length
 * matters for v1 (Content-Type, if sent, is ignored). Returns 0 and sets
 * *out_len on success, -1 on EOF/malformed. */
static int read_headers(long *out_len) {
	*out_len = -1;
	char line[1024];
	for (;;) {
		if (!fgets(line, sizeof line, stdin)) return -1;
		size_t n = strlen(line);
		while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
		if (n == 0) break;   /* blank line: end of headers */
		if (!strncasecmp(line, "Content-Length:", 15)) {
			const char *v = line + 15;
			while (*v == ' ') v++;
			*out_len = atol(v);
		}
	}
	return *out_len >= 0 ? 0 : -1;
}

/* ============================================================================
 * Workspace / documents.
 * ========================================================================== */

static char *g_root;          /* workspace root filesystem path, or NULL */
static int   g_got_shutdown;

static char *uri_to_path(const char *uri) {
	if (!strncmp(uri, "file://", 7)) uri += 7;
	char *out = malloc(strlen(uri) + 1);
	size_t j = 0;
	for (size_t i = 0; uri[i]; i++) {
		if (uri[i] == '%' && isxdigit((unsigned char)uri[i+1]) && isxdigit((unsigned char)uri[i+2])) {
			char h[3] = { uri[i+1], uri[i+2], 0 };
			out[j++] = (char)strtol(h, NULL, 16);
			i += 2;
		} else {
			out[j++] = uri[i];
		}
	}
	out[j] = 0;
	return out;
}

/* Load every *.so under <root>/build/ -- matches the output convention the
 * root .fb script's own `plugins` target produces. Best-effort: a plugin
 * that fails to load is silently skipped rather than failing the whole
 * check (a daemon shouldn't go dark over one bad/stale plugin). */
static void load_workspace_plugins(fb_State *S) {
	if (!g_root) { fprintf(stderr, "fbuildd: no workspace root, skipping plugin discovery\n"); return; }
	char dir[4096];
	snprintf(dir, sizeof dir, "%s/build", g_root);
	DIR *dp = opendir(dir);
	if (!dp) { fprintf(stderr, "fbuildd: can't open plugin dir '%s': %s\n", dir, strerror(errno)); return; }
	struct dirent *e;
	int found = 0;
	while ((e = readdir(dp))) {
		size_t n = strlen(e->d_name);
		if (n > 3 && !strcmp(e->d_name + n - 3, ".so") && strlen(dir) + n + 2 < 4096) {
			char path[4096];
			snprintf(path, sizeof path, "%.*s/%.*s", 2000, dir, 2000, e->d_name);
			found++;
			if (fb_load_plugin(S, path))
				fprintf(stderr, "fbuildd: failed to load plugin '%s': %s\n", path, fb_error_msg(S));
			else
				fprintf(stderr, "fbuildd: loaded plugin '%s'\n", path);
		}
	}
	closedir(dp);
	if (!found) fprintf(stderr, "fbuildd: no *.so found under '%s'\n", dir);
}

#define MAX_DOCS 64
#define MAX_DIAGS 256

typedef struct {
	char     *uri;
	fb_State *S;       /* recreated on every open/change, see doc_recheck */
	ast      *prog;    /* NULL if the last check hit a syntax error */
	char     *text;    /* raw source of the last check -- for the completion
	                    * dangling-dot splice fallback, see handle_completion */
	fb_Diag   diags[MAX_DIAGS];
	int       ndiags;
} fb_Doc;

static fb_Doc g_docs[MAX_DOCS];
static int    g_ndocs;

static fb_Doc *doc_find(const char *uri) {
	for (int i = 0; i < g_ndocs; i++)
		if (!strcmp(g_docs[i].uri, uri)) return &g_docs[i];
	return NULL;
}

static fb_Doc *doc_open(const char *uri) {
	fb_Doc *d = doc_find(uri);
	if (d) return d;
	if (g_ndocs >= MAX_DOCS) return NULL;
	d = &g_docs[g_ndocs++];
	memset(d, 0, sizeof *d);
	d->uri = strdup(uri);
	return d;
}

static void doc_close(const char *uri) {
	for (int i = 0; i < g_ndocs; i++) {
		if (!strcmp(g_docs[i].uri, uri)) {
			if (g_docs[i].S) fb_close(g_docs[i].S);
			free(g_docs[i].uri);
			free(g_docs[i].text);
			g_docs[i] = g_docs[--g_ndocs];
			return;
		}
	}
}

/* The interpreter's arena is a bump allocator with no free -- reusing one
 * fb_State across many edits would grow it unboundedly for the life of the
 * editing session, so each check gets a fresh state instead. */
static void doc_recheck(fb_Doc *d, const char *text) {
	if (d->S) fb_close(d->S);
	d->S = fb_new();
	load_workspace_plugins(d->S);
	d->prog = NULL;
	free(d->text);
	d->text = strdup(text);
	int n = fb_check(d->S, text, d->uri, d->diags, MAX_DIAGS, &d->prog);
	d->ndiags = n < 0 ? 0 : (n > MAX_DIAGS ? MAX_DIAGS : n);
}

static void send_diagnostics(fb_Doc *d) {
	dbuf b; dbuf_init(&b);
	dbuf_puts(&b, "{\"uri\":");
	dbuf_putjstr(&b, d->uri);
	dbuf_puts(&b, ",\"diagnostics\":[");
	for (int i = 0; i < d->ndiags; i++) {
		fb_Diag *g = &d->diags[i];
		int line = z(g->line), col = z(g->col);
		int eline = g->end_line > 0 ? z(g->end_line) : line;
		int ecol  = g->end_col  > 0 ? z(g->end_col)  : col + 1;
		if (i) dbuf_puts(&b, ",");
		dbuf_putf(&b,
			"{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
			"\"end\":{\"line\":%d,\"character\":%d}},\"severity\":%d,\"message\":",
			line, col, eline, ecol, (int)g->severity);
		dbuf_putjstr(&b, g->message);
		dbuf_puts(&b, "}");
	}
	dbuf_puts(&b, "]}");
	send_notification("textDocument/publishDiagnostics", b.buf);
	free(b.buf);
}

/* ============================================================================
 * Request handlers.
 * ========================================================================== */

static void handle_initialize(jval *id, jval *params) {
	/* rootUri/rootPath are deprecated in favor of workspaceFolders (LSP 3.17),
	 * and not every client still sends rootUri -- try both, rootUri first
	 * (still the common case), falling back to workspaceFolders[0].uri. */
	const char *ru = jstr(jget(params, "rootUri"));
	if (!ru) {
		jval *folders = jget(params, "workspaceFolders");
		if (folders && folders->kind == JV_ARR && folders->n > 0)
			ru = jstr(jget(folders->items[0], "uri"));
	}
	if (ru) {
		free(g_root);
		g_root = uri_to_path(ru);
		fprintf(stderr, "fbuildd: workspace root = '%s'\n", g_root);
	} else {
		fprintf(stderr, "fbuildd: initialize had no rootUri/workspaceFolders\n");
	}
	send_result(id,
		"{\"capabilities\":{\"textDocumentSync\":1,"
		"\"hoverProvider\":true,\"definitionProvider\":true,"
		"\"documentSymbolProvider\":true,"
		"\"completionProvider\":{},"
		"\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]}}}");
}

static void handle_did_open(jval *params) {
	jval *td = jget(params, "textDocument");
	const char *uri = jstr(jget(td, "uri"));
	const char *text = jstr(jget(td, "text"));
	if (!uri || !text) return;
	fb_Doc *d = doc_open(uri);
	if (!d) return;
	doc_recheck(d, text);
	send_diagnostics(d);
}

static void handle_did_change(jval *params) {
	jval *td = jget(params, "textDocument");
	const char *uri = jstr(jget(td, "uri"));
	jval *changes = jget(params, "contentChanges");
	if (!uri || !changes || changes->kind != JV_ARR || changes->n == 0) return;
	const char *text = jstr(jget(changes->items[0], "text"));   /* full-document sync */
	if (!text) return;
	fb_Doc *d = doc_find(uri);
	if (!d) d = doc_open(uri);
	if (!d) return;
	doc_recheck(d, text);
	send_diagnostics(d);
}

static void handle_did_close(jval *params) {
	jval *td = jget(params, "textDocument");
	const char *uri = jstr(jget(td, "uri"));
	if (!uri) return;
	fb_Doc *d = doc_find(uri);
	if (d) {                        /* clear diagnostics before dropping the doc */
		d->ndiags = 0;
		send_diagnostics(d);
	}
	doc_close(uri);
}

static const char *COMPLETE_PLACEHOLDER = "__fbcomplete__";

/* Is the '.' at byte offset `dot_off` in `text` "dangling" -- nothing
 * parseable directly after it (no identifier/digit/"{")? Also requires the
 * dot to follow something that looks like the end of an expression
 * (identifier/close-bracket char), to avoid mistaking e.g. a bare "." on
 * its own line for a receiver. This is exactly the shape parse_field
 * (build_system.c) rejects -- it requires TOK_ID/TOK_INT/TOK_LBRACE right
 * after the dot -- which is why the whole document fails to parse with no
 * partial AST salvaged the moment a user types "base." and pauses. */
static int is_dangling_dot(const char *text, long dot_off) {
	char before = dot_off > 0 ? text[dot_off - 1] : 0;
	if (!(isalnum((unsigned char)before) || before == '_' ||
	      before == ')' || before == ']' || before == '}'))
		return 0;
	char after = text[dot_off + 1];
	if (!after) return 1;
	return !(isalnum((unsigned char)after) || after == '_' || after == '{');
}

/* Splice a placeholder identifier immediately after every dangling dot in
 * `text` (see is_dangling_dot) so the whole document parses even mid-edit.
 * Every byte before an insertion point is unchanged, so a line/col derived
 * from the original text still lands on the same source position in the
 * spliced text. Returns NULL if there were no dangling dots (nothing to
 * recover). */
static char *splice_dangling_dots(const char *text) {
	size_t tl = strlen(text), pl = strlen(COMPLETE_PLACEHOLDER);
	int count = 0;
	for (size_t i = 0; text[i]; i++)
		if (text[i] == '.' && is_dangling_dot(text, (long)i)) count++;
	if (!count) return NULL;
	char *out = malloc(tl + (size_t)count * pl + 1);
	size_t len = 0;
	for (size_t i = 0; text[i]; i++) {
		out[len++] = text[i];
		if (text[i] == '.' && is_dangling_dot(text, (long)i)) {
			memcpy(out + len, COMPLETE_PLACEHOLDER, pl);
			len += pl;
		}
	}
	out[len] = 0;
	return out;
}

/* If `d`'s last check hit a syntax error (d->prog == NULL) purely because
 * of one or more dangling dots -- the classic mid-typing "base." state --
 * recover a usable ast by splicing placeholders and rechecking in a
 * throwaway fb_State. Returns NULL (leaving *out_scratch untouched) if
 * d->prog is already set, there's no text, no dangling dot was found, or
 * the spliced text still doesn't parse cleanly (some other error present
 * too). On success, `*out_scratch` is set to the owning fb_State: the
 * caller must finish using the returned ast (serialize any response)
 * BEFORE calling fb_close(*out_scratch), and must close it exactly once. */
static ast *recover_prog(fb_Doc *d, fb_State **out_scratch) {
	if (d->prog || !d->text) return NULL;
	char *spliced = splice_dangling_dots(d->text);
	if (!spliced) return NULL;
	fb_State *scratch = fb_new();
	load_workspace_plugins(scratch);
	ast *prog2 = NULL;
	fb_Diag tmp[1];
	fb_check(scratch, spliced, d->uri, tmp, 1, &prog2);
	free(spliced);
	if (!prog2) { fb_close(scratch); return NULL; }
	*out_scratch = scratch;
	return prog2;
}

/* Byte offset (0-based) of 1-based (line, col) in `text`. No UTF-16
 * handling, same ASCII-position assumption the rest of this file already
 * makes (LSP character offsets are just treated as byte offsets). */
static long line_col_offset(const char *text, int line1, int col1) {
	long off = 0;
	int cur_line = 1;
	while (cur_line < line1 && text[off]) {
		if (text[off] == '\n') cur_line++;
		off++;
	}
	return off + (col1 - 1);
}

/* Scan `text` backward from byte offset `pos` (the cursor) for the nearest
 * enclosing, still-open '(' -- tracking (), [], {} nesting depth so a
 * paren inside a nested call/list/map isn't mistaken for the enclosing
 * one -- counting top-level commas along the way (the active parameter
 * index). This is deliberately text-level, not AST-based: it works
 * whether or not the document currently parses at all, which matters here
 * since the very call being typed is often mid-typing and unclosed (a
 * syntax error under this grammar's no-recovery parser, same root cause
 * completion's dangling-dot splice works around -- but splicing a correct
 * closing paren back in is far more ambiguous than splicing an identifier,
 * so this sidesteps reparsing entirely instead). Sets *out_callee/
 * *out_callee_len to the identifier immediately before the matched '(',
 * and *out_active to the comma count. Returns 0 if no enclosing call is
 * found (no '(' before `pos`, or it's not immediately preceded by an
 * identifier, or it belongs to a list/map literal instead). Only a rough
 * heuristic for string-literal contents, same spirit as the dangling-dot
 * detector -- good enough for the common case, not exhaustive. */
/* Marks every byte of `text` that falls inside a '...'/"..." string literal
 * (funcbuild strings: backslash-escaped, don't span lines -- SYNTAX.md), so
 * find_call_context's bracket/comma scan can skip them. Without this, a
 * literal "]" or "," inside a string (e.g. cat('[', l, '] ', m)) is
 * indistinguishable from real syntax and corrupts the depth/comma count.
 * A "{ expr }" string interpolation is treated as opaque string content
 * too (not specially unmasked) -- a cursor inside one would need its own
 * nested scan, out of scope for this best-effort heuristic. Caller frees
 * the returned buffer. */
static char *mask_strings(const char *text, size_t len) {
	char *in_str = malloc(len);
	int q = 0;
	for (size_t i = 0; i < len; i++) {
		char c = text[i];
		if (c == '\n') { q = 0; in_str[i] = 0; continue; }
		if (q) {
			in_str[i] = 1;
			if (c == '\\' && i + 1 < len) { in_str[++i] = 1; continue; }
			if (c == q) q = 0;
			continue;
		}
		if (c == '\'' || c == '"') { q = c; in_str[i] = 1; continue; }
		in_str[i] = 0;
	}
	return in_str;
}

static int find_call_context(const char *text, long pos, const char **out_callee,
                              size_t *out_callee_len, int *out_active) {
	size_t len = strlen(text);
	char *in_str = mask_strings(text, len);
	int depth = 0;
	int active = 0;
	int found = 0;
	for (long i = pos - 1; i >= 0; i--) {
		if (in_str[i]) continue;
		char c = text[i];
		if (c == ')' || c == ']' || c == '}') { depth++; continue; }
		if (c == '(') {
			if (depth > 0) { depth--; continue; }
			long j = i - 1;
			while (j >= 0 && isspace((unsigned char)text[j])) j--;
			long end = j + 1;
			while (j >= 0 && (isalnum((unsigned char)text[j]) || text[j] == '_')) j--;
			long start = j + 1;
			if (start >= end) break;   /* nothing identifier-like before '(' */
			*out_callee = text + start;
			*out_callee_len = (size_t)(end - start);
			*out_active = active;
			found = 1;
			break;
		}
		if (c == '[' || c == '{') {
			if (depth > 0) { depth--; continue; }
			break;   /* enclosed by a list/map literal, not a call */
		}
		if (c == ',' && depth == 0) active++;
	}
	free(in_str);
	return found;
}

static void handle_signature_help(jval *id, jval *params) {
	jval *td = jget(params, "textDocument");
	jval *pos = jget(params, "position");
	const char *uri = jstr(jget(td, "uri"));
	int line = jint(jget(pos, "line"), 0) + 1;
	int col  = jint(jget(pos, "character"), 0) + 1;
	fb_Doc *d = uri ? doc_find(uri) : NULL;
	if (!d || !d->text) { send_result(id, "null"); return; }

	long off = line_col_offset(d->text, line, col);
	const char *callee; size_t callee_len; int active;
	if (!find_call_context(d->text, off, &callee, &callee_len, &active)) {
		send_result(id, "null"); return;
	}

	/* d->prog may be NULL (e.g. this very call is unclosed, mid-typing) --
	 * fb_lookup_signature only needs it for a user-defined callee's real
	 * type; builtins resolve without it. */
	obj_type *sig = fb_lookup_signature(d->prog, callee, callee_len);
	if (!sig || sig->ret != RET_FUNCTOR) { send_result(id, "null"); return; }

	obj_type *pt[32];
	int np = fb_functor_params(sig, pt, 32);
	if (np > 0 && active >= np) active = np - 1;   /* cap into the last (possibly variadic) slot */

	char *full = obj_type_sprint(sig);
	size_t label_cap = callee_len + strlen(full) + 1;
	char *label = malloc(label_cap);
	snprintf(label, label_cap, "%.*s%s", (int)callee_len, callee, full);

	dbuf b; dbuf_init(&b);
	dbuf_puts(&b, "{\"signatures\":[{\"label\":");
	dbuf_putjstr(&b, label);
	dbuf_puts(&b, ",\"parameters\":[");
	for (int i = 0; i < np; i++) {
		if (i) dbuf_puts(&b, ",");
		dbuf_puts(&b, "{\"label\":");
		dbuf_putjstr(&b, obj_type_sprint(pt[i]));
		dbuf_puts(&b, "}");
	}
	dbuf_putf(&b, "]}],\"activeSignature\":0,\"activeParameter\":%d}", np > 0 ? active : 0);
	send_result(id, b.buf);
	free(b.buf);
	free(label);
}

static void handle_hover(jval *id, jval *params) {
	jval *td = jget(params, "textDocument");
	jval *pos = jget(params, "position");
	const char *uri = jstr(jget(td, "uri"));
	int line = jint(jget(pos, "line"), 0) + 1;
	int col  = jint(jget(pos, "character"), 0) + 1;
	fb_Doc *d = uri ? doc_find(uri) : NULL;
	if (!d) { send_result(id, "null"); return; }

	/* d->prog may be NULL (syntax-broken, e.g. the classic mid-typing
	 * "base." state) -- recover_prog salvages a usable ast when that's the
	 * only thing wrong. Still null (unrecoverable or no text at all) means
	 * there's truly no tree to hover into. */
	fb_State *scratch = NULL;
	ast *prog = d->prog ? d->prog : recover_prog(d, &scratch);
	if (!prog) { send_result(id, "null"); return; }

	fb_HoverInfo hi;
	int ok = fb_hover_at(prog, line, col, &hi);
	if (ok) {
		dbuf b; dbuf_init(&b);
		dbuf_puts(&b, hi.markdown ? "{\"contents\":{\"kind\":\"markdown\",\"value\":"
		                          : "{\"contents\":{\"kind\":\"plaintext\",\"value\":");
		dbuf_putjstr(&b, hi.text);
		dbuf_putf(&b, "},\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
		           z(hi.span.start.line), z(hi.span.start.col), z(hi.span.end.line), z(hi.span.end.col));
		send_result(id, b.buf);
		free(b.buf);
	} else {
		send_result(id, "null");
	}
	if (scratch) fb_close(scratch);
}

static void handle_definition(jval *id, jval *params) {
	jval *td = jget(params, "textDocument");
	jval *pos = jget(params, "position");
	const char *uri = jstr(jget(td, "uri"));
	int line = jint(jget(pos, "line"), 0) + 1;
	int col  = jint(jget(pos, "character"), 0) + 1;
	fb_Doc *d = uri ? doc_find(uri) : NULL;
	if (!d) { send_result(id, "null"); return; }

	fb_State *scratch = NULL;
	ast *prog = d->prog ? d->prog : recover_prog(d, &scratch);
	if (!prog) { send_result(id, "null"); return; }

	fb_Definition defs[16];
	int n = fb_find_definition(prog, line, col, defs, 16);
	if (n <= 0) { send_result(id, "null"); if (scratch) fb_close(scratch); return; }

	dbuf b; dbuf_init(&b);
	if (n > 1) dbuf_puts(&b, "[");
	for (int i = 0; i < n; i++) {
		if (i) dbuf_puts(&b, ",");
		dbuf_puts(&b, "{\"uri\":");
		dbuf_putjstr(&b, uri);
		dbuf_putf(&b, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
		           z(defs[i].def_span.start.line), z(defs[i].def_span.start.col),
		           z(defs[i].def_span.end.line),   z(defs[i].def_span.end.col));
	}
	if (n > 1) dbuf_puts(&b, "]");
	send_result(id, b.buf);
	if (scratch) fb_close(scratch);
	free(b.buf);
}

static void handle_document_symbol(jval *id, jval *params) {
	jval *td = jget(params, "textDocument");
	const char *uri = jstr(jget(td, "uri"));
	fb_Doc *d = uri ? doc_find(uri) : NULL;
	if (!d) { send_result(id, "[]"); return; }

	fb_State *scratch = NULL;
	ast *prog = d->prog ? d->prog : recover_prog(d, &scratch);
	if (!prog) { send_result(id, "[]"); return; }

	static fb_DocSymbol syms[1024];
	int n = fb_document_symbols(prog, syms, 1024);

	dbuf b; dbuf_init(&b);
	dbuf_puts(&b, "[");
	for (int i = 0; i < n; i++) {
		if (i) dbuf_puts(&b, ",");
		/* LSP SymbolKind: 5 = Class, 6 = Method, 12 = Function, 13 = Variable */
		int kind;
		switch (syms[i].kind) {
		case FB_SYM_CLASS:    kind = 5;  break;
		case FB_SYM_METHOD:   kind = 6;  break;
		case FB_SYM_FUNCTION: kind = 12; break;
		default:              kind = 13; break;
		}
		dbuf_puts(&b, "{\"name\":");
		dbuf_putjstr(&b, syms[i].name);
		dbuf_putf(&b, ",\"kind\":%d,\"location\":{\"uri\":", kind);
		dbuf_putjstr(&b, uri);
		dbuf_putf(&b, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
		           z(syms[i].span.start.line), z(syms[i].span.start.col),
		           z(syms[i].span.end.line),   z(syms[i].span.end.col));
		if (syms[i].container) {
			dbuf_puts(&b, ",\"containerName\":");
			dbuf_putjstr(&b, syms[i].container);
		}
		dbuf_puts(&b, "}");
	}
	dbuf_puts(&b, "]");
	send_result(id, b.buf);
	if (scratch) fb_close(scratch);
	free(b.buf);
}

static void send_completion_items(jval *id, fb_CompItem *items, int n) {
	dbuf b; dbuf_init(&b);
	dbuf_puts(&b, "[");
	for (int i = 0; i < n; i++) {
		if (i) dbuf_puts(&b, ",");
		/* LSP CompletionItemKind: 3 = Function, 5 = Field, 6 = Variable */
		int kind;
		switch (items[i].kind) {
		case FB_COMP_BUILTIN:
		case FB_COMP_TOPLEVEL_FN: kind = 3; break;
		case FB_COMP_FIELD:       kind = 5; break;
		default:                  kind = 6; break;
		}
		dbuf_puts(&b, "{\"label\":");
		dbuf_putjstr(&b, items[i].name);
		dbuf_putf(&b, ",\"kind\":%d", kind);
		if (items[i].detail) {
			dbuf_puts(&b, ",\"detail\":");
			dbuf_putjstr(&b, items[i].detail);
		}
		dbuf_puts(&b, "}");
	}
	dbuf_puts(&b, "]");
	send_result(id, b.buf);
	free(b.buf);
}

/* Placeholder identifier spliced in right at the cursor for the
 * dangling-dot fallback below -- must be a syntactically valid bare
 * identifier, and distinctive enough it'll never collide with anything a
 * user actually typed. */
static void handle_completion(jval *id, jval *params) {
	jval *td = jget(params, "textDocument");
	jval *pos = jget(params, "position");
	const char *uri = jstr(jget(td, "uri"));
	int line = jint(jget(pos, "line"), 0) + 1;
	int col  = jint(jget(pos, "character"), 0) + 1;
	fb_Doc *d = uri ? doc_find(uri) : NULL;
	if (!d) { send_result(id, "[]"); return; }

	static fb_CompItem items[512];

	/* 1. Cursor already sits on a parsed base.field/base.method node
	 * (e.g. "base.partial<cursor>", which parses fine as-is -- only a
	 * bare trailing "." with nothing typed after fails to parse). */
	int n = d->prog ? fb_complete_field_at(d->prog, line, col, items, 512) : -1;
	if (n >= 0) { send_completion_items(id, items, n); return; }

	/* 2. d->prog may be NULL purely because of a dangling dot (the classic
	 * mid-typing "base." state) -- recover and retry against the same
	 * position. Response is fully serialized before `scratch` closes --
	 * its arena owns every string fb_complete_field_at just handed back. */
	fb_State *scratch = NULL;
	ast *prog2 = recover_prog(d, &scratch);
	if (prog2) {
		n = fb_complete_field_at(prog2, line, col, items, 512);
		send_completion_items(id, items, n < 0 ? 0 : n);
		fb_close(scratch);
		return;
	}

	/* 3. Not a field position at all: normal bare-identifier completion.
	 * d->prog may be NULL (syntax error) -- fb_complete_at degrades
	 * gracefully, still returning the builtin tier. */
	n = fb_complete_at(d->prog, line, col, items, 512);
	send_completion_items(id, items, n);
}

static void handle_shutdown(jval *id) {
	for (int i = 0; i < g_ndocs; i++)
		if (g_docs[i].S) fb_close(g_docs[i].S);
	g_ndocs = 0;
	g_got_shutdown = 1;
	send_result(id, "null");
}

static void handle_message(const char *body, size_t len) {
	jparser jp = { .s = body, .pos = 0, .len = len };
	jval *msg = jparse_value(&jp);
	if (!msg || msg->kind != JV_OBJ) { jfree(msg); return; }

	jval *method_v = jget(msg, "method");
	jval *id       = jget(msg, "id");
	jval *params   = jget(msg, "params");
	const char *method = jstr(method_v);
	if (!method) { jfree(msg); return; }

	if (!strcmp(method, "initialize"))                   handle_initialize(id, params);
	else if (!strcmp(method, "initialized"))              { /* no-op */ }
	else if (!strcmp(method, "textDocument/didOpen"))     handle_did_open(params);
	else if (!strcmp(method, "textDocument/didChange"))   handle_did_change(params);
	else if (!strcmp(method, "textDocument/didClose"))    handle_did_close(params);
	else if (!strcmp(method, "textDocument/hover"))       handle_hover(id, params);
	else if (!strcmp(method, "textDocument/definition"))  handle_definition(id, params);
	else if (!strcmp(method, "textDocument/documentSymbol")) handle_document_symbol(id, params);
	else if (!strcmp(method, "textDocument/completion"))  handle_completion(id, params);
	else if (!strcmp(method, "textDocument/signatureHelp")) handle_signature_help(id, params);
	else if (!strcmp(method, "shutdown"))                 handle_shutdown(id);
	else if (!strcmp(method, "exit")) {
		jfree(msg);
		exit(g_got_shutdown ? 0 : 1);
	}
	else if (id) send_result(id, "null");   /* unknown request: answer, don't hang the client */

	jfree(msg);
}

int main(void) {
	for (;;) {
		long len;
		if (read_headers(&len) != 0) break;   /* EOF or malformed stream */
		char *body = malloc((size_t)len + 1);
		size_t got = fread(body, 1, (size_t)len, stdin);
		body[got] = 0;
		handle_message(body, got);
		free(body);
	}
	return 0;
}
