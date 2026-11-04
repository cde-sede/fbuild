#include "lsp/document.hpp"

#include "ca/io.hpp"
#include "ca/print.hpp"
#include "syntax/parser.hpp"

namespace fb::lsp::impl {

	/* The bytes of the UTF-8 sequence led by c (1 for a stray continuation byte). */
	[[nodiscard]] static auto sequence(const unsigned char c) noexcept -> ca::usz {

		if (c < 0x80U)
			return 1U;
		if ((c >> 5U) == 0x6U)
			return 2U;
		if ((c >> 4U) == 0xEU)
			return 3U;
		if ((c >> 3U) == 0x1EU)
			return 4U;
		return 1U;
	}

	/* UTF-16 code units in the first bytes of line. */
	[[nodiscard]] static auto units(const ca::sv::str line, const ca::usz bytes) noexcept -> ca::i64 {

		ca::i64 count = 0;
		for (ca::usz i = 0U; i < bytes && i < line.size();) {
			const ca::usz n = fb::lsp::impl::sequence(static_cast<unsigned char>(line.data()[i]));
			count += n == 4U ? 2 : 1;
			i += n;
		}
		return count;
	}

	/* The bytes of line taking up the first units UTF-16 code units. */
	[[nodiscard]] static auto bytes(const ca::sv::str line, const ca::i64 units) noexcept -> ca::usz {

		ca::i64 count = 0;
		ca::usz i = 0U;
		while (i < line.size() && count < units) {
			const ca::usz n = fb::lsp::impl::sequence(static_cast<unsigned char>(line.data()[i]));
			count += n == 4U ? 2 : 1;
			i += n;
		}
		return i < line.size() ? i : line.size();
	}

	/* Spliced after a dangling dot so the text parses: a name no script would use. */
	inline constexpr ca::sv::str placeholder = "fbuildd_placeholder_";

	[[nodiscard]] static auto word(const char c) noexcept -> bool {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}

	/* A dot ending an expression with no field after it: what "base." is while it is typed.
	   Field access needs a name, an index or {expr} after the dot, so the parser rejects it. */
	[[nodiscard]] static auto dangling(const ca::sv::str text, const ca::usz at) noexcept -> bool {

		if (text.data()[at] != '.' || at == 0U)
			return false;
		const char before = text.data()[at - 1U];
		if (!fb::lsp::impl::word(before) && before != ')' && before != ']' && before != '}')
			return false;
		if (at + 1U >= text.size())
			return true;
		const char after = text.data()[at + 1U];
		return !fb::lsp::impl::word(after) && after != '{' && after != '.';
	}

	/* text with the placeholder after each dangling dot (bytes before each insertion keep
	   their positions); nullopt when there is none. */
	[[nodiscard]] static auto splice(fb::memory& memory, const ca::sv::str text) noexcept -> ca::optional<ca::sv::str> {

		fb::text out{memory.allocator()};
		bool any = false;
		ca::usz from = 0U;
		for (ca::usz i = 0U; i < text.size(); ++i) {
			if (!fb::lsp::impl::dangling(text, i))
				continue;
			any = true;
			if (!out.append(text.slice(from, i + 1U - from)) || !out.append(fb::lsp::impl::placeholder)) [[unlikely]]
				ca::trap("fbuildd: out of memory");
			from = i + 1U;
		}
		if (!any)
			return ca::nullopt;
		if (!out.append(text.without_prefix(from))) [[unlikely]]
			ca::trap("fbuildd: out of memory");
		return memory.copy(out.view());
	}

	/* text with line (1-based) replaced by spaces, every other position kept. */
	[[nodiscard]] static auto blank(fb::memory& memory, const ca::sv::str text, const int line) noexcept -> ca::optional<ca::sv::str> {

		if (line <= 0)
			return ca::nullopt;
		ca::sv::str copy = memory.copy(text);
		char* const bytes = const_cast<char*>(copy.data());
		int current = 1;
		bool changed = false;
		for (ca::usz i = 0U; i < copy.size(); ++i) {
			if (bytes[i] == '\n') {
				if (++current > line)
					break;
				continue;
			}
			if (current == line && bytes[i] != ' ' && bytes[i] != '\r') {
				bytes[i] = ' ';
				changed = true;
			}
		}
		if (!changed)
			return ca::nullopt;
		return copy;
	}

	/* The last line before line (1-based) holding more than blanks; 0 when none. An error found
	   at the end of input (an unclosed call) is reported past the line that causes it. */
	[[nodiscard]] static auto previous_content(const ca::sv::str text, const int line) noexcept -> int {

		int current = 1;
		int found = 0;
		bool content = false;
		for (ca::usz i = 0U; i <= text.size() && current < line; ++i) {
			const char c = i < text.size() ? text.data()[i] : '\n';
			if (c == '\n') {
				if (content)
					found = current;
				content = false;
				++current;
			} else if (c != ' ' && c != '\t' && c != '\r')
				content = true;
		}
		return found;
	}

	/* Checks text into a (reset first); the syntax error when it does not parse. */
	[[nodiscard]] static auto analyze(fb::lsp::analysis& a, const ca::sv::str text, const fb::interpreter& in) noexcept -> ca::optional<fb::diagnostic> {

		a.memory.reset();
		a.lines = a.memory.make<fb::lsp::lines>(a.memory, a.memory.copy(text));
		a.program = nullptr;
		a.types = nullptr;
		a.catalog = nullptr;
		a.report = nullptr;
		auto program = fb::parse_source(a.memory, a.lines->text());
		if (!program)
			return program.error();
		a.program = *program;
		a.types = a.memory.make<fb::check::types>(a.memory);
		a.catalog = a.memory.make<fb::check::catalog>(*a.types);
		if (auto described = in.describe(*a.catalog); !described)
			static_cast<void>(ca::print(ca::io::err(), "fbuildd: {}\n", described.error().message));
		a.report = fb::check::check(*a.types, *a.catalog, a.program);
		return ca::nullopt;
	}

} // namespace fb::lsp::impl

auto fb::lsp::length_of(const ca::sv::str text, const bool utf16) noexcept -> ca::i64 {
	return utf16 ? fb::lsp::impl::units(text, text.size()) : static_cast<ca::i64>(text.size());
}

// -- lines ---------------------------------------------------------------------

fb::lsp::lines::lines(fb::memory& memory, const ca::sv::str text) noexcept
: _text{text}, _starts{memory.list<ca::usz>()} {

	memory.push(_starts, ca::usz{0U});
	for (ca::usz i = 0U; i < text.size(); ++i)
		if (text.data()[i] == '\n')
			memory.push(_starts, i + 1U);
}

auto fb::lsp::lines::line(const ca::usz index) const noexcept -> ca::sv::str {

	if (index >= _starts.size())
		return {};
	const ca::usz start = _starts.data()[index];
	ca::usz end = index + 1U < _starts.size() ? _starts.data()[index + 1U] - 1U : _text.size();
	if (end > start && _text.data()[end - 1U] == '\r')
		--end;
	return _text.slice(start, end - start);
}

auto fb::lsp::lines::offset(const fb::position at) const noexcept -> ca::usz {

	const ca::usz index = at.line > 0 ? static_cast<ca::usz>(at.line - 1) : 0U;
	if (index >= _starts.size())
		return _text.size();
	const ca::sv::str l = this->line(index);
	const ca::usz column = at.col > 0 ? static_cast<ca::usz>(at.col - 1) : 0U;
	return _starts.data()[index] + (column < l.size() ? column : l.size());
}

auto fb::lsp::lines::place_of(const fb::position at, const bool utf16) const noexcept -> fb::lsp::place {

	const ca::usz index = at.line > 0 ? static_cast<ca::usz>(at.line - 1) : 0U;
	const ca::sv::str l = this->line(index);
	ca::usz column = at.col > 0 ? static_cast<ca::usz>(at.col - 1) : 0U;
	if (column > l.size())
		column = l.size();
	return fb::lsp::place{static_cast<ca::i64>(index), utf16 ? fb::lsp::impl::units(l, column) : static_cast<ca::i64>(column)};
}

auto fb::lsp::lines::place_after(const fb::position at, const bool utf16) const noexcept -> fb::lsp::place {

	const ca::usz index = at.line > 0 ? static_cast<ca::usz>(at.line - 1) : 0U;
	const ca::sv::str l = this->line(index);
	ca::usz column = at.col > 0 ? static_cast<ca::usz>(at.col - 1) : 0U;
	if (column < l.size()) {
		++column;
		while (column < l.size() && (static_cast<unsigned char>(l.data()[column]) & 0xC0U) == 0x80U)
			++column;
	} else
		column = l.size();
	return fb::lsp::place{static_cast<ca::i64>(index), utf16 ? fb::lsp::impl::units(l, column) : static_cast<ca::i64>(column)};
}

auto fb::lsp::lines::position_of(const fb::lsp::place at, const bool utf16) const noexcept -> fb::position {

	if (at.line < 0)
		return fb::position{1, 1};
	const ca::usz index = static_cast<ca::usz>(at.line);
	const ca::sv::str l = this->line(index);
	const ca::i64 character = at.character < 0 ? 0 : at.character;
	ca::usz column = utf16 ? fb::lsp::impl::bytes(l, character) : static_cast<ca::usz>(character);
	if (column > l.size())
		column = l.size();
	return fb::position{static_cast<int>(index + 1U), static_cast<int>(column + 1U)};
}

// -- document ------------------------------------------------------------------

fb::lsp::document::document(const ca::sv::str uri, fb::memory&& source, fb::memory&& first, fb::memory&& second) noexcept
: _uri{uri}, _version{0}, _source_memory{ca::move(source)}, _source{nullptr}, _syntax{},
  _first{ca::move(first), nullptr, nullptr, nullptr, nullptr, nullptr},
  _second{ca::move(second), nullptr, nullptr, nullptr, nullptr, nullptr},
  _usable{nullptr}, _fresh{false} {

	_source = _source_memory.make<fb::lsp::lines>(_source_memory, ca::sv::str{});
}

auto fb::lsp::document::update(const ca::sv::str text, const ca::i64 version, const fb::interpreter& in) noexcept -> void {

	_source_memory.reset();
	_source = _source_memory.make<fb::lsp::lines>(_source_memory, _source_memory.copy(text));
	_syntax = ca::nullopt;
	_version = version;
	fb::lsp::analysis* const spare = _usable == &_first ? &_second : &_first;
	auto failed = fb::lsp::impl::analyze(*spare, _source->text(), in);
	if (!failed) {
		_usable = spare;
		_fresh = true;
		return;
	}
	// The message lives in spare's memory, which the repairs below reset.
	const fb::diagnostic syntax{_source_memory.copy(failed->message), failed->at};
	_syntax = syntax;
	const ca::optional<ca::sv::str> repairs[] = {
		fb::lsp::impl::splice(_source_memory, _source->text()),
		fb::lsp::impl::blank(_source_memory, _source->text(), syntax.at.line),
		fb::lsp::impl::blank(_source_memory, _source->text(), fb::lsp::impl::previous_content(_source->text(), syntax.at.line)),
	};
	for (const ca::optional<ca::sv::str>& repaired : repairs)
		if (repaired && !fb::lsp::impl::analyze(*spare, *repaired, in)) {
			_usable = spare;
			_fresh = true;
			return;
		}
	_fresh = false;
}

auto fb::lsp::document::cleanup(void) noexcept -> ca::result<void> {

	const bool source = static_cast<bool>(_source_memory.cleanup());
	const bool first = static_cast<bool>(_first.memory.cleanup());
	const bool second = static_cast<bool>(_second.memory.cleanup());
	if (!source || !first || !second)
		return ca::failure{ca::errc::io_error};
	return {};
}
