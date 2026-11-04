#ifndef fb_runtime_library_hpp
#define fb_runtime_library_hpp

#include "ca/optional.hpp"
#include "ca/path.hpp"
#include "core/memory.hpp"
#include "runtime/interpreter.hpp"

namespace fb {

	// -- paths (paths.cpp) ----------------------------------------------------

	/* A path whose text lives in an interpreter's memory; never cleaned up alone. */
	using path = ca::fs::basic_path<ca::arena_allocator>;

	/* text as a path, normalized (no repeated or trailing slash, no "." component); "." when it
	   cannot be one (an embedded NUL). File values keep the text as written: this is for what is
	   computed from them. */
	[[nodiscard]] auto path_of(fb::memory& memory, ca::sv::str text) noexcept -> fb::path;

	/* The path without its last component: "a/b" -> "a", "a" -> ".", "/a" -> "/". */
	[[nodiscard]] auto path_parent(fb::memory& memory, ca::sv::str text) noexcept -> ca::sv::str;

	/* Last component; empty for "/" and ".". */
	[[nodiscard]] auto path_name(fb::memory& memory, ca::sv::str text) noexcept -> ca::sv::str;

	/* name without suffix. */
	[[nodiscard]] auto path_stem(fb::memory& memory, ca::sv::str text) noexcept -> ca::sv::str;

	/* The name from its last '.', unless that dot leads or ends it; empty otherwise. */
	[[nodiscard]] auto path_suffix(fb::memory& memory, ca::sv::str text) noexcept -> ca::sv::str;

	/* parent followed by name; name alone when parent is "." (an absolute name replaces it). */
	[[nodiscard]] auto path_join(fb::memory& memory, ca::sv::str parent, ca::sv::str name) noexcept -> ca::sv::str;

	/* Field name of a File (name, parent, stem, suffix, mtime, size), or unit. */
	[[nodiscard]] auto file_field(fb::interpreter& in, const fb::value* file, ca::sv::str name) noexcept -> fb::value*;

	/* File<*base, name=, stem=, suffix=, parent=>: base with the given parts replaced. */
	[[nodiscard]] auto rebuild_path(fb::memory& memory, ca::sv::str base, ca::optional<ca::sv::str> name, ca::optional<ca::sv::str> stem,
		ca::optional<ca::sv::str> suffix, ca::optional<ca::sv::str> parent) noexcept -> ca::sv::str;

	/* Whether text matches the shell pattern (* ? [set] [!set] \c), whole. */
	[[nodiscard]] auto pattern_match(ca::sv::str pattern, ca::sv::str text) noexcept -> bool;

	/* text rewritten by a morphism: when it matches from, to with each '*' replaced by what the
	   matching '*' of from took; text unchanged otherwise. */
	[[nodiscard]] auto substitute(fb::memory& memory, ca::sv::str from, ca::sv::str to, ca::sv::str text) noexcept -> ca::sv::str;

	/* A command word with its shell quoting removed: '...' kept verbatim, "..." and bare text
	   with backslash escapes. */
	[[nodiscard]] auto command_word(fb::memory& memory, ca::sv::str raw) noexcept -> ca::sv::str;

	/* The files matching pattern, '/'-separated components, "**" any number of directories;
	   names starting with '.' are skipped. */
	[[nodiscard]] auto glob(fb::interpreter& in, ca::sv::str pattern) noexcept -> fb::value*;

	/* Whether target (a File) is missing or older than any dependency, or a dependency is
	   missing. */
	[[nodiscard]] auto stale(fb::interpreter& in, fb::arguments args) noexcept -> bool;

	// -- arguments (arguments.cpp) -------------------------------------------

	/* Parser<schema>(argv): see SKILLS.md "cli args". */
	[[nodiscard]] auto parse_arguments(fb::interpreter& in, fb::value* schema, fb::value* argv) noexcept -> fb::outcome;

	// -- json (json.cpp) -----------------------------------------------------

	[[nodiscard]] auto json_parse(fb::interpreter& in, ca::sv::str text) noexcept -> fb::value*;

	[[nodiscard]] auto json_text(fb::interpreter& in, const fb::value* v, bool pretty) noexcept -> ca::sv::str;

} // namespace fb

#endif // fb_runtime_library_hpp
