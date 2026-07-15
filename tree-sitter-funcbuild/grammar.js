/**
 * @file Funcbuild grammar for tree-sitter
 * @author cde-sede <clement.desede@gmail.com>
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// Character classes lifted from the ABNF.
const ESCAPED = /\\[\x00-\x7f]/;                       // escaped = "\" %x00-7F
const SQUOTE = /'(\\[\x00-\x7f]|[\x20-\x26\x28-\x7e])*'/;
const DQUOTE = /"(\\[\x00-\x7f]|[\x20-\x21\x23-\x7e])*"/;
const COMMAND_CHAR = /[\x21-\x5f\x61-\x7a\x7c-\x7e]/;   // printable, no wsp / ` / {

export default grammar({
  name: "funcbuild",

  // Whitespace, including newlines, is insignificant: "ignore newlines unless
  // they break syntax". Statements still separate because no rule juxtaposes
  // two expressions, so consecutive statements cannot merge. (The executor
  // enforces the stricter newline-between-statements rule.)
  extras: $ => [/[ \t\r\n]/, $.comment],

  conflicts: $ => [
    // "id <" : constructor-suffix vs "<" comparison, told apart by lookahead.
    [$._postfix, $.postfix_expression],
    // a lambda parameter list and a tuple of atoms look identical until the
    // "->" arrives, so keep both alive.
    [$._atom, $.parameter],
    // single-element reduce vs continue at each precedence tier.
    [$._tuple, $.tuple],
    [$._sequence, $.sequence_expression],
    [$._pipe, $.pipe_expression],
    // `... expr`: the spread operand runs down to pipe level, so every
    // precedence tier needs a reduce-vs-continue conflict in that context.
    [$._guarded, $.guarded_expression],
    [$._logical, $.logical_expression],
    [$._comparison, $.comparison_expression],
    [$._additive, $.arith_expression],
    [$._multiplicative, $.arith_expression_mul],
    // `LVALUE <- expr`: the target is a bare _postfix, but _postfix also
    // reduces upward through every arithmetic/comparison tier, so the parser
    // needs to keep both alive until it sees whether `<-` follows.
    [$.store_expression, $._unary],
  ],

  word: $ => $.identifier,

  rules: {
    // program = *statement  (newlines are extras; statements can't merge)
    source_file: $ => repeat($.statement),

    statement: $ => choice(
      $.type_definition,
      $.method_definition,
      $.expression,
    ),

    // type NAME(params) = fields : a custom object type + constructor.
    // NOTE: the interpreter treats `type`/`method` as *contextual* keywords
    // (usable as ordinary identifiers), but LR keyword extraction makes them
    // reserved here -- a grammar-only limitation; real scripts rarely name a
    // variable `type` or `method`.
    type_definition: $ => seq(
      "type",
      field("name", $.identifier),
      "(",
      optional($.parameter_list),
      ")",
      "=",
      field("value", $.expression),
    ),

    // method TYPE.NAME = lambda : attach a method to a type.
    method_definition: $ => seq(
      "method",
      field("type", $.identifier),
      ".",
      field("name", $.identifier),
      "=",
      field("value", $.expression),
    ),

    //
    // Expressions
    //
    expression: $ => choice(
      $.assignment,
      $.eager_assignment,
      $.store_expression,
      $._tuple,
    ),

    assignment: $ => seq(
      field("name", $.identifier),
      "=",
      field("value", $.expression),
    ),

    // `~=`: eager, re-definable assignment (evaluated at this point).
    eager_assignment: $ => seq(
      field("name", $.identifier),
      "~=",
      field("value", $.expression),
    ),

    // `LVALUE <- expr`: mutable store. `<-` lexed before `<` (maximal munch),
    // so `a<-b` is a store, not `a < -b`.
    store_expression: $ => seq(
      field("target", $._postfix),
      "<-",
      field("value", $.expression),
    ),

    // tuple binds looser than everything but assignment.
    _tuple: $ => choice($.tuple, $._lambda_or_seq),
    tuple: $ => prec.left(seq(
      $._lambda_or_seq,
      repeat1(seq(",", $._lambda_or_seq)),
    )),

    _lambda_or_seq: $ => choice($.lambda, $._sequence),

    //
    // Sequencing
    //
    _sequence: $ => choice($.sequence_expression, $._pipe),
    sequence_expression: $ => prec.left(seq(
      $._pipe,
      repeat1(seq(">>", $._pipe)),
    )),

    //
    // Pipeline
    //
    _pipe: $ => choice($.pipe_expression, $._guarded),
    // a lambda may be the right-hand side of |> without parens (`x |> f -> body`)
    pipe_expression: $ => prec.left(seq(
      $._guarded,
      repeat1(seq("|>", choice($.lambda, $._guarded))),
    )),

    //
    // Guards and metadata
    //
    _guarded: $ => choice($.guarded_expression, $._logical),
    guarded_expression: $ => prec.left(seq(
      $._logical,
      choice(
        seq($.where_clause, optional($.when_clause)),
        $.when_clause,
      ),
    )),

    where_clause: $ => seq("where", $.bindings),

    bindings: $ => prec.right(seq($.binding, repeat(seq(",", $.binding)))),

    // value stays below guarded so it cannot swallow the where/when tail.
    binding: $ => seq(
      field("name", $.identifier),
      "=",
      field("value", $._logical),
    ),

    when_clause: $ => seq("when", $.expression),

    //
    // Logical (and / or / not), looser than comparison
    //
    _logical: $ => choice($.logical_expression, $._not),
    logical_expression: $ => prec.left(seq(
      $._not,
      repeat1(seq(choice("and", "or", "||"), $._not)),
    )),

    _not: $ => choice($.not_expression, $._comparison),
    not_expression: $ => prec.right(seq("not", $._not)),

    //
    // Comparisons
    //
    _comparison: $ => choice($.comparison_expression, $._additive),
    comparison_expression: $ => prec.left(seq(
      $._additive,
      repeat1(seq($.compare_op, $._additive)),
    )),

    compare_op: _ => choice("<", ">", "<=", ">=", "==", "!="),

    //
    // Arithmetic, tighter than comparison
    //
    _additive: $ => choice($.arith_expression, $._multiplicative),
    _multiplicative: $ => choice($.arith_expression_mul, $._unary),
    arith_expression: $ => prec.left(seq(
      $._multiplicative,
      repeat1(seq(choice("+", "-"), $._multiplicative)),
    )),
    arith_expression_mul: $ => prec.left(seq(
      $._unary,
      repeat1(seq(choice("*", "/", "%"), $._unary)),
    )),

    //
    // Unary minus, tighter than * / %, looser than postfix
    //
    // Caveat (same pre-existing "statements aren't newline-terminated"
    // looseness noted elsewhere in this grammar): `-` can now also START a
    // statement, so GLR sometimes resolves a trailing binary `a - b` as `a`
    // followed by a new `- b` statement instead of continuing the same
    // arith_expression. The hand-written interpreter parser (build_system.c)
    // has no such ambiguity; this only affects editor tooling.
    _unary: $ => choice($.neg_expression, $._postfix),
    neg_expression: $ => prec.right(seq("-", $._unary)),

    //
    // Postfix and atoms
    //
    _postfix: $ => choice($.postfix_expression, $._atom),
    postfix_expression: $ => prec.left(seq(
      $._atom,
      repeat1(choice(
        $.call_suffix,
        $.field_suffix,
        $.constructor_suffix,
        $.morphism_suffix,
      )),
    )),

    call_suffix: $ => seq("(", optional($.argument_list), ")"),
    field_suffix: $ => seq(".", choice($.field, $.interpolation)),
    constructor_suffix: $ => seq("<", optional($.constructor_fields), ">"),
    morphism_suffix: $ => seq("//", $._atom),

    _atom: $ => choice(
      $.capture,
      $.projection,
      $.metadata_expression,
      $.force_expression,
      $.splat_expression,
      $.tuple_constructor,
      $.map_constructor,
      $.parser_constructor,
      $.map,
      $.path,
      $.command,
      $.string,
      $.number,
      $.boolean,
      $.identifier,
      seq("(", $.expression, ")"),
    ),

    // `~x`: force re-evaluation of x, ignoring memoization.
    force_expression: $ => seq("~", $._postfix),

    // map literal: { "k": v, k2: v2 }  (keys: string or bare word)
    map: $ => seq(
      "{",
      optional(seq($.map_entry, repeat(seq(",", $.map_entry)), optional(","))),
      "}",
    ),
    map_entry: $ => seq(
      field("key", $._lambda_or_seq),
      ":",
      field("value", $._lambda_or_seq),
    ),

    //
    // Lambda
    //
    lambda: $ => seq(
      $.parameter_list,
      "->",
      field("body", $.expression),
    ),

    parameter_list: $ => seq(
      $.parameter,
      repeat(seq(",", $.parameter)),
    ),

    parameter: $ => choice(
      $.identifier,
      $.projection,
      $.capture,
    ),

    // prefer "@" id over a bare "@" when an identifier follows.
    capture: $ => choice(prec(1, seq("@", $.identifier)), "@"),

    projection: $ => choice(
      seq(".", $.identifier),
      seq(".", $.integer),
    ),

    //
    // Constructors
    //
    constructor_fields: $ => seq(
      $.constructor_field,
      repeat(seq(",", $.constructor_field)),
    ),

    constructor_field: $ => choice(
      seq("*", $._postfix),
      seq(field("name", $.identifier), "=", field("value", $._postfix)),
    ),

    //
    // Function calls
    //
    argument_list: $ => seq(
      $._lambda_or_seq,
      repeat(seq(",", $._lambda_or_seq)),
    ),

    //
    // Field access
    //
    field: $ => choice($.identifier, $.integer),

    //
    // Metadata
    //
    metadata_expression: $ => seq("&", $._postfix),

    //
    // Splat
    //
    // splat spreads the whole following expression up to pipe level, so
    // `...xs |> f` parses as `...(xs |> f)`.
    splat_expression: $ => prec.right(seq("...", $._pipe)),

    //
    // Command
    //
    command: $ => seq(
      "`",
      repeat(choice($.command_word, $.interpolation)),
      "`",
    ),

    command_word: _ => token(repeat1(choice(
      COMMAND_CHAR,
      SQUOTE,
      DQUOTE,
    ))),

    interpolation: $ => seq("{", $.expression, "}"),

    //
    // Lexical
    //
    path: _ => token(seq("[", repeat(/[\x20-\x5a\x5c\x5e-\x7e]/), "]")),

    // strings interpolate `{ expr }`; literal chunks and the closing quote are
    // token.immediate so no extras (whitespace, and crucially comments) can be
    // inserted inside a string -- a `#` in a string stays string text.
    string: $ => choice(
      seq('"', repeat(choice($.string_content_dq, $.interpolation)), token.immediate('"')),
      seq("'", repeat(choice($.string_content_sq, $.interpolation)), token.immediate("'")),
    ),

    // prec 1 so a `#` opening a chunk beats the `comment` extra inside a string.
    string_content_dq: _ => token.immediate(prec(1, repeat1(choice(ESCAPED, /[^"\\{\n]/)))),
    string_content_sq: _ => token.immediate(prec(1, repeat1(choice(ESCAPED, /[^'\\{\n]/)))),

    number: $ => choice($.float, $.integer),
    integer: _ => token(/[0-9]+/),
    float: _ => token(/[0-9]+\.[0-9]+/),

    // Tuple<e, ..., ...spread>: build a tuple from values and splats.
    // (_postfix already covers `...x` via splat_expression.)
    tuple_constructor: $ => seq(
      "Tuple", "<",
      optional(seq($._postfix, repeat(seq(",", $._postfix)))),
      ">",
    ),

    // Map<(k,v), (k2,v2), ...>: build a map from pair expressions.
    map_constructor: $ => seq(
      "Map", "<",
      optional(seq($._postfix, repeat(seq(",", $._postfix)))),
      ">",
    ),

    // Parser<schema>: schema is a single map expression.
    parser_constructor: $ => seq(
      "Parser", "<", $._postfix, ">",
    ),

    boolean: _ => choice("true", "false"),

    identifier: _ => /[A-Za-z_][A-Za-z0-9_]*/,

    comment: _ => token(seq("#", /[^\n]*/)),
  },
});
