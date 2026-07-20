/* fbfs -- filesystem tools for fbuild, as a plugin (.so).
 *
 * Build: cc -shared -fPIC -Isrc plugins/fbfs.c -o build/fbfs.so
 * Load:  FBUILD_PLUGINS=build/fbfs.so ./fbuild   (or list several, colon-sep)
 *
 * The build-essential FS ops (glob, stale, exists, mkdir, read, write, copy)
 * stay in the core interpreter; this plugin carries the rest.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "fbuild.h"

#define P(v) fb_tostr(S, (v))

/* byte copy src -> dst; 0 on success */
static int copy_file(const char *src, const char *dst) {
	int in = open(src, O_RDONLY);
	if (in < 0) return -1;
	struct stat st; fstat(in, &st);
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode ? st.st_mode & 0777 : 0644);
	if (out < 0) { close(in); return -1; }
	char buf[65536]; ssize_t n;
	int rc = 0;
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, (size_t)n) != n) { rc = -1; break; }
	if (n < 0) rc = -1;
	close(in); close(out);
	return rc;
}

/* move(src, dst) -> File: rename, falling back to copy+unlink across devices. */
static fb_Value bi_move(fb_State *S, fb_Value *a, int n) {
	(void)n;
	const char *s = P(a[0]); char d[4096]; snprintf(d, sizeof d, "%s", P(a[1]));
	if (rename(s, d) != 0 && copy_file(s, d) == 0) unlink(s);
	return fb_file(S, d);
}

/* symlink(target, link) -> File: (re)create the symlink. */
static fb_Value bi_symlink(fb_State *S, fb_Value *a, int n) {
	(void)n;
	const char *t = P(a[0]); char l[4096]; snprintf(l, sizeof l, "%s", P(a[1]));
	unlink(l);
	symlink(t, l);
	return fb_file(S, l);
}

/* remove(path) -> Bool: unlink a file, else rmdir a directory. */
static fb_Value bi_remove(fb_State *S, fb_Value *a, int n) {
	(void)n;
	const char *p = P(a[0]);
	if (unlink(p) == 0) return fb_bool(S, 1);
	return fb_bool(S, rmdir(p) == 0);
}

/* touch(path) -> File: create if absent, bump mtime to now. */
static fb_Value bi_touch(fb_State *S, fb_Value *a, int n) {
	(void)n;
	const char *p = P(a[0]);
	int fd = open(p, O_WRONLY | O_CREAT, 0644);
	if (fd >= 0) close(fd);
	utimes(p, NULL);
	return fb_file(S, p);
}

/* is_dir / is_file(path) -> Bool */
static fb_Value bi_is_dir(fb_State *S, fb_Value *a, int n) {
	(void)n; struct stat st;
	return fb_bool(S, stat(P(a[0]), &st) == 0 && S_ISDIR(st.st_mode));
}
static fb_Value bi_is_file(fb_State *S, fb_Value *a, int n) {
	(void)n; struct stat st;
	return fb_bool(S, stat(P(a[0]), &st) == 0 && S_ISREG(st.st_mode));
}

int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	static const fb_Reg regs[] = {
		{ "move", bi_move, 2, 2, FB_RET_FILE, 0, NULL },
		{ "symlink", bi_symlink, 2, 2, FB_RET_FILE, 0, NULL },
		{ "remove", bi_remove, 1, 1, FB_RET_BOOL, 0, NULL },
		{ "touch", bi_touch, 1, 1, FB_RET_FILE, 0, NULL },
		{ "is_dir", bi_is_dir, 1, 1, FB_RET_BOOL, 0, NULL },
		{ "is_file", bi_is_file, 1, 1, FB_RET_BOOL, 0, NULL },
	};
	for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
		fb_register(S, &regs[i]);
	return 0;
}
