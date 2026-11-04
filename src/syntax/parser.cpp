#include "syntax/parser.hpp"

#include "ca/failure.hpp"
#include "syntax/lexer.hpp"

namespace fb::impl {

	class parser final {

		private:

			using self = fb::impl::parser;
			using parsed = ca::result<fb::node*, fb::diagnostic>;
			using taken = ca::result<fb::token*, fb::diagnostic>;
			using failed = ca::failure<fb::diagnostic>;

			fb::memory& _memory;
			fb::list<fb::token>& _tokens;
			ca::usz _cur;
			bool _no_tuple;
			ca::u32 _nodes;

			// -- tokens -------------------------------------------------------

			[[nodiscard]] auto _token(const ca::usz index) const noexcept -> fb::token* {
				return index < _tokens.size() ? _tokens.data() + index : nullptr;
			}

			[[nodiscard]] auto _current(void) const noexcept -> fb::token* {
				return this->_token(_cur);
			}

			[[nodiscard]] auto _ahead(const ca::usz offset) const noexcept -> fb::token* {
				return this->_token(_cur + offset);
			}

			[[nodiscard]] auto _match(const fb::tok kind, const ca::usz index) const noexcept -> bool {

				const fb::token* const t = this->_token(index);
				return t != nullptr && t->kind == kind;
			}

			[[nodiscard]] auto _peek(const fb::tok kind) const noexcept -> bool {
				return this->_match(kind, _cur);
			}

			[[nodiscard]] auto _peek_word(const ca::sv::str word) const noexcept -> bool {
				return this->_peek(fb::tok::id) && this->_current()->text == word;
			}

			auto _advance(void) noexcept -> void {
				++_cur;
			}

			[[nodiscard]] auto _accept(const fb::tok kind) noexcept -> bool {

				if (!this->_peek(kind))
					return false;
				++_cur;
				return true;
			}

			[[nodiscard]] auto _expect(const fb::tok kind) noexcept -> taken {

				if (this->_peek(kind))
					return _tokens.data() + _cur++;
				const fb::token* const t = this->_current();
				fb::position at{0, 0};
				if (t != nullptr)
					at = t->start;
				else if (!_tokens.empty())
					at = _tokens.data()[_tokens.size() - 1U].end;
				const ca::sv::str got = t != nullptr
					? _memory.format("\"{}\"", t->text)
					: ca::sv::str{"<EOF>"};
				return failed{fb::diagnostic{
					_memory.format("parse error at {}:{}: expected kind {}, got {}", at.line, at.col, fb::tok_name(kind), got), at}};
			}

			[[nodiscard]] auto _at_eof(void) const noexcept -> bool {

				const fb::token* const t = this->_current();
				return t == nullptr || t->kind == fb::tok::eof;
			}

			auto _skip_newlines(void) noexcept -> void {

				while (this->_peek(fb::tok::newline))
					++_cur;
			}

			[[nodiscard]] auto _significant_from(const ca::usz from) const noexcept -> fb::tok {

				for (ca::usz i = from; i < _tokens.size(); ++i)
					if (_tokens.data()[i].kind != fb::tok::newline)
						return _tokens.data()[i].kind;
				return fb::tok::eof;
			}

			[[nodiscard]] auto _significant(void) const noexcept -> fb::tok {
				return this->_significant_from(_cur);
			}

			/* Whether the next significant token is the identifier word. */
			[[nodiscard]] auto _word_at_significant(const ca::sv::str word) const noexcept -> bool {

				for (ca::usz i = _cur; i < _tokens.size(); ++i)
					if (_tokens.data()[i].kind != fb::tok::newline)
						return _tokens.data()[i].kind == fb::tok::id && _tokens.data()[i].text == word;
				return false;
			}

			[[nodiscard]] auto _continues_with(const fb::tok kind) noexcept -> bool {

				if (this->_significant() != kind)
					return false;
				this->_skip_newlines();
				return this->_accept(kind);
			}

			// -- nodes --------------------------------------------------------

			[[nodiscard]] auto _node(const fb::node_kind kind, fb::token* const tok = nullptr) noexcept -> fb::node* {
				return _memory.make<fb::node>(kind, tok, _memory.list<fb::node*>(), _nodes++);
			}

			auto _push(fb::node* const parent, fb::node* const child) noexcept -> void {
				_memory.push(parent->kids, child);
			}

			[[nodiscard]] auto _fail(const ca::sv::str message, const fb::position at) const noexcept -> failed {
				return failed{fb::diagnostic{message, at}};
			}

			[[nodiscard]] auto _fail_here(const ca::sv::str message) const noexcept -> failed {

				const fb::token* const t = this->_current();
				return this->_fail(message, t != nullptr ? t->start : fb::position{0, 0});
			}

			// -- lookahead ----------------------------------------------------

			[[nodiscard]] auto _skip_newlines_at(ca::usz i) const noexcept -> ca::usz {

				while (i < _tokens.size() && _tokens.data()[i].kind == fb::tok::newline)
					++i;
				return i;
			}

			[[nodiscard]] auto _kind_at(const ca::usz i) const noexcept -> fb::tok {
				return i < _tokens.size() ? _tokens.data()[i].kind : fb::tok::eof;
			}

			[[nodiscard]] auto _lambda_ahead(const bool multi) const noexcept -> bool {

				ca::usz i = _cur;
				bool first = true;
				for (;;) {
					i = this->_skip_newlines_at(i);
					if (i >= _tokens.size())
						return false;
					const fb::tok kind = _tokens.data()[i].kind;
					bool paren = false;
					if (kind == fb::tok::lparen) {
						paren = true;
						i = this->_skip_newlines_at(i + 1U);
						if (this->_kind_at(i) != fb::tok::id || i >= _tokens.size())
							return false;
						++i;
						for (;;) {
							i = this->_skip_newlines_at(i);
							if (i >= _tokens.size() || this->_kind_at(i) != fb::tok::comma)
								break;
							i = this->_skip_newlines_at(i + 1U);
							if (i >= _tokens.size() || this->_kind_at(i) != fb::tok::id)
								return false;
							++i;
						}
						i = this->_skip_newlines_at(i);
						if (i >= _tokens.size() || this->_kind_at(i) != fb::tok::rparen)
							return false;
						++i;
					} else if (kind == fb::tok::at) {
						i = this->_skip_newlines_at(i + 1U);
						if (i < _tokens.size() && this->_kind_at(i) == fb::tok::id)
							++i;
					} else if (kind == fb::tok::dot) {
						i = this->_skip_newlines_at(i + 1U);
						if (i >= _tokens.size() || (this->_kind_at(i) != fb::tok::id && this->_kind_at(i) != fb::tok::integer))
							return false;
						++i;
					} else if (kind == fb::tok::id) {
						++i;
					} else {
						return false;
					}
					i = this->_skip_newlines_at(i);
					if (i >= _tokens.size())
						return false;
					const fb::tok after = this->_kind_at(i);
					if (after == fb::tok::arrow)
						return !(paren && first);
					if (after == fb::tok::comma && multi) {
						++i;
						first = false;
						continue;
					}
					return false;
				}
			}

			[[nodiscard]] auto _annotated_parens_ahead(void) const noexcept -> bool {

				ca::usz i = _cur;
				if (this->_kind_at(i) != fb::tok::lparen || i >= _tokens.size())
					return false;
				++i;
				int depth = 0;
				bool colon = false;
				for (;;) {
					if (i >= _tokens.size() || this->_kind_at(i) == fb::tok::eof)
						return false;
					const fb::tok kind = this->_kind_at(i);
					if (kind == fb::tok::lparen)
						++depth;
					else if (kind == fb::tok::rparen) {
						if (depth == 0)
							break;
						--depth;
					} else if (kind == fb::tok::colon && depth == 0)
						colon = true;
					++i;
				}
				if (!colon)
					return false;
				i = this->_skip_newlines_at(i + 1U);
				return i < _tokens.size() && this->_kind_at(i) == fb::tok::arrow;
			}

			[[nodiscard]] auto _constructor_ahead(void) const noexcept -> bool {

				if (!this->_peek(fb::tok::lt))
					return false;
				const fb::token* const next = this->_ahead(1U);
				if (next == nullptr)
					return false;
				if (next->kind == fb::tok::gt || next->kind == fb::tok::star)
					return true;
				if (next->kind != fb::tok::id)
					return false;
				const fb::token* const after = this->_ahead(2U);
				return after != nullptr && after->kind == fb::tok::assign;
			}

			[[nodiscard]] auto _expect_gt(void) noexcept -> ca::result<void, fb::diagnostic> {

				if (this->_accept(fb::tok::gt))
					return {};
				if (this->_peek(fb::tok::seq)) {
					fb::token* const t = this->_current();
					t->kind = fb::tok::gt;
					t->text = ca::sv::str{t->text.data(), 1U};
					return {};
				}
				auto gt = this->_expect(fb::tok::gt);
				if (!gt)
					return gt.propagate();
				return {};
			}

			// -- productions --------------------------------------------------

			[[nodiscard]] auto _pipe_rhs(void) noexcept -> parsed {
				return this->_lambda_ahead(false) ? this->_lambda() : this->_guarded();
			}

			[[nodiscard]] auto _lambda_body(void) noexcept -> parsed {
				return _no_tuple ? this->_expr_no_tuple() : this->_expression();
			}

			[[nodiscard]] auto _capture(fb::token* const at) noexcept -> fb::node* {

				fb::node* const n = this->_node(fb::node_kind::capture, at);
				this->_advance();
				if (this->_peek(fb::tok::id)) {
					this->_push(n, this->_node(fb::node_kind::id, this->_current()));
					this->_advance();
				}
				return n;
			}

			[[nodiscard]] auto _projection(const ca::sv::str message, const fb::token* const dot) noexcept -> parsed {

				fb::node* const n = this->_node(fb::node_kind::projection);
				this->_advance();
				if (!this->_peek(fb::tok::id) && !this->_peek(fb::tok::integer))
					return this->_fail(message, dot->start);
				n->tok = this->_current();
				this->_advance();
				return n;
			}

			[[nodiscard]] auto _prefixed(const fb::node_kind kind, fb::token* const t, const bool pipe) noexcept -> parsed {

				fb::node* const n = this->_node(kind, t);
				this->_advance();
				auto operand = pipe ? this->_pipe() : this->_postfix();
				if (!operand)
					return operand;
				this->_push(n, *operand);
				return n;
			}

			[[nodiscard]] auto _map_literal(void) noexcept -> parsed {

				fb::node* const n = this->_node(fb::node_kind::maplit);
				this->_advance();
				this->_skip_newlines();
				if (!this->_peek(fb::tok::rbrace)) {
					do {
						this->_skip_newlines();
						if (this->_peek(fb::tok::rbrace))
							break;
						if (this->_accept(fb::tok::splat)) {
							fb::node* const spread = this->_node(fb::node_kind::splat);
							auto value = this->_expr_no_tuple();
							if (!value)
								return value;
							this->_push(spread, *value);
							this->_push(n, spread);
							this->_skip_newlines();
							continue;
						}
						fb::node* const entry = this->_node(fb::node_kind::tuple);
						auto parsed_key = this->_expr_no_tuple();
						if (!parsed_key)
							return parsed_key;
						fb::node* key = *parsed_key;
						const bool bareword = key->kind == fb::node_kind::id;
						if (bareword && !this->_peek(fb::tok::colon)) {
							this->_push(entry, this->_node(fb::node_kind::string, key->tok));
							this->_push(entry, key);
							this->_push(n, entry);
							this->_skip_newlines();
							continue;
						}
						if (bareword)
							key = this->_node(fb::node_kind::string, key->tok);
						this->_push(entry, key);
						this->_skip_newlines();
						auto colon = this->_expect(fb::tok::colon);
						if (!colon)
							return colon.propagate();
						this->_skip_newlines();
						auto value = this->_expr_no_tuple();
						if (!value)
							return value;
						this->_push(entry, *value);
						this->_push(n, entry);
						this->_skip_newlines();
					} while (this->_accept(fb::tok::comma));
				}
				this->_skip_newlines();
				auto close = this->_expect(fb::tok::rbrace);
				if (!close)
					return close.propagate();
				return n;
			}

			[[nodiscard]] auto _annotated_lambda(void) noexcept -> parsed {

				this->_advance();
				fb::node* const params = this->_node(fb::node_kind::params);
				do {
					this->_skip_newlines();
					auto param = this->_parameter(true);
					if (!param)
						return param;
					this->_push(params, *param);
				} while (this->_continues_with(fb::tok::comma));
				this->_skip_newlines();
				auto close = this->_expect(fb::tok::rparen);
				if (!close)
					return close.propagate();
				this->_skip_newlines();
				auto arrow = this->_expect(fb::tok::arrow);
				if (!arrow)
					return arrow.propagate();
				this->_skip_newlines();
				return this->_lambda_of(params);
			}

			[[nodiscard]] auto _lambda_of(fb::node* const params) noexcept -> parsed {

				fb::node* const lambda = this->_node(fb::node_kind::lambda);
				this->_push(lambda, params);
				auto body = this->_lambda_body();
				if (!body)
					return body;
				this->_push(lambda, *body);
				return lambda;
			}

			[[nodiscard]] auto _parenthesized(fb::token* const open) noexcept -> parsed {

				if (this->_annotated_parens_ahead())
					return this->_annotated_lambda();
				this->_advance();
				this->_skip_newlines();
				if (this->_peek(fb::tok::rparen)) {
					if (this->_match(fb::tok::arrow, _cur + 1U)) {
						this->_advance();
						this->_skip_newlines();
						auto arrow = this->_expect(fb::tok::arrow);
						if (!arrow)
							return arrow.propagate();
						this->_skip_newlines();
						fb::node* const lambda = this->_node(fb::node_kind::lambda);
						this->_push(lambda, this->_node(fb::node_kind::params));
						auto body = this->_expression();
						if (!body)
							return body;
						this->_push(lambda, *body);
						return lambda;
					}
					fb::node* const nil = this->_node(fb::node_kind::nil, this->_current());
					this->_advance();
					return nil;
				}
				auto inner = this->_expression();
				if (!inner)
					return inner;
				fb::node* n = *inner;
				auto close = this->_expect(fb::tok::rparen);
				if (!close)
					return close.propagate();
				if (this->_accept(fb::tok::arrow)) {
					this->_skip_newlines();
					const ca::sv::str message = _memory.format(
						"parenthesized lambda parameter must be a plain identifier at {}:{}", open->start.line, open->start.col);
					fb::node* const params = this->_node(fb::node_kind::params);
					if (n->kind == fb::node_kind::tuple) {
						for (fb::node* const element : n->kids) {
							if (element->kind != fb::node_kind::id)
								return this->_fail(message, open->start);
							this->_push(params, element);
						}
					} else if (n->kind == fb::node_kind::id) {
						this->_push(params, n);
					} else {
						return this->_fail(message, open->start);
					}
					return this->_lambda_of(params);
				}
				if (n->kind == fb::node_kind::splat) {
					fb::node* const tuple = this->_node(fb::node_kind::tuple);
					this->_push(tuple, n);
					n = tuple;
				}
				return n;
			}

			[[nodiscard]] auto _atom(void) noexcept -> parsed {

				fb::token* const t = this->_current();
				if (t == nullptr || t->kind == fb::tok::eof)
					return this->_fail_here("unexpected end of input in expression");
				switch (t->kind) {
					case fb::tok::at:
						return this->_capture(t);
					case fb::tok::dot:
						return this->_projection(_memory.format("expected name or index after '.' at {}:{}", t->start.line, t->start.col), t);
					case fb::tok::amp:
						return this->_prefixed(fb::node_kind::metadata, t, false);
					case fb::tok::tilde:
						return this->_prefixed(fb::node_kind::force, t, false);
					case fb::tok::splat:
						return this->_prefixed(fb::node_kind::splat, t, true);
					case fb::tok::backtick:
						return this->_interpolated(fb::node_kind::command, fb::tok::backtick, fb::tok::backtick, fb::tok::command, fb::node_kind::cmdtext, "command");
					case fb::tok::str_beg:
						return this->_interpolated(fb::node_kind::string, fb::tok::str_beg, fb::tok::str_end, fb::tok::strtext, fb::node_kind::strtext, "string");
					case fb::tok::id:
						if (t->text == "match" && this->_match(fb::tok::lbrace, _cur + 1U))
							return this->_match_literal();
						return this->_leaf(fb::node_kind::id, t);
					case fb::tok::integer:
						return this->_leaf(fb::node_kind::integer, t);
					case fb::tok::floating:
						return this->_leaf(fb::node_kind::floating, t);
					case fb::tok::boolean:
						return this->_leaf(fb::node_kind::boolean, t);
					case fb::tok::nil:
						return this->_leaf(fb::node_kind::nil, t);
					case fb::tok::string:
						return this->_leaf(fb::node_kind::string, t);
					case fb::tok::path:
						return this->_leaf(fb::node_kind::path, t);
					case fb::tok::lbrace:
						return this->_map_literal();
					case fb::tok::lparen:
						return this->_parenthesized(t);
					default:
						return this->_fail(_memory.format("unexpected token '{}' at {}:{}", t->text, t->start.line, t->start.col), t->start);
				}
			}

			/* A pattern: _, a literal, a name (name: Type), or a tuple of patterns. */
			[[nodiscard]] auto _pattern(void) noexcept -> parsed {

				this->_skip_newlines();
				fb::token* const t = this->_current();
				if (t == nullptr || t->kind == fb::tok::eof)
					return this->_fail_here("unexpected end of input in a match pattern");
				switch (t->kind) {
					case fb::tok::id: {
						this->_advance();
						if (t->text == "_")
							return this->_node(fb::node_kind::pwild, t);
						fb::node* const bind = this->_node(fb::node_kind::pbind, t);
						if (this->_accept(fb::tok::colon)) {
							this->_skip_newlines();
							auto annotation = this->_type_expr();
							if (!annotation)
								return annotation;
							this->_push(bind, *annotation);
						}
						return bind;
					}
					case fb::tok::integer:
					case fb::tok::floating:
					case fb::tok::boolean:
					case fb::tok::nil:
					case fb::tok::string:
					case fb::tok::path: {
						fb::node* const literal = this->_node(fb::node_kind::plit, t);
						auto value = this->_atom();
						if (!value)
							return value;
						this->_push(literal, *value);
						return literal;
					}
					case fb::tok::str_beg: {
						fb::node* const literal = this->_node(fb::node_kind::plit, t);
						auto value = this->_atom();
						if (!value)
							return value;
						if ((*value)->size() != 0U)
							return this->_fail(_memory.format("a match pattern string cannot interpolate at {}:{}", t->start.line, t->start.col), t->start);
						this->_push(literal, *value);
						return literal;
					}
					case fb::tok::minus: {
						fb::node* const literal = this->_node(fb::node_kind::plit, t);
						auto value = this->_unary();
						if (!value)
							return value;
						const fb::node* const operand = (*value)->kid(0U);
						if (operand == nullptr || (operand->kind != fb::node_kind::integer && operand->kind != fb::node_kind::floating))
							return this->_fail(_memory.format("expected a number after '-' in a match pattern at {}:{}", t->start.line, t->start.col), t->start);
						this->_push(literal, *value);
						return literal;
					}
					case fb::tok::lparen: {
						this->_advance();
						this->_skip_newlines();
						fb::node* const tuple = this->_node(fb::node_kind::ptuple, t);
						if (this->_peek(fb::tok::rparen)) {
							this->_advance();
							fb::node* const literal = this->_node(fb::node_kind::plit, t);
							this->_push(literal, this->_node(fb::node_kind::nil, t));
							return literal;
						}
						do {
							auto element = this->_pattern();
							if (!element)
								return element;
							this->_push(tuple, *element);
							this->_skip_newlines();
						} while (this->_accept(fb::tok::comma));
						auto close = this->_expect(fb::tok::rparen);
						if (!close)
							return close.propagate();
						return tuple->size() == 1U ? tuple->kid(0U) : tuple;
					}
					default:
						return this->_fail(_memory.format("unexpected token '{}' in a match pattern at {}:{}", t->text, t->start.line, t->start.col), t->start);
				}
			}

			/* match { pattern [when cond] -> body, ... }: arms separated by commas or newlines. */
			[[nodiscard]] auto _match_literal(void) noexcept -> parsed {

				fb::node* const n = this->_node(fb::node_kind::match_, this->_current());
				this->_advance();
				this->_advance();
				for (;;) {
					this->_skip_newlines();
					if (this->_accept(fb::tok::rbrace))
						break;
					fb::node* const arm = this->_node(fb::node_kind::arm);
					auto pattern = this->_pattern();
					if (!pattern)
						return pattern;
					this->_push(arm, *pattern);
					fb::node* condition = nullptr;
					if (this->_significant() == fb::tok::when) {
						this->_skip_newlines();
						this->_advance();
						this->_skip_newlines();
						const bool saved = ca::exchange(_no_tuple, true);
						auto c = this->_logical();
						_no_tuple = saved;
						if (!c)
							return c;
						condition = *c;
					}
					this->_push(arm, condition);
					this->_skip_newlines();
					auto arrow = this->_expect(fb::tok::arrow);
					if (!arrow)
						return arrow.propagate();
					this->_skip_newlines();
					auto body = this->_expr_no_tuple();
					if (!body)
						return body;
					this->_push(arm, *body);
					this->_push(n, arm);
					if (this->_significant() == fb::tok::comma) {
						this->_skip_newlines();
						this->_advance();
						continue;
					}
					this->_skip_newlines();
					if (this->_accept(fb::tok::rbrace))
						break;
					// Inside braces the lexer keeps no newline tokens: a new line shows in the positions.
					const fb::token* const previous = this->_token(_cur - 1U);
					const fb::token* const next = this->_current();
					if (previous == nullptr || next == nullptr || (previous->kind != fb::tok::newline && next->start.line <= previous->end.line))
						return this->_fail_here("expected ',', a newline or '}' after a match arm");
				}
				if (n->size() == 0U)
					return this->_fail(_memory.format("match needs at least one arm at {}:{}", n->tok->start.line, n->tok->start.col), n->tok->start);
				return n;
			}

			[[nodiscard]] auto _leaf(const fb::node_kind kind, fb::token* const t) noexcept -> parsed {

				this->_advance();
				return this->_node(kind, t);
			}

			[[nodiscard]] auto _interpolated(const fb::node_kind kind, const fb::tok open, const fb::tok close, const fb::tok text, const fb::node_kind text_kind, const ca::sv::str what) noexcept -> parsed {

				auto begin = this->_expect(open);
				if (!begin)
					return begin.propagate();
				fb::node* const n = this->_node(kind);
				while (!this->_at_eof() && !this->_peek(close)) {
					if (this->_peek(text)) {
						this->_push(n, this->_node(text_kind, this->_current()));
						this->_advance();
					} else if (this->_accept(fb::tok::lbrace)) {
						auto part = this->_expression();
						if (!part)
							return part;
						this->_push(n, *part);
						auto brace = this->_expect(fb::tok::rbrace);
						if (!brace)
							return brace.propagate();
					} else {
						const fb::token* const t = this->_current();
						return this->_fail(_memory.format("unexpected token in {} at {}:{}", what, t->start.line, t->start.col), t->start);
					}
				}
				auto end = this->_expect(close);
				if (!end)
					return end.propagate();
				return n;
			}

			[[nodiscard]] auto _call(fb::node* const callee) noexcept -> parsed {

				auto open = this->_expect(fb::tok::lparen);
				if (!open)
					return open.propagate();
				fb::node* const n = this->_node(fb::node_kind::call);
				this->_push(n, callee);
				if (!this->_peek(fb::tok::rparen)) {
					do {
						auto argument = this->_expr_no_tuple();
						if (!argument)
							return argument;
						this->_push(n, *argument);
					} while (this->_accept(fb::tok::comma));
				}
				auto close = this->_expect(fb::tok::rparen);
				if (!close)
					return close.propagate();
				return n;
			}

			[[nodiscard]] auto _field(fb::node* const base) noexcept -> parsed {

				auto dot = this->_expect(fb::tok::dot);
				if (!dot)
					return dot.propagate();
				fb::node* const n = this->_node(fb::node_kind::field);
				if (this->_accept(fb::tok::lbrace)) {
					this->_push(n, base);
					auto index = this->_expression();
					if (!index)
						return index;
					this->_push(n, *index);
					auto close = this->_expect(fb::tok::rbrace);
					if (!close)
						return close.propagate();
					return n;
				}
				if (!this->_peek(fb::tok::id) && !this->_peek(fb::tok::integer))
					return this->_fail_here("expected field name or index after '.'");
				n->tok = this->_current();
				this->_advance();
				this->_push(n, base);
				return n;
			}

			[[nodiscard]] auto _constructor(fb::node* const base) noexcept -> parsed {

				fb::node* const n = this->_node(fb::node_kind::construct, base->tok);
				auto open = this->_expect(fb::tok::lt);
				if (!open)
					return open.propagate();
				if (!this->_peek(fb::tok::gt)) {
					do {
						fb::node* const field = this->_node(fb::node_kind::cfield);
						if (!this->_accept(fb::tok::star)) {
							auto name = this->_expect(fb::tok::id);
							if (!name)
								return name.propagate();
							field->tok = *name;
							auto assign = this->_expect(fb::tok::assign);
							if (!assign)
								return assign.propagate();
						}
						auto value = this->_postfix();
						if (!value)
							return value;
						this->_push(field, *value);
						this->_push(n, field);
					} while (this->_accept(fb::tok::comma));
				}
				auto close = this->_expect_gt();
				if (!close)
					return close.propagate();
				return n;
			}

			[[nodiscard]] auto _generic(const fb::node_kind kind, const bool splats, const bool trailing_comma) noexcept -> parsed {

				fb::node* const n = this->_node(kind);
				auto open = this->_expect(fb::tok::lt);
				if (!open)
					return open.propagate();
				if (!this->_peek(fb::tok::gt)) {
					do {
						this->_skip_newlines();
						if (trailing_comma && this->_peek(fb::tok::gt))
							break;
						if (splats && this->_accept(fb::tok::splat)) {
							fb::node* const spread = this->_node(fb::node_kind::splat);
							auto value = this->_postfix();
							if (!value)
								return value;
							this->_push(spread, *value);
							this->_push(n, spread);
						} else {
							auto value = this->_postfix();
							if (!value)
								return value;
							this->_push(n, *value);
						}
						this->_skip_newlines();
					} while (this->_accept(fb::tok::comma));
				}
				auto close = this->_expect_gt();
				if (!close)
					return close.propagate();
				return n;
			}

			[[nodiscard]] auto _parser_constructor(void) noexcept -> parsed {

				fb::node* const n = this->_node(fb::node_kind::parser);
				auto open = this->_expect(fb::tok::lt);
				if (!open)
					return open.propagate();
				this->_skip_newlines();
				auto schema = this->_postfix();
				if (!schema)
					return schema;
				this->_push(n, *schema);
				this->_skip_newlines();
				auto close = this->_expect_gt();
				if (!close)
					return close.propagate();
				return n;
			}

			[[nodiscard]] auto _postfix(void) noexcept -> parsed {

				auto atom = this->_atom();
				if (!atom)
					return atom;
				fb::node* base = *atom;
				for (;;) {
					const bool generic = base->kind == fb::node_kind::id && this->_peek(fb::tok::lt);
					parsed next{nullptr};
					if (this->_peek(fb::tok::lparen))
						next = this->_call(base);
					else if (this->_peek(fb::tok::dot))
						next = this->_field(base);
					else if (generic && base->tok->text == "Tuple")
						next = this->_generic(fb::node_kind::tuple, true, false);
					else if (generic && base->tok->text == "List")
						next = this->_generic(fb::node_kind::listlit, true, false);
					else if (generic && base->tok->text == "Map")
						next = this->_generic(fb::node_kind::maplit, false, true);
					else if (generic && base->tok->text == "Parser")
						next = this->_parser_constructor();
					else if (base->kind == fb::node_kind::id && this->_constructor_ahead())
						next = this->_constructor(base);
					else if (this->_significant() == fb::tok::morph) {
						this->_skip_newlines();
						fb::node* const m = this->_node(fb::node_kind::morphism, this->_current());
						this->_advance();
						this->_skip_newlines();
						this->_push(m, base);
						auto right = this->_atom();
						if (!right)
							return right;
						this->_push(m, *right);
						next = m;
					} else
						return base;
					if (!next)
						return next;
					base = *next;
				}
			}

			[[nodiscard]] auto _unary(void) noexcept -> parsed {

				if (this->_significant() != fb::tok::minus)
					return this->_postfix();
				return this->_prefix_operator(fb::node_kind::neg, &self::_unary);
			}

			[[nodiscard]] auto _prefix_operator(const fb::node_kind kind, auto (self::*operand)(void) noexcept -> parsed) noexcept -> parsed {

				this->_skip_newlines();
				fb::node* const n = this->_node(kind, this->_current());
				this->_advance();
				this->_skip_newlines();
				auto value = (this->*operand)();
				if (!value)
					return value;
				this->_push(n, *value);
				return n;
			}

			template <typename Accepts>
			[[nodiscard]] auto _binary(const fb::node_kind kind, auto (self::*operand)(void) noexcept -> parsed, const Accepts accepts) noexcept -> parsed {

				auto first = (this->*operand)();
				if (!first)
					return first;
				fb::node* left = *first;
				while (accepts(this->_significant())) {
					this->_skip_newlines();
					fb::node* const n = this->_node(kind, this->_current());
					this->_advance();
					this->_skip_newlines();
					this->_push(n, left);
					auto right = (this->*operand)();
					if (!right)
						return right;
					this->_push(n, *right);
					left = n;
				}
				return left;
			}

			[[nodiscard]] auto _multiplicative(void) noexcept -> parsed {
				return this->_binary(fb::node_kind::arith, &self::_unary, [](const fb::tok k) noexcept {
					return k == fb::tok::star || k == fb::tok::slash || k == fb::tok::percent;
				});
			}

			[[nodiscard]] auto _additive(void) noexcept -> parsed {
				return this->_binary(fb::node_kind::arith, &self::_multiplicative, [](const fb::tok k) noexcept {
					return k == fb::tok::plus || k == fb::tok::minus;
				});
			}

			[[nodiscard]] auto _comparison(void) noexcept -> parsed {
				return this->_binary(fb::node_kind::compare, &self::_additive, [](const fb::tok k) noexcept {
					return k == fb::tok::lt || k == fb::tok::gt || k == fb::tok::le || k == fb::tok::ge || k == fb::tok::eq || k == fb::tok::ne;
				});
			}

			[[nodiscard]] auto _not(void) noexcept -> parsed {

				if (this->_significant() != fb::tok::not_)
					return this->_comparison();
				return this->_prefix_operator(fb::node_kind::not_, &self::_not);
			}

			[[nodiscard]] auto _and(void) noexcept -> parsed {
				return this->_binary(fb::node_kind::logical, &self::_not, [](const fb::tok k) noexcept {
					return k == fb::tok::and_;
				});
			}

			[[nodiscard]] auto _logical(void) noexcept -> parsed {
				return this->_binary(fb::node_kind::logical, &self::_and, [](const fb::tok k) noexcept {
					return k == fb::tok::or_ || k == fb::tok::oror;
				});
			}

			[[nodiscard]] auto _guarded(void) noexcept -> parsed {

				auto body = this->_logical();
				if (!body)
					return body;
				fb::node* where = nullptr;
				fb::node* when = nullptr;
				if (this->_significant() == fb::tok::where) {
					this->_skip_newlines();
					this->_advance();
					this->_skip_newlines();
					where = this->_node(fb::node_kind::where);
					do {
						this->_skip_newlines();
						auto name = this->_expect(fb::tok::id);
						if (!name)
							return name.propagate();
						this->_skip_newlines();
						auto assign = this->_expect(fb::tok::assign);
						if (!assign)
							return assign.propagate();
						this->_skip_newlines();
						fb::node* const binding = this->_node(fb::node_kind::binding, *name);
						auto value = this->_logical();
						if (!value)
							return value;
						this->_push(binding, *value);
						this->_push(where, binding);
					} while (this->_continues_with(fb::tok::comma));
				}
				if (this->_significant() == fb::tok::when) {
					this->_skip_newlines();
					this->_advance();
					this->_skip_newlines();
					when = this->_node(fb::node_kind::when);
					auto condition = this->_expression();
					if (!condition)
						return condition;
					this->_push(when, *condition);
				}
				fb::node* otherwise = nullptr;
				if (when != nullptr && this->_word_at_significant("else")) {
					this->_skip_newlines();
					otherwise = this->_node(fb::node_kind::else_, this->_current());
					this->_advance();
					this->_skip_newlines();
					auto value = _no_tuple ? this->_expr_no_tuple() : this->_expression();
					if (!value)
						return value;
					this->_push(otherwise, *value);
				}
				if (where == nullptr && when == nullptr)
					return body;
				fb::node* const guard = this->_node(fb::node_kind::guard);
				this->_push(guard, *body);
				this->_push(guard, where);
				this->_push(guard, when);
				this->_push(guard, otherwise);
				return guard;
			}

			[[nodiscard]] auto _chain(const fb::node_kind kind, const fb::tok op, auto (self::*first)(void) noexcept -> parsed, auto (self::*rest)(void) noexcept -> parsed) noexcept -> parsed {

				auto left = (this->*first)();
				if (!left || this->_significant() != op)
					return left;
				fb::node* const n = this->_node(kind);
				this->_push(n, *left);
				while (this->_continues_with(op)) {
					this->_skip_newlines();
					auto right = (this->*rest)();
					if (!right)
						return right;
					this->_push(n, *right);
				}
				return n;
			}

			[[nodiscard]] auto _pipe(void) noexcept -> parsed {
				return this->_chain(fb::node_kind::pipe, fb::tok::pipe, &self::_guarded, &self::_pipe_rhs);
			}

			[[nodiscard]] auto _sequence(void) noexcept -> parsed {
				return this->_chain(fb::node_kind::seq, fb::tok::seq, &self::_pipe, &self::_pipe);
			}

			[[nodiscard]] auto _type_atom(void) noexcept -> parsed {

				auto name = this->_expect(fb::tok::id);
				if (!name)
					return name.propagate();
				fb::node* const n = this->_node(fb::node_kind::typeexpr, *name);
				if (this->_accept(fb::tok::lt)) {
					do {
						this->_skip_newlines();
						auto argument = this->_type_expr();
						if (!argument)
							return argument;
						this->_push(n, *argument);
						this->_skip_newlines();
					} while (this->_accept(fb::tok::comma));
					auto close = this->_expect(fb::tok::gt);
					if (!close)
						return close.propagate();
				}
				return n;
			}

			[[nodiscard]] auto _type_expr(void) noexcept -> parsed {

				auto first = this->_type_atom();
				if (!first || !this->_peek(fb::tok::bar))
					return first;
				fb::node* const u = this->_node(fb::node_kind::typeunion, (*first)->tok);
				this->_push(u, *first);
				while (this->_accept(fb::tok::bar)) {
					this->_skip_newlines();
					auto member = this->_type_atom();
					if (!member)
						return member;
					this->_push(u, *member);
				}
				return u;
			}

			[[nodiscard]] auto _parameter(const bool allow_type) noexcept -> parsed {

				fb::token* const t = this->_current();
				if (t->kind == fb::tok::lparen) {
					this->_advance();
					fb::node* const pattern = this->_node(fb::node_kind::tuple);
					do {
						this->_skip_newlines();
						auto name = this->_expect(fb::tok::id);
						if (!name)
							return name.propagate();
						this->_push(pattern, this->_node(fb::node_kind::id, *name));
						this->_skip_newlines();
					} while (this->_accept(fb::tok::comma));
					this->_skip_newlines();
					auto close = this->_expect(fb::tok::rparen);
					if (!close)
						return close.propagate();
					return pattern;
				}
				if (t->kind == fb::tok::at)
					return this->_capture(t);
				if (t->kind == fb::tok::dot)
					return this->_projection("expected name or index after '.' in parameter", t);
				auto name = this->_expect(fb::tok::id);
				if (!name)
					return name.propagate();
				fb::node* const n = this->_node(fb::node_kind::id, *name);
				if (this->_peek(fb::tok::colon)) {
					const fb::token* const id = *name;
					if (!allow_type)
						return this->_fail(_memory.format("parameter type annotations require parentheses: ({}: Type) -> ... at {}:{}",
							id->text, id->start.line, id->start.col), id->start);
					this->_advance();
					this->_skip_newlines();
					auto type = this->_type_expr();
					if (!type)
						return type;
					this->_push(n, *type);
				}
				return n;
			}

			[[nodiscard]] auto _lambda(void) noexcept -> parsed {

				fb::node* const params = this->_node(fb::node_kind::params);
				do {
					this->_skip_newlines();
					auto param = this->_parameter(false);
					if (!param)
						return param;
					this->_push(params, *param);
				} while (this->_continues_with(fb::tok::comma));
				this->_skip_newlines();
				auto arrow = this->_expect(fb::tok::arrow);
				if (!arrow)
					return arrow.propagate();
				this->_skip_newlines();
				return this->_lambda_of(params);
			}

			[[nodiscard]] auto _expr_no_tuple(void) noexcept -> parsed {

				const bool saved = ca::exchange(_no_tuple, true);
				auto r = this->_lambda_ahead(false) ? this->_lambda() : this->_sequence();
				_no_tuple = saved;
				return r;
			}

			[[nodiscard]] auto _type_definition(void) noexcept -> parsed {

				this->_advance();
				auto name = this->_expect(fb::tok::id);
				if (!name)
					return name.propagate();
				auto open = this->_expect(fb::tok::lparen);
				if (!open)
					return open.propagate();
				fb::node* const params = this->_node(fb::node_kind::params);
				this->_skip_newlines();
				if (!this->_peek(fb::tok::rparen)) {
					do {
						this->_skip_newlines();
						auto param = this->_parameter(true);
						if (!param)
							return param;
						this->_push(params, *param);
					} while (this->_continues_with(fb::tok::comma));
					this->_skip_newlines();
				}
				auto close = this->_expect(fb::tok::rparen);
				if (!close)
					return close.propagate();
				this->_skip_newlines();
				auto assign = this->_expect(fb::tok::assign);
				if (!assign)
					return assign.propagate();
				this->_skip_newlines();
				fb::node* const object = this->_node(fb::node_kind::objnew, *name);
				auto body = this->_expression();
				if (!body)
					return body;
				this->_push(object, *body);
				fb::node* const lambda = this->_node(fb::node_kind::lambda);
				this->_push(lambda, params);
				this->_push(lambda, object);
				fb::node* const definition = this->_node(fb::node_kind::assign, *name);
				this->_push(definition, lambda);
				return definition;
			}

			[[nodiscard]] auto _method_definition(void) noexcept -> parsed {

				this->_advance();
				fb::token* const class_name = this->_current();
				this->_advance();
				this->_advance();
				const bool wildcard = this->_peek(fb::tok::star);
				fb::token* const method_name = wildcard ? nullptr : this->_current();
				this->_advance();
				this->_skip_newlines();
				auto assign = this->_expect(fb::tok::assign);
				if (!assign)
					return assign.propagate();
				this->_skip_newlines();
				const bool copy = this->_match(fb::tok::id, _cur) && this->_match(fb::tok::dot, _cur + 1U)
					&& (this->_match(fb::tok::id, _cur + 2U) || this->_match(fb::tok::star, _cur + 2U))
					&& (this->_match(fb::tok::newline, _cur + 3U) || this->_match(fb::tok::eof, _cur + 3U));
				if (wildcard && !copy)
					return this->_fail(_memory.format("method {}.* = ... needs a wildcard source (Old.*) at {}:{}",
						class_name->text, class_name->start.line, class_name->start.col), class_name->start);
				if (!copy) {
					fb::node* const definition = this->_node(fb::node_kind::methoddef, method_name);
					this->_push(definition, this->_node(fb::node_kind::id, class_name));
					auto body = this->_expression();
					if (!body)
						return body;
					this->_push(definition, *body);
					return definition;
				}
				fb::token* const source = this->_current();
				this->_advance();
				this->_advance();
				const bool source_wildcard = this->_peek(fb::tok::star);
				fb::token* const source_method = source_wildcard ? nullptr : this->_current();
				this->_advance();
				if (wildcard != source_wildcard)
					return this->_fail(_memory.format("method {}.{} = {}.{}: wildcard must be used on both sides at {}:{}",
						class_name->text, wildcard ? "*" : "MNAME", source->text, source_wildcard ? "*" : "MNAME",
						class_name->start.line, class_name->start.col), class_name->start);
				fb::node* const copied = this->_node(fb::node_kind::methodcopy, method_name);
				this->_push(copied, this->_node(fb::node_kind::id, class_name));
				this->_push(copied, this->_node(fb::node_kind::id, source));
				if (source_method != nullptr)
					this->_push(copied, this->_node(fb::node_kind::id, source_method));
				return copied;
			}

			[[nodiscard]] auto _definition(const fb::node_kind kind) noexcept -> parsed {

				fb::node* const n = this->_node(kind, this->_current());
				this->_advance();
				this->_skip_newlines();
				this->_advance();
				this->_skip_newlines();
				auto value = this->_expression();
				if (!value)
					return value;
				this->_push(n, *value);
				return n;
			}

			[[nodiscard]] auto _statement_tail(fb::node* const first) noexcept -> parsed {

				if (this->_significant() == fb::tok::write) {
					this->_skip_newlines();
					fb::node* const store = this->_node(fb::node_kind::store, this->_current());
					this->_advance();
					this->_skip_newlines();
					this->_push(store, first);
					auto value = this->_expression();
					if (!value)
						return value;
					this->_push(store, *value);
					return store;
				}
				if (this->_significant() != fb::tok::comma)
					return first;
				fb::node* const tuple = this->_node(fb::node_kind::tuple);
				this->_push(tuple, first);
				while (this->_continues_with(fb::tok::comma)) {
					this->_skip_newlines();
					auto element = this->_expr_no_tuple();
					if (!element)
						return element;
					this->_push(tuple, *element);
				}
				return tuple;
			}

			[[nodiscard]] auto _expression_body(void) noexcept -> parsed {

				if (this->_peek_word("type") && this->_match(fb::tok::id, _cur + 1U) && this->_match(fb::tok::lparen, _cur + 2U))
					return this->_type_definition();
				if (this->_peek_word("method") && this->_match(fb::tok::id, _cur + 1U) && this->_match(fb::tok::dot, _cur + 2U)
					&& (this->_match(fb::tok::id, _cur + 3U) || this->_match(fb::tok::star, _cur + 3U))
					&& this->_match(fb::tok::assign, _cur + 4U))
					return this->_method_definition();
				if (this->_peek(fb::tok::id) && this->_significant_from(_cur + 1U) == fb::tok::assign)
					return this->_definition(fb::node_kind::assign);
				if (this->_peek(fb::tok::id) && this->_significant_from(_cur + 1U) == fb::tok::tilde_assign)
					return this->_definition(fb::node_kind::eager);
				auto first = this->_lambda_ahead(true) ? this->_lambda() : this->_expr_no_tuple();
				if (!first)
					return first;
				return this->_statement_tail(*first);
			}

			[[nodiscard]] auto _expression(void) noexcept -> parsed {

				const bool saved = ca::exchange(_no_tuple, false);
				auto r = this->_expression_body();
				_no_tuple = saved;
				return r;
			}

		public:

			parser(fb::memory& memory, fb::list<fb::token>& tokens) noexcept
			: _memory{memory}, _tokens{tokens}, _cur{0U}, _no_tuple{false}, _nodes{0U} {
			}

			[[nodiscard]] auto program(void) noexcept -> parsed {

				fb::node* const program = this->_node(fb::node_kind::program);
				this->_skip_newlines();
				while (!this->_at_eof()) {
					auto statement = this->_expression();
					if (!statement)
						return statement;
					this->_push(program, *statement);
					if (!this->_at_eof() && !this->_peek(fb::tok::newline)) {
						const fb::token* const t = this->_current();
						return this->_fail(_memory.format("expected newline after statement at {}:{}", t->start.line, t->start.col), t->start);
					}
					this->_skip_newlines();
				}
				return program;
			}

	}; // class parser

} // namespace fb::impl

auto fb::parse(fb::memory& memory, fb::list<fb::token>& tokens) noexcept -> ca::result<fb::node*, fb::diagnostic> {

	fb::impl::parser parser{memory, tokens};
	return parser.program();
}

auto fb::parse_source(fb::memory& memory, const ca::sv::str source) noexcept -> ca::result<fb::node*, fb::diagnostic> {

	auto tokens = fb::lex(memory, source);
	if (!tokens)
		return tokens.propagate();
	fb::list<fb::token>* const kept = memory.make<fb::list<fb::token>>(ca::move(*tokens));
	return fb::parse(memory, *kept);
}

auto fb::span_of(const fb::node* const n) noexcept -> fb::span {

	fb::span s{{0, 0}, {0, 0}};
	if (n == nullptr)
		return s;
	const auto before = [](const fb::position a, const fb::position b) noexcept {
		return a.line < b.line || (a.line == b.line && a.col < b.col);
	};
	bool have = false;
	if (n->tok != nullptr && n->tok->start.line != 0) {
		s = fb::span{n->tok->start, n->tok->end};
		have = true;
	}
	for (const fb::node* const kid : n->kids) {
		const fb::span k = fb::span_of(kid);
		if (k.start.line == 0)
			continue;
		if (!have || before(k.start, s.start))
			s.start = k.start;
		if (!have || before(s.end, k.end))
			s.end = k.end;
		have = true;
	}
	return s;
}

auto fb::node_name(const fb::node_kind kind) noexcept -> ca::sv::str {

	constexpr ca::sv::str names[] = {
		"program", "assign", "eager", "store", "force", "tuple", "listlit", "maplit", "parser", "lambda",
		"params", "seq", "pipe", "guard", "where", "binding", "when", "compare", "arith", "neg", "logical",
		"not", "morphism", "objnew", "methoddef", "methodcopy", "call", "field", "construct", "cfield",
		"splat", "metadata", "capture", "projection", "command", "cmdtext", "id", "int", "float", "bool",
		"nil", "string", "strtext", "path", "typeexpr", "typeunion", "else", "match", "arm", "pwild",
		"pbind", "plit", "ptuple",
	};
	return names[static_cast<ca::usz>(kind)];
}
