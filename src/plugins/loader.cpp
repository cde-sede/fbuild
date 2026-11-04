#include "plugins/loader.hpp"

#include "ca/environment.hpp"
#include "ca/report.hpp"
#include "fbuild.hpp"
#include "runtime/library.hpp"

namespace fb::impl {

	/* The mangled names of fb::plugin::api() and fb::plugin::open(fb::interpreter&). */
	inline constexpr ca::sv::str plugin_api_symbol = "_ZN2fb6plugin3apiEv";
	inline constexpr ca::sv::str plugin_open_symbol = "_ZN2fb6plugin4openERNS_11interpreterE";

	using api_function = auto (*)(void) noexcept -> ca::u32;
	using open_function = auto (*)(fb::interpreter&) noexcept -> ca::result<void, fb::diagnostic>;

} // namespace fb::impl

fb::plugin_set::plugin_set(fb::memory& memory) noexcept
: _memory{memory}, _libraries{memory.allocator()}, _paths{memory.allocator()}, _environment_done{false} {
}

auto fb::plugin_set::load(fb::interpreter& in, const ca::sv::str path) noexcept -> ca::result<void, fb::diagnostic> {

	auto loaded = ca::dl::library::make(path);
	if (!loaded) {
		const ca::sv::str why = _memory.format("plugin: {}", loaded.error());
		static_cast<void>(loaded.error().cleanup());
		return ca::failure{fb::diagnostic{why, {0, 0}}};
	}
	ca::dl::library library = ca::move(*loaded);
	auto api = library.find(fb::impl::plugin_api_symbol);
	auto open = library.find(fb::impl::plugin_open_symbol);
	ca::sv::str problem{};
	if (!api || !open)
		problem = _memory.format("plugin '{}': missing fb::plugin::{}", path, !api ? "api()" : "open(fb::interpreter&)");
	else if (const ca::u32 built = reinterpret_cast<fb::impl::api_function>(*api)(); built != fb::plugin_api)
		problem = _memory.format("plugin '{}': built for plugin API {} ({}), this fbuild uses {} ({})", path, built / 2U,
			(built & 1U) != 0U ? "debug" : "release", fb::plugin_api / 2U, ca_debug ? "debug" : "release");
	else {
		auto opened = reinterpret_cast<fb::impl::open_function>(*open)(in);
		if (!opened)
			problem = _memory.format("plugin '{}': {}", path, opened.error().message);
	}
	if (!problem.empty()) {
		static_cast<void>(library.cleanup());
		return ca::failure{fb::diagnostic{problem, {0, 0}}};
	}
	_memory.push(_libraries, ca::move(library));
	_memory.push(_paths, _memory.copy(path));
	return {};
}

auto fb::plugin_set::_load_directory(fb::interpreter& in, const ca::sv::str directory) noexcept -> ca::result<void, fb::diagnostic> {

	const fb::path base = fb::path_of(_memory, directory);
	auto is_directory = base.is_directory();
	if (!is_directory || !*is_directory)
		return ca::failure{fb::diagnostic{_memory.format("FBUILD_PLUGIN_PATH: cannot open '{}'", directory), {0, 0}}};
	auto found = base.glob("*.so");
	if (!found)
		return {};
	fb::list<ca::sv::str> names = _memory.list<ca::sv::str>();
	for (const fb::path& p : *found) {
		_memory.push(names, p.view());
		for (ca::usz i = names.size() - 1U; i != 0U && names.data()[i].compare(names.data()[i - 1U]) < 0; --i) {
			const ca::sv::str moved = names.data()[i];
			names.data()[i] = names.data()[i - 1U];
			names.data()[i - 1U] = moved;
		}
	}
	for (const ca::sv::str name : names) {
		auto loaded = this->load(in, name);
		if (!loaded)
			return loaded;
	}
	return {};
}

auto fb::plugin_set::load_environment(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {

	if (_environment_done)
		return {};
	_environment_done = true;
	if (auto listed = ca::sys::getenv("FBUILD_PLUGINS"))
		for (const ca::sv::str path : listed->split(":"))
			if (!path.empty()) {
				auto loaded = this->load(in, path);
				if (!loaded)
					return loaded;
			}
	if (auto directories = ca::sys::getenv("FBUILD_PLUGIN_PATH"))
		for (const ca::sv::str directory : directories->split(":"))
			if (!directory.empty()) {
				auto loaded = this->_load_directory(in, directory);
				if (!loaded)
					return loaded;
			}
	return {};
}

auto fb::plugin_set::cleanup(void) noexcept -> ca::result<void> {

	ca::result<void> outcome{};
	for (ca::usz i = _libraries.size(); i != 0U; --i) {
		auto released = _libraries.data()[i - 1U].cleanup();
		if (!released && outcome)
			outcome = released;
	}
	static_cast<void>(_libraries.clear());
	return outcome;
}
