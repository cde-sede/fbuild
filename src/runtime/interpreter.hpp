#ifndef fb_runtime_interpreter_hpp
#define fb_runtime_interpreter_hpp

#include "ca/failure.hpp"
#include "ca/hashmap.hpp"
#include "ca/optional.hpp"
#include "core/memory.hpp"
#include "runtime/value.hpp"

namespace fb::check {

	class catalog;

} // namespace fb::check

namespace fb {

	class runner;

	/* What config(...) set for the runner; see SKILLS.md "config keys". */
	struct run_settings final {
		int jobs;             // concurrent commands per parallel group; 0: the processor count
		bool verbose;
		bool keep_going;
		bool silent;
		int color;            // 0: auto (a terminal), 1: on, 2: off
		ca::i64 spinner_ms;
		int tail_lines;
		fb::value* spinner;   // Int -> String, or null
	};

	/* One script interpreter: its definitions, builtins, classes and stores, all in the memory
	   it is given. Several may live at once; they share nothing but the process. */
	class interpreter final {

		private:

			using self = fb::interpreter;

			struct definition final {
				const fb::node* rhs;
				fb::value* value;
				bool doing;
				bool pure;
			};

			fb::memory& _memory;
			fb::value* _unit;
			ca::hashmap<ca::sv::str, definition, ca::arena_allocator> _definitions;
			ca::hashmap<ca::sv::str, fb::builtin, ca::arena_allocator> _builtins;
			fb::list<ca::sv::str> _builtin_order;
			ca::hashmap<ca::sv::str, fb::value*, ca::arena_allocator> _bound;
			ca::hashmap<ca::sv::str, fb::class_info*, ca::arena_allocator> _classes;
			fb::environment _top;
			fb::value* _args;
			fb::value* _cwd;
			fb::value* _globals;
			fb::value* _shared;
			fb::list<ca::sv::str> _deferred;
			bool _in_run;
			bool _bench;
			bool _failed;
			bool _capture_failed;
			fb::run_settings _settings;
			fb::runner* _runner;
			const char* _stack_top;
			ca::usz _stack_room;

			template <typename T>
			static auto _must(ca::result<T>&& r) noexcept -> void {

				if (!r) [[unlikely]]
					ca::trap("fbuild: out of memory");
			}

			// -- evaluation (eval.cpp) ---------------------------------------

			[[nodiscard]] auto _eval(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_number(const fb::node* n, fb::environment* scope, ca::i64& integer, ca::f64& floating, bool& is_float) noexcept -> ca::result<void, fb::diagnostic>;
			[[nodiscard]] auto _eval_string(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_store(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_id(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_sequence(const fb::node* n, fb::environment* scope, fb::kind k) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_map(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_pipe(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_guard(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_field(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_object(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_method_copy(const fb::node* n) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_construct(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_compare(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_logical(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_command(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_call(const fb::node* n, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _eval_arguments(const fb::node* call, fb::environment* scope) noexcept -> ca::result<fb::list<fb::value*>, fb::diagnostic>;
			[[nodiscard]] auto _eval_match(const fb::node* n, fb::value* arg, fb::environment* scope) noexcept -> fb::outcome;
			[[nodiscard]] auto _matches(const fb::node* pattern, fb::value* v, fb::environment* frame, fb::environment* scope) noexcept -> ca::result<bool, fb::diagnostic>;
			[[nodiscard]] auto _has_type(const fb::value* v, const fb::node* annotation) noexcept -> bool;
			[[nodiscard]] auto _guarded_scope(const fb::node* where, fb::environment* scope) noexcept -> ca::result<fb::environment*, fb::diagnostic>;
			[[nodiscard]] auto _add_element(fb::value* into, const fb::node* element, fb::environment* scope) noexcept -> ca::result<void, fb::diagnostic>;
			[[nodiscard]] auto _definition(ca::sv::str name, bool memoized) noexcept -> ca::result<fb::value*, fb::diagnostic>;
			[[nodiscard]] auto _dispatch(const fb::builtin& b, fb::arguments args) noexcept -> fb::outcome;
			auto _bind_params(fb::environment* frame, const fb::node* params, fb::value* arg) noexcept -> void;
			auto _put(fb::environment* scope, ca::sv::str name, fb::value* v) noexcept -> void;
			[[nodiscard]] static auto _lookup(const fb::environment* scope, ca::sv::str name) noexcept -> fb::value*;
			[[nodiscard]] static auto _current_arg(const fb::environment* scope) noexcept -> fb::value*;
			[[nodiscard]] auto _positional(fb::value* arg, ca::usz index) const noexcept -> fb::value*;

			// -- running (run.cpp) -------------------------------------------

			auto _flush_deferred(void) noexcept -> void;
			[[nodiscard]] auto _exec(fb::value* v) noexcept -> ca::result<bool, fb::diagnostic>;
			[[nodiscard]] auto _collect(fb::value* v, void* commands) noexcept -> ca::result<void, fb::diagnostic>;
			[[nodiscard]] auto _capture_one(fb::value* v) noexcept -> fb::outcome;

		public:

			explicit interpreter(fb::memory& memory) noexcept;

			interpreter(const self&) = delete;

			auto operator=(const self&) -> self& = delete;

			// -- programs ------------------------------------------------------

			/* Sets the script's `args` (each copied); the CLI passes what follows its own flags. */
			auto set_args(ca::borrowed_slice<const ca::sv::str> args) noexcept -> void;

			/* Parses and runs source, top to bottom. */
			[[nodiscard]] auto run(ca::sv::str source) noexcept -> ca::result<void, fb::diagnostic>;

			/* Parses and runs an already parsed program. */
			[[nodiscard]] auto run(const fb::node* program) noexcept -> ca::result<void, fb::diagnostic>;

			/* The top-level definition name, forced; unit when there is none. */
			[[nodiscard]] auto global(ca::sv::str name) noexcept -> fb::outcome;

			/* Whether the last command shell() or capture() ran failed. */
			[[nodiscard]] auto failed_last_capture(void) const noexcept -> bool {
				return _capture_failed;
			}

			/* Prints every command's output (the end of a script). */
			auto flush_outputs(void) noexcept -> void;

			/* Stops what the interpreter started besides its memory (the launcher threads). */
			[[nodiscard]] auto cleanup(void) noexcept -> ca::result<void>;

			/* Whether a command run by the script failed. */
			[[nodiscard]] auto failed(void) const noexcept -> bool {
				return _failed;
			}

			[[nodiscard]] auto settings(void) noexcept -> fb::run_settings& {
				return _settings;
			}

			[[nodiscard]] auto bench(void) const noexcept -> bool {
				return _bench;
			}

			// -- extension API -------------------------------------------------

			[[nodiscard]] auto memory(void) noexcept -> fb::memory& {
				return _memory;
			}

			/* Registers b; a later registration of the same name replaces it. */
			auto define(const fb::builtin& b) noexcept -> void;

			[[nodiscard]] auto find_builtin(ca::sv::str name) const noexcept -> const fb::builtin*;

			[[nodiscard]] auto builtin_names(void) const noexcept -> ca::borrowed_slice<const ca::sv::str> {
				return _builtin_order.view();
			}

			/* Makes the bare identifier name evaluate to v (a script definition still shadows it). */
			auto bind(ca::sv::str name, fb::value* v) noexcept -> void;

			/* The class called name, created empty on first use. */
			[[nodiscard]] auto class_named(ca::sv::str name) noexcept -> fb::class_info*;

			/* Sets method name of cls to callable (a lambda, or native made by native_value). */
			auto set_method(fb::class_info* cls, ca::sv::str name, fb::value* callable) noexcept -> void;

			auto method(fb::class_info* cls, ca::sv::str name, fb::native fn) noexcept -> void;

			/* An instance of cls with no fields, carrying data for native methods. */
			[[nodiscard]] auto object(fb::class_info* cls, void* data) noexcept -> fb::value*;

			[[nodiscard]] auto unit(void) const noexcept -> fb::value* {
				return _unit;
			}

			[[nodiscard]] auto make(fb::kind k) noexcept -> fb::value*;
			[[nodiscard]] auto integer(ca::i64 v) noexcept -> fb::value*;
			[[nodiscard]] auto floating(ca::f64 v) noexcept -> fb::value*;
			[[nodiscard]] auto boolean(bool v) noexcept -> fb::value*;
			[[nodiscard]] auto string(ca::sv::str v) noexcept -> fb::value*;
			[[nodiscard]] auto file(ca::sv::str v) noexcept -> fb::value*;
			[[nodiscard]] auto native_value(fb::native fn) noexcept -> fb::value*;

			/* v, kept as is: the text must already live in this interpreter's memory. */
			[[nodiscard]] auto string_in_place(ca::sv::str v) noexcept -> fb::value*;
			[[nodiscard]] auto file_in_place(ca::sv::str v) noexcept -> fb::value*;

			auto push(fb::value* sequence, fb::value* element) noexcept -> void;

			/* Sets key of a map or object (insertion order kept, last write wins). */
			auto set(fb::value* mapping, ca::sv::str key, fb::value* v) noexcept -> void;

			/* key of a map or object, or unit. */
			[[nodiscard]] auto get(const fb::value* mapping, ca::sv::str key) const noexcept -> fb::value*;

			/* fn applied to one argument (a tuple of several is several). */
			[[nodiscard]] auto apply(fb::value* fn, fb::value* arg) noexcept -> fb::outcome;

			[[nodiscard]] auto apply(fb::value* fn, fb::arguments args) noexcept -> fb::outcome;

			/* What a lazy builtin's thunk evaluates to (anything else as is). */
			[[nodiscard]] auto force(fb::value* thunk) noexcept -> fb::outcome;

			/* v as text, the way a command argument or interpolation shows it. */
			[[nodiscard]] auto text(const fb::value* v) noexcept -> ca::sv::str;

			/* print's rendering of v; debug's (typed) when typed. */
			[[nodiscard]] auto repr(const fb::value* v, bool typed) noexcept -> ca::sv::str;

			/* Prints line and a newline, after the current run() if one is drawing. */
			auto emit(ca::sv::str line) noexcept -> void;

			/* Runs the commands target carries (run()); whether there were any. */
			[[nodiscard]] auto execute(fb::value* target) noexcept -> ca::result<bool, fb::diagnostic>;

			/* Captures what the commands of target print (shell()), shaped like target. */
			[[nodiscard]] auto capture(fb::value* target) noexcept -> fb::outcome;

			/* The script's mutable stores, `_GLOBALS` and `_SHARED`. */
			[[nodiscard]] auto globals(void) noexcept -> fb::value* {
				return _globals;
			}

			[[nodiscard]] auto shared(void) noexcept -> fb::value* {
				return _shared;
			}

			[[nodiscard]] auto args(void) noexcept -> fb::value* {
				return _args;
			}

			/* Adds what this interpreter knows besides a script's own definitions to a type
			   checker's catalog: every builtin (with its signature), every bound value (typed
			   from the value) and every class registered by C++ code. The error names a builtin
			   whose signature does not parse. */
			[[nodiscard]] auto describe(fb::check::catalog& catalog) const noexcept -> ca::result<void, fb::diagnostic>;

			/* An error stopping the script, message formatted in this interpreter's memory. */
			template <typename... Args>
			[[nodiscard]] auto fail(const ca::format_string<ca::type_identity<Args>...> format, const Args&... args) noexcept -> ca::failure<fb::diagnostic> {
				return ca::failure<fb::diagnostic>{fb::diagnostic{_memory.format(format, args...), {0, 0}}};
			}

	}; // class interpreter

	/* Registers the core builtins (builtins.cpp). */
	auto open_core(fb::interpreter& in) noexcept -> void;

} // namespace fb

#endif // fb_runtime_interpreter_hpp
