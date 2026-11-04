// A minimal plugin: one builtin, one class with a native method, one bound value.
#include "fbuild.hpp"

namespace {

	auto shout(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str text = in.text(a[0U]);
		char* const out = static_cast<char*>(in.memory().bytes(text.size() + 1U, 1U));
		for (ca::usz i = 0U; i < text.size(); ++i) {
			const char c = text.data()[i];
			out[i] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
		}
		out[text.size()] = '\0';
		return in.string_in_place(ca::sv::str{out, text.size()});
	}

	auto greet(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string_in_place(in.memory().format("hello, {}", in.text(in.get(a[0U], "name"))));
	}

} // namespace

auto fb::plugin::api(void) noexcept -> ca::u32 {
	return fb::plugin_api;
}

auto fb::plugin::open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {

	in.define({"shout", shout, 1, 1, fb::ret::string, 0U, "shout(text): text in capitals"});
	fb::class_info* const greeter = in.class_named("Greeter");
	in.method(greeter, "greet", greet);
	fb::value* const world = in.object(greeter, nullptr);
	in.set(world, "name", in.string("world"));
	in.define({"world", nullptr, 0, -1, fb::ret::any, fb::global, "world: a Greeter"});
	in.bind("world", world);
	return {};
}
