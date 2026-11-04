// fbfs: move, symlink, remove, touch, is_dir, is_file. Ported from cparser's plugins/fbfs.c,
// on fb::path (ca::fs::path) instead of libc.
#include "support.hpp"

namespace {

	auto path_of(fb::interpreter& in, const fb::value* const v) noexcept -> fb::path {
		return fb::path_of(in.memory(), in.text(v));
	}

	/* from's bytes into to, keeping from's permission bits; then from removed. */
	auto copy_then_remove(const fb::path& from, const fb::path& to) noexcept -> bool {

		auto status = from.stat();
		auto bytes = from.read_bytes();
		if (!status || !bytes)
			return false;
		if (!to.write_bytes(bytes->view()) || !to.chmod(status->permissions() & 0777U))
			return false;
		return static_cast<bool>(from.unlink());
	}

	auto move(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const fb::path from = path_of(in, a[0U]);
		const fb::path to = path_of(in, a[1U]);
		if (!from.rename(to))
			static_cast<void>(copy_then_remove(from, to));
		return in.file(in.text(a[1U]));
	}

	auto symlink(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const fb::path target = path_of(in, a[0U]);
		const fb::path link = path_of(in, a[1U]);
		static_cast<void>(link.unlink());
		static_cast<void>(link.symlink_to(target));
		return in.file(in.text(a[1U]));
	}

	auto remove(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		const fb::path p = path_of(in, a[0U]);
		return in.boolean(p.unlink() || p.rmdir());
	}

	auto touch(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		static_cast<void>(path_of(in, a[0U]).touch(0644U));
		return in.file(in.text(a[0U]));
	}

	auto is_dir(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		auto r = path_of(in, a[0U]).is_directory();
		return in.boolean(r && *r);
	}

	auto is_file(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		auto r = path_of(in, a[0U]).is_file();
		return in.boolean(r && *r);
	}

} // namespace

auto fb::plugin::api(void) noexcept -> ca::u32 {
	return fb::plugin_api;
}

auto fb::plugin::open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {

	using fb::ret;
	const fb::builtin builtins[] = {
		{"move", move, 2, 2, ret::file, 0U, "move(src, dst): rename, or copy and delete across filesystems", "Path, Path -> File"},
		{"symlink", symlink, 2, 2, ret::file, 0U, "symlink(target, link): create or replace a symlink", "Path, Path -> File"},
		{"remove", remove, 1, 1, ret::boolean, 0U, "remove(path): delete a file or an empty directory", "Path -> Bool"},
		{"touch", touch, 1, 1, ret::file, 0U, "touch(path): create if missing, bump the mtime", "Path -> File"},
		{"is_dir", is_dir, 1, 1, ret::boolean, 0U, "is_dir(path)", "Path -> Bool"},
		{"is_file", is_file, 1, 1, ret::boolean, 0U, "is_file(path)", "Path -> Bool"},
	};
	for (const fb::builtin& b : builtins)
		in.define(b);
	return {};
}
