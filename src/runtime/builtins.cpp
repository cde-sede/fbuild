#include "runtime/library.hpp"

#include "ca/environment.hpp"
#include "ca/error.hpp"
#include "ca/file.hpp"
#include "ca/path.hpp"
#include "ca/time.hpp"
#include "core/numbers.hpp"

namespace fb::impl {

	[[nodiscard]] static auto c_string(fb::interpreter& in, const ca::sv::str text) noexcept -> const char* {
		return in.memory().copy(text).data();
	}

	[[nodiscard]] static auto system_error(fb::interpreter& in, const ca::sv::str what, const ca::sv::str path, const ca::error e) noexcept -> ca::failure<fb::diagnostic> {
		return in.fail("{} '{}': {}", what, path, ca::sys::error_message(e));
	}

	[[nodiscard]] static auto read_file(fb::interpreter& in, const ca::sv::str path) noexcept -> ca::optional<ca::sv::str> {

		auto text = fb::path_of(in.memory(), path).read_text();
		if (!text)
			return ca::nullopt;
		return ca::sv::str{text->data(), text->size()};
	}

	[[nodiscard]] static auto write_file(fb::interpreter& in, const ca::sv::str path, const ca::sv::str content) noexcept -> ca::result<void, fb::diagnostic> {

		auto written = fb::path_of(in.memory(), path).write_text(content);
		if (!written)
			return fb::impl::system_error(in, "write", path, written.error());
		return {};
	}

	[[nodiscard]] static auto copy_file(fb::interpreter& in, const ca::sv::str from, const ca::sv::str to) noexcept -> ca::result<void> {

		const fb::path source = fb::path_of(in.memory(), from);
		const fb::path target = fb::path_of(in.memory(), to);
		auto status = source.stat();
		if (!status)
			return status.propagate();
		auto bytes = source.read_bytes();
		if (!bytes)
			return bytes.propagate();
		auto written = target.write_bytes(bytes->view());
		if (!written)
			return written;
		return target.chmod(status->permissions() & 0777U);
	}

	[[nodiscard]] static auto split_blanks(fb::interpreter& in, const ca::sv::str text) noexcept -> fb::value* {

		fb::value* const out = in.make(fb::kind::list);
		for (const ca::sv::str word : text.split())
			in.push(out, in.string(word));
		return out;
	}

	[[nodiscard]] static auto command_of(fb::interpreter& in, const ca::sv::str a, const ca::sv::str b, const ca::sv::str c) noexcept -> fb::value* {

		fb::value* const command = in.make(fb::kind::command);
		for (const ca::sv::str word : {a, b, c})
			in.push(command, in.string(word));
		return command;
	}

	// -- io / process ---------------------------------------------------------

	static auto env(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		auto v = ca::sys::getenv(fb::impl::c_string(in, in.text(a[0U])));
		return v ? in.string(*v) : in.unit();
	}

	static auto shell(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.capture(a[0U]);
	}

	static auto which(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str name = in.text(a[0U]);
		if (name.contains('/'))
			return ca::sys::access(fb::impl::c_string(in, name), ca::sys::access_checks::execute) ? in.file(name) : in.string("");
		auto path = ca::sys::getenv("PATH");
		if (!path)
			return in.string("");
		for (const ca::sv::str directory : path->split(":")) {
			if (directory.empty())
				continue;
			const ca::sv::str full = in.memory().format("{}/{}", directory, name);
			if (ca::sys::access(full.data(), ca::sys::access_checks::execute))
				return in.file_in_place(full);
		}
		return in.string("");
	}

	static auto pkgconfig(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str name = in.text(a[0U]);
		fb::value* const m = in.make(fb::kind::map);
		auto exists = in.capture(fb::impl::command_of(in, "pkg-config", "--exists", name));
		if (!exists)
			return exists;
		const bool found = !in.failed_last_capture();
		in.set(m, "found", in.boolean(found));
		if (!found) {
			in.set(m, "cflags", in.make(fb::kind::list));
			in.set(m, "libs", in.make(fb::kind::list));
			in.set(m, "version", in.string(""));
			return m;
		}
		constexpr ca::sv::str fields[] = {"cflags", "libs", "version"};
		constexpr ca::sv::str flags[] = {"--cflags", "--libs", "--modversion"};
		for (ca::usz i = 0U; i < 3U; ++i) {
			auto out = in.capture(fb::impl::command_of(in, "pkg-config", flags[i], name));
			if (!out)
				return out;
			ca::sv::str text = (*out)->text;
			while (!text.empty() && (text.data()[text.size() - 1U] == ' ' || text.data()[text.size() - 1U] == '\t'))
				text = text.prefix(text.size() - 1U);
			in.set(m, fields[i], i < 2U ? fb::impl::split_blanks(in, text) : in.string(text));
		}
		return m;
	}

	static auto run(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* target = a[0U];
		if (a.size() != 1U) {
			target = in.make(fb::kind::tuple);
			for (fb::value* const v : a)
				in.push(target, v);
		}
		auto ran = in.execute(target);
		if (!ran)
			return ran.propagate();
		return in.boolean(*ran);
	}

	static auto print_values(fb::interpreter& in, const fb::arguments a, const bool typed) noexcept -> fb::outcome {

		fb::text line{in.memory().allocator()};
		for (ca::usz i = 0U; i < a.size(); ++i) {
			if (i != 0U)
				static_cast<void>(line.push(' '));
			static_cast<void>(line.append(in.repr(a[i], typed)));
		}
		in.emit(in.memory().copy(line.view()));
		return in.unit();
	}

	static auto print(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return fb::impl::print_values(in, a, false);
	}

	static auto debug(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return fb::impl::print_values(in, a, true);
	}

	static auto config(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const fb::value* const m = a[0U];
		if (!m->is(fb::kind::map))
			return in.unit();
		fb::run_settings& s = in.settings();
		const fb::value* v;
		if ((v = m->find("jobs")) != nullptr && v->is(fb::kind::integer))
			s.jobs = static_cast<int>(v->integer);
		if ((v = m->find("verbose")) != nullptr && !v->is(fb::kind::unit))
			s.verbose = v->truthy();
		if ((v = m->find("keep_going")) != nullptr && !v->is(fb::kind::unit))
			s.keep_going = v->truthy();
		if ((v = m->find("silent")) != nullptr && !v->is(fb::kind::unit))
			s.silent = v->truthy();
		if ((v = m->find("color")) != nullptr && !v->is(fb::kind::unit))
			s.color = v->truthy() ? 1 : 2;
		if ((v = m->find("spinner_ms")) != nullptr && v->is(fb::kind::integer))
			s.spinner_ms = v->integer;
		if ((v = m->find("tail_lines")) != nullptr && v->is(fb::kind::integer))
			s.tail_lines = static_cast<int>(v->integer);
		if (fb::value* const spinner = m->find("spinner"); spinner != nullptr && spinner->is(fb::kind::lambda))
			s.spinner = spinner;
		return in.unit();
	}

	// -- time -------------------------------------------------------------------

	static auto clock(fb::interpreter& in, const fb::arguments) noexcept -> fb::outcome {
		return in.integer(ca::sys::now(ca::sys::clock::monotonic));
	}

	static auto time(fb::interpreter& in, const fb::arguments) noexcept -> fb::outcome {
		return in.integer(ca::sys::now(ca::sys::clock::realtime) / 1000000000);
	}

	static auto sleep(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (a[0U]->is(fb::kind::integer) && a[0U]->integer > 0)
			static_cast<void>(ca::sys::sleep_for(a[0U]->integer * 1000000));
		return in.unit();
	}

	static auto bench(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		auto label = in.force(a[0U]);
		if (!label)
			return label;
		const ca::i64 start = ca::sys::now(ca::sys::clock::monotonic);
		auto r = in.force(a[1U]);
		const ca::i64 end = ca::sys::now(ca::sys::clock::monotonic);
		if (!r)
			return r;
		if (in.bench()) {
			const ca::i64 micro = (end - start) / 1000;
			in.emit(in.memory().format("[bench] {}: {}.{:03} ms", in.text(*label), micro / 1000, micro % 1000));
		}
		return r;
	}

	// -- filesystem -------------------------------------------------------------

	static auto glob(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return fb::glob(in, in.text(a[0U]));
	}

	static auto exists(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		auto found = fb::path_of(in.memory(), a[0U]->path()).exists();
		return in.boolean(found && *found);
	}

	static auto mkdir(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str path = a[0U]->path();
		auto made = fb::path_of(in.memory(), path).mkdirs(0755U);
		if (!made)
			return fb::impl::system_error(in, "mkdir", path, made.error());
		return in.file(path);
	}

	static auto read(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		auto text = fb::impl::read_file(in, a[0U]->path());
		return in.string_in_place(text ? *text : ca::sv::str{""});
	}

	static auto write(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str path = a[0U]->path();
		auto written = fb::impl::write_file(in, path, in.text(a[1U]));
		if (!written)
			return written.propagate();
		return a[0U]->is(fb::kind::file) ? a[0U] : in.file(path);
	}

	static auto copy(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str from = a[0U]->path();
		const ca::sv::str to = a[1U]->path();
		auto copied = fb::impl::copy_file(in, from, to);
		if (!copied)
			return in.fail("copy '{}' -> '{}': {}", from, to, ca::sys::error_message(copied.error()));
		return in.file(to);
	}

	static auto stale(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.boolean(fb::stale(in, a));
	}

	// -- lists ------------------------------------------------------------------

	static auto zip(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		ca::usz shortest = ca::limits<ca::usz>::max();
		for (const fb::value* const v : a) {
			if (!v->is_sequence())
				return out;
			if (v->size() < shortest)
				shortest = v->size();
		}
		for (ca::usz i = 0U; i < shortest; ++i) {
			fb::value* const t = in.make(fb::kind::tuple);
			for (const fb::value* const v : a)
				in.push(t, v->at(i));
			in.push(out, t);
		}
		return out;
	}

	static auto filter(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (!a[0U]->is_sequence())
			return out;
		for (fb::value* const item : a[0U]->items) {
			auto keep = in.apply(a[1U], item);
			if (!keep)
				return keep;
			if ((*keep)->truthy())
				in.push(out, item);
		}
		return out;
	}

	static auto contains(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (!a[0U]->is_sequence())
			return in.boolean(false);
		const ca::sv::str wanted = in.text(a[1U]);
		for (const fb::value* const item : a[0U]->items)
			if (in.text(item) == wanted)
				return in.boolean(true);
		return in.boolean(false);
	}

	static auto concat(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		for (fb::value* const v : a) {
			if (v->is_sequence())
				for (fb::value* const item : v->items)
					in.push(out, item);
			else if (!v->is(fb::kind::unit))
				in.push(out, v);
		}
		return out;
	}

	static auto length(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.integer(a[0U]->is_sequence() ? static_cast<ca::i64>(a[0U]->size()) : 0);
	}

	static auto first(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return a[0U]->is_sequence() && a[0U]->size() != 0U ? a[0U]->at(0U) : in.unit();
	}

	static auto last(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return a[0U]->is_sequence() && a[0U]->size() != 0U ? a[0U]->at(a[0U]->size() - 1U) : in.unit();
	}

	// -- strings ----------------------------------------------------------------

	static auto cat(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::text out{in.memory().allocator()};
		for (const fb::value* const v : a)
			static_cast<void>(out.append(in.text(v)));
		return in.string(out.view());
	}

	static auto join(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (!a[0U]->is_sequence())
			return in.string("");
		const ca::sv::str separator = in.text(a[1U]);
		fb::text out{in.memory().allocator()};
		for (ca::usz i = 0U; i < a[0U]->size(); ++i) {
			if (i != 0U)
				static_cast<void>(out.append(separator));
			static_cast<void>(out.append(in.text(a[0U]->at(i))));
		}
		return in.string(out.view());
	}

	static auto split(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str text = in.text(a[0U]);
		const ca::sv::str separator = in.text(a[1U]);
		fb::value* const out = in.make(fb::kind::list);
		if (separator.empty()) {
			in.push(out, in.string(text));
			return out;
		}
		for (const ca::sv::str piece : text.split(separator))
			in.push(out, in.string(piece));
		return out;
	}

	static auto subst(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string_in_place(fb::substitute(in.memory(), in.text(a[1U]), in.text(a[2U]), in.text(a[0U])));
	}

	static auto to_int(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (a[0U]->is(fb::kind::floating))
			return in.integer(static_cast<ca::i64>(a[0U]->floating));
		return in.integer(fb::parse_integer_prefix(in.text(a[0U])));
	}

	static auto to_str(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string(in.text(a[0U]));
	}

	static auto to_float(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.floating(fb::parse_float_prefix(in.text(a[0U])));
	}

	// -- paths ------------------------------------------------------------------

	static auto parent(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string_in_place(fb::path_parent(in.memory(), a[0U]->path()));
	}

	static auto name(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string_in_place(fb::path_name(in.memory(), a[0U]->path()));
	}

	static auto stem(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string_in_place(fb::path_stem(in.memory(), a[0U]->path()));
	}

	static auto suffix(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string_in_place(fb::path_suffix(in.memory(), a[0U]->path()));
	}

	static auto with_suffix(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const fb::path from = fb::path_of(in.memory(), a[0U]->path());
		const ca::sv::str extension = in.text(a[1U]);
		auto changed = from.with_suffix(extension.starts_with(".") ? extension : in.memory().format(".{}", extension));
		return changed ? in.file_in_place(changed->view()) : in.file(from.view());
	}

	static auto with_name(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const fb::path from = fb::path_of(in.memory(), a[0U]->path());
		auto changed = from.with_name(in.text(a[1U]));
		return changed ? in.file_in_place(changed->view()) : in.file(from.view());
	}

	static auto path(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::path joined{in.memory().allocator()};
		for (const fb::value* const v : a) {
			const ca::sv::str part = in.text(v);
			if (!part.empty() && !joined.push(part)) [[unlikely]]
				return in.fail("path: '{}' is not a path", part);
		}
		return in.file_in_place(joined.view());
	}

	static auto abs(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const fb::path from = fb::path_of(in.memory(), a[0U]->path());
		auto resolved = from.resolve();
		return in.file_in_place(resolved ? resolved->view() : from.view());
	}

	// -- json / maps ------------------------------------------------------------

	static auto json_parse(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return fb::json_parse(in, in.text(a[0U]));
	}

	static auto json_read(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		auto text = fb::impl::read_file(in, a[0U]->path());
		return fb::json_parse(in, text ? *text : ca::sv::str{""});
	}

	static auto json_str(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string_in_place(fb::json_text(in, a[0U], false));
	}

	static auto json_pretty(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string_in_place(fb::json_text(in, a[0U], true));
	}

	static auto json_write(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str path = a[0U]->path();
		auto written = fb::impl::write_file(in, path, fb::json_text(in, a[1U], false));
		if (!written)
			return written.propagate();
		return a[0U]->is(fb::kind::file) ? a[0U] : in.file(path);
	}

	static auto get(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.get(a[0U], in.text(a[1U]));
	}

	static auto has(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.boolean(a[0U]->is(fb::kind::map) && a[0U]->find(in.text(a[1U])) != nullptr);
	}

	static auto keys(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (a[0U]->is(fb::kind::map))
			for (const ca::sv::str key : a[0U]->keys)
				in.push(out, in.string_in_place(key));
		return out;
	}

	static auto values(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (a[0U]->is(fb::kind::map))
			for (fb::value* const v : a[0U]->items)
				in.push(out, v);
		return out;
	}

	static auto items(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (a[0U]->is(fb::kind::map))
			for (ca::usz i = 0U; i < a[0U]->keys.size(); ++i) {
				fb::value* const pair = in.make(fb::kind::tuple);
				in.push(pair, in.string_in_place(a[0U]->keys.data()[i]));
				in.push(pair, a[0U]->at(i));
				in.push(out, pair);
			}
		return out;
	}

	// -- cli arguments ----------------------------------------------------------

	static auto flag(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		for (const fb::value* const wanted : a) {
			const ca::sv::str name = in.text(wanted);
			for (const fb::value* const given : in.args()->items)
				if (in.text(given) == name)
					return in.boolean(true);
		}
		return in.boolean(false);
	}

	static auto opt(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str name = in.text(a[0U]);
		const fb::value* const args = in.args();
		for (ca::usz i = 0U; i < args->size(); ++i) {
			const ca::sv::str current = in.text(args->at(i));
			if (current == name) {
				if (i + 1U < args->size())
					return in.string(in.text(args->at(i + 1U)));
				break;
			}
			if (current.size() > name.size() && current.starts_with(name) && current.data()[name.size()] == '=')
				return in.string(current.suffix(current.size() - name.size() - 1U));
		}
		if (a[1U]->is(fb::kind::lambda))
			return in.apply(a[1U], in.unit());
		return a[1U];
	}

	static auto positionals(fb::interpreter& in, const fb::arguments) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		for (const fb::value* const given : in.args()->items) {
			const ca::sv::str text = in.text(given);
			if (!text.starts_with("-"))
				in.push(out, in.string(text));
		}
		return out;
	}

	/* fail(message...): stops the script with the message (the values' text, joined by spaces). */
	static auto fail(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::text out{in.memory().allocator()};
		for (ca::usz i = 0U; i < a.size(); ++i) {
			if (i != 0U)
				static_cast<void>(out.append(" "));
			static_cast<void>(out.append(in.text(a[i])));
		}
		return in.fail("{}", out.empty() ? ca::sv::str{"fail()"} : out.view());
	}

	/* assert(cond, message?): fails with message (or "assertion failed") unless cond holds. */
	static auto assert_(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (a[0U]->truthy())
			return in.unit();
		return in.fail("{}", a.size() > 1U ? in.text(a[1U]) : ca::sv::str{"assertion failed"});
	}

	static auto not_callable(fb::interpreter& in, const fb::arguments) noexcept -> fb::outcome {
		return in.fail("not callable");
	}

} // namespace fb::impl

auto fb::open_core(fb::interpreter& in) noexcept -> void {

	using fb::ret;
	namespace i = fb::impl;
	const fb::builtin core[] = {
		{"env", i::env, 1, 1, ret::string, 0U, "env(name): the environment variable, or () when unset", "String -> String"},
		{"shell", i::shell, 1, 1, ret::any, 0U, "shell(cmd): run silently, capture stdout", "Command|List|Tuple|String -> ?"},
		{"which", i::which, 1, 1, ret::file, 0U, "which(name): the executable's path in $PATH, or \"\"", "String -> File"},
		{"pkgconfig", i::pkgconfig, 1, 1, ret::map, 0U, "pkgconfig(name): { found, cflags, libs, version }", "String -> {found: Bool, cflags: List<String>, libs: List<String>, version: String}"},
		{"run", i::run, 1, -1, ret::boolean, 0U, "run(targets...): run commands; whether there were any", "...? -> Bool"},
		{"fail", i::fail, 0, -1, ret::any, fb::located, "fail(message...): stop the script with the message", "...? -> Unit"},
		{"assert", i::assert_, 1, 2, ret::unit, fb::located, "assert(cond, message?): fail unless cond holds", "?, ? -> Unit"},
		{"print", i::print, 0, -1, ret::unit, 0U, "print(values...): print values", "...? -> Unit"},
		{"debug", i::debug, 0, -1, ret::unit, 0U, "debug(values...): print values with their types", "...? -> Unit"},
		{"config", i::config, 1, 1, ret::unit, 0U, "config(map): runner settings", "Map -> Unit"},
		{"clock", i::clock, 0, 0, ret::integer, 0U, "clock(): monotonic nanoseconds", "() -> Int"},
		{"time", i::time, 0, 0, ret::integer, 0U, "time(): unix epoch seconds", "() -> Int"},
		{"sleep", i::sleep, 1, 1, ret::unit, 0U, "sleep(ms): block for ms milliseconds", "Int -> Unit"},
		{"bench", i::bench, 2, 2, ret::any, fb::lazy, "bench(label, expr): time expr (printed with --bench)", "String, a -> a"},
		{"glob", i::glob, 1, 1, ret::list, 0U, "glob(pattern): matching files", "String|File -> List<File>"},
		{"exists", i::exists, 1, 1, ret::boolean, 0U, "exists(path)", "Path -> Bool"},
		{"mkdir", i::mkdir, 1, 1, ret::file, 0U, "mkdir(path): mkdir -p", "Path -> File"},
		{"read", i::read, 1, 1, ret::string, 0U, "read(path): file contents, \"\" when missing", "Path -> String"},
		{"write", i::write, 2, 2, ret::file, 0U, "write(path, text)", "Path, ? -> File"},
		{"copy", i::copy, 2, 2, ret::file, 0U, "copy(from, to): keeps the mode", "Path, Path -> File"},
		{"stale", i::stale, 1, -1, ret::boolean, 0U, "stale(target, deps...): whether target needs rebuilding", "Path, ...? -> Bool"},
		{"zip", i::zip, 2, -1, ret::list, 0U, "zip(seqs...): tuples, shortest length"},
		{"filter", i::filter, 2, 2, ret::list, 0U, "filter(list, predicate)", "List<a>, (a -> ?) -> List<a>"},
		{"contains", i::contains, 2, 2, ret::boolean, 0U, "contains(list, value)", "List, ? -> Bool"},
		{"concat", i::concat, 0, -1, ret::list, 0U, "concat(lists...)"},
		{"length", i::length, 1, 1, ret::integer, 0U, "length(list)", "? -> Int"},
		{"first", i::first, 1, 1, ret::any, 0U, "first(list)", "List<a> -> a"},
		{"last", i::last, 1, 1, ret::any, 0U, "last(list)", "List<a> -> a"},
		{"cat", i::cat, 0, -1, ret::string, 0U, "cat(strings...)", "...? -> String"},
		{"join", i::join, 2, 2, ret::string, 0U, "join(list, separator)", "List, ? -> String"},
		{"split", i::split, 2, 2, ret::list, 0U, "split(text, separator)", "String, String -> List<String>"},
		{"subst", i::subst, 3, 3, ret::string, 0U, "subst(text, from, to): glob rewrite", "Path, String, String -> String"},
		{"to_int", i::to_int, 1, 1, ret::integer, 0U, "to_int(value)", "? -> Int"},
		{"to_str", i::to_str, 1, 1, ret::string, 0U, "to_str(value)", "? -> String"},
		{"to_float", i::to_float, 1, 1, ret::floating, 0U, "to_float(value)", "? -> Float"},
		{"parent", i::parent, 1, 1, ret::string, 0U, "parent(path)", "Path -> String"},
		{"name", i::name, 1, 1, ret::string, 0U, "name(path)", "Path -> String"},
		{"stem", i::stem, 1, 1, ret::string, 0U, "stem(path)", "Path -> String"},
		{"suffix", i::suffix, 1, 1, ret::string, 0U, "suffix(path)", "Path -> String"},
		{"with_suffix", i::with_suffix, 2, 2, ret::file, 0U, "with_suffix(path, suffix)", "Path, String -> File"},
		{"with_name", i::with_name, 2, 2, ret::file, 0U, "with_name(path, name)", "Path, String -> File"},
		{"path", i::path, 0, -1, ret::file, 0U, "path(parts...): joined with /", "...? -> File"},
		{"abs", i::abs, 1, 1, ret::file, 0U, "abs(path): realpath", "Path -> File"},
		{"json_parse", i::json_parse, 1, 1, ret::any, 0U, "json_parse(text)", "String -> Json"},
		{"json_read", i::json_read, 1, 1, ret::any, 0U, "json_read(path)", "Path -> Json"},
		{"json_str", i::json_str, 1, 1, ret::string, 0U, "json_str(value): compact JSON", "? -> String"},
		{"json_pretty", i::json_pretty, 1, 1, ret::string, 0U, "json_pretty(value): indented JSON", "? -> String"},
		{"json_write", i::json_write, 2, 2, ret::file, 0U, "json_write(path, value)", "Path, ? -> File"},
		{"get", i::get, 2, 2, ret::any, 0U, "get(map, key): the value, or ()", "?, ? -> Json"},
		{"has", i::has, 2, 2, ret::boolean, 0U, "has(map, key)", "?, ? -> Bool"},
		{"keys", i::keys, 1, 1, ret::list, 0U, "keys(map)", "? -> List<String>"},
		{"values", i::values, 1, 1, ret::list, 0U, "values(map)", "? -> List"},
		{"items", i::items, 1, 1, ret::list, 0U, "items(map): (key, value) tuples", "? -> List<Tuple<String, Json>>"},
		{"flag", i::flag, 1, -1, ret::boolean, 0U, "flag(names...): any present in args", "...String -> Bool"},
		{"opt", i::opt, 2, 2, ret::string, 0U, "opt(name, default): `name v` or `name=v`", "String, ? -> String"},
		{"positionals", i::positionals, 0, 0, ret::list, 0U, "positionals(): args not starting with -", "() -> List<String>"},
		{"args", i::not_callable, 0, -1, ret::list_string, fb::global, "args: List<String>"},
		{"cwd", i::not_callable, 0, -1, ret::string, fb::global, "cwd: String"},
		{"_GLOBALS", i::not_callable, 0, -1, ret::any, fb::global, "_GLOBALS: the interpreter's mutable store"},
		{"_SHARED", i::not_callable, 0, -1, ret::any, fb::global, "_SHARED: the process's mutable store"},
	};
	for (const fb::builtin& b : core)
		in.define(b);
}
