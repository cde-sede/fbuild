/* fbuild executor: the builtin library.
 *
 * Split out of run.c so it compiles in parallel with the core evaluator.
 * Builtins dispatch through the fb_Reg registry, so run.c has no compile-time
 * dependency on the individual do_* functions here; the few cross-TU symbols
 * (both directions) are declared in run_internal.h. */

#include "run_internal.h"
#include <errno.h>

static rval *do_clock(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return rv_int((long)ts.tv_sec * 1000000000L + ts.tv_nsec);
}
/* wall-clock unix epoch, in seconds. */
static rval *do_time(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return rv_int((long)ts.tv_sec);
}
/* sleep for the given number of milliseconds; returns (). */
static rval *do_sleep(rval *ms) {
	long m = (ms && ms->k == RV_INT) ? ms->i : 0;
	if (m > 0) {
		struct timespec ts;
		ts.tv_sec  = m / 1000;
		ts.tv_nsec = (m % 1000) * 1000000L;
		nanosleep(&ts, NULL);
	}
	return &RV_UNIT;
}

/* config spinner: a language lambda (Int -> String). The build runtime calls
 * spin_trampoline() each refresh with this command's frame counter; we apply the
 * lambda and hand back its string (arena-owned, valid until the next call). */
static const char *spin_trampoline(int n) {
	if (!g_spinner) return "";
	const char *s = as_cstr(apply(g_spinner, rv_int(n)));
	return s ? s : "";
}

/* config(map): apply build-script settings. Recognized keys:
 *   jobs       : Int           cap concurrent child procs in PARALLEL groups
 *   verbose    : Bool          show every command's output (no collapse)
 *   keep_going : Bool          run all tasks, report failures at the end
 *   silent     : Bool          suppress the live UI
 *   color      : Bool          force the live UI on (true) / off (false)
 *   spinner_ms : Int           display refresh period in ms
 *   tail_lines : Int           live-output lines under a running command
 *   spinner    : (Int->String) custom spinner glyph per frame
 * Unknown keys are ignored so scripts stay forward-compatible. */
static rval *do_config(rval *m) {
	if (!m || m->k != RV_MAP) return &RV_UNIT;
	rval *v;
	if ((v = map_get(m, "jobs"))       && v->k == RV_INT)    build_set_jobs((int)v->i);
	if ((v = map_get(m, "verbose"))    && v->k != RV_NONE)   build_set_verbose(truthy(v));
	if ((v = map_get(m, "keep_going")) && v->k != RV_NONE)   build_set_keep_going(truthy(v));
	if ((v = map_get(m, "silent"))     && v->k != RV_NONE)   build_set_silent(truthy(v));
	if ((v = map_get(m, "color"))      && v->k != RV_NONE)   build_set_color(truthy(v) ? 1 : 2);
	if ((v = map_get(m, "spinner_ms")) && v->k == RV_INT)    build_set_spinner_ms(v->i);
	if ((v = map_get(m, "tail_lines")) && v->k == RV_INT)    build_set_tail_lines((int)v->i);
	if ((v = map_get(m, "spinner"))    && v->k == RV_LAMBDA) {
		g_spinner = v;
		build_set_spinner_fn(spin_trampoline);
	}
	return &RV_UNIT;
}

static rval *do_env(const char *name) {
	char *v = getenv(name);
	if (!v) return &RV_UNIT;            /* unset -> () (falsy), so `env(x) || d` works */
	return rv_str(a_dup(v));
}

/* ---- lightweight CLI arg helpers (over the `args` list) --------------- */

/* flag(name...) -> Bool: true if any of the given flag names appears in args. */
static rval *do_flag(rval **a, size_t n) {
	for (size_t k = 0; k < n; k++) {
		const char *name = as_cstr(a[k]);
		for (size_t i = 0; i < g_args->n; i++)
			if (!strcmp(as_cstr(g_args->items[i]), name)) return rv_bool(1);
	}
	return rv_bool(0);
}

/* opt(name, default) -> String: value of `name v` or `name=v` in args, else
 * default. A function default is invoked lazily -- only when the option is
 * absent -- so `opt('-x', _ -> expensive())` never runs expensive() when -x is
 * supplied. */
static rval *do_opt(rval *name, rval *deflt) {
	const char *nm = as_cstr(name);
	size_t nl = strlen(nm);
	for (size_t i = 0; i < g_args->n; i++) {
		const char *cur = as_cstr(g_args->items[i]);
		if (!strcmp(cur, nm)) {                          /* `name value` */
			if (i + 1 < g_args->n) return rv_str(a_dup(as_cstr(g_args->items[i + 1])));
			break;
		}
		if (!strncmp(cur, nm, nl) && cur[nl] == '=')     /* `name=value` */
			return rv_str(a_dup(cur + nl + 1));
	}
	return deflt->k == RV_LAMBDA ? apply(deflt, &RV_UNIT) : deflt;
}

/* positionals() -> List<String>: args that aren't flags (don't start with '-').
 * Lightweight: does not skip values consumed by opt(); use Parser for that. */
static rval *do_positionals(void) {
	rval *l = rv(RV_LIST);
	for (size_t i = 0; i < g_args->n; i++) {
		const char *cur = as_cstr(g_args->items[i]);
		if (cur[0] != '-') rv_push(l, rv_str(a_dup(cur)));
	}
	return l;
}

/* Parser<schema>(argv): drive build.h's args.h with a schema map. Each schema
 * entry is name -> (type string | spec map). Returns a map of name -> typed
 * value (with schema/spec defaults applied), plus `rest` = leftover positionals.
 * `-h`/`--help` is handled by args.h (prints help from the `help` fields). */
rval *do_parse(rval *schema, rval *argv_list) {
	if (!schema || schema->k != RV_MAP) return rv(RV_MAP);
	size_t ne = schema->n;

	t_arg_rule *rules = a_alloc((ne + 2) * sizeof *rules);
	memset(rules, 0, (ne + 2) * sizeof *rules);

	for (size_t i = 0; i < ne; i++) {
		const char *name = schema->keys[i];
		rval *spec = schema->items[i];
		rval *sm = (spec && spec->k == RV_MAP) ? spec : NULL;
		const char *tstr = sm ? "str" : as_cstr(spec);
		const char *longh = NULL, *shorth = NULL, *help = NULL;
		if (sm) {
			rval *v;
			if ((v = map_get(sm, "type"))->k  != RV_NONE) tstr   = as_cstr(v);
			if ((v = map_get(sm, "long"))->k  != RV_NONE) longh  = as_cstr(v);
			if ((v = map_get(sm, "short"))->k != RV_NONE) shorth = as_cstr(v);
			if ((v = map_get(sm, "help"))->k  != RV_NONE) help   = as_cstr(v);
		}
		if (!longh) { char *l = a_alloc(strlen(name) + 3); sprintf(l, "--%s", name); longh = l; }
		if (!shorth) {
			/* auto short = -<first letter>, only if unique among names and not -h */
			int dup = (name[0] == 'h');
			for (size_t j = 0; j < ne && !dup; j++)
				if (j != i && schema->keys[j][0] == name[0]) dup = 1;
			if (!dup) { char *s = a_alloc(3); s[0] = '-'; s[1] = name[0]; s[2] = 0; shorth = s; }
		}
		t_arg_type ty = ARG_STRING;
		if (!strcmp(tstr, "bool")) ty = ARG_BOOL;
		else if (!strcmp(tstr, "int") || !strcmp(tstr, "integer")) ty = ARG_INTEGER;
		rules[i].shorthand = shorth;
		rules[i].longhand  = longh;
		rules[i].flags     = 0;
		rules[i].type      = ty;
		rules[i].help      = help;
	}
	rules[ne].longhand = "rest";              /* capture leftover positionals */
	rules[ne].flags    = ARG_POSITIONAL | ARG_ARRAY;
	rules[ne].type     = ARG_STR_ARRAY;

	size_t na = (argv_list && is_seq(argv_list)) ? argv_list->n : 0;
	char **av = a_alloc((na + 1) * sizeof *av);
	av[0] = "fbuild";
	for (size_t i = 0; i < na; i++) av[i + 1] = (char *)as_cstr(argv_list->items[i]);

	t_argparser parser = { .rules = rules };
	if (args_parse(&parser, (int)(na + 1), av) == -2) {  /* -h/--help shown */
		args_free(&parser);
		exit(0);
	}

	rval *res = rv(RV_MAP);
	for (size_t i = 0; i < ne; i++) {
		const char *q = rules[i].longhand;
		rval *val;
		switch (rules[i].type) {
		case ARG_BOOL:    val = rv_bool(args_get_bool(&parser, q)); break;
		case ARG_INTEGER: val = rv_int(args_get_int(&parser, q));   break;
		default: { const char *s = args_get_str(&parser, q); val = rv_str(a_dup(s ? s : "")); }
		}
		if (!args_present(&parser, q) && schema->items[i]->k == RV_MAP) {
			rval *d = map_get(schema->items[i], "default");
			/* A function default is invoked lazily -- only here, when the arg
			 * was not supplied -- so an expensive default (e.g. shelling out)
			 * never runs when a value is given on the command line. */
			if (d->k == RV_LAMBDA) d = apply(d, &RV_UNIT);
			if (d->k != RV_NONE)   val = d;      /* spec default overrides type zero */
		}
		map_set(res, a_dup(schema->keys[i]), val);
	}
	rval *rest = rv(RV_LIST);
	t_arg_str_array *arr = args_get_array(&parser, "rest");
	if (arr) for (size_t i = 0; i < arr->count; i++) rv_push(rest, rv_str(a_dup(arr->items[i])));
	map_set(res, a_dup("rest"), rest);

	args_free(&parser);
	return res;
}

/* run a command via the shell, capture stdout, trim trailing newlines */
static rval *do_which(const char *name) {
	if (strchr(name, '/'))
		return access(name, X_OK) == 0 ? rv_file(a_dup(name)) : rv_str(a_dup(""));
	char *path = getenv("PATH");
	if (!path) return rv_str(a_dup(""));
	char *p = a_dup(path), *save = NULL;
	for (char *t = strtok_r(p, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
		char full[4096];
		snprintf(full, sizeof full, "%s/%s", t, name);
		if (access(full, X_OK) == 0) return rv_file(a_dup(full));
	}
	return rv_str(a_dup(""));
}

/* filesystem ----------------------------------------------------------- */

static rval *do_exists(rval *f) { return rv_bool(access(pstr(f), F_OK) == 0); }

static rval *do_mkdir(rval *d) {               /* mkdir -p */
	const char *p = pstr(d);
	char tmp[4096];
	snprintf(tmp, sizeof tmp, "%s", p);
	for (char *s = tmp + 1; *s; s++)
		if (*s == '/') { *s = '\0'; mkdir(tmp, 0755); *s = '/'; }   /* intermediate: EEXIST is fine */
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return (rval *)fb_error(g_active, "mkdir '%s': %s", p, strerror(errno));
	return rv_file(a_dup(p));
}

static rval *do_read(rval *f) {
	FILE *fp = fopen(pstr(f), "rb");
	if (!fp) return rv_str(a_dup(""));
	size_t cap = 256, n = 0;
	char *b = a_alloc(cap);
	int c;
	while ((c = fgetc(fp)) != EOF) {
		if (n + 1 >= cap) { b = a_grow(b, n, cap * 2); cap *= 2; }
		b[n++] = (char)c;
	}
	fclose(fp);
	b[n] = '\0';
	return rv_str(b);
}

static rval *do_write(rval *f, rval *content) {
	const char *p = pstr(f);
	FILE *fp = fopen(p, "wb");
	if (!fp)
		return (rval *)fb_error(g_active, "write '%s': %s", p, strerror(errno));
	int ok = fputs(as_cstr(content), fp) != EOF;
	int e = errno;
	if (fclose(fp) != 0 && ok) { ok = 0; e = errno; }
	if (!ok)
		return (rval *)fb_error(g_active, "write '%s': %s", p, strerror(e));
	return (f && f->k == RV_FILE) ? f : rv_file(a_dup(p));
}

/* raw byte copy of src -> dst, preserving src's mode. Returns 0 on success,
 * -1 with errno set (from the failing syscall) otherwise. */
static int file_copy(const char *src, const char *dst) {
	int in = open(src, O_RDONLY);
	if (in < 0) return -1;
	struct stat st;
	mode_t mode = (fstat(in, &st) == 0) ? st.st_mode & 0777 : 0644;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) { int e = errno; close(in); errno = e; return -1; }
	char buf[1 << 16];
	ssize_t r;
	int ok = 1;
	while ((r = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, (size_t)r) != r) { ok = 0; break; }
	if (r < 0) ok = 0;
	int e = errno;
	close(in);
	close(out);
	errno = e;
	return ok ? 0 : -1;
}

static rval *do_copy(rval *a, rval *b) {
	const char *s = pstr(a), *d = pstr(b);
	if (file_copy(s, d) != 0)
		return (rval *)fb_error(g_active, "copy '%s' -> '%s': %s", s, d, strerror(errno));
	return rv_file(a_dup(d));
}

/* lists ---------------------------------------------------------------- */

static rval *do_filter(rval *list, rval *fn) {
	rval *out = rv(RV_LIST);
	if (!is_seq(list)) return out;
	for (size_t i = 0; i < list->n; i++)
		if (truthy(apply_scratch(fn, list->items[i])))   /* predicate temps reclaimed */
			rv_push(out, list->items[i]);                /* kept item is durable */
	return out;
}

static rval *do_concat(rval **args, size_t n) {
	rval *out = rv(RV_LIST);
	for (size_t i = 0; i < n; i++) {
		rval *a = args[i];
		if (is_seq(a))
			for (size_t j = 0; j < a->n; j++) rv_push(out, a->items[j]);
		else if (a->k != RV_NONE)             /* skip () (e.g. a gated-off def) */
			rv_push(out, a);
	}
	return out;
}

static rval *do_contains(rval *list, rval *x) {
	if (!is_seq(list)) return rv_bool(0);
	const char *s = as_cstr(x);
	for (size_t i = 0; i < list->n; i++)
		if (!strcmp(as_cstr(list->items[i]), s)) return rv_bool(1);
	return rv_bool(0);
}

static rval *do_length(rval *list) {
	return rv_int(is_seq(list) ? (long)list->n : 0);
}
static rval *do_first(rval *list) {
	return (is_seq(list) && list->n) ? list->items[0] : &RV_UNIT;
}
static rval *do_last(rval *list) {
	return (is_seq(list) && list->n) ? list->items[list->n - 1] : &RV_UNIT;
}

/* take/drop/sort/unique moved to the fbloop plugin (list glue split, see
 * comment on fb_open_base's base[] table below); this file no longer carries
 * them (would duplicate the plugin's equivalently-optimized versions). */

/* python-style list ops -------------------------------------------------- */

/* first index where (key ? key(elem) : elem) equals val, or -1. */
/* sum/min/max accept either a single sequence or varargs: sum(L) == sum(1,2,3). */

/* strings -------------------------------------------------------------- */

static rval *do_join(rval *list, const char *sep) {
	if (!is_seq(list)) return rv_str(a_dup(""));
	size_t seplen = strlen(sep), tot = 1;
	for (size_t i = 0; i < list->n; i++)
		tot += strlen(as_cstr(list->items[i])) + seplen;
	char *b = a_alloc(tot), *w = b;      /* write pointer: strcat is O(n^2) */
	for (size_t i = 0; i < list->n; i++) {
		if (i) { memcpy(w, sep, seplen); w += seplen; }
		const char *p = as_cstr(list->items[i]);
		size_t l = strlen(p);
		memcpy(w, p, l); w += l;
	}
	*w = '\0';
	return rv_str(b);
}

static rval *do_split(const char *s, const char *sep) {
	rval *out = rv(RV_LIST);
	if (!*sep) { rv_push(out, rv_str(a_dup(s))); return out; }
	size_t sl = strlen(sep);
	const char *p = s, *q;
	while ((q = strstr(p, sep))) { rv_push(out, rv_str(a_dupn(p, q - p))); p = q + sl; }
	rv_push(out, rv_str(a_dup(p)));
	return out;
}

/* paths ---------------------------------------------------------------- */

static rval *do_with_suffix(rval *f, const char *ext) {
	const char *p = pstr(f);
	char *dir = path_dir(p), *st = path_stem(p);
	const char *dot = (ext[0] == '.') ? "" : ".";
	char *name = a_alloc(strlen(st) + strlen(dot) + strlen(ext) + 1);
	sprintf(name, "%s%s%s", st, dot, ext);
	char *out;
	if (dir[0] && strcmp(dir, ".") != 0) {
		out = a_alloc(strlen(dir) + 1 + strlen(name) + 1);
		sprintf(out, "%s/%s", dir, name);
	} else out = a_dup(name);
	return rv_file(out);
}

static rval *do_with_name(rval *f, const char *nm) {
	char *dir = path_dir(pstr(f));
	char *out;
	if (dir[0] && strcmp(dir, ".") != 0) {
		out = a_alloc(strlen(dir) + 1 + strlen(nm) + 1);
		sprintf(out, "%s/%s", dir, nm);
	} else out = a_dup(nm);
	return rv_file(out);
}

/* more string ops --------------------------------------------------------- */

static rval *do_to_int(rval *v) {
	if (v->k == RV_FLOAT) return rv_int((long)v->f);
	return rv_int(strtol(as_cstr(v), NULL, 10));
}
static rval *do_to_float(rval *v) { return rv_float(strtod(as_cstr(v), NULL)); }
static rval *do_to_str(rval *v) { return rv_str(a_dup(as_cstr(v))); }

static rval *do_cat(rval **a, size_t n) {       /* concat strings */
	size_t tot = 1;
	for (size_t i = 0; i < n; i++) tot += strlen(as_cstr(a[i]));
	char *b = a_alloc(tot), *w = b;
	for (size_t i = 0; i < n; i++) {
		const char *p = as_cstr(a[i]);
		size_t l = strlen(p);
		memcpy(w, p, l); w += l;
	}
	*w = '\0';
	return rv_str(b);
}
/* pad s to width with fill; side: -1 pad left, +1 pad right, 0 center */

/* more path ops ----------------------------------------------------------- */

static rval *do_path(rval **a, size_t n) {      /* join components with "/" */
	char *out = a_alloc(1);
	out[0] = '\0';
	size_t len = 0, cap = 1;
	int first = 1;
	for (size_t i = 0; i < n; i++) {
		const char *p = as_cstr(a[i]);
		if (!*p) continue;
		size_t pl = strlen(p);
		if (len + pl + 2 > cap) { out = a_grow(out, len + 1, len + pl + 2); cap = len + pl + 2; }
		if (!first) out[len++] = '/';
		memcpy(out + len, p, pl);
		len += pl; out[len] = '\0';
		first = 0;
	}
	return rv_file(out);
}
static rval *do_abs(rval *v) {                   /* realpath, fallback to as-is */
	char *r = realpath(pstr(v), NULL);
	if (!r) return rv_file(a_dup(pstr(v)));
	rval *res = rv_file(a_dup(r));   /* realpath buffer is libc-owned */
	free(r);
	return res;
}

/* print / debug rendering ------------------------------------------------- */

/* a growable, arena-backed string builder (avoids per-print open_memstream). */
typedef struct { char *buf; size_t len, cap; } sbuf;

static void sb_ensure(sbuf *s, size_t add) {
	if (s->len + add + 1 > s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 64;
		while (nc < s->len + add + 1) nc *= 2;
		s->buf = a_grow(s->buf, s->len, nc);
		s->cap = nc;
	}
}
static void sb_putc(sbuf *s, char c) { sb_ensure(s, 1); s->buf[s->len++] = c; s->buf[s->len] = '\0'; }
static void sb_puts(sbuf *s, const char *p) {
	size_t n = strlen(p);
	sb_ensure(s, n);
	memcpy(s->buf + s->len, p, n);
	s->len += n;
	s->buf[s->len] = '\0';
}
static void sb_putl(sbuf *s, long v) { char t[24]; snprintf(t, sizeof t, "%ld", v); sb_puts(s, t); }

/* repr: a plain, readable rendering of any value, any shape. */
static void repr(sbuf *s, rval *v) {
	if (!v) { sb_puts(s, "()"); return; }
	switch (v->k) {
	case RV_NONE:  sb_puts(s, "()");                       break;
	case RV_INT:   sb_putl(s, v->i);                       break;
	case RV_FLOAT: sb_puts(s, fmt_double(v->f));           break;
	case RV_BOOL:  sb_puts(s, v->i ? "true" : "false");    break;
	case RV_STR:   sb_puts(s, v->s ? v->s : "");           break;
	case RV_FILE:  sb_puts(s, v->s ? v->s : "");           break;
	case RV_LIST:
		sb_putc(s, '[');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			repr(s, v->items[i]);
		}
		sb_putc(s, ']');
		break;
	case RV_SEQ:   sb_puts(s, "<seq>");                    break;
	case RV_TUPLE:
		sb_putc(s, '(');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			repr(s, v->items[i]);
		}
		sb_putc(s, ')');
		break;
	case RV_CMD:
		sb_putc(s, '`');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_putc(s, ' ');
			repr(s, v->items[i]);
		}
		sb_putc(s, '`');
		break;
	case RV_MAP:
		sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			sb_puts(s, v->keys[i]);
			sb_puts(s, ": ");
			repr(s, v->items[i]);
		}
		sb_putc(s, '}');
		break;
	case RV_LAMBDA:  sb_puts(s, "<lambda>");                            break;
	case RV_MORPH:   sb_puts(s, "<"); sb_puts(s, v->from);
	                 sb_puts(s, " // "); sb_puts(s, v->to); sb_puts(s, ">"); break;
	case RV_BUILTIN: sb_puts(s, "<builtin "); sb_puts(s, v->s); sb_puts(s, ">"); break;
	case RV_THUNK:   sb_puts(s, "<thunk>");                     break;
	case RV_CFUN:    sb_puts(s, "<method>");                    break;
	case RV_OBJECT:
		sb_puts(s, v->cls ? ((fb_class_t *)v->cls)->name : "object");
		sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			sb_puts(s, v->keys[i]); sb_puts(s, ": "); repr(s, v->items[i]);
		}
		sb_putc(s, '}');
		break;
	}
}

/* debug: a typed, structural rendering (kind tags + quoting). */
static void debug_repr(sbuf *s, rval *v) {
	if (!v) { sb_puts(s, "None"); return; }
	switch (v->k) {
	case RV_NONE:  sb_puts(s, "None");                          break;
	case RV_INT:   sb_puts(s, "Int(");  sb_putl(s, v->i); sb_putc(s, ')'); break;
	case RV_FLOAT: sb_puts(s, "Float("); sb_puts(s, fmt_double(v->f)); sb_putc(s, ')'); break;
	case RV_BOOL:  sb_puts(s, v->i ? "Bool(true)" : "Bool(false)"); break;
	case RV_STR:   sb_puts(s, "Str(\""); sb_puts(s, v->s ? v->s : ""); sb_puts(s, "\")"); break;
	case RV_FILE:  sb_puts(s, "File("); sb_puts(s, v->s ? v->s : ""); sb_putc(s, ')'); break;
	case RV_LIST:
		sb_puts(s, "List#"); sb_putl(s, (long)v->n); sb_putc(s, '[');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, ']');
		break;
	case RV_SEQ:   sb_puts(s, "Seq");                          break;
	case RV_TUPLE:
		sb_puts(s, "Tuple#"); sb_putl(s, (long)v->n); sb_putc(s, '(');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, ')');
		break;
	case RV_CMD:
		sb_puts(s, "Cmd#"); sb_putl(s, (long)v->n); sb_putc(s, '[');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, ']');
		break;
	case RV_MAP:
		sb_puts(s, "Map#"); sb_putl(s, (long)v->n); sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			sb_putc(s, '"'); sb_puts(s, v->keys[i]); sb_puts(s, "\": ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, '}');
		break;
	case RV_LAMBDA:  sb_puts(s, "Lambda("); sb_putl(s, (long)(v->params ? v->params->nkids : 0)); sb_puts(s, " params)"); break;
	case RV_MORPH:   sb_puts(s, "Morph("); sb_puts(s, v->from); sb_puts(s, " // "); sb_puts(s, v->to); sb_putc(s, ')'); break;
	case RV_BUILTIN: sb_puts(s, "Builtin("); sb_puts(s, v->s); sb_putc(s, ')'); break;
	case RV_THUNK:   sb_puts(s, "Thunk");                       break;
	case RV_CFUN:    sb_puts(s, "Method");                      break;
	case RV_OBJECT:
		sb_puts(s, v->cls ? ((fb_class_t *)v->cls)->name : "Object");
		sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_puts(s, ", ");
			sb_putc(s, '"'); sb_puts(s, v->keys[i]); sb_puts(s, "\": ");
			debug_repr(s, v->items[i]);
		}
		sb_putc(s, '}');
		break;
	}
}

/* print(...) / debug(...): render every argument (space-separated) and emit,
 * deferring until the active run finishes if one is in progress. */
static rval *do_print(rval **a, size_t n, int dbg) {
	sbuf s = {0};
	for (size_t i = 0; i < n; i++) {
		if (i) sb_putc(&s, ' ');
		if (dbg) debug_repr(&s, a[i]);
		else     repr(&s, a[i]);
	}
	print_emit(s.buf ? s.buf : a_dup(""));   /* arena-owned line */
	return &RV_UNIT;
}

/* ---- JSON ------------------------------------------------------------- */
/* Values map: object -> RV_MAP, array -> RV_LIST, string -> RV_STR,
 * integer -> RV_INT, real -> RV_FLOAT,
 * true/false -> RV_BOOL, null -> (). All arena-allocated. */

static rval *json_num(char *text, int isreal) {
	if (isreal) return rv_float(strtod(text, NULL));
	return rv_int(strtol(text, NULL, 10));
}

static void json_ws(const char **p) {
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

/* parse a JSON string literal at *p (which points at the opening quote). */
static char *json_string(const char **p) {
	(*p)++;                                   /* skip opening quote */
	size_t cap = 16, len = 0;
	char *b = a_alloc(cap);
	while (**p && **p != '"') {
		char c = **p;
		if (c == '\\') {
			(*p)++;
			char e = **p;
			switch (e) {
			case 'n': c = '\n'; break;  case 't': c = '\t'; break;
			case 'r': c = '\r'; break;  case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;  case '/': c = '/';  break;
			case '"': c = '"';  break;  case '\\': c = '\\'; break;
			case 'u': {                 /* \uXXXX -> UTF-8 (BMP) */
				unsigned cp = 0;
				for (int k = 0; k < 4 && isxdigit((unsigned char)(*p)[1]); k++) {
					char h = *++(*p);
					cp = cp * 16 + (h <= '9' ? h - '0'
					              : (h | 32) - 'a' + 10);
				}
				if (len + 4 >= cap) { b = a_grow(b, len, cap *= 2); }
				if (cp < 0x80) b[len++] = (char)cp;
				else if (cp < 0x800) {
					b[len++] = (char)(0xC0 | (cp >> 6));
					b[len++] = (char)(0x80 | (cp & 0x3F));
				} else {
					b[len++] = (char)(0xE0 | (cp >> 12));
					b[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
					b[len++] = (char)(0x80 | (cp & 0x3F));
				}
				(*p)++;
				continue;
			}
			default: c = e; break;
			}
		}
		if (len + 1 >= cap) { b = a_grow(b, len, cap *= 2); }
		b[len++] = c;
		(*p)++;
	}
	if (**p == '"') (*p)++;
	b[len] = '\0';
	return b;
}

static rval *json_value(const char **p);

static rval *json_value(const char **p) {
	json_ws(p);
	char c = **p;
	if (c == '{') {
		(*p)++;
		rval *m = rv(RV_MAP);
		json_ws(p);
		if (**p == '}') { (*p)++; return m; }
		for (;;) {
			json_ws(p);
			if (**p != '"') break;
			char *k = json_string(p);
			json_ws(p);
			if (**p == ':') (*p)++;
			map_set(m, k, json_value(p));
			json_ws(p);
			if (**p == ',') { (*p)++; continue; }
			break;
		}
		json_ws(p);
		if (**p == '}') (*p)++;
		return m;
	}
	if (c == '[') {
		(*p)++;
		rval *l = rv(RV_LIST);
		json_ws(p);
		if (**p == ']') { (*p)++; return l; }
		for (;;) {
			rv_push(l, json_value(p));
			json_ws(p);
			if (**p == ',') { (*p)++; continue; }
			break;
		}
		json_ws(p);
		if (**p == ']') (*p)++;
		return l;
	}
	if (c == '"') return rv_str(json_string(p));
	if (!strncmp(*p, "true", 4))  { *p += 4; return rv_bool(1); }
	if (!strncmp(*p, "false", 5)) { *p += 5; return rv_bool(0); }
	if (!strncmp(*p, "null", 4))  { *p += 4; return &RV_UNIT; }
	if (c == '-' || isdigit((unsigned char)c)) {     /* number */
		const char *start = *p;
		int isreal = 0;
		if (**p == '-') (*p)++;
		while (isdigit((unsigned char)**p)) (*p)++;
		if (**p == '.') { isreal = 1; (*p)++; while (isdigit((unsigned char)**p)) (*p)++; }
		if (**p == 'e' || **p == 'E') {
			isreal = 1; (*p)++;
			if (**p == '+' || **p == '-') (*p)++;
			while (isdigit((unsigned char)**p)) (*p)++;
		}
		return json_num(a_dupn(start, *p - start), isreal);
	}
	(*p)++;                                   /* skip unknown byte */
	return &RV_UNIT;
}

static rval *do_json_parse(const char *s) {
	const char *p = s;
	return json_value(&p);
}

/* serialize a value as JSON into the string builder. */
static void json_emit(sbuf *s, rval *v, int pretty, int depth);

static void json_indent(sbuf *s, int depth) {
	sb_putc(s, '\n');
	for (int i = 0; i < depth; i++) sb_puts(s, "  ");
}

static void json_emit_str(sbuf *s, const char *p) {
	sb_putc(s, '"');
	for (; *p; p++) {
		switch (*p) {
		case '"':  sb_puts(s, "\\\""); break;
		case '\\': sb_puts(s, "\\\\"); break;
		case '\n': sb_puts(s, "\\n");  break;
		case '\t': sb_puts(s, "\\t");  break;
		case '\r': sb_puts(s, "\\r");  break;
		default:   sb_putc(s, *p);     break;
		}
	}
	sb_putc(s, '"');
}

static void json_emit(sbuf *s, rval *v, int pretty, int depth) {
	if (!v) { sb_puts(s, "null"); return; }
	switch (v->k) {
	case RV_NONE:  sb_puts(s, "null");                       break;
	case RV_INT:   sb_putl(s, v->i);                         break;
	case RV_FLOAT: sb_puts(s, fmt_double(v->f));             break;
	case RV_BOOL:  sb_puts(s, v->i ? "true" : "false");      break;
	case RV_STR:   json_emit_str(s, v->s ? v->s : "");       break;
	case RV_FILE:  json_emit_str(s, v->s ? v->s : "");       break;
	case RV_LIST:
	case RV_TUPLE:
		sb_putc(s, '[');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_putc(s, ',');
			if (pretty) json_indent(s, depth + 1);
			json_emit(s, v->items[i], pretty, depth + 1);
		}
		if (pretty && v->n) json_indent(s, depth);
		sb_putc(s, ']');
		break;
	case RV_MAP:
		sb_putc(s, '{');
		for (size_t i = 0; i < v->n; i++) {
			if (i) sb_putc(s, ',');
			if (pretty) json_indent(s, depth + 1);
			json_emit_str(s, v->keys[i]);
			sb_putc(s, ':');
			if (pretty) sb_putc(s, ' ');
			json_emit(s, v->items[i], pretty, depth + 1);
		}
		if (pretty && v->n) json_indent(s, depth);
		sb_putc(s, '}');
		break;
	default:       sb_puts(s, "null");                       break;
	}
}

static rval *do_json_str(rval *v, int pretty) {
	sbuf s = {0};
	json_emit(&s, v, pretty, 0);
	return rv_str(s.buf ? s.buf : a_dup(""));
}

/* map(items): build a Map from a list/tuple of (key, value) pairs. */
static rval *do_keys(rval *m) {
	rval *l = rv(RV_LIST);
	if (m && m->k == RV_MAP)
		for (size_t i = 0; i < m->n; i++) rv_push(l, rv_str(m->keys[i]));
	return l;
}
static rval *do_values(rval *m) {
	rval *l = rv(RV_LIST);
	if (m && m->k == RV_MAP)
		for (size_t i = 0; i < m->n; i++) rv_push(l, m->items[i]);
	return l;
}
static rval *do_items(rval *m) {
	rval *l = rv(RV_LIST);
	if (m && m->k == RV_MAP)
		for (size_t i = 0; i < m->n; i++) {
			rval *t = rv(RV_TUPLE);
			rv_push(t, rv_str(m->keys[i]));
			rv_push(t, m->items[i]);
			rv_push(l, t);
		}
	return l;
}
static rval *do_has(rval *m, rval *key) {
	if (!m || m->k != RV_MAP) return rv_bool(0);
	const char *k = as_cstr(key);
	for (size_t i = 0; i < m->n; i++)
		if (!strcmp(m->keys[i], k)) return rv_bool(1);
	return rv_bool(0);
}

/* is `name` a builtin (any arity)? used to make a bare reference a value */
int is_builtin(const char *s) {
	static const char *names[] = {
		"glob","zip","subst","stale","run","env","shell","which","exists",
		"mkdir","read","write","copy",
		"filter","contains","concat","length",
		"first","last","join","split",
		"cat",
		"parent","name","stem","suffix","with_suffix","with_name","path","abs",
		"print","debug",
		"json_parse","json_read","json_str","json_pretty","json_write",
		"get","has","keys","values","items",
		"to_int","to_str","to_float",
		"flag","opt","positionals", NULL };
	/* moved to plugins (fbfs/fbloop/fbstr): move symlink remove touch is_dir
	 * is_file flatten fold take drop sort unique replace upper lower trim
	 * starts_with ends_with capitalize title lstrip rstrip strip_prefix
	 * strip_suffix pad_* center range enumerate slice index reverse sum min max
	 * count find repeat chars -- resolved via fb_reg_find when loaded. */
	for (size_t i = 0; names[i]; i++) if (!strcmp(s, names[i])) return 1;
	return fb_reg_find(s) != NULL;   /* registered host/plugin builtin */
}

/* apply a unary builtin to one argument (e.g. used in `list |> name`). Builtins
 * that need more than one argument can't be used bare and yield unit. */
rval *call_builtin1(const char *n, rval *a) {
	const fb_Reg *rg = fb_reg_find(n);
	if (rg && rg->min <= 1 && (rg->max < 0 || rg->max >= 1)) {
		rval *as[1] = { a };
		return fb_dispatch(rg, as, 1);
	}
	return &RV_UNIT;   /* not a builtin, or one that can't be applied unary */
}

/* shell(v): like run() but silent (no UI), returning each child's captured
 * output as a String instead of streaming it. Reuses the library's capturing
 * runner (build_seq_visual into a silent group); we read back the per-command
 * build__buf and free what the group would otherwise hand to build_flush_outputs. */
static rval *capture_cmd(rval *v) {
	t_group_ctx grp = { .opts = { .silent = OPT_TRUE } };
	t_cmd c = {0};
	collect(v, &c);                       /* one argv (+ NULL terminator) */

	build_set_echo_start(0);              /* capture is quiet: no make-style echo */
	build_seq_visual(&c, g_env, &grp);    /* runs silent, captures into grp */

	size_t total = 0;
	for (size_t i = 0; i < grp.cmd_count; i++) {
		build__buf *b = (build__buf *)grp.commands[i].output;
		if (b) total += b->count;
	}
	char *out = a_alloc(total + 1);
	size_t n = 0;
	for (size_t i = 0; i < grp.cmd_count; i++) {
		build__buf *b = (build__buf *)grp.commands[i].output;
		if (b) {
			if (b->count) { memcpy(out + n, b->items, b->count); n += b->count; }
			free(b->items);
			free(b);
		}
		free(grp.commands[i].repr);
	}
	free(grp.commands);

	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;   /* trim trailing newline */
	out[n] = '\0';
	return rv_str(out);
}

/* run `pkg-config <args...>` (argv[0] must be "pkg-config"), capturing trimmed
 * stdout via the library's silent runner; sets *code to the child exit status.
 * Same capture/free pattern as capture_cmd, but keeps the exit code. */
static char *pkgconf_run(char **argv, size_t argc, int *code) {
	t_group_ctx grp = { .opts = { .silent = OPT_TRUE } };
	t_cmd c = {0};
	for (size_t i = 0; i < argc; i++) DA_APPEND(c, argv[i]);
	DA_APPEND(c, NULL);

	build_set_echo_start(0);
	build_seq_visual(&c, g_env, &grp);

	size_t total = 0;
	for (size_t i = 0; i < grp.cmd_count; i++) {
		build__buf *b = (build__buf *)grp.commands[i].output;
		if (b) total += b->count;
	}
	char *out = a_alloc(total + 1);
	size_t n = 0;
	int ec = 0;
	for (size_t i = 0; i < grp.cmd_count; i++) {
		build__buf *b = (build__buf *)grp.commands[i].output;
		if (b) {
			if (b->count) { memcpy(out + n, b->items, b->count); n += b->count; }
			free(b->items);
			free(b);
		}
		ec = grp.commands[i].exit_code;
		free(grp.commands[i].repr);
	}
	free(grp.commands);

	while (n && (out[n-1]=='\n' || out[n-1]=='\r' || out[n-1]==' ' || out[n-1]=='\t')) n--;
	out[n] = '\0';
	if (code) *code = ec;
	return out;
}

/* split on runs of whitespace, dropping empty tokens -> List<String> */
static rval *split_ws(const char *s) {
	rval *out = rv(RV_LIST);
	while (*s) {
		while (*s==' ' || *s=='\t' || *s=='\n' || *s=='\r') s++;
		if (!*s) break;
		const char *b = s;
		while (*s && *s!=' ' && *s!='\t' && *s!='\n' && *s!='\r') s++;
		rv_push(out, rv_str(a_dupn(b, s - b)));
	}
	return out;
}

/* pkgconfig(name) -> { found, cflags:List, libs:List, version } by shelling out
 * to pkg-config. `found` is pkg-config --exists succeeding; when absent the
 * lists are empty and version "". */
static rval *do_pkgconfig(const char *name) {
	char *nm = a_dup(name);
	rval *m = rv(RV_MAP);

	int code = 0;
	char *ex[] = { "pkg-config", "--exists", nm };
	(void)pkgconf_run(ex, 3, &code);
	int found = (code == 0);
	map_set(m, a_dup("found"), rv_bool(found));

	if (!found) {
		map_set(m, a_dup("cflags"),  rv(RV_LIST));
		map_set(m, a_dup("libs"),    rv(RV_LIST));
		map_set(m, a_dup("version"), rv_str(a_dup("")));
		return m;
	}

	char *cf[] = { "pkg-config", "--cflags", nm };
	char *lf[] = { "pkg-config", "--libs", nm };
	char *vf[] = { "pkg-config", "--modversion", nm };
	map_set(m, a_dup("cflags"),  split_ws(pkgconf_run(cf, 3, NULL)));
	map_set(m, a_dup("libs"),    split_ws(pkgconf_run(lf, 3, NULL)));
	map_set(m, a_dup("version"), rv_str(pkgconf_run(vf, 3, NULL)));
	return m;
}

static rval *do_shell(rval *v) {
	if (!v) return rv_str(a_dup(""));
	switch (v->k) {
	case RV_TUPLE: {                       /* nest captured strings per structure */
		rval *t = rv(RV_TUPLE);
		for (size_t i = 0; i < v->n; i++) rv_push(t, do_shell(v->items[i]));
		return t;
	}
	case RV_LIST: {
		rval *l = rv(RV_LIST);
		for (size_t i = 0; i < v->n; i++) rv_push(l, do_shell(v->items[i]));
		return l;
	}
	case RV_SEQ: {
		rval *t = rv(RV_TUPLE);
		for (size_t i = 0; i < v->body->nkids; i++)
			rv_push(t, do_shell(eval(v->body->kids[i], v->env)));
		return t;
	}
	case RV_CMD:
		return capture_cmd(v);
	default:
		return rv_str(a_dup(""));
	}
}

#define AR(i) ((rval *)a[i])
#define AC(i) as_cstr((rval *)a[i])
#define AI(k) (((rval *)a[k])->i)
#define RV(x) ((fb_Value)(x))

/* generators for the common wrapper shapes */
#define WV1(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(fn(AR(0)));}
#define WV2(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(fn(AR(0),AR(1)));}
#define WC1(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(fn(AC(0)));}
#define WC2(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(fn(AC(0),AC(1)));}
#define WVN(w,fn) static fb_Value w(fb_State*S,fb_Value*a,int n){(void)S;return RV(fn((rval**)a,(size_t)n));}

WC1(w_glob, do_glob)          WVN(w_zip, do_zip)          WVN(w_stale, do_stale)
WC1(w_env, do_env)            WV1(w_shell, do_shell)      WC1(w_which, do_which)
WV1(w_exists, do_exists)      WV1(w_mkdir, do_mkdir)      WV1(w_read, do_read)
WV2(w_write, do_write)        WV2(w_copy, do_copy)
WV2(w_filter, do_filter)      WV2(w_contains, do_contains)WV1(w_length, do_length)      WV1(w_first, do_first)      WV1(w_last, do_last)
WVN(w_concat, do_concat)    WVN(w_cat, do_cat)
WC2(w_split, do_split)        WV1(w_to_int, do_to_int)    WV1(w_to_str, do_to_str)
WV1(w_to_float, do_to_float)
WV1(w_abs, do_abs)          WVN(w_path, do_path)
WV2(w_has, do_has)            WV1(w_keys, do_keys)        WV1(w_values, do_values)
WV1(w_items, do_items)        WV1(w_sleep, do_sleep)      WV1(w_config, do_config)
WV2(w_opt, do_opt)            /* wrappers with argument reshaping / mixed types */
static fb_Value w_subst(fb_State*S,fb_Value*a,int n){(void)S;(void)n;
	return RV(rv_str(subst_a(AC(1), AC(2), AC(0)))); }        /* subst(s,from,to) */
static fb_Value w_join(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_join(AR(0), AC(1)));}
static fb_Value w_with_suffix(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_with_suffix(AR(0), AC(1)));}
static fb_Value w_with_name(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_with_name(AR(0), AC(1)));}
static fb_Value w_parent(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(rv_str(path_dir(pstr(AR(0)))));}
static fb_Value w_name(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(rv_str(path_base(pstr(AR(0)))));}
static fb_Value w_stem(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(rv_str(path_stem(pstr(AR(0)))));}
static fb_Value w_suffix(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(rv_str(path_suffix(pstr(AR(0)))));}
static fb_Value w_get(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(map_get(AR(0), AC(1)));}
static fb_Value w_clock(fb_State*S,fb_Value*a,int n){(void)S;(void)a;(void)n;return RV(do_clock());}
static fb_Value w_time(fb_State*S,fb_Value*a,int n){(void)S;(void)a;(void)n;return RV(do_time());}
static fb_Value w_positionals(fb_State*S,fb_Value*a,int n){(void)S;(void)a;(void)n;return RV(do_positionals());}
static fb_Value w_json_parse(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_json_parse(AC(0)));}
static fb_Value w_json_read(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_json_parse(as_cstr(do_read(AR(0)))));}
static fb_Value w_json_str(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_json_str(AR(0), 0));}
static fb_Value w_json_pretty(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_json_str(AR(0), 1));}
static fb_Value w_json_write(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_write(AR(0), do_json_str(AR(1), 0)));}
static fb_Value w_print(fb_State*S,fb_Value*a,int n){(void)S;return RV(do_print((rval**)a,(size_t)n,0));}
static fb_Value w_debug(fb_State*S,fb_Value*a,int n){(void)S;return RV(do_print((rval**)a,(size_t)n,1));}
static fb_Value w_flag(fb_State*S,fb_Value*a,int n){(void)S;return RV(do_flag((rval**)a,(size_t)n));}
static fb_Value l_pkgconfig(fb_State*S,fb_Value*a,int n){(void)S;(void)n;return RV(do_pkgconfig(AC(0)));}
/* bench(label, expr): FB_LAZY -- times evaluation of the (unevaluated) expr
 * between two clock reads, prints elapsed when --bench is set, returns expr's
 * value so it stays transparent in a pipeline. The reference lazy builtin. */
static fb_Value w_bench(fb_State*S,fb_Value*a,int n){(void)n;
	const char *label = as_cstr((rval *)fb_force(S, a[0]));
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rval *r = (rval *)fb_force(S, a[1]);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (g_bench) {
		double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3
		          + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
		size_t cap = strlen(label) + 64;
		char *line = a_alloc(cap);
		snprintf(line, cap, "[bench] %s: %.3f ms", label, ms);
		print_emit(line);
	}
	return (fb_Value)r; }
/* run(targets...): build a target (single, or a tuple = parallel), run it,
 * return whether it carried any work. Eager: args are evaluated commands/seqs. */
/* registered so the type checker recognizes the bare identifiers `args`/`cwd`
 * (FB_GLOBAL: typed from `ret`, not RET_VAR); the real values are bound by
 * run_program via fb_global_bind, which eval() resolves first, so this only
 * runs if someone calls args(...)/cwd(...) directly. */
static fb_Value w_global_noncallable(fb_State*S,fb_Value*a,int n){(void)a;(void)n;
	return fb_error(S, "not callable"); }

static fb_Value w_run(fb_State*S,fb_Value*a,int n){(void)S;
	rval *target;
	if (n == 1) target = AR(0);
	else { target = rv(RV_TUPLE); for (int i = 0; i < n; i++) rv_push(target, AR(i)); }
	int ran = has_cmd(target);
	run_tree(target);
	return RV(rv_bool(ran)); }

/* Register the interpreter's ABI builtins. Idempotent (fb_register dedups). */
void fb_open_base(fb_State *S) {
	static const fb_Reg base[] = {
		/* io / process */
		{"env",w_env,1,1,FB_RET_STR,0}, {"shell",w_shell,1,1,FB_RET_ANY,0},
		{"which",w_which,1,1,FB_RET_FILE,0}, {"pkgconfig",l_pkgconfig,1,1,FB_RET_MAP,0},
		{"run",w_run,1,-1,FB_RET_BOOL,0}, {"print",w_print,0,-1,FB_RET_UNIT,0},
		{"debug",w_debug,0,-1,FB_RET_UNIT,0}, {"config",w_config,1,1,FB_RET_UNIT,0},
		/* time */
		{"clock",w_clock,0,0,FB_RET_INT,0}, {"time",w_time,0,0,FB_RET_INT,0},
		{"sleep",w_sleep,1,1,FB_RET_UNIT,0}, {"bench",w_bench,2,2,FB_RET_ANY,FB_LAZY},
		/* filesystem (move/symlink/remove/touch/is_dir/is_file -> fbfs plugin) */
		{"glob",w_glob,1,1,FB_RET_LIST,0}, {"exists",w_exists,1,1,FB_RET_BOOL,0},
		{"mkdir",w_mkdir,1,1,FB_RET_FILE,0}, {"read",w_read,1,1,FB_RET_STR,0},
		{"write",w_write,2,2,FB_RET_FILE,0}, {"copy",w_copy,2,2,FB_RET_FILE,0},
		{"stale",w_stale,1,-1,FB_RET_BOOL,0},
		/* list glue (iteration/transform ops -> fbloop plugin) */
		{"zip",w_zip,2,-1,FB_RET_LIST,0}, {"filter",w_filter,2,2,FB_RET_LIST,0},
		{"contains",w_contains,2,2,FB_RET_BOOL,0},
		{"concat",w_concat,0,-1,FB_RET_LIST,0},
		{"length",w_length,1,1,FB_RET_INT,0}, {"first",w_first,1,1,FB_RET_ANY,0},
		{"last",w_last,1,1,FB_RET_ANY,0},
		/* string glue (text utilities -> fbstr plugin) */
		{"cat",w_cat,0,-1,FB_RET_STR,0}, {"join",w_join,2,2,FB_RET_STR,0},
		{"split",w_split,2,2,FB_RET_LIST,0}, {"subst",w_subst,3,3,FB_RET_STR,0},
		{"to_int",w_to_int,1,1,FB_RET_INT,0}, {"to_str",w_to_str,1,1,FB_RET_STR,0},
		{"to_float",w_to_float,1,1,FB_RET_FLOAT,0},
		/* path */
		{"parent",w_parent,1,1,FB_RET_STR,0}, {"name",w_name,1,1,FB_RET_STR,0},
		{"stem",w_stem,1,1,FB_RET_STR,0}, {"suffix",w_suffix,1,1,FB_RET_STR,0},
		{"with_suffix",w_with_suffix,2,2,FB_RET_FILE,0}, {"with_name",w_with_name,2,2,FB_RET_FILE,0},
		{"path",w_path,0,-1,FB_RET_FILE,0}, {"abs",w_abs,1,1,FB_RET_FILE,0},
		/* json / map */
		{"json_parse",w_json_parse,1,1,FB_RET_ANY,0}, {"json_read",w_json_read,1,1,FB_RET_ANY,0},
		{"json_str",w_json_str,1,1,FB_RET_STR,0}, {"json_pretty",w_json_pretty,1,1,FB_RET_STR,0},
		{"json_write",w_json_write,2,2,FB_RET_FILE,0}, {"get",w_get,2,2,FB_RET_ANY,0},
		{"has",w_has,2,2,FB_RET_BOOL,0}, {"keys",w_keys,1,1,FB_RET_LIST,0},
		{"values",w_values,1,1,FB_RET_LIST,0}, {"items",w_items,1,1,FB_RET_LIST,0},
		/* cli args */
		{"flag",w_flag,1,-1,FB_RET_BOOL,0}, {"opt",w_opt,2,2,FB_RET_STR,0},
		{"positionals",w_positionals,0,0,FB_RET_LIST,0},
		/* CLI globals (bound values, not calls; see run_program's fb_global_bind) */
		{"args",w_global_noncallable,0,-1,FB_RET_LIST_STR,FB_GLOBAL},
		{"cwd",w_global_noncallable,0,-1,FB_RET_STR,FB_GLOBAL},
	};
	for (size_t i = 0; i < sizeof base / sizeof base[0]; i++)
		fb_register(S, &base[i]);
}

