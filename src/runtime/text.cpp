#include "runtime/library.hpp"

#include "ca/format_append.hpp"
#include "core/numbers.hpp"

namespace fb::impl {

	class renderer final {

		private:

			fb::text& _out;

			auto _put(const ca::sv::str text) noexcept -> void {

				if (!_out.append(text)) [[unlikely]]
					ca::trap("fbuild: out of memory");
			}

			auto _number(const fb::value* const v) noexcept -> void {

				if (v->is(fb::kind::floating)) {
					char buffer[32];
					const ca::usz size = fb::format_float(v->floating, buffer);
					this->_put(ca::sv::str{buffer, size});
					return;
				}
				char buffer[24];
				const ca::usz size = fb::format_integer(v->integer, buffer);
				this->_put(ca::sv::str{buffer, size});
			}

			auto _items(const fb::value* const v, const ca::sv::str open, const ca::sv::str separator, const ca::sv::str close, const bool typed) noexcept -> void {

				this->_put(open);
				for (ca::usz i = 0U; i < v->size(); ++i) {
					if (i != 0U)
						this->_put(separator);
					this->render(v->at(i), typed);
				}
				this->_put(close);
			}

			auto _fields(const fb::value* const v, const bool typed) noexcept -> void {

				this->_put("{");
				for (ca::usz i = 0U; i < v->keys.size(); ++i) {
					if (i != 0U)
						this->_put(", ");
					if (typed)
						this->_put("\"");
					this->_put(v->keys.data()[i]);
					this->_put(typed ? "\": " : ": ");
					this->render(v->at(i), typed);
				}
				this->_put("}");
			}

			auto _count(const ca::sv::str name, const ca::usz count) noexcept -> void {

				this->_put(name);
				this->_put("#");
				char buffer[24];
				const ca::usz size = fb::format_integer(static_cast<ca::i64>(count), buffer);
				this->_put(ca::sv::str{buffer, size});
			}

		public:

			explicit renderer(fb::text& out) noexcept
			: _out{out} {
			}

			auto text(const fb::value* const v) noexcept -> void {

				if (v == nullptr)
					return;
				switch (v->kind) {
					case fb::kind::string:
					case fb::kind::file:
						this->_put(v->text);
						return;
					case fb::kind::integer:
					case fb::kind::boolean:
					case fb::kind::floating:
						this->_number(v);
						return;
					case fb::kind::command:
						for (ca::usz i = 0U; i < v->size(); ++i) {
							if (i != 0U)
								this->_put(" ");
							this->text(v->at(i));
						}
						return;
					default:
						return;
				}
			}

			auto render(const fb::value* const v, const bool typed) noexcept -> void {

				if (v == nullptr) {
					this->_put(typed ? "None" : "()");
					return;
				}
				switch (v->kind) {
					case fb::kind::unit:
						this->_put(typed ? "None" : "()");
						return;
					case fb::kind::integer:
						if (typed)
							this->_put("Int(");
						this->_number(v);
						if (typed)
							this->_put(")");
						return;
					case fb::kind::floating:
						if (typed)
							this->_put("Float(");
						this->_number(v);
						if (typed)
							this->_put(")");
						return;
					case fb::kind::boolean:
						this->_put(typed ? (v->integer != 0 ? "Bool(true)" : "Bool(false)") : (v->integer != 0 ? "true" : "false"));
						return;
					case fb::kind::string:
						this->_put(typed ? "Str(\"" : "");
						this->_put(v->text);
						this->_put(typed ? "\")" : "");
						return;
					case fb::kind::file:
						this->_put(typed ? "File(" : "");
						this->_put(v->text);
						this->_put(typed ? ")" : "");
						return;
					case fb::kind::list:
						if (typed)
							this->_count("List", v->size());
						this->_items(v, "[", ", ", "]", typed);
						return;
					case fb::kind::seq:
						this->_put(typed ? "Seq" : "<seq>");
						return;
					case fb::kind::tuple:
						if (typed)
							this->_count("Tuple", v->size());
						this->_items(v, "(", ", ", ")", typed);
						return;
					case fb::kind::command:
						if (typed) {
							this->_count("Cmd", v->size());
							this->_items(v, "[", ", ", "]", true);
						} else
							this->_items(v, "`", " ", "`", false);
						return;
					case fb::kind::map:
						if (typed)
							this->_count("Map", v->keys.size());
						this->_fields(v, typed);
						return;
					case fb::kind::object:
						this->_put(v->cls != nullptr ? v->cls->name : ca::sv::str{typed ? "Object" : "object"});
						this->_fields(v, typed);
						return;
					case fb::kind::lambda:
						if (typed) {
							this->_put("Lambda(");
							char buffer[24];
							const ca::usz size = fb::format_integer(static_cast<ca::i64>(v->params != nullptr ? v->params->size() : 0U), buffer);
							this->_put(ca::sv::str{buffer, size});
							this->_put(" params)");
						} else
							this->_put("<lambda>");
						return;
					case fb::kind::morph:
						this->_put(typed ? "Morph(" : "<");
						this->_put(v->text);
						this->_put(" // ");
						this->_put(v->other);
						this->_put(typed ? ")" : ">");
						return;
					case fb::kind::builtin:
						this->_put(typed ? "Builtin(" : "<builtin ");
						this->_put(v->text);
						this->_put(typed ? ")" : ">");
						return;
					case fb::kind::thunk:
						this->_put(typed ? "Thunk" : "<thunk>");
						return;
					case fb::kind::matcher:
						this->_put(typed ? "Match" : "<match>");
						return;
					case fb::kind::native:
						this->_put(typed ? "Method" : "<method>");
						return;
					case fb::kind::class_:
						this->_put(typed ? "Class(\"" : "<class ");
						this->_put(v->cls != nullptr ? v->cls->name : ca::sv::str{""});
						this->_put(typed ? "\")" : ">");
						return;
				}
			}
	};

} // namespace fb::impl

auto fb::interpreter::text(const fb::value* const v) noexcept -> ca::sv::str {

	if (v == nullptr)
		return "";
	if (v->is(fb::kind::string) || v->is(fb::kind::file))
		return v->text;
	fb::text out{_memory.allocator()};
	fb::impl::renderer{out}.text(v);
	return _memory.copy(out.view());
}

auto fb::interpreter::repr(const fb::value* const v, const bool typed) noexcept -> ca::sv::str {

	fb::text out{_memory.allocator()};
	fb::impl::renderer{out}.render(v, typed);
	return _memory.copy(out.view());
}
