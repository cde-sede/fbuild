#include "runtime/library.hpp"

#include "ca/unicode.hpp"
#include "core/numbers.hpp"

namespace fb::impl {

	class json_reader final {

		private:

			fb::interpreter& _in;
			ca::sv::str _text;
			ca::usz _i;

			[[nodiscard]] auto _at(const ca::usz offset = 0U) const noexcept -> char {
				return _i + offset < _text.size() ? _text.data()[_i + offset] : '\0';
			}

			auto _blanks(void) noexcept -> void {

				while (_at() == ' ' || _at() == '\t' || _at() == '\n' || _at() == '\r')
					++_i;
			}

			[[nodiscard]] static auto _hex(const char c) noexcept -> int {

				if (c >= '0' && c <= '9')
					return c - '0';
				if (c >= 'a' && c <= 'f')
					return c - 'a' + 10;
				if (c >= 'A' && c <= 'F')
					return c - 'A' + 10;
				return -1;
			}

			[[nodiscard]] auto _string(void) noexcept -> ca::sv::str {

				++_i;
				fb::text out{_in.memory().allocator()};
				ca::result<void> appended{};
				while (_at() != '\0' && _at() != '"') {
					char c = _at();
					if (c == '\\') {
						++_i;
						const char e = _at();
						switch (e) {
							case 'n': c = '\n'; break;
							case 't': c = '\t'; break;
							case 'r': c = '\r'; break;
							case 'b': c = '\b'; break;
							case 'f': c = '\f'; break;
							case 'u': {
								char32_t point = 0;
								for (int k = 0; k < 4 && self_hex(_at(1U)) >= 0; ++k) {
									++_i;
									point = point * 16 + static_cast<char32_t>(self_hex(_at()));
								}
								char units[4];
								const ca::usz size = ca::unicode::encode(point, units);
								appended = out.append(ca::sv::str{units, size});
								++_i;
								continue;
							}
							default: c = e; break;
						}
					}
					appended = out.push(c);
					++_i;
				}
				if (_at() == '"')
					++_i;
				static_cast<void>(appended);
				return _in.memory().copy(out.view());
			}

			[[nodiscard]] static auto self_hex(const char c) noexcept -> int {
				return _hex(c);
			}

			[[nodiscard]] auto _word(const ca::sv::str word) const noexcept -> bool {
				return _text.suffix(_text.size() - _i).starts_with(word);
			}

		public:

			json_reader(fb::interpreter& in, const ca::sv::str text) noexcept
			: _in{in}, _text{text}, _i{0U} {
			}

			[[nodiscard]] auto value(void) noexcept -> fb::value* {

				this->_blanks();
				const char c = _at();
				if (c == '{') {
					++_i;
					fb::value* const m = _in.make(fb::kind::map);
					this->_blanks();
					if (_at() == '}') {
						++_i;
						return m;
					}
					for (;;) {
						this->_blanks();
						if (_at() != '"')
							break;
						const ca::sv::str key = this->_string();
						this->_blanks();
						if (_at() == ':')
							++_i;
						_in.set(m, key, this->value());
						this->_blanks();
						if (_at() == ',') {
							++_i;
							continue;
						}
						break;
					}
					this->_blanks();
					if (_at() == '}')
						++_i;
					return m;
				}
				if (c == '[') {
					++_i;
					fb::value* const l = _in.make(fb::kind::list);
					this->_blanks();
					if (_at() == ']') {
						++_i;
						return l;
					}
					for (;;) {
						_in.push(l, this->value());
						this->_blanks();
						if (_at() == ',') {
							++_i;
							continue;
						}
						break;
					}
					this->_blanks();
					if (_at() == ']')
						++_i;
					return l;
				}
				if (c == '"')
					return _in.string_in_place(this->_string());
				if (this->_word("true")) {
					_i += 4U;
					return _in.boolean(true);
				}
				if (this->_word("false")) {
					_i += 5U;
					return _in.boolean(false);
				}
				if (this->_word("null")) {
					_i += 4U;
					return _in.unit();
				}
				if (c == '-' || (c >= '0' && c <= '9')) {
					const ca::usz start = _i;
					bool real = false;
					if (_at() == '-')
						++_i;
					while (_at() >= '0' && _at() <= '9')
						++_i;
					if (_at() == '.') {
						real = true;
						++_i;
						while (_at() >= '0' && _at() <= '9')
							++_i;
					}
					if (_at() == 'e' || _at() == 'E') {
						real = true;
						++_i;
						if (_at() == '+' || _at() == '-')
							++_i;
						while (_at() >= '0' && _at() <= '9')
							++_i;
					}
					const ca::sv::str number = _text.slice(start, _i - start);
					return real ? _in.floating(fb::parse_float_prefix(number)) : _in.integer(fb::parse_integer_prefix(number));
				}
				if (_i < _text.size())
					++_i;
				return _in.unit();
			}
	};

	class json_writer final {

		private:

			fb::interpreter& _in;
			fb::text& _out;
			bool _pretty;

			auto _put(const ca::sv::str text) noexcept -> void {

				if (!_out.append(text)) [[unlikely]]
					ca::trap("fbuild: out of memory");
			}

			auto _indent(const int depth) noexcept -> void {

				this->_put("\n");
				for (int i = 0; i < depth; ++i)
					this->_put("  ");
			}

			auto _string(const ca::sv::str text) noexcept -> void {

				this->_put("\"");
				for (const char c : text) {
					switch (c) {
						case '"': this->_put("\\\""); break;
						case '\\': this->_put("\\\\"); break;
						case '\n': this->_put("\\n"); break;
						case '\t': this->_put("\\t"); break;
						case '\r': this->_put("\\r"); break;
						default: this->_put(ca::sv::str{&c, 1U}); break;
					}
				}
				this->_put("\"");
			}

		public:

			json_writer(fb::interpreter& in, fb::text& out, const bool pretty) noexcept
			: _in{in}, _out{out}, _pretty{pretty} {
			}

			auto write(const fb::value* const v, const int depth) noexcept -> void {

				if (v == nullptr) {
					this->_put("null");
					return;
				}
				switch (v->kind) {
					case fb::kind::integer:
					case fb::kind::floating:
						this->_put(_in.text(v));
						return;
					case fb::kind::boolean:
						this->_put(v->integer != 0 ? "true" : "false");
						return;
					case fb::kind::string:
					case fb::kind::file:
						this->_string(v->text);
						return;
					case fb::kind::list:
					case fb::kind::tuple:
						this->_put("[");
						for (ca::usz i = 0U; i < v->size(); ++i) {
							if (i != 0U)
								this->_put(",");
							if (_pretty)
								this->_indent(depth + 1);
							this->write(v->at(i), depth + 1);
						}
						if (_pretty && v->size() != 0U)
							this->_indent(depth);
						this->_put("]");
						return;
					case fb::kind::map:
						this->_put("{");
						for (ca::usz i = 0U; i < v->keys.size(); ++i) {
							if (i != 0U)
								this->_put(",");
							if (_pretty)
								this->_indent(depth + 1);
							this->_string(v->keys.data()[i]);
							this->_put(_pretty ? ": " : ":");
							this->write(v->at(i), depth + 1);
						}
						if (_pretty && v->keys.size() != 0U)
							this->_indent(depth);
						this->_put("}");
						return;
					default:
						this->_put("null");
						return;
				}
			}
	};

} // namespace fb::impl

auto fb::json_parse(fb::interpreter& in, const ca::sv::str text) noexcept -> fb::value* {

	fb::impl::json_reader reader{in, text};
	return reader.value();
}

auto fb::json_text(fb::interpreter& in, const fb::value* const v, const bool pretty) noexcept -> ca::sv::str {

	fb::text out{in.memory().allocator()};
	fb::impl::json_writer{in, out, pretty}.write(v, 0);
	return in.memory().copy(out.view());
}
