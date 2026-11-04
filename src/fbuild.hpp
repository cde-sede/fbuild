#ifndef fb_fbuild_hpp
#define fb_fbuild_hpp

/* The plugin interface. A plugin is a shared object built -fPIC -shared -nostdlib
   -fno-stack-protector against these headers (make plugins does), loaded by ca's own loader
   (ca::dl): what it leaves undefined binds to what fbuild exports (it links -static-pie
   --export-dynamic, with all of libca.a), thread-local variables excepted. So
   a plugin calls the fb:: API (fb::interpreter, fb::value, fb::memory, fb::path ...) and the
   ca:: code directly; a symbol fbuild does not define fails the load, naming it.
   A plugin never touches a ca thread_local itself (ca::io::out(), ca::print to stdout, the
   error trace): print through fb::interpreter::emit.

   A plugin defines exactly these two functions:

       auto fb::plugin::api(void) noexcept -> ca::u32 {
           return fb::plugin_api;
       }

       auto fb::plugin::open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {
           in.define({"shout", shout, 1, 1, fb::ret::string, 0U, "shout(text): text in capitals"});
           return {};
       }

   api() lets fbuild refuse a plugin built against another version of this interface or in
   another ca mode (NDEBUG changes the layout of result and optional). open() registers the
   plugin's builtins, classes and bound values; a failure stops fbuild before the script runs. */

#include "ca/config.hpp"
#include "ca/integers.hpp"
#include "ca/result.hpp"
#include "core/diagnostic.hpp"
#include "core/memory.hpp"
#include "runtime/interpreter.hpp"
#include "runtime/library.hpp"
#include "runtime/value.hpp"

namespace fb {

	/* The interface version a plugin is built against, and ca's mode in the low bit. */
	inline constexpr ca::u32 plugin_api = 2U * 2U + (ca_debug ? 1U : 0U);

	namespace plugin {

		/* fb::plugin_api, as the plugin was built. */
		[[nodiscard]] auto api(void) noexcept -> ca::u32;

		/* Registers what the plugin adds to in. */
		[[nodiscard]] auto open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic>;

	} // namespace plugin

} // namespace fb

#endif // fb_fbuild_hpp
