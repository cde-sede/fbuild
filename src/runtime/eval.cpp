#include "runtime/interpreter.hpp"

#include "ca/environment.hpp"
#include "ca/file.hpp"
#include "ca/span.hpp"
#include "3-system/math/impl/exact_math.hpp"
#include "core/numbers.hpp"
#include "ca/debug.hpp"
#include "ca/process.hpp"
#include "core/interrupt.hpp"
#include "runner/runner.hpp"
#include "runtime/library.hpp"
#include "syntax/parser.hpp"

namespace fb::impl {

	using step = ca::result<void, fb::diagnostic>;

	[[nodiscard]] static auto unescape(fb::memory& memory, const ca::sv::str raw) noexcept -> ca::sv::str {

		char* const out = static_cast<char*>(memory.bytes(raw.size() + 1U, 1U));
		ca::usz j = 0U;
		for (ca::usz i = 0U; i < raw.size(); ++i) {
			const char c = raw.data()[i];
			if (c != '\\' || i + 1U >= raw.size()) {
				out[j++] = c;
				continue;
			}
			switch (raw.data()[++i]) {
				case 'n': out[j++] = '\n'; break;
				case 't': out[j++] = '\t'; break;
				case 'r': out[j++] = '\r'; break;
				case '0': out[j++] = '\0'; break;
				case 'a': out[j++] = '\a'; break;
				case 'b': out[j++] = '\b'; break;
				case 'f': out[j++] = '\f'; break;
				case 'v': out[j++] = '\v'; break;
				default: out[j++] = raw.data()[i]; break;
			}
		}
		out[j] = '\0';
		return ca::sv::str{out, j};
	}

	[[nodiscard]] static auto index_of(const fb::token* const t) noexcept -> ca::i64 {

		if (t == nullptr || t->text.empty())
			return -1;
		ca::i64 v = 0;
		for (const char c : t->text) {
			if (c < '0' || c > '9')
				return -1;
			v = v * 10 + (c - '0');
		}
		return v;
	}

	[[nodiscard]] static auto is_effectful_call(const fb::node* const n) noexcept -> bool {

		if (n->kind != fb::node_kind::call || n->kid(0U)->kind != fb::node_kind::id)
			return false;
		constexpr ca::sv::str effectful[] = {"shell", "run", "mkdir", "write", "move", "copy", "symlink", "remove", "touch"};
		const ca::sv::str name = n->kid(0U)->tok->text;
		for (const ca::sv::str e : effectful)
			if (name == e)
				return true;
		return false;
	}

	[[nodiscard]] static auto is_pure(const fb::node* const n) noexcept -> bool {

		if (n == nullptr)
			return true;
		if (n->kind == fb::node_kind::force || fb::impl::is_effectful_call(n))
			return false;
		if (n->kind == fb::node_kind::lambda || n->kind == fb::node_kind::match_)
			return true;
		for (const fb::node* const kid : n->kids)
			if (!fb::impl::is_pure(kid))
				return false;
		return true;
	}

	template <typename T>
	[[nodiscard]] static auto compared(const T a, const T b, const fb::tok op) noexcept -> bool {

		switch (op) {
			case fb::tok::lt: return a < b;
			case fb::tok::gt: return a > b;
			case fb::tok::le: return a <= b;
			case fb::tok::ge: return a >= b;
			case fb::tok::eq: return a == b;
			default: return a != b;
		}
	}

	[[nodiscard]] static auto fmod(const ca::f64 x, const ca::f64 y) noexcept -> ca::f64 {
		return ca::sys::impl::fmod(x, y);
	}

} // namespace fb::impl

// -- lifecycle --------------------------------------------------------------

fb::interpreter::interpreter(fb::memory& memory) noexcept
: _memory{memory}, _unit{nullptr}, _definitions{memory.allocator()}, _builtins{memory.allocator()},
  _builtin_order{memory.allocator()}, _bound{memory.allocator()}, _classes{memory.allocator()},
  _top{nullptr, nullptr, nullptr}, _args{nullptr}, _cwd{nullptr}, _globals{nullptr}, _shared{nullptr},
  _deferred{memory.allocator()}, _in_run{false}, _bench{false}, _failed{false}, _capture_failed{false},
  _settings{0, false, false, false, 0, 50, 5, nullptr}, _runner{nullptr}, _stack_top{nullptr}, _stack_room{0U} {

	_runner = memory.make<fb::runner>(memory, _settings);

	_unit = this->make(fb::kind::unit);
	_args = this->make(fb::kind::list);
	_globals = this->make(fb::kind::map);
	_shared = this->make(fb::kind::map);
	char directory[4096];
	auto cwd = ca::sys::getcwd(ca::span<char>{directory, sizeof directory});
	_cwd = this->string(cwd ? ca::sv::str{directory, *cwd} : ca::sv::str{""});
	fb::open_core(*this);
}

auto fb::interpreter::set_args(const ca::borrowed_slice<const ca::sv::str> args) noexcept -> void {

	ca::usz start = 0U;
	for (ca::usz i = 0U; i < args.size(); ++i)
		if (args.data()[i] == ":::") {
			start = i + 1U;
			break;
		}
	_args = this->make(fb::kind::list);
	for (ca::usz i = start; i < args.size(); ++i) {
		this->push(_args, this->string(args.data()[i]));
		if (args.data()[i] == "--bench")
			_bench = true;
	}
}

auto fb::interpreter::run(const ca::sv::str source) noexcept -> ca::result<void, fb::diagnostic> {

	auto program = fb::parse_source(_memory, source);
	if (!program)
		return program.propagate();
	return this->run(*program);
}

auto fb::interpreter::run(const fb::node* const program) noexcept -> ca::result<void, fb::diagnostic> {

	constexpr ca::usz margin = 512U * 1024U;
	ca::u64 stack = 8U * 1024U * 1024U;
	if (auto limit = ca::sys::getrlimit(ca::sys::resource::stack_size); limit && limit->current() >= 2U * margin && limit->current() < (1ULL << 32U))
		stack = limit->current();
	_stack_top = static_cast<const char*>(ca::impl::frame_address());
	_stack_room = static_cast<ca::usz>(stack) - margin;
	this->bind("args", _args);
	this->bind("cwd", _cwd);
	this->bind("_GLOBALS", _globals);
	this->bind("_SHARED", _shared);
	for (const fb::node* const statement : program->kids)
		if (statement->kind == fb::node_kind::assign)
			self::_must(_definitions.setdefault(statement->tok->text,
				definition{statement->kid(0U), nullptr, false, fb::impl::is_pure(statement->kid(0U))}));
	for (const fb::node* const statement : program->kids) {
		if (statement->kind == fb::node_kind::assign)
			continue;
		auto done = this->_eval(statement, &_top);
		if (!done) {
			this->_flush_deferred();
			this->flush_outputs();
			return done.propagate();
		}
	}
	this->_flush_deferred();
	this->flush_outputs();
	return {};
}

auto fb::interpreter::global(const ca::sv::str name) noexcept -> fb::outcome {

	auto v = this->_definition(name, true);
	if (!v)
		return v.propagate();
	return *v != nullptr ? *v : _unit;
}

// -- extension API ----------------------------------------------------------

auto fb::interpreter::define(const fb::builtin& b) noexcept -> void {

	auto existing = _builtins.get(b.name);
	if (existing) {
		*existing = b;
		return;
	}
	const ca::sv::str name = _memory.copy(b.name);
	fb::builtin kept = b;
	kept.name = name;
	self::_must(_builtins.set(name, kept));
	_memory.push(_builtin_order, name);
}

auto fb::interpreter::find_builtin(const ca::sv::str name) const noexcept -> const fb::builtin* {

	auto found = _builtins.get(name);
	return found ? &*found : nullptr;
}

auto fb::interpreter::bind(const ca::sv::str name, fb::value* const v) noexcept -> void {

	auto existing = _bound.get(name);
	if (existing) {
		*existing = v;
		return;
	}
	self::_must(_bound.set(_memory.copy(name), v));
}

auto fb::interpreter::class_named(const ca::sv::str name) noexcept -> fb::class_info* {

	auto existing = _classes.get(name);
	if (existing)
		return *existing;
	const ca::sv::str kept = _memory.copy(name);
	fb::class_info* const cls = _memory.make<fb::class_info>(kept, _memory.list<ca::sv::str>(), _memory.list<fb::value*>());
	self::_must(_classes.set(kept, cls));
	return cls;
}

auto fb::interpreter::set_method(fb::class_info* const cls, const ca::sv::str name, fb::value* const callable) noexcept -> void {

	for (ca::usz i = 0U; i < cls->names.size(); ++i)
		if (cls->names.data()[i] == name) {
			cls->methods.data()[i] = callable;
			return;
		}
	_memory.push(cls->names, _memory.copy(name));
	_memory.push(cls->methods, callable);
}

auto fb::interpreter::method(fb::class_info* const cls, const ca::sv::str name, const fb::native fn) noexcept -> void {
	this->set_method(cls, name, this->native_value(fn));
}

auto fb::interpreter::object(fb::class_info* const cls, void* const data) noexcept -> fb::value* {

	fb::value* const o = this->make(fb::kind::object);
	o->cls = cls;
	o->data = data;
	return o;
}

auto fb::interpreter::make(const fb::kind k) noexcept -> fb::value* {
	return _memory.make<fb::value>(k, _memory.allocator());
}

auto fb::interpreter::integer(const ca::i64 v) noexcept -> fb::value* {

	fb::value* const r = this->make(fb::kind::integer);
	r->integer = v;
	return r;
}

auto fb::interpreter::floating(const ca::f64 v) noexcept -> fb::value* {

	fb::value* const r = this->make(fb::kind::floating);
	r->floating = v;
	return r;
}

auto fb::interpreter::boolean(const bool v) noexcept -> fb::value* {

	fb::value* const r = this->make(fb::kind::boolean);
	r->integer = v ? 1 : 0;
	return r;
}

auto fb::interpreter::string(const ca::sv::str v) noexcept -> fb::value* {
	return this->string_in_place(_memory.copy(v));
}

auto fb::interpreter::file(const ca::sv::str v) noexcept -> fb::value* {
	return this->file_in_place(_memory.copy(v));
}

auto fb::interpreter::string_in_place(const ca::sv::str v) noexcept -> fb::value* {

	fb::value* const r = this->make(fb::kind::string);
	r->text = v;
	return r;
}

auto fb::interpreter::file_in_place(const ca::sv::str v) noexcept -> fb::value* {

	fb::value* const r = this->make(fb::kind::file);
	r->text = v;
	return r;
}

auto fb::interpreter::native_value(const fb::native fn) noexcept -> fb::value* {

	fb::value* const r = this->make(fb::kind::native);
	r->fn = fn;
	return r;
}

auto fb::interpreter::push(fb::value* const sequence, fb::value* const element) noexcept -> void {
	_memory.push(sequence->items, element);
}

auto fb::interpreter::set(fb::value* const mapping, const ca::sv::str key, fb::value* const v) noexcept -> void {

	for (ca::usz i = 0U; i < mapping->keys.size(); ++i)
		if (mapping->keys.data()[i] == key) {
			mapping->items.data()[i] = v;
			return;
		}
	_memory.push(mapping->keys, key);
	_memory.push(mapping->items, v);
}

auto fb::interpreter::get(const fb::value* const mapping, const ca::sv::str key) const noexcept -> fb::value* {

	fb::value* const found = mapping != nullptr ? mapping->find(key) : nullptr;
	return found != nullptr ? found : _unit;
}

auto fb::interpreter::force(fb::value* const thunk) noexcept -> fb::outcome {

	if (thunk != nullptr && thunk->is(fb::kind::thunk))
		return this->_eval(thunk->body, thunk->scope);
	return thunk;
}

auto fb::interpreter::apply(fb::value* const fn, const fb::arguments args) noexcept -> fb::outcome {

	if (args.size() == 1U)
		return this->apply(fn, args[0U]);
	fb::value* const tuple = this->make(fb::kind::tuple);
	for (fb::value* const a : args)
		this->push(tuple, a);
	return this->apply(fn, tuple);
}

auto fb::interpreter::apply(fb::value* const fn, fb::value* const arg) noexcept -> fb::outcome {

	if (fn == nullptr)
		return _unit;
	switch (fn->kind) {
		case fb::kind::builtin: {
			const fb::builtin* const b = this->find_builtin(fn->text);
			if (b == nullptr || b->min > 1 || (b->max >= 0 && b->max < 1))
				return _unit;
			fb::value* one[1] = {arg};
			return this->_dispatch(*b, fb::arguments{one, 1U});
		}
		case fb::kind::lambda: {
			if (fb::interrupt::pending() != 0) [[unlikely]]
				return this->fail("interrupted by user");
			if (_stack_top != nullptr && static_cast<ca::usz>(_stack_top - static_cast<const char*>(ca::impl::frame_address())) > _stack_room) [[unlikely]]
				return this->fail("recursion too deep: the stack ({} KiB) is nearly full", (_stack_room + 512U * 1024U) / 1024U);
			fb::environment* const frame = _memory.make<fb::environment>(nullptr, nullptr, fn->scope);
			this->_bind_params(frame, fn->params, arg);
			return this->_eval(fn->body, frame);
		}
		case fb::kind::matcher:
			if (fb::interrupt::pending() != 0) [[unlikely]]
				return this->fail("interrupted by user");
			return this->_eval_match(fn->body, arg, fn->scope);
		case fb::kind::morph:
			if (!arg->is(fb::kind::file))
				return _unit;
			return this->file_in_place(fb::substitute(_memory, fn->text, fn->other, arg->text));
		case fb::kind::native:
			if (arg->is(fb::kind::tuple))
				return fn->fn(*this, fb::arguments{arg->items.data(), arg->size()});
			else {
				fb::value* one[1] = {arg};
				return fn->fn(*this, fb::arguments{one, 1U});
			}
		default:
			return _unit;
	}
}

// -- environments -----------------------------------------------------------

auto fb::interpreter::_put(fb::environment* const scope, const ca::sv::str name, fb::value* const v) noexcept -> void {
	scope->head = _memory.make<fb::binding>(name, v, scope->head);
}

auto fb::interpreter::_lookup(const fb::environment* scope, const ca::sv::str name) noexcept -> fb::value* {

	for (; scope != nullptr; scope = scope->parent)
		for (const fb::binding* b = scope->head; b != nullptr; b = b->next)
			if (b->name == name)
				return b->value;
	return nullptr;
}

auto fb::interpreter::_current_arg(const fb::environment* scope) noexcept -> fb::value* {

	for (; scope != nullptr; scope = scope->parent)
		if (scope->arg != nullptr)
			return scope->arg;
	return nullptr;
}

auto fb::interpreter::_positional(fb::value* const arg, const ca::usz index) const noexcept -> fb::value* {

	if (arg != nullptr && arg->is_sequence() && index < arg->size())
		return arg->at(index);
	return _unit;
}

auto fb::interpreter::_bind_params(fb::environment* const frame, const fb::node* const params, fb::value* const arg) noexcept -> void {

	frame->arg = arg;
	const ca::usz count = params->size();
	for (ca::usz i = 0U; i < count; ++i) {
		const fb::node* const p = params->kid(i);
		fb::value* const slot = count == 1U ? arg : this->_positional(arg, i);
		switch (p->kind) {
			case fb::node_kind::id:
			case fb::node_kind::projection:
				if (p->tok != nullptr)
					this->_put(frame, p->tok->text, slot);
				break;
			case fb::node_kind::capture:
				if (p->size() != 0U) {
					fb::value* rest;
					if (arg->is_sequence()) {
						rest = this->make(fb::kind::tuple);
						for (ca::usz j = i; j < arg->size(); ++j)
							this->push(rest, arg->at(j));
					} else
						rest = i == 0U ? arg : _unit;
					this->_put(frame, p->kid(0U)->tok->text, rest);
				}
				break;
			case fb::node_kind::tuple:
				for (const fb::node* const sub : p->kids)
					if (sub->tok != nullptr)
						this->_put(frame, sub->tok->text, this->get(slot, sub->tok->text));
				break;
			default:
				break;
		}
	}
}

auto fb::interpreter::_definition(const ca::sv::str name, const bool memoized) noexcept -> ca::result<fb::value*, fb::diagnostic> {

	auto found = _definitions.get(name);
	if (!found)
		return static_cast<fb::value*>(nullptr);
	definition& d = *found;
	if (memoized && d.pure && d.value != nullptr)
		return d.value;
	if (d.doing)
		return this->fail("cyclic definition '{}'", name);
	d.doing = true;
	auto v = this->_eval(d.rhs, &_top);
	d.doing = false;
	if (!v)
		return v.propagate();
	if ((*v)->label.empty())
		(*v)->label = name;
	if (memoized && d.pure)
		d.value = *v;
	return *v;
}

auto fb::interpreter::_dispatch(const fb::builtin& b, const fb::arguments args) noexcept -> fb::outcome {

	const ca::usz count = args.size();
	if (static_cast<ca::i64>(count) < b.min || (b.max >= 0 && static_cast<ca::i64>(count) > b.max))
		return this->fail("{}: expects {}..{} args, got {}", b.name, b.min, b.max, count);
	return b.fn(*this, args);
}

// -- evaluation -------------------------------------------------------------

auto fb::interpreter::_guarded_scope(const fb::node* const where, fb::environment* const scope) noexcept -> ca::result<fb::environment*, fb::diagnostic> {

	if (where == nullptr)
		return scope;
	fb::environment* const inner = _memory.make<fb::environment>(nullptr, self::_current_arg(scope), scope);
	for (const fb::node* const binding : where->kids) {
		auto v = this->_eval(binding->kid(0U), inner);
		if (!v)
			return v.propagate();
		this->_put(inner, binding->tok->text, *v);
	}
	return inner;
}

auto fb::interpreter::_add_element(fb::value* const into, const fb::node* const element, fb::environment* const scope) noexcept -> ca::result<void, fb::diagnostic> {

	if (element->kind == fb::node_kind::guard) {
		auto inner = this->_guarded_scope(element->kid(1U), scope);
		if (!inner)
			return inner.propagate();
		if (element->kid(2U) != nullptr) {
			auto condition = this->_eval(element->kid(2U)->kid(0U), *inner);
			if (!condition)
				return condition.propagate();
			if (!(*condition)->truthy())
				return element->kid(3U) != nullptr ? this->_add_element(into, element->kid(3U)->kid(0U), *inner) : ca::result<void, fb::diagnostic>{};
		}
		return this->_add_element(into, element->kid(0U), *inner);
	}
	if (element->kind == fb::node_kind::splat) {
		auto v = this->_eval(element->kid(0U), scope);
		if (!v)
			return v.propagate();
		if ((*v)->is_sequence())
			for (fb::value* const item : (*v)->items)
				this->push(into, item);
		else if (!(*v)->is(fb::kind::unit))
			this->push(into, *v);
		return {};
	}
	auto v = this->_eval(element, scope);
	if (!v)
		return v.propagate();
	if (!(*v)->is(fb::kind::unit))
		this->push(into, *v);
	return {};
}

auto fb::interpreter::_eval_arguments(const fb::node* const call, fb::environment* const scope) noexcept -> ca::result<fb::list<fb::value*>, fb::diagnostic> {

	fb::list<fb::value*> out = _memory.list<fb::value*>();
	for (ca::usz i = 1U; i < call->size(); ++i) {
		const fb::node* const a = call->kid(i);
		const bool splat = a->kind == fb::node_kind::splat;
		auto v = this->_eval(splat ? a->kid(0U) : a, scope);
		if (!v)
			return v.propagate();
		if (splat && (*v)->is_sequence())
			for (fb::value* const item : (*v)->items)
				_memory.push(out, item);
		else
			_memory.push(out, *v);
	}
	return out;
}

auto fb::interpreter::_eval_number(const fb::node* const n, fb::environment* const scope, ca::i64& integer, ca::f64& floating, bool& is_float) noexcept -> ca::result<void, fb::diagnostic> {

	if (n->kind == fb::node_kind::integer) {
		integer = fb::parse_integer_prefix(n->tok->text);
		is_float = false;
		return {};
	}
	if (n->kind != fb::node_kind::arith) {
		auto v = this->_eval(n, scope);
		if (!v)
			return v.propagate();
		is_float = (*v)->is(fb::kind::floating);
		integer = (*v)->integer;
		floating = (*v)->floating;
		return {};
	}
	ca::i64 ai = 0, bi = 0;
	ca::f64 af = 0.0, bf = 0.0;
	bool a_float = false, b_float = false;
	auto left = this->_eval_number(n->kid(0U), scope, ai, af, a_float);
	if (!left)
		return left;
	auto right = this->_eval_number(n->kid(1U), scope, bi, bf, b_float);
	if (!right)
		return right;
	const fb::tok op = n->tok->kind;
	if (a_float || b_float) {
		const ca::f64 x = a_float ? af : static_cast<ca::f64>(ai);
		const ca::f64 y = b_float ? bf : static_cast<ca::f64>(bi);
		switch (op) {
			case fb::tok::plus: floating = x + y; break;
			case fb::tok::minus: floating = x - y; break;
			case fb::tok::star: floating = x * y; break;
			case fb::tok::slash: floating = y != 0.0 ? x / y : 0.0; break;
			default: floating = y != 0.0 ? fb::impl::fmod(x, y) : 0.0; break;
		}
		is_float = true;
		return {};
	}
	switch (op) {
		case fb::tok::plus: integer = static_cast<ca::i64>(static_cast<ca::u64>(ai) + static_cast<ca::u64>(bi)); break;
		case fb::tok::minus: integer = static_cast<ca::i64>(static_cast<ca::u64>(ai) - static_cast<ca::u64>(bi)); break;
		case fb::tok::star: integer = static_cast<ca::i64>(static_cast<ca::u64>(ai) * static_cast<ca::u64>(bi)); break;
		case fb::tok::slash: integer = bi != 0 && !(bi == -1 && ai == ca::limits<ca::i64>::min()) ? ai / bi : 0; break;
		default: integer = bi != 0 && bi != -1 ? ai % bi : 0; break;
	}
	is_float = false;
	return {};
}

auto fb::interpreter::_eval_string(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	if (n->size() == 0U)
		return this->string_in_place(fb::impl::unescape(_memory, n->tok->text));
	fb::text out{_memory.allocator()};
	for (const fb::node* const part : n->kids) {
		auto v = this->_eval(part, scope);
		if (!v)
			return v.propagate();
		self::_must(out.append(this->text(*v)));
	}
	return this->string_in_place(_memory.copy(out.view()));
}

auto fb::interpreter::_eval_store(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	const fb::node* const target = n->kid(0U);
	auto rhs = this->_eval(n->kid(1U), scope);
	if (!rhs)
		return rhs;
	if (target->kind != fb::node_kind::field)
		return this->fail("<- target is not a writable field");
	auto base = this->_eval(target->kid(0U), scope);
	if (!base)
		return base;
	if (!(*base)->is_mapping())
		return this->fail("<- target base is not a map");
	ca::sv::str key;
	if (target->tok == nullptr && target->size() == 2U) {
		auto index = this->_eval(target->kid(1U), scope);
		if (!index)
			return index;
		key = _memory.copy(this->text(*index));
	} else
		key = target->tok->text;
	this->set(*base, key, *rhs);
	return *rhs;
}

auto fb::interpreter::_eval_id(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	const ca::sv::str name = n->tok->text;
	if (fb::value* const local = self::_lookup(scope, name))
		return local;
	auto defined = this->_definition(name, true);
	if (!defined)
		return defined.propagate();
	if (*defined != nullptr)
		return *defined;
	if (auto bound = _bound.get(name))
		return *bound;
	if (this->find_builtin(name) != nullptr) {
		fb::value* const v = this->make(fb::kind::builtin);
		v->text = name;
		return v;
	}
	return this->string_in_place(name);
}

auto fb::interpreter::_eval_sequence(const fb::node* const n, fb::environment* const scope, const fb::kind k) noexcept -> fb::outcome {

	fb::value* const out = this->make(k);
	for (const fb::node* const element : n->kids) {
		auto added = this->_add_element(out, element, scope);
		if (!added)
			return added.propagate();
	}
	return out;
}

auto fb::interpreter::_eval_map(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	fb::value* const m = this->make(fb::kind::map);
	for (const fb::node* const entry : n->kids) {
		if (entry->kind == fb::node_kind::splat) {
			auto source = this->_eval(entry->kid(0U), scope);
			if (!source)
				return source;
			if (!(*source)->is_mapping())
				return this->fail("...: cannot spread a non-map/object value into a map literal");
			for (ca::usz j = 0U; j < (*source)->keys.size(); ++j)
				this->set(m, (*source)->keys.data()[j], (*source)->at(j));
			continue;
		}
		auto pair = this->_eval(entry, scope);
		if (!pair)
			return pair;
		if ((*pair)->is_sequence() && (*pair)->size() >= 2U)
			this->set(m, _memory.copy(this->text((*pair)->at(0U))), (*pair)->at(1U));
	}
	return m;
}

auto fb::interpreter::_eval_pipe(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	auto first = this->_eval(n->kid(0U), scope);
	if (!first)
		return first;
	fb::value* acc = *first;
	for (ca::usz i = 1U; i < n->size(); ++i) {
		auto fn = this->_eval(n->kid(i), scope);
		if (!fn)
			return fn;
		if (acc->is_sequence()) {
			fb::value* const out = this->make(fb::kind::list);
			for (fb::value* const item : acc->items) {
				auto mapped = this->apply(*fn, item);
				if (!mapped)
					return mapped;
				this->push(out, *mapped);
			}
			acc = out;
		} else {
			auto applied = this->apply(*fn, acc);
			if (!applied)
				return applied;
			acc = *applied;
		}
	}
	return acc;
}

auto fb::interpreter::_eval_guard(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	auto inner = this->_guarded_scope(n->kid(1U), scope);
	if (!inner)
		return inner.propagate();
	if (n->kid(2U) != nullptr) {
		auto condition = this->_eval(n->kid(2U)->kid(0U), *inner);
		if (!condition)
			return condition;
		if (!(*condition)->truthy())
			return n->kid(3U) != nullptr ? this->_eval(n->kid(3U)->kid(0U), *inner) : _unit;
	}
	return this->_eval(n->kid(0U), *inner);
}

auto fb::interpreter::_has_type(const fb::value* const v, const fb::node* const annotation) noexcept -> bool {

	if (annotation->kind == fb::node_kind::typeunion) {
		for (const fb::node* const member : annotation->kids)
			if (this->_has_type(v, member))
				return true;
		return false;
	}
	const ca::sv::str name = annotation->tok->text;
	if (name == "Int") return v->is(fb::kind::integer);
	if (name == "Float") return v->is(fb::kind::floating);
	if (name == "Number") return v->is(fb::kind::integer) || v->is(fb::kind::floating);
	if (name == "String") return v->is(fb::kind::string);
	if (name == "Bool") return v->is(fb::kind::boolean);
	if (name == "Path" || name == "File") return v->is(fb::kind::file);
	if (name == "Command") return v->is(fb::kind::command);
	if (name == "Unit") return v->is(fb::kind::unit);
	if (name == "Map") return v->is(fb::kind::map);
	if (name == "Json") return true;
	if (name == "List") {
		if (!v->is(fb::kind::list))
			return false;
		if (annotation->size() != 0U)
			for (const fb::value* const item : v->items)
				if (!this->_has_type(item, annotation->kid(0U)))
					return false;
		return true;
	}
	if (name == "Tuple") {
		if (!v->is(fb::kind::tuple))
			return false;
		if (annotation->size() == 0U)
			return true;
		if (v->size() != annotation->size())
			return false;
		for (ca::usz i = 0U; i < v->size(); ++i)
			if (!this->_has_type(v->at(i), annotation->kid(i)))
				return false;
		return true;
	}
	return v->is(fb::kind::object) && v->cls != nullptr && v->cls->name == name;
}

auto fb::interpreter::_matches(const fb::node* const pattern, fb::value* const v, fb::environment* const frame, fb::environment* const scope) noexcept
	-> ca::result<bool, fb::diagnostic> {

	switch (pattern->kind) {
		case fb::node_kind::pwild:
			return true;
		case fb::node_kind::pbind:
			if (pattern->size() != 0U && !this->_has_type(v, pattern->kid(0U)))
				return false;
			this->_put(frame, pattern->tok->text, v);
			return true;
		case fb::node_kind::plit: {
			auto literal = this->_eval(pattern->kid(0U), scope);
			if (!literal)
				return literal.propagate();
			const fb::value* const l = *literal;
			const auto number = [](const fb::value* const x) noexcept {
				return x->is(fb::kind::integer) || x->is(fb::kind::floating);
			};
			const auto texty = [](const fb::value* const x) noexcept {
				return x->is(fb::kind::string) || x->is(fb::kind::file);
			};
			if (number(l) && number(v))
				return l->is(fb::kind::floating) || v->is(fb::kind::floating) ? l->as_float() == v->as_float() : l->integer == v->integer;
			if (texty(l) && texty(v))
				return l->text == v->text;
			if (l->kind != v->kind)
				return false;
			return l->is(fb::kind::boolean) ? l->integer == v->integer : l->is(fb::kind::unit);
		}
		case fb::node_kind::ptuple: {
			if (!v->is_sequence() || v->size() != pattern->size())
				return false;
			for (ca::usz i = 0U; i < pattern->size(); ++i) {
				auto sub = this->_matches(pattern->kid(i), v->at(i), frame, scope);
				if (!sub || !*sub)
					return sub;
			}
			return true;
		}
		default:
			return false;
	}
}

/* The first arm whose pattern takes arg (and whose condition holds), evaluated with the
   pattern's names bound; unit when none does. */
auto fb::interpreter::_eval_match(const fb::node* const n, fb::value* const arg, fb::environment* const scope) noexcept -> fb::outcome {

	if (_stack_top != nullptr && static_cast<ca::usz>(_stack_top - static_cast<const char*>(ca::impl::frame_address())) > _stack_room) [[unlikely]]
		return this->fail("recursion too deep: the stack ({} KiB) is nearly full", (_stack_room + 512U * 1024U) / 1024U);
	for (const fb::node* const arm : n->kids) {
		fb::environment* const frame = _memory.make<fb::environment>(nullptr, arg, scope);
		auto matched = this->_matches(arm->kid(0U), arg, frame, scope);
		if (!matched)
			return matched.propagate();
		if (!*matched)
			continue;
		if (const fb::node* const condition = arm->kid(1U)) {
			auto holds = this->_eval(condition, frame);
			if (!holds)
				return holds;
			if (!(*holds)->truthy())
				continue;
		}
		return this->_eval(arm->kid(2U), frame);
	}
	return _unit;
}

auto fb::interpreter::_eval_field(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	auto evaluated = this->_eval(n->kid(0U), scope);
	if (!evaluated)
		return evaluated;
	fb::value* const base = *evaluated;
	if (n->tok == nullptr && n->size() == 2U) {
		auto key = this->_eval(n->kid(1U), scope);
		if (!key)
			return key;
		if (base->is_mapping())
			return this->get(base, this->text(*key));
		if ((*key)->is(fb::kind::integer))
			return (*key)->integer < 0 ? _unit : this->_positional(base, static_cast<ca::usz>((*key)->integer));
		return fb::file_field(*this, base, this->text(*key));
	}
	const ca::sv::str name = n->tok->text;
	if (base->is(fb::kind::object)) {
		fb::value* const m = base->cls != nullptr ? base->cls->method(name) : nullptr;
		return m != nullptr ? m : this->get(base, name);
	}
	if (base->is(fb::kind::map))
		return this->get(base, name);
	const ca::i64 index = fb::impl::index_of(n->tok);
	if (index >= 0)
		return this->_positional(base, static_cast<ca::usz>(index));
	return fb::file_field(*this, base, name);
}

auto fb::interpreter::_eval_object(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	fb::class_info* const cls = this->class_named(n->tok->text);
	auto fields = this->_eval(n->kid(0U), scope);
	if (!fields)
		return fields;
	fb::value* const o = this->object(cls, nullptr);
	if ((*fields)->is_mapping())
		for (ca::usz i = 0U; i < (*fields)->keys.size(); ++i) {
			_memory.push(o->keys, (*fields)->keys.data()[i]);
			_memory.push(o->items, (*fields)->at(i));
		}
	return o;
}

auto fb::interpreter::_eval_method_copy(const fb::node* const n) noexcept -> fb::outcome {

	const ca::sv::str destination_name = n->kid(0U)->tok->text;
	const ca::sv::str source_name = n->kid(1U)->tok->text;
	fb::class_info* const destination = this->class_named(destination_name);
	fb::class_info* const source = this->class_named(source_name);
	if (n->tok != nullptr) {
		const ca::sv::str method = n->kid(2U)->tok->text;
		fb::value* const m = source->method(method);
		if (m == nullptr)
			return this->fail("method {}.{} = {}.{}: {} has no method '{}'", destination_name, n->tok->text, source_name, method, source_name, method);
		this->set_method(destination, n->tok->text, m);
		return _unit;
	}
	for (ca::usz i = 0U; i < source->names.size(); ++i)
		this->set_method(destination, source->names.data()[i], source->methods.data()[i]);
	return _unit;
}

auto fb::interpreter::_eval_construct(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	if (n->tok->text != "File")
		return _unit;
	const fb::value* base = nullptr;
	ca::optional<ca::sv::str> name, stem, suffix, parent;
	for (const fb::node* const field : n->kids) {
		auto v = this->_eval(field->kid(0U), scope);
		if (!v)
			return v;
		if (field->tok == nullptr) {
			if ((*v)->is(fb::kind::file))
				base = *v;
			continue;
		}
		const ca::sv::str key = field->tok->text;
		if (key == "name")
			name = this->text(*v);
		else if (key == "stem")
			stem = this->text(*v);
		else if (key == "suffix")
			suffix = this->text(*v);
		else if (key == "parent")
			parent = this->text(*v);
	}
	const ca::sv::str from = base != nullptr ? base->text : ca::sv::str{""};
	return this->file_in_place(fb::rebuild_path(_memory, from, name, stem, suffix, parent));
}

auto fb::interpreter::_eval_compare(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	auto left = this->_eval(n->kid(0U), scope);
	if (!left)
		return left;
	auto right = this->_eval(n->kid(1U), scope);
	if (!right)
		return right;
	const fb::value* const l = *left;
	const fb::value* const r = *right;
	const fb::tok op = n->tok->kind;
	if (l->is_number() && r->is_number()) {
		if (l->is(fb::kind::floating) || r->is(fb::kind::floating))
			return this->boolean(fb::impl::compared(l->as_float(), r->as_float(), op));
		return this->boolean(fb::impl::compared(l->integer, r->integer, op));
	}
	const int order = this->text(l).compare(this->text(r));
	return this->boolean(fb::impl::compared(order, 0, op));
}

auto fb::interpreter::_eval_logical(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	auto left = this->_eval(n->kid(0U), scope);
	if (!left)
		return left;
	const fb::tok op = n->tok->kind;
	const bool truthy = (*left)->truthy();
	if (op == fb::tok::oror && truthy)
		return left;
	if ((op == fb::tok::and_ && !truthy) || (op == fb::tok::or_ && truthy))
		return this->boolean(truthy);
	auto right = this->_eval(n->kid(1U), scope);
	if (!right || op == fb::tok::oror)
		return right;
	return this->boolean((*right)->truthy());
}

auto fb::interpreter::_eval_command(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	fb::value* const command = this->make(fb::kind::command);
	for (const fb::node* const part : n->kids) {
		if (part->kind == fb::node_kind::cmdtext) {
			fb::value* const word = this->string_in_place(fb::command_word(_memory, part->tok->text));
			word->other = part->tok->text;
			this->push(command, word);
			continue;
		}
		const bool splat = part->kind == fb::node_kind::splat;
		auto v = this->_eval(splat ? part->kid(0U) : part, scope);
		if (!v)
			return v;
		if (splat && (*v)->is_sequence())
			for (fb::value* const item : (*v)->items)
				this->push(command, item);
		else
			this->push(command, *v);
	}
	return command;
}

auto fb::interpreter::_eval_call(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	if (fb::interrupt::pending() != 0) [[unlikely]]
		return this->fail("interrupted by user");
	const fb::node* const callee = n->kid(0U);
	if (callee->kind == fb::node_kind::parser) {
		auto schema = this->_eval(callee->kid(0U), scope);
		if (!schema)
			return schema;
		fb::value* argv = _args;
		if (n->size() >= 2U) {
			auto given = this->_eval(n->kid(1U), scope);
			if (!given)
				return given;
			argv = *given;
		}
		return fb::parse_arguments(*this, *schema, argv);
	}
	if (callee->kind == fb::node_kind::field && callee->tok != nullptr && callee->size() == 1U) {
		auto receiver = this->_eval(callee->kid(0U), scope);
		if (!receiver)
			return receiver;
		fb::value* const self_value = *receiver;
		fb::value* const m = self_value->is(fb::kind::object) && self_value->cls != nullptr ? self_value->cls->method(callee->tok->text) : nullptr;
		if (m != nullptr) {
			auto args = this->_eval_arguments(n, scope);
			if (!args)
				return args.propagate();
			fb::value* arg;
			if (m->is(fb::kind::lambda) && m->params->size() > 1U) {
				arg = this->make(fb::kind::tuple);
				for (const fb::node* const p : m->params->kids)
					this->push(arg, p->kind == fb::node_kind::id && p->tok != nullptr ? this->get(self_value, p->tok->text) : _unit);
			} else if (args->empty()) {
				arg = self_value;
			} else {
				arg = this->make(fb::kind::tuple);
				this->push(arg, self_value);
				for (fb::value* const a : *args)
					this->push(arg, a);
			}
			return this->apply(m, arg);
		}
	}
	if (callee->kind == fb::node_kind::id && self::_lookup(scope, callee->tok->text) == nullptr
		&& !_definitions.contains(callee->tok->text)) {
		if (const fb::builtin* const b = this->find_builtin(callee->tok->text)) {
			if ((b->flags & fb::lazy) != 0U) {
				fb::list<fb::value*> thunks = _memory.list<fb::value*>();
				for (ca::usz i = 1U; i < n->size(); ++i) {
					fb::value* const t = this->make(fb::kind::thunk);
					t->body = n->kid(i);
					t->scope = scope;
					_memory.push(thunks, t);
				}
				return this->_dispatch(*b, fb::arguments{thunks.data(), thunks.size()});
			}
			auto args = this->_eval_arguments(n, scope);
			if (!args)
				return args.propagate();
			auto result = this->_dispatch(*b, fb::arguments{args->data(), args->size()});
			if (!result && (b->flags & fb::located) != 0U && result.error().at.line == 0)
				return ca::failure<fb::diagnostic>{fb::diagnostic{result.error().message, callee->tok->start}};
			return result;
		}
	}
	auto fn = this->_eval(callee, scope);
	if (!fn)
		return fn;
	auto args = this->_eval_arguments(n, scope);
	if (!args)
		return args.propagate();
	return this->apply(*fn, fb::arguments{args->data(), args->size()});
}

auto fb::interpreter::_eval(const fb::node* const n, fb::environment* const scope) noexcept -> fb::outcome {

	switch (n->kind) {
		case fb::node_kind::integer:
			return this->integer(fb::parse_integer_prefix(n->tok->text));
		case fb::node_kind::floating:
			return this->floating(fb::parse_float_prefix(n->tok->text));
		case fb::node_kind::boolean:
			return this->boolean(n->tok->text == "true");
		case fb::node_kind::nil:
			return _unit;
		case fb::node_kind::strtext:
			return this->string_in_place(fb::impl::unescape(_memory, n->tok->text));
		case fb::node_kind::string:
			return this->_eval_string(n, scope);
		case fb::node_kind::path:
			return this->file_in_place(n->tok->text);
		case fb::node_kind::eager: {
			auto v = this->_eval(n->kid(0U), scope);
			if (!v)
				return v;
			if ((*v)->label.empty())
				(*v)->label = n->tok->text;
			this->_put(scope, n->tok->text, *v);
			return v;
		}
		case fb::node_kind::store:
			return this->_eval_store(n, scope);
		case fb::node_kind::force: {
			const fb::node* const operand = n->kid(0U);
			if (operand->kind == fb::node_kind::id) {
				if (fb::value* const local = self::_lookup(scope, operand->tok->text))
					return local;
				auto forced = this->_definition(operand->tok->text, false);
				if (!forced)
					return forced.propagate();
				if (*forced != nullptr)
					return *forced;
			}
			return this->_eval(operand, scope);
		}
		case fb::node_kind::id:
			return this->_eval_id(n, scope);
		case fb::node_kind::lambda: {
			fb::value* const v = this->make(fb::kind::lambda);
			v->params = n->kid(0U);
			v->body = n->kid(1U);
			v->scope = scope;
			return v;
		}
		case fb::node_kind::match_: {
			fb::value* const v = this->make(fb::kind::matcher);
			v->body = n;
			v->scope = scope;
			return v;
		}
		case fb::node_kind::morphism: {
			fb::value* const v = this->make(fb::kind::morph);
			v->text = n->kid(0U)->tok != nullptr ? n->kid(0U)->tok->text : ca::sv::str{""};
			v->other = n->kid(1U)->tok != nullptr ? n->kid(1U)->tok->text : ca::sv::str{""};
			return v;
		}
		case fb::node_kind::tuple:
			return this->_eval_sequence(n, scope, fb::kind::tuple);
		case fb::node_kind::listlit:
			return this->_eval_sequence(n, scope, fb::kind::list);
		case fb::node_kind::maplit:
			return this->_eval_map(n, scope);
		case fb::node_kind::parser: {
			auto schema = this->_eval(n->kid(0U), scope);
			if (!schema)
				return schema;
			return fb::parse_arguments(*this, *schema, _args);
		}
		case fb::node_kind::pipe:
			return this->_eval_pipe(n, scope);
		case fb::node_kind::seq: {
			fb::value* const v = this->make(fb::kind::seq);
			v->body = n;
			v->scope = scope;
			return v;
		}
		case fb::node_kind::guard:
			return this->_eval_guard(n, scope);
		case fb::node_kind::field:
			return this->_eval_field(n, scope);
		case fb::node_kind::objnew:
			return this->_eval_object(n, scope);
		case fb::node_kind::methoddef: {
			fb::class_info* const cls = this->class_named(n->kid(0U)->tok->text);
			auto m = this->_eval(n->kid(1U), scope);
			if (!m)
				return m;
			this->set_method(cls, n->tok->text, *m);
			return _unit;
		}
		case fb::node_kind::methodcopy:
			return this->_eval_method_copy(n);
		case fb::node_kind::projection: {
			if (n->tok != nullptr)
				if (fb::value* const local = self::_lookup(scope, n->tok->text))
					return local;
			const ca::i64 index = fb::impl::index_of(n->tok);
			fb::value* const arg = self::_current_arg(scope);
			if (index >= 0)
				return this->_positional(arg, static_cast<ca::usz>(index));
			return fb::file_field(*this, arg, n->tok->text);
		}
		case fb::node_kind::capture: {
			if (n->size() != 0U)
				if (fb::value* const local = self::_lookup(scope, n->kid(0U)->tok->text))
					return local;
			fb::value* const arg = self::_current_arg(scope);
			return arg != nullptr ? arg : _unit;
		}
		case fb::node_kind::construct:
			return this->_eval_construct(n, scope);
		case fb::node_kind::compare:
			return this->_eval_compare(n, scope);
		case fb::node_kind::arith: {
			ca::i64 i = 0;
			ca::f64 f = 0.0;
			bool is_float = false;
			auto done = this->_eval_number(n, scope, i, f, is_float);
			if (!done)
				return done.propagate();
			return is_float ? this->floating(f) : this->integer(i);
		}
		case fb::node_kind::neg: {
			auto v = this->_eval(n->kid(0U), scope);
			if (!v)
				return v;
			if ((*v)->is(fb::kind::floating))
				return this->floating(-(*v)->floating);
			return this->integer(static_cast<ca::i64>(0U - static_cast<ca::u64>((*v)->integer)));
		}
		case fb::node_kind::logical:
			return this->_eval_logical(n, scope);
		case fb::node_kind::not_: {
			auto v = this->_eval(n->kid(0U), scope);
			if (!v)
				return v;
			return this->boolean(!(*v)->truthy());
		}
		case fb::node_kind::command:
			return this->_eval_command(n, scope);
		case fb::node_kind::call:
			return this->_eval_call(n, scope);
		case fb::node_kind::assign:
			return this->_eval(n->kid(0U), scope);
		default:
			return this->fail("cannot execute node '{}'", fb::node_name(n->kind));
	}
}
