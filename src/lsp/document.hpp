#ifndef fb_lsp_document_hpp
#define fb_lsp_document_hpp

#include "ca/integers.hpp"
#include "ca/optional.hpp"
#include "ca/sv.hpp"
#include "check/catalog.hpp"
#include "check/checker.hpp"
#include "core/diagnostic.hpp"
#include "core/memory.hpp"
#include "runtime/interpreter.hpp"
#include "syntax/ast.hpp"

namespace fb::lsp {

	/* A protocol position: 0-based line, and character counted in the negotiated encoding
	   (UTF-16 code units unless the client accepts UTF-8 bytes). */
	struct place final {
		ca::i64 line;
		ca::i64 character;
	};

	/* The length of text in the negotiated encoding: UTF-16 code units, or bytes. */
	[[nodiscard]] auto length_of(ca::sv::str text, bool utf16) noexcept -> ca::i64;

	/* A text and where its lines start, converting between fbuild positions (1-based line and
	   byte column) and protocol places. */
	class lines final {

		private:

			ca::sv::str _text;
			fb::list<ca::usz> _starts;

		public:

			lines(fb::memory& memory, ca::sv::str text) noexcept;

			[[nodiscard]] auto text(void) const noexcept -> ca::sv::str {
				return _text;
			}

			[[nodiscard]] auto count(void) const noexcept -> ca::usz {
				return _starts.size();
			}

			/* Line index (0-based) without its line break; empty past the end. */
			[[nodiscard]] auto line(ca::usz index) const noexcept -> ca::sv::str;

			/* Byte offset of a position, clamped to the text. */
			[[nodiscard]] auto offset(fb::position at) const noexcept -> ca::usz;

			/* The place of a position's first byte. */
			[[nodiscard]] auto place_of(fb::position at, bool utf16) const noexcept -> fb::lsp::place;

			/* The place just past a position's character: the protocol's exclusive end for the
			   inclusive end of an fbuild span. */
			[[nodiscard]] auto place_after(fb::position at, bool utf16) const noexcept -> fb::lsp::place;

			/* The position of a place; a character past the line's end lands just after it. */
			[[nodiscard]] auto position_of(fb::lsp::place at, bool utf16) const noexcept -> fb::position;
	};

	/* One check of a document's text: the program, the types inferred, the diagnostics. Owns
	   everything in its memory, which the next analysis into it resets. */
	struct analysis final {
		fb::memory memory;
		fb::lsp::lines* lines;        // the text analyzed (possibly repaired, see document)
		const fb::node* program;      // nullptr: the text did not parse
		fb::check::types* types;
		fb::check::catalog* catalog;
		const fb::check::report* report;
	};

	/* An open document: its latest text, and the most recent analysis whose text parsed. The
	   parser stops at the first error, which mid-edit is most of the time, so the text is
	   repaired when it does not parse: a placeholder after a dot with nothing after it (the
	   "base." of a completion request), or else the line of the error blanked out, or the last
	   line with text before it (an error at the end of input). When none parses, the last analysis that did stays, fresh() false. */
	class document final {

		private:

			ca::sv::str _uri;
			ca::i64 _version;
			fb::memory _source_memory;   // _source, _syntax: reset on each update
			fb::lsp::lines* _source;
			ca::optional<fb::diagnostic> _syntax;
			fb::lsp::analysis _first;
			fb::lsp::analysis _second;
			fb::lsp::analysis* _usable;  // nullptr until some text parses
			bool _fresh;

		public:

			/* uri lives in memory that outlives the document. */
			document(ca::sv::str uri, fb::memory&& source, fb::memory&& first, fb::memory&& second) noexcept;

			document(const document&) = delete;

			auto operator=(const document&) -> document& = delete;

			[[nodiscard]] auto uri(void) const noexcept -> ca::sv::str {
				return _uri;
			}

			[[nodiscard]] auto version(void) const noexcept -> ca::i64 {
				return _version;
			}

			/* Replaces the text and checks it against what in has registered. */
			auto update(ca::sv::str text, ca::i64 version, const fb::interpreter& in) noexcept -> void;

			[[nodiscard]] auto source(void) const noexcept -> const fb::lsp::lines& {
				return *_source;
			}

			/* Why the latest text does not parse; nullopt when it does. */
			[[nodiscard]] auto syntax_error(void) const noexcept -> const ca::optional<fb::diagnostic>& {
				return _syntax;
			}

			/* The analysis queries read; nullptr when no text has parsed yet. */
			[[nodiscard]] auto usable(void) const noexcept -> const fb::lsp::analysis* {
				return _usable;
			}

			/* Whether usable() is of the latest text (repaired or not), so its positions hold. */
			[[nodiscard]] auto fresh(void) const noexcept -> bool {
				return _fresh;
			}

			[[nodiscard]] auto cleanup(void) noexcept -> ca::result<void>;
	};

} // namespace fb::lsp

#endif // fb_lsp_document_hpp
