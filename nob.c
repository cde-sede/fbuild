#define NOB_IMPLEMENTATION
#include "nob.h"

/* bootstrap: cc -o nob nob.c   then   ./nob
 * language sources live in src/; cgen is the symlinked runtime (-Icgen). */

int main(int argc, char **argv) {
	NOB_GO_REBUILD_URSELF(argc, argv);

	const char *inputs[] = {
		"src/build_system.c", "src/main.c", "src/walk.c", "src/run.c",
		"src/eval.c", "src/cparser.h", "src/ast.h", "src/graph.h",
	};
	int need = nob_needs_rebuild("fbuild", inputs, NOB_ARRAY_LEN(inputs));
	if (need < 0) return 1;
	if (!need) {
		nob_log(NOB_INFO, "fbuild up to date");
		return 0;
	}

	Nob_Cmd cmd = {0};
	nob_cc(&cmd);
	nob_cmd_append(&cmd, "-ggdb", "-Icgen");
	nob_cc_flags(&cmd);
	nob_cc_output(&cmd, "fbuild");
	nob_cc_inputs(&cmd, "src/build_system.c", "src/main.c", "src/walk.c",
	              "src/run.c", "src/eval.c");
	if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;

	nob_log(NOB_INFO, "built: fbuild");
	return 0;
}
