/* fbuildd: the language server for funcbuild scripts. It speaks the Language Server Protocol
   over standard input and output, and checks each open document with the same parser, type
   checker and builtins (plugins included) as fbuild, without running anything.

     fbuildd              serve on stdin/stdout (what an editor starts)
     fbuildd --version

   Plugins: the paths in initializationOptions.plugins, then $FBUILD_PLUGINS and
   $FBUILD_PLUGIN_PATH, as for fbuild. Notices go to standard error. */

#include "ca/flush.hpp"
#include "ca/io.hpp"
#include "ca/print.hpp"
#include "core/memory.hpp"
#include "core/version.hpp"
#include "lsp/json.hpp"
#include "lsp/server.hpp"
#include "lsp/transport.hpp"
#include "plugins/loader.hpp"
#include "runtime/interpreter.hpp"

namespace fb::lsp::impl {

	[[nodiscard]] static auto serve(fb::memory& memory) noexcept -> int {

		auto scratch = fb::memory::make();
		if (!scratch)
			return 1;
		fb::interpreter* const in = memory.make<fb::interpreter>(memory);
		fb::plugin_set plugins{memory};
		fb::lsp::server server{memory, *in, plugins};
		int status = 1;  // the input ended without exit: as an exit without shutdown
		for (;;) {
			scratch->reset();
			auto body = fb::lsp::read_message(*scratch);
			if (!body)
				break;
			const fb::lsp::json* const message = fb::lsp::parse_json(*scratch, *body);
			auto handled = message != nullptr ? server.handle(*scratch, message) : server.reject(*scratch);
			if (!handled) {
				static_cast<void>(ca::print(ca::io::err(), "fbuildd: cannot write to the client\n"));
				break;
			}
			if (server.exiting()) {
				status = server.status();
				break;
			}
		}
		const bool closed = static_cast<bool>(server.cleanup());
		const bool stopped = static_cast<bool>(in->cleanup());
		const bool unloaded = static_cast<bool>(plugins.cleanup());
		static_cast<void>(scratch->cleanup());
		return status != 0 ? status : closed && stopped && unloaded ? 0 : 1;
	}

} // namespace fb::lsp::impl

auto main(const int argc, const char* const* const argv) -> int {

	if (argc == 2 && ca::sv::str{argv[1]} == "--version") {
		static_cast<void>(ca::print("fbuildd {}\n", fb::version));
		return ca::flush() ? 0 : 1;
	}
	if (argc != 1) {
		static_cast<void>(ca::print(ca::io::err(), "usage: fbuildd [--version]   (a language server on stdin/stdout)\n"));
		return 2;
	}
	auto memory = fb::memory::make();
	if (!memory)
		return 1;
	const int status = fb::lsp::impl::serve(*memory);
	static_cast<void>(memory->cleanup());
	return status;
}
