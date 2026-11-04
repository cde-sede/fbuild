#ifndef fb_lsp_transport_hpp
#define fb_lsp_transport_hpp

#include "ca/optional.hpp"
#include "ca/result.hpp"
#include "ca/sv.hpp"
#include "core/memory.hpp"

namespace fb::lsp {

	/* The base protocol over the standard streams: each message is "Content-Length: N\r\n",
	   other headers (ignored), a blank line, then N bytes of JSON. Nothing else may reach
	   standard output; notices go to standard error. */

	/* The next message's body, in memory; nullopt at the end of input or when the stream is
	   no longer framed (a missing or malformed Content-Length). */
	[[nodiscard]] auto read_message(fb::memory& memory) noexcept -> ca::optional<ca::sv::str>;

	/* Frames body and writes it out at once. */
	[[nodiscard]] auto write_message(ca::sv::str body) noexcept -> ca::result<void>;

} // namespace fb::lsp

#endif // fb_lsp_transport_hpp
