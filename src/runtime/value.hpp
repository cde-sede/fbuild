#ifndef fb_runtime_value_hpp
#define fb_runtime_value_hpp

#include "ca/integers.hpp"
#include "ca/floats.hpp"
#include "ca/result.hpp"
#include "ca/sv.hpp"
#include "core/diagnostic.hpp"
#include "core/memory.hpp"
#include "syntax/ast.hpp"

namespace fb {

	class interpreter;
	struct value;
	struct environment;
	struct class_info;

	/* What evaluating something gave: a value, or why the script stops. */
	using outcome = ca::result<fb::value*, fb::diagnostic>;

	/* The arguments a builtin receives: evaluated, splats flattened (thunks for a lazy one). */
	class arguments final {

		private:

			fb::value* const* _data;
			ca::usz _size;

		public:

			constexpr arguments(fb::value* const* const data, const ca::usz size) noexcept
			: _data{data}, _size{size} {
			}

			[[nodiscard]] constexpr auto size(void) const noexcept -> ca::usz {
				return _size;
			}

			[[nodiscard]] constexpr auto operator[](const ca::usz index) const noexcept -> fb::value* {
				return _data[index];
			}

			[[nodiscard]] constexpr auto data(void) const noexcept -> fb::value* const* {
				return _data;
			}

			[[nodiscard]] constexpr auto begin(void) const noexcept -> fb::value* const* {
				return _data;
			}

			[[nodiscard]] constexpr auto end(void) const noexcept -> fb::value* const* {
				return _data + _size;
			}
	};

	/* A builtin or method implemented in C++. */
	using native = auto (*)(fb::interpreter&, fb::arguments) noexcept -> fb::outcome;

	/* The declared result of a builtin, for the type checker; any unifies with everything. */
	enum class ret : ca::u8 {
		any,
		integer,
		floating,
		string,
		boolean,
		file,
		list,
		map,
		unit,
		list_string,
		number,
	};

	/* builtin::flags */
	inline constexpr ca::u32 lazy = 1U;    // arguments arrive as thunks; fb::interpreter::force them
	inline constexpr ca::u32 global = 2U;  // the name is a bound value (interpreter::bind), not a call
	inline constexpr ca::u32 located = 4U; // a failure it returns is placed at the call

	/* A builtin's registration: name, implementation, arity (max < 0: variadic), result, flags,
	   one-line documentation, and optionally its signature for the type checker, as
	   fb::check::parse_signature reads it ("List<a>, (a -> ?) -> List<a>"); without one, it is
	   checked from its arity and result alone. */
	struct builtin final {
		ca::sv::str name;
		fb::native fn;
		int min;
		int max;
		fb::ret result;
		ca::u32 flags;
		ca::sv::str doc;
		ca::sv::str signature = {};
	};

	enum class kind : ca::u8 {
		unit,
		integer,
		floating,
		string,
		boolean,
		file,
		list,
		seq,
		tuple,
		command,
		lambda,
		morph,
		builtin,
		map,
		thunk,
		object,
		native,
		class_,
		matcher,  // a match { ... }: body (the match node) over scope
	};

	struct binding final {
		ca::sv::str name;
		fb::value* value;
		fb::binding* next;
	};

	struct environment final {
		fb::binding* head;
		fb::value* arg;
		fb::environment* parent;
	};

	/* A runtime value; every one lives in its interpreter's memory and is never freed alone.
	   - integer, boolean: integer
	   - floating: floating
	   - string, file, builtin (its name): text
	   - morph: text (from) and other (to)
	   - list, tuple, command, seq-less sequences: items
	   - map, object: keys and items, insertion ordered
	   - lambda: params and body over scope; thunk: body over scope; seq: body (the >> node) over scope;
	     matcher: body (the match node) over scope
	   - object: cls and data; native: fn; class_: cls */
	struct value final {
		fb::kind kind;
		ca::i64 integer;
		ca::f64 floating;
		ca::sv::str text;
		ca::sv::str other;
		fb::list<fb::value*> items;
		fb::list<ca::sv::str> keys;
		const fb::node* params;
		const fb::node* body;
		fb::environment* scope;
		ca::sv::str label;
		fb::class_info* cls;
		void* data;
		fb::native fn;

		value(const fb::kind k, const ca::arena_allocator allocator) noexcept
		: kind{k}, integer{0}, floating{0.0}, text{}, other{}, items{allocator}, keys{allocator},
		  params{nullptr}, body{nullptr}, scope{nullptr}, label{}, cls{nullptr}, data{nullptr}, fn{nullptr} {
		}

		[[nodiscard]] auto is(const fb::kind k) const noexcept -> bool {
			return kind == k;
		}

		[[nodiscard]] auto is_sequence(void) const noexcept -> bool {
			return kind == fb::kind::list || kind == fb::kind::tuple;
		}

		[[nodiscard]] auto is_mapping(void) const noexcept -> bool {
			return kind == fb::kind::map || kind == fb::kind::object;
		}

		[[nodiscard]] auto is_number(void) const noexcept -> bool {
			return kind == fb::kind::integer || kind == fb::kind::boolean || kind == fb::kind::floating;
		}

		[[nodiscard]] auto size(void) const noexcept -> ca::usz {
			return items.size();
		}

		[[nodiscard]] auto at(const ca::usz index) const noexcept -> fb::value* {
			return items.data()[index];
		}

		[[nodiscard]] auto as_float(void) const noexcept -> ca::f64 {
			return kind == fb::kind::floating ? floating : static_cast<ca::f64>(integer);
		}

		/* A path or string's text; empty for anything else. */
		[[nodiscard]] auto path(void) const noexcept -> ca::sv::str {
			return kind == fb::kind::file || kind == fb::kind::string ? text : ca::sv::str{""};
		}

		/* Whether a guard or logical operator takes it as true: not unit, not 0, not false. */
		[[nodiscard]] auto truthy(void) const noexcept -> bool {

			switch (kind) {
				case fb::kind::unit:
					return false;
				case fb::kind::boolean:
				case fb::kind::integer:
					return integer != 0;
				case fb::kind::floating:
					return floating != 0.0;
				default:
					return true;
			}
		}

		/* The value under key in a map or object; nullptr when absent or not a mapping. */
		[[nodiscard]] auto find(const ca::sv::str key) const noexcept -> fb::value* {

			if (!this->is_mapping())
				return nullptr;
			for (ca::usz i = 0U; i < keys.size(); ++i)
				if (keys.data()[i] == key)
					return items.data()[i];
			return nullptr;
		}
	};

	/* A class: its name, and its methods by name (language lambdas or natives). */
	struct class_info final {
		ca::sv::str name;
		fb::list<ca::sv::str> names;
		fb::list<fb::value*> methods;

		[[nodiscard]] auto method(const ca::sv::str key) const noexcept -> fb::value* {

			for (ca::usz i = 0U; i < names.size(); ++i)
				if (names.data()[i] == key)
					return methods.data()[i];
			return nullptr;
		}
	};

} // namespace fb

#endif // fb_runtime_value_hpp
