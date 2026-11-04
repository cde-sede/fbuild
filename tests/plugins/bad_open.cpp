// A plugin whose open() fails: fbuild must stop before running the script.
#include "fbuild.hpp"

auto fb::plugin::api(void) noexcept -> ca::u32 {
	return fb::plugin_api;
}

auto fb::plugin::open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {
	return in.fail("needs a configuration it does not have");
}
