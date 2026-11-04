#ifndef fb_runner_launcher_hpp
#define fb_runner_launcher_hpp

#include "ca/channel.hpp"
#include "ca/descriptor.hpp"
#include "ca/error.hpp"
#include "ca/optional.hpp"
#include "ca/thread.hpp"
#include "core/memory.hpp"

namespace fb {

	/* One program to start: argv, and the write end of the pipe its stdout and stderr go to.
	   pid and error are filled in by the launcher thread that starts it. */
	struct launch final {
		char* const* argv;
		ca::fs::descriptor output;
		ca::i32 pid;
		ca::error error;
	};

	/* Threads that start programs, so the thread driving the build never waits for a vfork child
	   to reach execve: submit hands a launch over, wake() becomes readable when one is done, and
	   collect() returns it. Stays put once started (the threads hold its address). */
	class launcher final {

		private:

			using self = fb::launcher;

			ca::channel<fb::launch*> _requests;
			ca::channel<fb::launch*> _results;
			ca::fs::descriptor _wake_read;
			ca::fs::descriptor _wake_write;
			fb::list<ca::sys::thread> _threads;

			static auto _work(void* argument) noexcept -> void;

		public:

			launcher(ca::channel<fb::launch*>&& requests, ca::channel<fb::launch*>&& results, ca::fs::descriptor&& wake_read,
				ca::fs::descriptor&& wake_write, fb::list<ca::sys::thread>&& threads) noexcept
			: _requests{ca::move(requests)}, _results{ca::move(results)}, _wake_read{ca::move(wake_read)},
			  _wake_write{ca::move(wake_write)}, _threads{ca::move(threads)} {
			}

			/* Launches in flight at most: submit fails with errc::no_space beyond it. */
			static constexpr ca::usz CAPACITY = 256U;

			launcher(const self&) = delete;

			auto operator=(const self&) -> self& = delete;

			/* A launcher in memory with count threads (at least one started). */
			[[nodiscard]] static auto make(fb::memory& memory, ca::usz count) noexcept -> ca::result<self*>;

			/* Queues l without waiting; errc::no_space when CAPACITY launches are in flight. */
			[[nodiscard]] auto submit(fb::launch* l) noexcept -> ca::result<void>;

			/* Readable when a launch is done (poll it, then collect). */
			[[nodiscard]] auto wake(void) const noexcept -> int {
				return _wake_read.native();
			}

			/* A finished launch, or nothing; consumes wake-ups as it goes. */
			[[nodiscard]] auto collect(void) noexcept -> ca::optional<fb::launch*>;

			/* Stops the threads (after the launches queued), joins them, closes everything. */
			[[nodiscard]] auto cleanup(void) noexcept -> ca::result<void>;

	}; // class launcher

} // namespace fb

#endif // fb_runner_launcher_hpp
