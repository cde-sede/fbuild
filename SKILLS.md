# funcbuild

Concrete grammar (lexer + parser, precedence ladder, literals, statements) lives
in `SYNTAX.md`. This file covers types, builtins, and semantics.

## types

```
Int  Float  String  Bool  Path  File  Command  Unit
List<a>  Tuple<a,b,..>  (a -> b)
Map  Json                              JSON object / dynamic value
true  false                            Bool literals
```

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
and or                 Bool -> Bool -> Bool
||                     a -> b -> (a|b)   value-or: left if truthy else right
not                    Bool -> Bool
where : e where n = v, ...   local bindings for e (let); sequential (later see earlier)
when  : e when cond          gate: e if cond else () (unit, falsy)
```
Guard tail on an expression: `e where <binds> when <cond>` (where first, so the
`when` can use the bindings). `where` names things; `when` gates the whole thing.
Common if/else idiom (since `when` yields `()` when false):
`x = (a when cond) || b`  — a if cond else b.

## construct

```
File<*[path]>                   inherit, autofill name/stem/suffix/parent
File<*f, name=expr>             override field
Tuple<a, ...xs, b>              build a tuple from values + splats
Map<(k,v), (k2,v2), ...>        build a map from key/value pairs
{ "k": v, k2: v2 }              map literal (sugar); keys: string or bare word
(...xs)                         splat materialized as a tuple
seq |> e -> e                   any sequence -> List (promotion)
```
Map is an intrinsic type (was `map()`, now removed). `{}` is the empty map.
Bare-word keys are literal unless the word is a bound variable (then computed key).

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
run     : (Command | Tuple<Command>)... -> Unit
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
find       : String -> String -> Int    substring index or -1
repeat     : String -> Int -> String
chars      : String -> List<String>     split into characters
to_int     : String|Int|Float -> Int      truncates a Float
to_float   : String|Int|Float -> Float
to_str     : a -> String
```

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
object -> Map, array -> List, number -> Int|String(real), bool -> Bool, null -> ()

## vars

```
args     : List<String>
cwd      : String
_GLOBALS : Map                        per-interpreter mutable store (see below)
_SHARED  : Map                        process-global mutable store (all states/plugins)
```

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
