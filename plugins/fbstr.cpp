// fbstr: upper, lower, capitalize, title, trim, lstrip, rstrip, starts_with, ends_with,
// strip_prefix, strip_suffix, find, rfind, occurrences, repeat, chars, words, partition, substr,
// replace, pad_left, pad_right, center, is_alpha, is_digit, is_alnum, is_space, is_upper,
// is_lower, ord, chr. Ported from cparser's plugins/fbstr.c: byte-oriented, ASCII classes.
#include "support.hpp"

namespace {

	namespace s = fb::support;

	/* Bounds repeat and the pads like the C plugin: past it, an error instead of an allocation. */
	constexpr ca::usz max_result = ca::usz{1} << 30U;

	auto text(fb::interpreter& in, const fb::arguments a, const ca::usz i) noexcept -> ca::sv::str {
		return in.text(a[i]);
	}

	template <typename F>
	auto mapped(fb::interpreter& in, const ca::sv::str from, F&& map) noexcept -> fb::value* {

		char* const out = s::buffer(in, from.size());
		for (ca::usz i = 0U; i < from.size(); ++i)
			out[i] = map(i, from.data()[i]);
		return in.string_in_place(ca::sv::str{out, from.size()});
	}

	auto stripped_start(ca::sv::str t) noexcept -> ca::sv::str {

		while (!t.empty() && s::is_space(t.data()[0]))
			t = t.without_prefix(1U);
		return t;
	}

	auto stripped_end(ca::sv::str t) noexcept -> ca::sv::str {

		while (!t.empty() && s::is_space(t.data()[t.size() - 1U]))
			t = t.without_suffix(1U);
		return t;
	}

	auto upper(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return mapped(in, text(in, a, 0U), [](ca::usz, const char c) noexcept { return s::upper(c); });
	}

	auto lower(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return mapped(in, text(in, a, 0U), [](ca::usz, const char c) noexcept { return s::lower(c); });
	}

	auto capitalize(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return mapped(in, text(in, a, 0U), [](const ca::usz i, const char c) noexcept { return i == 0U ? s::upper(c) : s::lower(c); });
	}

	auto title(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		bool start = true;
		return mapped(in, text(in, a, 0U), [&start](ca::usz, const char c) noexcept {
			if (s::is_space(c)) {
				start = true;
				return c;
			}
			const char r = start ? s::upper(c) : s::lower(c);
			start = false;
			return r;
		});
	}

	auto trim(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string(stripped_end(stripped_start(text(in, a, 0U))));
	}

	auto lstrip(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string(stripped_start(text(in, a, 0U)));
	}

	auto rstrip(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string(stripped_end(text(in, a, 0U)));
	}

	auto starts_with(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.boolean(text(in, a, 0U).starts_with(text(in, a, 1U)));
	}

	auto ends_with(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.boolean(text(in, a, 0U).ends_with(text(in, a, 1U)));
	}

	auto strip_prefix(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string(text(in, a, 0U).strip_prefix(text(in, a, 1U)));
	}

	auto strip_suffix(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.string(text(in, a, 0U).strip_suffix(text(in, a, 1U)));
	}

	auto find(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str t = text(in, a, 0U);
		const ca::i64 size = static_cast<ca::i64>(t.size());
		ca::i64 from = a.size() >= 3U ? s::integer(a[2U]) : 0;
		if (from < 0)
			from += size;
		if (from < 0)
			from = 0;
		if (from > size)
			return in.integer(-1);
		auto at = t.without_prefix(static_cast<ca::usz>(from)).find(text(in, a, 1U));
		return in.integer(at ? from + static_cast<ca::i64>(*at) : -1);
	}

	auto rfind(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str t = text(in, a, 0U);
		const ca::sv::str wanted = text(in, a, 1U);
		if (wanted.empty())
			return in.integer(static_cast<ca::i64>(t.size()));
		auto at = t.find_last(wanted);
		return in.integer(at ? static_cast<ca::i64>(*at) : -1);
	}

	auto occurrences(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str wanted = text(in, a, 1U);
		return in.integer(wanted.empty() ? 0 : static_cast<ca::i64>(text(in, a, 0U).count(wanted)));
	}

	auto repeat(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str t = text(in, a, 0U);
		ca::i64 times = s::integer(a[1U]);
		if (times < 0)
			times = 0;
		ca::usz total = 0U;
		if (__builtin_mul_overflow(t.size(), static_cast<ca::usz>(times), &total) || total > max_result)
			return in.fail("repeat: result too large ({} * {})", t, times);
		char* const out = s::buffer(in, total);
		for (ca::usz i = 0U; i < total; ++i)
			out[i] = t.data()[i % t.size()];
		return in.string_in_place(ca::sv::str{out, total});
	}

	auto chars(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		const ca::sv::str t = text(in, a, 0U);
		for (ca::usz i = 0U; i < t.size(); ++i)
			in.push(out, in.string(t.slice(i, 1U)));
		return out;
	}

	auto words(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		const ca::sv::str t = text(in, a, 0U);
		ca::usz i = 0U;
		while (i < t.size()) {
			while (i < t.size() && s::is_space(t.data()[i]))
				++i;
			const ca::usz start = i;
			while (i < t.size() && !s::is_space(t.data()[i]))
				++i;
			if (i > start)
				in.push(out, in.string(t.slice(start, i - start)));
		}
		return out;
	}

	auto partition(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str t = text(in, a, 0U);
		const ca::sv::str separator = text(in, a, 1U);
		fb::value* const out = in.make(fb::kind::tuple);
		auto at = separator.empty() ? ca::optional<ca::usz>{} : t.find(separator);
		if (!at) {
			in.push(out, in.string(t));
			in.push(out, in.string(""));
			in.push(out, in.string(""));
			return out;
		}
		in.push(out, in.string(t.prefix(*at)));
		in.push(out, in.string(separator));
		in.push(out, in.string(t.without_prefix(*at + separator.size())));
		return out;
	}

	auto substr(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str t = text(in, a, 0U);
		const ca::i64 size = static_cast<ca::i64>(t.size());
		ca::i64 start = s::integer(a[1U]);
		ca::i64 end = a.size() >= 3U ? s::integer(a[2U]) : size;
		if (start < 0)
			start += size;
		if (end < 0)
			end += size;
		start = start < 0 ? 0 : start > size ? size : start;
		end = end > size ? size : end < start ? start : end;
		return in.string(t.slice(static_cast<ca::usz>(start), static_cast<ca::usz>(end - start)));
	}

	auto replace(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str t = text(in, a, 0U);
		const ca::sv::str from = text(in, a, 1U);
		const ca::sv::str to = text(in, a, 2U);
		if (from.empty())
			return in.string(t);
		fb::text out{in.memory().allocator()};
		ca::sv::str rest = t;
		for (auto at = rest.find(from); at; at = rest.find(from)) {
			static_cast<void>(out.append(rest.prefix(*at)));
			static_cast<void>(out.append(to));
			rest = rest.without_prefix(*at + from.size());
		}
		static_cast<void>(out.append(rest));
		return in.string(out.view());
	}

	/* side: -1 pads on the left, 1 on the right, 0 both (center). */
	auto pad(fb::interpreter& in, const fb::arguments a, const int side) noexcept -> fb::outcome {

		const ca::sv::str t = text(in, a, 0U);
		const ca::i64 width = s::integer(a[1U]);
		char fill = ' ';
		if (a.size() >= 3U && !text(in, a, 2U).empty())
			fill = text(in, a, 2U).data()[0];
		if (width < 0 || static_cast<ca::usz>(width) <= t.size())
			return in.string(t);
		if (static_cast<ca::usz>(width) > max_result)
			return in.fail("pad: width too large ({})", width);
		const ca::usz total = static_cast<ca::usz>(width);
		const ca::usz padding = total - t.size();
		const ca::usz left = side < 0 ? padding : side > 0 ? 0U : padding / 2U;
		char* const out = s::buffer(in, total);
		for (ca::usz i = 0U; i < total; ++i)
			out[i] = i >= left && i - left < t.size() ? t.data()[i - left] : fill;
		return in.string_in_place(ca::sv::str{out, total});
	}

	auto pad_left(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return pad(in, a, -1);
	}

	auto pad_right(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return pad(in, a, 1);
	}

	auto center(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return pad(in, a, 0);
	}

	template <auto (*predicate)(char) noexcept -> bool>
	auto all_of(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str t = text(in, a, 0U);
		if (t.empty())
			return in.boolean(false);
		for (const char c : t)
			if (!predicate(c))
				return in.boolean(false);
		return in.boolean(true);
	}

	constexpr auto alnum(const char c) noexcept -> bool {
		return s::is_alpha(c) || s::is_digit(c);
	}

	/* Has a letter of case wanted and none of the other. */
	template <bool upper_wanted>
	auto cased(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		bool seen = false;
		for (const char c : text(in, a, 0U)) {
			if (upper_wanted ? s::is_lower(c) : s::is_upper(c))
				return in.boolean(false);
			if (upper_wanted ? s::is_upper(c) : s::is_lower(c))
				seen = true;
		}
		return in.boolean(seen);
	}

	auto ord(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::sv::str t = text(in, a, 0U);
		if (t.empty())
			return in.fail("ord: empty string");
		return in.integer(static_cast<ca::u8>(t.data()[0]));
	}

	auto chr(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::i64 v = s::integer(a[0U]);
		if (v < 0 || v > 255)
			return in.fail("chr: {} out of byte range", v);
		char* const out = s::buffer(in, 1U);
		out[0] = static_cast<char>(v);
		return in.string_in_place(ca::sv::str{out, 1U});
	}

} // namespace

auto fb::plugin::api(void) noexcept -> ca::u32 {
	return fb::plugin_api;
}

auto fb::plugin::open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {

	using fb::ret;
	const fb::builtin builtins[] = {
		{"upper", upper, 1, 1, ret::string, 0U, "upper(text)", "Path -> String"},
		{"lower", lower, 1, 1, ret::string, 0U, "lower(text)", "Path -> String"},
		{"capitalize", capitalize, 1, 1, ret::string, 0U, "capitalize(text): first upper, rest lower", "Path -> String"},
		{"title", title, 1, 1, ret::string, 0U, "title(text): capitalize each word", "Path -> String"},
		{"trim", trim, 1, 1, ret::string, 0U, "trim(text): strip both ends", "Path -> String"},
		{"lstrip", lstrip, 1, 1, ret::string, 0U, "lstrip(text): strip leading whitespace", "Path -> String"},
		{"rstrip", rstrip, 1, 1, ret::string, 0U, "rstrip(text): strip trailing whitespace", "Path -> String"},
		{"starts_with", starts_with, 2, 2, ret::boolean, 0U, "starts_with(text, prefix)", "Path, String -> Bool"},
		{"ends_with", ends_with, 2, 2, ret::boolean, 0U, "ends_with(text, suffix)", "Path, String -> Bool"},
		{"strip_prefix", strip_prefix, 2, 2, ret::string, 0U, "strip_prefix(text, prefix): drop it if present", "Path, String -> String"},
		{"strip_suffix", strip_suffix, 2, 2, ret::string, 0U, "strip_suffix(text, suffix): drop it if present", "Path, String -> String"},
		{"find", find, 2, 3, ret::integer, 0U, "find(text, sub, from?): index, or -1", "Path, String, Int? -> Int"},
		{"rfind", rfind, 2, 2, ret::integer, 0U, "rfind(text, sub): last index, or -1", "Path, String -> Int"},
		{"occurrences", occurrences, 2, 2, ret::integer, 0U, "occurrences(text, sub): non-overlapping matches", "Path, String -> Int"},
		{"repeat", repeat, 2, 2, ret::string, 0U, "repeat(text, n)", "String, Int -> String"},
		{"chars", chars, 1, 1, ret::list, 0U, "chars(text): one string per byte", "Path -> List<String>"},
		{"words", words, 1, 1, ret::list, 0U, "words(text): split on whitespace runs", "Path -> List<String>"},
		{"partition", partition, 2, 2, ret::any, 0U, "partition(text, sep): (before, sep, after)", "Path, String -> Tuple<String, String, String>"},
		{"substr", substr, 2, 3, ret::string, 0U, "substr(text, start, end?): [start, end), negative from the end", "Path, Int, Int? -> String"},
		{"replace", replace, 3, 3, ret::string, 0U, "replace(text, from, to): every occurrence", "Path, String, String -> String"},
		{"pad_left", pad_left, 2, 3, ret::string, 0U, "pad_left(text, width, fill?): right-justify", "Path, Int, String? -> String"},
		{"pad_right", pad_right, 2, 3, ret::string, 0U, "pad_right(text, width, fill?): left-justify", "Path, Int, String? -> String"},
		{"center", center, 2, 3, ret::string, 0U, "center(text, width, fill?)", "Path, Int, String? -> String"},
		{"is_alpha", all_of<s::is_alpha>, 1, 1, ret::boolean, 0U, "is_alpha(text)", "Path -> Bool"},
		{"is_digit", all_of<s::is_digit>, 1, 1, ret::boolean, 0U, "is_digit(text)", "Path -> Bool"},
		{"is_alnum", all_of<alnum>, 1, 1, ret::boolean, 0U, "is_alnum(text)", "Path -> Bool"},
		{"is_space", all_of<s::is_space>, 1, 1, ret::boolean, 0U, "is_space(text)", "Path -> Bool"},
		{"is_upper", cased<true>, 1, 1, ret::boolean, 0U, "is_upper(text): a cased letter, none lowercase", "Path -> Bool"},
		{"is_lower", cased<false>, 1, 1, ret::boolean, 0U, "is_lower(text): a cased letter, none uppercase", "Path -> Bool"},
		{"ord", ord, 1, 1, ret::integer, 0U, "ord(text): byte value of the first char", "String -> Int"},
		{"chr", chr, 1, 1, ret::string, 0U, "chr(n): one-byte string, n in 0..255", "Int -> String"},
	};
	for (const fb::builtin& b : builtins)
		in.define(b);
	return {};
}
