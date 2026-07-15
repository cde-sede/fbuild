/* fbstream -- incremental file-writing streams for fbuild, as a plugin (.so).
 *
 * Build: cc -shared -fPIC -Isrc plugins/fbstream.c -o build/fbstream.so
 * Load:  FBUILD_PLUGINS=build/fbstream.so ./fbuild
 *
 * Core's write(path, str) -> File is atomic: one whole-content write. This
 * plugin is for building a file up incrementally -- append line by line
 * without holding the whole thing in a String first. Handy for codegen, e.g.
 * walking a Json map (json_parse) and emitting a source file line by line.
 *
 *   stream(path[, mode])  Stream   open; mode "w" (default, truncate) or "a"
 *   .write(s)             Stream   raw write, no newline (self, chainable)
 *   .writeln(s)           Stream   s + "\n"
 *   .writelines(list)     Stream   each element stringified + "\n"
 *   .flush()              Bool
 *   .close()               Bool    flush + close; safe to call more than once
 *   .path                          field set at construction
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fbuild.h"

typedef struct { FILE *f; } stream_t;

static fb_Class STREAM_CLS;

#define P(v) fb_tostr(S, (v))

static fb_Value stream_new(fb_State *S, const char *path, const char *mode) {
	FILE *f = fopen(path, mode);
	if (!f) return fb_error(S, "stream: cannot open '%s' (mode \"%s\")", path, mode);
	stream_t *st = malloc(sizeof *st);
	st->f = f;
	fb_Value obj = fb_object(S, STREAM_CLS, st);
	fb_set(S, obj, "path", fb_str(S, path));
	return obj;
}

/* stream(path[, mode]) -> Stream */
static fb_Value bi_stream(fb_State *S, fb_Value *a, int n) {
	const char *path = P(a[0]);
	const char *mode = n >= 2 ? P(a[1]) : "w";
	if (strcmp(mode, "w") != 0 && strcmp(mode, "a") != 0)
		return fb_error(S, "stream: mode must be \"w\" or \"a\", got \"%s\"", mode);
	return stream_new(S, path, mode);
}

/* .write(s) -> self: raw write, no newline. */
static fb_Value m_write(fb_State *S, fb_Value *a, int n) {
	(void)n;
	stream_t *st = fb_object_data(a[0]);
	if (!st->f) return fb_error(S, "stream: write on closed stream");
	fputs(P(a[1]), st->f);
	return a[0];
}

/* .writeln(s) -> self: s + "\n". */
static fb_Value m_writeln(fb_State *S, fb_Value *a, int n) {
	(void)n;
	stream_t *st = fb_object_data(a[0]);
	if (!st->f) return fb_error(S, "stream: writeln on closed stream");
	fputs(P(a[1]), st->f);
	fputc('\n', st->f);
	return a[0];
}

/* .writelines(list) -> self: each element stringified + "\n" (bare value ok too). */
static fb_Value m_writelines(fb_State *S, fb_Value *a, int n) {
	(void)n;
	stream_t *st = fb_object_data(a[0]);
	if (!st->f) return fb_error(S, "stream: writelines on closed stream");
	fb_Value l = a[1];
	fb_Kind k = fb_kind(l);
	if (k == FB_LIST || k == FB_TUPLE) {
		int len = fb_len(l);
		for (int i = 0; i < len; i++) { fputs(P(fb_at(l, i)), st->f); fputc('\n', st->f); }
	} else {
		fputs(P(l), st->f); fputc('\n', st->f);
	}
	return a[0];
}

/* .flush() -> Bool */
static fb_Value m_flush(fb_State *S, fb_Value *a, int n) {
	(void)n;
	stream_t *st = fb_object_data(a[0]);
	return fb_bool(S, st->f && fflush(st->f) == 0);
}

/* .close() -> Bool: flush + close, idempotent. */
static fb_Value m_close(fb_State *S, fb_Value *a, int n) {
	(void)n;
	stream_t *st = fb_object_data(a[0]);
	if (!st->f) return fb_bool(S, 1);
	int rc = fclose(st->f) == 0;
	st->f = NULL;
	return fb_bool(S, rc);
}

int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	STREAM_CLS = fb_class(S, "Stream");
	fb_method(S, STREAM_CLS, "write",      m_write);
	fb_method(S, STREAM_CLS, "writeln",    m_writeln);
	fb_method(S, STREAM_CLS, "writelines", m_writelines);
	fb_method(S, STREAM_CLS, "flush",      m_flush);
	fb_method(S, STREAM_CLS, "close",      m_close);

	static const fb_Reg regs[] = {
		{ "stream", bi_stream, 1, 2, FB_RET_ANY, 0 },
	};
	for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
		fb_register(S, &regs[i]);
	return 0;
}
