# Writing an fbuild plugin

A plugin is a shared object (`.so`) that adds builtins to the language. It uses
the same registration ABI as an embedding host (`src/fbuild.h`); the difference
is only how it gets loaded — `fbuild` `dlopen`s it at startup.

## Anatomy

A plugin must export two symbols:

```c
#include "fbuild.h"

int fbuild_abi(void)  { return FBUILD_ABI; }   /* version guard  */
int fbuild_open(fb_State *S) {                  /* register here  */
    fb_register(S, &(fb_Reg){ "shout", l_shout, 1, 1, FB_RET_STR, 0 });
    return 0;                                    /* 0 = ok         */
}
```

`fbuild` refuses to load a plugin whose `fbuild_abi()` does not match the
`FBUILD_ABI` it was built with, so a stale `.so` fails cleanly instead of
crashing.

## A complete plugin

```c
/* shout.c */
#include <string.h>
#include <ctype.h>
#include "fbuild.h"

static fb_Value l_shout(fb_State *S, fb_Value *a, int n) {
    (void)n;
    const char *s = fb_tostr(S, a[0]);
    char buf[256]; size_t i = 0;
    for (; s[i] && i < sizeof buf - 2; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    buf[i++] = '!'; buf[i] = '\0';
    return fb_str(S, buf);
}

int fbuild_abi(void) { return FBUILD_ABI; }
int fbuild_open(fb_State *S) {
    fb_register(S, &(fb_Reg){ "shout", l_shout, 1, 1, FB_RET_STR, 0 });
    return 0;
}
```

Build it (position-independent, shared; the `fb_*` symbols resolve from the
`fbuild` executable at load time, which is why `fbuild` is linked `-rdynamic`):

```sh
cc -fPIC -shared -Isrc -Icds-lib shout.c -o shout.so
```

## Loading

Three ways, all take effect **before** type-checking so the checker knows the
new names:

```sh
fbuild --plugin ./shout.so             # CLI flag (repeatable)
FBUILD_PLUGINS=./shout.so fbuild       # colon-separated list of .so paths
FBUILD_PLUGIN_PATH=./build fbuild          # colon-separated list of DIRS; every
                                        # *.so directly inside each dir loads,
                                        # sorted by filename, non-recursive
```

`--plugin` arguments are consumed by fbuild and not forwarded to the script.
Load order: `--plugin` flags first, then `FBUILD_PLUGINS`, then
`FBUILD_PLUGIN_PATH` directories. A directory in `FBUILD_PLUGIN_PATH` that can't be
opened is an error, not a silent skip.

`fbuild`'s own flags (`--plugin`, `--script`, `--list-plugins`) are only
recognized **before** a `:::` separator, if one is present — the same
separator that splits fbuild's own args from the args forwarded to the script
(see `args` in SKILLS.md). Anything at or after `:::` is passed through
untouched, so `fbuild ::: --list-plugins` forwards `--list-plugins` to the
script instead of acting on it.

```sh
fbuild --list-plugins    # print resolved plugin .so paths (--plugin flags,
                          # then FBUILD_PLUGINS, then FBUILD_PLUGIN_PATH), one per
                          # line, and exit without running a script
```

Then use it in a `.fb`:

```
print(shout('hello'))     # HELLO!
```

## What a builtin can do

Everything the embedding value API offers (see EMBEDDING.md): construct and
inspect ints/strings/bools/files/lists/maps, iterate map keys (`fb_key`), build
runnable commands (`fb_cmd` / `fb_cmd_push`) to hand back to `run()`, call
language functions (`fb_apply`), and fail with `fb_error`.

For state that must persist between builtin calls or be shared with the host and
other plugins, use the mutable stores rather than C `static` variables:
`fb_shared_get/set(S, key, v)` (process-global, script `_SHARED`) or
`fb_global_get/set` (per-interpreter, script `_GLOBALS`). They survive across
runs and the script sees the same maps — the supported replacement for a stashed
`static fb_Value` slot.

Set `FB_LAZY` in the flags to receive unevaluated thunk arguments and control
evaluation yourself with `fb_force` — that is how you add new *control forms*, not
just new functions. Example: a command factory plus a lazy conditional:

```c
static fb_Value l_cc(fb_State *S, fb_Value *a, int n) {      /* cc(src,obj) */
    (void)n;
    fb_Value c = fb_cmd(S);
    fb_cmd_push(S, c, fb_str(S, "cc"));
    fb_cmd_push(S, c, fb_str(S, "-c"));
    fb_cmd_push(S, c, a[0]);
    fb_cmd_push(S, c, fb_str(S, "-o"));
    fb_cmd_push(S, c, a[1]);
    return c;                                                 /* run(cc(...)) works */
}
static fb_Value l_pick(fb_State *S, fb_Value *a, int n) {    /* pick(cond,x,y) */
    (void)n;
    return fb_toint(fb_force(S, a[0])) ? fb_force(S, a[1]) : fb_force(S, a[2]);
}

int fbuild_open(fb_State *S) {
    fb_register(S, &(fb_Reg){ "cc",   l_cc,   2, 2, FB_RET_ANY, 0 });
    fb_register(S, &(fb_Reg){ "pick", l_pick, 3, 3, FB_RET_ANY, FB_LAZY });
    return 0;
}
```

## Custom object types

A plugin can define a new object type with a constructor and methods callable as
`obj.method(args)`. See EMBEDDING.md for the full example; the shape is:

```c
fb_Class CLS = fb_class(S, "Socket");
fb_method(S, CLS, "send",  l_send);       /* l_send(self, data) */
fb_method(S, CLS, "close", l_close);
/* a constructor builtin returns fb_object(S, CLS, native_ptr) */
```

```
s = socket_open("host", 8080)   # constructor builtin
s.send("hi")                     # method: self = s
s.close()
```

`fb_object_data` retrieves the native pointer inside a method. Objects also have
a field map (`fb_set` / `fb_field` / `fb_key`), interchangeable with language
`type` objects.

## Bare-name namespace objects

The `socket_open(...)` example above needs a constructor call before `.send`
works. For a *stateless* object — a bag of related functions, not a resource
tied to a constructor argument — skip the constructor and bind one shared
instance straight to an identifier with `fb_global_bind`, so the script can
call methods on it immediately with no setup:

```c
fb_Class RE = fb_class(S, "Regex");
fb_method(S, RE, "findall", m_findall);
/* ...other methods... */

/* type checker must recognize the bare name; the fn only runs if someone
 * calls regex(...) directly instead of a method, so make it fb_error.
 * FB_GLOBAL tells the checker this identifier is a bound value, not a
 * callable, so a bare reference types from `ret` instead of RET_VAR. */
fb_register(S, &(fb_Reg){ "regex", bi_regex_noncallable, 0, -1, FB_RET_ANY, FB_GLOBAL });
fb_global_bind(S, "regex", fb_object(S, RE, NULL));
```

```
print(regex.findall("a1 b22 c333", "[0-9]+"))   # [1, 22, 333] -- no construction
```

A script-level `regex = ...` def still shadows the bind (defs resolve first).

For a plain data value (not an object with methods) where the precise type
matters — e.g. a bound `List<String>` — declare `ret` accordingly
(`FB_RET_LIST_STR` for `List<String>`, or one of the other `FB_RET_*` scalars);
`fbuild`'s own `args`/`cwd`/`script`/`plugins` globals use this (see `src/main.c`,
`src/run_builtins.c`).
See `plugins/fbregex.c` for the full worked example.

## ABI stability

`FBUILD_ABI` (in `fbuild.h`) is bumped on any incompatible change to the ABI.
Rebuild plugins against the matching `fbuild.h` when it changes.
