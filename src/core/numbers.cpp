#include "core/numbers.hpp"

#include "ca/span.hpp"
#include "ca/text.hpp"

namespace fb::impl {

	[[nodiscard]] static constexpr auto is_space(const char c) noexcept -> bool {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
	}

	[[nodiscard]] static constexpr auto is_digit(const char c) noexcept -> bool {
		return c >= '0' && c <= '9';
	}

	inline constexpr ca::f64 exact_powers[] = {
		1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
		1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
	};

	[[nodiscard]] static auto scale(ca::f64 value, ca::i64 exponent) noexcept -> ca::f64 {

		const bool down = exponent < 0;
		ca::u64 rest = static_cast<ca::u64>(down ? -exponent : exponent);
		if (rest > 400U)
			rest = 400U;
		ca::f64 factor = 1.0;
		ca::f64 power = 10.0;
		for (; rest != 0U; rest >>= 1U) {
			if ((rest & 1U) != 0U)
				factor *= power;
			power *= power;
		}
		return down ? value / factor : value * factor;
	}

} // namespace fb::impl

auto fb::parse_integer_prefix(const ca::sv::str text) noexcept -> ca::i64 {

	ca::usz i = 0U;
	while (i < text.size() && fb::impl::is_space(text.data()[i]))
		++i;
	bool negative = false;
	if (i < text.size() && (text.data()[i] == '+' || text.data()[i] == '-'))
		negative = text.data()[i++] == '-';
	ca::u64 magnitude = 0U;
	bool overflow = false;
	for (; i < text.size() && fb::impl::is_digit(text.data()[i]); ++i) {
		const ca::u64 digit = static_cast<ca::u64>(text.data()[i] - '0');
		if (magnitude > (ca::limits<ca::u64>::max() - digit) / 10U)
			overflow = true;
		else
			magnitude = magnitude * 10U + digit;
	}
	const ca::u64 limit = negative ? static_cast<ca::u64>(ca::limits<ca::i64>::max()) + 1U : static_cast<ca::u64>(ca::limits<ca::i64>::max());
	if (overflow || magnitude > limit)
		return negative ? ca::limits<ca::i64>::min() : ca::limits<ca::i64>::max();
	return negative ? static_cast<ca::i64>(0U - magnitude) : static_cast<ca::i64>(magnitude);
}

auto fb::parse_float_prefix(const ca::sv::str text) noexcept -> ca::f64 {

	ca::usz i = 0U;
	while (i < text.size() && fb::impl::is_space(text.data()[i]))
		++i;
	bool negative = false;
	if (i < text.size() && (text.data()[i] == '+' || text.data()[i] == '-'))
		negative = text.data()[i++] == '-';
	ca::u64 digits = 0U;
	int kept = 0;
	ca::i64 exponent = 0;
	bool any = false;
	for (; i < text.size() && fb::impl::is_digit(text.data()[i]); ++i) {
		any = true;
		if (kept < 19) {
			digits = digits * 10U + static_cast<ca::u64>(text.data()[i] - '0');
			if (digits != 0U)
				++kept;
		} else
			++exponent;
	}
	if (i < text.size() && text.data()[i] == '.') {
		++i;
		for (; i < text.size() && fb::impl::is_digit(text.data()[i]); ++i) {
			any = true;
			if (kept < 19) {
				digits = digits * 10U + static_cast<ca::u64>(text.data()[i] - '0');
				if (digits != 0U)
					++kept;
				--exponent;
			}
		}
	}
	if (!any)
		return 0.0;
	if (i < text.size() && (text.data()[i] == 'e' || text.data()[i] == 'E')) {
		ca::usz j = i + 1U;
		bool down = false;
		if (j < text.size() && (text.data()[j] == '+' || text.data()[j] == '-'))
			down = text.data()[j++] == '-';
		if (j < text.size() && fb::impl::is_digit(text.data()[j])) {
			ca::i64 written = 0;
			for (; j < text.size() && fb::impl::is_digit(text.data()[j]); ++j)
				if (written < 100000)
					written = written * 10 + (text.data()[j] - '0');
			exponent += down ? -written : written;
		}
	}
	ca::f64 value;
	if (digits <= (1ULL << 53U) && exponent >= -22 && exponent <= 22) {
		value = static_cast<ca::f64>(digits);
		value = exponent < 0 ? value / fb::impl::exact_powers[-exponent] : value * fb::impl::exact_powers[exponent];
	} else
		value = fb::impl::scale(static_cast<ca::f64>(digits), exponent);
	return negative ? -value : value;
}

auto fb::format_float(const ca::f64 value, char (&buffer)[32]) noexcept -> ca::usz {

	ca::usz size = ca::sys::format_float(ca::span<char>{buffer, 28U}, value, 'g', 15, false);
	if (size > 27U)
		size = 27U;
	for (ca::usz i = 0U; i < size; ++i) {
		const char c = buffer[i];
		if (c == '.' || c == 'e' || c == 'n' || c == 'i')
			return size;
	}
	buffer[size++] = '.';
	buffer[size++] = '0';
	return size;
}

auto fb::format_integer(const ca::i64 value, char (&buffer)[24]) noexcept -> ca::usz {

	char reversed[24];
	ca::usz count = 0U;
	ca::u64 rest = value < 0 ? 0U - static_cast<ca::u64>(value) : static_cast<ca::u64>(value);
	do {
		reversed[count++] = static_cast<char>('0' + rest % 10U);
		rest /= 10U;
	} while (rest != 0U);
	ca::usz size = 0U;
	if (value < 0)
		buffer[size++] = '-';
	while (count != 0U)
		buffer[size++] = reversed[--count];
	return size;
}
