#include "lsp/server.hpp"

#include "ca/io.hpp"
#include "ca/print.hpp"
#include "check/type.hpp"
#include "core/version.hpp"
#include "lsp/query.hpp"
#include "lsp/tokens.hpp"
#include "lsp/transport.hpp"
#include "runtime/host.hpp"

namespace fb::lsp::impl {

	using writer = fb::lsp::json_writer;

	// JSON-RPC and protocol error codes.
	inline constexpr ca::i64 parse_error = -32700;
	inline constexpr ca::i64 invalid_request = -32600;
	inline constexpr ca::i64 method_not_found = -32601;
	inline constexpr ca::i64 not_initialized = -32002;
	inline constexpr ca::i64 content_modified = -32801;

	// SymbolKind, CompletionItemKind.
	inline constexpr ca::i64 symbol_class = 5;
	inline constexpr ca::i64 symbol_method = 6;
	inline constexpr ca::i64 symbol_function = 12;
	inline constexpr ca::i64 symbol_variable = 13;
	inline constexpr ca::i64 item_method = 2;
	inline constexpr ca::i64 item_function = 3;
	inline constexpr ca::i64 item_field = 5;
	inline constexpr ca::i64 item_variable = 6;
	inline constexpr ca::i64 item_class = 7;
	inline constexpr ca::i64 item_module = 9;
	inline constexpr ca::i64 item_keyword = 14;

	inline constexpr ca::sv::str keywords[] = {
		"when", "where", "else", "and", "or", "not", "match", "type", "method", "true", "false", "nil",
	};

	inline constexpr ca::sv::str parser_help =
		"Parser<{...}>: a command-line parser built from a schema, one entry per option.\n"
		"\n"
		"**Entry value**\n"
		"- `\"bool\"` / `\"int\"` / `\"str\"`: the type alone (long and short flags derived)\n"
		"- or a spec: `{type: ..., long: ..., short: ..., default: ..., help: ...}`\n"
		"\n"
		"**Spec keys**\n"
		"- `type`: `\"bool\"` | `\"int\"` | `\"str\"`\n"
		"- `long`: the long flag, `\"--jobs\"`\n"
		"- `short`: the short flag; `-<first letter>` when unique\n"
		"- `default`: a value, or a `_ -> value` lambda computed when needed\n"
		"- `help`: shown by the generated `--help`\n"
		"\n"
		"`Parser<schema>` parses the script's arguments; `Parser<schema>(list)` parses list.";

	[[nodiscard]] static auto trap_on(ca::result<fb::memory>&& made) noexcept -> fb::memory {

		if (!made) [[unlikely]]
			ca::trap("fbuildd: out of memory");
		return ca::move(*made);
	}

	template <typename F>
	[[nodiscard]] static auto reply(fb::memory& scratch, const fb::lsp::json* const id, F&& result) noexcept -> ca::result<void> {

		fb::text out{scratch.allocator()};
		writer w{out};
		w.begin_object();
		w.key("jsonrpc");
		w.string("2.0");
		w.key("id");
		w.raw(id);
		w.key("result");
		result(w);
		w.end_object();
		return fb::lsp::write_message(out.view());
	}

	[[nodiscard]] static auto refuse(fb::memory& scratch, const fb::lsp::json* const id, const ca::i64 code, const ca::sv::str message) noexcept -> ca::result<void> {

		fb::text out{scratch.allocator()};
		writer w{out};
		w.begin_object();
		w.key("jsonrpc");
		w.string("2.0");
		w.key("id");
		w.raw(id);
		w.key("error");
		w.begin_object();
		w.key("code");
		w.integer(code);
		w.key("message");
		w.string(message);
		w.end_object();
		w.end_object();
		return fb::lsp::write_message(out.view());
	}

	[[nodiscard]] static auto null_result(fb::memory& scratch, const fb::lsp::json* const id) noexcept -> ca::result<void> {
		return fb::lsp::impl::reply(scratch, id, [](writer& w) noexcept { w.null(); });
	}

	static auto place(writer& w, const fb::lsp::place p) noexcept -> void {

		w.begin_object();
		w.key("line");
		w.integer(p.line);
		w.key("character");
		w.integer(p.character);
		w.end_object();
	}

	/* An fbuild span (inclusive end) as a protocol range (exclusive end). */
	static auto range(writer& w, const fb::lsp::lines& lines, const fb::span s, const bool utf16) noexcept -> void {

		const fb::position end = s.end.line != 0 ? s.end : s.start;
		w.begin_object();
		w.key("start");
		fb::lsp::impl::place(w, lines.place_of(s.start, utf16));
		w.key("end");
		fb::lsp::impl::place(w, lines.place_after(end, utf16));
		w.end_object();
	}

	static auto location(writer& w, const ca::sv::str uri, const fb::lsp::lines& lines, const fb::span s, const bool utf16) noexcept -> void {

		w.begin_object();
		w.key("uri");
		w.string(uri);
		w.key("range");
		fb::lsp::impl::range(w, lines, s, utf16);
		w.end_object();
	}

	static auto contents(writer& w, const ca::sv::str kind, const ca::sv::str value) noexcept -> void {

		w.begin_object();
		w.key("kind");
		w.string(kind);
		w.key("value");
		w.string(value);
		w.end_object();
	}

	/* Where a request points: the document, its usable analysis, and the place as a position
	   and an offset of the document's latest text. */
	struct request final {
		fb::lsp::document* document;
		const fb::lsp::analysis* analysis;
		fb::position at;
		ca::usz offset;
	};

	[[nodiscard]] static auto uri_of(const fb::lsp::json* const params) noexcept -> ca::sv::str {

		auto uri = fb::lsp::text_of(fb::lsp::member(fb::lsp::member(params, "textDocument"), "uri"));
		return uri ? *uri : ca::sv::str{};
	}

	[[nodiscard]] static auto request_at(fb::lsp::document* const d, const fb::lsp::json* const params, const bool utf16) noexcept -> ca::optional<request> {

		if (d == nullptr)
			return ca::nullopt;
		const fb::lsp::json* const position = fb::lsp::member(params, "position");
		const fb::lsp::place p{fb::lsp::integer_of(fb::lsp::member(position, "line"), 0), fb::lsp::integer_of(fb::lsp::member(position, "character"), 0)};
		const fb::position at = d->source().position_of(p, utf16);
		return request{d, d->usable(), at, d->source().offset(at)};
	}

	[[nodiscard]] static auto is_class(const fb::node* const statement) noexcept -> bool {

		const fb::node* const rhs = statement != nullptr ? statement->kid(0U) : nullptr;
		return statement != nullptr && statement->kind == fb::node_kind::assign && rhs != nullptr && rhs->kind == fb::node_kind::lambda
			&& rhs->kid(1U) != nullptr && rhs->kid(1U)->kind == fb::node_kind::objnew;
	}

	/* What a name stands for when the checker linked it to nothing: a builtin or a bound value
	   is no place in the script; any other name defines itself (a parameter, a binding). */
	[[nodiscard]] static auto site_of(const fb::lsp::analysis& a, const fb::node* const n) noexcept -> const fb::node* {

		if (const fb::node* const site = a.report->target_of(n))
			return site;
		if ((n->kind == fb::node_kind::id || n->kind == fb::node_kind::typeexpr)
			&& (a.catalog->builtin(n->tok->text) != nullptr || a.catalog->global(n->tok->text) != nullptr || n->kind == fb::node_kind::typeexpr))
			return nullptr;
		if (n->kind == fb::node_kind::field)
			return nullptr;
		return n;
	}

	/* The name node at a request, in a fresh analysis only: a stale one's positions are off. */
	[[nodiscard]] static auto name_at(fb::memory& scratch, const request& r) noexcept -> const fb::node* {

		if (r.analysis == nullptr || !r.document->fresh())
			return nullptr;
		const fb::list<const fb::node*> path = fb::lsp::locate(scratch, r.analysis->program, r.at);
		return fb::lsp::name_at(path.view(), r.at);
	}

	/* A builtin's type as a value: its signature, or any arguments to its result. */
	[[nodiscard]] static auto builtin_type(fb::check::types& types, const fb::check::builtin_entry& b) noexcept -> fb::check::type* {

		if (b.signature != nullptr)
			return b.signature;
		fb::check::type* const slots = types.tuple();
		types.push(slots, types.variadic(types.unknown()));
		return types.function(slots, b.result, 0U);
	}

	/* The parameter names of the lambda a definition binds, when it binds one. */
	[[nodiscard]] static auto parameter_names(const fb::node* const site) noexcept -> const fb::node* {

		if (site == nullptr)
			return nullptr;
		const fb::node* const value = site->kind == fb::node_kind::assign || site->kind == fb::node_kind::eager || site->kind == fb::node_kind::binding ? site->kid(0U) : nullptr;
		return value != nullptr && value->kind == fb::node_kind::lambda ? value->kid(0U) : nullptr;
	}

	[[nodiscard]] static auto has(const fb::list<ca::sv::str>& names, const ca::sv::str name) noexcept -> bool {

		for (const ca::sv::str n : names)
			if (n == name)
				return true;
		return false;
	}

	static auto item(writer& w, const ca::sv::str label, const ca::i64 kind, const ca::sv::str detail, const ca::sv::str doc, const bool markdown) noexcept -> void {

		w.begin_object();
		w.key("label");
		w.string(label);
		w.key("kind");
		w.integer(kind);
		if (!detail.empty()) {
			w.key("detail");
			w.string(detail);
		}
		if (!doc.empty()) {
			w.key("documentation");
			fb::lsp::impl::contents(w, markdown ? "markdown" : "plaintext", doc);
		}
		w.end_object();
	}

	[[nodiscard]] static auto symbol_kind(const fb::check::symbol_kind k) noexcept -> ca::i64 {

		switch (k) {
			case fb::check::symbol_kind::class_: return fb::lsp::impl::symbol_class;
			case fb::check::symbol_kind::method: return fb::lsp::impl::symbol_method;
			case fb::check::symbol_kind::function: return fb::lsp::impl::symbol_function;
			case fb::check::symbol_kind::value:
			default: return fb::lsp::impl::symbol_variable;
		}
	}

} // namespace fb::lsp::impl

namespace i = fb::lsp::impl;

fb::lsp::server::server(fb::memory& memory, fb::interpreter& in, fb::plugin_set& plugins) noexcept
: _memory{memory}, _in{in}, _plugins{plugins}, _documents{memory.list<fb::lsp::document*>()}, _utf16{true}, _hierarchy{false},
  _markdown{false}, _initialized{false}, _shut_down{false}, _exiting{false} {
}

auto fb::lsp::server::_find(const ca::sv::str uri) noexcept -> fb::lsp::document* {

	for (fb::lsp::document* const d : _documents)
		if (d->uri() == uri)
			return d;
	return nullptr;
}

auto fb::lsp::server::_open(const ca::sv::str uri) noexcept -> fb::lsp::document* {

	if (fb::lsp::document* const existing = this->_find(uri))
		return existing;
	fb::lsp::document* const d = _memory.make<fb::lsp::document>(_memory.copy(uri), i::trap_on(fb::memory::make()), i::trap_on(fb::memory::make()),
		i::trap_on(fb::memory::make()));
	_memory.push(_documents, d);
	return d;
}

auto fb::lsp::server::_close(const ca::sv::str uri) noexcept -> void {

	fb::lsp::document* const d = this->_find(uri);
	if (d == nullptr)
		return;
	static_cast<void>(d->cleanup());
	static_cast<void>(_documents.remove(d));
}

auto fb::lsp::server::_publish(fb::memory& scratch, const fb::lsp::document& d) noexcept -> ca::result<void> {

	fb::text out{scratch.allocator()};
	i::writer w{out};
	w.begin_object();
	w.key("jsonrpc");
	w.string("2.0");
	w.key("method");
	w.string("textDocument/publishDiagnostics");
	w.key("params");
	w.begin_object();
	w.key("uri");
	w.string(d.uri());
	w.key("version");
	w.integer(d.version());
	w.key("diagnostics");
	w.begin_array();
	const auto diagnostic = [&](const fb::span at, const ca::i64 severity, const ca::sv::str message) noexcept {
		w.begin_object();
		w.key("range");
		i::range(w, d.source(), at, _utf16);
		w.key("severity");
		w.integer(severity);
		w.key("source");
		w.string("fbuild");
		w.key("message");
		w.string(message);
		w.end_object();
	};
	if (const auto& syntax = d.syntax_error())
		diagnostic(fb::span{syntax->at, syntax->at}, 1, syntax->message);
	else if (d.usable() != nullptr)
		for (const fb::check::diagnostic& g : d.usable()->report->diagnostics())
			diagnostic(g.at, g.level == fb::check::severity::error ? 1 : 2, g.message);
	w.end_array();
	w.end_object();
	w.end_object();
	return fb::lsp::write_message(out.view());
}

auto fb::lsp::server::_initialize(fb::memory& scratch, const fb::lsp::json* const id, const fb::lsp::json* const params) noexcept -> ca::result<void> {

	const fb::lsp::json* const capabilities = fb::lsp::member(params, "capabilities");
	const fb::lsp::json* const encodings = fb::lsp::member(fb::lsp::member(capabilities, "general"), "positionEncodings");
	for (ca::usz k = 0U; k < fb::lsp::length(encodings); ++k)
		if (fb::lsp::text_of(fb::lsp::element(encodings, k)) == ca::optional<ca::sv::str>{"utf-8"})
			_utf16 = false;
	const fb::lsp::json* const document = fb::lsp::member(capabilities, "textDocument");
	_hierarchy = fb::lsp::truth_of(fb::lsp::member(fb::lsp::member(document, "documentSymbol"), "hierarchicalDocumentSymbolSupport"), false);
	const fb::lsp::json* const formats = fb::lsp::member(fb::lsp::member(document, "hover"), "contentFormat");
	for (ca::usz k = 0U; k < fb::lsp::length(formats); ++k)
		if (fb::lsp::text_of(fb::lsp::element(formats, k)) == ca::optional<ca::sv::str>{"markdown"})
			_markdown = true;

	// Plugins: initializationOptions.plugins (paths), then $FBUILD_PLUGINS / $FBUILD_PLUGIN_PATH
	// as for fbuild. A plugin that fails to load is reported and skipped: the scripts using it
	// get "unknown function" diagnostics rather than no server.
	const fb::lsp::json* const listed = fb::lsp::member(fb::lsp::member(params, "initializationOptions"), "plugins");
	for (ca::usz k = 0U; k < fb::lsp::length(listed); ++k)
		if (auto path = fb::lsp::text_of(fb::lsp::element(listed, k)))
			if (auto loaded = _plugins.load(_in, *path); !loaded)
				static_cast<void>(ca::print(ca::io::err(), "fbuildd: {}\n", loaded.error().message));
	if (auto loaded = _plugins.load_environment(_in); !loaded)
		static_cast<void>(ca::print(ca::io::err(), "fbuildd: {}\n", loaded.error().message));
	for (const ca::sv::str path : _plugins.paths())
		static_cast<void>(ca::print(ca::io::err(), "fbuildd: loaded plugin {}\n", path));
	fb::bind_host(_in, "", _plugins.paths());
	_initialized = true;

	return i::reply(scratch, id, [&](i::writer& w) noexcept {
		w.begin_object();
		w.key("capabilities");
		w.begin_object();
		w.key("positionEncoding");
		w.string(_utf16 ? "utf-16" : "utf-8");
		w.key("textDocumentSync");
		w.begin_object();
		w.key("openClose");
		w.boolean(true);
		w.key("change");
		w.integer(1);  // the whole text on each change
		w.end_object();
		w.key("hoverProvider");
		w.boolean(true);
		w.key("definitionProvider");
		w.boolean(true);
		w.key("referencesProvider");
		w.boolean(true);
		w.key("documentHighlightProvider");
		w.boolean(true);
		w.key("documentSymbolProvider");
		w.boolean(true);
		w.key("completionProvider");
		w.begin_object();
		w.key("triggerCharacters");
		w.begin_array();
		w.string(".");
		w.end_array();
		w.end_object();
		w.key("semanticTokensProvider");
		w.begin_object();
		w.key("legend");
		w.begin_object();
		w.key("tokenTypes");
		w.begin_array();
		for (const ca::sv::str t : fb::lsp::token_types)
			w.string(t);
		w.end_array();
		w.key("tokenModifiers");
		w.begin_array();
		for (const ca::sv::str t : fb::lsp::token_modifiers)
			w.string(t);
		w.end_array();
		w.end_object();
		w.key("full");
		w.boolean(true);
		w.end_object();
		w.key("signatureHelpProvider");
		w.begin_object();
		w.key("triggerCharacters");
		w.begin_array();
		w.string("(");
		w.string(",");
		w.end_array();
		w.end_object();
		w.end_object();
		w.key("serverInfo");
		w.begin_object();
		w.key("name");
		w.string("fbuildd");
		w.key("version");
		w.string(fb::version);
		w.end_object();
		w.end_object();
	});
}

auto fb::lsp::server::_hover(fb::memory& scratch, const fb::lsp::json* const id, const fb::lsp::json* const params) noexcept -> ca::result<void> {

	auto r = i::request_at(this->_find(i::uri_of(params)), params, _utf16);
	if (!r || r->analysis == nullptr || !r->document->fresh())
		return i::null_result(scratch, id);
	const fb::lsp::analysis& a = *r->analysis;
	const fb::list<const fb::node*> path = fb::lsp::locate(scratch, a.program, r->at);
	const fb::node* n = fb::lsp::name_at(path.view(), r->at);
	ca::sv::str head;
	ca::sv::str doc;
	if (n != nullptr) {
		ca::sv::str name = fb::lsp::name_of(scratch, n);
		const fb::check::type* t = a.report->type_of(n);
		const fb::node* const site = a.report->target_of(n);
		if (n->kind == fb::node_kind::id && site == nullptr) {
			if (const fb::check::builtin_entry* const b = a.catalog->builtin(n->tok->text)) {
				t = i::builtin_type(*a.types, *b);
				doc = b->doc;
			} else if (fb::check::type* const g = a.catalog->global(n->tok->text))
				t = g;
		}
		if (t == nullptr && site != nullptr)
			t = a.report->type_of(site);
		if (i::is_class(n) || (site != nullptr && n->kind == fb::node_kind::typeexpr && i::is_class(site)))
			name = scratch.format("type {}", name);
		if (t != nullptr)
			head = scratch.format("{} : {}", name, fb::check::render(scratch, t, true));
		else if (!doc.empty())
			head = name;
	} else {
		// No name here: the type of the innermost expression whose token is at the place.
		for (ca::usz k = path.size(); k > 0U && head.empty(); --k) {
			const fb::node* const e = path.data()[k - 1U];
			const fb::check::type* const t = a.report->type_of(e);
			if (t != nullptr && e->tok != nullptr && fb::lsp::contains(fb::span{e->tok->start, e->tok->end}, r->at)) {
				head = fb::check::render(scratch, t, true);
				n = e;
			}
		}
	}
	if (head.empty())
		return i::null_result(scratch, id);
	const ca::sv::str value = _markdown
		? (doc.empty() ? scratch.format("```funcbuild\n{}\n```", head) : scratch.format("```funcbuild\n{}\n```\n\n{}", head, doc))
		: (doc.empty() ? head : scratch.format("{}\n\n{}", head, doc));
	const fb::node* const shown = n;
	return i::reply(scratch, id, [&](i::writer& w) noexcept {
		w.begin_object();
		w.key("contents");
		i::contents(w, _markdown ? "markdown" : "plaintext", value);
		w.key("range");
		i::range(w, *a.lines, fb::span{shown->tok->start, shown->tok->end}, _utf16);
		w.end_object();
	});
}

auto fb::lsp::server::_definition(fb::memory& scratch, const fb::lsp::json* const id, const fb::lsp::json* const params) noexcept -> ca::result<void> {

	auto r = i::request_at(this->_find(i::uri_of(params)), params, _utf16);
	if (!r)
		return i::null_result(scratch, id);
	const fb::node* const n = i::name_at(scratch, *r);
	const fb::node* const site = n != nullptr ? i::site_of(*r->analysis, n) : nullptr;
	if (site == nullptr)
		return i::null_result(scratch, id);
	return i::reply(scratch, id, [&](i::writer& w) noexcept {
		i::location(w, r->document->uri(), *r->analysis->lines, fb::lsp::site_span(site), _utf16);
	});
}

auto fb::lsp::server::_references(fb::memory& scratch, const fb::lsp::json* const id, const fb::lsp::json* const params, const bool highlight) noexcept -> ca::result<void> {

	auto r = i::request_at(this->_find(i::uri_of(params)), params, _utf16);
	if (!r)
		return i::null_result(scratch, id);
	const fb::node* const n = i::name_at(scratch, *r);
	const fb::node* const site = n != nullptr ? i::site_of(*r->analysis, n) : nullptr;
	if (site == nullptr)
		return i::null_result(scratch, id);
	const bool declaration = highlight || fb::lsp::truth_of(fb::lsp::member(fb::lsp::member(params, "context"), "includeDeclaration"), true);
	const fb::list<const fb::node*> uses = fb::lsp::uses_of(scratch, r->analysis->program, *r->analysis->report, site);
	return i::reply(scratch, id, [&](i::writer& w) noexcept {
		w.begin_array();
		for (ca::usz k = declaration ? 0U : 1U; k < uses.size(); ++k) {
			const fb::span at = fb::lsp::site_span(uses.data()[k]);
			if (!highlight) {
				i::location(w, r->document->uri(), *r->analysis->lines, at, _utf16);
				continue;
			}
			w.begin_object();
			w.key("range");
			i::range(w, *r->analysis->lines, at, _utf16);
			w.key("kind");
			w.integer(k == 0U ? 3 : 2);  // Write for the definition, Read for the uses
			w.end_object();
		}
		w.end_array();
	});
}

auto fb::lsp::server::_symbols(fb::memory& scratch, const fb::lsp::json* const id, const fb::lsp::json* const params) noexcept -> ca::result<void> {

	const fb::lsp::document* const d = this->_find(i::uri_of(params));
	const fb::lsp::analysis* const a = d != nullptr ? d->usable() : nullptr;
	if (a == nullptr)
		return i::reply(scratch, id, [](i::writer& w) noexcept {
			w.begin_array();
			w.end_array();
		});
	const auto symbols = a->report->symbols();
	const auto detail = [&](const fb::check::symbol& s) noexcept -> ca::sv::str {
		const fb::check::type* const t = a->report->type_of(s.site);
		return t != nullptr ? fb::check::render(scratch, t) : ca::sv::str{};
	};
	const auto document_symbol = [&](i::writer& w, const fb::check::symbol& s, const ca::sv::str name, const bool children) noexcept {
		w.begin_object();
		w.key("name");
		w.string(name);
		if (const ca::sv::str shown = detail(s); !shown.empty()) {
			w.key("detail");
			w.string(shown);
		}
		w.key("kind");
		w.integer(i::symbol_kind(s.kind));
		w.key("range");
		i::range(w, *a->lines, fb::span_of(s.site), _utf16);
		w.key("selectionRange");
		i::range(w, *a->lines, fb::lsp::site_span(s.site), _utf16);
		if (children) {
			w.key("children");
			w.begin_array();
			for (const fb::check::symbol& m : symbols) {
				if (m.kind != fb::check::symbol_kind::method || m.container != s.name || m.site->kind != fb::node_kind::methoddef)
					continue;
				w.begin_object();
				w.key("name");
				w.string(m.name);
				w.key("kind");
				w.integer(i::symbol_method);
				w.key("range");
				i::range(w, *a->lines, fb::span_of(m.site), _utf16);
				w.key("selectionRange");
				i::range(w, *a->lines, fb::lsp::site_span(m.site), _utf16);
				w.end_object();
			}
			w.end_array();
		}
		w.end_object();
	};
	const auto class_named = [&](const ca::sv::str name) noexcept -> bool {
		for (const fb::check::symbol& s : symbols)
			if (s.kind == fb::check::symbol_kind::class_ && s.name == name)
				return true;
		return false;
	};
	return i::reply(scratch, id, [&](i::writer& w) noexcept {
		w.begin_array();
		for (const fb::check::symbol& s : symbols) {
			const bool method = s.kind == fb::check::symbol_kind::method;
			if (method && s.site->kind != fb::node_kind::methoddef)
				continue;  // a copied method is no definition of its own
			if (_hierarchy) {
				if (method && class_named(s.container))
					continue;  // listed under its class
				document_symbol(w, s, method ? scratch.format("{}.{}", s.container, s.name) : s.name, s.kind == fb::check::symbol_kind::class_);
				continue;
			}
			w.begin_object();
			w.key("name");
			w.string(s.name);
			w.key("kind");
			w.integer(i::symbol_kind(s.kind));
			w.key("location");
			i::location(w, d->uri(), *a->lines, fb::span_of(s.site), _utf16);
			if (!s.container.empty()) {
				w.key("containerName");
				w.string(s.container);
			}
			w.end_object();
		}
		w.end_array();
	});
}

auto fb::lsp::server::_completion(fb::memory& scratch, const fb::lsp::json* const id, const fb::lsp::json* const params) noexcept -> ca::result<void> {

	auto r = i::request_at(this->_find(i::uri_of(params)), params, _utf16);
	const auto empty = [](i::writer& w) noexcept {
		w.begin_array();
		w.end_array();
	};
	if (!r || r->analysis == nullptr)
		return i::reply(scratch, id, empty);
	const fb::lsp::analysis& a = *r->analysis;
	const ca::sv::str text = r->document->source().text();
	const fb::lsp::word_before word = fb::lsp::word_at(text, r->offset);
	const fb::position start{r->at.line, r->at.col - static_cast<int>(r->offset - word.start)};

	if (word.after_dot) {
		const fb::node* const field = r->document->fresh() ? fb::lsp::field_at(a.program, start) : nullptr;
		const fb::check::type* const base = field != nullptr ? a.report->type_of(field->kid(0U)) : nullptr;
		if (base == nullptr)
			return i::reply(scratch, id, empty);
		return i::reply(scratch, id, [&](i::writer& w) noexcept {
			w.begin_array();
			fb::list<ca::sv::str> seen = scratch.list<ca::sv::str>();
			const auto member = [&](const ca::sv::str name, const ca::i64 kind, const fb::check::type* const t) noexcept {
				if (i::has(seen, name))
					return;
				scratch.push(seen, name);
				i::item(w, name, kind, t != nullptr ? fb::check::render(scratch, t) : ca::sv::str{}, {}, _markdown);
			};
			if (base->is(fb::check::form::object) || base->is(fb::check::form::file)) {
				if (base->is(fb::check::form::object) && !base->native)
					for (const fb::check::symbol& s : a.report->symbols())
						if (s.kind == fb::check::symbol_kind::method && s.container == base->name)
							member(s.name, i::item_method, a.report->type_of(s.site));
				for (ca::usz k = 0U; k < base->names.size(); ++k)
					member(base->names.data()[k], i::item_field, base->kid(k));
				if (base->native)
					if (const fb::check::native_class* const cls = a.catalog->native(base->name))
						for (const ca::sv::str m : cls->methods)
							member(m, i::item_method, nullptr);
			} else if (base->is(fb::check::form::tuple) && base->sized)
				for (ca::usz k = 0U; k < base->size(); ++k)
					member(scratch.format("{}", k), i::item_field, base->kid(k));
			w.end_array();
		});
	}

	// A name: the locals in scope, the script's definitions, the builtins, the keywords.
	const fb::position probe = word.start < r->offset ? start : fb::position{r->at.line, r->at.col > 1 ? r->at.col - 1 : 1};
	const fb::list<const fb::node*> path = r->document->fresh() ? fb::lsp::locate(scratch, a.program, probe) : scratch.list<const fb::node*>();
	const fb::list<fb::lsp::binder> locals = fb::lsp::binders(scratch, path.view(), probe);
	return i::reply(scratch, id, [&](i::writer& w) noexcept {
		w.begin_array();
		fb::list<ca::sv::str> seen = scratch.list<ca::sv::str>();
		const auto add = [&](const ca::sv::str name, const ca::i64 kind, const fb::check::type* const t, const ca::sv::str doc) noexcept {
			if (name.empty() || i::has(seen, name))
				return;
			scratch.push(seen, name);
			i::item(w, name, kind, t != nullptr ? fb::check::render(scratch, t) : ca::sv::str{}, doc, _markdown);
		};
		for (const fb::lsp::binder& b : locals) {
			const fb::check::type* const t = a.report->type_of(b.site);
			add(b.name, t != nullptr && t->is(fb::check::form::function) ? i::item_function : i::item_variable, t, {});
		}
		for (const fb::check::symbol& s : a.report->symbols()) {
			if (s.kind == fb::check::symbol_kind::method)
				continue;
			const ca::i64 kind = s.kind == fb::check::symbol_kind::class_ ? i::item_class
				: s.kind == fb::check::symbol_kind::function ? i::item_function : i::item_variable;
			add(s.name, kind, a.report->type_of(s.site), {});
		}
		for (const ca::sv::str name : a.catalog->names()) {
			if (const fb::check::builtin_entry* const b = a.catalog->builtin(name)) {
				add(name, i::item_function, i::builtin_type(*a.types, *b), b->doc);
				continue;
			}
			const fb::check::type* const g = a.catalog->global(name);
			add(name, g != nullptr && g->is(fb::check::form::object) && g->native ? i::item_module : i::item_variable, g, {});
		}
		for (const ca::sv::str k : i::keywords)
			add(k, i::item_keyword, nullptr, {});
		w.end_array();
	});
}

auto fb::lsp::server::_semantic_tokens(fb::memory& scratch, const fb::lsp::json* const id, const fb::lsp::json* const params) noexcept -> ca::result<void> {

	const fb::lsp::document* const d = this->_find(i::uri_of(params));
	const fb::lsp::analysis* const a = d != nullptr ? d->usable() : nullptr;
	// The last analysis is of older text: its positions would color the wrong places. Clients
	// keep the tokens they have on ContentModified.
	if (a != nullptr && !d->fresh())
		return i::refuse(scratch, id, i::content_modified, "the document does not parse");
	const fb::list<fb::lsp::semantic_token> tokens = a != nullptr
		? fb::lsp::semantic_tokens(scratch, a->program, *a->report, *a->catalog)
		: scratch.list<fb::lsp::semantic_token>();
	return i::reply(scratch, id, [&](i::writer& w) noexcept {
		w.begin_object();
		w.key("data");
		w.begin_array();
		// Five integers a token: line and start relative to the previous token's, length,
		// type, modifiers.
		fb::lsp::place previous{0, 0};
		for (const fb::lsp::semantic_token& t : tokens) {
			const fb::lsp::place start = a->lines->place_of(t.start, _utf16);
			const fb::lsp::place end = a->lines->place_after(t.end, _utf16);
			if (end.line != start.line || end.character <= start.character)
				continue;
			w.integer(start.line - previous.line);
			w.integer(start.line == previous.line ? start.character - previous.character : start.character);
			w.integer(end.character - start.character);
			w.integer(static_cast<ca::i64>(t.type));
			w.integer(static_cast<ca::i64>(t.modifiers));
			previous = start;
		}
		w.end_array();
		w.end_object();
	});
}

auto fb::lsp::server::_signature(fb::memory& scratch, const fb::lsp::json* const id, const fb::lsp::json* const params) noexcept -> ca::result<void> {

	auto r = i::request_at(this->_find(i::uri_of(params)), params, _utf16);
	if (!r)
		return i::null_result(scratch, id);
	const ca::sv::str text = r->document->source().text();
	if (fb::lsp::in_parser_schema(scratch, text, r->offset))
		return i::reply(scratch, id, [&](i::writer& w) noexcept {
			w.begin_object();
			w.key("signatures");
			w.begin_array();
			w.begin_object();
			w.key("label");
			w.string("Parser<{...}>");
			w.key("documentation");
			i::contents(w, _markdown ? "markdown" : "plaintext", i::parser_help);
			w.key("parameters");
			w.begin_array();
			w.end_array();
			w.end_object();
			w.end_array();
			w.end_object();
		});
	auto call = fb::lsp::call_at(scratch, text, r->offset);
	if (!call || call->method || r->analysis == nullptr)
		return i::null_result(scratch, id);
	const fb::lsp::analysis& a = *r->analysis;

	// The callee: a definition of the script (its parameter names known), else a builtin.
	const fb::check::type* fn = nullptr;
	const fb::node* names = nullptr;
	ca::sv::str doc;
	for (const fb::check::symbol& s : a.report->symbols())
		if (s.kind != fb::check::symbol_kind::method && s.name == call->callee) {
			fn = a.report->type_of(s.site);
			names = i::parameter_names(s.site);
			break;
		}
	if (fn == nullptr)
		if (const fb::check::builtin_entry* const b = a.catalog->builtin(call->callee)) {
			fn = i::builtin_type(*a.types, *b);
			doc = b->doc;
		}
	if (fn == nullptr || !fn->is(fb::check::form::function) || fn->parameters() == nullptr)
		return i::null_result(scratch, id);

	const fb::check::type* const slots = fn->parameters();
	fb::text label{scratch.allocator()};
	fb::list<ca::i64> bounds = scratch.list<ca::i64>();
	const auto put = [&](const ca::sv::str piece) noexcept {
		if (!label.append(piece)) [[unlikely]]
			ca::trap("fbuildd: out of memory");
	};
	put(call->callee);
	put("(");
	for (ca::usz k = 0U; k < slots->size(); ++k) {
		if (k != 0U)
			put(", ");
		scratch.push(bounds, fb::lsp::length_of(label.view(), _utf16));
		const fb::node* const p = names != nullptr ? names->kid(k) : nullptr;
		if (p != nullptr && p->tok != nullptr && (p->kind == fb::node_kind::id || p->kind == fb::node_kind::projection))
			put(scratch.format("{}: ", p->tok->text));
		const fb::check::type* const t = slots->kid(k);
		const bool spread = t->is(fb::check::form::list) && t->variadic;
		put(spread ? scratch.format("...{}", fb::check::render(scratch, t->kid(0U))) : fb::check::render(scratch, t));
		if (!spread && k >= fn->required)
			put("?");
		scratch.push(bounds, fb::lsp::length_of(label.view(), _utf16));
	}
	put(") -> ");
	put(fb::check::render(scratch, fn->result()));

	const ca::usz count = slots->size();
	const ca::usz active = count == 0U ? 0U : call->active < count ? call->active : count - 1U;
	const ca::sv::str shown = label.view();
	return i::reply(scratch, id, [&](i::writer& w) noexcept {
		w.begin_object();
		w.key("signatures");
		w.begin_array();
		w.begin_object();
		w.key("label");
		w.string(shown);
		if (!doc.empty()) {
			w.key("documentation");
			i::contents(w, _markdown ? "markdown" : "plaintext", doc);
		}
		w.key("parameters");
		w.begin_array();
		for (ca::usz k = 0U; k < count; ++k) {
			w.begin_object();
			w.key("label");
			w.begin_array();
			w.integer(bounds.data()[2U * k]);
			w.integer(bounds.data()[2U * k + 1U]);
			w.end_array();
			w.end_object();
		}
		w.end_array();
		w.end_object();
		w.end_array();
		w.key("activeSignature");
		w.integer(0);
		w.key("activeParameter");
		w.integer(static_cast<ca::i64>(active));
		w.end_object();
	});
}

auto fb::lsp::server::handle(fb::memory& scratch, const fb::lsp::json* const message) noexcept -> ca::result<void> {

	const auto method = fb::lsp::text_of(fb::lsp::member(message, "method"));
	const fb::lsp::json* const id = fb::lsp::member(message, "id");
	const fb::lsp::json* const params = fb::lsp::member(message, "params");
	if (!method)
		return {};  // a response: the server sends no requests
	const ca::sv::str m = *method;
	if (m == "exit") {
		_exiting = true;
		return {};
	}
	if (!_initialized && m != "initialize")
		return id != nullptr ? i::refuse(scratch, id, i::not_initialized, "not initialized") : ca::result<void>{};
	if (_shut_down)
		return id != nullptr ? i::refuse(scratch, id, i::invalid_request, "shut down") : ca::result<void>{};

	if (m == "initialize")
		return this->_initialize(scratch, id, params);
	if (m == "shutdown") {
		_shut_down = true;
		return i::null_result(scratch, id);
	}
	if (m == "textDocument/didOpen" || m == "textDocument/didChange") {
		const ca::sv::str uri = i::uri_of(params);
		if (uri.empty())
			return {};
		ca::optional<ca::sv::str> text;
		if (m == "textDocument/didOpen")
			text = fb::lsp::text_of(fb::lsp::member(fb::lsp::member(params, "textDocument"), "text"));
		else {
			// Whole-text sync: the last change is the text.
			const fb::lsp::json* const changes = fb::lsp::member(params, "contentChanges");
			if (fb::lsp::length(changes) != 0U)
				text = fb::lsp::text_of(fb::lsp::member(fb::lsp::element(changes, fb::lsp::length(changes) - 1U), "text"));
		}
		if (!text)
			return {};
		fb::lsp::document* const d = this->_open(uri);
		d->update(*text, fb::lsp::integer_of(fb::lsp::member(fb::lsp::member(params, "textDocument"), "version"), 0), _in);
		return this->_publish(scratch, *d);
	}
	if (m == "textDocument/didClose") {
		const ca::sv::str uri = i::uri_of(params);
		// Clears what the editor shows for it.
		fb::text out{scratch.allocator()};
		i::writer w{out};
		w.begin_object();
		w.key("jsonrpc");
		w.string("2.0");
		w.key("method");
		w.string("textDocument/publishDiagnostics");
		w.key("params");
		w.begin_object();
		w.key("uri");
		w.string(uri);
		w.key("diagnostics");
		w.begin_array();
		w.end_array();
		w.end_object();
		w.end_object();
		this->_close(uri);
		return fb::lsp::write_message(out.view());
	}
	if (m == "textDocument/hover")
		return this->_hover(scratch, id, params);
	if (m == "textDocument/definition")
		return this->_definition(scratch, id, params);
	if (m == "textDocument/references")
		return this->_references(scratch, id, params, false);
	if (m == "textDocument/documentHighlight")
		return this->_references(scratch, id, params, true);
	if (m == "textDocument/documentSymbol")
		return this->_symbols(scratch, id, params);
	if (m == "textDocument/completion")
		return this->_completion(scratch, id, params);
	if (m == "textDocument/semanticTokens/full")
		return this->_semantic_tokens(scratch, id, params);
	if (m == "textDocument/signatureHelp")
		return this->_signature(scratch, id, params);
	if (id != nullptr)
		return i::refuse(scratch, id, i::method_not_found, scratch.format("unsupported method {}", m));
	return {};  // notifications the server does not need: initialized, didSave, $/cancelRequest...
}

auto fb::lsp::server::reject(fb::memory& scratch) noexcept -> ca::result<void> {
	return i::refuse(scratch, nullptr, i::parse_error, "the message is not JSON");
}

auto fb::lsp::server::cleanup(void) noexcept -> ca::result<void> {

	bool ok = true;
	for (fb::lsp::document* const d : _documents)
		ok = static_cast<bool>(d->cleanup()) && ok;
	static_cast<void>(_documents.clear());
	if (!ok)
		return ca::failure{ca::errc::io_error};
	return {};
}
