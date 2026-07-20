/* fbuild CLI: a thin driver over libfbuild (see fbuild.h). Discovers the build
 * script in the cwd, forwards args, and runs it. All interpreter logic lives in
 * the library; this file is the only part that isn't linked when embedding. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/syscall.h>

#include "cparser.h"   /* arena_fini, a_dup */
#include "ast.h"       /* fbuild_version */
#include "fbuild.h"

#define die(msg, ...) do { fprintf(stderr, "error:" msg "\n", ##__VA_ARGS__); exit(1); } while (0)

/* registered so the type checker recognizes the bare identifiers `script` and
 * `plugins`; the real values come from fb_global_bind below, which eval()
 * resolves first, so these only run if someone calls script(...)/plugins(...)
 * directly. Same pattern a plugin uses for a bare namespace object (see
 * plugins/fbregex.c). */
static fb_Value cli_global_noncallable(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n;
	return fb_error(S, "not callable");
}

/* SIGINT: the build runs nested event loops with spinners that hide the cursor,
 * so on Ctrl-C restore the cursor and print a notice. Only async-signal-safe
 * calls here (write, munmap via arena_fini, _exit). isatty cached at startup. */
static volatile sig_atomic_t g_stderr_tty = 0;
static void on_sigint(int sig) {
	(void)sig;
	if (g_stderr_tty) {
		static const char msg[] = "\033[?25h\n\033[33minterrupted by user\033[0m\n";
		write(STDERR_FILENO, msg, sizeof msg - 1);
	} else {
		static const char msg[] = "\ninterrupted by user\n";
		write(STDERR_FILENO, msg, sizeof msg - 1);
	}
	arena_fini();   /* release the program arena (munmap is signal-safe) */
	_exit(130);
}

/* one directory entry from getdents64 (kernel ABI; no portable header) */
struct find_dirent64 {
	unsigned long long d_ino;
	long long          d_off;
	unsigned short     d_reclen;
	unsigned char      d_type;
	char               d_name[];
};

/* locate the build script in the cwd: prefer the conventional names, else the
 * sole *.fb file; error if none or ambiguous. Reads the dir with getdents64. */
static const char *find_script(void) {
	static const char *named[] = { ".fb", "Build.fb", "build.fb", NULL };
	for (int i = 0; named[i]; i++)
		if (access(named[i], R_OK) == 0)
			return named[i];

	int fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		die("cannot read current directory");
	const char *found = NULL;
	int count = 0;
	char buf[8192];
	for (;;) {
		long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
		if (n <= 0) break;
		for (long off = 0; off < n; ) {
			struct find_dirent64 *e = (struct find_dirent64 *)(buf + off);
			off += e->d_reclen;
			size_t l = strlen(e->d_name);
			if (l > 3 && !strcmp(e->d_name + l - 3, ".fb")) {
				count++;
				found = a_dup(e->d_name);
			}
		}
	}
	close(fd);

	if (count == 1)
		return found;
	if (count == 0)
		die("no build script: expected .fb / Build.fb / build.fb or a single *.fb");
	die("ambiguous: multiple *.fb files; name one .fb / Build.fb / build.fb");
	return NULL;
}

int main(int ac, char **av) {
	if (ac == 2 && !strcmp(av[1], "--version")) {
		printf("fbuild %s\n", fbuild_version());
		return 0;
	}

	g_stderr_tty = isatty(STDERR_FILENO) ? 1 : 0;
	struct sigaction sa = {0};
	sa.sa_handler = on_sigint;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;            /* no SA_RESTART: interrupt the blocked wait */
	sigaction(SIGINT, &sa, NULL);

	fb_State *S = fb_new();          /* inits the arena; find_script uses a_dup */

	/* ::: splits fbuild's own flags (before it) from args forwarded verbatim
	 * to the script (at and after it, `:::` included) — same separator
	 * run_program() looks for when building the script's `args` list. Flags
	 * below are only recognized strictly before the separator, so e.g.
	 * `fbuild ::: --list-plugins` forwards --list-plugins to the script
	 * instead of acting on it. */
	int sep = ac;
	for (int i = 1; i < ac; i++)
		if (!strcmp(av[i], ":::")) { sep = i; break; }

	/* --plugin PATH / --plugin=PATH : dlopen extensions before running, and
	 * drop them from the args the script sees (compacted in place, no alloc).
	 * --script PATH / --script=PATH : run PATH instead of the discovered
	 * script (same arg-dropping treatment); FBUILD_SCRIPT env var is the
	 * lower-precedence fallback for the same override.
	 * --list-plugins : print the resolved plugin list (--plugin flags seen so
	 * far, then $FBUILD_PLUGINS / $FBUILD_PLUGIN_PATH) and exit without running
	 * anything. */
	int fc = 1;
	const char *script = NULL;
	int list_plugins = 0;
	for (int i = 1; i < ac; i++) {
		if (i >= sep) { av[fc++] = av[i]; continue; }
		const char *path = NULL;
		if (!strcmp(av[i], "--plugin")) {
			if (i + 1 >= ac) die("--plugin needs a path");
			path = av[++i];
		} else if (!strncmp(av[i], "--plugin=", 9)) {
			path = av[i] + 9;
		} else if (!strcmp(av[i], "--script")) {
			if (i + 1 >= ac) die("--script needs a path");
			script = av[++i];
			continue;
		} else if (!strncmp(av[i], "--script=", 9)) {
			script = av[i] + 9;
			continue;
		} else if (!strcmp(av[i], "--list-plugins")) {
			list_plugins = 1;
			continue;
		} else {
			av[fc++] = av[i];
			continue;
		}
		if (fb_load_plugin(S, path)) die("%s", fb_error_msg(S));
	}
	if (!script) script = getenv("FBUILD_SCRIPT");

	if (list_plugins) {
		if (fb_load_env_plugins(S)) die("%s", fb_error_msg(S));
		int n = fb_plugin_count(S);
		for (int i = 0; i < n; i++) printf("%s\n", fb_plugin_path(S, i));
		fb_close(S);
		return 0;
	}

	if (!script) script = find_script();

	/* expose `script` (the .fb path being run) and `plugins` (loaded .so
	 * paths, --plugin + $FBUILD_PLUGINS + $FBUILD_PLUGIN_PATH) to the script
	 * via the same bind mechanism a plugin would use -- main.c is just
	 * another consumer of the extension ABI, not a special case in the
	 * interpreter core. fb_load_env_plugins is idempotent, so calling it here
	 * to finalize the list before fb_dofile calls it again is safe. */
	if (fb_load_env_plugins(S)) die("%s", fb_error_msg(S));
	fb_register(S, &(fb_Reg){ "script", cli_global_noncallable, 0, -1, FB_RET_STR, FB_GLOBAL,
	            "script: String -- path of the running script" });
	fb_register(S, &(fb_Reg){ "plugins", cli_global_noncallable, 0, -1, FB_RET_LIST_STR, FB_GLOBAL,
	            "plugins: List<String> -- paths of every loaded plugin" });
	fb_global_bind(S, "script", fb_str(S, script));
	fb_Value plugins = fb_list(S);
	for (int i = 0; i < fb_plugin_count(S); i++)
		fb_push(S, plugins, fb_str(S, fb_plugin_path(S, i)));
	fb_global_bind(S, "plugins", plugins);

	fb_set_args(S, fc, av);
	int rc = fb_dofile(S, script);
	if (rc && fb_error_msg(S)[0])
		fprintf(stderr, "error: %s\n", fb_error_msg(S));
	fb_close(S);
	return rc;
}
