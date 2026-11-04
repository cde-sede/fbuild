#ifndef fb_syntax_dump_hpp
#define fb_syntax_dump_hpp

#include "ca/result.hpp"
#include "core/memory.hpp"
#include "syntax/ast.hpp"

namespace fb {

	/* Prints root and its descendants to stdout as a tree, one "kind token" line per node. */
	[[nodiscard]] auto dump(fb::memory& memory, const fb::node* root) noexcept -> ca::result<void>;

} // namespace fb

#endif // fb_syntax_dump_hpp
