#ifndef fb_lsp_query_hpp
#define fb_lsp_query_hpp

#include "ca/optional.hpp"
#include "ca/sv.hpp"
#include "check/checker.hpp"
#include "core/diagnostic.hpp"
#include "core/memory.hpp"
#include "syntax/ast.hpp"

namespace fb::lsp {

	/* Questions about a checked program, by position; the server turns the answers into
	   protocol messages. */

	/* Whether at lies within s (both ends inclusive). */
	[[nodiscard]] auto contains(fb::span s, fb::position at) noexcept -> bool;

	/* The nodes containing at, from the program down to the innermost. */
	[[nodiscard]] auto locate(fb::memory& memory, const fb::node* program, fb::position at) noexcept -> fb::list<const fb::node*>;

	/* The innermost node of path whose own token holds at and names something (an identifier,
	   a field, a definition, a parameter, a pattern, a type); nullptr when there is none. */
	[[nodiscard]] auto name_at(ca::borrowed_slice<const fb::node* const> path, fb::position at) noexcept -> const fb::node*;

	/* What a name node defines or uses, as the text a hover shows ("x", "Point.area"). */
	[[nodiscard]] auto name_of(fb::memory& memory, const fb::node* n) noexcept -> ca::sv::str;

	/* Where the definition at site is named: its name token, else all of it. */
	[[nodiscard]] auto site_span(const fb::node* site) noexcept -> fb::span;

	/* Every node of the program that uses what site defines, site's own name first. */
	[[nodiscard]] auto uses_of(fb::memory& memory, const fb::node* program, const fb::check::report& report, const fb::node* site) noexcept
		-> fb::list<const fb::node*>;

	/* A name visible at a place: a lambda parameter, a where binding, a pattern variable, a
	   ~= binding above it. */
	struct binder final {
		ca::sv::str name;
		const fb::node* site;
	};

	/* The local names visible at the end of path, innermost first. */
	[[nodiscard]] auto binders(fb::memory& memory, ca::borrowed_slice<const fb::node* const> path, fb::position at) noexcept -> fb::list<fb::lsp::binder>;

	/* The field node whose name starts at (a completion after "base."); nullptr when none. */
	[[nodiscard]] auto field_at(const fb::node* program, fb::position at) noexcept -> const fb::node*;

	/* The call a cursor at offset of text is inside: the callee's name, the argument the
	   cursor is in (0-based), and whether the callee is a method (after a '.'). Found from the
	   text, as the call being typed is usually unclosed: brackets are matched backwards,
	   strings and comments skipped. */
	struct call_context final {
		ca::sv::str callee;
		ca::usz active;
		bool method;
	};

	[[nodiscard]] auto call_at(fb::memory& memory, ca::sv::str text, ca::usz offset) noexcept -> ca::optional<fb::lsp::call_context>;

	/* Whether offset is inside the schema of a Parser<{...}>. */
	[[nodiscard]] auto in_parser_schema(fb::memory& memory, ca::sv::str text, ca::usz offset) noexcept -> bool;

	/* The identifier ending at offset of text (what completion filters by), possibly empty,
	   and whether a '.' comes right before it. */
	struct word_before final {
		ca::usz start;
		bool after_dot;
	};

	[[nodiscard]] auto word_at(ca::sv::str text, ca::usz offset) noexcept -> fb::lsp::word_before;

} // namespace fb::lsp

#endif // fb_lsp_query_hpp
