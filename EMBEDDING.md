# Embedding funcbuild

The interpreter ships as a library, `libfbuild.a` / `libfbuild.so`, with a small
C API in `src/fbuild.h`. A host program creates an interpreter, optionally
registers its own builtins, runs a script, and reads results back. The `fbuild`
CLI is itself just a thin driver over this API (`src/main.c`).

Single implicit state: one interpreter per process.

## Minimal host

```c
#include <stdio.h>
#include "fbuild.h"

int main(void) {
    fb_State *S = fb_new();           /* inits the arena + core builtins */
    fb_set_args(S, 0, NULL);          /* the script's `args` list        */

    if (fb_dostring(S, "answer = 6 * 7\n", "demo")) {
        fprintf(stderr, "%s\n", fb_error_msg(S));
        return 1;
    }
    printf("answer = %ld\n", fb_toint(fb_global(S, "answer")));

    fb_close(S);                      /* tears the arena down            */
    return 0;
}
```

Build (static):

```sh
cc -Isrc -Icgen host.c libfbuild.a -o host
```

Add `-rdynamic -ldl` if the host also loads plugins (so a plugin can resolve the
`fb_*` symbols from your executable). See PLUGINS.md.

## Lifecycle

| Call | Purpose |
|------|---------|
| `fb_State *fb_new(void)` | Create the interpreter (inits arena, registers core builtins). |
| `void fb_close(fb_State*)` | Destroy it (frees the arena; all `fb_Value`s become invalid). |
| `void fb_set_args(fb_State*, int argc, char **argv)` | Set the script-visible `args` (argv[1..]; a `:::` splits fbuild args from script args). |
| `int fb_dostring(fb_State*, const char *src, const char *name)` | Type-check and run source. 0 = ok. |
| `int fb_dofile(fb_State*, const char *path)` | Same, reading a file. |
| `const char *fb_error_msg(fb_State*)` | Message after a non-zero return. |
| `fb_Value fb_global(fb_State*, const char *name)` | A top-level def's value after a run (nil if absent). |

## Registering a host builtin

A builtin is a C function that receives evaluated arguments and returns a value:

```c
static fb_Value l_greet(fb_State *S, fb_Value *a, int n) {
    (void)n;
    char buf[128];
    snprintf(buf, sizeof buf, "hello, %s!", fb_tostr(S, a[0]));
    return fb_str(S, buf);
}

fb_register(S, &(fb_Reg){ "greet", l_greet, /*min*/1, /*max*/1, FB_RET_STR, 0 });
```

Register **before** `fb_dostring`/`fb_dofile` so the type checker sees the name.
`max < 0` means variadic. `ret` is the declared return type used by the checker.

## Values

`fb_Value` is an opaque handle into the interpreter arena. Never free it; it
lives until `fb_close`.

Construct: `fb_nil fb_int fb_bool fb_str fb_file fb_list fb_push fb_map fb_set
fb_cmd fb_cmd_push`.

Inspect: `fb_kind fb_toint fb_tostr fb_len fb_at fb_field fb_key`.

```c
fb_Value m = fb_map(S);
fb_set(S, m, "n", fb_int(S, 3));
fb_set(S, m, "tags", fb_list(S));           /* nested */
for (int i = 0; i < fb_len(m); i++)
    printf("%s\n", fb_key(m, i));            /* iterate map keys */

fb_Value cmd = fb_cmd(S);                    /* a runnable command */
fb_cmd_push(S, cmd, fb_str(S, "cc"));
fb_cmd_push(S, cmd, fb_str(S, "-c"));
```

Call a language function (lambda or builtin) from C:

```c
fb_Value args[1] = { fb_int(S, 41) };
fb_Value r = fb_apply(S, fb_global(S, "succ"), args, 1);
```

## Lazy builtins (control forms)

By default arguments are evaluated before your builtin runs. Set `FB_LAZY` to
receive them **unevaluated** as thunks and decide what to evaluate, and when,
with `fb_force` — this is how you write things like a custom `bench`, a lazy
default, or a short-circuiting conditional:

```c
static fb_Value l_pick(fb_State *S, fb_Value *a, int n) {   /* pick(cond,x,y) */
    (void)n;
    return fb_toint(fb_force(S, a[0])) ? fb_force(S, a[1])   /* only one branch */
                                       : fb_force(S, a[2]);  /* is evaluated    */
}
fb_register(S, &(fb_Reg){ "pick", l_pick, 3, 3, FB_RET_ANY, FB_LAZY });
```

## Errors

`fb_error(S, fmt, ...)` aborts the current builtin. Under `fb_dostring` it
unwinds to a protected boundary: the call returns non-zero with the message in
`fb_error_msg`, and the host survives. (Limit: an error raised deep inside a
`run()` build coroutine cannot unwind safely yet.)

```c
if (bad) return fb_error(S, "greet: expected a string, got kind %d", fb_kind(a[0]));
```
