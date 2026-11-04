#include "check/catalog.hpp"

#include "ca/failure.hpp"

namespace fb::check::impl {

	using fb::check::form;
	using fb::check::type;

	template <typename T>
	static auto must(ca::result<T>&& r) noexcept -> void {

		if (!r) [[unlikely]]
			ca::trap("fbuild: out of memory");
	}

	/* Recursive descent over a signature's text. */
	class signature_parser final {

		private:

			using parsed = ca::result<type*, ca::sv::str>;
			using failed = ca::failure<ca::sv::str>;

			fb::check::types& _types;
			ca::sv::str _text;
			ca::usz _at;

			[[nodiscard]] static auto word_char(const char c) noexcept -> bool {
				return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
			}

			auto _blank(void) noexcept -> void {

				while (_at < _text.size() && (_text.data()[_at] == ' ' || _text.data()[_at] == '\t'))
					++_at;
			}

			[[nodiscard]] auto _peek(const ca::sv::str what) noexcept -> bool {

				this->_blank();
				return _text.size() - _at >= what.size() && ca::sv::str{_text.data() + _at, what.size()} == what;
			}

			[[nodiscard]] auto _accept(const ca::sv::str what) noexcept -> bool {

				if (!this->_peek(what))
					return false;
				_at += what.size();
				return true;
			}

			[[nodiscard]] auto _fail(const ca::sv::str what) noexcept -> failed {
				return failed{_types.memory().format("{} at column {} of \"{}\"", what, _at + 1U, _text)};
			}

			[[nodiscard]] auto _expect(const ca::sv::str what) noexcept -> ca::result<void, ca::sv::str> {

				if (this->_accept(what))
					return {};
				return this->_fail(_types.memory().format("expected '{}'", what));
			}

			[[nodiscard]] auto _word(void) noexcept -> ca::sv::str {

				this->_blank();
				const ca::usz start = _at;
				while (_at < _text.size() && word_char(_text.data()[_at]))
					++_at;
				return ca::sv::str{_text.data() + start, _at - start};
			}

			[[nodiscard]] auto _named(const ca::sv::str name) noexcept -> parsed {

				if (name == "Int") return _types.integer();
				if (name == "Float") return _types.floating();
				if (name == "Number") return _types.number();
				if (name == "String") return _types.string();
				if (name == "Bool") return _types.boolean();
				if (name == "File") return _types.file();
				if (name == "Path") return _types.either(_types.file(), _types.string());
				if (name == "Command") return _types.command();
				if (name == "Unit") return _types.unit();
				if (name == "Map") return _types.map();
				if (name == "Json" || name == "Any") return _types.unknown();
				if (name.data()[0] >= 'a' && name.data()[0] <= 'z')
					return _types.variable(_types.memory().copy(name));
				return _types.object(_types.memory().copy(name), false);
			}

			[[nodiscard]] auto _generic(const ca::sv::str name) noexcept -> parsed {

				type* const t = name == "List" ? _types.make(form::list) : name == "Tuple" ? _types.tuple() : nullptr;
				if (t == nullptr)
					return this->_fail(_types.memory().format("unknown generic type '{}'", name));
				do {
					auto kid = this->_alternatives();
					if (!kid)
						return kid;
					_types.push(t, *kid);
				} while (this->_accept(","));
				auto close = this->_expect(">");
				if (!close)
					return close.propagate();
				if (t->is(form::list) && t->size() != 1U)
					return this->_fail("List takes one type");
				return t;
			}

			[[nodiscard]] auto _record(void) noexcept -> parsed {

				type* const r = _types.object("", true);
				if (this->_accept("}"))
					return r;
				do {
					const ca::sv::str name = this->_word();
					if (name.empty())
						return this->_fail("expected a field name");
					auto colon = this->_expect(":");
					if (!colon)
						return colon.propagate();
					auto t = this->_alternatives();
					if (!t)
						return t;
					_types.set_field(r, _types.memory().copy(name), *t);
				} while (this->_accept(","));
				auto close = this->_expect("}");
				if (!close)
					return close.propagate();
				return r;
			}

			[[nodiscard]] auto _atom(void) noexcept -> parsed {

				if (this->_accept("?"))
					return _types.unknown();
				if (this->_accept("{"))
					return this->_record();
				if (this->_accept("(")) {
					auto inner = this->_function_or_group();
					if (!inner)
						return inner;
					auto close = this->_expect(")");
					if (!close)
						return close.propagate();
					return inner;
				}
				const ca::sv::str name = this->_word();
				if (name.empty())
					return this->_fail("expected a type");
				if (this->_accept("<"))
					return this->_generic(name);
				if (name == "List")
					return _types.list(_types.unknown());
				if (name == "Tuple")
					return _types.any_tuple();
				return this->_named(name);
			}

			[[nodiscard]] auto _alternatives(void) noexcept -> parsed {

				auto first = this->_atom();
				if (!first)
					return first;
				type* t = *first;
				while (this->_accept("|")) {
					auto next = this->_atom();
					if (!next)
						return next;
					t = _types.either(t, *next);
				}
				return t;
			}

			/* Parameters then "-> result", into a function. */
			[[nodiscard]] auto _parameters_then_result(type* const parameters, ca::u32 required, bool optional_seen) noexcept -> parsed {

				if (!this->_accept("()")) {
					do {
						if (this->_accept("...")) {
							auto tail = this->_alternatives();
							if (!tail)
								return tail;
							_types.push(parameters, _types.variadic(*tail));
							break;
						}
						auto p = this->_alternatives();
						if (!p)
							return p;
						_types.push(parameters, *p);
						if (this->_accept("?"))
							optional_seen = true;
						else if (optional_seen)
							return this->_fail("a required parameter after an optional one");
						else
							++required;
					} while (this->_accept(","));
				}
				auto arrow = this->_expect("->");
				if (!arrow)
					return arrow.propagate();
				auto result = this->_alternatives();
				if (!result)
					return result;
				return _types.function(parameters, *result, required);
			}

			/* Inside parentheses: a function "(a, b -> c)", or a grouped type "(A|B)". */
			[[nodiscard]] auto _function_or_group(void) noexcept -> parsed {

				const ca::usz start = _at;
				int depth = 0;
				bool arrow = false;
				for (ca::usz i = _at; i + 1U < _text.size(); ++i) {
					const char c = _text.data()[i];
					if (c == '>' && i > 0U && _text.data()[i - 1U] == '-')
						continue;
					if (c == '(' || c == '<' || c == '{')
						++depth;
					else if (c == ')' || c == '>' || c == '}') {
						if (depth == 0)
							break;
						--depth;
					} else if (depth == 0 && c == '-' && _text.data()[i + 1U] == '>') {
						arrow = true;
						break;
					}
				}
				_at = start;
				if (!arrow)
					return this->_alternatives();
				return this->_parameters_then_result(_types.tuple(), 0U, false);
			}

		public:

			signature_parser(fb::check::types& types, const ca::sv::str text) noexcept
			: _types{types}, _text{text}, _at{0U} {
			}

			[[nodiscard]] auto signature(void) noexcept -> parsed {

				auto f = this->_parameters_then_result(_types.tuple(), 0U, false);
				if (!f)
					return f;
				this->_blank();
				if (_at != _text.size())
					return this->_fail("unexpected text");
				return f;
			}

			[[nodiscard]] auto single(void) noexcept -> parsed {

				auto t = this->_alternatives();
				if (!t)
					return t;
				this->_blank();
				if (_at != _text.size())
					return this->_fail("unexpected text");
				return t;
			}
	};

} // namespace fb::check::impl

auto fb::check::parse_signature(fb::check::types& types, const ca::sv::str text) noexcept -> ca::result<fb::check::type*, ca::sv::str> {

	fb::check::impl::signature_parser parser{types, text};
	return parser.signature();
}

auto fb::check::parse_type(fb::check::types& types, const ca::sv::str text) noexcept -> ca::result<fb::check::type*, ca::sv::str> {

	fb::check::impl::signature_parser parser{types, text};
	return parser.single();
}

fb::check::catalog::catalog(fb::check::types& types) noexcept
: _types{types}, _builtins{types.memory().allocator()}, _globals{types.memory().allocator()},
  _classes{types.memory().allocator()}, _names{types.memory().list<ca::sv::str>()} {
}

auto fb::check::catalog::add_builtin(const ca::sv::str name, const int min, const int max, const ca::sv::str signature,
	fb::check::type* const result, const ca::sv::str doc) noexcept -> ca::result<void, ca::sv::str> {

	fb::check::type* parsed = nullptr;
	if (!signature.empty()) {
		auto s = fb::check::parse_signature(_types, signature);
		if (!s)
			return ca::failure<ca::sv::str>{_types.memory().format("{}: bad signature: {}", name, s.error())};
		parsed = *s;
	}
	const fb::check::builtin_entry entry{name, min, max, parsed, result != nullptr ? result : _types.unknown(), doc};
	if (auto existing = _builtins.get(name)) {
		*existing = entry;
		return {};
	}
	const ca::sv::str kept = _types.memory().copy(name);
	fb::check::builtin_entry stored = entry;
	stored.name = kept;
	fb::check::impl::must(_builtins.set(kept, stored));
	_types.memory().push(_names, kept);
	return {};
}

auto fb::check::catalog::add_global(const ca::sv::str name, fb::check::type* const t) noexcept -> void {

	if (auto existing = _globals.get(name)) {
		*existing = t;
		return;
	}
	const ca::sv::str kept = _types.memory().copy(name);
	fb::check::impl::must(_globals.set(kept, t));
	if (!_builtins.contains(name))
		_types.memory().push(_names, kept);
}

auto fb::check::catalog::add_class(const ca::sv::str name) noexcept -> fb::check::native_class* {

	if (auto existing = _classes.get(name))
		return *existing;
	const ca::sv::str kept = _types.memory().copy(name);
	fb::check::native_class* const cls = _types.memory().make<fb::check::native_class>(kept, _types.memory().list<ca::sv::str>());
	fb::check::impl::must(_classes.set(kept, cls));
	return cls;
}

auto fb::check::catalog::add_method(fb::check::native_class* const cls, const ca::sv::str method) noexcept -> void {

	if (!cls->has(method))
		_types.memory().push(cls->methods, _types.memory().copy(method));
}

auto fb::check::catalog::builtin(const ca::sv::str name) const noexcept -> const fb::check::builtin_entry* {

	auto found = _builtins.get(name);
	return found ? &*found : nullptr;
}

auto fb::check::catalog::global(const ca::sv::str name) const noexcept -> fb::check::type* {

	auto found = _globals.get(name);
	return found ? *found : nullptr;
}

auto fb::check::catalog::native(const ca::sv::str name) const noexcept -> const fb::check::native_class* {

	auto found = _classes.get(name);
	return found ? *found : nullptr;
}
