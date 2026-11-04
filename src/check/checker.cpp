#include "check/checker.hpp"

#include "ca/hashmap.hpp"

namespace fb::check {

	class checker final {

		private:

			using self = fb::check::checker;
			using form = fb::check::form;
			using type = fb::check::type;

			struct local final {
				ca::sv::str name;
				type* t;
				const fb::node* site;  // the parameter, binding or pattern naming it
				local* next;
			};

			struct scope final {
				local* head;
				type* arg;      // the implicit argument (projections, captures)
				scope* parent;
			};

			struct definition final {
				const fb::node* statement;
				type* t;
				bool doing;
			};

			struct method final {
				ca::sv::str name;
				ca::sv::str owner;
				const fb::node* site;
				method* source;  // a copied method: the one it copies
				type* t;
				bool doing;
			};

			struct klass final {
				ca::sv::str name;
				fb::list<method*> methods;

				[[nodiscard]] auto find(const ca::sv::str key) const noexcept -> method* {

					for (method* const m : methods)
						if (m->name == key)
							return m;
					return nullptr;
				}
			};

			/* A call's arguments: their types, their nodes (for lambdas to apply in place), and
			   whether a spread of unknown length makes the positions past it unknown. */
			struct arguments final {
				fb::list<type*> types;
				fb::list<const fb::node*> nodes;
				bool open;
			};

			fb::check::types& _types;
			const fb::check::catalog& _catalog;
			fb::memory& _memory;
			fb::check::report& _report;
			ca::hashmap<ca::sv::str, definition, ca::arena_allocator> _definitions;
			ca::hashmap<ca::sv::str, klass*, ca::arena_allocator> _classes;
			scope _top;

			template <typename T>
			static auto _must(ca::result<T>&& r) noexcept -> void {

				if (!r) [[unlikely]]
					ca::trap("fbuild: out of memory");
			}

			// -- diagnostics ---------------------------------------------------

			auto _diagnose(const fb::check::severity level, const fb::span at, const ca::sv::str message) noexcept -> void {

				for (const fb::check::diagnostic& d : _report._diagnostics)
					if (d.message == message && d.at.start.line == at.start.line && d.at.start.col == at.start.col)
						return;
				_memory.push(_report._diagnostics, fb::check::diagnostic{level, message, at});
				if (level == fb::check::severity::error)
					++_report._errors;
			}

			[[nodiscard]] static auto _span(const fb::token* const t) noexcept -> fb::span {
				return t != nullptr ? fb::span{t->start, t->end} : fb::span{{0, 0}, {0, 0}};
			}

			/* Where a node's problem shows: its own token when it has one (an operator, a name),
			   else all of it. */
			[[nodiscard]] static auto _place(const fb::node* const n) noexcept -> fb::span {

				if (n == nullptr)
					return fb::span{{0, 0}, {0, 0}};
				if (n->tok != nullptr && n->tok->start.line != 0)
					return self::_span(n->tok);
				return fb::span_of(n);
			}

			template <typename... Args>
			auto _error(const fb::node* const at, const ca::format_string<ca::type_identity<Args>...> format, const Args&... args) noexcept -> void {
				this->_diagnose(fb::check::severity::error, self::_place(at), _memory.format(format, args...));
			}

			template <typename... Args>
			auto _error_at(const fb::span at, const ca::format_string<ca::type_identity<Args>...> format, const Args&... args) noexcept -> void {
				this->_diagnose(fb::check::severity::error, at, _memory.format(format, args...));
			}

			template <typename... Args>
			auto _warning(const fb::node* const at, const ca::format_string<ca::type_identity<Args>...> format, const Args&... args) noexcept -> void {
				this->_diagnose(fb::check::severity::warning, self::_place(at), _memory.format(format, args...));
			}

			[[nodiscard]] auto _name(const type* const t) noexcept -> ca::sv::str {
				return fb::check::render(_memory, t);
			}

			auto _record(const fb::node* const n, type* const t) noexcept -> type* {

				if (n == nullptr)
					return t;
				while (_report._types.size() <= n->id)
					_memory.push(_report._types, static_cast<type*>(nullptr));
				_report._types.data()[n->id] = t;
				return t;
			}

			// -- scopes ----------------------------------------------------------

			[[nodiscard]] auto _scope(scope* const parent, type* const arg) noexcept -> scope* {
				return _memory.make<scope>(nullptr, arg, parent);
			}

			auto _put(scope* const s, const ca::sv::str name, type* const t, const fb::node* const site) noexcept -> void {
				s->head = _memory.make<local>(name, t, site, s->head);
			}

			[[nodiscard]] static auto _find(const scope* s, const ca::sv::str name) noexcept -> const local* {

				for (; s != nullptr; s = s->parent)
					for (const local* l = s->head; l != nullptr; l = l->next)
						if (l->name == name)
							return l;
				return nullptr;
			}

			[[nodiscard]] static auto _lookup(const scope* s, const ca::sv::str name) noexcept -> type* {

				const local* const l = self::_find(s, name);
				return l != nullptr ? l->t : nullptr;
			}

			/* Notes that the name at n stands for what site defines. */
			auto _link(const fb::node* const n, const fb::node* const site) noexcept -> void {

				if (n == nullptr || site == nullptr)
					return;
				while (_report._targets.size() <= n->id)
					_memory.push(_report._targets, static_cast<const fb::node*>(nullptr));
				_report._targets.data()[n->id] = site;
			}

			/* Links a name used at n to its local or top-level definition, when it has one. */
			auto _resolve(const fb::node* const n, const ca::sv::str name, const scope* const sc) noexcept -> void {

				if (const local* const l = self::_find(sc, name))
					this->_link(n, l->site);
				else if (auto found = _definitions.get(name))
					this->_link(n, found->statement);
			}

			[[nodiscard]] static auto _arg(const scope* s) noexcept -> type* {

				for (; s != nullptr; s = s->parent)
					if (s->arg != nullptr)
						return s->arg;
				return nullptr;
			}

			[[nodiscard]] static auto _index(const fb::token* const t) noexcept -> ca::i64 {

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

			[[nodiscard]] auto _positional(type* const t, const ca::usz index) noexcept -> type* {

				if (t != nullptr && t->is(form::tuple) && t->sized)
					return index < t->size() ? t->kid(index) : _types.unit();
				if (t != nullptr && t->is(form::list))
					return t->kid(0U);
				return _types.unknown();
			}

			// -- symbols ---------------------------------------------------------

			[[nodiscard]] auto _class(const ca::sv::str name) noexcept -> klass* {

				if (auto existing = _classes.get(name))
					return *existing;
				klass* const k = _memory.make<klass>(name, _memory.list<method*>());
				self::_must(_classes.set(name, k));
				return k;
			}

			[[nodiscard]] auto _find_class(const ca::sv::str name) const noexcept -> klass* {

				auto found = _classes.get(name);
				return found ? *found : nullptr;
			}

			auto _set_method(klass* const k, method* const m) noexcept -> void {

				for (method*& existing : k->methods)
					if (existing->name == m->name) {
						existing = m;
						return;
					}
				_memory.push(k->methods, m);
			}

			[[nodiscard]] static auto _is_class_definition(const fb::node* const statement) noexcept -> bool {

				const fb::node* const rhs = statement->kid(0U);
				return rhs != nullptr && rhs->kind == fb::node_kind::lambda && rhs->kid(1U) != nullptr
					&& rhs->kid(1U)->kind == fb::node_kind::objnew;
			}

			auto _symbol(const ca::sv::str name, const fb::check::symbol_kind kind, const fb::node* const site, const ca::sv::str container) noexcept -> void {
				_memory.push(_report._symbols, fb::check::symbol{name, kind, site, container});
			}

			/* Definitions, classes and methods, in statement order (methods are set when their
			   statement runs, so a copy sees only what is defined above it). */
			auto _collect(const fb::node* const program) noexcept -> void {

				for (const fb::node* const statement : program->kids) {
					switch (statement->kind) {
						case fb::node_kind::assign:
						case fb::node_kind::eager: {
							const ca::sv::str name = statement->tok->text;
							if (!_definitions.contains(name) && statement->kind == fb::node_kind::assign)
								self::_must(_definitions.set(name, definition{statement, nullptr, false}));
							const bool cls = self::_is_class_definition(statement);
							if (cls)
								static_cast<void>(this->_class(name));
							this->_symbol(name, cls ? fb::check::symbol_kind::class_
								: statement->kid(0U)->kind == fb::node_kind::lambda ? fb::check::symbol_kind::function : fb::check::symbol_kind::value,
								statement, "");
							break;
						}
						case fb::node_kind::methoddef: {
							const ca::sv::str owner = statement->kid(0U)->tok->text;
							method* const m = _memory.make<method>(statement->tok->text, owner, statement, nullptr, nullptr, false);
							this->_set_method(this->_class(owner), m);
							this->_symbol(m->name, fb::check::symbol_kind::method, statement, owner);
							break;
						}
						case fb::node_kind::methodcopy: {
							const ca::sv::str destination = statement->kid(0U)->tok->text;
							const ca::sv::str source = statement->kid(1U)->tok->text;
							klass* const from = this->_class(source);
							klass* const to = this->_class(destination);
							if (statement->tok == nullptr) {
								const ca::usz count = from->methods.size();
								for (ca::usz i = 0U; i < count; ++i) {
									method* const original = from->methods.data()[i];
									method* const m = _memory.make<method>(original->name, destination, statement, original, nullptr, false);
									this->_set_method(to, m);
									this->_symbol(m->name, fb::check::symbol_kind::method, statement, destination);
								}
								break;
							}
							const ca::sv::str wanted = statement->kid(2U)->tok->text;
							method* const original = from->find(wanted);
							if (original == nullptr) {
								this->_error(statement->kid(2U), "{} has no method '{}' (above this line)", source, wanted);
								break;
							}
							method* const m = _memory.make<method>(statement->tok->text, destination, statement, original, nullptr, false);
							this->_set_method(to, m);
							this->_symbol(m->name, fb::check::symbol_kind::method, statement, destination);
							break;
						}
						default:
							break;
					}
				}
			}

			// -- definitions, classes, methods -----------------------------------

			[[nodiscard]] auto _definition(const ca::sv::str name) noexcept -> type* {

				auto found = _definitions.get(name);
				if (!found)
					return nullptr;
				definition& d = *found;
				if (d.t != nullptr)
					return d.t;
				if (d.doing)
					return _types.unknown();
				d.doing = true;
				type* const t = this->_infer(d.statement->kid(0U), &_top);
				d.doing = false;
				d.t = t;
				this->_record(d.statement, t);
				return t;
			}

			[[nodiscard]] auto _defines(const ca::sv::str name) const noexcept -> bool {
				return _definitions.contains(name);
			}

			/* The type of an instance of a script class: its constructor's result. */
			[[nodiscard]] auto _instance(const ca::sv::str name) noexcept -> type* {

				auto found = _definitions.get(name);
				if (!found || !self::_is_class_definition(found->statement))
					return nullptr;
				type* const ctor = this->_definition(name);
				if (ctor == nullptr || !ctor->is(form::function))
					return nullptr;
				type* const result = ctor->result();
				return result != nullptr && result->is(form::object) ? result : nullptr;
			}

			[[nodiscard]] auto _method_type(method* const m) noexcept -> type* {

				if (m->source != nullptr)
					return this->_method_type(m->source);
				if (m->t != nullptr)
					return m->t;
				if (m->doing)
					return _types.unknown();
				m->doing = true;
				const fb::node* const lambda = m->site->kid(1U);
				type* self_type = this->_instance(m->owner);
				this->_record(m->site->kid(0U), self_type != nullptr ? self_type : _types.object(m->owner, false));
				if (auto owner = _definitions.get(m->owner))
					this->_link(m->site->kid(0U), owner->statement);
				type* t;
				if (lambda->kind == fb::node_kind::lambda && lambda->kid(0U)->size() >= 1U) {
					if (self_type == nullptr)
						self_type = _types.object(m->owner, false);
					const fb::node* const params = lambda->kid(0U);
					scope* const frame = this->_scope(&_top, self_type);
					type* const slots = _types.tuple();
					if (params->size() == 1U) {
						this->_bind(frame, params, self_type, false);
						_types.push(slots, self_type);
					} else {
						for (const fb::node* const p : params->kids) {
							type* bound = _types.unknown();
							if ((p->kind == fb::node_kind::id || p->kind == fb::node_kind::projection) && p->tok != nullptr) {
								type* const field = self_type->sized ? self_type->field(p->tok->text) : nullptr;
								bound = field != nullptr ? field : self_type->sized ? _types.unit() : _types.unknown();
								this->_put(frame, p->tok->text, bound, p);
								this->_record(p, bound);
							}
							_types.push(slots, bound);
						}
					}
					t = _types.function(slots, this->_infer(lambda->kid(1U), frame));
					this->_record(lambda, t);
				} else
					t = this->_infer(lambda, &_top);
				m->doing = false;
				m->t = t;
				this->_record(m->site, t);
				return t;
			}

			[[nodiscard]] auto _object_method(const type* const receiver, const ca::sv::str name) noexcept -> method* {

				if (receiver == nullptr || !receiver->is(form::object) || receiver->native || receiver->name.empty())
					return nullptr;
				const klass* const k = this->_find_class(receiver->name);
				return k != nullptr ? k->find(name) : nullptr;
			}

			// -- annotations -----------------------------------------------------

			[[nodiscard]] auto _annotation(const fb::node* const te) noexcept -> type* {

				if (te->kind == fb::node_kind::typeunion) {
					type* t = nullptr;
					for (const fb::node* const member : te->kids) {
						type* const m = this->_annotation(member);
						t = t == nullptr ? m : _types.either(t, m);
					}
					return t != nullptr ? t : _types.unknown();
				}
				const ca::sv::str name = te->tok->text;
				if (te->size() == 0U) {
					if (name == "Int") return _types.integer();
					if (name == "Float") return _types.floating();
					if (name == "Number") return _types.number();
					if (name == "String") return _types.string();
					if (name == "Bool") return _types.boolean();
					if (name == "Path" || name == "File") return _types.file();
					if (name == "Command") return _types.command();
					if (name == "Unit") return _types.unit();
					if (name == "Map") return _types.map();
					if (name == "Json") return _types.unknown();
					if (name == "List") return _types.list(_types.unknown());
					if (name == "Tuple") return _types.any_tuple();
					if (auto found = _definitions.get(name); found && self::_is_class_definition(found->statement)) {
						this->_link(te, found->statement);
						type* const instance = this->_instance(name);
						return instance != nullptr ? instance : _types.object(name, false);
					}
					if (_catalog.native(name) != nullptr)
						return this->_native_object(name);
					this->_error(te, "unknown type '{}'", name);
					return _types.unknown();
				}
				if (name == "List" && te->size() == 1U)
					return _types.list(this->_annotation(te->kid(0U)));
				if (name == "Tuple") {
					type* const t = _types.tuple();
					for (const fb::node* const kid : te->kids)
						_types.push(t, this->_annotation(kid));
					return t;
				}
				this->_error(te, "unknown generic type '{}<...>'", name);
				return _types.unknown();
			}

			[[nodiscard]] auto _native_object(const ca::sv::str name) noexcept -> type* {

				type* const o = _types.object(name, false);
				o->native = true;
				return o;
			}

			// -- parameters ------------------------------------------------------

			/* Binds a lambda's parameters to the argument it gets, as the interpreter does: one
			   parameter takes the whole argument, several take its elements; (a, b) takes fields
			   by name, @rest the remaining elements. checked: compare the argument with each
			   annotation (an application), or not (a lambda alone, typed from its annotations). */
			auto _bind(scope* const frame, const fb::node* const params, type* const arg, const bool checked) noexcept -> void {

				const ca::usz count = params->size();
				for (ca::usz i = 0U; i < count; ++i) {
					const fb::node* const p = params->kid(i);
					type* slot = count == 1U ? arg : this->_positional(arg, i);
					switch (p->kind) {
						case fb::node_kind::id:
						case fb::node_kind::projection:
							if (p->kind == fb::node_kind::id && p->size() != 0U) {
								type* const declared = this->_annotation(p->kid(0U));
								if (checked && !fb::check::compatible(declared, slot))
									this->_error(p, "parameter '{}' is declared {}, got {}", p->tok->text, this->_name(declared), this->_name(slot));
								slot = declared;
							}
							if (p->tok != nullptr)
								this->_put(frame, p->tok->text, slot, p);
							this->_record(p, slot);
							break;
						case fb::node_kind::capture:
							if (p->size() != 0U) {
								type* rest;
								if (arg != nullptr && arg->is(form::tuple) && arg->sized) {
									rest = _types.tuple();
									for (ca::usz j = i; j < arg->size(); ++j)
										_types.push(rest, arg->kid(j));
								} else
									rest = i == 0U ? (arg != nullptr ? arg : _types.unknown()) : _types.unknown();
								this->_put(frame, p->kid(0U)->tok->text, rest, p->kid(0U));
								this->_record(p->kid(0U), rest);
							}
							break;
						case fb::node_kind::tuple:
							for (const fb::node* const sub : p->kids) {
								if (sub->tok == nullptr)
									continue;
								type* const field = slot != nullptr && (slot->is(form::file) || slot->is(form::object)) ? slot->field(sub->tok->text) : nullptr;
								type* const bound = field != nullptr ? field : _types.unknown();
								this->_put(frame, sub->tok->text, bound, sub);
								this->_record(sub, bound);
							}
							break;
						default:
							break;
					}
				}
			}

			/* A lambda's parameter types without an argument: annotations, else unknown. */
			[[nodiscard]] auto _declared(const fb::node* const params) noexcept -> type* {

				type* const slots = _types.tuple();
				for (const fb::node* const p : params->kids) {
					if (p->kind == fb::node_kind::capture)
						_types.push(slots, _types.variadic(_types.unknown()));
					else if (p->kind == fb::node_kind::id && p->size() != 0U)
						_types.push(slots, this->_annotation(p->kid(0U)));
					else
						_types.push(slots, _types.unknown());
				}
				return slots;
			}

			[[nodiscard]] auto _lambda(const fb::node* const n, scope* const sc) noexcept -> type* {

				const fb::node* const params = n->kid(0U);
				type* const slots = this->_declared(params);
				scope* const frame = this->_scope(sc, nullptr);
				type* arg = slots->size() == 1U ? slots->kid(0U) : slots;
				if (slots->size() == 1U && arg->is(form::list) && arg->variadic)
					arg = _types.unknown();
				frame->arg = arg;
				this->_bind(frame, params, arg, false);
				return _types.function(slots, this->_infer(n->kid(1U), frame));
			}

			/* A lambda applied to one argument in place (a pipe, a callback), its parameters
			   typed from it. */
			[[nodiscard]] auto _apply_lambda(const fb::node* const n, type* const arg, scope* const sc) noexcept -> type* {

				const fb::node* const params = n->kid(0U);
				scope* const frame = this->_scope(sc, arg);
				this->_bind(frame, params, arg, true);
				type* const result = this->_infer(n->kid(1U), frame);
				type* const slots = _types.tuple();
				for (ca::usz i = 0U; i < params->size(); ++i)
					_types.push(slots, params->size() == 1U ? arg : this->_positional(arg, i));
				this->_record(n, _types.function(slots, result));
				return result;
			}

			// -- match -----------------------------------------------------------

			/* Binds what a pattern names, reporting a pattern no value of t can take. */
			auto _pattern(const fb::node* const p, type* const t, scope* const frame) noexcept -> void {

				switch (p->kind) {
					case fb::node_kind::pbind: {
						type* bound = t;
						if (p->size() != 0U) {
							type* const declared = this->_annotation(p->kid(0U));
							if (!fb::check::compatible(declared, t))
								this->_error(p, "pattern '{}: {}' can never match {}", p->tok->text, this->_name(declared), this->_name(t));
							bound = declared;
						}
						this->_put(frame, p->tok->text, bound, p);
						this->_record(p, bound);
						return;
					}
					case fb::node_kind::plit: {
						type* const literal = this->_infer(p->kid(0U), frame);
						if (!fb::check::compatible(literal, t) && !(literal->is(form::unit)))
							this->_error(p, "a {} pattern can never match {}", this->_name(literal), this->_name(t));
						this->_record(p, literal);
						return;
					}
					case fb::node_kind::ptuple: {
						const bool sequence = t->is(form::tuple) || t->is(form::list);
						const bool open = t->is(form::unknown) || t->is(form::variable) || t->is(form::alternatives) || t->is(form::unit);
						if (!sequence && !open)
							this->_error(p, "a tuple pattern can never match {}", this->_name(t));
						else if (t->is(form::tuple) && t->sized && t->size() != p->size())
							this->_error(p, "a tuple pattern of {} can never match {}", p->size(), this->_name(t));
						for (ca::usz i = 0U; i < p->size(); ++i)
							this->_pattern(p->kid(i), sequence ? this->_positional(t, i) : _types.unknown(), frame);
						return;
					}
					default:
						return;
				}
			}

			/* A match applied to an argument of type arg: the arms' results together. */
			[[nodiscard]] auto _apply_match(const fb::node* const n, type* const arg, scope* const sc) noexcept -> type* {

				type* result = nullptr;
				for (const fb::node* const arm : n->kids) {
					scope* const frame = this->_scope(sc, arg);
					this->_pattern(arm->kid(0U), arg, frame);
					if (arm->kid(1U) != nullptr)
						static_cast<void>(this->_infer(arm->kid(1U), frame));
					type* const r = this->_infer(arm->kid(2U), frame);
					result = result == nullptr ? r : _types.either(result, r);
					this->_record(arm, r);
				}
				if (result == nullptr)
					result = _types.unit();
				type* const slots = _types.tuple();
				_types.push(slots, arg);
				this->_record(n, _types.function(slots, result));
				return result;
			}

			/* A match alone: its argument is unknown. */
			[[nodiscard]] auto _match(const fb::node* const n, scope* const sc) noexcept -> type* {

				static_cast<void>(this->_apply_match(n, _types.unknown(), sc));
				return this->_report.type_of(n);
			}

			[[nodiscard]] static auto _callable(const fb::node* const n) noexcept -> bool {
				return n->kind == fb::node_kind::lambda || n->kind == fb::node_kind::match_;
			}

			/* A lambda or match typed without an argument. */
			[[nodiscard]] auto _alone(const fb::node* const n, scope* const sc) noexcept -> type* {
				return n->kind == fb::node_kind::match_ ? this->_match(n, sc) : this->_lambda(n, sc);
			}

			// -- calls -----------------------------------------------------------

			[[nodiscard]] auto _arguments(void) noexcept -> arguments {
				return arguments{_memory.list<type*>(), _memory.list<const fb::node*>(), false};
			}

			/* A call's arguments, splats of sized tuples spread; lambdas are left untyped
			   (nullptr) for the callee to apply. */
			[[nodiscard]] auto _gather(const fb::node* const call, scope* const sc) noexcept -> arguments {

				arguments a = this->_arguments();
				for (ca::usz i = 1U; i < call->size(); ++i) {
					const fb::node* const node = call->kid(i);
					if (node->kind == fb::node_kind::splat) {
						type* const spread = this->_record(node, this->_infer(node->kid(0U), sc));
						if (spread->is(form::tuple) && spread->sized)
							for (type* const item : spread->kids) {
								_memory.push(a.types, item);
								_memory.push(a.nodes, static_cast<const fb::node*>(nullptr));
							}
						else if (!spread->is(form::unit))
							a.open = true;
						continue;
					}
					_memory.push(a.types, self::_callable(node) ? static_cast<type*>(nullptr) : this->_infer(node, sc));
					_memory.push(a.nodes, node);
				}
				return a;
			}

			/* Types the lambdas _gather left, without a callee to tell their parameters. */
			auto _settle(arguments& a, scope* const sc) noexcept -> void {

				for (ca::usz i = 0U; i < a.types.size(); ++i)
					if (a.types.data()[i] == nullptr)
						a.types.data()[i] = this->_record(a.nodes.data()[i], this->_alone(a.nodes.data()[i], sc));
			}

			[[nodiscard]] auto _whole(const arguments& a) noexcept -> type* {

				if (a.types.size() == 1U && !a.open)
					return a.types.data()[0U];
				if (a.open)
					return _types.any_tuple();
				type* const t = _types.tuple();
				for (type* const item : a.types)
					_types.push(t, item);
				return t;
			}

			auto _arity(const fb::node* const at, const ca::sv::str name, const int min, const int max, const arguments& a) noexcept -> void {

				const ca::usz got = a.types.size();
				if (a.open && (max < 0 || static_cast<ca::i64>(got) <= max))
					return;
				if (static_cast<ca::i64>(got) >= min && (max < 0 || static_cast<ca::i64>(got) <= max))
					return;
				const ca::sv::str s = min == 1 ? "" : "s";
				if (max < 0)
					this->_error(at, "{} expects at least {} argument{}, got {}", name, min, s, got);
				else if (min == max)
					this->_error(at, "{} expects {} argument{}, got {}", name, min, s, got);
				else
					this->_error(at, "{} expects {} to {} arguments, got {}", name, min, max, got);
			}

			/* Applies a function type to arguments: each fits its parameter (binding type
			   variables), lambdas are applied in place to what their parameter expects, and the
			   result has the variables replaced. */
			[[nodiscard]] auto _call_function(type* const fn, arguments& a, const fb::node* const at, const ca::sv::str name, scope* const sc, const bool user) noexcept -> type* {

				type* const params = fn->parameters();
				fb::check::bindings bound{_memory};
				if (params == nullptr) {
					this->_settle(a, sc);
					return _types.unknown();
				}
				const ca::usz count = params->size();
				type* const last = count != 0U ? params->kid(count - 1U) : nullptr;
				const bool variadic = last != nullptr && last->is(form::list) && last->variadic;
				const ca::usz fixed = variadic ? count - 1U : count;
				// One parameter takes all the arguments as one; one tuple argument spreads
				// over several parameters.
				if (count == 1U && !variadic && (a.types.size() != 1U || a.open)) {
					this->_settle(a, sc);
					type* const whole = this->_whole(a);
					if (!fb::check::match(_types, params->kid(0U), whole, bound))
						this->_error(at, "{} expects {}, got {}", name, this->_name(params->kid(0U)), this->_name(whole));
					return fb::check::substitute(_types, fn->result(), bound);
				}
				if (user && count > 1U && a.types.size() == 1U && !a.open && a.types.data()[0U] != nullptr) {
					type* const only = a.types.data()[0U];
					if (only->is(form::tuple) && only->sized) {
						arguments spread = this->_arguments();
						for (type* const item : only->kids) {
							_memory.push(spread.types, item);
							_memory.push(spread.nodes, static_cast<const fb::node*>(nullptr));
						}
						return this->_call_function(fn, spread, at, name, sc, user);
					}
					if (!only->is(form::tuple)) {
						return fn->result();
					}
				}
				if (user && !variadic && count > 1U && a.types.size() > count && !a.open)
					this->_error(at, "{} takes {} arguments, got {}", name, count, a.types.size());
				const auto expected_at = [&](const ca::usz i) noexcept -> type* {
					if (i < fixed)
						return params->kid(i);
					return variadic ? last->kid(0U) : nullptr;
				};
				// Arguments first, then the lambdas, once the variables they need are bound.
				for (ca::usz i = 0U; i < a.types.size(); ++i) {
					type* const given = a.types.data()[i];
					type* const expected = expected_at(i);
					if (given == nullptr)
						continue;
					if (expected != nullptr && !fb::check::match(_types, expected, given, bound))
						this->_error(a.nodes.data()[i] != nullptr ? a.nodes.data()[i] : at, "argument {} of {} is {}, expected {}",
							i + 1U, name, this->_name(given), this->_name(fb::check::substitute(_types, expected, bound)));
				}
				for (ca::usz i = 0U; i < a.types.size(); ++i) {
					if (a.types.data()[i] != nullptr)
						continue;
					const fb::node* const lambda = a.nodes.data()[i];
					type* const expected = expected_at(i);
					if (expected == nullptr || !expected->is(form::function)) {
						a.types.data()[i] = this->_record(lambda, this->_alone(lambda, sc));
						continue;
					}
					type* const wanted = fb::check::substitute(_types, expected->parameters(), bound);
					type* const arg = wanted->size() == 1U ? wanted->kid(0U) : wanted;
					type* const result = lambda->kind == fb::node_kind::match_ ? this->_apply_match(lambda, arg, sc) : this->_apply_lambda(lambda, arg, sc);
					a.types.data()[i] = this->_record(lambda, _types.function(wanted, result, 0U));
					if (!fb::check::match(_types, expected->result(), result, bound))
						this->_error(lambda, "the function given to {} returns {}, expected {}", name, this->_name(result), this->_name(fb::check::substitute(_types, expected->result(), bound)));
				}
				return fb::check::substitute(_types, fn->result(), bound);
			}

			/* A builtin's type as a value (`|> stem`, `filter(xs, is_file)`). */
			[[nodiscard]] auto _builtin_value(const fb::check::builtin_entry& b) noexcept -> type* {

				if (b.signature != nullptr)
					return b.signature;
				type* const slots = _types.tuple();
				_types.push(slots, _types.variadic(_types.unknown()));
				return _types.function(slots, b.result, 0U);
			}

			[[nodiscard]] auto _common(type* const a, type* const b) noexcept -> type* {

				if (a == nullptr)
					return b;
				return fb::check::same(a, b) || b->is(form::unit) ? a : a->is(form::unit) ? b : _types.unknown();
			}

			[[nodiscard]] auto _call_builtin(const fb::node* const n, const fb::check::builtin_entry& b, scope* const sc) noexcept -> type* {

				const fb::node* const callee = n->kid(0U);
				arguments a = this->_gather(n, sc);
				this->_arity(callee, b.name, b.min, b.max, a);
				this->_record(callee, this->_builtin_value(b));
				if (b.name == "zip") {
					this->_settle(a, sc);
					type* const row = _types.tuple();
					for (ca::usz i = 0U; i < a.types.size(); ++i) {
						type* const t = a.types.data()[i];
						if (!t->is(form::list) && !t->is(form::tuple) && !t->is(form::unknown) && !t->is(form::unit))
							this->_error(a.nodes.data()[i] != nullptr ? a.nodes.data()[i] : callee, "argument {} of zip is {}, expected a List or Tuple", i + 1U, this->_name(t));
						_types.push(row, fb::check::element(_types, t));
					}
					if (a.open)
						row->sized = false;
					return _types.list(row);
				}
				if (b.name == "concat") {
					this->_settle(a, sc);
					type* item = nullptr;
					for (type* const t : a.types)
						item = this->_common(item, t->is(form::list) || t->is(form::tuple) ? fb::check::element(_types, t) : t);
					return _types.list(a.open || item == nullptr ? _types.unknown() : item);
				}
				if (b.signature != nullptr)
					return this->_call_function(b.signature, a, callee, b.name, sc, false);
				this->_settle(a, sc);
				return b.result;
			}

			[[nodiscard]] auto _call_method(const fb::node* const n, type* const receiver, scope* const sc) noexcept -> ca::optional<type*> {

				const fb::node* const callee = n->kid(0U);
				const ca::sv::str name = callee->tok->text;
				if (receiver->is(form::object) && receiver->native) {
					const fb::check::native_class* const cls = _catalog.native(receiver->name);
					arguments a = this->_gather(n, sc);
					this->_settle(a, sc);
					if (cls != nullptr && !cls->has(name) && receiver->field(name) == nullptr)
						this->_error(callee, "{} has no method '{}'", receiver->name, name);
					return _types.unknown();
				}
				method* const m = this->_object_method(receiver, name);
				if (m == nullptr)
					return ca::nullopt;
				arguments a = this->_gather(n, sc);
				this->_settle(a, sc);
				type* const t = this->_method_type(m);
				this->_record(callee, t);
				this->_link(callee, m->site);
				return t->is(form::function) ? t->result() : _types.unknown();
			}

			[[nodiscard]] auto _call(const fb::node* const n, scope* const sc) noexcept -> type* {

				const fb::node* const callee = n->kid(0U);
				if (callee->kind == fb::node_kind::parser) {
					arguments a = this->_gather(n, sc);
					this->_settle(a, sc);
					return this->_infer(callee, sc);
				}
				if (callee->kind == fb::node_kind::field && callee->tok != nullptr && callee->size() == 1U) {
					type* const receiver = this->_infer(callee->kid(0U), sc);
					if (auto result = this->_call_method(n, receiver, sc))
						return *result;
				}
				if (callee->kind == fb::node_kind::id) {
					const ca::sv::str name = callee->tok->text;
					this->_resolve(callee, name, sc);
					type* fn = self::_lookup(sc, name);
					if (fn == nullptr && !this->_defines(name)) {
						if (const fb::check::builtin_entry* const b = _catalog.builtin(name))
							return this->_call_builtin(n, *b, sc);
						fn = _catalog.global(name);
						if (fn == nullptr) {
							arguments a = this->_gather(n, sc);
							this->_settle(a, sc);
							this->_error(callee, "unknown function '{}'", name);
							return _types.unknown();
						}
					}
					if (fn == nullptr)
						fn = this->_definition(name);
					this->_record(callee, fn);
					return this->_apply(fn, n, name, sc);
				}
				type* const fn = this->_record(callee, this->_infer(callee, sc));
				return this->_apply(fn, n, "the function", sc);
			}

			[[nodiscard]] auto _apply(type* const fn, const fb::node* const n, const ca::sv::str name, scope* const sc) noexcept -> type* {

				arguments a = this->_gather(n, sc);
				if (fn->is(form::function))
					return this->_call_function(fn, a, n->kid(0U), name, sc, true);
				this->_settle(a, sc);
				if (!fn->is(form::unknown) && !fn->is(form::variable))
					this->_error(n->kid(0U), "{} is {}, not a function", name, this->_name(fn));
				return _types.unknown();
			}

			/* fnode applied to one argument (a pipe stage). */
			[[nodiscard]] auto _apply_node(const fb::node* const fnode, type* const arg, scope* const sc) noexcept -> type* {

				if (fnode->kind == fb::node_kind::lambda)
					return this->_apply_lambda(fnode, arg, sc);
				if (fnode->kind == fb::node_kind::match_)
					return this->_apply_match(fnode, arg, sc);
				if (fnode->kind == fb::node_kind::morphism) {
					this->_record(fnode, this->_morphism());
					if (!fb::check::compatible(_types.file(), arg))
						this->_error(fnode, "a morphism rewrites a File, got {}", this->_name(arg));
					return _types.file();
				}
				type* const fn = this->_infer(fnode, sc);
				if (fn->is(form::function)) {
					arguments a = this->_arguments();
					_memory.push(a.types, arg);
					_memory.push(a.nodes, static_cast<const fb::node*>(nullptr));
					const ca::sv::str name = fnode->kind == fb::node_kind::id ? fnode->tok->text : ca::sv::str{"the function"};
					return this->_call_function(fn, a, fnode, name, sc, true);
				}
				if (!fn->is(form::unknown) && !fn->is(form::variable))
					this->_error(fnode, "cannot pipe into {}", this->_name(fn));
				return _types.unknown();
			}

			[[nodiscard]] auto _morphism(void) noexcept -> type* {

				type* const slots = _types.tuple();
				_types.push(slots, _types.file());
				return _types.function(slots, _types.file());
			}

			// -- expressions -----------------------------------------------------

			[[nodiscard]] auto _id(const fb::node* const n, scope* const sc) noexcept -> type* {

				const ca::sv::str name = n->tok->text;
				this->_resolve(n, name, sc);
				if (type* const local = self::_lookup(sc, name))
					return local;
				if (type* const defined = this->_definition(name))
					return defined;
				if (type* const bound = _catalog.global(name))
					return bound;
				if (const fb::check::builtin_entry* const b = _catalog.builtin(name))
					return this->_builtin_value(*b);
				this->_warning(n, "'{}' is not defined: it stands for the string \"{}\"", name, name);
				return _types.unknown();
			}

			[[nodiscard]] auto _field(const fb::node* const n, scope* const sc) noexcept -> type* {

				type* const base = this->_infer(n->kid(0U), sc);
				if (n->tok == nullptr) {
					type* const key = this->_infer(n->kid(1U), sc);
					if (base->is(form::list))
						return base->kid(0U);
					if (base->is(form::tuple)) {
						return fb::check::element(_types, base);
					}
					static_cast<void>(key);
					return _types.unknown();
				}
				const ca::sv::str name = n->tok->text;
				switch (base->kind) {
					case form::unknown:
					case form::unit:
					case form::variable:
					case form::map:
						return _types.unknown();
					case form::object: {
						if (base->native) {
							if (type* const field = base->field(name))
								return field;
							const fb::check::native_class* const cls = _catalog.native(base->name);
							if (cls != nullptr && !cls->has(name))
								this->_error(n, "{} has no field or method '{}'", base->name, name);
							return _types.unknown();
						}
						if (method* const m = this->_object_method(base, name)) {
							this->_link(n, m->site);
							return this->_method_type(m);
						}
						if (type* const field = base->field(name))
							return field;
						if (base->sized)
							this->_error(n, "{} has no field '{}'", this->_name(base), name);
						return _types.unknown();
					}
					case form::file: {
						if (type* const field = base->field(name))
							return field;
						this->_error(n, "File has no field '{}'", name);
						return _types.unknown();
					}
					case form::tuple:
					case form::list: {
						const ca::i64 index = self::_index(n->tok);
						if (index < 0) {
							this->_error(n, "{} has no field '{}'", this->_name(base), name);
							return _types.unknown();
						}
						if (base->is(form::tuple) && base->sized && static_cast<ca::usz>(index) >= base->size()) {
							this->_error(n, "{} has no element {}", this->_name(base), index);
							return _types.unknown();
						}
						return this->_positional(base, static_cast<ca::usz>(index));
					}
					case form::alternatives:
						return _types.unknown();
					default:
						this->_error(n, "{} has no field '{}'", this->_name(base), name);
						return _types.unknown();
				}
			}

			[[nodiscard]] auto _projection(const fb::node* const n, scope* const sc) noexcept -> type* {

				if (n->tok != nullptr)
					if (const local* const l = self::_find(sc, n->tok->text)) {
						this->_link(n, l->site);
						return l->t;
					}
				type* const arg = self::_arg(sc);
				const ca::i64 index = self::_index(n->tok);
				if (index >= 0)
					return this->_positional(arg, static_cast<ca::usz>(index));
				if (arg != nullptr && (arg->is(form::file) || arg->is(form::object)))
					if (type* const field = arg->field(n->tok->text))
						return field;
				return _types.unknown();
			}

			[[nodiscard]] auto _sequence(const fb::node* const n, scope* const sc, const bool listed) noexcept -> type* {

				type* const t = _types.tuple();
				type* item = nullptr;
				bool known = true;
				const auto add = [&](type* const element) noexcept {
					_types.push(t, element);
					item = this->_common(item, element);
				};
				for (const fb::node* const kid : n->kids) {
					const fb::node* element = kid;
					if (element->kind == fb::node_kind::guard && element->kid(3U) == nullptr && element->kid(2U) != nullptr) {
						known = false;
						item = this->_common(item, this->_infer(element, sc));
						continue;
					}
					if (element->kind == fb::node_kind::splat) {
						type* const spread = this->_record(element, this->_infer(element->kid(0U), sc));
						if (spread->is(form::tuple) && spread->sized)
							for (type* const x : spread->kids)
								add(x);
						else if (spread->is(form::list) || spread->is(form::tuple)) {
							known = false;
							item = this->_common(item, fb::check::element(_types, spread));
						} else if (!spread->is(form::unit))
							add(spread);
						continue;
					}
					type* const v = this->_infer(element, sc);
					if (!v->is(form::unit))
						add(v);
				}
				if (listed)
					return _types.list(item != nullptr ? item : _types.unknown());
				if (!known)
					return _types.any_tuple();
				return t;
			}

			[[nodiscard]] auto _map(const fb::node* const n, scope* const sc) noexcept -> type* {

				for (const fb::node* const entry : n->kids) {
					if (entry->kind == fb::node_kind::splat) {
						type* const source = this->_record(entry, this->_infer(entry->kid(0U), sc));
						if (!source->is(form::map) && !source->is(form::object) && !source->is(form::unknown) && !source->is(form::variable) && !source->is(form::alternatives))
							this->_error(entry, "cannot spread {} into a map", this->_name(source));
						continue;
					}
					for (const fb::node* const part : entry->kids)
						static_cast<void>(this->_infer(part, sc));
				}
				return _types.map();
			}

			/* type NAME(...) = { ... }: the fields, when the body is a map literal of fixed keys
			   (and spreads of sized records); unsized otherwise. */
			[[nodiscard]] auto _object(const fb::node* const n, scope* const sc) noexcept -> type* {

				const fb::node* const body = n->kid(0U);
				const ca::sv::str name = n->tok->text;
				if (body->kind != fb::node_kind::maplit) {
					static_cast<void>(this->_infer(body, sc));
					return _types.object(name, false);
				}
				type* const o = _types.object(name, true);
				for (const fb::node* const entry : body->kids) {
					if (entry->kind == fb::node_kind::splat) {
						type* const source = this->_record(entry, this->_infer(entry->kid(0U), sc));
						if ((source->is(form::object) || source->is(form::file)) && source->sized)
							for (ca::usz i = 0U; i < source->size(); ++i)
								_types.set_field(o, source->names.data()[i], source->kid(i));
						else
							o->sized = false;
						continue;
					}
					const fb::node* const key = entry->kid(0U);
					type* const value = this->_infer(entry->kid(1U), sc);
					if (key->kind == fb::node_kind::string && key->size() == 0U && key->tok != nullptr)
						_types.set_field(o, key->tok->text, value);
					else {
						static_cast<void>(this->_infer(key, sc));
						o->sized = false;
					}
				}
				this->_record(body, _types.map());
				return o;
			}

			/* Parser<schema>: a record of the schema's fields typed from their "bool" / "int" /
			   "str", and rest. */
			[[nodiscard]] auto _parser(const fb::node* const n, scope* const sc) noexcept -> type* {

				const fb::node* const schema = n->kid(0U);
				static_cast<void>(this->_infer(schema, sc));
				if (schema->kind != fb::node_kind::maplit)
					return _types.unknown();
				type* const record = _types.object("Parser", true);
				const auto text_of = [](const fb::node* const s) noexcept -> ca::optional<ca::sv::str> {
					if (s != nullptr && s->kind == fb::node_kind::string && s->size() == 0U && s->tok != nullptr)
						return s->tok->text;
					return ca::nullopt;
				};
				for (const fb::node* const entry : schema->kids) {
					if (entry->kind != fb::node_kind::tuple || entry->size() != 2U)
						continue;
					const auto key = text_of(entry->kid(0U));
					if (!key)
						continue;
					ca::optional<ca::sv::str> kind = text_of(entry->kid(1U));
					if (!kind && entry->kid(1U)->kind == fb::node_kind::maplit)
						for (const fb::node* const spec : entry->kid(1U)->kids)
							if (spec->kind == fb::node_kind::tuple && spec->size() == 2U && text_of(spec->kid(0U)) == ca::optional<ca::sv::str>{"type"})
								kind = text_of(spec->kid(1U));
					type* t = _types.string();
					if (kind && *kind == "bool")
						t = _types.boolean();
					else if (kind && *kind == "int")
						t = _types.integer();
					_types.set_field(record, *key, t);
				}
				_types.set_field(record, "rest", _types.list(_types.string()));
				return record;
			}

			[[nodiscard]] auto _construct(const fb::node* const n, scope* const sc) noexcept -> type* {

				if (n->tok->text != "File") {
					for (const fb::node* const field : n->kids)
						static_cast<void>(this->_infer(field->kid(0U), sc));
					this->_error(n, "unknown constructor '{}<...>' (only File<...>)", n->tok->text);
					return _types.unknown();
				}
				type* const record = _types.file();
				for (const fb::node* const field : n->kids) {
					type* const v = this->_infer(field->kid(0U), sc);
					if (field->tok != nullptr) {
						const ca::sv::str key = field->tok->text;
						type* const existing = record->field(key);
						if (existing != nullptr && !fb::check::compatible(existing, v))
							this->_error(field, "field '{}' is {}, cannot set to {}", key, this->_name(existing), this->_name(v));
						else if (existing == nullptr)
							_types.set_field(record, key, v);
					} else if (!v->is(form::file) && !fb::check::compatible(_types.either(_types.file(), _types.string()), v))
						this->_error(n, "cannot splat {} into File (expected File)", this->_name(v));
				}
				return record;
			}

			[[nodiscard]] auto _guard(const fb::node* const n, scope* const sc) noexcept -> type* {

				scope* inner = sc;
				if (const fb::node* const where = n->kid(1U)) {
					inner = this->_scope(sc, self::_arg(sc));
					for (const fb::node* const binding : where->kids) {
						type* const v = this->_infer(binding->kid(0U), inner);
						this->_put(inner, binding->tok->text, v, binding);
						this->_record(binding, v);
					}
				}
				if (const fb::node* const when = n->kid(2U))
					static_cast<void>(this->_infer(when->kid(0U), inner));
				type* const body = this->_infer(n->kid(0U), inner);
				if (const fb::node* const otherwise = n->kid(3U))
					return _types.either(body, this->_record(otherwise, this->_infer(otherwise->kid(0U), inner)));
				return body;
			}

			[[nodiscard]] auto _arith(const fb::node* const n, scope* const sc) noexcept -> type* {

				type* const l = this->_infer(n->kid(0U), sc);
				type* const r = this->_infer(n->kid(1U), sc);
				for (type* const side : {l, r})
					if (!fb::check::numeric(side) && !side->is(form::unknown) && !side->is(form::variable) && !side->is(form::unit))
						this->_error(n, "arithmetic operand must be Int or Float, got {}", this->_name(side));
				if (l->is(form::floating) || r->is(form::floating))
					return _types.floating();
				if (l->is(form::integer) && r->is(form::integer))
					return _types.integer();
				if (fb::check::numeric(l) || fb::check::numeric(r))
					return _types.number();
				return _types.number();
			}

			[[nodiscard]] auto _compare(const fb::node* const n, scope* const sc) noexcept -> type* {

				type* const l = this->_infer(n->kid(0U), sc);
				type* const r = this->_infer(n->kid(1U), sc);
				const auto numberish = [](const type* const t) noexcept {
					return fb::check::numeric(t) || t->is(form::boolean);
				};
				if (!fb::check::compatible(l, r) && !(numberish(l) && numberish(r)))
					this->_error(n, "cannot compare {} with {}", this->_name(l), this->_name(r));
				return _types.boolean();
			}

			[[nodiscard]] auto _store(const fb::node* const n, scope* const sc) noexcept -> type* {

				type* const rhs = this->_infer(n->kid(1U), sc);
				const fb::node* const target = n->kid(0U);
				if (target->kind != fb::node_kind::field) {
					this->_error(n, "<- needs a field to write into");
					return rhs;
				}
				type* const base = this->_infer(target->kid(0U), sc);
				if (target->tok == nullptr && target->size() == 2U)
					static_cast<void>(this->_infer(target->kid(1U), sc));
				if (!base->is(form::map) && !base->is(form::object) && !base->is(form::unknown) && !base->is(form::variable) && !base->is(form::alternatives))
					this->_error(n, "<- writes into a map, got {}", this->_name(base));
				this->_record(target, rhs);
				return rhs;
			}

			[[nodiscard]] auto _infer(const fb::node* const n, scope* const sc) noexcept -> type* {
				return n == nullptr ? _types.unknown() : this->_record(n, this->_infer_node(n, sc));
			}

			[[nodiscard]] auto _infer_node(const fb::node* const n, scope* const sc) noexcept -> type* {

				switch (n->kind) {
					case fb::node_kind::integer:
						return _types.integer();
					case fb::node_kind::floating:
						return _types.floating();
					case fb::node_kind::boolean:
						return _types.boolean();
					case fb::node_kind::nil:
						return _types.unit();
					case fb::node_kind::strtext:
						return _types.string();
					case fb::node_kind::string:
					case fb::node_kind::command:
						for (const fb::node* const part : n->kids)
							if (part->kind != fb::node_kind::cmdtext && part->kind != fb::node_kind::strtext)
								static_cast<void>(this->_infer(part->kind == fb::node_kind::splat ? part->kid(0U) : part, sc));
						return n->kind == fb::node_kind::string ? _types.string() : _types.command();
					case fb::node_kind::path:
						return _types.file();
					case fb::node_kind::id:
						return this->_id(n, sc);
					case fb::node_kind::assign:
						return this->_infer(n->kid(0U), sc);
					case fb::node_kind::eager: {
						type* const v = this->_infer(n->kid(0U), sc);
						this->_put(sc, n->tok->text, v, n);
						return v;
					}
					case fb::node_kind::store:
						return this->_store(n, sc);
					case fb::node_kind::force: {
						const fb::node* const operand = n->kid(0U);
						return this->_infer(operand, sc);
					}
					case fb::node_kind::tuple:
						return this->_sequence(n, sc, false);
					case fb::node_kind::listlit:
						return this->_sequence(n, sc, true);
					case fb::node_kind::maplit:
						return this->_map(n, sc);
					case fb::node_kind::parser:
						return this->_parser(n, sc);
					case fb::node_kind::lambda:
						return this->_lambda(n, sc);
					case fb::node_kind::match_:
						return this->_match(n, sc);
					case fb::node_kind::pipe: {
						type* t = this->_infer(n->kid(0U), sc);
						for (ca::usz i = 1U; i < n->size(); ++i) {
							// Unknown input may be a sequence (mapped) or not: the result is unknown.
							const bool over = t->is(form::list) || t->is(form::tuple);
							const bool unsure = t->is(form::unknown) || t->is(form::variable) || t->is(form::alternatives);
							type* const result = this->_apply_node(n->kid(i), over ? fb::check::element(_types, t) : t, sc);
							t = over ? _types.list(result) : unsure ? _types.unknown() : result;
						}
						return t;
					}
					case fb::node_kind::seq: {
						type* t = _types.unit();
						for (const fb::node* const step : n->kids)
							t = this->_infer(step, sc);
						return t;
					}
					case fb::node_kind::guard:
						return this->_guard(n, sc);
					case fb::node_kind::compare:
						return this->_compare(n, sc);
					case fb::node_kind::arith:
						return this->_arith(n, sc);
					case fb::node_kind::neg: {
						type* const v = this->_infer(n->kid(0U), sc);
						if (!fb::check::numeric(v) && !v->is(form::unknown) && !v->is(form::variable) && !v->is(form::unit))
							this->_error(n, "'-' operand must be Int or Float, got {}", this->_name(v));
						return v->is(form::floating) ? _types.floating() : v->is(form::integer) ? _types.integer() : _types.number();
					}
					case fb::node_kind::logical: {
						type* const l = this->_infer(n->kid(0U), sc);
						type* const r = this->_infer(n->kid(1U), sc);
						return n->tok->kind == fb::tok::oror ? _types.either(l, r) : _types.boolean();
					}
					case fb::node_kind::not_:
						static_cast<void>(this->_infer(n->kid(0U), sc));
						return _types.boolean();
					case fb::node_kind::morphism:
						return this->_morphism();
					case fb::node_kind::objnew:
						return this->_object(n, sc);
					case fb::node_kind::methoddef: {
						klass* const k = this->_find_class(n->kid(0U)->tok->text);
						method* const m = k != nullptr ? k->find(n->tok->text) : nullptr;
						if (m != nullptr && m->site == n)
							return this->_method_type(m);
						method* const shadowed = _memory.make<method>(n->tok->text, n->kid(0U)->tok->text, n, nullptr, nullptr, false);
						return this->_method_type(shadowed);
					}
					case fb::node_kind::methodcopy:
						return _types.unit();
					case fb::node_kind::call:
						return this->_call(n, sc);
					case fb::node_kind::field:
						return this->_field(n, sc);
					case fb::node_kind::construct:
						return this->_construct(n, sc);
					case fb::node_kind::splat:
						return this->_infer(n->kid(0U), sc);
					case fb::node_kind::metadata:
						static_cast<void>(this->_infer(n->kid(0U), sc));
						return _types.unknown();
					case fb::node_kind::capture: {
						if (n->size() != 0U)
							if (const local* const l = self::_find(sc, n->kid(0U)->tok->text)) {
								this->_link(n->kid(0U), l->site);
								return l->t;
							}
						type* const arg = self::_arg(sc);
						return arg != nullptr ? arg : _types.unknown();
					}
					case fb::node_kind::projection:
						return this->_projection(n, sc);
					default:
						return _types.unknown();
				}
			}

			// -- recursion ---------------------------------------------------------

			/* Every self-call must sit under a guard (the base case); an unguarded one always
			   loops. Syntactic: it proves a guard exists, not that it stops. */
			auto _recursion(const fb::token* const name, const fb::node* const n, const bool guarded) noexcept -> void {

				if (n == nullptr)
					return;
				if (n->kind == fb::node_kind::call && n->kid(0U)->kind == fb::node_kind::id && n->kid(0U)->tok->text == name->text && !guarded)
					this->_error_at(self::_span(n->kid(0U)->tok), "unbounded recursion: '{}' calls itself with no 'when' guard (no base case)", name->text);
				if (n->kind == fb::node_kind::lambda && n->kid(0U) != nullptr)
					for (const fb::node* const p : n->kid(0U)->kids)
						if ((p->kind == fb::node_kind::id || p->kind == fb::node_kind::projection) && p->tok != nullptr && p->tok->text == name->text)
							return;
				if (n->kind == fb::node_kind::guard) {
					const bool gated = guarded || n->kid(2U) != nullptr;
					this->_recursion(name, n->kid(0U), gated);
					this->_recursion(name, n->kid(1U), guarded);
					this->_recursion(name, n->kid(2U), guarded);
					this->_recursion(name, n->kid(3U), gated);
					return;
				}
				if (n->kind == fb::node_kind::match_) {
					// An arm runs only when its pattern (or condition) selects it: with more than
					// one arm, or a refutable one, each is a branch.
					const fb::node* const only = n->size() == 1U ? n->kid(0U) : nullptr;
					const bool total = only != nullptr && only->kid(1U) == nullptr
						&& (only->kid(0U)->kind == fb::node_kind::pwild || (only->kid(0U)->kind == fb::node_kind::pbind && only->kid(0U)->size() == 0U));
					for (const fb::node* const arm : n->kids) {
						this->_recursion(name, arm->kid(1U), guarded);
						this->_recursion(name, arm->kid(2U), guarded || !total);
					}
					return;
				}
				if (n->kind == fb::node_kind::logical && n->tok->kind == fb::tok::oror) {
					this->_recursion(name, n->kid(0U), guarded);
					this->_recursion(name, n->kid(1U), true);
					return;
				}
				for (const fb::node* const kid : n->kids)
					this->_recursion(name, kid, guarded);
			}

			auto _sort(void) noexcept -> void {

				fb::check::diagnostic* const d = _report._diagnostics.data();
				const auto before = [](const fb::check::diagnostic& a, const fb::check::diagnostic& b) noexcept {
					return a.at.start.line < b.at.start.line || (a.at.start.line == b.at.start.line && a.at.start.col < b.at.start.col);
				};
				for (ca::usz i = 1U; i < _report._diagnostics.size(); ++i)
					for (ca::usz j = i; j > 0U && before(d[j], d[j - 1U]); --j) {
						const fb::check::diagnostic t = d[j];
						d[j] = d[j - 1U];
						d[j - 1U] = t;
					}
			}

		public:

			checker(fb::check::types& types, const fb::check::catalog& catalog, fb::check::report& report) noexcept
			: _types{types}, _catalog{catalog}, _memory{types.memory()}, _report{report}, _definitions{types.memory().allocator()},
			  _classes{types.memory().allocator()}, _top{nullptr, nullptr, nullptr} {
			}

			auto run(const fb::node* const program) noexcept -> void {

				this->_collect(program);
				for (const fb::node* const statement : program->kids) {
					if (statement->kind == fb::node_kind::assign) {
						auto found = _definitions.get(statement->tok->text);
						if (found && found->statement == statement) {
							static_cast<void>(this->_definition(statement->tok->text));
							this->_recursion(statement->tok, statement->kid(0U), false);
						} else
							this->_record(statement, this->_infer(statement->kid(0U), &_top));
						continue;
					}
					static_cast<void>(this->_infer(statement, &_top));
				}
				this->_sort();
			}
	};

} // namespace fb::check

auto fb::check::check(fb::check::types& types, const fb::check::catalog& catalog, const fb::node* const program) noexcept -> fb::check::report* {

	fb::check::report* const report = types.memory().make<fb::check::report>(types.memory());
	fb::check::checker c{types, catalog, *report};
	c.run(program);
	return report;
}
