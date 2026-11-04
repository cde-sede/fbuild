#include "runtime/library.hpp"

#include "ca/err.hpp"
#include "ca/flush.hpp"
#include "ca/print.hpp"
#include "ca/process.hpp"
#include "ca/text.hpp"

namespace fb::impl {

	enum class argument_type : ca::u8 {
		boolean,
		integer,
		string,
		rest,
	};

	struct rule final {
		ca::sv::str name;
		ca::sv::str shorthand;
		ca::sv::str longhand;
		ca::sv::str help;
		fb::impl::argument_type type;
		bool present;
		bool flag;
		ca::i64 integer;
		ca::sv::str text;
	};

	[[nodiscard]] static auto lower_equals(const ca::sv::str text, const ca::sv::str word) noexcept -> bool {

		if (text.size() != word.size())
			return false;
		for (ca::usz i = 0U; i < text.size(); ++i) {
			char c = text.data()[i];
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
			if (c != word.data()[i])
				return false;
		}
		return true;
	}

	[[nodiscard]] static auto trim_blanks(ca::sv::str text) noexcept -> ca::sv::str {

		while (!text.empty() && (text.data()[0] == ' ' || text.data()[0] == '\t'))
			text = text.suffix(text.size() - 1U);
		return text;
	}

	[[nodiscard]] static auto parse_int(const ca::sv::str given, ca::i64& out) noexcept -> bool {

		ca::sv::str text = fb::impl::trim_blanks(given);
		while (!text.empty() && (text.data()[text.size() - 1U] == ' ' || text.data()[text.size() - 1U] == '\t'))
			text = text.prefix(text.size() - 1U);
		bool negative = false;
		if (!text.empty() && (text.data()[0] == '+' || text.data()[0] == '-')) {
			negative = text.data()[0] == '-';
			text = text.suffix(text.size() - 1U);
		}
		int base = 10;
		if (text.size() > 2U && text.data()[0] == '0' && (text.data()[1] == 'x' || text.data()[1] == 'X')) {
			base = 16;
			text = text.suffix(text.size() - 2U);
		} else if (text.size() > 2U && text.data()[0] == '0' && (text.data()[1] == 'b' || text.data()[1] == 'B')) {
			base = 2;
			text = text.suffix(text.size() - 2U);
		} else if (text.size() > 1U && text.data()[0] == '0') {
			base = 8;
			text = text.suffix(text.size() - 1U);
		}
		auto parsed = ca::sys::parse_integer<ca::i64>(text, base);
		if (!parsed)
			return false;
		out = negative ? -*parsed : *parsed;
		return true;
	}

	[[nodiscard]] static auto parse_bool(const ca::sv::str given, bool& out) noexcept -> bool {

		const ca::sv::str text = fb::impl::trim_blanks(given);
		for (const ca::sv::str yes : {ca::sv::str{"true"}, ca::sv::str{"yes"}, ca::sv::str{"1"}, ca::sv::str{"on"}})
			if (fb::impl::lower_equals(text, yes)) {
				out = true;
				return true;
			}
		for (const ca::sv::str no : {ca::sv::str{"false"}, ca::sv::str{"no"}, ca::sv::str{"0"}, ca::sv::str{"off"}})
			if (fb::impl::lower_equals(text, no)) {
				out = false;
				return true;
			}
		return false;
	}

	[[nodiscard]] static auto type_name(const fb::impl::argument_type type) noexcept -> ca::sv::str {

		switch (type) {
			case fb::impl::argument_type::integer: return "NUM";
			case fb::impl::argument_type::string: return "STR";
			case fb::impl::argument_type::rest: return "ARG";
			default: return "";
		}
	}

	[[nodiscard]] static auto expected_name(const fb::impl::argument_type type) noexcept -> ca::sv::str {

		switch (type) {
			case fb::impl::argument_type::boolean: return "boolean";
			case fb::impl::argument_type::integer: return "integer";
			default: return "value";
		}
	}

	class argument_parser final {

		private:

			fb::interpreter& _in;
			fb::list<fb::impl::rule> _rules;
			fb::value* _rest;
			bool _error;

			[[nodiscard]] auto _find(const ca::sv::str name) noexcept -> fb::impl::rule* {

				for (fb::impl::rule& r : _rules)
					if ((!r.shorthand.empty() && r.shorthand == name) || (!r.longhand.empty() && r.longhand == name))
						return &r;
				return nullptr;
			}

			[[nodiscard]] auto _find_prefix(const ca::sv::str arg, ca::sv::str& value) noexcept -> fb::impl::rule* {

				for (fb::impl::rule& r : _rules) {
					if (r.type == fb::impl::argument_type::boolean)
						continue;
					for (const ca::sv::str spelling : {r.longhand, r.shorthand})
						if (!spelling.empty() && arg.size() > spelling.size() && arg.starts_with(spelling)) {
							value = arg.suffix(arg.size() - spelling.size());
							return &r;
						}
				}
				return nullptr;
			}

			/* getopt-style -abc: a and b bool short flags, c may take a value (the rest of the
			   cluster, else the next argument). Whether arg was such a cluster, all handled. */
			[[nodiscard]] auto _bundle(const ca::sv::str arg, const fb::value* const argv, const ca::usz count, ca::usz& i) noexcept -> bool {

				if (arg.size() < 3U || arg.data()[0] != '-' || arg.data()[1] == '-')
					return false;
				fb::memory& memory = _in.memory();
				for (ca::usz at = 1U; at < arg.size(); ++at) {
					const fb::impl::rule* const r = this->_find(memory.format("-{}", arg.data()[at]));
					if (r == nullptr)
						return false;
					if (r->type != fb::impl::argument_type::boolean)
						break;
				}
				for (ca::usz at = 1U; at < arg.size(); ++at) {
					fb::impl::rule& r = *this->_find(memory.format("-{}", arg.data()[at]));
					if (r.type == fb::impl::argument_type::boolean) {
						r.present = true;
						r.flag = true;
						continue;
					}
					ca::sv::str value = arg.suffix(arg.size() - at - 1U);
					if (value.starts_with("="))
						value = value.suffix(value.size() - 1U);
					if (!value.empty())
						this->_set(r, r.longhand, value);
					else if (i + 1U < count && !_in.text(argv->at(i + 1U)).starts_with("-"))
						this->_set(r, r.longhand, memory.copy(_in.text(argv->at(++i))));
					else
						this->_report(memory.format("{} requires a value", r.longhand));
					return true;
				}
				return true;
			}

			auto _report(const ca::sv::str message) noexcept -> void {

				static_cast<void>(ca::print(ca::io::err(), "Error: {}\n", message));
				_error = true;
			}

			auto _set(fb::impl::rule& r, const ca::sv::str name, const ca::sv::str value) noexcept -> void {

				r.present = true;
				bool ok = true;
				switch (r.type) {
					case fb::impl::argument_type::boolean:
						ok = fb::impl::parse_bool(value, r.flag);
						break;
					case fb::impl::argument_type::integer:
						ok = fb::impl::parse_int(value, r.integer);
						break;
					default:
						r.text = value;
						break;
				}
				if (!ok)
					this->_report(_in.memory().format("{} expects {}, got '{}'", name, fb::impl::expected_name(r.type), value));
			}

			[[nodiscard]] auto _synopsis(void) noexcept -> ca::sv::str {

				fb::text out{_in.memory().allocator()};
				static_cast<void>(out.append("fbuild"));
				for (const fb::impl::rule& r : _rules) {
					if (r.type == fb::impl::argument_type::rest)
						continue;
					const ca::sv::str name = !r.shorthand.empty() ? r.shorthand : r.longhand;
					const ca::sv::str type = fb::impl::type_name(r.type);
					static_cast<void>(type.empty() ? ca::format_append(out, " [{}]", name) : ca::format_append(out, " [{} {}]", name, type));
				}
				static_cast<void>(out.append(" [rest]"));
				return _in.memory().copy(out.view());
			}

			[[nodiscard]] auto _signature_width(const fb::impl::rule& r) const noexcept -> ca::usz {

				ca::usz width = r.shorthand.size() + r.longhand.size();
				if (!r.shorthand.empty() && !r.longhand.empty())
					width += 2U;
				const ca::sv::str type = fb::impl::type_name(r.type);
				if (!type.empty())
					width += 1U + type.size();
				return width;
			}

			[[nodiscard]] auto _signature(const fb::impl::rule& r) noexcept -> ca::sv::str {

				const ca::sv::str type = fb::impl::type_name(r.type);
				return _in.memory().format("{}{}{}{}{}", r.shorthand, !r.shorthand.empty() && !r.longhand.empty() ? ", " : "", r.longhand,
					type.empty() ? "" : " ", type);
			}

			auto _help(void) noexcept -> void {

				constexpr ca::sv::str spaces = "                                                                ";
				static_cast<void>(ca::print("NAME\n       fbuild\n\nSYNOPSIS\n       {}\n\nOPTIONS\n", this->_synopsis()));
				ca::usz width = 15U;
				for (const fb::impl::rule& r : _rules)
					if (r.type != fb::impl::argument_type::rest && this->_signature_width(r) > width)
						width = this->_signature_width(r);
				width += 2U;
				for (const fb::impl::rule& r : _rules) {
					if (r.type == fb::impl::argument_type::rest)
						continue;
					const ca::sv::str signature = this->_signature(r);
					static_cast<void>(ca::print("       {}{}{}\n", signature, spaces.prefix(width - signature.size()), r.help));
				}
				static_cast<void>(ca::print("       -h, --help [OPT]{}Show help (optionally for a specific option)\n", spaces.prefix(width - 15U)));
			}

			auto _help_for(const ca::sv::str name) noexcept -> void {

				fb::impl::rule* r = this->_find(name);
				if (r == nullptr && !name.starts_with("-")) {
					r = this->_find(_in.memory().format("--{}", name));
					if (r == nullptr)
						r = this->_find(_in.memory().format("-{}", name));
				}
				if (r == nullptr) {
					static_cast<void>(ca::print(ca::io::err(), "Unknown option: {}\n", name));
					return;
				}
				fb::text out{_in.memory().allocator()};
				static_cast<void>(ca::format_append(out, "       {}", r->shorthand));
				if (!r->shorthand.empty() && !r->longhand.empty())
					static_cast<void>(out.append(", "));
				static_cast<void>(out.append(r->longhand));
				const ca::sv::str type = fb::impl::type_name(r->type);
				if (!type.empty())
					static_cast<void>(ca::format_append(out, " {}", type));
				static_cast<void>(out.push('\n'));
				if (!r->help.empty())
					static_cast<void>(ca::format_append(out, "              {}\n", r->help));
				static_cast<void>(ca::print("{}", out.view()));
			}

			[[noreturn]] auto _exit_after_help(void) noexcept -> void {

				static_cast<void>(ca::flush());
				ca::sys::exit(0);
			}

		public:

			explicit argument_parser(fb::interpreter& in) noexcept
			: _in{in}, _rules{in.memory().list<fb::impl::rule>()}, _rest{in.make(fb::kind::list)}, _error{false} {
			}

			auto add(const fb::impl::rule& r) noexcept -> void {
				_in.memory().push(_rules, r);
			}

			[[nodiscard]] auto rules(void) noexcept -> fb::list<fb::impl::rule>& {
				return _rules;
			}

			[[nodiscard]] auto rest(void) const noexcept -> fb::value* {
				return _rest;
			}

			auto parse(const fb::value* const argv) noexcept -> void {

				fb::memory& memory = _in.memory();
				const ca::usz count = argv != nullptr && argv->is_sequence() ? argv->size() : 0U;
				for (ca::usz i = 0U; i < count; ++i) {
					const ca::sv::str arg = memory.copy(_in.text(argv->at(i)));
					if (!arg.starts_with("-")) {
						_in.push(_rest, _in.string_in_place(arg));
						continue;
					}
					auto equals = arg.find('=');
					const ca::sv::str key = equals ? arg.prefix(*equals) : arg;
					ca::optional<ca::sv::str> value;
					if (equals)
						value = arg.suffix(arg.size() - *equals - 1U);
					fb::impl::rule* r = this->_find(key);
					if (r == nullptr && !equals) {
						ca::sv::str prefixed;
						r = this->_find_prefix(arg, prefixed);
						if (r != nullptr) {
							if (prefixed.starts_with("="))
								prefixed = prefixed.suffix(prefixed.size() - 1U);
							value = prefixed;
						}
					}
					if (r == nullptr && !equals && this->_bundle(arg, argv, count, i))
						continue;
					if (r == nullptr) {
						ca::optional<ca::sv::str> topic;
						bool help = false;
						if (key == "-h" || key == "--help") {
							help = true;
							if (value)
								topic = *value;
							else if (i + 1U < count && !_in.text(argv->at(i + 1U)).starts_with("-"))
								topic = memory.copy(_in.text(argv->at(++i)));
						} else if (arg.starts_with("--help") || arg.starts_with("-h")) {
							help = true;
							const ca::usz skip = arg.starts_with("--help") ? 6U : 2U;
							ca::sv::str rest = arg.suffix(arg.size() - skip);
							if (rest.starts_with("="))
								rest = rest.suffix(rest.size() - 1U);
							topic = rest;
						}
						if (help) {
							if (topic)
								this->_help_for(*topic);
							else
								this->_help();
							this->_exit_after_help();
						}
						this->_report(memory.format("Unknown argument '{}'", arg));
						continue;
					}
					if (value) {
						this->_set(*r, r->longhand, *value);
						continue;
					}
					r->present = true;
					if (r->type == fb::impl::argument_type::boolean) {
						r->flag = true;
						continue;
					}
					if (i + 1U >= count || _in.text(argv->at(i + 1U)).starts_with("-")) {
						this->_report(memory.format("{} requires a value", arg));
						continue;
					}
					this->_set(*r, arg, memory.copy(_in.text(argv->at(++i))));
				}
				if (_error)
					static_cast<void>(ca::print(ca::io::err(), "\nUsage: {}\nTry 'fbuild --help' for more information.\n", this->_synopsis()));
			}
	};

} // namespace fb::impl

auto fb::parse_arguments(fb::interpreter& in, fb::value* const schema, fb::value* const argv) noexcept -> fb::outcome {

	if (schema == nullptr || !schema->is(fb::kind::map))
		return in.make(fb::kind::map);
	fb::memory& memory = in.memory();
	fb::impl::argument_parser parser{in};
	const ca::usz count = schema->keys.size();
	for (ca::usz i = 0U; i < count; ++i) {
		const ca::sv::str name = schema->keys.data()[i];
		fb::value* const spec = schema->at(i);
		const bool detailed = spec->is(fb::kind::map);
		ca::sv::str type = detailed ? ca::sv::str{"str"} : in.text(spec);
		fb::impl::rule r{name, "", "", "", fb::impl::argument_type::string, false, false, 0, ""};
		if (detailed) {
			if (fb::value* const v = spec->find("type"); v != nullptr && !v->is(fb::kind::unit))
				type = in.text(v);
			if (fb::value* const v = spec->find("long"); v != nullptr && !v->is(fb::kind::unit))
				r.longhand = memory.copy(in.text(v));
			if (fb::value* const v = spec->find("short"); v != nullptr && !v->is(fb::kind::unit))
				r.shorthand = memory.copy(in.text(v));
			if (fb::value* const v = spec->find("help"); v != nullptr && !v->is(fb::kind::unit))
				r.help = memory.copy(in.text(v));
		}
		if (r.longhand.empty())
			r.longhand = memory.format("--{}", name);
		if (r.shorthand.empty() && !name.empty()) {
			bool taken = name.data()[0] == 'h';
			for (ca::usz j = 0U; j < count && !taken; ++j)
				if (j != i && !schema->keys.data()[j].empty() && schema->keys.data()[j].data()[0] == name.data()[0])
					taken = true;
			if (!taken)
				r.shorthand = memory.format("-{}", name.data()[0]);
		}
		if (type == "bool")
			r.type = fb::impl::argument_type::boolean;
		else if (type == "int" || type == "integer")
			r.type = fb::impl::argument_type::integer;
		parser.add(r);
	}
	parser.parse(argv);
	fb::value* const out = in.make(fb::kind::map);
	for (ca::usz i = 0U; i < count; ++i) {
		const fb::impl::rule& r = parser.rules().data()[i];
		fb::value* v;
		switch (r.type) {
			case fb::impl::argument_type::boolean: v = in.boolean(r.flag); break;
			case fb::impl::argument_type::integer: v = in.integer(r.integer); break;
			default: v = in.string(r.text); break;
		}
		if (!r.present && schema->at(i)->is(fb::kind::map)) {
			fb::value* fallback = in.get(schema->at(i), "default");
			if (fallback->is(fb::kind::lambda)) {
				auto computed = in.apply(fallback, in.unit());
				if (!computed)
					return computed;
				fallback = *computed;
			}
			if (!fallback->is(fb::kind::unit))
				v = fallback;
		}
		in.set(out, schema->keys.data()[i], v);
	}
	in.set(out, "rest", parser.rest());
	return out;
}
