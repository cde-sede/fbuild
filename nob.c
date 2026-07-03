#define NOB_IMPLEMENTATION
#include "nob.h"

/* bootstrap: cc -o nob nob.c   then   ./nob
 * language sources live in src/; cgen is the symlinked runtime (-Icgen).
 *
 * Builds three artifacts:
 *   libfbuild.a / libfbuild.so   the interpreter as a library (no main)
 *   fbuild                       thin CLI = src/main.c linked against the lib
 */

/* library translation units (everything except the CLI entry point) */
static const char *lib_src[] = {
	"src/build_system.c", "src/parser.c", "src/walk.c", "src/run.c", "src/eval.c",
};
/* headers: any change forces a full rebuild (nob can't see #include deps) */
static const char *hdrs[] = {
	"src/cparser.h", "src/ast.h", "src/graph.h", "src/fbuild.h",
	"cgen/cds_lib/build.h",
};

static int compile(const char *src, const char *obj) {
	Nob_Cmd cmd = {0};
	nob_cc(&cmd);
	nob_cmd_append(&cmd, "-ggdb", "-Icgen", "-fPIC");
	nob_cc_flags(&cmd);
	nob_cmd_append(&cmd, "-c", src, "-o", obj);
	return nob_cmd_run_sync_and_reset(&cmd);
}

int main(int argc, char **argv) {
	NOB_GO_REBUILD_URSELF(argc, argv);

	/* rebuild everything when any source or header is newer than the exe */
	Nob_File_Paths deps = {0};
	for (size_t i = 0; i < NOB_ARRAY_LEN(lib_src); i++) nob_da_append(&deps, lib_src[i]);
	for (size_t i = 0; i < NOB_ARRAY_LEN(hdrs);    i++) nob_da_append(&deps, hdrs[i]);
	nob_da_append(&deps, "src/main.c");
	int need = nob_needs_rebuild("fbuild", deps.items, deps.count);
	if (need < 0) return 1;
	if (!need) { nob_log(NOB_INFO, "fbuild up to date"); return 0; }

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
		nob_cmd_append(&cmd, "-ldl");
		if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
	}
	nob_log(NOB_INFO, "built: libfbuild.a / libfbuild.so");

	/* fbuild = CLI object linked against the static library */
	if (!compile("src/main.c", "build/main.c.o")) return 1;
	{
		Nob_Cmd cmd = {0};
		nob_cc(&cmd);
		nob_cmd_append(&cmd, "-Icgen", "-rdynamic");   /* export fb_* for plugins */
		nob_cc_output(&cmd, "fbuild");
		nob_cmd_append(&cmd, "build/main.c.o", "libfbuild.a", "-ldl");
		if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
	}
	nob_log(NOB_INFO, "built: fbuild");
	return 0;
}
