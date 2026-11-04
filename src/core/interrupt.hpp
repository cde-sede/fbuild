#ifndef fb_core_interrupt_hpp
#define fb_core_interrupt_hpp

#include "ca/process.hpp"
#include "ca/result.hpp"

namespace fb::interrupt {

	/* Catches SIGINT, SIGTERM and SIGHUP for the whole process: the first one is kept (pending)
	   and wakes descriptor(); nothing else happens in the handler, so any thread may take it.
	   Never block these in a thread that starts children: they inherit its mask. Call once. */
	[[nodiscard]] auto install(void) noexcept -> ca::result<void>;

	/* The first signal caught, or 0. */
	[[nodiscard]] auto pending(void) noexcept -> int;

	/* Readable once a signal is caught (a pipe's read end); -1 before install. */
	[[nodiscard]] auto descriptor(void) noexcept -> int;

} // namespace fb::interrupt

#endif // fb_core_interrupt_hpp
