#ifndef fb_check_checker_hpp
#define fb_check_checker_hpp

#include "check/catalog.hpp"
#include "check/type.hpp"
#include "syntax/ast.hpp"

namespace fb::check {

	enum class severity : ca::u8 {
		error,    // the script would misbehave: running it is refused
		warning,  // suspicious but legal (a bare word standing for its own text)
	};

	struct diagnostic final {
		fb::check::severity level;
		ca::sv::str message;
		fb::span at;
	};

	enum class symbol_kind : ca::u8 {
		function,  // a top-level definition of a lambda
		value,     // a top-level definition of anything else
		class_,    // type NAME(...) = ...
		method,    // method CLASS.NAME = ..., or a copy of one
	};

	/* A top-level name a script defines, for outlines and go-to-definition. */
	struct symbol final {
		ca::sv::str name;
		fb::check::symbol_kind kind;
		const fb::node* site;   // the statement defining it
		ca::sv::str container;  // the class of a method; empty otherwise
	};

	/* What checking a program found: diagnostics in source order, the type of every node it
	   inferred, and the top-level symbols. Everything lives in the types' memory, and refers to
	   the program's nodes. */
	class report final {

		private:

			fb::list<fb::check::diagnostic> _diagnostics;
			fb::list<fb::check::type*> _types;
			fb::list<fb::check::symbol> _symbols;
			fb::list<const fb::node*> _targets;
			ca::usz _errors;

			friend class checker;

		public:

			explicit report(fb::memory& memory) noexcept
			: _diagnostics{memory.list<fb::check::diagnostic>()}, _types{memory.list<fb::check::type*>()},
			  _symbols{memory.list<fb::check::symbol>()}, _targets{memory.list<const fb::node*>()}, _errors{0U} {
			}

			[[nodiscard]] auto diagnostics(void) const noexcept -> ca::borrowed_slice<const fb::check::diagnostic> {
				return _diagnostics.view();
			}

			[[nodiscard]] auto symbols(void) const noexcept -> ca::borrowed_slice<const fb::check::symbol> {
				return _symbols.view();
			}

			[[nodiscard]] auto errors(void) const noexcept -> ca::usz {
				return _errors;
			}

			/* The type inferred for a node (at its last visit: a lambda's body as applied last);
			   nullptr when the checker never reached it (a parameter list, a type annotation). */
			[[nodiscard]] auto type_of(const fb::node* const n) const noexcept -> fb::check::type* {
				return n != nullptr && n->id < _types.size() ? _types.data()[n->id] : nullptr;
			}

			/* What the name at n stands for: the parameter, binding, pattern or statement defining it
			   (the method's, for a method call or field); nullptr for a builtin, an undefined name, or a
			   node that uses no name. */
			[[nodiscard]] auto target_of(const fb::node* const n) const noexcept -> const fb::node* {
				return n != nullptr && n->id < _targets.size() ? _targets.data()[n->id] : nullptr;
			}
	};

	/* Infers the types of a parsed program and reports what cannot work: calls with the wrong
	   arity or argument types, fields a value cannot have, arithmetic on non-numbers, unknown
	   functions, types and methods, unbounded recursion. Gradual: what cannot be known
	   statically (a JSON value, a lambda parameter without annotation) is not reported. Reads
	   the catalog for the names the script does not define; never runs anything. */
	[[nodiscard]] auto check(fb::check::types& types, const fb::check::catalog& catalog, const fb::node* program) noexcept -> fb::check::report*;

} // namespace fb::check

#endif // fb_check_checker_hpp
