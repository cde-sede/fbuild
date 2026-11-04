# Intended divergences from the C fbuild

The C fbuild is deprecated; fbuild is no longer kept compatible with it. Language
added since (no C equivalent): `e when c else f`, `match { ... }`, `fail`, `assert`;
the type checker is rewritten in src/check/ (gradual, signature-driven) and also runs
before every script, as the C one did.

Behaviour that differs from ~/probes/cparser on purpose, each because the C
version contradicts its own spec (SYNTAX.md / SKILLS.md).

- `method NEW.m = OLD.fn(args)`: the C parser's "nothing follows" check matches
  `TOK_EOF`, which is 0 = NONE = "any kind" in its matcher, so every `OLD.x ...`
  right-hand side is taken for a method copy and then fails ("expected newline
  after statement"). SYNTAX.md says it falls through to an ordinary expression;
  fbuild does that.

- Commands: a failed step of `a >> b` stops the sequence (unless
  `config({keep_going: true})`); the C runner evaluated and ran the later steps
  anyway. `>>` is documented as "ordered; build dependency".
- Exit status: fbuild exits 1 when any command it ran failed; the C version
  exited 0.
- Command words are unquoted like a shell's (`sh -c "echo a; b"` passes
  `echo a; b`); the C version kept the quote characters in argv. The display
  still shows the words as written.
- A failed command's exit code is reported as is (128+N for signal N); the C
  runner used WEXITSTATUS only.
- A call `f(x)` resolves a script definition or local `f` before a builtin of the
  same name; the C evaluator always called the builtin.
- Paths are computed by `ca::fs::path`, with pathlib's rules: `.bashrc` has stem
  `.bashrc` and no suffix (C: stem "", suffix `.bashrc`); `a.` has no suffix;
  `parent([/x])` is `/` (C: ""); `name([a/b/])` is `b` (C: ""); `path("a", "/b")`
  is `/b` and `path("a/", "b")` is `a/b` (C: `a//b`). A path computed from
  another (parent, with_name, with_suffix, `File<*p, ...>`, path(...)) is
  normalized: `[./a/b.c].parent` is `a` (C: `./a`). Path literals and glob
  results keep their text.
- glob: a hidden name written literally in the pattern matches
  (`glob("sub/.hid/*.c")`); the C glob skipped every name starting with '.'.
  Hidden names are still skipped where a wildcard would match them.
- `sort` (fbloop) is stable; the C plugin used qsort, so the order of items with
  equal text could differ.
- `math.min` / `math.max` (fbmath) work; the C type checker rejected every call
  (its generic `min`/`max` signature, which wants a List, also caught the methods).
- fbregex runs on `ca::basic_regex`, not glibc's POSIX `<regex.h>`: patterns are
  Python `re` syntax, matched leftmost-first (`a|ab` on "ab" finds `a`; POSIX
  takes the longest, `ab`), with no backreferences in patterns and no POSIX
  bracket classes such as `[[:digit:]]` (use `\d`). Result shapes are the C
  plugin's (findall gives whole matches, split drops groups, replace takes
  `\0`-`\9`). An invalid pattern's message is Python's, with its position.
  `regex.split` keeps the text an empty match skips over: `split("abc", "x*")`
  is `[abc]` (C: `[]`, the pointer it advanced dropped the pending piece).
  `regex.escape(s)` is new.
- Display: the runner's output is fbuild's own, not the C runner's. On a terminal: a
  live tree (one-cell spinner, ✓/✗, per-step and per-group times, right-aligned and
  clipped with …); a finished group folds into one summary line unless it failed or
  `verbose` is set; unlabelled nested groups show their commands as their parent's.
  Otherwise one `+`/`x` line per finished command (parallel groups in declaration order
  when the group ends), then a `+ run: N steps` summary. The closing output dump puts
  `--- cmd` before each command's output (`✓ cmd` and a `│` gutter on a terminal), with
  failed commands last. tests/run_diff.sh shows the closing durations as `T`.
- A bad value in a short-flag bundle (`-rjx`, `-j` an int) is reported as
  `--jobs expects integer, got 'x'`, as it is unbundled; the C bundle path set the error
  without a message (only the usage was printed).
