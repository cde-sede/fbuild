; Funcbuild highlights
;
; Note on ordering: nvim-treesitter lets later patterns win on ties, so the
; broad `(identifier) @variable` fallback comes first and specific captures
; below override it.

; Fallback: any identifier is a variable reference.
(identifier) @variable

; SCREAMING_SNAKE_CASE identifiers are build vars/constants.
((identifier) @constant
  (#match? @constant "^[a-zA-Z_][a-zA-Z0-9_]*$"))

; Assignment / binding targets.
(assignment name: (identifier) @variable)
(binding name: (identifier) @variable)
((assignment name: (identifier) @constant)
  (#match? @constant "^[a-zA-Z_][a-zA-Z0-9_]*$"))

; Function calls: identifier immediately followed by "(...)".
(postfix_expression
  (identifier) @function.call
  (call_suffix))

; Constructor name.
(postfix_expression
  (identifier) @type
  (constructor_suffix))
(tuple_constructor "Tuple" @type)
(parser_constructor "Parser" @type)
(constructor_field name: (identifier) @property)

; Field access and projections.
(field (identifier) @property)
(projection (identifier) @property)

; Lambda parameters and captures.
; Only the identifier is a parameter; the "@" sigil stays an operator.
(parameter (identifier) @variable.parameter)
(capture (identifier) @variable.parameter)

; Keywords.
[
  "where"
  "when"
  "and"
  "or"
  "not"
  "type"
  "method"
] @keyword

; Object type name + method name.
(type_definition name: (identifier) @type)
(method_definition type: (identifier) @type name: (identifier) @function.method)

; Operators.
[
  ">>"
  "|>"
  "||"
  "->"
  "//"
  "="
  "&"
  "..."
  "@"
  "*"
  "+"
  "-"
  "/"
  "%"
] @operator
(compare_op) @operator

; Punctuation.
[
  "("
  ")"
  "<"
  ">"
  "{"
  "}"
] @punctuation.bracket
[
  ","
  "."
] @punctuation.delimiter
"`" @punctuation.special

; Booleans.
(boolean) @boolean

; Comments.
(comment) @comment

; Literals.
(string) @string
(string_content_dq) @string
(string_content_sq) @string
(number) @number
(integer) @number
(float) @number
(path) @string.special.path
(command_word) @string
