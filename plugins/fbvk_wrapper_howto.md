# Adding a new wrapped Vulkan function to plugins/fbvk.c

Context for agent task: wrap Vulkan function `vkXxx` as new method/field on
fbvk plugin. Self-contained — no need read whole 4000-line `plugins/fbvk.c`
first, just this. Read `src/fbuild.h` too before code; authoritative ABI doc,
short, this file assume it.

## The shape of the plugin

`plugins/fbvk.c` dlopens `libvulkan.so.1`/`libglfw.so.3` at runtime (no
link-time dep — matches every plugin in repo: "links against nothing but the
fbuild host"). Exposes **curated** API (`vk.app()`, `App.draw()`, ...), not
1:1 binding — most of ~623 Vulkan commands deliberately unwrapped (see
coverage comment block at end of file). Adding wrapper means:

1. dlsym raw `vkXxx` command (if not already fetched).
2. Write small C function translating funcbuild args ↔ Vulkan call.
3. Register as method (`fb_method`) or free function (`fb_register`).
4. Type params/return so checker not left showing `?`.
5. Mark `[x]` in coverage block at end of file.
6. Build + test.

## 1. Fetching the raw command

Every Vulkan command used anywhere in file is one entry in `VK_FN_LIST`
X-macro near top:

```c
#define VK_FN_LIST(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	...
	X(vkUnmapMemory)
```

Command not in list? Add one line: `X(vkYourCommand) \`. That's it —
`load_fns()` (right below macro) fetches every listed command via
`vkGetInstanceProcAddr(instance, name)` automatically, becomes callable as
`app->fn.vkYourCommand(...)` (every `vkapp_t` embeds `vk_fns_t fn`).

Read big comment directly above `load_fns` before assuming need
`vkGetDeviceProcAddr` instead — don't; plugin deliberately fetches
device-level commands through instance-level loader too (see comment for why
safe on real Vulkan Loader). Two commands (`vkCreateInstance`,
`vkGetInstanceProcAddr` itself) special-cased outside X-macro in
`ensure_loader()` — basically never touch those.

## 2. Writing the wrapper function

Look at existing one close to what adding first — `m_buf_destroy`
(zero-arg, self only), `bi_buffer` (constructor w/ args + object creation),
`m_draw` (method w/ typed native-class args) — three shapes to reuse.
Signature always:

```c
static fb_Value m_your_method(fb_State *S, fb_Value *a, int n) {
	vkapp_t *app = fb_object_data(a[0]);   /* a[0] is ALWAYS self for a method */
	/* real args start at a[1] */
	...
	VkResult r = app->fn.vkYourCommand(app->dev, ...);
	if (r != VK_SUCCESS) return fb_error(S, "vk: vkYourCommand failed (%d)", (int)r);
	return a[0];   /* or fb_bool/fb_int/fb_object(...) etc, whatever fits */
}
```

Critical, easy get wrong: **for method (`fb_method`), `a[0]` self, real args
start at `a[1]`.** For free function (`fb_register` directly, no
`fb_method`), no self — `a[0]` is first real arg. Get wrong, every arg reads
one slot off (see `bi_app`'s doc comment for worked note exact this).

`fb_object_data(a[0])` gets back native struct (`vkapp_t*`/`vkbuf_t*`/
`vkshader_t*`/`vkpipe_t*`) stashed via `fb_object()` at construction — how
state survives between calls.

## 3. Registering it

In `fbuild_open()`, near other methods same class:

```c
fb_method(S, APP_CLS, "your_method", m_your_method,
          &(fb_Reg){ "your_method", m_your_method, MIN, MAX, FB_RET_XXX, 0,
                     "one-line doc for hover", param_types, nparams, ret_class,
                     NULL, NULL });
```

- `MIN`/`MAX` count **real args only, self excluded** (same convention
  `obj.method(args)` uses at call site).
- `FB_RET_XXX`: plain return kind (`FB_RET_BOOL`, `FB_RET_INT`, ...) —
  ignored if `ret_class` set (see below).
- `param_types`: `static const fb_Ret arr[] = { FB_RET_INT, ... };` — one
  entry per real arg (self excluded), only covers plain kinds (Int/Float/Str/
  Bool/File/List/Map/Unit/Num). Pass `NULL, 0` if nothing to say.
- `ret_class`: set to `"App"`/`"Buffer"`/`"Shader"`/`"Pipeline"` (string,
  matching existing `fb_class(S, "...")` name) if this returns/re-returns
  instance of one those classes — e.g. every chainable method (`.poll()`,
  `.draw()`, ...) returns `a[0]`, sets `ret_class = "App"` so chaining keeps
  typing. `NULL` if return plain kind or `FB_RET_ANY`.

**Param must be specific native class** (e.g. `Pipeline`/`Buffer` argument)
can't be expressed in `param_types` (no class concept there) — `fb_Reg`
deliberately doesn't grow field for this (would force ABI bump +
recompiling every plugin, see big comment on `fb_reg_param_class` in
`fbuild.h` for why). Instead call separately right after `fb_method`, keyed
by exact `"ClassName.methodname"` string `fb_method`'s shadow registration
builds:

```c
fb_reg_param_class("App.your_method", 0, "Pipeline");  /* first real arg */
fb_reg_param_class("App.your_method", 1, "Buffer");    /* second real arg */
```

Index 0-based, self excluded, same counting as `min`/`max`.

**Plain data field** set via `fb_set()` at construction (not method) needs
matching `fb_class_field()` call or checker wrongly rejects reading it back
(`Object has no field 'X'`) on any class also having methods:

```c
fb_class_field(S, YOUR_CLS, "field_name", FB_RET_STR, NULL);
```

(4th arg `ret_class` if field itself native-class instance, else `NULL`.)

## 4. New class vs. new method on an existing class

Existing native classes: `App`, `Buffer`, `Shader`, `Pipeline`, `Device`
(plus `vk` namespace itself). Add method to one of these if thing wrapping
operates ON that resource. Only add whole new `fb_class(S, "Name")` +
`vkYourStruct_t` native-data struct if wrapping something genuinely new kind
of Vulkan handle/resource plugin doesn't yet model (e.g. descriptor set,
texture/image). Follow `Buffer`/`Shader` pattern: small
`typedef struct { vkapp_t *app; VkYourHandle h; ... } t;`, `fb_object(S, cls, ptr)`
to wrap it, `fb_object_data()` to unwrap in every method, `.destroy()`
method, `fb_class_field()` for whatever plain fields exposed.

## 5. Update the coverage block

At very end of `plugins/fbvk.c`: generated `[x]`/`[ ]` list of every Vulkan
command and type. After adding wrapper, flip command's marker from `[ ]` to
`[x]` (and any newly-referenced `VkXxx` type identifiers too — grep file for
exact identifier, check already `[x]`). Counts in header of block
(`Commands: N / 623`, `Types: N / 2301`) need bumping too. Don't regenerate
whole block from scratch for one-function change — hand-edit specific
lines; regen recipe in block's header comment for full refresh (e.g. after
Vulkan header upgrade), not routine.

## 6. Build + test loop

Compile plugin directly while iterating (fast, catches errors/warnings):

```sh
cc -shared -fPIC -Isrc -ggdb -Icds-lib -Wall -Wextra -O2 \
   -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 plugins/fbvk.c -o build/fbvk.so
```

Before done, also build via **real** project recipe, adds `-Werror`, catches
things command above won't (e.g. missing `fb_Reg` field initializers):

```sh
./fbuild plugins
```

(Never run bare `./fbuild` with no args/`--script` in this repo — that
self-builds *interpreter itself* from `src/*.c`. Safe, just not what want
here, slower.)

Test with throwaway `.fb` script:

```sh
FBUILD_PLUGINS=build/fbloop.so:build/fbvk.so ./fbuild --script /tmp/test.fb
```

`build/fbloop.so` needed if test uses `while`/`range`/etc. — separate plugin,
not core language. Two script-syntax traps not obvious from language docs
(SYNTAX.md/SKILLS.md), both bitten writers of existing
`examples/vk_triangle.fb`:

- List literals `List<1, 2, 3>`, **not** `[1, 2, 3]` (`[...]` Path literal
  this language).
- Negative number as `List<...>` element needs parens:
  `List<1, (0 - 2), 3>` — bare `List<1, -2, 3>` parse error.

Testing something requiring real GPU device query, `vk.devices()` works
headless (no window). Testing `App`/window-based method, need live
`$DISPLAY`/`$WAYLAND_DISPLAY` — check `echo $DISPLAY` first; unset, ask
before testing windowed path.

## 7. Commit when done

One function wrapped + tested (build clean, real `.fb` test passes) = one
commit, right then, before starting next function. Don't batch several
wrapped functions into one commit; don't leave a working wrapper uncommitted
while moving on.

- `git add` only what that function touched: the specific hunks in
  `plugins/fbvk.c` (VK_FN_LIST entry, wrapper fn, registration,
  `fb_reg_param_class`/`fb_class_field` calls if any, coverage-block
  marker+count edits) — check `git status`/`git diff` first, never `git add -A`.
- Conventional-commit subject, terse body, no `Co-Authored-By` trailer (see
  `git log` for tone): `feat(fbvk): wrap vkYourCommand as App.your_method`,
  one-line body only if the subject doesn't already say why.
- Touched shared files outside `plugins/fbvk.c` (e.g. `src/fbuild.h`/`run.c`/
  `walk.c` for a genuinely new ABI capability, not just calling an existing
  one)? Flag it in the report back instead of silently bundling into the same
  commit — bigger decision than one wrapper fn, needs a human call on scope.

## Worked example: wrapping `vkQueueWaitIdle` as `App.wait_idle()`

1. Add to `VK_FN_LIST`: `	X(vkQueueWaitIdle) \`
2. Write wrapper (near `m_app_destroy`, similar App-level op):
   ```c
   static fb_Value m_wait_idle(fb_State *S, fb_Value *a, int n) {
   	(void)n;
   	vkapp_t *app = fb_object_data(a[0]);
   	VkResult r = app->fn.vkQueueWaitIdle(app->queue);
   	if (r != VK_SUCCESS) return fb_error(S, "vk: vkQueueWaitIdle failed (%d)", (int)r);
   	return fb_bool(S, 1);
   }
   ```
3. Register in `fbuild_open()`, near other `APP_CLS` methods:
   ```c
   fb_method(S, APP_CLS, "wait_idle", m_wait_idle,
             &(fb_Reg){ "wait_idle", m_wait_idle, 0, 0, FB_RET_BOOL, 0,
                        "block until the graphics queue is idle", NULL, 0, NULL, NULL, NULL });
   ```
   (No `param_classes` needed — no args. No `fb_class_field` needed — no new field.)
4. Coverage block: find `* [ ] vkQueueWaitIdle` in Commands section, flip to
   `* [x]`, bump `Commands: 64 / 623` → `Commands: 65 / 623` in header.
5. Build: `cc -shared ...` then `./fbuild plugins` (both clean, no warnings).
6. Test:
   ```sh
   cat > /tmp/t.fb <<'EOF'
   app = vk.app("t", 64, 64)
   print(app.wait_idle())
   app.destroy()
   EOF
   FBUILD_PLUGINS=build/fbvk.so ./fbuild --script /tmp/t.fb
   ```
   Expect `true` printed, exit 0.
7. Commit: `git add plugins/fbvk.c` (only this file changed for this
   example), `git commit -m 'feat(fbvk): wrap vkQueueWaitIdle as App.wait_idle'`.