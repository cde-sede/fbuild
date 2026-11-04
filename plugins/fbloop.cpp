// fbloop: iter, while, range, enumerate, slice, take, drop, reverse, flatten, sort, unique,
// count, sum, min, max, fold, index. Ported from cparser's plugins/fbloop.c.
#include "ca/hashmap.hpp"
#include "support.hpp"

namespace {

	namespace s = fb::support;

	/* Bounds `while` like the C plugin: a runaway loop stops after this many rounds. */
	constexpr ca::i64 while_cap = 10000000;

	auto payload(fb::interpreter& in, const fb::arguments a, const ca::i64 index) noexcept -> fb::value* {

		if (a.size() == 1U)
			return in.integer(index);
		if (a.size() == 2U)
			return a[1U];
		fb::value* const t = in.make(fb::kind::tuple);
		for (ca::usz i = 1U; i < a.size(); ++i)
			in.push(t, a[i]);
		return t;
	}

	/* [start, end) of a sequence of size, Python-style negative ends, clamped. */
	auto clamp(ca::i64 index, const ca::i64 size) noexcept -> ca::i64 {

		if (index < 0)
			index += size;
		if (index < 0)
			return 0;
		return index > size ? size : index;
	}

	auto sublist(fb::interpreter& in, const fb::value* const v, const ca::i64 start, const ca::i64 end) noexcept -> fb::value* {

		fb::value* const out = in.make(fb::kind::list);
		for (ca::i64 i = start; i < end; ++i)
			in.push(out, v->at(static_cast<ca::usz>(i)));
		return out;
	}

	auto iter(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::i64 count = s::integer(a[0U]);
		fb::value* const out = in.make(fb::kind::list);
		for (ca::i64 i = 0; i < count; ++i)
			in.push(out, payload(in, a, i));
		return out;
	}

	auto while_(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		auto body = in.force(a[1U]);
		if (!body)
			return body;
		ca::i64 round = 0;
		for (; round < while_cap; ++round) {
			auto condition = in.force(a[0U]);
			if (!condition)
				return condition;
			if (!(*condition)->truthy())
				break;
			auto ran = in.apply(*body, in.integer(round));
			if (!ran)
				return ran;
		}
		return in.integer(round);
	}

	auto range(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const ca::i64 start = a.size() == 1U ? 0 : s::integer(a[0U]);
		const ca::i64 stop = s::integer(a[a.size() == 1U ? 0U : 1U]);
		fb::value* const out = in.make(fb::kind::list);
		for (ca::i64 i = start; i < stop; ++i)
			in.push(out, in.integer(i));
		return out;
	}

	auto enumerate(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (!a[0U]->is_sequence())
			return out;
		for (ca::usz i = 0U; i < a[0U]->size(); ++i) {
			fb::value* const pair = in.make(fb::kind::tuple);
			in.push(pair, in.integer(static_cast<ca::i64>(i)));
			in.push(pair, a[0U]->at(i));
			in.push(out, pair);
		}
		return out;
	}

	auto slice(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (!a[0U]->is_sequence())
			return in.make(fb::kind::list);
		const ca::i64 size = static_cast<ca::i64>(a[0U]->size());
		return sublist(in, a[0U], clamp(s::integer(a[1U]), size), clamp(s::integer(a[2U]), size));
	}

	auto take(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (!a[0U]->is_sequence())
			return in.make(fb::kind::list);
		const ca::i64 size = static_cast<ca::i64>(a[0U]->size());
		return sublist(in, a[0U], 0, clamp(s::integer(a[1U]), size));
	}

	auto drop(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (!a[0U]->is_sequence())
			return in.make(fb::kind::list);
		const ca::i64 size = static_cast<ca::i64>(a[0U]->size());
		return sublist(in, a[0U], clamp(s::integer(a[1U]), size), size);
	}

	auto reverse(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (a[0U]->is_sequence())
			for (ca::usz i = a[0U]->size(); i != 0U; --i)
				in.push(out, a[0U]->at(i - 1U));
		return out;
	}

	auto flatten(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (!a[0U]->is_sequence())
			return out;
		for (fb::value* const e : a[0U]->items) {
			if (e->is_sequence())
				for (fb::value* const inner : e->items)
					in.push(out, inner);
			else
				in.push(out, e);
		}
		return out;
	}

	/* Sorted by text, stable (a merge sort; the C plugin used qsort, unstable). */
	auto sort(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (!a[0U]->is_sequence())
			return out;
		const ca::usz n = a[0U]->size();
		fb::list<ca::sv::str> keys = in.memory().list<ca::sv::str>();
		fb::list<ca::usz> order = in.memory().list<ca::usz>();
		fb::list<ca::usz> spare = in.memory().list<ca::usz>();
		for (ca::usz i = 0U; i < n; ++i) {
			in.memory().push(keys, in.text(a[0U]->at(i)));
			in.memory().push(order, i);
			in.memory().push(spare, i);
		}
		ca::usz* from = order.data();
		ca::usz* to = spare.data();
		for (ca::usz width = 1U; width < n; width *= 2U) {
			for (ca::usz lo = 0U; lo < n; lo += 2U * width) {
				const ca::usz mid = lo + width < n ? lo + width : n;
				const ca::usz hi = lo + 2U * width < n ? lo + 2U * width : n;
				ca::usz i = lo, j = mid, k = lo;
				while (i < mid && j < hi)
					to[k++] = keys.data()[from[j]].compare(keys.data()[from[i]]) < 0 ? from[j++] : from[i++];
				while (i < mid)
					to[k++] = from[i++];
				while (j < hi)
					to[k++] = from[j++];
			}
			ca::usz* const swapped = from;
			from = to;
			to = swapped;
		}
		for (ca::usz i = 0U; i < n; ++i)
			in.push(out, a[0U]->at(from[i]));
		return out;
	}

	auto unique(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (!a[0U]->is_sequence())
			return out;
		ca::hashmap<ca::sv::str, bool, ca::arena_allocator> seen{in.memory().allocator()};
		for (fb::value* const e : a[0U]->items) {
			const ca::sv::str key = in.text(e);
			if (seen.contains(key))
				continue;
			if (!seen.set(key, true)) [[unlikely]]
				return in.fail("unique: out of memory");
			in.push(out, e);
		}
		return out;
	}

	auto count(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		ca::i64 c = 0;
		if (a[0U]->is_sequence()) {
			const ca::sv::str wanted = in.text(a[1U]);
			for (const fb::value* const e : a[0U]->items)
				if (in.text(e) == wanted)
					++c;
		}
		return in.integer(c);
	}

	/* sum/min/max take one sequence or several values. */
	auto aggregate(fb::interpreter& in, const fb::arguments a, const int which) noexcept -> fb::outcome {

		const bool one = a.size() == 1U && a[0U]->is_sequence();
		const ca::usz n = one ? a[0U]->size() : a.size();
		auto at = [&](const ca::usz i) noexcept -> ca::i64 {
			return s::integer(one ? a[0U]->at(i) : a[i]);
		};
		if (which == 0) {
			ca::i64 total = 0;
			for (ca::usz i = 0U; i < n; ++i)
				total += at(i);
			return in.integer(total);
		}
		if (n == 0U)
			return in.integer(0);
		ca::i64 best = at(0U);
		for (ca::usz i = 1U; i < n; ++i)
			if (which < 0 ? at(i) < best : at(i) > best)
				best = at(i);
		return in.integer(best);
	}

	auto sum(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return aggregate(in, a, 0);
	}

	auto min(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return aggregate(in, a, -1);
	}

	auto max(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return aggregate(in, a, 1);
	}

	auto fold(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* acc = a[1U];
		if (a[0U]->is_sequence())
			for (fb::value* const e : a[0U]->items) {
				fb::value* pair[2] = {acc, e};
				auto next = in.apply(a[2U], fb::arguments{pair, 2U});
				if (!next)
					return next;
				acc = *next;
			}
		return acc;
	}

	auto index(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (!a[0U]->is_sequence())
			return in.integer(-1);
		const ca::sv::str wanted = in.text(a[1U]);
		for (ca::usz i = 0U; i < a[0U]->size(); ++i) {
			fb::value* e = a[0U]->at(i);
			if (a.size() >= 3U) {
				auto keyed = in.apply(a[2U], e);
				if (!keyed)
					return keyed;
				e = *keyed;
			}
			if (in.text(e) == wanted)
				return in.integer(static_cast<ca::i64>(i));
		}
		return in.integer(-1);
	}

} // namespace

auto fb::plugin::api(void) noexcept -> ca::u32 {
	return fb::plugin_api;
}

auto fb::plugin::open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {

	using fb::ret;
	const fb::builtin builtins[] = {
		{"iter", iter, 1, -1, ret::list, 0U, "iter(n, values...): n items: the index, the value, or a tuple of them", "Int, ...? -> List"},
		{"while", while_, 2, 2, ret::integer, fb::lazy, "while(cond, body): apply body to the round while cond holds", "?, ? -> Int"},
		{"range", range, 1, 2, ret::list, 0U, "range(n) = [0..n-1], range(a, b) = [a..b-1]", "Int, Int? -> List<Int>"},
		{"enumerate", enumerate, 1, 1, ret::list, 0U, "enumerate(list): (index, item) tuples", "List<a> -> List<Tuple<Int, a>>"},
		{"slice", slice, 3, 3, ret::list, 0U, "slice(list, start, end): [start, end), negative from the end", "List<a>, Int, Int -> List<a>"},
		{"take", take, 2, 2, ret::list, 0U, "take(list, n): the first n (negative: all but the last -n)", "List<a>, Int -> List<a>"},
		{"drop", drop, 2, 2, ret::list, 0U, "drop(list, n): without the first n (negative: the last -n)", "List<a>, Int -> List<a>"},
		{"reverse", reverse, 1, 1, ret::list, 0U, "reverse(list)", "List<a> -> List<a>"},
		{"flatten", flatten, 1, 1, ret::list, 0U, "flatten(list): one level", "List -> List"},
		{"sort", sort, 1, 1, ret::list, 0U, "sort(list): by text, stable", "List<a> -> List<a>"},
		{"unique", unique, 1, 1, ret::list, 0U, "unique(list): first of each text", "List<a> -> List<a>"},
		{"count", count, 2, 2, ret::integer, 0U, "count(list, value): occurrences", "List, ? -> Int"},
		{"sum", sum, 1, -1, ret::integer, 0U, "sum(list) or sum(values...)", "...? -> Int"},
		{"min", min, 1, -1, ret::integer, 0U, "min(list) or min(values...)", "...? -> Int"},
		{"max", max, 1, -1, ret::integer, 0U, "max(list) or max(values...)", "...? -> Int"},
		{"fold", fold, 3, 3, ret::any, 0U, "fold(list, init, (acc, item) -> acc)", "List<a>, b, (b, a -> b) -> b"},
		{"index", index, 2, 3, ret::integer, 0U, "index(list, value, key?): first index, or -1", "List<a>, ?, (a -> ?)? -> Int"},
	};
	for (const fb::builtin& b : builtins)
		in.define(b);
	return {};
}
