#include "lsp/transport.hpp"

#include "ca/io.hpp"
#include "ca/print.hpp"
#include "ca/text.hpp"

namespace fb::lsp::impl {

	/* Whether line starts with name, letters compared without case (header names are
	   case-insensitive). */
	[[nodiscard]] static auto header_is(const ca::sv::str line, const ca::sv::str name) noexcept -> bool {

		if (line.size() < name.size())
			return false;
		for (ca::usz i = 0U; i < name.size(); ++i) {
			char a = line.data()[i];
			char b = name.data()[i];
			if (a >= 'A' && a <= 'Z')
				a = static_cast<char>(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = static_cast<char>(b - 'A' + 'a');
			if (a != b)
				return false;
		}
		return true;
	}

	/* Upper bound for one message: a document of a few megabytes, escaped. */
	inline constexpr ca::usz largest_message = 64U << 20U;

} // namespace fb::lsp::impl

auto fb::lsp::read_message(fb::memory& memory) noexcept -> ca::optional<ca::sv::str> {

	ca::io::reader& in = ca::io::in();
	ca::optional<ca::usz> length;
	bool any = false;
	for (;;) {
		auto line = in.read_until('\n');
		if (!line || line->empty())
			return ca::nullopt;
		const ca::sv::str header = line->strip();
		if (header.empty()) {
			if (!any)
				continue;  // stray blank lines between messages
			break;
		}
		any = true;
		constexpr ca::sv::str name = "content-length:";
		if (!fb::lsp::impl::header_is(header, name))
			continue;
		auto parsed = ca::sys::parse_integer<ca::u64>(header.without_prefix(name.size()).strip());
		if (!parsed || *parsed > fb::lsp::impl::largest_message)
			return ca::nullopt;
		length = static_cast<ca::usz>(*parsed);
	}
	if (!length)
		return ca::nullopt;
	char* const body = static_cast<char*>(memory.bytes(*length + 1U, 1U));
	auto got = in.read_full(ca::span<ca::byte>{reinterpret_cast<ca::byte*>(body), *length});
	if (!got || *got != *length)
		return ca::nullopt;
	body[*length] = '\0';
	return ca::sv::str{body, *length};
}

auto fb::lsp::write_message(const ca::sv::str body) noexcept -> ca::result<void> {

	ca::io::writer& out = ca::io::out();
	if (auto header = ca::print(out, "Content-Length: {}\r\n\r\n", body.size()); !header)
		return header.propagate();
	if (auto written = out.write(ca::span<const ca::byte>{reinterpret_cast<const ca::byte*>(body.data()), body.size()}); !written)
		return written.propagate();
	return out.flush();
}
