#ifndef fb_runner_runner_hpp
#define fb_runner_runner_hpp

#include "ca/integers.hpp"
#include "ca/result.hpp"
#include "ca/str.hpp"
#include "ca/sv.hpp"
#include "core/memory.hpp"

namespace fb {

	struct run_settings;
	class launcher;

	/* One program to run: argv (null-terminated, in memory) and the text the display shows. */
	struct command final {
		ca::sv::str display;
		char* const* argv;
	};

	/* Commands run one after the other, as one entry of a parallel group. */
	struct task final {
		ca::sv::str label;
		fb::list<fb::command> commands;
	};

	/* What a captured command printed (stdout and stderr), and how it ended. */
	struct captured final {
		ca::sv::str output;
		int code;
	};

	/* Runs commands with the build display: nested groups (sequential) and parallel groups,
	   a live tree on a terminal, summary lines otherwise, every command's output kept for
	   flush_outputs(). Single-threaded: the thread that evaluates the script drives it. */
	class runner final {

		public:

			/* The spinner text for a running command's frame (config's spinner lambda). */
			using spinner_text = auto (*)(void* owner, ca::u64 frame) noexcept -> ca::sv::str;

		private:

			struct entry;
			struct context;
			struct slot;
			struct node;
			struct tally;

			fb::memory& _memory;
			const fb::run_settings& _settings;
			context* _current;
			context* _root;
			fb::list<entry*> _records;
			ca::usz _drawn;
			ca::usz _width;
			ca::usz _height;
			ca::usz _pass;
			bool _fold;
			bool _tty;
			bool _decided;
			spinner_text _spinner_text;
			void* _spinner_owner;
			fb::launcher* _launcher;
			fb::memory _scratch;
			ca::str _frame;

			auto _fresh_scratch(void) noexcept -> void;

			[[nodiscard]] auto _launchers(void) noexcept -> ca::result<fb::launcher*>;
			[[nodiscard]] auto _begin(char* const* argv, ca::usz index) noexcept -> ca::result<slot>;

			[[nodiscard]] auto _live(void) const noexcept -> bool;
			[[nodiscard]] auto _entry(ca::sv::str display, ca::usz task) noexcept -> entry*;
			[[nodiscard]] auto _run(fb::list<entry*>& entries, fb::list<const fb::command*>& commands, fb::list<ca::usz>& task_of, ca::usz tasks) noexcept -> bool;
			auto _flatten(context* c, fb::list<node>& kids) noexcept -> void;
			auto _children(const node& n, fb::list<node>& kids) noexcept -> void;
			auto _count(const context* c, ca::usz task, tally& t) const noexcept -> void;
			[[nodiscard]] auto _duration(ca::i64 ms) noexcept -> ca::sv::str;
			[[nodiscard]] auto _summary(const tally& t, bool color) noexcept -> ca::sv::str;
			[[nodiscard]] auto _path(const context* from) noexcept -> ca::sv::str;
			auto _render(const node& n, ca::sv::str prefix, bool last, bool root, fb::list<ca::sv::str>& lines) noexcept -> void;
			auto _folded(const fb::list<node>& kids, ca::sv::str below, fb::list<ca::sv::str>& lines) noexcept -> void;
			auto _command(entry* e, const context* owner, ca::sv::str branch, ca::sv::str below, ca::sv::str label, fb::list<ca::sv::str>& lines) noexcept -> void;
			auto _tail(fb::list<ca::sv::str>& lines, ca::sv::str below, const entry* e) noexcept -> void;
			auto _emit(fb::list<ca::sv::str>& lines, ca::sv::str left, ca::sv::str right) noexcept -> void;
			auto _finished(const entry* e) noexcept -> void;
			auto _draw(void) noexcept -> void;
			auto _print(ca::sv::str text) noexcept -> void;

		public:

			runner(fb::memory& memory, const fb::run_settings& settings) noexcept;

			/* Where a configured spinner's text comes from; null for the built-in one. */
			auto set_spinner(spinner_text text, void* owner) noexcept -> void {

				_spinner_text = text;
				_spinner_owner = owner;
			}

			/* Whether the display is a live terminal one (colors, redraws). */
			[[nodiscard]] auto terminal(void) const noexcept -> bool {
				return _tty;
			}

			/* Opens a group (sequential) or parallel context under the current one. An empty
			   label names nothing: a nested sequential context without one shows its commands
			   as its parent's. */
			auto enter(ca::sv::str label, bool parallel) noexcept -> void;

			/* Closes the current context; whether everything in it succeeded. */
			auto leave(void) noexcept -> bool;

			/* Runs commands in order in the current group; stops at the first failure unless
			   keep_going. Whether all succeeded. */
			[[nodiscard]] auto sequence(const fb::list<fb::command>& commands) noexcept -> bool;

			/* Runs tasks at once in the current parallel context, at most jobs commands at a
			   time. Whether all succeeded. */
			[[nodiscard]] auto parallel(const fb::list<fb::task>& tasks) noexcept -> bool;

			/* Runs command with no display, its output returned instead of kept. */
			[[nodiscard]] auto capture(const fb::command& command) noexcept -> fb::captured;

			/* Stops the launcher threads, if any started. */
			[[nodiscard]] auto cleanup(void) noexcept -> ca::result<void>;

			/* Prints what every command printed under a header naming it, the failed ones last. */
			auto flush_outputs(void) noexcept -> void;

	}; // class runner

} // namespace fb

#endif // fb_runner_runner_hpp
