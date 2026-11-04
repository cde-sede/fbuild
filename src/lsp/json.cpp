#include "lsp/json.hpp"

#include "ca/format_append.hpp"
#include "ca/unicode.hpp"
#include "core/numbers.hpp"

namespace fb::lsp::impl {

	class json_reader final {

		private:

			using self = fb::lsp::impl::json_reader;

			fb::memory& _memory;
			ca::sv::str _text;
			ca::usz _i;
			bool _failed;

			[[nodiscard]] auto _at(const ca::usz offset = 0U) const noexcept -> char {
				return _i + offset < _text.size() ? _text.data()[_i + offset] : '\0';
			}

			auto _blanks(void) noexcept -> void {

				while (_at() == ' ' || _at() == '\t' || _at() == '\n' || _at() == '\r')
					++_i;
			}

			[[nodiscard]] auto _fail(void) noexcept -> fb::lsp::json* {

				_failed = true;
				return nullptr;
			}

			[[nodiscard]] auto _make(const fb::lsp::json_kind kind) noexcept -> fb::lsp::json* {
				return _memory.make<fb::lsp::json>(kind, false, 0.0, ca::sv::str{}, _memory.list<ca::sv::str>(), _memory.list<fb::lsp::json*>());
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

			/* The four hex digits after "\u"; -1 when they are not. */
			[[nodiscard]] auto _unit(void) noexcept -> ca::i32 {

				ca::i32 unit = 0;
				for (ca::usz k = 0U; k < 4U; ++k) {
					const int digit = self::_hex(_at(k));
					if (digit < 0)
						return -1;
					unit = unit * 16 + digit;
				}
				_i += 4U;
				return unit;
			}

			[[nodiscard]] auto _string(void) noexcept -> ca::optional<ca::sv::str> {

				++_i;
				fb::text out{_memory.allocator()};
				const auto put = [&](const ca::sv::str piece) noexcept {
					if (!out.append(piece)) [[unlikely]]
						ca::trap("fbuildd: out of memory");
				};
				for (;;) {
					const char c = _at();
					if (_i >= _text.size())
						return ca::nullopt;
					++_i;
					if (c == '"')
						break;
					if (c != '\\') {
						put(ca::sv::str{&c, 1U});
						continue;
					}
					const char e = _at();
					++_i;
					switch (e) {
						case 'n': put("\n"); break;
						case 't': put("\t"); break;
						case 'r': put("\r"); break;
						case 'b': put("\b"); break;
						case 'f': put("\f"); break;
						case '"': put("\""); break;
						case '\\': put("\\"); break;
						case '/': put("/"); break;
						case 'u': {
							ca::i32 unit = this->_unit();
							if (unit < 0)
								return ca::nullopt;
							char32_t point = static_cast<char32_t>(unit);
							// A surrogate pair stands for one code point past the BMP.
							if (unit >= 0xD800 && unit < 0xDC00 && _at() == '\\' && _at(1U) == 'u') {
								_i += 2U;
								const ca::i32 low = this->_unit();
								if (low < 0xDC00 || low >= 0xE000)
									return ca::nullopt;
								point = 0x10000U + ((static_cast<char32_t>(unit) - 0xD800U) << 10U) + (static_cast<char32_t>(low) - 0xDC00U);
							}
							char units[4];
							const ca::usz size = ca::unicode::encode(point, units);
							put(ca::sv::str{units, size});
							break;
						}
						default:
							return ca::nullopt;
					}
				}
				return _memory.copy(out.view());
			}

			[[nodiscard]] auto _word(const ca::sv::str word) noexcept -> bool {

				if (!_text.without_prefix(_i).starts_with(word))
					return false;
				_i += word.size();
				return true;
			}

		public:

			json_reader(fb::memory& memory, const ca::sv::str text) noexcept
			: _memory{memory}, _text{text}, _i{0U}, _failed{false} {
			}

			[[nodiscard]] auto failed(void) const noexcept -> bool {
				return _failed;
			}

			[[nodiscard]] auto at_end(void) noexcept -> bool {

				this->_blanks();
				return _i >= _text.size();
			}

			[[nodiscard]] auto value(const int depth) noexcept -> fb::lsp::json* {

				if (depth > 256)
					return this->_fail();
				this->_blanks();
				const char c = _at();
				if (c == '"') {
					auto text = this->_string();
					if (!text)
						return this->_fail();
					fb::lsp::json* const v = this->_make(fb::lsp::json_kind::string);
					v->text = *text;
					return v;
				}
				if (c == '{') {
					++_i;
					fb::lsp::json* const v = this->_make(fb::lsp::json_kind::object);
					this->_blanks();
					if (_at() == '}') {
						++_i;
						return v;
					}
					for (;;) {
						this->_blanks();
						if (_at() != '"')
							return this->_fail();
						auto key = this->_string();
						if (!key)
							return this->_fail();
						this->_blanks();
						if (_at() != ':')
							return this->_fail();
						++_i;
						fb::lsp::json* const item = this->value(depth + 1);
						if (item == nullptr)
							return nullptr;
						_memory.push(v->keys, *key);
						_memory.push(v->items, item);
						this->_blanks();
						if (_at() == ',') {
							++_i;
							continue;
						}
						if (_at() != '}')
							return this->_fail();
						++_i;
						return v;
					}
				}
				if (c == '[') {
					++_i;
					fb::lsp::json* const v = this->_make(fb::lsp::json_kind::array);
					this->_blanks();
					if (_at() == ']') {
						++_i;
						return v;
					}
					for (;;) {
						fb::lsp::json* const item = this->value(depth + 1);
						if (item == nullptr)
							return nullptr;
						_memory.push(v->items, item);
						this->_blanks();
						if (_at() == ',') {
							++_i;
							continue;
						}
						if (_at() != ']')
							return this->_fail();
						++_i;
						return v;
					}
				}
				if (const bool yes = this->_word("true"); yes || this->_word("false")) {
					fb::lsp::json* const v = this->_make(fb::lsp::json_kind::boolean);
					v->truth = yes;
					return v;
				}
				if (this->_word("null"))
					return this->_make(fb::lsp::json_kind::null);
				const ca::usz start = _i;
				if (_at() == '-')
					++_i;
				while ((_at() >= '0' && _at() <= '9') || _at() == '.' || _at() == 'e' || _at() == 'E' || _at() == '+' || _at() == '-')
					++_i;
				if (_i == start)
					return this->_fail();
				fb::lsp::json* const v = this->_make(fb::lsp::json_kind::number);
				v->number = fb::parse_float_prefix(_text.slice(start, _i - start));
				return v;
			}
	};

} // namespace fb::lsp::impl

auto fb::lsp::parse_json(fb::memory& memory, const ca::sv::str text) noexcept -> const fb::lsp::json* {

	fb::lsp::impl::json_reader reader{memory, text};
	const fb::lsp::json* const v = reader.value(0);
	if (v == nullptr || reader.failed() || !reader.at_end())
		return nullptr;
	return v;
}

auto fb::lsp::member(const fb::lsp::json* const object, const ca::sv::str key) noexcept -> const fb::lsp::json* {

	if (object == nullptr || !object->is(fb::lsp::json_kind::object))
		return nullptr;
	for (ca::usz i = 0U; i < object->keys.size(); ++i)
		if (object->keys.data()[i] == key)
			return object->items.data()[i];
	return nullptr;
}

auto fb::lsp::element(const fb::lsp::json* const array, const ca::usz index) noexcept -> const fb::lsp::json* {

	if (array == nullptr || !array->is(fb::lsp::json_kind::array) || index >= array->items.size())
		return nullptr;
	return array->items.data()[index];
}

auto fb::lsp::length(const fb::lsp::json* const array) noexcept -> ca::usz {
	return array != nullptr && array->is(fb::lsp::json_kind::array) ? array->items.size() : 0U;
}

auto fb::lsp::text_of(const fb::lsp::json* const v) noexcept -> ca::optional<ca::sv::str> {

	if (v == nullptr || !v->is(fb::lsp::json_kind::string))
		return ca::nullopt;
	return v->text;
}

auto fb::lsp::integer_of(const fb::lsp::json* const v, const ca::i64 fallback) noexcept -> ca::i64 {

	if (v == nullptr || !v->is(fb::lsp::json_kind::number))
		return fallback;
	if (v->number > 9.0e18 || v->number < -9.0e18)
		return fallback;
	return static_cast<ca::i64>(v->number);
}

auto fb::lsp::truth_of(const fb::lsp::json* const v, const bool fallback) noexcept -> bool {
	return v != nullptr && v->is(fb::lsp::json_kind::boolean) ? v->truth : fallback;
}

// -- writer ---------------------------------------------------------------------

auto fb::lsp::json_writer::_put(const ca::sv::str text) noexcept -> void {

	if (!_out.append(text)) [[unlikely]]
		ca::trap("fbuildd: out of memory");
}

auto fb::lsp::json_writer::_value(void) noexcept -> void {

	if (_comma)
		this->_put(",");
	_comma = true;
}

auto fb::lsp::json_writer::begin_object(void) noexcept -> void {

	this->_value();
	this->_put("{");
	_comma = false;
}

auto fb::lsp::json_writer::end_object(void) noexcept -> void {

	this->_put("}");
	_comma = true;
}

auto fb::lsp::json_writer::begin_array(void) noexcept -> void {

	this->_value();
	this->_put("[");
	_comma = false;
}

auto fb::lsp::json_writer::end_array(void) noexcept -> void {

	this->_put("]");
	_comma = true;
}

auto fb::lsp::json_writer::key(const ca::sv::str name) noexcept -> void {

	this->string(name);
	this->_put(":");
	_comma = false;
}

auto fb::lsp::json_writer::string(const ca::sv::str text) noexcept -> void {

	this->_value();
	this->_put("\"");
	constexpr char digits[] = "0123456789abcdef";
	ca::usz plain = 0U;
	for (ca::usz i = 0U; i < text.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(text.data()[i]);
		if (c >= 0x20U && c != '"' && c != '\\')
			continue;
		this->_put(text.slice(plain, i - plain));
		plain = i + 1U;
		switch (c) {
			case '"': this->_put("\\\""); break;
			case '\\': this->_put("\\\\"); break;
			case '\n': this->_put("\\n"); break;
			case '\r': this->_put("\\r"); break;
			case '\t': this->_put("\\t"); break;
			default: {
				const char escape[6] = {'\\', 'u', '0', '0', digits[c >> 4U], digits[c & 15U]};
				this->_put(ca::sv::str{escape, 6U});
				break;
			}
		}
	}
	this->_put(text.without_prefix(plain));
	this->_put("\"");
}

auto fb::lsp::json_writer::integer(const ca::i64 value) noexcept -> void {

	this->_value();
	if (!ca::format_append(_out, "{}", value)) [[unlikely]]
		ca::trap("fbuildd: out of memory");
}

auto fb::lsp::json_writer::boolean(const bool value) noexcept -> void {

	this->_value();
	this->_put(value ? "true" : "false");
}

auto fb::lsp::json_writer::null(void) noexcept -> void {

	this->_value();
	this->_put("null");
}

auto fb::lsp::json_writer::raw(const fb::lsp::json* const v) noexcept -> void {

	if (v == nullptr) {
		this->null();
		return;
	}
	switch (v->kind) {
		case fb::lsp::json_kind::null:
			this->null();
			return;
		case fb::lsp::json_kind::boolean:
			this->boolean(v->truth);
			return;
		case fb::lsp::json_kind::number: {
			if (v->number == static_cast<ca::f64>(fb::lsp::integer_of(v, 0))) {
				this->integer(fb::lsp::integer_of(v, 0));
				return;
			}
			char buffer[32];
			const ca::usz size = fb::format_float(v->number, buffer);
			this->_value();
			this->_put(ca::sv::str{buffer, size});
			return;
		}
		case fb::lsp::json_kind::string:
			this->string(v->text);
			return;
		case fb::lsp::json_kind::array:
			this->begin_array();
			for (const fb::lsp::json* const item : v->items)
				this->raw(item);
			this->end_array();
			return;
		case fb::lsp::json_kind::object:
			this->begin_object();
			for (ca::usz i = 0U; i < v->keys.size(); ++i) {
				this->key(v->keys.data()[i]);
				this->raw(v->items.data()[i]);
			}
			this->end_object();
			return;
	}
}
