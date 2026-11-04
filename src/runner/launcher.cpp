#include "runner/launcher.hpp"


#include "ca/environment.hpp"
#include "ca/fs.hpp"
#include "ca/io.hpp"
#include "ca/process.hpp"
#include "ca/span.hpp"

namespace fb::impl {

	/* Starts l: a spawn whose stdout and stderr are l's pipe; the outcome lands in l. */
	static auto spawn(fb::launch& l) noexcept -> void {

		auto actions = ca::sys::spawn_actions::make();
		if (!actions) {
			l.error = actions.error();
			return;
		}
		const int output = l.output.native();
		auto redirected = actions->add_dup2(output, 1);
		if (redirected)
			redirected = actions->add_dup2(output, 2);
		if (!redirected) {
			l.error = redirected.error();
			static_cast<void>(actions->cleanup());
			return;
		}
		auto pid = ca::sys::posix_spawnp(l.argv[0], l.argv, ca::sys::environ(), &*actions);
		static_cast<void>(actions->cleanup());
		if (pid)
			l.pid = *pid;
		else
			l.error = pid.error();
	}

} // namespace fb::impl

auto fb::launcher::_work(void* const argument) noexcept -> void {

	self& launcher = *static_cast<self*>(argument);
	for (;;) {
		auto next = launcher._requests.pop();
		if (!next)
			return;
		fb::impl::spawn(**next);
		static_cast<void>(launcher._results.push(*next));
		const ca::byte signal{1};
		static_cast<void>(launcher._wake_write.write(ca::span<const ca::byte>{&signal, 1U}));
	}
}

auto fb::launcher::make(fb::memory& memory, const ca::usz count) noexcept -> ca::result<self*> {

	auto requests = ca::channel<fb::launch*>::make(self::CAPACITY);
	if (!requests)
		return requests.propagate();
	auto results = ca::channel<fb::launch*>::make(self::CAPACITY * 2U);
	if (!results) {
		static_cast<void>(requests->cleanup());
		return results.propagate();
	}
	auto ends = ca::sys::pipe(ca::fs::impl::CLOEXEC | ca::fs::impl::NONBLOCK);
	if (!ends) {
		static_cast<void>(requests->cleanup());
		static_cast<void>(results->cleanup());
		return ends.propagate();
	}
	self* const l = memory.make<self>(ca::move(*requests), ca::move(*results), ca::fs::descriptor::adopt(ends->read_end()),
		ca::fs::descriptor::adopt(ends->write_end()), memory.list<ca::sys::thread>());
	for (ca::usz i = 0U; i < count; ++i) {
		auto thread = ca::sys::thread::make(&self::_work, l, 256U * 1024U);
		if (!thread) {
			if (l->_threads.empty()) {
				static_cast<void>(l->cleanup());
				return thread.propagate();
			}
			break;
		}
		memory.push(l->_threads, ca::move(*thread));
	}
	return l;
}

auto fb::launcher::submit(fb::launch* const l) noexcept -> ca::result<void> {
	return _requests.try_push(l);
}

auto fb::launcher::collect(void) noexcept -> ca::optional<fb::launch*> {

	ca::byte drained[64];
	static_cast<void>(_wake_read.read(ca::span<ca::byte>{drained, sizeof drained}));
	return _results.try_pop();
}

auto fb::launcher::cleanup(void) noexcept -> ca::result<void> {

	ca::result<void> outcome{};
	_requests.close();
	for (ca::sys::thread& t : _threads) {
		auto joined = t.cleanup();
		if (!joined && outcome)
			outcome = joined;
	}
	static_cast<void>(_threads.clear());
	for (auto* end : {&_wake_read, &_wake_write}) {
		auto closed = end->cleanup();
		if (!closed && outcome)
			outcome = closed;
	}
	static_cast<void>(_requests.cleanup());
	static_cast<void>(_results.cleanup());
	return outcome;
}
