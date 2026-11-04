#ifndef fb_check_type_hpp
#define fb_check_type_hpp

#include "ca/integers.hpp"
#include "ca/sv.hpp"
#include "core/memory.hpp"

namespace fb::check {

	/* What a type is. The checker is gradual: unknown stands for anything it cannot tell, and
	   is compatible with every type, so only what is known for sure is reported. */
	enum class form : ca::u8 {
		unknown,
		unit,          // nil / (): fits anywhere, as a missing value does at run time
		integer,
		floating,
		string,
		boolean,
		file,          // a path: a record of name, stem, suffix, parent (String), mtime, size (Int)
		command,
		map,           // a dynamic map: its fields are not tracked
		list,          // kids[0]: the element
		tuple,         // kids: the elements, when sized
		function,      // kids[0]: the parameters (a tuple), kids[1]: the result
		object,        // name: the class ("" for a bare record); names/kids: the fields when sized
		alternatives,  // kids: the members (A|B|C); never nested
		variable,      // name: a type variable of a builtin's signature ("a")
	};

	struct type final {
		fb::check::form kind;
		fb::list<fb::check::type*> kids;
		fb::list<ca::sv::str> names;  // file and object fields, parallel to kids
		ca::sv::str name;             // object: the class; variable: its name
		bool sized;                   // tuple: kids are all the elements; object: names are all the fields
		bool variadic;                // list: the "...T" tail of a parameter list
		bool native;                  // object: a class registered by C++ code, whose methods are all known
		ca::u32 required;             // function: parameters that must be given (the rest are optional)

		[[nodiscard]] auto is(const fb::check::form k) const noexcept -> bool {
			return kind == k;
		}

		[[nodiscard]] auto kid(const ca::usz index) const noexcept -> fb::check::type* {
			return index < kids.size() ? kids.data()[index] : nullptr;
		}

		[[nodiscard]] auto size(void) const noexcept -> ca::usz {
			return kids.size();
		}

		/* A file or object field by name; nullptr when absent. */
		[[nodiscard]] auto field(const ca::sv::str key) const noexcept -> fb::check::type* {

			for (ca::usz i = 0U; i < names.size(); ++i)
				if (names.data()[i] == key)
					return kids.data()[i];
			return nullptr;
		}

		/* function: the parameter list, the result. */
		[[nodiscard]] auto parameters(void) const noexcept -> fb::check::type* {
			return this->kid(0U);
		}

		[[nodiscard]] auto result(void) const noexcept -> fb::check::type* {
			return this->kid(1U);
		}
	};

	/* Makes types in a memory. Scalars are shared (types are never changed once built, except
	   records while their builder fills them). */
	class types final {

		private:

			fb::memory& _memory;
			fb::check::type* _unknown;
			fb::check::type* _unit;
			fb::check::type* _integer;
			fb::check::type* _floating;
			fb::check::type* _string;
			fb::check::type* _boolean;
			fb::check::type* _command;
			fb::check::type* _map;

		public:

			explicit types(fb::memory& memory) noexcept;

			types(const types&) = delete;

			auto operator=(const types&) -> types& = delete;

			[[nodiscard]] auto memory(void) noexcept -> fb::memory& {
				return _memory;
			}

			[[nodiscard]] auto make(fb::check::form kind) noexcept -> fb::check::type*;

			[[nodiscard]] auto unknown(void) noexcept -> fb::check::type* { return _unknown; }
			[[nodiscard]] auto unit(void) noexcept -> fb::check::type* { return _unit; }
			[[nodiscard]] auto integer(void) noexcept -> fb::check::type* { return _integer; }
			[[nodiscard]] auto floating(void) noexcept -> fb::check::type* { return _floating; }
			[[nodiscard]] auto string(void) noexcept -> fb::check::type* { return _string; }
			[[nodiscard]] auto boolean(void) noexcept -> fb::check::type* { return _boolean; }
			[[nodiscard]] auto command(void) noexcept -> fb::check::type* { return _command; }
			[[nodiscard]] auto map(void) noexcept -> fb::check::type* { return _map; }

			/* Int|Float. */
			[[nodiscard]] auto number(void) noexcept -> fb::check::type*;

			/* A new file record with the standard fields. */
			[[nodiscard]] auto file(void) noexcept -> fb::check::type*;

			[[nodiscard]] auto list(fb::check::type* element) noexcept -> fb::check::type*;

			/* The "...T" tail of a parameter list: zero or more T. */
			[[nodiscard]] auto variadic(fb::check::type* element) noexcept -> fb::check::type*;

			/* An empty, sized tuple; push its elements. */
			[[nodiscard]] auto tuple(void) noexcept -> fb::check::type*;

			/* A tuple of unknown size. */
			[[nodiscard]] auto any_tuple(void) noexcept -> fb::check::type*;

			/* parameters: a tuple; required: how many must be given. */
			[[nodiscard]] auto function(fb::check::type* parameters, fb::check::type* result, ca::u32 required) noexcept -> fb::check::type*;

			/* A function of every parameter of one tuple, all required. */
			[[nodiscard]] auto function(fb::check::type* parameters, fb::check::type* result) noexcept -> fb::check::type*;

			/* An object of class name, without fields (sized: with all of them, to be pushed). */
			[[nodiscard]] auto object(ca::sv::str name, bool sized) noexcept -> fb::check::type*;

			[[nodiscard]] auto variable(ca::sv::str name) noexcept -> fb::check::type*;

			/* A|B: members flattened, duplicates dropped; one member alone stands for itself, and
			   unknown absorbs everything. */
			[[nodiscard]] auto either(fb::check::type* a, fb::check::type* b) noexcept -> fb::check::type*;

			auto push(fb::check::type* into, fb::check::type* kid) noexcept -> void;

			/* Adds a field to a file or object record (or replaces one of that name). */
			auto set_field(fb::check::type* record, ca::sv::str name, fb::check::type* t) noexcept -> void;

			/* A copy of a record, to add fields to. */
			[[nodiscard]] auto copy_record(const fb::check::type* record) noexcept -> fb::check::type*;
	};

	/* Whether a value of one type may stand where the other is expected. unknown, unit and
	   variables fit everything; an alternative fits when one of its members does; Int and
	   Float fit each other; an unsized tuple fits any tuple; objects of different classes never
	   do, and fields are compared by name when both are sized. */
	[[nodiscard]] auto compatible(const fb::check::type* a, const fb::check::type* b) noexcept -> bool;

	/* Whether every value of it is a number: Int, Float, or alternatives of them. */
	[[nodiscard]] auto numeric(const fb::check::type* t) noexcept -> bool;

	/* Whether the two are the same type, structurally. */
	[[nodiscard]] auto same(const fb::check::type* a, const fb::check::type* b) noexcept -> bool;

	/* The type as the language writes it: Int, List<File>, Tuple<Int, String>, (Int, ? -> Bool),
	   Point. detailed: objects with their fields, Point{x: Int, y: Int}. */
	[[nodiscard]] auto render(fb::memory& memory, const fb::check::type* t, bool detailed = false) noexcept -> ca::sv::str;

	/* What a builtin's type variables stand for at one call. */
	class bindings final {

		private:

			fb::list<ca::sv::str> _names;
			fb::list<fb::check::type*> _types;

		public:

			explicit bindings(fb::memory& memory) noexcept
			: _names{memory.list<ca::sv::str>()}, _types{memory.list<fb::check::type*>()} {
			}

			[[nodiscard]] auto get(ca::sv::str name) const noexcept -> fb::check::type*;

			auto set(fb::memory& memory, ca::sv::str name, fb::check::type* t) noexcept -> void;
	};

	/* Whether arg fits expected, binding the variables of expected that arg determines; a
	   variable already bound must fit again (unknown arguments leave it as it is). */
	[[nodiscard]] auto match(fb::check::types& types, const fb::check::type* expected, fb::check::type* arg, fb::check::bindings& bound) noexcept -> bool;

	/* t with its bound variables replaced, and unbound ones by unknown. */
	[[nodiscard]] auto substitute(fb::check::types& types, fb::check::type* t, const fb::check::bindings& bound) noexcept -> fb::check::type*;

	/* Whether t mentions a type variable. */
	[[nodiscard]] auto generic(const fb::check::type* t) noexcept -> bool;

	/* The element type of a list or tuple (unknown for a tuple of mixed or unknown elements,
	   or anything else). */
	[[nodiscard]] auto element(fb::check::types& types, const fb::check::type* t) noexcept -> fb::check::type*;

} // namespace fb::check

#endif // fb_check_type_hpp
