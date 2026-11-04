#ifndef fb_syntax_ast_hpp
#define fb_syntax_ast_hpp

#include "ca/integers.hpp"
#include "ca/sv.hpp"
#include "core/memory.hpp"
#include "syntax/token.hpp"

namespace fb {

	/* Node kinds; the layout of each node's token and children is noted beside it. */
	enum class node_kind : ca::u8 {
		program,     // kids: statements
		assign,      // tok: name; kids[0]: value (lazy, memoized)
		eager,       // tok: name; kids[0]: value (~=)
		store,       // tok: "<-"; kids[0]: lvalue field chain, kids[1]: value
		force,       // tok: "~"; kids[0]: operand
		tuple,       // kids: elements
		listlit,     // kids: elements (List<...>)
		maplit,      // kids: entries, each a tuple (key, value), or a splat
		parser,      // kids[0]: schema
		lambda,      // kids[0]: params, kids[1]: body
		params,      // kids: parameters
		seq,         // kids: operands of >>
		pipe,        // kids: operands of |>
		guard,       // kids[0]: body, kids[1]: where or null, kids[2]: when or null, kids[3]: else or null
		where,       // kids: bindings
		binding,     // tok: name; kids[0]: value
		when,        // kids[0]: condition
		compare,     // tok: operator; kids[0], kids[1]
		arith,       // tok: + - * / %; kids[0], kids[1]
		neg,         // tok: "-"; kids[0]
		logical,     // tok: and / or / ||; kids[0], kids[1]
		not_,        // tok: "not"; kids[0]
		morphism,    // tok: "//"; kids[0], kids[1]
		objnew,      // tok: class name; kids[0]: fields body
		methoddef,   // tok: method name; kids[0]: class id, kids[1]: body
		methodcopy,  // tok: method name or null (wildcard); kids[0]: dest class, kids[1]: source class, kids[2]: source method
		call,        // kids[0]: callee, kids[1..]: arguments
		field,       // tok: field name or index; kids[0]: base; dynamic .{e}: tok null, kids[1]: index
		construct,   // tok: type name; kids: cfields
		cfield,      // tok: name, or null for "*"; kids[0]: value
		splat,       // kids[0]: expression
		metadata,    // tok: "&"; kids[0]
		capture,     // tok: "@"; kids[0]: id, or none
		projection,  // tok: name or index
		command,     // kids: cmdtext or interpolated expressions
		cmdtext,     // tok: command word
		id,          // tok; as a parameter, kids[0]: optional type annotation
		integer,     // tok
		floating,    // tok
		boolean,     // tok
		nil,         // tok: "nil" or "("
		string,      // tok (plain) or kids: strtext and interpolated expressions
		strtext,     // tok
		path,        // tok
		typeexpr,    // tok: base type name; kids: generic arguments
		typeunion,   // tok: first member's; kids: members
		else_,       // kids[0]: the value when the guard's condition fails
		match_,      // tok: "match"; kids: arms
		arm,         // kids[0]: pattern, kids[1]: condition (when) or null, kids[2]: body
		pwild,       // tok: "_"
		pbind,       // tok: name; kids[0]: optional type annotation
		plit,        // kids[0]: the literal (int, float, bool, nil, string, path, or neg of a number)
		ptuple,      // kids: element patterns
	};

	/* Name of a node kind, as the AST dump shows it ("assign"). */
	[[nodiscard]] auto node_name(fb::node_kind kind) noexcept -> ca::sv::str;

	/* A stretch of a script, both ends 1-based and inclusive of the last token's end; {0, 0}
	   when unknown. */
	struct span final {
		fb::position start;
		fb::position end;
	};

	struct node final {
		fb::node_kind kind;
		fb::token* tok;
		fb::list<fb::node*> kids;
		ca::u32 id;  // 0, 1, 2, ... in creation order within one parse: indexes side tables

		[[nodiscard]] auto size(void) const noexcept -> ca::usz {
			return kids.size();
		}

		[[nodiscard]] auto kid(const ca::usz index) const noexcept -> fb::node* {
			return index < kids.size() ? kids.data()[index] : nullptr;
		}
	};

	/* The source a node and everything below it covers. */
	[[nodiscard]] auto span_of(const fb::node* n) noexcept -> fb::span;

} // namespace fb

#endif // fb_syntax_ast_hpp
