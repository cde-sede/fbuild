#include "core/interrupt.hpp"

#include "ca/atomic.hpp"
#include "ca/fs.hpp"
#include "ca/io.hpp"
#include "ca/syscall.hpp"

namespace fb::impl {

	static constinit ca::atomic<int> caught_signal{0};
	static constinit ca::atomic<int> wake_write{-1};
	static constinit ca::atomic<int> wake_read{-1};

	/* Async-signal-safe: an atomic exchange and a raw write, errno untouched. */
	static auto on_signal(const int signal) noexcept -> void {

		int expected = 0;
		static_cast<void>(fb::impl::caught_signal.compare_exchange_strong(expected, signal));
		const char byte = 1;
		static_cast<void>(ca::syscall::write(fb::impl::wake_write.load(), &byte, 1));
	}

} // namespace fb::impl

auto fb::interrupt::install(void) noexcept -> ca::result<void> {

	auto ends = ca::sys::pipe(ca::fs::impl::CLOEXEC | ca::fs::impl::NONBLOCK);
	if (!ends)
		return ends.propagate();
	fb::impl::wake_read.store(ends->read_end());
	fb::impl::wake_write.store(ends->write_end());
	for (const int signal : {ca::sys::signals::interrupt, ca::sys::signals::terminate, ca::sys::signals::hangup}) {
		auto installed = ca::sys::sigaction(signal, &fb::impl::on_signal);
		if (!installed)
			return installed;
	}
	return {};
}

auto fb::interrupt::pending(void) noexcept -> int {
	return fb::impl::caught_signal.load();
}

auto fb::interrupt::descriptor(void) noexcept -> int {
	return fb::impl::wake_read.load();
}
