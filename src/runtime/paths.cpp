#include "runtime/library.hpp"

#include "ca/path.hpp"
#include "ca/str.hpp"
#include "5-collections/fs/impl/path_text.hpp"

namespace fb::impl {

	[[nodiscard]] static auto class_matches(const char*& pattern, const char* const end, const char c) noexcept -> bool {

		const char* p = pattern;
		bool negated = false;
		bool matched = false;
		if (p < end && (*p == '!' || *p == '^')) {
			negated = true;
			++p;
		}
		while (p < end && *p != ']') {
			if (p + 2 < end && p[1] == '-' && p[2] != ']') {
				if (c >= p[0] && c <= p[2])
					matched = true;
				p += 3;
			} else {
				if (c == *p)
					matched = true;
				++p;
			}
		}
		if (p < end && *p == ']')
			++p;
		pattern = p;
		return negated ? !matched : matched;
	}

	/* The morphism matcher: ca's glob match does not report what each '*' took, which subst
	   and // need. */
	struct capture final {
		ca::usz start;
		ca::usz size;
	};

	inline constexpr ca::usz max_captures = 32U;

	[[nodiscard]] static auto match(const char* pattern, const char* const pattern_end, const char* text, const char* const text_begin,
		const char* const text_end, fb::impl::capture* const captures, ca::usz& count) noexcept -> bool {

		while (pattern < pattern_end) {
			if (*pattern == '*') {
				++pattern;
				if (count >= fb::impl::max_captures)
					return false;
				const ca::usz mine = count++;
				captures[mine].start = static_cast<ca::usz>(text - text_begin);
				for (const char* s = text;; ++s) {
					captures[mine].size = static_cast<ca::usz>(s - text);
					if (fb::impl::match(pattern, pattern_end, s, text_begin, text_end, captures, count))
						return true;
					if (s == text_end)
						break;
				}
				--count;
				return false;
			}
			if (text == text_end)
				return false;
			if (*pattern == '?') {
				++pattern;
				++text;
				continue;
			}
			if (*pattern == '[') {
				++pattern;
				if (!fb::impl::class_matches(pattern, pattern_end, *text))
					return false;
				++text;
				continue;
			}
			if (*pattern == '\\' && pattern + 1 < pattern_end)
				++pattern;
			if (*pattern != *text)
				return false;
			++pattern;
			++text;
		}
		return text == text_end;
	}

	[[nodiscard]] static auto modified(fb::memory& memory, const ca::sv::str text) noexcept -> ca::optional<ca::i64> {

		auto status = fb::path_of(memory, text).stat();
		if (!status)
			return ca::nullopt;
		return status->modified();
	}

	/* Whether found came from pattern without a magic component matching a hidden name: the C
	   glob skipped names starting with '.', unless written literally. */
	[[nodiscard]] static auto visible(const ca::sv::str found, const ca::sv::str pattern) noexcept -> bool {

		for (const ca::sv::str component : found.split("/")) {
			if (!component.starts_with("."))
				continue;
			bool literal = false;
			for (const ca::sv::str written : pattern.split("/"))
				if (written == component)
					literal = true;
			if (!literal)
				return false;
		}
		return true;
	}

} // namespace fb::impl

auto fb::path_of(fb::memory& memory, const ca::sv::str text) noexcept -> fb::path {

	auto made = fb::path::make_in(memory.allocator(), text);
	return made ? ca::move(*made) : fb::path{memory.allocator()};
}

auto fb::path_parent(fb::memory& memory, const ca::sv::str text) noexcept -> ca::sv::str {
	return fb::path_of(memory, text).parent();
}

auto fb::path_name(fb::memory& memory, const ca::sv::str text) noexcept -> ca::sv::str {
	return fb::path_of(memory, text).name();
}

auto fb::path_stem(fb::memory& memory, const ca::sv::str text) noexcept -> ca::sv::str {
	return fb::path_of(memory, text).stem();
}

auto fb::path_suffix(fb::memory& memory, const ca::sv::str text) noexcept -> ca::sv::str {
	return fb::path_of(memory, text).suffix();
}

auto fb::path_join(fb::memory& memory, const ca::sv::str parent, const ca::sv::str name) noexcept -> ca::sv::str {

	auto joined = fb::path_of(memory, parent).join(name);
	return joined ? joined->view() : memory.copy(name);
}

auto fb::file_field(fb::interpreter& in, const fb::value* const file, const ca::sv::str name) noexcept -> fb::value* {

	if (file == nullptr || !file->is(fb::kind::file))
		return in.unit();
	const fb::path path = fb::path_of(in.memory(), file->text);
	if (name == "name")
		return in.string_in_place(path.name());
	if (name == "parent")
		return in.string_in_place(path.parent());
	if (name == "stem")
		return in.string_in_place(path.stem());
	if (name == "suffix")
		return in.string_in_place(path.suffix());
	if (name == "mtime" || name == "size") {
		auto status = path.stat();
		if (!status)
			return in.integer(0);
		if (name == "size")
			return in.integer(static_cast<ca::i64>(status->size()));
		return in.integer(status->modified() / 1000000000);
	}
	return in.unit();
}

auto fb::rebuild_path(fb::memory& memory, const ca::sv::str base, const ca::optional<ca::sv::str> name, const ca::optional<ca::sv::str> stem,
	const ca::optional<ca::sv::str> suffix, const ca::optional<ca::sv::str> parent) noexcept -> ca::sv::str {

	const fb::path from = fb::path_of(memory, base);
	ca::sv::str file_name;
	if (name)
		file_name = *name;
	else if (stem || suffix)
		file_name = memory.format("{}{}", stem ? *stem : from.stem(), suffix ? *suffix : from.suffix());
	else
		file_name = from.name();
	return fb::path_join(memory, parent ? *parent : from.parent(), file_name);
}

auto fb::pattern_match(const ca::sv::str pattern, const ca::sv::str text) noexcept -> bool {
	return ca::fs::impl::path_glob_matches(pattern, text);
}

auto fb::substitute(fb::memory& memory, const ca::sv::str from, const ca::sv::str to, const ca::sv::str text) noexcept -> ca::sv::str {

	fb::impl::capture captures[fb::impl::max_captures];
	ca::usz count = 0U;
	if (!fb::impl::match(from.data(), from.data() + from.size(), text.data(), text.data(), text.data() + text.size(), captures, count))
		return memory.copy(text);
	fb::text out{memory.allocator()};
	ca::usz used = 0U;
	for (ca::usz i = 0U; i < to.size(); ++i) {
		const char c = to.data()[i];
		ca::result<void> appended{};
		if (c == '*' && used < count) {
			appended = out.append(text.slice(captures[used].start, captures[used].size));
			++used;
		} else if (c == '\\' && i + 1U < to.size())
			appended = out.push(to.data()[++i]);
		else
			appended = out.push(c);
		if (!appended) [[unlikely]]
			ca::trap("fbuild: out of memory");
	}
	return memory.copy(out.view());
}

auto fb::command_word(fb::memory& memory, const ca::sv::str raw) noexcept -> ca::sv::str {

	char* const out = static_cast<char*>(memory.bytes(raw.size() + 1U, 1U));
	ca::usz j = 0U;
	char quote = '\0';
	for (ca::usz i = 0U; i < raw.size(); ++i) {
		const char c = raw.data()[i];
		if (quote == '\'') {
			if (c == '\'')
				quote = '\0';
			else
				out[j++] = c;
			continue;
		}
		if (c == '\\' && i + 1U < raw.size()) {
			const char next = raw.data()[i + 1U];
			if (quote == '\0' || next == '"' || next == '\\' || next == '$' || next == '`') {
				out[j++] = next;
				++i;
				continue;
			}
			out[j++] = c;
			continue;
		}
		if (quote == '"' && c == '"') {
			quote = '\0';
			continue;
		}
		if (quote == '\0' && (c == '"' || c == '\'')) {
			quote = c;
			continue;
		}
		out[j++] = c;
	}
	out[j] = '\0';
	return ca::sv::str{out, j};
}

auto fb::glob(fb::interpreter& in, const ca::sv::str pattern) noexcept -> fb::value* {

	fb::memory& memory = in.memory();
	fb::value* const out = in.make(fb::kind::list);
	ca::sv::str relative = pattern;
	while (relative.starts_with("/"))
		relative = relative.without_prefix(1U);
	const fb::path base = fb::path_of(memory, relative.size() == pattern.size() ? ca::sv::str{"."} : ca::sv::str{"/"});
	auto found = base.glob(relative);
	if (!found)
		return out;
	for (const fb::path& p : *found)
		if (fb::impl::visible(p.view(), pattern))
			in.push(out, in.file_in_place(p.view()));
	return out;
}

auto fb::stale(fb::interpreter& in, const fb::arguments args) noexcept -> bool {

	if (args.size() == 0U)
		return false;
	const fb::value* const target = args[0U];
	if (!target->is(fb::kind::file))
		return true;
	auto built = fb::impl::modified(in.memory(), target->text);
	if (!built)
		return true;
	for (ca::usz i = 1U; i < args.size(); ++i) {
		const ca::sv::str dependency = args[i]->is(fb::kind::file) ? args[i]->text : ca::sv::str{""};
		auto changed = fb::impl::modified(in.memory(), dependency);
		if (!changed || *changed > *built)
			return true;
	}
	return false;
}
