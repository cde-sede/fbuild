; Funcbuild scopes / variable definitions / references.
;
; Any identifier on the left of "=" is a variable definition, regardless of
; case (UPPER and lower are both valid LHS names). Lambda parameters and
; where-bindings are definitions in their own scope.

; Scopes
(source_file) @local.scope
(lambda) @local.scope
(where_clause) @local.scope

; Definitions
(assignment name: (identifier) @local.definition.var)
(binding name: (identifier) @local.definition.var)
(parameter (identifier) @local.definition.parameter)
(capture (identifier) @local.definition.parameter)

; References
(identifier) @local.reference
