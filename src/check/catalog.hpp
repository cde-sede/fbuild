#ifndef fb_check_catalog_hpp
#define fb_check_catalog_hpp

#include "ca/hashmap.hpp"
#include "ca/result.hpp"
#include "check/type.hpp"

namespace fb::check {

	/* A builtin as the checker sees it: arity (max < 0: variadic), its signature when one was
	   given, else what it returns. */
	struct builtin_entry final {
		ca::sv::str name;
		int min;
		int max;
		fb::check::type* signature;  // a function, or nullptr
		fb::check::type* result;     // the result when there is no signature
		ca::sv::str doc;
	};

	/* A class registered by C++ code (a plugin's namespace object, `regex`): its methods are
	   all known, so any other name on it is an error. */
	struct native_class final {
		ca::sv::str name;
		fb::list<ca::sv::str> methods;

		[[nodiscard]] auto has(const ca::sv::str method) const noexcept -> bool {

			for (const ca::sv::str m : methods)
				if (m == method)
					return true;
			return false;
		}
	};

	/* Everything a script can name besides its own definitions: builtins, bound values and
	   native classes. An embedder fills it from what is registered (the interpreter's
	   describe()), a language server from the same or a fixed list; the checker only reads it. */
	class catalog final {

		private:

			fb::check::types& _types;
			ca::hashmap<ca::sv::str, fb::check::builtin_entry, ca::arena_allocator> _builtins;
			ca::hashmap<ca::sv::str, fb::check::type*, ca::arena_allocator> _globals;
			ca::hashmap<ca::sv::str, fb::check::native_class*, ca::arena_allocator> _classes;
			fb::list<ca::sv::str> _names;

		public:

			explicit catalog(fb::check::types& types) noexcept;

			catalog(const catalog&) = delete;

			auto operator=(const catalog&) -> catalog& = delete;

			[[nodiscard]] auto types(void) noexcept -> fb::check::types& {
				return _types;
			}

			/* Adds (or replaces) a builtin. signature: "List<a>, (a -> ?) -> List<a>", or empty
			   to type it from result alone; the error says what in it is malformed. */
			[[nodiscard]] auto add_builtin(ca::sv::str name, int min, int max, ca::sv::str signature, fb::check::type* result, ca::sv::str doc) noexcept
				-> ca::result<void, ca::sv::str>;

			/* Adds (or replaces) a bound value of type t: `args`, `regex`. */
			auto add_global(ca::sv::str name, fb::check::type* t) noexcept -> void;

			/* The native class of that name, made empty the first time. */
			[[nodiscard]] auto add_class(ca::sv::str name) noexcept -> fb::check::native_class*;

			auto add_method(fb::check::native_class* cls, ca::sv::str method) noexcept -> void;

			[[nodiscard]] auto builtin(ca::sv::str name) const noexcept -> const fb::check::builtin_entry*;

			[[nodiscard]] auto global(ca::sv::str name) const noexcept -> fb::check::type*;

			[[nodiscard]] auto native(ca::sv::str name) const noexcept -> const fb::check::native_class*;

			/* Every builtin and global name, in the order added (completion). */
			[[nodiscard]] auto names(void) const noexcept -> ca::borrowed_slice<const ca::sv::str> {
				return _names.view();
			}
	};

	/* A signature, "String, Int? -> List<String>": parameters (T? optional, ...T variadic, ()
	   for none) and a result. Types: ? (anything), Int Float Number String Bool File Path
	   (File|String) Command Unit Map Json (?), List<T>, List, Tuple<T, ...>, Tuple (any size),
	   { name: T, ... } (a record), (T, U -> R) (a function), A|B, and lowercase names as type
	   variables. */
	[[nodiscard]] auto parse_signature(fb::check::types& types, ca::sv::str text) noexcept -> ca::result<fb::check::type*, ca::sv::str>;

	/* One type, in the notation of parse_signature. */
	[[nodiscard]] auto parse_type(fb::check::types& types, ca::sv::str text) noexcept -> ca::result<fb::check::type*, ca::sv::str>;

} // namespace fb::check

#endif // fb_check_catalog_hpp
