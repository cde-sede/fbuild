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

Two ways, both take effect **before** type-checking so the checker knows the new
names:

```sh
fbuild --plugin ./shout.so             # CLI flag (repeatable)
FBUILD_PLUGINS=./shout.so fbuild       # colon-separated list
```

`--plugin` arguments are consumed by fbuild and not forwarded to the script.

Then use it in a `.fb`:

```
print(shout('hello'))     # HELLO!
```

## What a builtin can do

Everything the embedding value API offers (see EMBEDDING.md): construct and
inspect ints/strings/bools/files/lists/maps, iterate map keys (`fb_key`), build
runnable commands (`fb_cmd` / `fb_cmd_push`) to hand back to `run()`, call
language functions (`fb_apply`), and fail with `fb_error`.

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

## ABI stability

`FBUILD_ABI` (in `fbuild.h`) is bumped on any incompatible change to the ABI.
Rebuild plugins against the matching `fbuild.h` when it changes.
