#ifndef fb_syntax_lexer_hpp
#define fb_syntax_lexer_hpp

#include "ca/result.hpp"
#include "ca/sv.hpp"
#include "core/diagnostic.hpp"
#include "core/memory.hpp"
#include "syntax/token.hpp"

namespace fb {

	/* The tokens of source, ending with one tok::eof; see SYNTAX.md for the rules. */
	[[nodiscard]] auto lex(fb::memory& memory, ca::sv::str source) noexcept -> ca::result<fb::list<fb::token>, fb::diagnostic>;

} // namespace fb

#endif // fb_syntax_lexer_hpp
