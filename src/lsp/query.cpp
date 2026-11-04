#include "lsp/query.hpp"

namespace fb::lsp::impl {

	[[nodiscard]] static auto before(const fb::position a, const fb::position b) noexcept -> bool {
		return a.line < b.line || (a.line == b.line && a.col < b.col);
	}

	[[nodiscard]] static auto token_holds(const fb::token* const t, const fb::position at) noexcept -> bool {
		return t != nullptr && t->start.line != 0 && fb::lsp::contains(fb::span{t->start, t->end}, at);
	}

	[[nodiscard]] static auto names_something(const fb::node* const n) noexcept -> bool {

		switch (n->kind) {
			case fb::node_kind::id:
			case fb::node_kind::projection:
			case fb::node_kind::field:
			case fb::node_kind::assign:
			case fb::node_kind::eager:
			case fb::node_kind::binding:
			case fb::node_kind::pbind:
			case fb::node_kind::methoddef:
			case fb::node_kind::typeexpr:
				return n->tok != nullptr;
			default:
				return false;
		}
	}

	template <typename F>
	static auto walk(const fb::node* const n, F& visit) noexcept -> void {

		if (n == nullptr)
			return;
		visit(n);
		for (const fb::node* const kid : n->kids)
			fb::lsp::impl::walk(kid, visit);
	}

	/* The names a parameter list binds. */
	static auto parameters(fb::memory& memory, const fb::node* const params, fb::list<fb::lsp::binder>& out) noexcept -> void {

		if (params == nullptr)
			return;
		for (const fb::node* const p : params->kids) {
			switch (p->kind) {
				case fb::node_kind::id:
				case fb::node_kind::projection:
					if (p->tok != nullptr)
						memory.push(out, fb::lsp::binder{p->tok->text, p});
					break;
				case fb::node_kind::capture:
					if (p->size() != 0U && p->kid(0U)->tok != nullptr)
						memory.push(out, fb::lsp::binder{p->kid(0U)->tok->text, p->kid(0U)});
					break;
				case fb::node_kind::tuple:
					for (const fb::node* const sub : p->kids)
						if (sub->tok != nullptr)
							memory.push(out, fb::lsp::binder{sub->tok->text, sub});
					break;
				default:
					break;
			}
		}
	}

	/* The names a match pattern binds. */
	static auto pattern(fb::memory& memory, const fb::node* const p, fb::list<fb::lsp::binder>& out) noexcept -> void {

		if (p == nullptr)
			return;
		if (p->kind == fb::node_kind::pbind && p->tok != nullptr) {
			memory.push(out, fb::lsp::binder{p->tok->text, p});
			return;
		}
		if (p->kind == fb::node_kind::ptuple)
			for (const fb::node* const sub : p->kids)
				fb::lsp::impl::pattern(memory, sub, out);
	}

	[[nodiscard]] static auto word(const char c) noexcept -> bool {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}

	/* For each byte of text, whether it is inside a string, a command or a comment: their
	   brackets and commas are not syntax. A {expr} inside a string counts as string. */
	[[nodiscard]] static auto masked(fb::memory& memory, const ca::sv::str text) noexcept -> bool* {

		bool* const mask = static_cast<bool*>(memory.bytes(text.size() + 1U, 1U));
		char quote = '\0';
		bool comment = false;
		for (ca::usz i = 0U; i < text.size(); ++i) {
			const char c = text.data()[i];
			if (c == '\n') {
				comment = false;
				if (quote != '`')
					quote = '\0';
				mask[i] = false;
				continue;
			}
			if (comment) {
				mask[i] = true;
				continue;
			}
			if (quote != '\0') {
				mask[i] = true;
				if (c == '\\' && i + 1U < text.size() && text.data()[i + 1U] != '\n') {
					mask[++i] = true;
					continue;
				}
				if (c == quote)
					quote = '\0';
				continue;
			}
			if (c == '\'' || c == '"' || c == '`') {
				quote = c;
				mask[i] = true;
				continue;
			}
			if (c == '#') {
				comment = true;
				mask[i] = true;
				continue;
			}
			mask[i] = false;
		}
		return mask;
	}

	/* How far back a call is looked for. */
	inline constexpr ca::usz call_window = 8192U;

} // namespace fb::lsp::impl

auto fb::lsp::contains(const fb::span s, const fb::position at) noexcept -> bool {
	return s.start.line != 0 && !fb::lsp::impl::before(at, s.start) && !fb::lsp::impl::before(s.end, at);
}

auto fb::lsp::locate(fb::memory& memory, const fb::node* const program, const fb::position at) noexcept -> fb::list<const fb::node*> {

	fb::list<const fb::node*> path = memory.list<const fb::node*>();
	if (program == nullptr)
		return path;
	memory.push(path, program);
	const fb::node* n = program;
	for (bool deeper = true; deeper;) {
		deeper = false;
		for (const fb::node* const kid : n->kids)
			if (kid != nullptr && fb::lsp::contains(fb::span_of(kid), at)) {
				memory.push(path, kid);
				n = kid;
				deeper = true;
				break;
			}
	}
	return path;
}

auto fb::lsp::name_at(const ca::borrowed_slice<const fb::node* const> path, const fb::position at) noexcept -> const fb::node* {

	for (ca::usz i = path.size(); i > 0U; --i) {
		const fb::node* const n = path.data()[i - 1U];
		if (fb::lsp::impl::names_something(n) && fb::lsp::impl::token_holds(n->tok, at))
			return n;
	}
	return nullptr;
}

auto fb::lsp::name_of(fb::memory& memory, const fb::node* const n) noexcept -> ca::sv::str {

	if (n == nullptr || n->tok == nullptr)
		return {};
	if (n->kind == fb::node_kind::field)
		return memory.format(".{}", n->tok->text);
	if (n->kind == fb::node_kind::methoddef && n->kid(0U) != nullptr && n->kid(0U)->tok != nullptr)
		return memory.format("{}.{}", n->kid(0U)->tok->text, n->tok->text);
	return n->tok->text;
}

auto fb::lsp::site_span(const fb::node* const site) noexcept -> fb::span {

	if (site == nullptr)
		return fb::span{{0, 0}, {0, 0}};
	if (site->tok != nullptr && site->tok->start.line != 0)
		return fb::span{site->tok->start, site->tok->end};
	return fb::span_of(site);
}

auto fb::lsp::uses_of(fb::memory& memory, const fb::node* const program, const fb::check::report& report, const fb::node* const site) noexcept
	-> fb::list<const fb::node*> {

	fb::list<const fb::node*> uses = memory.list<const fb::node*>();
	if (site == nullptr)
		return uses;
	memory.push(uses, site);
	const auto visit = [&](const fb::node* const n) noexcept {
		if (n != site && report.target_of(n) == site)
			memory.push(uses, n);
	};
	fb::lsp::impl::walk(program, visit);
	return uses;
}

auto fb::lsp::binders(fb::memory& memory, const ca::borrowed_slice<const fb::node* const> path, const fb::position at) noexcept -> fb::list<fb::lsp::binder> {

	fb::list<fb::lsp::binder> out = memory.list<fb::lsp::binder>();
	for (ca::usz i = path.size(); i > 0U; --i) {
		const fb::node* const n = path.data()[i - 1U];
		const fb::node* const inside = i < path.size() ? path.data()[i] : nullptr;
		switch (n->kind) {
			case fb::node_kind::lambda: {
				const fb::node* const params = n->kid(0U);
				const fb::span declared = fb::span_of(params);
				if (inside == n->kid(1U) || declared.start.line == 0 || fb::lsp::impl::before(declared.end, at))
					fb::lsp::impl::parameters(memory, params, out);
				break;
			}
			case fb::node_kind::guard:
				if (const fb::node* const where = n->kid(1U))
					for (const fb::node* const binding : where->kids)
						if (binding->tok != nullptr && binding != inside)
							memory.push(out, fb::lsp::binder{binding->tok->text, binding});
				break;
			case fb::node_kind::arm: {
				const fb::span pattern = fb::span_of(n->kid(0U));
				if (inside != n->kid(0U) && (pattern.start.line == 0 || fb::lsp::impl::before(pattern.end, at)))
					fb::lsp::impl::pattern(memory, n->kid(0U), out);
				break;
			}
			case fb::node_kind::seq:
				for (const fb::node* const step : n->kids) {
					if (step == inside)
						break;
					if (step->kind == fb::node_kind::eager && step->tok != nullptr)
						memory.push(out, fb::lsp::binder{step->tok->text, step});
				}
				break;
			default:
				break;
		}
	}
	return out;
}

auto fb::lsp::field_at(const fb::node* const program, const fb::position at) noexcept -> const fb::node* {

	const fb::node* found = nullptr;
	const auto visit = [&](const fb::node* const n) noexcept {
		if (found == nullptr && n->kind == fb::node_kind::field && n->tok != nullptr && n->tok->start.line == at.line && n->tok->start.col == at.col)
			found = n;
	};
	fb::lsp::impl::walk(program, visit);
	return found;
}

auto fb::lsp::call_at(fb::memory& memory, const ca::sv::str text, const ca::usz offset) noexcept -> ca::optional<fb::lsp::call_context> {

	const ca::usz end = offset < text.size() ? offset : text.size();
	const bool* const mask = fb::lsp::impl::masked(memory, text.prefix(end));
	const ca::usz floor = end > fb::lsp::impl::call_window ? end - fb::lsp::impl::call_window : 0U;
	ca::usz depth = 0U;
	ca::usz active = 0U;
	for (ca::usz i = end; i > floor; --i) {
		const ca::usz at = i - 1U;
		if (mask[at])
			continue;
		const char c = text.data()[at];
		if (c == ')' || c == ']' || c == '}') {
			++depth;
			continue;
		}
		if (c == '[' || c == '{') {
			if (depth == 0U)
				return ca::nullopt;  // inside a list or map literal, not directly in a call
			--depth;
			continue;
		}
		if (c == ',' && depth == 0U) {
			++active;
			continue;
		}
		if (c != '(')
			continue;
		if (depth > 0U) {
			--depth;
			continue;
		}
		ca::usz j = at;
		while (j > 0U && (text.data()[j - 1U] == ' ' || text.data()[j - 1U] == '\t'))
			--j;
		const ca::usz name_end = j;
		while (j > 0U && fb::lsp::impl::word(text.data()[j - 1U]))
			--j;
		if (j == name_end || (text.data()[j] >= '0' && text.data()[j] <= '9'))
			return ca::nullopt;  // a parenthesized expression or a tuple
		const bool method = j > 0U && text.data()[j - 1U] == '.';
		return fb::lsp::call_context{text.slice(j, name_end - j), active, method};
	}
	return ca::nullopt;
}

auto fb::lsp::in_parser_schema(fb::memory& memory, const ca::sv::str text, const ca::usz offset) noexcept -> bool {

	const ca::sv::str head = text.prefix(offset);
	auto opening = head.find_last(ca::sv::str{"Parser<"});
	if (!opening)
		return false;
	const bool* const mask = fb::lsp::impl::masked(memory, head);
	if (mask[*opening])
		return false;
	ca::usz depth = 0U;
	for (ca::usz i = *opening + 7U; i < head.size(); ++i) {
		if (mask[i])
			continue;
		const char c = head.data()[i];
		if (c == '{' || c == '[' || c == '(')
			++depth;
		else if ((c == '}' || c == ']' || c == ')') && depth > 0U)
			--depth;
		else if (c == '>' && depth == 0U)
			return false;
	}
	return true;
}

auto fb::lsp::word_at(const ca::sv::str text, const ca::usz offset) noexcept -> fb::lsp::word_before {

	ca::usz start = offset < text.size() ? offset : text.size();
	while (start > 0U && fb::lsp::impl::word(text.data()[start - 1U]))
		--start;
	return fb::lsp::word_before{start, start > 0U && text.data()[start - 1U] == '.'};
}
