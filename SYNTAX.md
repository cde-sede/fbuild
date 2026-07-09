# funcbuild syntax

Concrete syntax as accepted by the lexer (`src/build_system.c` `lex`) and the
recursive-descent parser. For what the builtins/values *mean*, see `SKILLS.md`.

## Lexical structure

- **Whitespace / newlines**: insignificant (statements can't merge because no rule
  juxtaposes two expressions). Indentation has no meaning.
- **Comment**: `#` to end of line. Not recognized inside strings/commands.
- **Identifier**: `[A-Za-z_][A-Za-z0-9_]*`.
- **Integer**: `[0-9]+`.  **Float**: `[0-9]+.[0-9]+`.
- **Boolean**: `true` `false`.
- **Keywords**: `when` `where` `and` `or` `not`. (`true`/`false` too.) Everything
  else that looks like an identifier is an identifier.
- **String**: `'...'` or `"..."`. Backslash escapes `\<char>`. A string may
  **interpolate** with `{ expr }`: `"out/{name}.o"`. A `#` inside a string is text.
- **Command**: backtick `` `...` `` — shell-ish word list, may interpolate `{ expr }`
  and splat `{...xs}`: `` `{CC} {...CFLAGS} -c {.s} -o {.o}` ``. Produces a Command
  value (argv), not a `/bin/sh` string.
- **Path**: `[ ... ]` — a bare path literal, e.g. `[src/*.c]`, `[build]`,
  `[/usr/bin]`. No nested `[`/`]`.
- **Operators / punctuation**:
  `=  ~=  ~  ->  |>  //  ...  >>  <-  < > <= >= == !=  . , :  * & @ + - / %  ( ) { } [ ]`.
  (`<-` is lexed before `<`, so `a<-b` is a store, not `a < -b`; space the compare if meant.)

Bare-word note: an identifier that is **not** bound (no def/param) evaluates to its
literal text (a string). So map keys and command words can be written unquoted.

## Statements

A program is a sequence of statements; each statement is one expression.

```
NAME = expr          # lazy, memoized top-level definition (first def wins)
NAME ~= expr         # eager, re-definable assignment (runs at this point)
LVALUE <- expr       # store into a mutable slot (see "Mutable stores" below)
expr                 # evaluated eagerly, top to bottom (side effects: run, print, ...)

type NAME(params) = expr        # define a custom object type; NAME becomes a
                                # constructor: NAME(args) builds an object whose
                                # fields are `expr` (usually a map), tagged NAME.
method NAME.MNAME = expr        # attach a method to type NAME; expr is a lambda
                                # whose first parameter is the receiver (self).
```

`type` and `method` are contextual keywords: they only start these forms when
followed by the exact shape above, so they stay usable as ordinary identifiers.
Call a method with `obj.MNAME(args)` — `obj` is passed as the first (self)
argument. `obj.field` reads a field; `obj.MNAME` (no call) yields the method
unbound.

## Expressions (loosest → tightest precedence)

1. **assignment / store** `NAME = e`, `NAME ~= e`, `LVALUE <- e`
2. **tuple** `a, b, c` (comma) → a Tuple
3. **lambda** `params -> body` (`x -> ...`, `a, b -> ...`, `@rest -> ...`, `.f -> ...`)
4. **sequence** `a >> b` (ordered; build dependency)
5. **pipe** `a |> f` (= `f(a)`; maps element-wise over a list/tuple)
6. **guard** `e where b = v, ... when cond` (also `e when cond`)
7. **logical** `and` `or` `||` (and `not e`)
8. **comparison** `< > <= >= == !=`
9. **additive** `+ -`
10. **multiplicative** `* / %`
11. **postfix**: call `f(a, b)`, field `.name` / `.0`, dynamic field `.{e}`,
    constructor `T<...>`, morphism `path // path`
12. **atom** (below)

`and`/`or`/`||` short-circuit; `||` is value-or (left if truthy, else right).
`when` evaluates the condition first and skips the body when false.

## Atoms

```
identifier          NAME
number              42   3.14
boolean             true   false
string              'x'   "out/{e}.o"
command             `cc -c {.s}`
path                [src/*.c]
( e )               grouping
capture             @        @name        (binds the call's argument / rest)
projection          .name    .0           (field/index of the implicit arg)
metadata            &e
force               ~e                     (re-evaluate e, ignore memoization)
splat               ...e                   (spread into a tuple/args/command)
tuple constructor   Tuple<a, ...xs, b>
map constructor     Map<(k,v), (k2,v2)>
map literal         { "k": v, k2: e }      (empty: {} ; keys: string or bare word)
parser              Parser<schema>         (schema is a map; see SKILLS.md)
```

## Constructors

```
File<*base>                     inherit from a path/File, autofill fields
File<*f, name=e, suffix=e>      override fields (name/stem/suffix/parent)
Tuple<a, ...xs, b>              tuple from values + splats
Map<(k, v), (k2, v2)>           map from key/value pairs
{ "k": v, k2: e }              map literal (sugar for Map)
Parser<{ "flag": "bool", ... }>  CLI arg parser (see SKILLS.md)
```

## Lambdas, captures, projections

```
f = x -> x + 1
g = a, b -> cat(a, b)
create = (f, artifact, @deps -> f(.artifact, ...deps))   # @deps = rest of args
entry  = o, s -> { "file": .s, "obj": .o }               # .s/.o = projections
```

## Guards

```
compile = o, s -> `{CC} -c {...s} -o {.o}` when stale(.o, ...s) or FORCE
val     = e where tmp = heavy(), n = length(tmp) when n > 0
run(x) when contains(TARGETS, 'all') or length(TARGETS) == 0
```

## Mutable stores (`<-`)

Everything else is immutable (`=` is a lazy def, `.f` reads). `<-` is the one
mutation form: it writes into a slot of a map, in place, eagerly. `LVALUE` is a
field/index chain whose base evaluates to a map:

```
_GLOBALS.count <- 0                    # named slot
_GLOBALS.count <- _GLOBALS.count + 1   # reads are plain field access
_GLOBALS.{i}   <- item                 # dynamic slot (.{expr})
_SHARED.theme  <- [dark]               # process-global store
n = _GLOBALS.count                     # read
```

Two built-in stores back this: `_GLOBALS` (per-interpreter, the usual choice)
and `_SHARED` (one store shared across every interpreter and plugin in the
process). Both are ordinary maps — `keys`, `has`, `get`, `.field` all work — that
happen to be writable and persist across the whole run. Writing a base that
isn't a map is a type error; new keys on `_GLOBALS`/`_SHARED` are always allowed.
The value stored is what you wrote (no copy in this build). See SKILLS.md for
semantics and EMBEDDING.md for the `fb_global_*` / `fb_shared_*` C API.

## Interpolation

```
"build/{stem(f)}.o"            # in strings
`{CC} {...CFLAGS} -o {.a}`     # in commands; {...xs} splats a list into argv
```
