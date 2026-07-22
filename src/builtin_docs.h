#ifndef BUILTIN_DOCS_H
#define BUILTIN_DOCS_H

/* Centralized hover documentation for core builtins (run_builtins.c's
 * base[]). One place to edit prose without touching the dense registration
 * array or any C implementation code -- base[] just references these by
 * name (`.doc = FBDOC_zip`).
 *
 * Plugins are separate translation units and don't include this header;
 * each plugin fills its own fb_Reg.doc fields directly at its own
 * registration call sites, using the same convention below (plugins convey
 * doc text through the fb_Reg ABI, no different from core).
 *
 * Convention for each doc string:
 *
 *   <one-sentence summary, no trailing period requirement>
 *
 *   **Params**
 *   - `name` — description
 *   - `name` — description
 *
 *   **Returns** description
 *
 *   **Example**
 *   ```
 *   code
 *   ```
 *
 * For a bound value (not callable, e.g. `args`/`cwd`), use **Type** instead
 * of **Params** / **Returns**. Omit **Params** entirely for a 0-arg builtin.
 * fb_hover_at (walk.c) prepends the computed signature as its own fenced
 * code block ahead of this text and renders the whole thing as markdown --
 * don't repeat the signature here. Keep the summary line self-contained
 * (it's also what completion shows as the item's short detail). */

/* ---- io / process -------------------------------------------------------- */

#define FBDOC_env \
"Reads an environment variable.\n" \
"\n" \
"**Params**\n" \
"- `name` — variable name\n" \
"\n" \
"**Returns** the variable's value, or `()` (falsy) if unset -- `env(x) || default` is the usual idiom.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"home = env('HOME') || '/tmp'\n" \
"```"

#define FBDOC_shell \
"Runs a command (or command literal) silently and captures its stdout, without going through the build scheduler.\n" \
"\n" \
"**Params**\n" \
"- `cmd` — a `Command`, or a `Tuple`/`List` of them (result nests to match)\n" \
"\n" \
"**Returns** the captured stdout as a `String` per command, nested the same shape as the input (one `Command` in, one `String` out).\n" \
"\n" \
"**Example**\n" \
"```\n" \
"rev = shell(`git rev-parse --short HEAD`)\n" \
"```"

#define FBDOC_which \
"Resolves an executable name against `$PATH`, the way a shell would find it.\n" \
"\n" \
"**Params**\n" \
"- `name` — executable name (or path)\n" \
"\n" \
"**Returns** the resolved `Path`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"cc = which('cc')\n" \
"```"

#define FBDOC_pkgconfig \
"Queries `pkg-config` for a library's build flags. Memoize with `=` so pkg-config only runs once per library.\n" \
"\n" \
"**Params**\n" \
"- `name` — pkg-config package name\n" \
"\n" \
"**Returns** a `Map`: `{ found: Bool, cflags: List<String>, libs: List<String>, version: String }` -- `found: false` and empty lists if the package isn't known.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"gtk = pkgconfig('gtk4')\n" \
"CFLAGS = concat(CFLAGS, gtk.cflags) when gtk.found\n" \
"```"

#define FBDOC_run \
"Schedules one or more build commands to run (deferred until the script finishes evaluating; the scheduler parallelizes independent commands).\n" \
"\n" \
"**Params**\n" \
"- `cmd...` — one or more `Command`s, or `Tuple<Command>`s (splat a list of commands with `...`)\n" \
"\n" \
"**Returns** `Bool` -- whether anything actually ran (false if every target was already up to date).\n" \
"\n" \
"**Example**\n" \
"```\n" \
"run(create_cmd(compile, obj, src))\n" \
"```"

#define FBDOC_print \
"Prints a plain `repr` of each argument, deferred until the run finishes (so output doesn't interleave with the live build UI).\n" \
"\n" \
"**Params**\n" \
"- `values...` — any values\n" \
"\n" \
"**Returns** `()`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"print('objects:', objs)\n" \
"```"

#define FBDOC_debug \
"Like `print`, but shows each value's type alongside its `repr` -- useful when a value's shape is unclear.\n" \
"\n" \
"**Params**\n" \
"- `values...` — any values\n" \
"\n" \
"**Returns** `()`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"debug(opts)   # e.g. Map{force: Bool(false), jobs: Int(4)}\n" \
"```"

#define FBDOC_config \
"Applies build-runner settings before `run()`. Unknown keys are ignored, so scripts stay forward-compatible with future keys.\n" \
"\n" \
"**Params**\n" \
"- `settings` — a `Map`, e.g. `{ jobs: 4, verbose: true }`. Keys: `jobs: Int`, `verbose: Bool`, `keep_going: Bool`, `silent: Bool`, `color: Bool`, `spinner_ms: Int`, `tail_lines: Int`, `spinner: (Int -> String)`\n" \
"\n" \
"**Returns** `()`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"config({ 'jobs': 4, 'verbose': flag('-v') })\n" \
"```"

/* ---- time ----------------------------------------------------------------- */

#define FBDOC_clock \
"Reads a monotonic clock, in nanoseconds -- diff two reads to time something (unaffected by wall-clock adjustments).\n" \
"\n" \
"**Returns** `Int` nanoseconds.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"t0 = clock()\n" \
"```"

#define FBDOC_time \
"Reads the wall-clock time as a Unix epoch timestamp.\n" \
"\n" \
"**Returns** `Int` seconds since epoch.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"stamp = time()\n" \
"```"

#define FBDOC_sleep \
"Blocks the calling thread for a fixed duration.\n" \
"\n" \
"**Params**\n" \
"- `ms` — milliseconds to sleep\n" \
"\n" \
"**Returns** `()`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"sleep(100)\n" \
"```"

#define FBDOC_bench \
"Times evaluating an expression and prints the elapsed time (only when `--bench` was passed on the CLI) -- use this instead of diffing two `clock()` reads around a lazy `=` def, since a plain def is only timed at first use.\n" \
"\n" \
"**Params**\n" \
"- `label` — String shown alongside the timing\n" \
"- `expr` — the (lazy) expression to time and return\n" \
"\n" \
"**Returns** the value of `expr`, unchanged.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"objs = bench('compile', units |> u -> create_cmd(compile, ...u))\n" \
"```"

/* ---- filesystem ------------------------------------------------------------ */

#define FBDOC_glob \
"Expands a filesystem glob pattern (`*`, `**`, `?`, `[...]`).\n" \
"\n" \
"**Params**\n" \
"- `pattern` — glob pattern String\n" \
"\n" \
"**Returns** a `List<Path>` of matches (empty if none).\n" \
"\n" \
"**Example**\n" \
"```\n" \
"srcs = glob('src/*.c')\n" \
"```"

#define FBDOC_exists \
"Checks whether a path exists (file, directory, or anything else the filesystem has there).\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"has_config = exists(path('config.json'))\n" \
"```"

#define FBDOC_mkdir \
"Creates a directory, including any missing parent directories (like `mkdir -p`).\n" \
"\n" \
"**Params**\n" \
"- `path` — directory `Path` to create\n" \
"\n" \
"**Returns** the created `File`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"mkdir('build/obj')\n" \
"```"

#define FBDOC_read \
"Reads a file's entire contents as text.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** the file's contents as a `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"version = trim(read('VERSION'))\n" \
"```"

#define FBDOC_write \
"Writes text to a file, creating or overwriting it.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path` to write\n" \
"- `content` — text to write\n" \
"\n" \
"**Returns** the written `File`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"write('build/VERSION', '1.2.3')\n" \
"```"

#define FBDOC_copy \
"Copies a file's raw bytes, preserving its mode (permission bits).\n" \
"\n" \
"**Params**\n" \
"- `src` — source `Path`\n" \
"- `dst` — destination `Path`\n" \
"\n" \
"**Returns** the destination `File`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"copy('build/app', 'dist/app')\n" \
"```"

#define FBDOC_stale \
"Checks whether a build target is out of date: missing, or older than any of the given dependencies. The usual guard on a `run()` action.\n" \
"\n" \
"**Params**\n" \
"- `target` — the output `File` (a non-`File` value, e.g. a missing target, counts as stale)\n" \
"- `deps...` — zero or more dependency `File`s\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"run(cmd) when stale(obj, src, ...HDRS)\n" \
"```"

/* ---- list ------------------------------------------------------------------ */

#define FBDOC_zip \
"Pairs up elements from two or more sequences positionally, stopping at the shortest.\n" \
"\n" \
"**Params**\n" \
"- `a` — first sequence\n" \
"- `b` — second sequence (more may follow)\n" \
"\n" \
"**Returns** a `List` of tuples, one per position.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"zip([1, 2], ['a', 'b'])   # -> [(1, 'a'), (2, 'b')]\n" \
"```"

#define FBDOC_filter \
"Keeps only the elements a predicate accepts.\n" \
"\n" \
"**Params**\n" \
"- `list` — a `List`\n" \
"- `pred` — `a -> Bool`, called per element\n" \
"\n" \
"**Returns** a new `List` of the elements where `pred` was truthy.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"c_files = filter(files, f -> suffix(f) == '.c')\n" \
"```"

#define FBDOC_contains \
"Checks whether a list has an element equal to a given value.\n" \
"\n" \
"**Params**\n" \
"- `list` — a `List`\n" \
"- `value` — value to search for\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"contains(TARGETS, 'plugins')\n" \
"```"

#define FBDOC_concat \
"Concatenates any number of lists into one, in order.\n" \
"\n" \
"**Params**\n" \
"- `lists...` — zero or more `List`s (splat with `...` to flatten a list of lists in)\n" \
"\n" \
"**Returns** one combined `List`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"CFLAGS = concat(['-Wall'], gtk.cflags)\n" \
"```"

#define FBDOC_length \
"Counts a list's elements.\n" \
"\n" \
"**Params**\n" \
"- `list` — a `List`\n" \
"\n" \
"**Returns** `Int`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"n = length(srcs)\n" \
"```"

#define FBDOC_first \
"Gets a list's first element.\n" \
"\n" \
"**Params**\n" \
"- `list` — a non-empty `List`\n" \
"\n" \
"**Returns** the first element.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"main_src = first(srcs)\n" \
"```"

#define FBDOC_last \
"Gets a list's last element.\n" \
"\n" \
"**Params**\n" \
"- `list` — a non-empty `List`\n" \
"\n" \
"**Returns** the last element.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"tail = last(parts)\n" \
"```"

/* ---- string ----------------------------------------------------------------- */

#define FBDOC_cat \
"Concatenates strings together.\n" \
"\n" \
"**Params**\n" \
"- `parts...` — zero or more `String`s\n" \
"\n" \
"**Returns** one combined `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"cat('[', name, ']')   # -> '[name]'\n" \
"```"

#define FBDOC_join \
"Joins a list of strings with a separator between each.\n" \
"\n" \
"**Params**\n" \
"- `list` — `List<String>`\n" \
"- `sep` — separator `String`\n" \
"\n" \
"**Returns** the joined `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"join(['a', 'b', 'c'], ', ')   # -> 'a, b, c'\n" \
"```"

#define FBDOC_split \
"Splits a string on every occurrence of a separator.\n" \
"\n" \
"**Params**\n" \
"- `s` — string to split\n" \
"- `sep` — separator `String`\n" \
"\n" \
"**Returns** `List<String>`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"split('a,b,c', ',')   # -> ['a', 'b', 'c']\n" \
"```"

#define FBDOC_subst \
"Replaces every occurrence of a substring with another.\n" \
"\n" \
"**Params**\n" \
"- `s` — source string\n" \
"- `from` — substring to find\n" \
"- `to` — replacement text\n" \
"\n" \
"**Returns** the substituted `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"subst('a.c', '.c', '.o')   # -> 'a.o'\n" \
"```"

#define FBDOC_to_int \
"Converts a value to an `Int`, truncating a `Float`.\n" \
"\n" \
"**Params**\n" \
"- `v` — `String`, `Int`, or `Float`\n" \
"\n" \
"**Returns** `Int`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"n = to_int(opt('-j', '1'))\n" \
"```"

#define FBDOC_to_str \
"Converts any value to its string representation.\n" \
"\n" \
"**Params**\n" \
"- `v` — any value\n" \
"\n" \
"**Returns** `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"to_str(42)   # -> '42'\n" \
"```"

#define FBDOC_to_float \
"Converts a value to a `Float`.\n" \
"\n" \
"**Params**\n" \
"- `v` — `String`, `Int`, or `Float`\n" \
"\n" \
"**Returns** `Float`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"to_float('3.14')\n" \
"```"

/* ---- path -------------------------------------------------------------------- */

#define FBDOC_parent \
"Gets a path's parent directory, as text.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"parent(path('src/main.c'))   # -> 'src'\n" \
"```"

#define FBDOC_name \
"Gets a path's final component (filename with extension), as text.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"name(path('src/main.c'))   # -> 'main.c'\n" \
"```"

#define FBDOC_stem \
"Gets a path's filename without its extension.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"stem(path('src/main.c'))   # -> 'main'\n" \
"```"

#define FBDOC_suffix \
"Gets a path's extension, including the leading dot.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"suffix(path('src/main.c'))   # -> '.c'\n" \
"```"

#define FBDOC_with_suffix \
"Builds a new path with a different extension.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"- `ext` — new extension, including the leading dot\n" \
"\n" \
"**Returns** the new `File`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"obj = with_suffix(src, '.o')\n" \
"```"

#define FBDOC_with_name \
"Builds a new path with a different final component, same parent directory.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"- `name` — new filename\n" \
"\n" \
"**Returns** the new `File`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"with_name(src, 'main_test.c')\n" \
"```"

#define FBDOC_path \
"Builds a path by joining components.\n" \
"\n" \
"**Params**\n" \
"- `parts...` — zero or more `String` components\n" \
"\n" \
"**Returns** a `File`/`Path`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"path('build', 'obj', 'main.o')\n" \
"```"

#define FBDOC_abs \
"Resolves a path to an absolute one (relative to the current working directory).\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** the absolute `File`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"abs(path('build'))\n" \
"```"

/* ---- json / map --------------------------------------------------------------- */

#define FBDOC_json_parse \
"Parses a JSON string into a dynamic value.\n" \
"\n" \
"**Params**\n" \
"- `text` — JSON text\n" \
"\n" \
"**Returns** `Json` (object -> `Map`, array -> `List`, number -> `Int`/`Float`, `null` -> `()`).\n" \
"\n" \
"**Example**\n" \
"```\n" \
"cfg = json_parse('{\"jobs\": 4}')\n" \
"```"

#define FBDOC_json_read \
"Reads a file and parses its contents as JSON.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path`\n" \
"\n" \
"**Returns** `Json`, same shape as `json_parse`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"cfg = json_read('config.json')\n" \
"```"

#define FBDOC_json_str \
"Serializes a value to a compact JSON string.\n" \
"\n" \
"**Params**\n" \
"- `v` — any `Json`-compatible value\n" \
"\n" \
"**Returns** `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"json_str({'jobs': 4})   # -> '{\"jobs\":4}'\n" \
"```"

#define FBDOC_json_pretty \
"Serializes a value to an indented, human-readable JSON string.\n" \
"\n" \
"**Params**\n" \
"- `v` — any `Json`-compatible value\n" \
"\n" \
"**Returns** `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"write('out.json', json_pretty(cfg))\n" \
"```"

#define FBDOC_json_write \
"Serializes a value to compact JSON and writes it to a file.\n" \
"\n" \
"**Params**\n" \
"- `path` — a `Path` to write\n" \
"- `v` — any `Json`-compatible value\n" \
"\n" \
"**Returns** the written `File`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"json_write('cache.json', { 'hash': h })\n" \
"```"

#define FBDOC_get \
"Reads a value out of a map by key.\n" \
"\n" \
"**Params**\n" \
"- `map` — a `Map`\n" \
"- `key` — `String` key\n" \
"\n" \
"**Returns** the value, or `()` if the key is absent.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"jobs = get(cfg, 'jobs') || 1\n" \
"```"

#define FBDOC_has \
"Checks whether a map has a given key.\n" \
"\n" \
"**Params**\n" \
"- `map` — a `Map`\n" \
"- `key` — `String` key\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"has(cfg, 'jobs')\n" \
"```"

#define FBDOC_keys \
"Lists a map's keys, in insertion order.\n" \
"\n" \
"**Params**\n" \
"- `map` — a `Map`\n" \
"\n" \
"**Returns** `List<String>`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"keys(cfg)\n" \
"```"

#define FBDOC_values \
"Lists a map's values, in the same order as `keys`.\n" \
"\n" \
"**Params**\n" \
"- `map` — a `Map`\n" \
"\n" \
"**Returns** `List<Json>`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"values(cfg)\n" \
"```"

#define FBDOC_items \
"Lists a map's entries as key/value tuples, for iterating with `|>`.\n" \
"\n" \
"**Params**\n" \
"- `map` — a `Map`\n" \
"\n" \
"**Returns** `List<Tuple<String, Json>>`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"items(cfg) |> (k, v) -> print(k, v)\n" \
"```"

/* ---- cli args --------------------------------------------------------------- */

#define FBDOC_flag \
"Checks whether any of the given flag names is present in the CLI args.\n" \
"\n" \
"**Params**\n" \
"- `names...` — one or more flag spellings, e.g. `'-v'`, `'--verbose'`\n" \
"\n" \
"**Returns** `Bool`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"config({ 'verbose': flag('-v', '--verbose') })\n" \
"```"

#define FBDOC_opt \
"Reads an option's value from the CLI args (`name value` or `name=value` form). The default is only evaluated if the option is absent, so a lambda default can defer expensive work.\n" \
"\n" \
"**Params**\n" \
"- `name` — option spelling, e.g. `'-j'`\n" \
"- `default` — value (or `Unit -> value` lambda) used when the option isn't present\n" \
"\n" \
"**Returns** `String`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"jobs = to_int(opt('-j', '1'))\n" \
"```"

#define FBDOC_positionals \
"Collects every CLI argument that isn't a flag (doesn't start with `-`). Lighter-weight than a full `Parser<schema>` when a script just wants leftover file/target names.\n" \
"\n" \
"**Returns** `List<String>`.\n" \
"\n" \
"**Example**\n" \
"```\n" \
"targets = positionals()\n" \
"```"

/* ---- CLI-bound globals (values, not calls) ------------------------------------ */

#define FBDOC_args \
"The script's CLI arguments (everything after the `.fb` path).\n" \
"\n" \
"**Type** `List<String>`\n" \
"\n" \
"**Example**\n" \
"```\n" \
"jobs = opt('-j', '1')   # reads from `args` internally\n" \
"```"

#define FBDOC_cwd \
"The current working directory the script was invoked from.\n" \
"\n" \
"**Type** `String`\n" \
"\n" \
"**Example**\n" \
"```\n" \
"root = path(cwd)\n" \
"```"

/* ---- mutable stores ------------------------------------------------------------ */

#define FBDOC_GLOBALS \
"Per-interpreter mutable store, for state that doesn't fit the lazy-def model (counters, caches). Written in place with `<-`, read with `.field`/`get`; saved and restored per interpreter (safe across multiple `fb_State`s).\n" \
"\n" \
"**Type** `Map`\n" \
"\n" \
"**Example**\n" \
"```\n" \
"_GLOBALS.count <- 0\n" \
"xs |> n -> (_GLOBALS.count <- _GLOBALS.count + 1)\n" \
"```"

#define FBDOC_SHARED \
"Mutable store shared across every interpreter and plugin in the process (unlike `_GLOBALS`, which is per-interpreter). Same `<-`/`.field` access.\n" \
"\n" \
"**Type** `Map`\n" \
"\n" \
"**Example**\n" \
"```\n" \
"_SHARED.cfg <- { 'jobs': 4 }\n" \
"```"

#endif /* BUILTIN_DOCS_H */
