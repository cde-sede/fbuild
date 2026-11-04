#include "syntax/lexer.hpp"

#include "ca/failure.hpp"

namespace fb::impl {

	[[nodiscard]] constexpr auto is_digit(const char c) noexcept -> bool {
		return c >= '0' && c <= '9';
	}

	[[nodiscard]] constexpr auto is_alpha(const char c) noexcept -> bool {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	[[nodiscard]] constexpr auto is_word(const char c) noexcept -> bool {
		return fb::impl::is_alpha(c) || fb::impl::is_digit(c) || c == '_';
	}

	[[nodiscard]] constexpr auto is_blank(const char c) noexcept -> bool {
		return c == ' ' || c == '\t' || c == '\r';
	}

	struct operator_spelling final {
		ca::sv::str spelling;
		fb::tok kind;
	};

	inline constexpr fb::impl::operator_spelling operators[] = {
		{"...", fb::tok::splat},
		{"->", fb::tok::arrow},
		{"~=", fb::tok::tilde_assign},
		{"|>", fb::tok::pipe},
		{"||", fb::tok::oror},
		{"//", fb::tok::morph},
		{"<=", fb::tok::le},
		{"<-", fb::tok::write},
		{">>", fb::tok::seq},
		{">=", fb::tok::ge},
		{"==", fb::tok::eq},
		{"!=", fb::tok::ne},
	};

	inline constexpr fb::impl::operator_spelling keywords[] = {
		{"when", fb::tok::when},
		{"where", fb::tok::where},
		{"and", fb::tok::and_},
		{"or", fb::tok::or_},
		{"not", fb::tok::not_},
		{"true", fb::tok::boolean},
		{"false", fb::tok::boolean},
		{"nil", fb::tok::nil},
	};

	class lexer final {

		private:

			using self = fb::impl::lexer;
			using failed = ca::failure<fb::diagnostic>;

			fb::memory& _memory;
			ca::sv::str _source;
			fb::list<fb::token> _tokens;
			ca::usz _i;
			int _line;
			int _col;
			bool _command;
			bool _string;
			char _quote;
			int _depth;
			int _parens;

			[[nodiscard]] auto _at(const ca::usz index) const noexcept -> char {
				return index < _source.size() ? _source.data()[index] : '\0';
			}

			auto _emit(const fb::tok kind, const ca::usz start, const ca::usz size, const fb::position from, const fb::position to) noexcept -> void {
				_memory.push(_tokens, fb::token{kind, from, to, ca::sv::str{_source.data() + start, size}});
			}

			auto _emit_one(const fb::tok kind) noexcept -> void {

				this->_emit(kind, _i, 1U, {_line, _col}, {_line, _col});
				++_i;
				++_col;
			}

			[[nodiscard]] auto _fail(const ca::sv::str message) const noexcept -> failed {
				return failed{fb::diagnostic{message, {_line, _col}}};
			}

			auto _advance(void) noexcept -> void {

				++_i;
				++_col;
			}

			auto _newline(void) noexcept -> void {

				++_i;
				++_line;
				_col = 1;
			}

			[[nodiscard]] auto _interpolation(const char c) noexcept -> bool {

				if (fb::impl::is_blank(c)) {
					this->_advance();
					return true;
				}
				if (c == '\n') {
					this->_newline();
					return true;
				}
				if (c == '{' || c == '(') {
					this->_emit_one(c == '{' ? fb::tok::lbrace : fb::tok::lparen);
					++_depth;
					return true;
				}
				if (c == '}' || c == ')') {
					this->_emit_one(c == '}' ? fb::tok::rbrace : fb::tok::rparen);
					--_depth;
					return true;
				}
				return false;
			}

			[[nodiscard]] auto _command_text(const char c) noexcept -> ca::result<void, fb::diagnostic> {

				if (fb::impl::is_blank(c)) {
					this->_advance();
					return {};
				}
				if (c == '\n') {
					this->_newline();
					return {};
				}
				if (c == '`') {
					this->_emit_one(fb::tok::backtick);
					_command = false;
					return {};
				}
				if (c == '{') {
					this->_emit_one(fb::tok::lbrace);
					++_depth;
					return {};
				}
				const ca::usz start = _i;
				const fb::position from{_line, _col};
				for (char ch = this->_at(_i); ch != '\0'; ch = this->_at(_i)) {
					if (fb::impl::is_blank(ch) || ch == '\n' || ch == '`' || ch == '{')
						break;
					if (ch == '\'' || ch == '"') {
						this->_advance();
						while (this->_at(_i) != '\0' && this->_at(_i) != ch) {
							if (this->_at(_i) == '\\' && this->_at(_i + 1U) != '\0') {
								_i += 2U;
								_col += 2;
								continue;
							}
							if (this->_at(_i) == '\n')
								return this->_fail("newline in command string");
							this->_advance();
						}
						if (this->_at(_i) == '\0')
							return this->_fail("unterminated string in command");
						this->_advance();
						continue;
					}
					this->_advance();
				}
				this->_emit(fb::tok::command, start, _i - start, from, {_line, _col - 1});
				return {};
			}

			[[nodiscard]] auto _string_text(const char c) noexcept -> ca::result<void, fb::diagnostic> {

				if (c == _quote) {
					this->_emit_one(fb::tok::str_end);
					_string = false;
					return {};
				}
				if (c == '{') {
					this->_emit_one(fb::tok::lbrace);
					++_depth;
					return {};
				}
				const ca::usz start = _i;
				const fb::position from{_line, _col};
				while (this->_at(_i) != '\0' && this->_at(_i) != _quote && this->_at(_i) != '{') {
					if (this->_at(_i) == '\\' && this->_at(_i + 1U) != '\0') {
						_i += 2U;
						_col += 2;
						continue;
					}
					if (this->_at(_i) == '\n')
						return this->_fail("newline in string");
					this->_advance();
				}
				if (this->_at(_i) == '\0')
					return this->_fail("unterminated string");
				this->_emit(fb::tok::strtext, start, _i - start, from, {_line, _col - 1});
				return {};
			}

			[[nodiscard]] auto _path(void) noexcept -> ca::result<void, fb::diagnostic> {

				const fb::position from{_line, _col + 1};
				this->_advance();
				const ca::usz start = _i;
				while (this->_at(_i) != '\0' && this->_at(_i) != ']') {
					if (this->_at(_i) == '\n')
						return this->_fail("newline inside path literal");
					this->_advance();
				}
				if (this->_at(_i) == '\0')
					return this->_fail("unterminated path literal");
				this->_emit(fb::tok::path, start, _i - start, from, {_line, _col});
				this->_advance();
				return {};
			}

			[[nodiscard]] auto _quoted(const char quote) noexcept -> ca::result<void, fb::diagnostic> {

				if (quote == '"') {
					for (ca::usz j = _i + 1U; this->_at(j) != '\0' && this->_at(j) != quote; ++j) {
						if (this->_at(j) == '\\' && this->_at(j + 1U) != '\0') {
							++j;
							continue;
						}
						if (this->_at(j) == '\n')
							break;
						if (this->_at(j) == '{') {
							this->_emit_one(fb::tok::str_beg);
							_string = true;
							_quote = quote;
							_depth = 0;
							return {};
						}
					}
				}
				const fb::position from{_line, _col + 1};
				this->_advance();
				const ca::usz start = _i;
				while (this->_at(_i) != '\0' && this->_at(_i) != quote) {
					if (this->_at(_i) == '\\' && this->_at(_i + 1U) != '\0') {
						_i += 2U;
						_col += 2;
						continue;
					}
					if (this->_at(_i) == '\n')
						return this->_fail("newline in string");
					this->_advance();
				}
				if (this->_at(_i) == '\0')
					return this->_fail("unterminated string");
				this->_emit(fb::tok::string, start, _i - start, from, {_line, _col});
				this->_advance();
				return {};
			}

			[[nodiscard]] auto _operator(void) noexcept -> bool {

				const ca::sv::str rest{_source.data() + _i, _source.size() - _i};
				for (const fb::impl::operator_spelling& op : fb::impl::operators) {
					if (!rest.starts_with(op.spelling))
						continue;
					const int width = static_cast<int>(op.spelling.size());
					this->_emit(op.kind, _i, op.spelling.size(), {_line, _col}, {_line, _col + width - 1});
					_i += op.spelling.size();
					_col += width;
					return true;
				}
				return false;
			}

			auto _number(void) noexcept -> void {

				const ca::usz start = _i;
				const fb::position from{_line, _col};
				while (fb::impl::is_digit(this->_at(_i)))
					this->_advance();
				fb::tok kind = fb::tok::integer;
				if (this->_at(_i) == '.' && fb::impl::is_digit(this->_at(_i + 1U))) {
					this->_advance();
					while (fb::impl::is_digit(this->_at(_i)))
						this->_advance();
					kind = fb::tok::floating;
				}
				this->_emit(kind, start, _i - start, from, {_line, _col - 1});
			}

			auto _word(void) noexcept -> void {

				const ca::usz start = _i;
				const fb::position from{_line, _col};
				while (fb::impl::is_word(this->_at(_i)))
					this->_advance();
				const ca::sv::str word{_source.data() + start, _i - start};
				fb::tok kind = fb::tok::id;
				for (const fb::impl::operator_spelling& keyword : fb::impl::keywords)
					if (word == keyword.spelling)
						kind = keyword.kind;
				this->_emit(kind, start, word.size(), from, {_line, _col - 1});
			}

			[[nodiscard]] auto _single(const char c) noexcept -> ca::result<void, fb::diagnostic> {

				fb::tok kind;
				switch (c) {
					case '=': kind = fb::tok::assign; break;
					case '~': kind = fb::tok::tilde; break;
					case '.': kind = fb::tok::dot; break;
					case ',': kind = fb::tok::comma; break;
					case ':': kind = fb::tok::colon; break;
					case '*': kind = fb::tok::star; break;
					case '+': kind = fb::tok::plus; break;
					case '-': kind = fb::tok::minus; break;
					case '/': kind = fb::tok::slash; break;
					case '%': kind = fb::tok::percent; break;
					case '&': kind = fb::tok::amp; break;
					case '|': kind = fb::tok::bar; break;
					case '@': kind = fb::tok::at; break;
					case '<': kind = fb::tok::lt; break;
					case '>': kind = fb::tok::gt; break;
					case '(':
					case '{':
						++_parens;
						kind = c == '(' ? fb::tok::lparen : fb::tok::lbrace;
						break;
					case ')':
					case '}':
						if (_parens > 0)
							--_parens;
						kind = c == ')' ? fb::tok::rparen : fb::tok::rbrace;
						break;
					default:
						return failed{fb::diagnostic{_memory.format("unexpected character '{}' at {}:{}", c, _line, _col), {_line, _col}}};
				}
				this->_emit_one(kind);
				return {};
			}

			[[nodiscard]] auto _normal(const char c) noexcept -> ca::result<void, fb::diagnostic> {

				if (fb::impl::is_blank(c)) {
					this->_advance();
					return {};
				}
				if (c == '\n') {
					if (_parens == 0)
						this->_emit(fb::tok::newline, _i, 1U, {_line, _col}, {_line, _col});
					this->_newline();
					return {};
				}
				if (c == '#') {
					while (this->_at(_i) != '\0' && this->_at(_i) != '\n')
						this->_advance();
					return {};
				}
				if (c == '`') {
					this->_emit_one(fb::tok::backtick);
					_command = true;
					_depth = 0;
					return {};
				}
				if (c == '[')
					return this->_path();
				if (c == '"' || c == '\'')
					return this->_quoted(c);
				if (this->_operator())
					return {};
				if (fb::impl::is_digit(c)) {
					this->_number();
					return {};
				}
				if (fb::impl::is_alpha(c) || c == '_') {
					this->_word();
					return {};
				}
				return this->_single(c);
			}

		public:

			lexer(fb::memory& memory, const ca::sv::str source) noexcept
			: _memory{memory}, _source{source}, _tokens{memory.list<fb::token>()}, _i{0U}, _line{1}, _col{1},
			  _command{false}, _string{false}, _quote{'\0'}, _depth{0}, _parens{0} {
			}

			[[nodiscard]] auto run(void) noexcept -> ca::result<fb::list<fb::token>, fb::diagnostic> {

				for (char c = this->_at(_i); c != '\0'; c = this->_at(_i)) {
					if ((_command || _string) && _depth > 0 && this->_interpolation(c))
						continue;
					ca::result<void, fb::diagnostic> step{};
					if ((_command || _string) && _depth == 0)
						step = _command ? this->_command_text(c) : this->_string_text(c);
					else
						step = this->_normal(c);
					if (!step)
						return step.propagate();
				}
				if (_command)
					return this->_fail("unterminated command literal");
				if (_string)
					return this->_fail("unterminated string literal");
				this->_emit(fb::tok::eof, 0U, 0U, {_line, _col}, {_line, _col});
				return ca::move(_tokens);
			}

	}; // class lexer

} // namespace fb::impl

auto fb::lex(fb::memory& memory, const ca::sv::str source) noexcept -> ca::result<fb::list<fb::token>, fb::diagnostic> {

	fb::impl::lexer lexer{memory, source};
	return lexer.run();
}

auto fb::tok_name(const fb::tok kind) noexcept -> ca::sv::str {

	constexpr ca::sv::str names[] = {
		"TOK_EOF", "TOK_NEWLINE", "TOK_ID", "TOK_INT", "TOK_FLOAT", "TOK_BOOL", "TOK_NIL", "TOK_STRING",
		"TOK_BACKTICK", "TOK_COMMAND", "TOK_PATH", "TOK_WHEN", "TOK_WHERE", "TOK_AND", "TOK_OR", "TOK_OROR",
		"TOK_NOT", "TOK_ASSIGN", "TOK_TILDE_ASSIGN", "TOK_TILDE", "TOK_ARROW", "TOK_PIPE", "TOK_BAR",
		"TOK_MORPH", "TOK_SPLAT", "TOK_SEQ", "TOK_WRITE", "TOK_LT", "TOK_GT", "TOK_LE", "TOK_GE", "TOK_EQ",
		"TOK_NE", "TOK_DOT", "TOK_COMMA", "TOK_COLON", "TOK_STAR", "TOK_AMP", "TOK_AT", "TOK_PLUS",
		"TOK_MINUS", "TOK_SLASH", "TOK_PERCENT", "TOK_LPAREN", "TOK_RPAREN", "TOK_LBRACE", "TOK_RBRACE",
		"TOK_STR_BEG", "TOK_STR_END", "TOK_STRTEXT",
	};
	return names[static_cast<ca::usz>(kind)];
}
