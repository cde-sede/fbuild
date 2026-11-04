// fbregex: the `regex` namespace object (match, find, findall, groups, replace, replaceall,
// split, escape). Ported from cparser's plugins/fbregex.c, on ca::basic_regex instead of POSIX
// <regex.h>: patterns are Python re syntax (leftmost-first, linear time, no backreferences or
// lookaround), not POSIX ERE. The results keep the C plugin's shapes: findall gives whole
// matches whatever the groups, split drops the groups and ignores empty matches, replace takes
// \0-\9. Methods receive the namespace object first; its data is the cache of compiled patterns.
#include "5-collections/regex/basic_regex.hpp"
#include "support.hpp"

namespace {

	namespace s = fb::support;

	using regex = ca::basic_regex<ca::arena_allocator>;

	/* The patterns compiled so far, by text, so a pattern used in a loop compiles once. Everything
	   lives in the interpreter's memory, released with it. */
	using cache = ca::hashmap<ca::sv::str, regex*, ca::arena_allocator>;

	auto text(fb::interpreter& in, const fb::arguments a, const ca::usz i) noexcept -> ca::sv::str {
		return in.text(a[i]);
	}

	/* The arguments after the namespace object, all Strings. */
	auto arity(fb::interpreter& in, const fb::arguments a, const ca::usz count, const ca::sv::str name) noexcept -> ca::result<void, fb::diagnostic> {

		if (a.size() != count + 1U)
			return in.fail("regex.{}: expected {} arguments, got {}", name, count, a.size() - 1U);
		return {};
	}

	/* The compiled pattern argument i, from the cache or compiled into it. */
	auto compiled(fb::interpreter& in, const fb::arguments a, const ca::usz i) noexcept -> ca::result<regex*, fb::diagnostic> {

		cache* const known = static_cast<cache*>(a[0U]->data);
		const ca::sv::str pattern = text(in, a, i);
		if (auto found = known->get(pattern))
			return *found;
		// check() first: it gives the position, and keeps make_in to what it cannot know (size).
		if (const auto problem = ca::regex_pattern{pattern}.check())
			return in.fail("regex: invalid pattern '{}': {} at position {}", pattern, problem->message(), problem->position());
		const ca::sv::str kept = in.memory().copy(pattern);
		auto made = regex::make_in(in.memory().allocator(), ca::regex_pattern{kept});
		if (!made)
			return in.fail("regex: invalid pattern '{}': too large", pattern);
		regex* const r = in.memory().make<regex>(ca::move(*made));
		if (!known->set(kept, r))
			ca::trap("fbregex: out of memory");
		return r;
	}

	/* The next match of r in its text, or nothing; out of memory traps like the interpreter. */
	auto next(regex::matches_type& matches) noexcept -> ca::optional<regex::match_type> {

		auto found = matches.next();
		if (!found)
			ca::trap("fbregex: out of memory");
		return ca::move(*found);
	}

	auto first(regex& r, const ca::sv::str t) noexcept -> ca::optional<regex::match_type> {

		auto found = r.search(t);
		if (!found)
			ca::trap("fbregex: out of memory");
		return ca::move(*found);
	}

	/* replacement onto out, \0-\9 standing for the groups of m (nothing for a group that took
	   no part or does not exist), any other character as is. */
	auto expand(fb::text& out, const ca::sv::str replacement, const regex::match_type& m) noexcept -> void {

		for (ca::usz i = 0U; i < replacement.size(); ++i) {
			const char c = replacement.data()[i];
			if (c == '\\' && i + 1U < replacement.size() && s::is_digit(replacement.data()[i + 1U])) {
				const auto group = m.group(static_cast<ca::usz>(replacement.data()[++i] - '0'));
				if (group)
					static_cast<void>(out.append(*group));
				continue;
			}
			static_cast<void>(out.push(c));
		}
	}

	/* t with its first count matches (0: all) replaced. */
	auto substitute(fb::interpreter& in, const fb::arguments a, const ca::usz count, const ca::sv::str name) noexcept -> fb::outcome {

		if (auto ok = arity(in, a, 3U, name); !ok)
			return ok.propagate();
		auto r = compiled(in, a, 2U);
		if (!r)
			return r.propagate();
		const ca::sv::str t = text(in, a, 1U);
		const ca::sv::str replacement = text(in, a, 3U);
		fb::text out{in.memory().allocator()};
		auto matches = (*r)->finditer(t);
		ca::usz last = 0U;
		ca::usz done = 0U;
		while (count == 0U || done < count) {
			auto m = next(matches);
			if (!m)
				break;
			const ca::usz start = static_cast<ca::usz>(m->group().data() - t.data());
			static_cast<void>(out.append(t.slice(last, start - last)));
			expand(out, replacement, *m);
			last = start + m->group().size();
			++done;
		}
		static_cast<void>(out.append(t.without_prefix(last)));
		return in.string(out.view());
	}

	auto m_match(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = arity(in, a, 2U, "match"); !ok)
			return ok.propagate();
		auto r = compiled(in, a, 2U);
		if (!r)
			return r.propagate();
		return in.boolean(static_cast<bool>(first(**r, text(in, a, 1U))));
	}

	auto m_find(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = arity(in, a, 2U, "find"); !ok)
			return ok.propagate();
		auto r = compiled(in, a, 2U);
		if (!r)
			return r.propagate();
		const auto m = first(**r, text(in, a, 1U));
		return m ? in.string(m->group()) : in.unit();
	}

	auto m_findall(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = arity(in, a, 2U, "findall"); !ok)
			return ok.propagate();
		auto r = compiled(in, a, 2U);
		if (!r)
			return r.propagate();
		fb::value* const out = in.make(fb::kind::list);
		auto matches = (*r)->finditer(text(in, a, 1U));
		for (auto m = next(matches); m; m = next(matches))
			in.push(out, in.string(m->group()));
		return out;
	}

	auto m_groups(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = arity(in, a, 2U, "groups"); !ok)
			return ok.propagate();
		auto r = compiled(in, a, 2U);
		if (!r)
			return r.propagate();
		fb::value* const out = in.make(fb::kind::list);
		const auto m = first(**r, text(in, a, 1U));
		if (!m)
			return out;
		for (ca::usz g = 0U; g <= m->group_count(); ++g) {
			const auto group = m->group(g);
			in.push(out, group ? in.string(*group) : in.unit());
		}
		return out;
	}

	auto m_replace(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return substitute(in, a, 1U, "replace");
	}

	auto m_replaceall(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return substitute(in, a, 0U, "replaceall");
	}

	auto m_split(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = arity(in, a, 2U, "split"); !ok)
			return ok.propagate();
		auto r = compiled(in, a, 2U);
		if (!r)
			return r.propagate();
		const ca::sv::str t = text(in, a, 1U);
		fb::value* const out = in.make(fb::kind::list);
		auto matches = (*r)->finditer(t);
		ca::usz last = 0U;
		for (auto m = next(matches); m; m = next(matches)) {
			if (m->group().empty())
				continue;
			const ca::usz start = static_cast<ca::usz>(m->group().data() - t.data());
			in.push(out, in.string(t.slice(last, start - last)));
			last = start + m->group().size();
		}
		in.push(out, in.string(t.without_prefix(last)));
		return out;
	}

	auto m_escape(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = arity(in, a, 1U, "escape"); !ok)
			return ok.propagate();
		auto escaped = regex::escape_in(in.memory().allocator(), text(in, a, 1U));
		if (!escaped)
			ca::trap("fbregex: out of memory");
		return in.string_in_place(escaped->view());
	}

	auto not_callable(fb::interpreter& in, const fb::arguments) noexcept -> fb::outcome {
		return in.fail("regex is a namespace object; call e.g. regex.match(s, pattern), not regex(...)");
	}

} // namespace

auto fb::plugin::api(void) noexcept -> ca::u32 {
	return fb::plugin_api;
}

auto fb::plugin::open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {

	fb::class_info* const cls = in.class_named("regex");
	in.method(cls, "match", m_match);
	in.method(cls, "find", m_find);
	in.method(cls, "findall", m_findall);
	in.method(cls, "groups", m_groups);
	in.method(cls, "replace", m_replace);
	in.method(cls, "replaceall", m_replaceall);
	in.method(cls, "split", m_split);
	in.method(cls, "escape", m_escape);
	cache* const known = in.memory().make<cache>(in.memory().allocator());
	in.define({"regex", not_callable, 0, -1, fb::ret::any, fb::global,
		"regex namespace (fbregex, Python re syntax): match, find, findall, groups, replace, replaceall, split, escape"});
	in.bind("regex", in.object(cls, known));
	return {};
}
