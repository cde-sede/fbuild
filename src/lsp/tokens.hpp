#ifndef fb_lsp_tokens_hpp
#define fb_lsp_tokens_hpp

#include "ca/integers.hpp"
#include "ca/sv.hpp"
#include "check/catalog.hpp"
#include "check/checker.hpp"
#include "core/memory.hpp"
#include "syntax/ast.hpp"

namespace fb::lsp {

	/* Semantic token types, in the order of the legend the server announces (their indexes
	   are what tokens carry). */
	enum class token_type : ca::u8 {
		namespace_,  // a native object of a plugin: regex
		type,        // a built-in type in an annotation: Int, List, File
		class_,      // a script class: type Point(...)
		parameter,
		variable,
		property,    // a field
		function,
		method,
		keyword,     // else, match, and, or, not, true, false, nil
		string,
		number,
	};

	inline constexpr ca::sv::str token_types[] = {
		"namespace", "type", "class", "parameter", "variable", "property", "function", "method", "keyword", "string", "number",
	};

	/* Modifier bits, in legend order. */
	inline constexpr ca::u32 declaration = 1U << 0U;     // where the name is defined
	inline constexpr ca::u32 default_library = 1U << 1U; // a builtin, or a plugin's
	inline constexpr ca::u32 unresolved = 1U << 2U;      // an undefined name, which stands for its own text

	inline constexpr ca::sv::str token_modifiers[] = {
		"declaration", "defaultLibrary", "unresolved",
	};

	/* One classified token of the script; a single line. */
	struct semantic_token final {
		fb::position start;
		fb::position end;  // inclusive, as tokens are
		fb::lsp::token_type type;
		ca::u32 modifiers;
	};

	/* The program's names and literals, classified from what the checker resolved, in source
	   order without overlaps. */
	[[nodiscard]] auto semantic_tokens(fb::memory& memory, const fb::node* program, const fb::check::report& report, const fb::check::catalog& catalog) noexcept
		-> fb::list<fb::lsp::semantic_token>;

} // namespace fb::lsp

#endif // fb_lsp_tokens_hpp
