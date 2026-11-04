#include "runtime/host.hpp"

namespace fb::impl {

	static auto not_callable(fb::interpreter& in, const fb::arguments) noexcept -> fb::outcome {
		return in.fail("not callable");
	}

} // namespace fb::impl

auto fb::bind_host(fb::interpreter& in, const ca::sv::str script, const ca::borrowed_slice<const ca::sv::str> plugins) noexcept -> void {

	in.define({"script", fb::impl::not_callable, 0, -1, fb::ret::string, fb::global, "script: path of the running script"});
	in.define({"plugins", fb::impl::not_callable, 0, -1, fb::ret::list_string, fb::global, "plugins: paths of every loaded plugin"});
	in.bind("script", in.string(script));
	fb::value* const loaded = in.make(fb::kind::list);
	for (const ca::sv::str path : plugins)
		in.push(loaded, in.string_in_place(path));
	in.bind("plugins", loaded);
}
