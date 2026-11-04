#include "ca/environment.hpp"
#include "ca/err.hpp"
#include "ca/flush.hpp"
#include "ca/io.hpp"
#include "ca/path.hpp"
#include "ca/print.hpp"
#include "check/catalog.hpp"
#include "check/checker.hpp"
#include "core/interrupt.hpp"
#include "core/memory.hpp"
#include "core/version.hpp"
#include "runtime/host.hpp"
#include "runtime/interpreter.hpp"
#include "plugins/loader.hpp"
#include "runtime/library.hpp"
#include "syntax/dump.hpp"
#include "syntax/parser.hpp"

namespace fb::impl {

	[[nodiscard]] static auto read_source(fb::memory& memory, const ca::sv::str file) noexcept -> ca::optional<ca::sv::str> {

		auto text = fb::path_of(memory, file).read_text();
		if (!text)
			return ca::nullopt;
		return ca::sv::str{text->data(), text->size()};
	}

	[[nodiscard]] static auto find_script(fb::memory& memory) noexcept -> ca::optional<ca::sv::str> {

		for (const ca::sv::str name : {ca::sv::str{".fb"}, ca::sv::str{"Build.fb"}, ca::sv::str{"build.fb"}})
			if (auto exists = fb::path_of(memory, name).exists(); exists && *exists)
				return name;
		ca::optional<ca::sv::str> found;
		int count = 0;
		if (auto scripts = fb::path_of(memory, ".").glob("*.fb"))
			for (const fb::path& script : *scripts)
				if (script.name().size() > 3U) {
					++count;
					found = script.view();
				}
		if (count == 1)
			return found;
		if (count == 0)
			static_cast<void>(ca::print(ca::io::err(), "error: no build script: expected .fb / Build.fb / build.fb or a single *.fb\n"));
		else
			static_cast<void>(ca::print(ca::io::err(), "error: ambiguous: multiple *.fb files; name one .fb / Build.fb / build.fb\n"));
		return ca::nullopt;
	}

	[[nodiscard]] static auto dump_file(fb::memory& memory, const ca::sv::str file) noexcept -> int {

		auto source = fb::impl::read_source(memory, file);
		if (!source) {
			static_cast<void>(ca::print(ca::io::err(), "error: cannot read {}\n", file));
			return 1;
		}
		auto program = fb::parse_source(memory, *source);
		if (!program) {
			static_cast<void>(ca::print(ca::io::err(), "error: {}\n", program.error().message));
			return 1;
		}
		return fb::dump(memory, *program) ? 0 : 1;
	}

	[[nodiscard]] static auto report(const fb::diagnostic& d) noexcept -> int {

		static_cast<void>(ca::flush());
		static_cast<void>(ca::print(ca::io::err(), "error: {}\n", d.message));
		return 1;
	}

	enum class mode : ca::u8 {
		run,         // check, then run when there is no error
		check,       // check only, warnings included
		dump_types,  // print each top-level definition's type and every diagnostic
	};

	/* Checks program against what in has registered; prints errors (and warnings unless
	   running). Whether running may go on. */
	[[nodiscard]] static auto check(fb::memory& memory, const fb::interpreter& in, const fb::node* const program, const ca::sv::str script, const mode m) noexcept
		-> ca::result<bool, fb::diagnostic> {

		fb::check::types types{memory};
		fb::check::catalog catalog{types};
		if (auto described = in.describe(catalog); !described)
			return described.propagate();
		const fb::check::report* const report = fb::check::check(types, catalog, program);
		static_cast<void>(ca::flush());
		if (m == mode::dump_types) {
			for (const fb::check::symbol& s : report->symbols())
				if (s.kind != fb::check::symbol_kind::method)
					static_cast<void>(ca::print("{} : {}\n", s.name, fb::check::render(memory, report->type_of(s.site), true)));
			for (const fb::check::diagnostic& d : report->diagnostics())
				static_cast<void>(ca::print("{} {}:{}: {}\n", d.level == fb::check::severity::error ? "error" : "warning", d.at.start.line, d.at.start.col, d.message));
			return report->errors() == 0U;
		}
		for (const fb::check::diagnostic& d : report->diagnostics())
			if (d.level == fb::check::severity::error || m == mode::check)
				static_cast<void>(ca::print(ca::io::err(), "{}:{}:{}: {}: {}\n", script, d.at.start.line, d.at.start.col,
					d.level == fb::check::severity::error ? "error" : "warning", d.message));
		if (report->errors() != 0U)
			static_cast<void>(ca::print(ca::io::err(), "{} type error{}\n", report->errors(), report->errors() == 1U ? "" : "s"));
		return report->errors() == 0U;
	}

	[[nodiscard]] static auto drive(fb::memory& memory, fb::interpreter& in, fb::plugin_set& plugins, const int argc, const char* const* const argv) noexcept -> int {

		fb::list<ca::sv::str> forwarded = memory.list<ca::sv::str>();
		ca::optional<ca::sv::str> script;
		bool separated = false;
		bool list_plugins = false;
		mode m = mode::run;
		for (int i = 1; i < argc; ++i) {
			const ca::sv::str arg = argv[i];
			if (separated || arg == ":::") {
				separated = true;
				memory.push(forwarded, arg);
				continue;
			}
			if (arg == "--script" || arg == "--plugin") {
				if (i + 1 >= argc) {
					static_cast<void>(ca::print(ca::io::err(), "error:{} needs a path\n", arg));
					return 1;
				}
				const ca::sv::str value{argv[++i]};
				if (arg == "--script")
					script = value;
				else if (auto loaded = plugins.load(in, value); !loaded)
					return fb::impl::report(loaded.error());
				continue;
			}
			if (arg.starts_with("--script=")) {
				script = arg.without_prefix(9U);
				continue;
			}
			if (arg.starts_with("--plugin=")) {
				if (auto loaded = plugins.load(in, arg.without_prefix(9U)); !loaded)
					return fb::impl::report(loaded.error());
				continue;
			}
			if (arg == "--list-plugins") {
				list_plugins = true;
				continue;
			}
			if (arg == "--check" || arg == "--dump-types") {
				m = arg == "--check" ? mode::check : mode::dump_types;
				continue;
			}
			memory.push(forwarded, arg);
		}
		if (auto loaded = plugins.load_environment(in); !loaded)
			return fb::impl::report(loaded.error());
		if (list_plugins) {
			for (const ca::sv::str path : plugins.paths())
				static_cast<void>(ca::print("{}\n", path));
			return 0;
		}
		if (!script)
			if (auto from_env = ca::sys::getenv("FBUILD_SCRIPT"))
				script = *from_env;
		if (!script)
			script = fb::impl::find_script(memory);
		if (!script)
			return 1;
		auto source = fb::impl::read_source(memory, *script);
		if (!source) {
			static_cast<void>(ca::print(ca::io::err(), "error: cannot open '{}'\n", *script));
			return 1;
		}
		fb::bind_host(in, *script, plugins.paths());
		in.set_args(forwarded.view());
		auto program = fb::parse_source(memory, *source);
		if (!program)
			return fb::impl::report(program.error());
		auto checked = fb::impl::check(memory, in, *program, *script, m);
		if (!checked)
			return fb::impl::report(checked.error());
		if (m != mode::run)
			return *checked ? 0 : 1;
		if (!*checked)
			return 1;
		auto ran = in.run(*program);
		if (const int signal = fb::interrupt::pending(); signal != 0) {
			static_cast<void>(ca::flush());
			if (ca::sys::isatty(2))
				static_cast<void>(ca::print(ca::io::err(), "\033[?25h\n\033[33minterrupted by user\033[0m\n"));
			else
				static_cast<void>(ca::print(ca::io::err(), "\ninterrupted by user\n"));
			return 128 + signal;
		}
		if (!ran) {
			if (ran.error().at.line == 0)
				return fb::impl::report(ran.error());
			static_cast<void>(ca::flush());
			static_cast<void>(ca::print(ca::io::err(), "{}:{}:{}: error: {}\n", *script, ran.error().at.line, ran.error().at.col, ran.error().message));
			return 1;
		}
		return in.failed() ? 1 : 0;
	}

	[[nodiscard]] static auto run(fb::memory& memory, const int argc, const char* const* const argv) noexcept -> int {

		if (argc == 2 && ca::sv::str{argv[1]} == "--version") {
			static_cast<void>(ca::print("fbuild {}\n", fb::version));
			return 0;
		}
		if (argc == 3 && ca::sv::str{argv[1]} == "--dump-ast")
			return fb::impl::dump_file(memory, argv[2]);
		fb::interpreter* const in = memory.make<fb::interpreter>(memory);
		fb::plugin_set plugins{memory};
		const int status = fb::impl::drive(memory, *in, plugins, argc, argv);
		const bool stopped = static_cast<bool>(in->cleanup());
		const bool unloaded = static_cast<bool>(plugins.cleanup());
		return status != 0 ? status : stopped && unloaded ? 0 : 1;
	}

} // namespace fb::impl

auto main(const int argc, const char* const* const argv) -> int {

	if (!fb::interrupt::install())
		return 1;
	auto memory = fb::memory::make();
	if (!memory)
		return 1;
	const int status = fb::impl::run(*memory, argc, argv);
	const bool flushed = static_cast<bool>(ca::flush());
	static_cast<void>(memory->cleanup());
	return flushed ? status : 1;
}
