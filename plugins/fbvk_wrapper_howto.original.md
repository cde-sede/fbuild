# Adding a new wrapped Vulkan function to plugins/fbvk.c

Context for an agent task: "wrap Vulkan function `vkXxx` as a new method/field
on the fbvk plugin". Self-contained — you shouldn't need to read the whole
4000-line `plugins/fbvk.c` first, just this. Read `src/fbuild.h` too before
writing code; it's the authoritative ABI doc, short, and this file assumes it.

## The shape of the plugin

`plugins/fbvk.c` dlopens `libvulkan.so.1`/`libglfw.so.3` at runtime (no link-time
dep — matches every plugin in this repo: "links against nothing but the fbuild
host"). It exposes a **curated** API (`vk.app()`, `App.draw()`, ...), not a 1:1
binding — most of the ~623 Vulkan commands are deliberately unwrapped (see the
coverage comment block at the end of the file). Adding a wrapper means:

1. dlsym the raw `vkXxx` command (if not already fetched).
2. Write a small C function translating funcbuild args ↔ Vulkan call.
3. Register it as a method (`fb_method`) or free function (`fb_register`).
4. Type its params/return so the checker isn't left showing `?`.
5. Mark it `[x]` in the coverage block at the end of the file.
6. Build + test.

## 1. Fetching the raw command

Every Vulkan command used anywhere in the file is one entry in the `VK_FN_LIST`
X-macro near the top:

```c
#define VK_FN_LIST(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	...
	X(vkUnmapMemory)
```

If the command you need isn't in that list, add one line: `X(vkYourCommand) \`.
That's it — `load_fns()` (right below the macro) fetches every listed command
via `vkGetInstanceProcAddr(instance, name)` automatically, and it becomes
callable as `app->fn.vkYourCommand(...)` (every `vkapp_t` embeds a `vk_fns_t fn`).

Read the big comment directly above `load_fns` before assuming you need
`vkGetDeviceProcAddr` instead — you don't; this plugin deliberately fetches
device-level commands through the instance-level loader too (see that comment
for why it's safe on the real Vulkan Loader). Two commands
(`vkCreateInstance`, `vkGetInstanceProcAddr` itself) are special-cased outside
the X-macro in `ensure_loader()` — you'll basically never touch those.

## 2. Writing the wrapper function

Look at an existing one close to what you're adding first — `m_buf_destroy`
(zero-arg, self only), `bi_buffer` (a constructor with args + object creation),
`m_draw` (a method with typed native-class args) are the three shapes you'll
reuse. Signature is always:

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

Critical, easy to get wrong: **for a method (`fb_method`), `a[0]` is self and
real arguments start at `a[1]`.** For a free function (`fb_register` directly,
no `fb_method`), there's no self — `a[0]` is the first real argument. Get this
wrong and every arg reads one slot off (see `bi_app`'s doc comment for a
worked note on exactly this).

`fb_object_data(a[0])` gets you back the native struct (`vkapp_t*`/`vkbuf_t*`/
`vkshader_t*`/`vkpipe_t*`) you stashed via `fb_object()` at construction —
that's how state survives between calls.

## 3. Registering it

In `fbuild_open()`, near the other methods of the same class:

```c
fb_method(S, APP_CLS, "your_method", m_your_method,
          &(fb_Reg){ "your_method", m_your_method, MIN, MAX, FB_RET_XXX, 0,
                     "one-line doc for hover", param_types, nparams, ret_class,
                     NULL, NULL });
```

- `MIN`/`MAX` count **real args only, self excluded** (same convention
  `obj.method(args)` uses at the call site).
- `FB_RET_XXX`: plain return kind (`FB_RET_BOOL`, `FB_RET_INT`, ...) — ignored
  if `ret_class` is set (see below).
- `param_types`: `static const fb_Ret arr[] = { FB_RET_INT, ... };` — one
  entry per real arg (self excluded), only covers plain kinds (Int/Float/Str/
  Bool/File/List/Map/Unit/Num). Pass `NULL, 0` if nothing to say.
- `ret_class`: set to `"App"`/`"Buffer"`/`"Shader"`/`"Pipeline"` (a string,
  matching an existing `fb_class(S, "...")` name) if this returns/re-returns
  an instance of one of those classes — e.g. every chainable method
  (`.poll()`, `.draw()`, ...) returns `a[0]` and sets `ret_class = "App"` so
  chaining keeps typing. `NULL` if the return is a plain kind or `FB_RET_ANY`.

**A param that must be a specific native class** (e.g. a `Pipeline` or
`Buffer` argument) can't be expressed in `param_types` (no class concept
there) — `fb_Reg` deliberately doesn't grow a field for this (would force an
ABI bump + recompiling every plugin, see the big comment on
`fb_reg_param_class` in `fbuild.h` for why). Instead, call it separately right
after `fb_method`, keyed by the exact `"ClassName.methodname"` string
`fb_method`'s shadow registration builds:

```c
fb_reg_param_class("App.your_method", 0, "Pipeline");  /* first real arg */
fb_reg_param_class("App.your_method", 1, "Buffer");    /* second real arg */
```

Index is 0-based, self excluded, same counting as `min`/`max`.

**A plain data field** set via `fb_set()` at construction (not a method) needs
a matching `fb_class_field()` call or the checker will wrongly reject reading
it back (`Object has no field 'X'`) on any class that also has methods:

```c
fb_class_field(S, YOUR_CLS, "field_name", FB_RET_STR, NULL);
```

(4th arg is `ret_class` if the field is itself a native-class instance, else
`NULL`.)

## 4. New class vs. new method on an existing class

Existing native classes: `App`, `Buffer`, `Shader`, `Pipeline`, `Device` (plus
the `vk` namespace itself). Add a method to one of these if the thing you're
wrapping operates ON that resource. Only add a whole new `fb_class(S, "Name")`
+ `vkYourStruct_t` native-data struct if you're wrapping something that's a
genuinely new kind of Vulkan handle/resource this plugin doesn't yet model
(e.g. a descriptor set, a texture/image). Follow the `Buffer`/`Shader`
pattern: a small `typedef struct { vkapp_t *app; VkYourHandle h; ... } t;`,
`fb_object(S, cls, ptr)` to wrap it, `fb_object_data()` to unwrap it in every
method, a `.destroy()` method, `fb_class_field()` for whatever plain fields
you expose.

## 5. Update the coverage block

At the very end of `plugins/fbvk.c` is a generated `[x]`/`[ ]` list of every
Vulkan command and type. After adding a wrapper, flip the command's marker
from `[ ]` to `[x]` (and any newly-referenced `VkXxx` type identifiers too —
grep the file for the exact identifier to see if it's already `[x]`). The
counts in the header of that block (`Commands: N / 623`, `Types: N / 2301`)
need bumping too. Don't regenerate the whole block from scratch for a
one-function change — hand-edit the specific lines; the regen recipe in that
block's header comment is for a full refresh (e.g. after a Vulkan header
upgrade), not routine.

## 6. Build + test loop

Compile the plugin directly while iterating (fast, catches errors/warnings):

```sh
cc -shared -fPIC -Isrc -ggdb -Icds-lib -Wall -Wextra -O2 \
   -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 plugins/fbvk.c -o build/fbvk.so
```

Before considering it done, also build via the **real** project recipe, which
adds `-Werror` and will catch things the command above won't (e.g. missing
`fb_Reg` field initializers):

```sh
./fbuild plugins
```

(Never run bare `./fbuild` with no args/`--script` in this repo — that
self-builds the *interpreter itself* from `src/*.c`. Safe, just not what you
want here, and slower.)

Test with a throwaway `.fb` script:

```sh
FBUILD_PLUGINS=build/fbloop.so:build/fbvk.so ./fbuild --script /tmp/test.fb
```

`build/fbloop.so` is needed if your test uses `while`/`range`/etc. — those are
a separate plugin, not core language. Two script-syntax traps that aren't
obvious from the language docs (SYNTAX.md/SKILLS.md), both bitten by writers
of the existing `examples/vk_triangle.fb`:

- List literals are `List<1, 2, 3>`, **not** `[1, 2, 3]` (`[...]` is a Path
  literal in this language).
- A negative number as a `List<...>` element needs parens:
  `List<1, (0 - 2), 3>` — bare `List<1, -2, 3>` is a parse error.

If you're testing something requiring a real GPU device query, `vk.devices()`
works headless (no window). If you're testing an `App`/window-based method,
you need a live `$DISPLAY`/`$WAYLAND_DISPLAY` — check `echo $DISPLAY` first;
if unset, ask before trying to test a windowed path.

## Worked example: wrapping `vkQueueWaitIdle` as `App.wait_idle()`

1. Add to `VK_FN_LIST`: `	X(vkQueueWaitIdle) \`
2. Write the wrapper (near `m_app_destroy`, since it's a similar App-level op):
   ```c
   static fb_Value m_wait_idle(fb_State *S, fb_Value *a, int n) {
   	(void)n;
   	vkapp_t *app = fb_object_data(a[0]);
   	VkResult r = app->fn.vkQueueWaitIdle(app->queue);
   	if (r != VK_SUCCESS) return fb_error(S, "vk: vkQueueWaitIdle failed (%d)", (int)r);
   	return fb_bool(S, 1);
   }
   ```
3. Register it in `fbuild_open()`, near the other `APP_CLS` methods:
   ```c
   fb_method(S, APP_CLS, "wait_idle", m_wait_idle,
             &(fb_Reg){ "wait_idle", m_wait_idle, 0, 0, FB_RET_BOOL, 0,
                        "block until the graphics queue is idle", NULL, 0, NULL, NULL, NULL });
   ```
   (No `param_classes` needed — no args. No `fb_class_field` needed — no new field.)
4. Coverage block: find `* [ ] vkQueueWaitIdle` in the Commands section, flip
   to `* [x]`, bump `Commands: 64 / 623` → `Commands: 65 / 623` in the header.
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
