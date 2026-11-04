# funcbuild

Concrete grammar (lexer + parser, precedence ladder, literals, statements) lives
in `SYNTAX.md`. This file covers types, builtins, and semantics.

## types

```
Int  Float  String  Bool  Path  File  Command  Unit
List<a>  Tuple<a,b,..>  (a -> b)
Map  Json                              JSON object / dynamic value
true  false                            Bool literals
nil  ()                                Unit literal (both spellings, same value)
```
A plain-identifier param can be annotated with one of the above, `name: Type`
-- checked at every call site, and stronger than inference inside the body.
Requires parens around the whole param list (`(x: Int, y: Int) -> ...`, or a
`type NAME(...)` ctor, always parenthesized); a bare `x: Int -> ...` is a
parse error. `Type` is a scalar name above, a `type NAME(...)` class, one
level of `List<Type>` / `Tuple<Type, ...>`, or a `|`-separated union of any
of those, `Type|Type|...` (e.g. `x: Int|Float`, `x: Int|String|Point`,
`xs: List<Int|Float>`) -- a value matches the annotation if it matches any
one member; unions don't nest. See SYNTAX.md.

## type checking

Every script is type-checked before it runs; any error stops it from running
(`FILE:LINE:COL: error: ...`, then "N type errors"). `fbuild --check` checks
without running and also prints warnings; `fbuild --dump-types` prints each
top-level definition's type and every diagnostic. The checker is gradual: what it
cannot know (a JSON value, an unannotated lambda parameter) is `?` and fits
anything, so it only reports what is sure to go wrong: wrong argument counts or
types for builtins and annotated functions, fields a value cannot have (String,
File, sized objects, out-of-range tuple indexes), arithmetic on non-numbers,
comparing unrelated types, calling a non-function, unknown functions, types and
copied methods, match patterns that can never match, and unbounded recursion
(a self-call outside every `when`/`else`/match branch). Using an undefined name as
a value is a warning (it is the bare word's string). Builtins are checked against
their signatures (`filter : List<a>, (a -> ?) -> List<a>`), so a lambda given to
one gets typed parameters; plugins give theirs in `fb::builtin::signature`.

## operators

```
=    : name = expr      lazy memoized def (first def wins; eval'd on first use)
~=   : name ~= expr     eager assign at this point; re-definable, shadows `=`
~e   : ~var             force re-eval of var, ignoring memoization
<-   : lval <- expr     store into a mutable map slot (only mutation; see vars)
->   : params -> body
|>   : a |> f          = f(a)
>>   : a >> b          sequential
//   : f // x          morphism
&    : &expr           metadata
...  : {...xs}         splat / interpolation
@    : @id             capture
.f   : record.field
.{e} : record.{expr}   dynamic field / index
{e}  : "..{expr}.."    interpolation (strings + commands)
+ - * / %              (Int|Float) -> (Int|Float) -> Int|Float
                       Float if either operand is Float, else Int.
                       % on a Float pair uses fmod (C's %, not floor-mod).
-e                     (Int|Float) -> Int|Float   unary negation
< > <= >= == !=        a -> a -> Bool             (Int/Float compare cross-type)
and or                 a -> a -> Bool    any type; runtime truthy() decides (not Bool-only)
||                     a -> b -> (a|b)   value-or: left if truthy else right
not                    a -> Bool         any type; runtime truthy() decides
where : e where n = v, ...   local bindings for e (let); sequential (later see earlier)
when  : e when cond          gate: e if cond else nil; cond can be any type (truthy())
else  : e when cond else f   e if cond, else f (only after a when)
```
Guard tail on an expression: `e where <binds> when <cond> else <f>` (where first,
so the `when` and both branches can use the bindings). `where` names things;
`when` gates the whole thing; `else` is the value when the gate is closed.
if/else is `when ... else`, chained to the right:
```
x    = a when cond else b
kind = 'neg' when n < 0 else 'zero' when n == 0 else 'pos'
run(objs) when stale(exe, ...objs) else print('up to date')
```
`else` branches on the condition, not on truthiness: `0 when true else 7` is 0.
Do not write `(a when cond) || b` for if/else: `||` takes b whenever a is falsy
(0, "", false, nil), so `(0 when true) || 7` is 7. Keep `||` for "first truthy
value" (`n = args.1 || 1`).
In a tuple, `(a, b when c)` drops b when c fails; `(a, b when c else d)` keeps d.
`nil`/`()` is the only Unit value: `x == nil` is the reliable way to test whether
a guard fired.

## match

`match { pattern -> body, ... }` is a function (like a lambda): applied to a
value, the first arm whose pattern takes it runs, with the pattern's names bound;
no arm taking it gives nil. Arms are separated by commas or newlines.
```
flags = os |> match {
  'linux'  -> List<'-pthread'>,
  'darwin' -> List<>,
  _        -> fail('unsupported os', os),
}
describe = match {
  (n: Int) when n < 0 -> 'negative',
  n: Int              -> "int {n}",
  s: String           -> 'string',
  (a, b)              -> 'pair',
  _                   -> 'other',
}
fib = match { 0 -> 0, 1 -> 1, n -> fib(n - 1) + fib(n - 2) }
srcs |> match { f when suffix(f) == '.c' -> cc(f), f -> cxx(f) }
```
Patterns: `_` (anything), a literal (number, -number, quoted string, path, bool,
nil), a name (binds the value; a bare word in a pattern always binds, so quote
literal strings), `name: Type` (only values of that type: Int Float Number String
Bool Path/File Command Unit Map Json List<T> Tuple<...> or a class), a tuple of
patterns `(p, q)` (a sequence of exactly that length), and any of these followed
by `when cond`. A pipe over a list maps the match over its elements, as with any
function. `() -> body` is an explicit zero-arity lambda (distinguished from
the `()` literal by the `->` immediately after); `@ -> body` also works as an
n-ary-ignoring lambda if arity truly doesn't matter.

## construct

```
File<*[path]>                   inherit, autofill name/stem/suffix/parent
File<*f, name=expr>             override field
Tuple<a, ...xs, b>              build a tuple from values + splats
List<a, ...xs, b>               build a real List from values + splats
Map<(k,v), (k2,v2), ...>        build a map from key/value pairs
{ "k": v, k2: v2 }              map literal (sugar); keys: string or bare word
{ x, y }                        shorthand entries: `x` = `"x": x` (value from a
                                 same-named local)
{ ...m, "k": v }                spread m's key/values in (m: Map or object);
                                 a later entry overrides an earlier spread one
(...xs)                         splat materialized as a tuple
seq |> e -> e                   any sequence -> List (promotion)
```
Map is an intrinsic type (was `map()`, now removed). `{}` is the empty map.
A bare-word key is always its own literal text, even if a same-named variable
is in scope (no accidental computed-key shadowing) -- parens don't change this,
`(name)` parses down to the same identifier. For an actually computed key, use
a string interpolation: `"{expr}": v`. Spreading a non-Map/object value
(`{ ...5 }`) is a runtime error, not a silent no-op.

Composing objects: field "inheritance" is just spread + a normal forwarding
call, no dedicated syntax needed --
```
type Point(x, y)         = { x, y }
type Point3D(x, y, z)    = { ...Point(x, y), z }
type ColorPoint(x, y, c) = { ...Point(x, y), color: c }
```
stays fully positional at the call site (`Point3D(1, 2, 3)`); fields compose
this way but methods don't come along for free -- `Point3D` starts with its
own separate, empty method table. Copy methods in explicitly, eagerly (not a
live parent chain -- see "method NAME.MNAME = expr" above for the two copy
forms):
```
method Point3D.repr = Point.repr    # one method
method Point3D.*    = Point.*       # every method Point has right now
```

## custom objects

```
type Point(x, y) = { "x": x, "y": y }        # NAME becomes a constructor
method Point.norm = self -> to_int(self.x)   # method; self = the receiver
method Point.add  = self, o -> Point(self.x + o.x, self.y + o.y)

p = Point(3, 4)
p.x            # field access -> 3
p.norm()       # method call (self = p)
p.add(Point(1,1))                            # -> Point{x:4, y:5}
```
An object is a map tagged with a class name; methods live on the class. Plugins
can define native object types with C methods and opaque data (see PLUGINS.md).

## io / process

```
config  : Map -> ()                   apply build settings (see below)
env     : String -> String
shell   : (Command | Tuple | List) -> (String | Tuple | List)
                                      run silently, capture stdout; nests result
                                      to match input (one cmd -> one String)
which   : String -> Path
pkgconfig : String -> Map          query pkg-config for a lib; returns
                                    { found:Bool, cflags:List<String>,
                                      libs:List<String>, version:String }
                                    (absent -> found:false, empty lists)
run     : (Command | Tuple<Command> | Tuple<String, Command>)... -> Unit
                                      a (String, Command) pair stands in for
                                      that target in the build display: one
                                      command prints as the string instead of
                                      its argv (it still runs the real one), a
                                      sequence/list gets it as a group label.
                                      run(("compile", objs), ("link", exe))
fail    : any... -> never             stop the script: "FILE:LINE:COL: error: <values joined by spaces>"
assert  : any -> String? -> Unit      fail with the message (or "assertion failed") unless cond is truthy
print   : any... -> Unit              repr; deferred until run finishes
debug   : any... -> Unit              typed repr; deferred until run finishes
```
external deps (find a system lib), memoized via `=` so pkg-config runs once:
```
gtk    = pkgconfig('gtk4')
CFLAGS = concat(CFLAGS, gtk.cflags)
LIBS   = gtk.libs
log('ERR', 'need gtk4') when not gtk.found
```
### config keys (call before run())
```
jobs       : Int            cap concurrent child procs per PARALLEL run; 0=unlimited
verbose    : Bool           show every command's output, don't collapse
keep_going : Bool           run all tasks, report failures at the end
silent     : Bool           no live UI; drop successful output, still show failures
color      : Bool           force live UI on (true) / plain output off (false); unset=auto
spinner_ms : Int            display refresh period in ms (default 50)
tail_lines : Int            live-output lines shown under a running command (default 5)
spinner    : (Int -> String) custom spinner glyph; gets a per-command frame counter
```
single key: `config(map(('jobs', 4)))`; many: `config(map((('jobs',4),('verbose',true))))`.
spinner lambda must be pure/cheap (runs every refresh per running command), e.g.
`config(map(('spinner', n -> chars('|/-\\').{n % 4})))`.

## time / bench

```
clock : () -> Int                     monotonic nanoseconds (diff two reads)
time  : () -> Int                     unix epoch seconds
sleep : Int -> ()                     block for N milliseconds
bench : String -> a -> a              time evaluating arg, print elapsed, return it
                                      prints only when --bench passed on CLI
```
note: top-level `X = clock()` is a lazy def (timed at first use); use `bench(label, expr)` to time a run/seq.

## filesystem

```
glob    : String -> List<Path>
exists  : Path -> Bool
mkdir   : Path -> File              mkdir -p
read    : Path -> String
write   : Path -> String -> File
move    : Path -> Path -> File      rename (copy+unlink across filesystems)
copy    : Path -> Path -> File      preserves mode
symlink : Path -> Path -> File      symlink(target, link)
remove  : Path -> Bool              unlink file or rmdir dir
touch   : Path -> File              create / bump mtime
is_dir  : Path -> Bool
is_file : Path -> Bool
stale   : Path -> Path... -> Bool
```

## list

```
zip     : Seq<a> -> Seq<b> -> ... -> List<Tuple<a,b,...>>   n-ary, len = shortest
filter  : List<a> -> (a -> Bool) -> List<a>
contains: List<a> -> a -> Bool
flatten : List<List<a>> -> List<a>
concat  : List<a>... -> List<a>
fold    : List<a> -> b -> (b -> a -> b) -> b
length  : List<a> -> Int
first   : List<a> -> a
last    : List<a> -> a
take    : List<a> -> Int -> List<a>     neg n counts from end
drop    : List<a> -> Int -> List<a>     neg n counts from end
sort    : List<a> -> List<a>
unique  : List<a> -> List<a>
range     : Int -> List<Int>            range(n)=[0..n-1], range(a,b)=[a..b-1]
enumerate : List<a> -> List<Tuple<Int,a>>
slice     : List<a> -> Int -> Int -> List<a>   [s,e), neg from end
index     : List<a> -> b -> (a -> b)? -> Int   first index (optional key fn) or -1
reverse   : List<a> -> List<a>
sum       : (List<Int> | Int...) -> Int
min       : (List<Int> | Int...) -> Int
max       : (List<Int> | Int...) -> Int
count     : List<a> -> a -> Int         occurrences
```

## string

```
cat        : String... -> String
join       : List<String> -> String -> String
split      : String -> String -> List<String>
replace    : String -> String -> String -> String
subst      : String -> String -> String -> String
upper      : String -> String
lower      : String -> String
capitalize : String -> String          first upper, rest lower
title      : String -> String          capitalize each word
trim       : String -> String          strip both ends
lstrip     : String -> String          strip leading whitespace
rstrip     : String -> String          strip trailing whitespace
strip_prefix: String -> String -> String   drop prefix if present
strip_suffix: String -> String -> String   drop suffix if present
pad_left   : String -> Int -> String? -> String   right-justify, fill=' '
pad_right  : String -> Int -> String? -> String   left-justify,  fill=' '
center     : String -> Int -> String? -> String   center,        fill=' '
starts_with: String -> String -> Bool
ends_with  : String -> String -> Bool
find       : String -> String -> Int? -> Int   substring index at/after `from` (default 0), or -1
rfind      : String -> String -> Int    last substring index, or -1
occurrences: String -> String -> Int    count of non-overlapping matches
repeat     : String -> Int -> String
chars      : String -> List<String>     split into characters
words      : String -> List<String>     split on whitespace runs, no empties
partition  : String -> String -> (String, String, String)   (before, sep, after); sep absent -> (s, "", "")
substr     : String -> Int -> Int? -> String   [start,end) byte range, neg from end
is_alpha   : String -> Bool             non-empty and every char a letter
is_digit   : String -> Bool             non-empty and every char a digit
is_alnum   : String -> Bool             non-empty and every char alnum
is_space   : String -> Bool             non-empty and every char whitespace
is_upper   : String -> Bool             has a cased letter, none lowercase
is_lower   : String -> Bool             has a cased letter, none uppercase
ord        : String -> Int              byte value of first char
chr        : Int -> String              one-byte string for code 0-255
to_int     : String|Int|Float -> Int      truncates a Float
to_float   : String|Int|Float -> Float
to_str     : a -> String
```

`find`/`rfind`/`occurrences`/`words`/`partition`/`is_*`/`ord`/`chr` are byte-oriented
(no UTF-8 decoding), same as the rest of this section — one `char` is one byte.

## path

```
path        : String... -> Path
abs         : Path -> Path
parent      : Path -> String
name        : Path -> String
stem        : Path -> String
suffix      : Path -> String
with_suffix : Path -> String -> Path
with_name   : Path -> String -> Path
```

## json

```
json_parse  : String -> Json
json_read   : File   -> Json
json_str    : Json -> String          compact
json_pretty : Json -> String          indented
json_write  : File -> Json -> File
get         : Map -> String -> Json   value by key (() if absent)
has         : Map -> String -> Bool
keys        : Map -> List<String>
values      : Map -> List<Json>
items       : Map -> List<Tuple<String,Json>>    iterate: m |> (k,v -> ..)
m.key   m.{expr}                       field access on a Map
```
object -> Map, array -> List, number -> Int|Float(real), bool -> Bool, null -> ()

## vars

```
args     : List<String>
cwd      : String
script   : String                     path of the .fb file being run (CLI-bound, see below)
plugins  : List<String>               loaded plugin .so paths (--plugin + $FBUILD_PLUGINS + $FBUILD_PLUGIN_PATH)
_GLOBALS : Map                        per-interpreter mutable store (see below)
_SHARED  : Map                        process-global mutable store (all states/plugins)
```
All four are bound with `fb_global_bind`, the same mechanism a plugin uses for
a bare namespace object (e.g. `regex`) — not special-cased in the AST_ID
lookup. `args`/`cwd` are core interpreter state, bound by `run_program` on
every run; `script`/`plugins` are CLI-only, bound by `fbuild`'s `main()` — an
embedder that calls `fb_dofile` directly without going through main.c won't
see `script`/`plugins` unless it binds them itself.

### mutable stores

`_GLOBALS` and `_SHARED` are the only mutable state. Both are ordinary maps you
can write into with `<-` (in place, eager) and read with `.field` / `get` — a
place to accumulate across a run when threading a value through returns is
awkward (counters, caches, host/plugin state):
```
_GLOBALS.count <- 0
xs |> n -> (_GLOBALS.count <- _GLOBALS.count + 1)   # side effect per element
total = _GLOBALS.count
_SHARED.cfg <- { "jobs": 4 }                        # visible to every interpreter
```
`_GLOBALS` is saved/restored with each interpreter (multi-state safe); `_SHARED`
is one store for the whole process (cross-state / plugin sharing). Writing into a
base that isn't a map is a type error; new keys are always allowed. Same maps are
reachable from C via `fb_global_*` / `fb_shared_*` (EMBEDDING.md).

## cli args

```
flag        : String... -> Bool         any of the given flag names present in args
opt         : String -> String -> String  value of `name v` / `name=v`, else default
positionals : () -> List<String>        non-flag args (lightweight; see Parser)
```
Schema parser (uses build.h args.h), auto `--help`:
```
opts = Parser<{
  "force": "bool",                                   # type-only: --force, -f auto
  "jobs":  { "type": "int", "long": "--jobs",        # explicit spec
             "default": 1, "help": "parallel jobs" },
  "out":   "str",
}>                          # bare: parses program args; Parser<schema>(list) for custom
opts.force   opts.jobs   opts.out   opts.rest        # rest = leftover positionals
```
type = bool|int|str. spec keys: type, long, short, default, help. short auto = `-<first
letter>` if unique. `-h`/`--help` prints generated help (from `help` fields) and exits.
