#include "lsp/tokens.hpp"

namespace fb::lsp::impl {

	inline constexpr ca::sv::str builtin_types[] = {
		"Int", "Float", "Number", "String", "Bool", "Path", "File", "Command", "Unit", "Map", "Json", "List", "Tuple",
	};

	class classifier final {

		private:

			using type = fb::lsp::token_type;

			fb::memory& _memory;
			const fb::check::report& _report;
			const fb::check::catalog& _catalog;
			fb::list<ca::u8> _roles;  // by node id: 1 + the token type of what the node declares, 0 for none
			fb::list<fb::lsp::semantic_token> _tokens;

			[[nodiscard]] static auto _is_class(const fb::node* const value) noexcept -> bool {
				return value != nullptr && value->kind == fb::node_kind::lambda && value->kid(1U) != nullptr && value->kid(1U)->kind == fb::node_kind::objnew;
			}

			/* What a definition of value is: a class, a function or a variable. */
			[[nodiscard]] static auto _defined(const fb::node* const value) noexcept -> type {

				if (classifier::_is_class(value))
					return type::class_;
				if (value != nullptr && (value->kind == fb::node_kind::lambda || value->kind == fb::node_kind::match_))
					return type::function;
				return type::variable;
			}

			auto _set(const fb::node* const n, const type t) noexcept -> void {

				if (n != nullptr && n->id < _roles.size())
					_roles.data()[n->id] = static_cast<ca::u8>(static_cast<ca::u8>(t) + 1U);
			}

			[[nodiscard]] auto _role(const fb::node* const n) const noexcept -> ca::optional<type> {

				if (n == nullptr || n->id >= _roles.size() || _roles.data()[n->id] == 0U)
					return ca::nullopt;
				return static_cast<type>(_roles.data()[n->id] - 1U);
			}

			auto _emit(const fb::token* const t, const type kind, const ca::u32 modifiers) noexcept -> void {

				if (t == nullptr || t->start.line == 0 || t->end.line != t->start.line || t->end.col < t->start.col)
					return;
				_memory.push(_tokens, fb::lsp::semantic_token{t->start, t->end, kind, modifiers});
			}

			static auto _highest(const fb::node* const n, ca::u32& top) noexcept -> void {

				if (n == nullptr)
					return;
				if (n->id > top)
					top = n->id;
				for (const fb::node* const kid : n->kids)
					classifier::_highest(kid, top);
			}

			/* Pass 1: what each declaring node declares, so that uses take its kind. */
			auto _mark(const fb::node* const n) noexcept -> void {

				if (n == nullptr)
					return;
				switch (n->kind) {
					case fb::node_kind::lambda:
						if (const fb::node* const params = n->kid(0U))
							for (const fb::node* const p : params->kids) {
								if (p->kind == fb::node_kind::id || p->kind == fb::node_kind::projection)
									this->_set(p, type::parameter);
								else if (p->kind == fb::node_kind::capture)
									this->_set(p->kid(0U), type::parameter);
								else if (p->kind == fb::node_kind::tuple)
									for (const fb::node* const sub : p->kids)
										this->_set(sub, type::parameter);
							}
						break;
					case fb::node_kind::assign:
					case fb::node_kind::eager:
					case fb::node_kind::binding:
						this->_set(n, classifier::_defined(n->kid(0U)));
						break;
					case fb::node_kind::pbind:
						this->_set(n, type::variable);
						break;
					case fb::node_kind::methoddef:
					case fb::node_kind::methodcopy:
						this->_set(n, type::method);
						break;
					default:
						break;
				}
				for (const fb::node* const kid : n->kids)
					this->_mark(kid);
			}

			/* A name used at n: the kind of what it resolves to. */
			auto _use(const fb::node* const n) noexcept -> void {

				if (const fb::node* const site = _report.target_of(n)) {
					auto role = this->_role(site);
					this->_emit(n->tok, role ? *role : type::variable, 0U);
					return;
				}
				const ca::sv::str name = n->tok->text;
				if (_catalog.builtin(name) != nullptr) {
					this->_emit(n->tok, type::function, fb::lsp::default_library);
					return;
				}
				if (const fb::check::type* const g = _catalog.global(name)) {
					const bool native = g->is(fb::check::form::object) && g->native;
					this->_emit(n->tok, native ? type::namespace_ : type::variable, fb::lsp::default_library);
					return;
				}
				this->_emit(n->tok, type::variable, fb::lsp::unresolved);
			}

			auto _kids(const fb::node* const n) noexcept -> void {

				for (const fb::node* const kid : n->kids)
					this->_visit(kid, n);
			}

			/* Pass 2: the tokens, declarations, uses and literals. */
			auto _visit(const fb::node* const n, const fb::node* const parent) noexcept -> void {

				if (n == nullptr)
					return;
				switch (n->kind) {
					case fb::node_kind::assign:
					case fb::node_kind::eager:
					case fb::node_kind::binding:
					case fb::node_kind::pbind: {
						auto role = this->_role(n);
						this->_emit(n->tok, role ? *role : type::variable, fb::lsp::declaration);
						this->_kids(n);
						return;
					}
					case fb::node_kind::methoddef:
						if (const fb::node* const owner = n->kid(0U))
							this->_emit(owner->tok, type::class_, 0U);
						this->_emit(n->tok, type::method, fb::lsp::declaration);
						this->_visit(n->kid(1U), n);
						return;
					case fb::node_kind::methodcopy:
						if (const fb::node* const to = n->kid(0U))
							this->_emit(to->tok, type::class_, 0U);
						this->_emit(n->tok, type::method, fb::lsp::declaration);
						if (const fb::node* const from = n->kid(1U))
							this->_emit(from->tok, type::class_, 0U);
						if (const fb::node* const source = n->kid(2U))
							this->_emit(source->tok, type::method, 0U);
						return;
					case fb::node_kind::id:
						if (n->tok != nullptr) {
							if (auto role = this->_role(n))
								this->_emit(n->tok, *role, fb::lsp::declaration);
							else
								this->_use(n);
						}
						this->_kids(n);
						return;
					case fb::node_kind::projection:
						if (n->tok != nullptr) {
							if (auto role = this->_role(n))
								this->_emit(n->tok, *role, fb::lsp::declaration);
							else if (_report.target_of(n) != nullptr)
								this->_use(n);
							else
								this->_emit(n->tok, n->tok->kind == fb::tok::integer ? type::number : type::property, 0U);
						}
						return;
					case fb::node_kind::field: {
						if (n->tok != nullptr) {
							const bool called = parent != nullptr && parent->kind == fb::node_kind::call && parent->kid(0U) == n;
							if (n->tok->kind == fb::tok::integer)
								this->_emit(n->tok, type::number, 0U);
							else
								this->_emit(n->tok, _report.target_of(n) != nullptr || called ? type::method : type::property, 0U);
						}
						this->_kids(n);
						return;
					}
					case fb::node_kind::typeexpr: {
						if (n->tok != nullptr) {
							const ca::sv::str name = n->tok->text;
							bool builtin = false;
							for (const ca::sv::str t : fb::lsp::impl::builtin_types)
								builtin = builtin || t == name;
							if (_report.target_of(n) != nullptr)
								this->_emit(n->tok, type::class_, 0U);
							else if (builtin)
								this->_emit(n->tok, type::type, fb::lsp::default_library);
							else if (_catalog.native(name) != nullptr)
								this->_emit(n->tok, type::class_, fb::lsp::default_library);
							else
								this->_emit(n->tok, type::type, 0U);
						}
						this->_kids(n);
						return;
					}
					case fb::node_kind::objnew:
						this->_emit(n->tok, type::class_, 0U);
						this->_kids(n);
						return;
					case fb::node_kind::construct:
						this->_emit(n->tok, type::type, fb::lsp::default_library);
						this->_kids(n);
						return;
					case fb::node_kind::cfield:
						this->_emit(n->tok, type::property, 0U);
						this->_kids(n);
						return;
					case fb::node_kind::integer:
					case fb::node_kind::floating:
						this->_emit(n->tok, type::number, 0U);
						return;
					case fb::node_kind::boolean:
						this->_emit(n->tok, type::keyword, 0U);
						return;
					case fb::node_kind::nil:
						if (n->tok != nullptr && n->tok->kind == fb::tok::nil)
							this->_emit(n->tok, type::keyword, 0U);
						return;
					case fb::node_kind::string:
						if (n->size() == 0U && n->tok != nullptr) {
							if (n->tok->kind != fb::tok::string) {
								this->_emit(n->tok, type::property, 0U);  // a bare map key: {name: ...}
								return;
							}
							// The token starts after the opening quote and ends on the closing one.
							fb::token quoted = *n->tok;
							--quoted.start.col;
							this->_emit(&quoted, type::string, 0U);
						}
						this->_kids(n);
						return;
					case fb::node_kind::strtext:
					case fb::node_kind::path:
						this->_emit(n->tok, type::string, 0U);
						return;
					case fb::node_kind::logical:
						if (n->tok != nullptr && (n->tok->kind == fb::tok::and_ || n->tok->kind == fb::tok::or_))
							this->_emit(n->tok, type::keyword, 0U);
						this->_kids(n);
						return;
					case fb::node_kind::not_:
						if (n->tok != nullptr && n->tok->kind == fb::tok::not_)
							this->_emit(n->tok, type::keyword, 0U);
						this->_kids(n);
						return;
					case fb::node_kind::else_:
					case fb::node_kind::match_:
						if (n->tok != nullptr && (n->tok->text == "else" || n->tok->text == "match"))
							this->_emit(n->tok, type::keyword, 0U);
						this->_kids(n);
						return;
					default:
						this->_kids(n);
						return;
				}
			}

			[[nodiscard]] static auto _before(const fb::position a, const fb::position b) noexcept -> bool {
				return a.line < b.line || (a.line == b.line && a.col < b.col);
			}

		public:

			classifier(fb::memory& memory, const fb::check::report& report, const fb::check::catalog& catalog) noexcept
			: _memory{memory}, _report{report}, _catalog{catalog}, _roles{memory.list<ca::u8>()}, _tokens{memory.list<fb::lsp::semantic_token>()} {
			}

			[[nodiscard]] auto run(const fb::node* const program) noexcept -> fb::list<fb::lsp::semantic_token> {

				ca::u32 top = 0U;
				classifier::_highest(program, top);
				for (ca::u32 k = 0U; k <= top; ++k)
					_memory.push(_roles, ca::u8{0U});
				this->_mark(program);
				this->_visit(program, nullptr);
				// Mostly in order already (the tree follows the source): insertion sort, then drop
				// what overlaps the token before (one token seen from two nodes).
				fb::lsp::semantic_token* const t = _tokens.data();
				for (ca::usz k = 1U; k < _tokens.size(); ++k)
					for (ca::usz j = k; j > 0U && classifier::_before(t[j].start, t[j - 1U].start); --j) {
						const fb::lsp::semantic_token swap = t[j];
						t[j] = t[j - 1U];
						t[j - 1U] = swap;
					}
				fb::list<fb::lsp::semantic_token> kept = _memory.list<fb::lsp::semantic_token>();
				for (const fb::lsp::semantic_token& token : _tokens)
					if (kept.size() == 0U || classifier::_before(kept.data()[kept.size() - 1U].end, token.start))
						_memory.push(kept, token);
				return kept;
			}
	};

} // namespace fb::lsp::impl

auto fb::lsp::semantic_tokens(fb::memory& memory, const fb::node* const program, const fb::check::report& report, const fb::check::catalog& catalog) noexcept
	-> fb::list<fb::lsp::semantic_token> {

	fb::lsp::impl::classifier c{memory, report, catalog};
	return c.run(program);
}
