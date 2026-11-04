#ifndef fb_plugins_loader_hpp
#define fb_plugins_loader_hpp

#include "ca/dl.hpp"
#include "ca/result.hpp"
#include "core/diagnostic.hpp"
#include "core/memory.hpp"
#include "runtime/interpreter.hpp"

namespace fb {

	/* The plugins an interpreter loaded, in load order; they stay mapped until cleanup(), which
	   runs after the interpreter is done with what they registered. */
	class plugin_set final {

		private:

			using self = fb::plugin_set;

			fb::memory& _memory;
			fb::list<ca::dl::library> _libraries;
			fb::list<ca::sv::str> _paths;
			bool _environment_done;

			[[nodiscard]] auto _load_directory(fb::interpreter& in, ca::sv::str directory) noexcept -> ca::result<void, fb::diagnostic>;

		public:

			explicit plugin_set(fb::memory& memory) noexcept;

			plugin_set(const self&) = delete;

			auto operator=(const self&) -> self& = delete;

			/* Loads the shared object at path and lets it register into in. */
			[[nodiscard]] auto load(fb::interpreter& in, ca::sv::str path) noexcept -> ca::result<void, fb::diagnostic>;

			/* $FBUILD_PLUGINS (':'-separated paths), then every *.so directly in each directory
			   of $FBUILD_PLUGIN_PATH (':'-separated), by name. Once; later calls do nothing. */
			[[nodiscard]] auto load_environment(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic>;

			/* What was loaded, in order. */
			[[nodiscard]] auto paths(void) const noexcept -> ca::borrowed_slice<const ca::sv::str> {
				return _paths.view();
			}

			/* Unloads every plugin, last loaded first (their finalizers run). */
			[[nodiscard]] auto cleanup(void) noexcept -> ca::result<void>;

	}; // class plugin_set

} // namespace fb

#endif // fb_plugins_loader_hpp
