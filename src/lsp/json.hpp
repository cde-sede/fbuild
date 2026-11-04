#ifndef fb_lsp_json_hpp
#define fb_lsp_json_hpp

#include "ca/integers.hpp"
#include "ca/optional.hpp"
#include "ca/sv.hpp"
#include "core/memory.hpp"

namespace fb::lsp {

	/* JSON for the protocol's messages. The runtime's own codec (runtime/json.cpp) builds script
	   values inside an interpreter; requests are read as these plain trees instead, in a memory
	   that is reset after each message. */
	enum class json_kind : ca::u8 {
		null,
		boolean,
		number,
		string,
		array,
		object,
	};

	struct json final {
		fb::lsp::json_kind kind;
		bool truth;                  // boolean
		ca::f64 number;              // number
		ca::sv::str text;            // string, decoded
		fb::list<ca::sv::str> keys;  // object: the member names, parallel to items
		fb::list<fb::lsp::json*> items;  // array: the elements; object: the member values

		[[nodiscard]] auto is(const fb::lsp::json_kind k) const noexcept -> bool {
			return kind == k;
		}
	};

	/* The value of text, or nullptr when it is not JSON. */
	[[nodiscard]] auto parse_json(fb::memory& memory, ca::sv::str text) noexcept -> const fb::lsp::json*;

	/* These accept nullptr for a missing value, so that lookups chain:
	   integer_of(member(member(params, "position"), "line")). */

	/* The member key of an object; nullptr when absent or not an object. */
	[[nodiscard]] auto member(const fb::lsp::json* object, ca::sv::str key) noexcept -> const fb::lsp::json*;

	/* The element index of an array; nullptr when absent. */
	[[nodiscard]] auto element(const fb::lsp::json* array, ca::usz index) noexcept -> const fb::lsp::json*;

	/* An array's size; 0 for anything else. */
	[[nodiscard]] auto length(const fb::lsp::json* array) noexcept -> ca::usz;

	[[nodiscard]] auto text_of(const fb::lsp::json* v) noexcept -> ca::optional<ca::sv::str>;

	/* A number, truncated; fallback for anything else. */
	[[nodiscard]] auto integer_of(const fb::lsp::json* v, ca::i64 fallback) noexcept -> ca::i64;

	[[nodiscard]] auto truth_of(const fb::lsp::json* v, bool fallback) noexcept -> bool;

	/* Writes JSON text into out, placing the commas: begin_object(), then key() before each
	   member's value; values in an array follow one another. */
	class json_writer final {

		private:

			fb::text& _out;
			bool _comma;

			auto _put(ca::sv::str text) noexcept -> void;

			auto _value(void) noexcept -> void;

		public:

			explicit json_writer(fb::text& out) noexcept
			: _out{out}, _comma{false} {
			}

			auto begin_object(void) noexcept -> void;

			auto end_object(void) noexcept -> void;

			auto begin_array(void) noexcept -> void;

			auto end_array(void) noexcept -> void;

			auto key(ca::sv::str name) noexcept -> void;

			auto string(ca::sv::str text) noexcept -> void;

			auto integer(ca::i64 value) noexcept -> void;

			auto boolean(bool value) noexcept -> void;

			auto null(void) noexcept -> void;

			/* A value already written as JSON, a request's id echoed back. */
			auto raw(const fb::lsp::json* v) noexcept -> void;
	};

} // namespace fb::lsp

#endif // fb_lsp_json_hpp
