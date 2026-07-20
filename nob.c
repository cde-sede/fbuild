#define NOB_IMPLEMENTATION
#include "nob.h"

/* bootstrap: cc -o nob nob.c   then   ./nob   [fbuildd]
 * language sources live in src/; cds-lib is the symlinked runtime (-Icds-lib).
 *
 * Builds, always:
 *   libfbuild.a / libfbuild.so   the interpreter as a library (no main)
 *   fbuild                       thin CLI = src/main.c linked against the lib
 * Builds, opt-in (pass "fbuildd" as an argument, like `./nob fbuildd`):
 *   fbuildd                      LSP daemon = src/main_lspd.c linked against the lib
 */

/* library translation units (everything except the CLI entry point) */
static const char *lib_src[] = {
	"src/build_system.c", "src/parser.c", "src/walk.c", "src/run.c", "src/eval.c",
	"src/run_builtins.c", "src/build_impl.c",
};
/* headers: any change forces a full rebuild (nob can't see #include deps) */
static const char *hdrs[] = {
	"src/cparser.h", "src/ast.h", "src/graph.h", "src/fbuild.h", "src/run_internal.h",
	"cds-lib/cds_lib/build.h",
};

static int compile(const char *src, const char *obj) {
	Nob_Cmd cmd = {0};
	nob_cc(&cmd);
	nob_cmd_append(&cmd, "-ggdb", "-Icds-lib", "-fPIC");
	nob_cc_flags(&cmd);
	nob_cmd_append(&cmd, "-c", src, "-o", obj);
	return nob_cmd_run_sync_and_reset(&cmd);
}

int main(int argc, char **argv) {
	NOB_GO_REBUILD_URSELF(argc, argv);

	int want_lspd = 0;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "fbuildd")) want_lspd = 1;

	/* rebuild everything when any source or header is newer than the exe.
	 * deps_exe and deps_lspd are independent arrays (each built fresh from
	 * lib_src[]/hdrs[]) rather than copies of one another, since Nob_File_Paths
	 * is a dynamic array whose backing buffer nob_da_append may realloc --
	 * sharing one buffer between two of these structs would risk a dangling
	 * pointer in one once the other reallocs it. */
	Nob_File_Paths deps_exe = {0};
	for (size_t i = 0; i < NOB_ARRAY_LEN(lib_src); i++) nob_da_append(&deps_exe, lib_src[i]);
	for (size_t i = 0; i < NOB_ARRAY_LEN(hdrs);    i++) nob_da_append(&deps_exe, hdrs[i]);
	nob_da_append(&deps_exe, "src/main.c");
	int need = nob_needs_rebuild("fbuild", deps_exe.items, deps_exe.count);
	if (need < 0) return 1;

	Nob_File_Paths deps_lspd = {0};
	int need_lspd = 0;
	if (want_lspd) {
		for (size_t i = 0; i < NOB_ARRAY_LEN(lib_src); i++) nob_da_append(&deps_lspd, lib_src[i]);
		for (size_t i = 0; i < NOB_ARRAY_LEN(hdrs);    i++) nob_da_append(&deps_lspd, hdrs[i]);
		nob_da_append(&deps_lspd, "src/main_lspd.c");
		need_lspd = nob_needs_rebuild("fbuildd", deps_lspd.items, deps_lspd.count);
		if (need_lspd < 0) return 1;
	}

	if (!need && !need_lspd) { nob_log(NOB_INFO, "fbuild up to date"); return 0; }

	if (!nob_mkdir_if_not_exists("build")) return 1;

	/* compile the library objects */
	Nob_File_Paths objs = {0};
	for (size_t i = 0; i < NOB_ARRAY_LEN(lib_src); i++) {
		const char *stem = nob_temp_sprintf("build/%s.o",
			nob_path_name(lib_src[i]));   /* e.g. build/run.c.o */
		if (!compile(lib_src[i], stem)) return 1;
		nob_da_append(&objs, stem);
	}

	/* libfbuild.a */
	{
		Nob_Cmd cmd = {0};
		nob_cmd_append(&cmd, "ar", "rcs", "libfbuild.a");
		for (size_t i = 0; i < objs.count; i++) nob_cmd_append(&cmd, objs.items[i]);
		if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
	}
	/* libfbuild.so */
	{
		Nob_Cmd cmd = {0};
		nob_cc(&cmd);
		nob_cmd_append(&cmd, "-shared", "-o", "libfbuild.so");
		for (size_t i = 0; i < objs.count; i++) nob_cmd_append(&cmd, objs.items[i]);
		nob_cmd_append(&cmd, "-ldl", "-lm");
		if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
	}
	nob_log(NOB_INFO, "built: libfbuild.a / libfbuild.so");

	/* fbuild = CLI object linked against the static library */
	if (need) {
		if (!compile("src/main.c", "build/main.c.o")) return 1;
		Nob_Cmd cmd = {0};
		nob_cc(&cmd);
		nob_cmd_append(&cmd, "-Icds-lib", "-rdynamic");   /* export fb_* for plugins */
		nob_cc_output(&cmd, "fbuild");
		nob_cmd_append(&cmd, "build/main.c.o", "libfbuild.a", "-ldl", "-lm");
		if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
		nob_log(NOB_INFO, "built: fbuild");
	}

	/* fbuildd = LSP daemon object linked against the static library, opt-in */
	if (need_lspd) {
		if (!compile("src/main_lspd.c", "build/main_lspd.c.o")) return 1;
		Nob_Cmd cmd = {0};
		nob_cc(&cmd);
		nob_cmd_append(&cmd, "-Icds-lib", "-rdynamic");   /* export fb_* for plugins */
		nob_cc_output(&cmd, "fbuildd");
		nob_cmd_append(&cmd, "build/main_lspd.c.o", "libfbuild.a", "-ldl", "-lm");
		if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
		nob_log(NOB_INFO, "built: fbuildd");
	}
	return 0;
}
