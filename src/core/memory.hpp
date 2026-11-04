#ifndef fb_core_memory_hpp
#define fb_core_memory_hpp

#include "ca/dynamic_array.hpp"
#include "ca/format_append.hpp"
#include "ca/memory.hpp"
#include "ca/debug.hpp"
#include "ca/utility.hpp"
#include "ca/result.hpp"
#include "ca/str.hpp"
#include "ca/sv.hpp"

namespace fb {

	/* A growable sequence whose storage lives in an fb::memory arena. */
	template <typename T>
	using list = ca::dynamic_array<T, ca::arena_allocator>;

	/* Text whose storage lives in an fb::memory arena. */
	using text = ca::basic_str<char, ca::arena_allocator>;

	/* Owner of everything a script's front end and runtime allocate: tokens, nodes, values,
	   strings. Nothing is released one by one; cleanup() releases it all. Running out of memory
	   traps, as the interpreter has no way to carry on without its own storage. */
	class memory final {

		private:

			using self = fb::memory;

			ca::arena _arena;

			explicit memory(ca::arena&& arena) noexcept
			: _arena{ca::move(arena)} {
			}

			[[noreturn, gnu::cold, gnu::noinline]] static auto _exhausted(void) noexcept -> void {

				ca::trap("fbuild: out of memory");
			}

		public:

			memory(void) noexcept = default;

			memory(const self&) = delete;

			memory(self&&) noexcept = default;

			auto operator=(const self&) -> self& = delete;

			auto operator=(self&&) noexcept -> self& = default;

			[[nodiscard]] static auto make(void) noexcept -> ca::result<self> {

				auto arena = ca::arena::make(1U << 20U);
				if (!arena)
					return arena.propagate();
				return self{ca::move(*arena)};
			}

			/* Whether make() built it and cleanup() has not run. */
			[[nodiscard]] auto holds(void) const noexcept -> bool {
				return _arena.holds();
			}

			/* Releases everything allocated so far, keeping the first chunk for what comes next. */
			auto reset(void) noexcept -> void {
				_arena.reset();
			}

			[[nodiscard]] auto allocator(void) const noexcept -> ca::arena_allocator {
				return _arena.allocator();
			}

			[[nodiscard]] auto bytes(const ca::usz size, const ca::usz alignment) noexcept -> void* {

				auto block = _arena.allocator().allocate(size == 0U ? 1U : size, alignment);
				if (!block) [[unlikely]]
					self::_exhausted();
				return block->data();
			}

			template <typename T, typename... Args>
			[[nodiscard]] auto make(Args&&... args) noexcept -> T* {

				void* const at = this->bytes(sizeof(T), alignof(T));
				return ca::construct_at(static_cast<T*>(at), ca::forward<Args>(args)...);
			}

			template <typename T>
			[[nodiscard]] auto list(void) noexcept -> fb::list<T> {
				return fb::list<T>{_arena.allocator()};
			}

			template <typename T, typename U>
			auto push(fb::list<T>& into, U&& value) noexcept -> void {

				if (!into.append(ca::forward<U>(value))) [[unlikely]]
					self::_exhausted();
			}

			/* A NUL-terminated copy of value, in the arena. */
			[[nodiscard]] auto copy(const ca::sv::str value) noexcept -> ca::sv::str {

				char* const at = static_cast<char*>(this->bytes(value.size() + 1U, 1U));
				ca::copy_bytes(at, value.data(), value.size());
				at[value.size()] = '\0';
				return ca::sv::str{at, value.size()};
			}

			/* args formatted with format, NUL-terminated, in the arena. */
			template <typename... Args>
			[[nodiscard]] auto format(const ca::format_string<ca::type_identity<Args>...> format, const Args&... args) noexcept -> ca::sv::str {

				fb::text out{_arena.allocator()};
				if (!ca::format_append(out, format, args...)) [[unlikely]]
					self::_exhausted();
				return this->copy(out.view());
			}

			[[nodiscard]] auto cleanup(void) noexcept -> ca::result<void> {
				return _arena.cleanup();
			}

	}; // class memory

} // namespace fb

#endif // fb_core_memory_hpp
