/* funcbuild + raylib, fully auto-bound. Registers the whole generated raylib
 * surface (raylib_gen.c) then runs a .fb script that must define `main`, called
 * once per frame between BeginDrawing/EndDrawing.
 *
 *   ./gfx-auto [script.fb]      (default: demo_full.fb)
 */
#include <stdio.h>
#include <stdlib.h>
#include "fbuild.h"
#include "raylib.h"

void raylib_register(fb_State *S);   /* from raylib_gen.c */

/* persistent state slot so a script can carry data across frames (the language
 * has no mutable global). save(x) stores x and returns it; restore() returns
 * the last saved value, or nil the first time. */
static fb_Value g_slot;
static int      g_have_slot;
static fb_Value l_save(fb_State *S, fb_Value *a, int n) {
	(void)S; (void)n; g_slot = a[0]; g_have_slot = 1; return a[0];
}
static fb_Value l_restore(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n; return g_have_slot ? g_slot : fb_nil(S);
}

int main(int argc, char **argv) {
	const char *script = argc > 1 ? argv[1] : "demo_full.fb";

	fb_State *S = fb_new();
	fb_set_args(S, 0, NULL);
	raylib_register(S);              /* all bound raylib funcs + enum constants */
	fb_register(S, &(fb_Reg){ "save",    l_save,    1, 1, FB_RET_ANY, 0 });
	fb_register(S, &(fb_Reg){ "restore", l_restore, 0, 0, FB_RET_ANY, 0 });

	InitWindow(800, 600, "funcbuild + raylib (auto)");
	SetTargetFPS(60);

	if (fb_dofile(S, script)) { fprintf(stderr, "fb: %s\n", fb_error_msg(S)); CloseWindow(); return 1; }
	fb_Value mainfn = fb_global(S, "main");
	if (fb_kind(mainfn) != FB_LAMBDA) { fprintf(stderr, "script must define `main`\n"); CloseWindow(); return 1; }

	long frame = 0;
	while (!WindowShouldClose()) {
		BeginDrawing();
		fb_Value arg[1] = { fb_int(S, frame) };
		fb_apply(S, mainfn, arg, 1);
		EndDrawing();
		frame++;
	}
	CloseWindow();
	fb_close(S);
	return 0;
}
