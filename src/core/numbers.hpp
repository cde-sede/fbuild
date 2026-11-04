#ifndef fb_core_numbers_hpp
#define fb_core_numbers_hpp

#include "ca/floats.hpp"
#include "ca/integers.hpp"
#include "ca/limits.hpp"
#include "ca/sv.hpp"

namespace fb {

	/* The integer at the start of text, as strtol(text, 0, 10) reads it: leading blanks, a sign,
	   digits up to the first non-digit; 0 when there are none; clamped on overflow. */
	[[nodiscard]] auto parse_integer_prefix(ca::sv::str text) noexcept -> ca::i64;

	/* The decimal number at the start of text, as strtod reads one (no hex, no inf/nan): exact
	   when the digits and exponent fit an f64 multiplication, otherwise within a few ulps. */
	[[nodiscard]] auto parse_float_prefix(ca::sv::str text) noexcept -> ca::f64;

	/* value as %.15g, with ".0" appended when that shows no '.', exponent, inf or nan: a Float
	   always reads as one. Writes into buffer (32 chars) and returns the length. */
	[[nodiscard]] auto format_float(ca::f64 value, char (&buffer)[32]) noexcept -> ca::usz;

	/* value in decimal, into buffer (24 chars); returns the length. */
	[[nodiscard]] auto format_integer(ca::i64 value, char (&buffer)[24]) noexcept -> ca::usz;

} // namespace fb

#endif // fb_core_numbers_hpp
