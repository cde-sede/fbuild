#ifndef fb_syntax_token_hpp
#define fb_syntax_token_hpp

#include "ca/integers.hpp"
#include "ca/sv.hpp"
#include "core/diagnostic.hpp"

namespace fb {

	enum class tok : ca::u8 {
		eof,
		newline,

		id,
		integer,
		floating,
		boolean,
		nil,
		string,
		backtick,
		command,
		path,

		when,
		where,
		and_,
		or_,
		oror,
		not_,

		assign,
		tilde_assign,
		tilde,
		arrow,
		pipe,
		bar,
		morph,
		splat,
		seq,
		write,

		lt,
		gt,
		le,
		ge,
		eq,
		ne,

		dot,
		comma,
		colon,
		star,
		amp,
		at,
		plus,
		minus,
		slash,
		percent,

		lparen,
		rparen,
		lbrace,
		rbrace,
		str_beg,
		str_end,
		strtext,
	};

	/* Name of a token kind, as parse errors show it ("TOK_ID"). */
	[[nodiscard]] auto tok_name(fb::tok kind) noexcept -> ca::sv::str;

	/* A slice of the script: its kind, its text (raw, escapes not decoded) and its span. */
	struct token final {
		fb::tok kind;
		fb::position start;
		fb::position end;
		ca::sv::str text;
	};

} // namespace fb

#endif // fb_syntax_token_hpp
