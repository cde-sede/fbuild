// fbopt: memoized filesystem metadata and content-based staleness (stalem, mtimem, newest,
// oldest, existsm, is_filem, is_dirm, globm, needed, depsm, hashm, stale_hash, cache_load,
// cache_save, stat_reset). Ported from cparser's plugins/fbopt.c, on fb::path and
// ca::fs::descriptor instead of libc.
//
// Each distinct path is stat'd (and, for hashing, read) at most once until stat_reset(): a
// path's mtime and hash are frozen at first access. The caches live in the memory of the
// interpreter that opened the plugin, released with it; the builtins reach them through one
// plugin-wide pointer, so the plugin serves one interpreter (the one fbuild runs).
#include "ca/descriptor.hpp"
#include "ca/text.hpp"
#include "support.hpp"

namespace {

	/* One path as first seen. */
	struct entry final {
		ca::sv::str path;   // the text as the script gave it
		ca::i64 modified;   // nanoseconds since the epoch
		ca::sys::file_kind kind;
		bool missing;
		bool hashed;
		ca::u64 hash;       // FNV-1a of the contents, once hashed
	};

	template <typename V>
	using table = ca::hashmap<ca::sv::str, V, ca::arena_allocator>;

	struct state final {
		table<entry*> stats;
		fb::list<entry*> order;                 // stats in first-seen order, for cache_save
		table<fb::list<ca::sv::str>*> globs;
		table<ca::u64> baseline;                // path -> hash, from cache_load
	};

	state* current = nullptr;

	template <typename T>
	auto must(ca::result<T>&& r) noexcept -> void {

		if (!r) [[unlikely]]
			ca::trap("fbopt: out of memory");
	}

	auto stat_memo(fb::interpreter& in, const ca::sv::str path) noexcept -> entry* {

		if (auto found = current->stats.get(path))
			return *found;
		const ca::sv::str kept = in.memory().copy(path);
		entry* const e = in.memory().make<entry>(entry{kept, 0, ca::sys::file_kind::unknown, true, false, 0U});
		if (auto status = fb::path_of(in.memory(), kept).stat()) {
			e->missing = false;
			e->modified = status->modified();
			e->kind = status->kind();
		}
		must(current->stats.set(kept, e));
		in.memory().push(current->order, e);
		return e;
	}

	auto stat_of(fb::interpreter& in, const fb::value* const v) noexcept -> entry* {
		return stat_memo(in, in.text(v));
	}

	/* FNV-1a 64 over the file's bytes; 0 when missing or unreadable. Memoized. */
	auto hash_of(fb::interpreter& in, entry* const e) noexcept -> ca::u64 {

		if (e->hashed)
			return e->hash;
		e->hashed = true;
		e->hash = 0U;
		if (e->missing)
			return 0U;
		auto opened = ca::fs::descriptor::make(fb::path_of(in.memory(), e->path));
		if (!opened)
			return 0U;
		ca::u64 h = 1469598103934665603ULL;
		ca::byte buffer[16384];
		// A read error ends the hash with what came before it, as fread's loop did.
		for (auto got = opened->read(ca::span<ca::byte>{buffer, sizeof buffer}); got && *got != 0U;
			got = opened->read(ca::span<ca::byte>{buffer, sizeof buffer}))
			for (ca::usz i = 0U; i < *got; ++i) {
				h ^= static_cast<ca::u8>(buffer[i]);
				h *= 1099511628211ULL;
			}
		static_cast<void>(opened->cleanup());
		e->hash = h;
		return h;
	}

	/* Whether dependency d makes something built at built stale. */
	auto outdates(const entry* const d, const ca::i64 built) noexcept -> bool {
		return d->missing || d->modified > built;
	}

	auto stalem(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (a.size() == 0U)
			return in.boolean(false);
		const entry* const target = stat_of(in, a[0U]);
		if (target->missing)
			return in.boolean(true);
		const ca::i64 built = target->modified;
		for (ca::usz i = 1U; i < a.size(); ++i)
			if (outdates(stat_of(in, a[i]), built))
				return in.boolean(true);
		return in.boolean(false);
	}

	auto mtimem(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const entry* const e = stat_of(in, a[0U]);
		return in.integer(e->missing ? -1 : e->modified);
	}

	/* The newest (or oldest) existing argument, as a File; unit when none exists. */
	auto extreme(fb::interpreter& in, const fb::arguments a, const bool newest) noexcept -> fb::outcome {

		const fb::value* best = nullptr;
		ca::i64 when = 0;
		for (const fb::value* const v : a) {
			const entry* const e = stat_of(in, v);
			if (e->missing)
				continue;
			if (best == nullptr || (newest ? e->modified > when : e->modified < when)) {
				best = v;
				when = e->modified;
			}
		}
		return best == nullptr ? in.unit() : in.file(in.text(best));
	}

	auto newest(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return extreme(in, a, true);
	}

	auto oldest(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return extreme(in, a, false);
	}

	auto existsm(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.boolean(!stat_of(in, a[0U])->missing);
	}

	auto is_filem(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const entry* const e = stat_of(in, a[0U]);
		return in.boolean(!e->missing && e->kind == ca::sys::file_kind::regular);
	}

	auto is_dirm(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const entry* const e = stat_of(in, a[0U]);
		return in.boolean(!e->missing && e->kind == ca::sys::file_kind::directory);
	}

	/* "dir/<wildcard>" read once: the matching names of dir (hidden ones too, as fnmatch without
	   FNM_PERIOD), each as "dir/name", sorted. No slash: dir is ".". */
	auto glob_memo(fb::interpreter& in, const ca::sv::str pattern) noexcept -> const fb::list<ca::sv::str>* {

		if (auto found = current->globs.get(pattern))
			return *found;
		fb::memory& memory = in.memory();
		const ca::sv::str kept = memory.copy(pattern);
		fb::list<ca::sv::str>* const items = memory.make<fb::list<ca::sv::str>>(memory.allocator());
		must(current->globs.set(kept, items));

		const auto split = kept.partition_last("/");
		const ca::sv::str directory = split.found() ? split.before() : ca::sv::str{"."};
		const ca::sv::str wild = split.found() ? split.after() : kept;
		auto entries = fb::path_of(memory, directory).iterdir();
		if (!entries)
			return items;
		for (const fb::path& p : *entries) {
			const ca::sv::str name = p.name();
			if (fb::pattern_match(wild, name))
				memory.push(*items, memory.format("{}/{}", directory, name));
		}
		// Insertion sort, by bytes like strcmp: directories are small.
		ca::sv::str* const data = items->data();
		for (ca::usz i = 1U; i < items->size(); ++i)
			for (ca::usz j = i; j != 0U && data[j].compare(data[j - 1U]) < 0; --j) {
				const ca::sv::str moved = data[j];
				data[j] = data[j - 1U];
				data[j - 1U] = moved;
			}
		return items;
	}

	auto globm(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		for (const ca::sv::str item : *glob_memo(in, in.text(a[0U])))
			in.push(out, in.file_in_place(item));
		return out;
	}

	/* The units [obj, src, ...] whose obj is missing or older than src or a shared dep. */
	auto needed(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		if (a.size() == 0U || !a[0U]->is_sequence())
			return out;
		for (fb::value* const unit : a[0U]->items) {
			if (!unit->is_sequence() || unit->size() == 0U)
				continue;
			const entry* const object = stat_of(in, unit->at(0U));
			bool stale = object->missing;
			if (!stale) {
				const ca::i64 built = object->modified;
				if (unit->size() >= 2U && outdates(stat_of(in, unit->at(1U)), built))
					stale = true;
				for (ca::usz i = 1U; !stale && i < a.size(); ++i)
					stale = outdates(stat_of(in, a[i]), built);
			}
			if (stale)
				in.push(out, unit);
		}
		return out;
	}

	/* The prerequisites of the `cc -MMD` depfile beside an object (x.o -> x.d, else x + ".d"). */
	auto depsm(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::value* const out = in.make(fb::kind::list);
		const ca::sv::str object = in.text(a[0U]);
		const ca::sv::str depfile = object.ends_with(".o")
			? in.memory().format("{}d", object.without_suffix(1U))
			: in.memory().format("{}.d", object);
		auto content = fb::path_of(in.memory(), depfile).read_text();
		if (!content)
			return out;
		const ca::sv::str all{content->data(), content->size()};
		const auto rule = all.partition(":");
		const ca::sv::str rest = rule.found() ? rule.after() : all;
		// Words split on blanks and backslashes (which covers the line continuations).
		ca::usz start = 0U;
		for (ca::usz i = 0U; i <= rest.size(); ++i) {
			const char c = i < rest.size() ? rest.data()[i] : ' ';
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\\')
				continue;
			if (i > start)
				in.push(out, in.file(rest.slice(start, i - start)));
			start = i + 1U;
		}
		return out;
	}

	auto hashm(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return in.integer(static_cast<ca::i64>(hash_of(in, stat_of(in, a[0U]))));
	}

	/* Target missing, or a dep missing or with contents other than the baseline's. */
	auto stale_hash(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (a.size() == 0U)
			return in.boolean(false);
		if (stat_of(in, a[0U])->missing)
			return in.boolean(true);
		for (ca::usz i = 1U; i < a.size(); ++i) {
			entry* const d = stat_of(in, a[i]);
			if (d->missing)
				return in.boolean(true);
			const ca::u64 h = hash_of(in, d);
			const auto known = current->baseline.get(d->path);
			if (!known || *known != h)
				return in.boolean(true);
		}
		return in.boolean(false);
	}

	[[nodiscard]] constexpr auto is_hex(const char c) noexcept -> bool {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	}

	/* "hexhash path" lines into the baseline; an earlier line for a path wins. */
	auto cache_load(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		auto content = fb::path_of(in.memory(), in.text(a[0U])).read_text();
		if (!content)
			return in.boolean(false);
		const ca::sv::str text{content->data(), content->size()};
		for (const ca::sv::str line : text.split("\n")) {
			const auto fields = line.partition(" ");
			if (!fields.found())
				continue;
			const ca::sv::str path = fields.after().strip_end("\r\n");
			if (path.empty() || current->baseline.contains(path))
				continue;
			// strtoull's reading: the leading hex digits, 0 when there are none.
			ca::sv::str digits = fields.before();
			ca::usz n = 0U;
			while (n < digits.size() && is_hex(digits.data()[n]))
				++n;
			auto hash = ca::sys::parse_integer<ca::u64>(digits.slice(0U, n), 16);
			must(current->baseline.set(in.memory().copy(path), hash ? *hash : ca::u64{0U}));
		}
		return in.boolean(true);
	}

	/* The hash of every file hashed so far this run, as the next baseline. */
	auto cache_save(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		fb::text out{in.memory().allocator()};
		for (const entry* const e : current->order)
			if (e->hashed && !e->missing)
				must(ca::format_append(out, "{:016x} {}\n", e->hash, e->path));
		return in.boolean(static_cast<bool>(fb::path_of(in.memory(), in.text(a[0U])).write_text(out.view())));
	}

	/* Forgets the stats and globs (not the baseline), so the next checks look again. */
	auto stat_reset(fb::interpreter& in, const fb::arguments) noexcept -> fb::outcome {

		must(current->stats.clear());
		must(current->order.clear());
		must(current->globs.clear());
		return in.boolean(true);
	}

} // namespace

auto fb::plugin::api(void) noexcept -> ca::u32 {
	return fb::plugin_api;
}

auto fb::plugin::open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {

	const ca::arena_allocator allocator = in.memory().allocator();
	current = in.memory().make<state>(state{table<entry*>{allocator}, fb::list<entry*>{allocator},
		table<fb::list<ca::sv::str>*>{allocator}, table<ca::u64>{allocator}});

	using fb::ret;
	const fb::builtin builtins[] = {
		{"stalem", stalem, 1, -1, ret::boolean, 0U, "stalem(target, ...deps): stale() with each path stat'd once per run, nanosecond mtimes", "Path, ...? -> Bool"},
		{"mtimem", mtimem, 1, 1, ret::integer, 0U, "mtimem(path): mtime in nanoseconds since the epoch, -1 if missing (memoized)", "Path -> Int"},
		{"newest", newest, 1, -1, ret::file, 0U, "newest(...paths): the most recently modified existing path, or nil", "...? -> File"},
		{"oldest", oldest, 1, -1, ret::file, 0U, "oldest(...paths): the least recently modified existing path, or nil", "...? -> File"},
		{"existsm", existsm, 1, 1, ret::boolean, 0U, "existsm(path): exists(), memoized", "Path -> Bool"},
		{"is_filem", is_filem, 1, 1, ret::boolean, 0U, "is_filem(path): regular-file test, memoized", "Path -> Bool"},
		{"is_dirm", is_dirm, 1, 1, ret::boolean, 0U, "is_dirm(path): directory test, memoized", "Path -> Bool"},
		{"globm", globm, 1, 1, ret::list, 0U, "globm('dir/*.ext'): one-level glob read once per pattern, sorted", "String -> List<File>"},
		{"needed", needed, 1, -1, ret::list, 0U, "needed(units, ...deps): the [obj, src, ...] units whose obj is missing or older than src or a dep", "List, ...? -> List"},
		{"depsm", depsm, 1, 1, ret::list, 0U, "depsm(obj): the prerequisites in obj's sibling cc -MMD depfile (x.o -> x.d), [] if none", "Path -> List<File>"},
		{"hashm", hashm, 1, 1, ret::integer, 0U, "hashm(path): FNV-1a hash of the contents, 0 if missing (memoized)", "Path -> Int"},
		{"stale_hash", stale_hash, 1, -1, ret::boolean, 0U, "stale_hash(target, ...deps): target missing, or a dep's contents differ from the cache_load baseline", "Path, ...? -> Bool"},
		{"cache_load", cache_load, 1, 1, ret::boolean, 0U, "cache_load(file): load the stale_hash baseline; false if it cannot be read", "Path -> Bool"},
		{"cache_save", cache_save, 1, 1, ret::boolean, 0U, "cache_save(file): write the hashes of every file hashed this run as the next baseline", "Path -> Bool"},
		{"stat_reset", stat_reset, 0, 0, ret::boolean, 0U, "stat_reset(): forget the memoized stats and globs (not the baseline)", "() -> Bool"},
	};
	for (const fb::builtin& b : builtins)
		in.define(b);
	return {};
}
