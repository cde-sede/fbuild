#ifndef fb_syntax_parser_hpp
#define fb_syntax_parser_hpp

#include "ca/result.hpp"
#include "core/diagnostic.hpp"
#include "core/memory.hpp"
#include "syntax/ast.hpp"
#include "syntax/token.hpp"

namespace fb {

	/* The program node of tokens (from fb::lex); tokens must outlive it, and may be edited (a
	   ">>" closing two generic brackets is split in place). */
	[[nodiscard]] auto parse(fb::memory& memory, fb::list<fb::token>& tokens) noexcept -> ca::result<fb::node*, fb::diagnostic>;

	/* fb::lex then fb::parse. */
	[[nodiscard]] auto parse_source(fb::memory& memory, ca::sv::str source) noexcept -> ca::result<fb::node*, fb::diagnostic>;

} // namespace fb

#endif // fb_syntax_parser_hpp
