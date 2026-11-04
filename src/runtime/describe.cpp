#include "check/catalog.hpp"
#include "runtime/interpreter.hpp"

namespace fb::impl {

	[[nodiscard]] static auto result_type(fb::check::types& types, const fb::ret r) noexcept -> fb::check::type* {

		switch (r) {
			case fb::ret::integer: return types.integer();
			case fb::ret::floating: return types.floating();
			case fb::ret::string: return types.string();
			case fb::ret::boolean: return types.boolean();
			case fb::ret::file: return types.file();
			case fb::ret::list: return types.list(types.unknown());
			case fb::ret::map: return types.map();
			case fb::ret::unit: return types.unit();
			case fb::ret::list_string: return types.list(types.string());
			case fb::ret::number: return types.number();
			case fb::ret::any:
			default: return types.unknown();
		}
	}

	[[nodiscard]] static auto has_natives(const fb::class_info* const cls) noexcept -> bool {

		for (const fb::value* const m : cls->methods)
			if (m->is(fb::kind::native))
				return true;
		return false;
	}

	/* A bound value's type, read from the value. */
	[[nodiscard]] static auto value_type(fb::check::types& types, const fb::value* const v) noexcept -> fb::check::type* {

		switch (v->kind) {
			case fb::kind::unit: return types.unit();
			case fb::kind::integer: return types.integer();
			case fb::kind::floating: return types.floating();
			case fb::kind::string: return types.string();
			case fb::kind::boolean: return types.boolean();
			case fb::kind::file: return types.file();
			case fb::kind::command: return types.command();
			case fb::kind::map: return types.map();
			case fb::kind::list: {
				fb::check::type* item = nullptr;
				for (const fb::value* const element : v->items) {
					fb::check::type* const t = fb::impl::value_type(types, element);
					item = item == nullptr || fb::check::same(item, t) ? t : types.unknown();
				}
				return types.list(item != nullptr ? item : types.unknown());
			}
			case fb::kind::tuple: {
				fb::check::type* const t = types.tuple();
				for (const fb::value* const element : v->items)
					types.push(t, fb::impl::value_type(types, element));
				return t;
			}
			case fb::kind::object: {
				if (v->cls == nullptr)
					return types.object("", false);
				fb::check::type* const o = types.object(v->cls->name, false);
				o->native = fb::impl::has_natives(v->cls);
				for (ca::usz i = 0U; i < v->keys.size(); ++i)
					types.set_field(o, v->keys.data()[i], fb::impl::value_type(types, v->at(i)));
				return o;
			}
			default:
				return types.unknown();
		}
	}

} // namespace fb::impl

auto fb::interpreter::describe(fb::check::catalog& catalog) const noexcept -> ca::result<void, fb::diagnostic> {

	fb::check::types& types = catalog.types();
	for (const ca::sv::str name : _builtin_order) {
		auto found = _builtins.get(name);
		if (!found)
			continue;
		const fb::builtin& b = *found;
		fb::check::type* const result = fb::impl::result_type(types, b.result);
		if ((b.flags & fb::global) != 0U) {
			auto bound = _bound.get(name);
			catalog.add_global(name, !result->is(fb::check::form::unknown) || !bound ? result : fb::impl::value_type(types, *bound));
			continue;
		}
		auto added = catalog.add_builtin(name, b.min, b.max, b.signature, result, b.doc);
		if (!added)
			return ca::failure<fb::diagnostic>{fb::diagnostic{added.error(), {0, 0}}};
	}
	for (const auto& item : _bound.items())
		if (!_builtins.contains(item.key))
			catalog.add_global(item.key, fb::impl::value_type(types, item.value));
	for (const auto& item : _classes.items()) {
		if (!fb::impl::has_natives(item.value))
			continue;
		fb::check::native_class* const cls = catalog.add_class(item.key);
		for (const ca::sv::str method : item.value->names)
			catalog.add_method(cls, method);
	}
	return {};
}
