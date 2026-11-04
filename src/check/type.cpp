#include "check/type.hpp"

namespace fb::check::impl {

	using fb::check::form;
	using fb::check::type;

	[[nodiscard]] static auto lenient(const type* const t) noexcept -> bool {
		return t == nullptr || t->is(form::unknown) || t->is(form::unit) || t->is(form::variable);
	}

	[[nodiscard]] static auto is_number(const type* const t) noexcept -> bool {
		return t->is(form::integer) || t->is(form::floating);
	}

	static auto render_into(fb::text& out, const type* const t, const bool detailed) noexcept -> void;

	static auto render_list(fb::text& out, const type* const t, const ca::sv::str separator, const bool detailed) noexcept -> void {

		for (ca::usz i = 0U; i < t->size(); ++i) {
			if (i != 0U)
				static_cast<void>(out.append(separator));
			fb::check::impl::render_into(out, t->kid(i), detailed);
		}
	}

	static auto render_fields(fb::text& out, const type* const t, const bool detailed) noexcept -> void {

		static_cast<void>(out.append("{"));
		for (ca::usz i = 0U; i < t->size(); ++i) {
			if (i != 0U)
				static_cast<void>(out.append(", "));
			static_cast<void>(out.append(t->names.data()[i]));
			static_cast<void>(out.append(": "));
			fb::check::impl::render_into(out, t->kid(i), detailed);
		}
		static_cast<void>(out.append("}"));
	}

	static auto render_into(fb::text& out, const type* const t, const bool detailed) noexcept -> void {

		if (t == nullptr) {
			static_cast<void>(out.append("?"));
			return;
		}
		switch (t->kind) {
			case form::unknown: static_cast<void>(out.append("?")); return;
			case form::unit: static_cast<void>(out.append("Unit")); return;
			case form::integer: static_cast<void>(out.append("Int")); return;
			case form::floating: static_cast<void>(out.append("Float")); return;
			case form::string: static_cast<void>(out.append("String")); return;
			case form::boolean: static_cast<void>(out.append("Bool")); return;
			case form::file: static_cast<void>(out.append("File")); return;
			case form::command: static_cast<void>(out.append("Command")); return;
			case form::map: static_cast<void>(out.append("Map")); return;
			case form::variable: static_cast<void>(out.append(t->name)); return;
			case form::list:
				if (t->variadic) {
					static_cast<void>(out.append("..."));
					fb::check::impl::render_into(out, t->kid(0U), detailed);
					return;
				}
				static_cast<void>(out.append("List<"));
				fb::check::impl::render_into(out, t->kid(0U), detailed);
				static_cast<void>(out.append(">"));
				return;
			case form::tuple:
				if (!t->sized) {
					static_cast<void>(out.append("Tuple"));
					return;
				}
				static_cast<void>(out.append("Tuple<"));
				fb::check::impl::render_list(out, t, ", ", detailed);
				static_cast<void>(out.append(">"));
				return;
			case form::function: {
				static_cast<void>(out.append("("));
				const type* const p = t->parameters();
				if (p == nullptr || p->size() == 0U)
					static_cast<void>(out.append("()"));
				else
					for (ca::usz i = 0U; i < p->size(); ++i) {
						if (i != 0U)
							static_cast<void>(out.append(", "));
						fb::check::impl::render_into(out, p->kid(i), detailed);
						if (i >= t->required && !(p->kid(i)->is(form::list) && p->kid(i)->variadic))
							static_cast<void>(out.append("?"));
					}
				static_cast<void>(out.append(" -> "));
				fb::check::impl::render_into(out, t->result(), detailed);
				static_cast<void>(out.append(")"));
				return;
			}
			case form::object:
				if (!t->name.empty())
					static_cast<void>(out.append(t->name));
				if (t->name.empty() || (detailed && t->sized && t->size() != 0U))
					fb::check::impl::render_fields(out, t, detailed);
				return;
			case form::alternatives:
				fb::check::impl::render_list(out, t, "|", detailed);
				return;
		}
	}

	[[nodiscard]] static auto objects_compatible(const type* const a, const type* const b) noexcept -> bool {

		if (a->name != b->name)
			return false;
		if (!a->sized || !b->sized)
			return true;
		for (ca::usz i = 0U; i < a->size(); ++i) {
			const type* const other = b->field(a->names.data()[i]);
			if (other != nullptr && !fb::check::compatible(a->kid(i), other))
				return false;
		}
		return true;
	}

} // namespace fb::check::impl

// -- types --------------------------------------------------------------------

fb::check::types::types(fb::memory& memory) noexcept
: _memory{memory}, _unknown{nullptr}, _unit{nullptr}, _integer{nullptr}, _floating{nullptr}, _string{nullptr},
  _boolean{nullptr}, _command{nullptr}, _map{nullptr} {

	_unknown = this->make(fb::check::form::unknown);
	_unit = this->make(fb::check::form::unit);
	_integer = this->make(fb::check::form::integer);
	_floating = this->make(fb::check::form::floating);
	_string = this->make(fb::check::form::string);
	_boolean = this->make(fb::check::form::boolean);
	_command = this->make(fb::check::form::command);
	_map = this->make(fb::check::form::map);
}

auto fb::check::types::make(const fb::check::form kind) noexcept -> fb::check::type* {
	return _memory.make<fb::check::type>(kind, _memory.list<fb::check::type*>(), _memory.list<ca::sv::str>(), ca::sv::str{}, false, false, false, 0U);
}

auto fb::check::types::number(void) noexcept -> fb::check::type* {
	return this->either(_integer, _floating);
}

auto fb::check::types::file(void) noexcept -> fb::check::type* {

	fb::check::type* const f = this->make(fb::check::form::file);
	f->sized = true;
	for (const ca::sv::str name : {ca::sv::str{"name"}, ca::sv::str{"stem"}, ca::sv::str{"suffix"}, ca::sv::str{"parent"}})
		this->set_field(f, name, _string);
	this->set_field(f, "mtime", _integer);
	this->set_field(f, "size", _integer);
	return f;
}

auto fb::check::types::list(fb::check::type* const element) noexcept -> fb::check::type* {

	fb::check::type* const l = this->make(fb::check::form::list);
	this->push(l, element != nullptr ? element : _unknown);
	return l;
}

auto fb::check::types::variadic(fb::check::type* const element) noexcept -> fb::check::type* {

	fb::check::type* const l = this->list(element);
	l->variadic = true;
	return l;
}

auto fb::check::types::tuple(void) noexcept -> fb::check::type* {

	fb::check::type* const t = this->make(fb::check::form::tuple);
	t->sized = true;
	return t;
}

auto fb::check::types::any_tuple(void) noexcept -> fb::check::type* {
	return this->make(fb::check::form::tuple);
}

auto fb::check::types::function(fb::check::type* const parameters, fb::check::type* const result, const ca::u32 required) noexcept -> fb::check::type* {

	fb::check::type* const f = this->make(fb::check::form::function);
	this->push(f, parameters);
	this->push(f, result != nullptr ? result : _unknown);
	f->required = required;
	return f;
}

auto fb::check::types::function(fb::check::type* const parameters, fb::check::type* const result) noexcept -> fb::check::type* {

	ca::u32 required = 0U;
	for (const fb::check::type* const p : parameters->kids)
		if (!(p->is(fb::check::form::list) && p->variadic))
			++required;
	return this->function(parameters, result, required);
}

auto fb::check::types::object(const ca::sv::str name, const bool sized) noexcept -> fb::check::type* {

	fb::check::type* const o = this->make(fb::check::form::object);
	o->name = name;
	o->sized = sized;
	return o;
}

auto fb::check::types::variable(const ca::sv::str name) noexcept -> fb::check::type* {

	fb::check::type* const v = this->make(fb::check::form::variable);
	v->name = name;
	return v;
}

auto fb::check::types::either(fb::check::type* const a, fb::check::type* const b) noexcept -> fb::check::type* {

	if (a == nullptr || a->is(fb::check::form::unknown) || b == nullptr || b->is(fb::check::form::unknown))
		return _unknown;
	if (a->is(fb::check::form::unit))
		return b;
	if (b->is(fb::check::form::unit))
		return a;
	fb::check::type* const u = this->make(fb::check::form::alternatives);
	const auto add = [&](fb::check::type* const member) noexcept {
		for (const fb::check::type* const existing : u->kids)
			if (fb::check::same(existing, member))
				return;
		this->push(u, member);
	};
	for (fb::check::type* const side : {a, b}) {
		if (side->is(fb::check::form::alternatives))
			for (fb::check::type* const member : side->kids)
				add(member);
		else
			add(side);
	}
	return u->size() == 1U ? u->kid(0U) : u;
}

auto fb::check::types::push(fb::check::type* const into, fb::check::type* const kid) noexcept -> void {
	_memory.push(into->kids, kid);
}

auto fb::check::types::set_field(fb::check::type* const record, const ca::sv::str name, fb::check::type* const t) noexcept -> void {

	for (ca::usz i = 0U; i < record->names.size(); ++i)
		if (record->names.data()[i] == name) {
			record->kids.data()[i] = t;
			return;
		}
	_memory.push(record->names, name);
	_memory.push(record->kids, t);
}

auto fb::check::types::copy_record(const fb::check::type* const record) noexcept -> fb::check::type* {

	fb::check::type* const r = this->make(record->kind);
	r->name = record->name;
	r->sized = record->sized;
	r->native = record->native;
	for (ca::usz i = 0U; i < record->size(); ++i)
		this->set_field(r, record->names.data()[i], record->kid(i));
	return r;
}

// -- relations ------------------------------------------------------------------

auto fb::check::compatible(const fb::check::type* const a, const fb::check::type* const b) noexcept -> bool {

	using fb::check::form;
	if (fb::check::impl::lenient(a) || fb::check::impl::lenient(b))
		return true;
	if (a->is(form::alternatives) || b->is(form::alternatives)) {
		const fb::check::type* const u = a->is(form::alternatives) ? a : b;
		const fb::check::type* const o = a->is(form::alternatives) ? b : a;
		for (const fb::check::type* const member : u->kids)
			if (fb::check::compatible(member, o))
				return true;
		return false;
	}
	if (fb::check::impl::is_number(a) && fb::check::impl::is_number(b))
		return true;
	// Lists and tuples are both sequences: what takes one takes the other.
	if ((a->is(form::list) && b->is(form::tuple)) || (a->is(form::tuple) && b->is(form::list))) {
		const fb::check::type* const l = a->is(form::list) ? a : b;
		const fb::check::type* const t = a->is(form::list) ? b : a;
		for (const fb::check::type* const item : t->kids)
			if (!fb::check::compatible(l->kid(0U), item))
				return false;
		return true;
	}
	if (a->kind != b->kind)
		return false;
	switch (a->kind) {
		case form::list:
			return fb::check::compatible(a->kid(0U), b->kid(0U));
		case form::tuple:
			if (!a->sized || !b->sized)
				return true;
			if (a->size() != b->size())
				return false;
			for (ca::usz i = 0U; i < a->size(); ++i)
				if (!fb::check::compatible(a->kid(i), b->kid(i)))
					return false;
			return true;
		case form::function:
			return true;
		case form::object:
			return fb::check::impl::objects_compatible(a, b);
		default:
			return true;
	}
}

auto fb::check::numeric(const fb::check::type* const t) noexcept -> bool {

	if (t == nullptr)
		return false;
	if (fb::check::impl::is_number(t))
		return true;
	if (!t->is(fb::check::form::alternatives) || t->size() == 0U)
		return false;
	for (const fb::check::type* const member : t->kids)
		if (!fb::check::numeric(member))
			return false;
	return true;
}

auto fb::check::same(const fb::check::type* const a, const fb::check::type* const b) noexcept -> bool {

	if (a == b)
		return true;
	if (a == nullptr || b == nullptr || a->kind != b->kind || a->name != b->name || a->size() != b->size()
		|| a->sized != b->sized || a->variadic != b->variadic)
		return false;
	if (a->is(fb::check::form::file))
		return true;
	for (ca::usz i = 0U; i < a->size(); ++i)
		if (!fb::check::same(a->kid(i), b->kid(i)))
			return false;
	return true;
}

auto fb::check::render(fb::memory& memory, const fb::check::type* const t, const bool detailed) noexcept -> ca::sv::str {

	fb::text out{memory.allocator()};
	fb::check::impl::render_into(out, t, detailed);
	return memory.copy(out.view());
}

// -- type variables -------------------------------------------------------------

auto fb::check::bindings::get(const ca::sv::str name) const noexcept -> fb::check::type* {

	for (ca::usz i = 0U; i < _names.size(); ++i)
		if (_names.data()[i] == name)
			return _types.data()[i];
	return nullptr;
}

auto fb::check::bindings::set(fb::memory& memory, const ca::sv::str name, fb::check::type* const t) noexcept -> void {

	for (ca::usz i = 0U; i < _names.size(); ++i)
		if (_names.data()[i] == name) {
			_types.data()[i] = t;
			return;
		}
	memory.push(_names, name);
	memory.push(_types, t);
}

auto fb::check::element(fb::check::types& types, const fb::check::type* const t) noexcept -> fb::check::type* {

	if (t == nullptr)
		return types.unknown();
	if (t->is(fb::check::form::list))
		return t->kid(0U);
	if (t->is(fb::check::form::tuple) && t->sized && t->size() != 0U) {
		for (ca::usz i = 1U; i < t->size(); ++i)
			if (!fb::check::same(t->kid(0U), t->kid(i)))
				return types.unknown();
		return t->kid(0U);
	}
	return types.unknown();
}

auto fb::check::match(fb::check::types& types, const fb::check::type* const expected, fb::check::type* const arg, fb::check::bindings& bound) noexcept -> bool {

	using fb::check::form;
	if (expected == nullptr || arg == nullptr || arg->is(form::unknown) || arg->is(form::unit))
		return true;
	if (expected->is(form::variable)) {
		fb::check::type* const was = bound.get(expected->name);
		if (was == nullptr || was->is(form::unknown)) {
			bound.set(types.memory(), expected->name, arg);
			return true;
		}
		if (fb::check::compatible(was, arg)) {
			if (!fb::check::same(was, arg) && !fb::check::numeric(was))
				bound.set(types.memory(), expected->name, types.either(was, arg));
			return true;
		}
		return false;
	}
	if (!fb::check::generic(expected))
		return fb::check::compatible(expected, arg);
	if (expected->is(form::alternatives)) {
		for (const fb::check::type* const member : expected->kids)
			if (fb::check::compatible(member, arg))
				return fb::check::match(types, member, arg, bound);
		return false;
	}
	if (expected->is(form::list) && (arg->is(form::list) || arg->is(form::tuple))) {
		if (arg->is(form::tuple) && arg->sized) {
			for (fb::check::type* const item : arg->kids)
				if (!fb::check::match(types, expected->kid(0U), item, bound))
					return false;
			return true;
		}
		return fb::check::match(types, expected->kid(0U), fb::check::element(types, arg), bound);
	}
	if (expected->kind != arg->kind)
		return fb::check::compatible(expected, arg);
	if (expected->is(form::tuple) && expected->sized && arg->sized) {
		if (expected->size() != arg->size())
			return false;
		for (ca::usz i = 0U; i < expected->size(); ++i)
			if (!fb::check::match(types, expected->kid(i), arg->kid(i), bound))
				return false;
		return true;
	}
	if (expected->is(form::function)) {
		const fb::check::type* const ep = expected->parameters();
		const fb::check::type* const ap = arg->parameters();
		if (ep != nullptr && ap != nullptr && ep->size() == ap->size())
			for (ca::usz i = 0U; i < ep->size(); ++i)
				static_cast<void>(fb::check::match(types, ep->kid(i), ap->kid(i), bound));
		return fb::check::match(types, expected->result(), arg->result(), bound);
	}
	return fb::check::compatible(expected, arg);
}

auto fb::check::generic(const fb::check::type* const t) noexcept -> bool {

	if (t == nullptr)
		return false;
	if (t->is(fb::check::form::variable))
		return true;
	if (t->is(fb::check::form::file))
		return false;
	for (const fb::check::type* const kid : t->kids)
		if (fb::check::generic(kid))
			return true;
	return false;
}

auto fb::check::substitute(fb::check::types& types, fb::check::type* const t, const fb::check::bindings& bound) noexcept -> fb::check::type* {

	if (t == nullptr || !fb::check::generic(t))
		return t;
	if (t->is(fb::check::form::variable)) {
		fb::check::type* const v = bound.get(t->name);
		return v != nullptr ? v : types.unknown();
	}
	if (t->is(fb::check::form::alternatives)) {
		fb::check::type* out = nullptr;
		for (fb::check::type* const member : t->kids) {
			fb::check::type* const m = fb::check::substitute(types, member, bound);
			out = out == nullptr ? m : types.either(out, m);
		}
		return out != nullptr ? out : types.unknown();
	}
	fb::check::type* const copy = types.make(t->kind);
	copy->name = t->name;
	copy->sized = t->sized;
	copy->variadic = t->variadic;
	copy->native = t->native;
	copy->required = t->required;
	for (ca::usz i = 0U; i < t->size(); ++i) {
		types.push(copy, fb::check::substitute(types, t->kid(i), bound));
		if (i < t->names.size())
			types.memory().push(copy->names, t->names.data()[i]);
	}
	return copy;
}
