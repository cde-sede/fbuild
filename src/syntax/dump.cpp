#include "syntax/dump.hpp"

#include "ca/print.hpp"

namespace fb::impl {

	[[nodiscard]] static auto dump_label(const fb::node* const n) noexcept -> ca::result<void> {

		auto printed = n->tok != nullptr && !n->tok->text.empty()
			? ca::print("{} {}\n", fb::node_name(n->kind), n->tok->text)
			: ca::print("{}\n", fb::node_name(n->kind));
		if (!printed)
			return printed.propagate();
		return {};
	}

	[[nodiscard]] static auto dump_kids(fb::memory& memory, const fb::node* const n, const ca::sv::str prefix) noexcept -> ca::result<void> {

		ca::usz total = 0U;
		for (const fb::node* const kid : n->kids)
			if (kid != nullptr)
				++total;
		ca::usz seen = 0U;
		for (const fb::node* const kid : n->kids) {
			if (kid == nullptr)
				continue;
			const bool last = ++seen == total;
			auto connector = ca::print("{}{}", prefix, last ? "└─ " : "├─ ");
			if (!connector)
				return connector.propagate();
			auto label = fb::impl::dump_label(kid);
			if (!label)
				return label;
			auto below = fb::impl::dump_kids(memory, kid, memory.format("{}{}", prefix, last ? "   " : "│  "));
			if (!below)
				return below;
		}
		return {};
	}

} // namespace fb::impl

auto fb::dump(fb::memory& memory, const fb::node* const root) noexcept -> ca::result<void> {

	if (root == nullptr)
		return {};
	auto label = fb::impl::dump_label(root);
	if (!label)
		return label;
	return fb::impl::dump_kids(memory, root, "");
}
