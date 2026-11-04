#ifndef fb_plugins_support_hpp
#define fb_plugins_support_hpp

/* What the standard plugins share: the coercions of the C plugin API they were ported from, so
   a script sees the same results. */

#include "fbuild.hpp"

namespace fb::support {

	/* A value as an Int: a Float truncated, a Bool or Int as is, anything else 0. */
	[[nodiscard]] inline auto integer(const fb::value* const v) noexcept -> ca::i64 {
		return v->is(fb::kind::floating) ? static_cast<ca::i64>(v->floating) : v->integer;
	}

	/* A value as a Float: an Int or Bool converted, anything else 0. */
	[[nodiscard]] inline auto floating(const fb::value* const v) noexcept -> ca::f64 {
		return v->as_float();
	}

	[[nodiscard]] inline auto is_number(const fb::value* const v) noexcept -> bool {
		return v->is(fb::kind::integer) || v->is(fb::kind::floating);
	}

	/* The ASCII classes of <ctype.h> in the C locale. */
	[[nodiscard]] constexpr auto is_space(const char c) noexcept -> bool {
		return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
	}

	[[nodiscard]] constexpr auto is_upper(const char c) noexcept -> bool {
		return c >= 'A' && c <= 'Z';
	}

	[[nodiscard]] constexpr auto is_lower(const char c) noexcept -> bool {
		return c >= 'a' && c <= 'z';
	}

	[[nodiscard]] constexpr auto is_digit(const char c) noexcept -> bool {
		return c >= '0' && c <= '9';
	}

	[[nodiscard]] constexpr auto is_alpha(const char c) noexcept -> bool {
		return is_upper(c) || is_lower(c);
	}

	[[nodiscard]] constexpr auto upper(const char c) noexcept -> char {
		return is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c;
	}

	[[nodiscard]] constexpr auto lower(const char c) noexcept -> char {
		return is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
	}

	/* Room for size chars and a NUL in in's memory. */
	[[nodiscard]] inline auto buffer(fb::interpreter& in, const ca::usz size) noexcept -> char* {

		char* const out = static_cast<char*>(in.memory().bytes(size + 1U, 1U));
		out[size] = '\0';
		return out;
	}

} // namespace fb::support

#endif // fb_plugins_support_hpp
