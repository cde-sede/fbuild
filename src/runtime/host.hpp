#ifndef fb_runtime_host_hpp
#define fb_runtime_host_hpp

#include "ca/borrowed_slice.hpp"
#include "ca/sv.hpp"
#include "runtime/interpreter.hpp"

namespace fb {

	/* Binds what the fbuild program gives a script besides the builtins: `script`, the script's
	   path, and `plugins`, the paths of the loaded plugins. The command line and the language
	   server both call it, so a script checks the same in either. */
	auto bind_host(fb::interpreter& in, ca::sv::str script, ca::borrowed_slice<const ca::sv::str> plugins) noexcept -> void;

} // namespace fb

#endif // fb_runtime_host_hpp
