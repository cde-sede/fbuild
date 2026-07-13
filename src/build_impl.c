/* cds-lib runtime implementation TU.
 *
 * build.h is a header-only library whose implementation (BUILD_IMPLEMENTATION)
 * is heavy to compile (~5s). It used to be emitted inside run.c, forcing every
 * edit to run.c to pay that cost serially. It now lives here, in its own small
 * translation unit, so it compiles once in parallel with the rest of src/ and
 * links against the header-only consumers (run.c, run_builtins.c).
 *
 * The metadata block (build_info / .my_metadata) is intentionally NOT emitted
 * here: run.c defines METADATA_LIST and owns the single static build_info that
 * fbuild_version() reads back. */

#define _GNU_SOURCE

/* preamble must match run_internal.h's so build.h sees identical config */
#define BUILD_DA_GENERIC_EXTRA(func)   \
    DA_DECL_GENERIC(char, func)        \
    DA_DECL_GENERIC(voidptr, func)     \
    DA_DECL_GENERIC(int, func)
#define BUILD_TAIL_LINES 5
#define CORO_STACK_SIZE (512 * 1024)
#define BUILD_IMPLEMENTATION

typedef void* voidptr;
#include "cds_lib/build.h"

/* build_ui_cleanup is static inside build.h's implementation block, so it is
 * only linkable from this TU. Expose a non-static shim for fb_close (run.c). */
void fb_ui_cleanup(void) { build_ui_cleanup(); }
