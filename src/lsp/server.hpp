#ifndef fb_lsp_server_hpp
#define fb_lsp_server_hpp

#include "ca/result.hpp"
#include "core/memory.hpp"
#include "lsp/document.hpp"
#include "lsp/json.hpp"
#include "plugins/loader.hpp"
#include "runtime/interpreter.hpp"

namespace fb::lsp {

	/* The language server: answers one message at a time, with every open document checked
	   against what the interpreter registers (its builtins and the plugins loaded at
	   initialize). Requests are answered from the documents' last analysis; nothing runs. */
	class server final {

		private:

			fb::memory& _memory;  // documents and their URIs; lives as long as the server
			fb::interpreter& _in;
			fb::plugin_set& _plugins;
			fb::list<fb::lsp::document*> _documents;
			bool _utf16;
			bool _hierarchy;     // the client shows nested document symbols
			bool _markdown;      // the client renders markdown in hovers
			bool _initialized;
			bool _shut_down;
			bool _exiting;

			[[nodiscard]] auto _find(ca::sv::str uri) noexcept -> fb::lsp::document*;

			[[nodiscard]] auto _open(ca::sv::str uri) noexcept -> fb::lsp::document*;

			auto _close(ca::sv::str uri) noexcept -> void;

			[[nodiscard]] auto _publish(fb::memory& scratch, const fb::lsp::document& d) noexcept -> ca::result<void>;

			[[nodiscard]] auto _initialize(fb::memory& scratch, const fb::lsp::json* id, const fb::lsp::json* params) noexcept -> ca::result<void>;

			[[nodiscard]] auto _hover(fb::memory& scratch, const fb::lsp::json* id, const fb::lsp::json* params) noexcept -> ca::result<void>;

			[[nodiscard]] auto _definition(fb::memory& scratch, const fb::lsp::json* id, const fb::lsp::json* params) noexcept -> ca::result<void>;

			[[nodiscard]] auto _references(fb::memory& scratch, const fb::lsp::json* id, const fb::lsp::json* params, bool highlight) noexcept -> ca::result<void>;

			[[nodiscard]] auto _symbols(fb::memory& scratch, const fb::lsp::json* id, const fb::lsp::json* params) noexcept -> ca::result<void>;

			[[nodiscard]] auto _completion(fb::memory& scratch, const fb::lsp::json* id, const fb::lsp::json* params) noexcept -> ca::result<void>;

			[[nodiscard]] auto _semantic_tokens(fb::memory& scratch, const fb::lsp::json* id, const fb::lsp::json* params) noexcept -> ca::result<void>;

			[[nodiscard]] auto _signature(fb::memory& scratch, const fb::lsp::json* id, const fb::lsp::json* params) noexcept -> ca::result<void>;

		public:

			server(fb::memory& memory, fb::interpreter& in, fb::plugin_set& plugins) noexcept;

			server(const server&) = delete;

			auto operator=(const server&) -> server& = delete;

			/* Handles one message (a request, a notification, or a stray response); scratch holds
			   whatever the answer needs and is the caller's to reset. Fails only when standard
			   output does. */
			[[nodiscard]] auto handle(fb::memory& scratch, const fb::lsp::json* message) noexcept -> ca::result<void>;

			/* Answers a message that is not JSON. */
			[[nodiscard]] auto reject(fb::memory& scratch) noexcept -> ca::result<void>;

			/* Whether the client sent exit. */
			[[nodiscard]] auto exiting(void) const noexcept -> bool {
				return _exiting;
			}

			/* The exit status the protocol asks for: 0 after shutdown, 1 otherwise. */
			[[nodiscard]] auto status(void) const noexcept -> int {
				return _shut_down ? 0 : 1;
			}

			[[nodiscard]] auto cleanup(void) noexcept -> ca::result<void>;
	};

} // namespace fb::lsp

#endif // fb_lsp_server_hpp
