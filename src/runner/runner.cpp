#include "runner/runner.hpp"

#include "ca/machine.hpp"
#include "core/interrupt.hpp"
#include "runner/launcher.hpp"

#include "ca/environment.hpp"
#include "ca/file.hpp"
#include "ca/flush.hpp"
#include "ca/fs.hpp"
#include "ca/io.hpp"
#include "ca/print.hpp"
#include "ca/process.hpp"
#include "ca/span.hpp"
#include "ca/time.hpp"
#include "ca/unicode.hpp"
#include "runtime/interpreter.hpp"

namespace fb::impl {

	inline constexpr ca::sv::str reset = "\033[0m";
	inline constexpr ca::sv::str bold = "\033[1m";
	inline constexpr ca::sv::str grey = "\033[90m";
	inline constexpr ca::sv::str green = "\033[32m";
	inline constexpr ca::sv::str red = "\033[31m";
	inline constexpr ca::sv::str cyan = "\033[36m";

	inline constexpr ca::usz no_task = static_cast<ca::usz>(-1);

	enum class status : ca::u8 {
		scheduled,
		running,
		done,
		error,
		nested,
	};

	[[nodiscard]] static auto milliseconds(void) noexcept -> ca::i64 {
		return ca::sys::now(ca::sys::clock::monotonic) / 1000000;
	}

	/* A one-cell braille spinner, a frame every 80 ms. */
	[[nodiscard]] static auto spinner(const ca::i64 elapsed) noexcept -> ca::sv::str {

		constexpr ca::sv::str frames[10] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
		return frames[static_cast<ca::u64>(elapsed < 0 ? 0 : elapsed) / 80U % 10U];
	}

	/* The terminal columns text takes: escape sequences take none, a code point one. */
	[[nodiscard]] static auto columns(const ca::sv::str text) noexcept -> ca::usz {

		ca::usz count = 0U;
		for (ca::usz i = 0U; i < text.size(); ++i) {
			const char c = text.data()[i];
			if (c == '\033') {
				while (i + 1U < text.size() && !((text.data()[i + 1U] >= 'A' && text.data()[i + 1U] <= 'Z') || (text.data()[i + 1U] >= 'a' && text.data()[i + 1U] <= 'z')))
					++i;
				++i;
				continue;
			}
			if ((static_cast<ca::u8>(c) & 0xC0U) != 0x80U)
				++count;
		}
		return count;
	}

	/* Appends text to out, its escape sequences all and at most limit columns of the rest. */
	static auto clip(fb::text& out, const ca::sv::str text, const ca::usz limit) noexcept -> void {

		ca::usz count = 0U;
		ca::usz i = 0U;
		while (i < text.size()) {
			const char c = text.data()[i];
			if (c == '\033') {
				const ca::usz begin = i;
				while (i < text.size() && !((text.data()[i] >= 'A' && text.data()[i] <= 'Z') || (text.data()[i] >= 'a' && text.data()[i] <= 'z')))
					++i;
				if (i < text.size())
					++i;
				static_cast<void>(out.append(text.slice(begin, i - begin)));
				continue;
			}
			ca::usz units = 1U;
			while (i + units < text.size() && (static_cast<ca::u8>(text.data()[i + units]) & 0xC0U) == 0x80U)
				++units;
			if (count < limit)
				static_cast<void>(out.append(text.slice(i, units)));
			++count;
			i += units;
		}
	}

	[[nodiscard]] static auto exit_code(const ca::i32 pid) noexcept -> int {

		auto status = ca::sys::waitpid(pid);
		if (!status)
			return 1;
		if (auto code = status->exit_code())
			return *code;
		if (auto signal = status->signal())
			return 128 + *signal;
		return 1;
	}

} // namespace fb::impl

struct fb::runner::entry final {
	ca::sv::str display;
	fb::impl::status state;
	int code;
	fb::text output;
	ca::i64 started;
	ca::i64 finished;
	ca::usz task;
	ca::u64 frames;
	fb::runner::context* nested;
};

/* One line of the tree and what hangs under it: a command (e), a context (c), or the
   commands of one labelled task of the parallel context owner. */
struct fb::runner::node final {
	fb::runner::entry* e;
	fb::runner::context* c;
	const fb::runner::context* owner;
	ca::usz task;
};

/* How the commands under a node stand, and the time they span. */
struct fb::runner::tally final {
	ca::usz total;
	ca::usz done;
	ca::usz failed;
	ca::usz running;
	ca::i64 first;
	ca::i64 last;
};

struct fb::runner::slot final {
	fb::launch* launch;
	ca::fs::descriptor output;
	ca::usz index;
	bool started;
};

struct fb::runner::context final {
	bool parallel;
	ca::sv::str label;
	int depth;
	fb::runner::context* parent;
	fb::list<fb::runner::entry*> entries;
	fb::list<ca::sv::str> task_labels;
	bool failed;
	bool open;
	ca::i64 started;
	ca::i64 finished;
};

fb::runner::runner(fb::memory& memory, const fb::run_settings& settings) noexcept
: _memory{memory}, _settings{settings}, _current{nullptr}, _root{nullptr}, _records{memory.list<entry*>()},
  _drawn{0U}, _width{80U}, _height{24U}, _pass{0U}, _fold{false}, _tty{false}, _decided{false}, _spinner_text{nullptr}, _spinner_owner{nullptr}, _launcher{nullptr}, _scratch{}, _frame{} {
}

auto fb::runner::_launchers(void) noexcept -> ca::result<fb::launcher*> {

	if (_launcher != nullptr)
		return _launcher;
	ca::usz count = ca::sys::processor_count();
	count = count < 1U ? 1U : count > 8U ? 8U : count;
	auto made = fb::launcher::make(_memory, count);
	if (!made)
		return made.propagate();
	_launcher = *made;
	return *made;
}

auto fb::runner::_begin(char* const* const argv, const ca::usz index) noexcept -> ca::result<slot> {

	auto launcher = this->_launchers();
	if (!launcher)
		return launcher.propagate();
	auto ends = ca::sys::pipe(ca::fs::impl::CLOEXEC);
	if (!ends)
		return ends.propagate();
	fb::launch* const l = _memory.make<fb::launch>(argv, ca::fs::descriptor::adopt(ends->write_end()), ca::i32{-1}, ca::error{});
	slot s{l, ca::fs::descriptor::adopt(ends->read_end()), index, false};
	auto submitted = (*launcher)->submit(l);
	if (!submitted) {
		static_cast<void>(l->output.cleanup());
		static_cast<void>(s.output.cleanup());
		return submitted.propagate();
	}
	return s;
}

auto fb::runner::cleanup(void) noexcept -> ca::result<void> {

	if (_launcher == nullptr)
		return {};
	auto done = _launcher->cleanup();
	_launcher = nullptr;
	static_cast<void>(_frame.cleanup());
	if (_scratch.holds())
		static_cast<void>(_scratch.cleanup());
	return done;
}

auto fb::runner::_live(void) const noexcept -> bool {
	return _tty && !_settings.silent;
}

auto fb::runner::_print(const ca::sv::str text) noexcept -> void {

	static_cast<void>(ca::print("{}", text));
	static_cast<void>(ca::flush());
}

auto fb::runner::_entry(const ca::sv::str display, const ca::usz task) noexcept -> entry* {
	return _memory.make<entry>(display, fb::impl::status::scheduled, 0, fb::text{_memory.allocator()}, ca::i64{0}, ca::i64{0}, task, ca::u64{0U}, nullptr);
}

auto fb::runner::_fresh_scratch(void) noexcept -> void {

	if (!_scratch.holds()) {
		auto made = fb::memory::make();
		if (!made) [[unlikely]]
			ca::trap("fbuild: out of memory");
		_scratch = ca::move(*made);
		return;
	}
	_scratch.reset();
}

auto fb::runner::_flatten(context* const c, fb::list<node>& kids) noexcept -> void {

	ca::usz shown = fb::impl::no_task;
	for (entry* const e : c->entries) {
		if (e->state == fb::impl::status::nested) {
			context* const n = e->nested;
			if (n->label.empty() && !n->parallel)
				this->_flatten(n, kids);
			else
				_scratch.push(kids, node{nullptr, n, nullptr, 0U});
			continue;
		}
		if (c->parallel && !c->task_labels.data()[e->task].empty()) {
			if (e->task != shown)
				_scratch.push(kids, node{nullptr, nullptr, c, e->task});
			shown = e->task;
			continue;
		}
		_scratch.push(kids, node{e, nullptr, c, 0U});
	}
}

auto fb::runner::_children(const node& n, fb::list<node>& kids) noexcept -> void {

	if (n.c != nullptr) {
		this->_flatten(n.c, kids);
		return;
	}
	for (entry* const e : n.owner->entries)
		if (e->state != fb::impl::status::nested && e->task == n.task)
			_scratch.push(kids, node{e, nullptr, n.owner, n.task});
}

auto fb::runner::_count(const context* const c, const ca::usz task, tally& t) const noexcept -> void {

	for (const entry* const e : c->entries) {
		if (e->state == fb::impl::status::nested) {
			if (task == fb::impl::no_task)
				this->_count(e->nested, fb::impl::no_task, t);
			continue;
		}
		if (task != fb::impl::no_task && e->task != task)
			continue;
		++t.total;
		switch (e->state) {
			case fb::impl::status::done: ++t.done; break;
			case fb::impl::status::error: ++t.failed; break;
			case fb::impl::status::running: ++t.running; break;
			default: break;
		}
		if (e->started != 0 && (t.first == 0 || e->started < t.first))
			t.first = e->started;
		if (e->finished > t.last)
			t.last = e->finished;
	}
}

auto fb::runner::_duration(ca::i64 ms) noexcept -> ca::sv::str {

	if (ms < 0)
		ms = 0;
	if (ms < 1000)
		return _scratch.format("{}ms", ms);
	if (ms < 10000)
		return _scratch.format("{}.{}{}s", ms / 1000, ms % 1000 / 100, ms % 100 / 10);
	if (ms < 60000)
		return _scratch.format("{}.{}s", ms / 1000, ms % 1000 / 100);
	const ca::i64 s = ms / 1000;
	return _scratch.format("{}m{}{}s", s / 60, s % 60 / 10, s % 10);
}

auto fb::runner::_summary(const tally& t, const bool color) noexcept -> ca::sv::str {

	const ca::sv::str separator = color ? ca::sv::str{" · "} : ca::sv::str{", "};
	fb::text out{_scratch.allocator()};
	if (t.failed != 0)
		static_cast<void>(ca::format_append(out, "{}{} of {} failed{}", color ? fb::impl::red : ca::sv::str{""}, t.failed, t.total, color ? fb::impl::grey : ca::sv::str{""}));
	else
		static_cast<void>(ca::format_append(out, "{} step{}", t.total, t.total == 1U ? "" : "s"));
	const ca::usz skipped = t.total - t.done - t.failed - t.running;
	if (skipped != 0U)
		static_cast<void>(ca::format_append(out, "{}{} skipped", separator, skipped));
	return _scratch.copy(out.view());
}

auto fb::runner::_path(const context* const from) noexcept -> ca::sv::str {

	fb::list<ca::sv::str> labels = _scratch.list<ca::sv::str>();
	for (const context* c = from; c != nullptr; c = c->parent)
		if (!c->label.empty())
			_scratch.push(labels, c->label);
	fb::text out{_scratch.allocator()};
	for (ca::usz i = labels.size(); i != 0U; --i) {
		if (i != labels.size())
			static_cast<void>(out.append(" / "));
		static_cast<void>(out.append(labels.data()[i - 1U]));
	}
	return _scratch.copy(out.view());
}

auto fb::runner::_emit(fb::list<ca::sv::str>& lines, const ca::sv::str left, const ca::sv::str right) noexcept -> void {

	fb::text line{_scratch.allocator()};
	const ca::usz used = fb::impl::columns(left);
	const ca::usz wanted = fb::impl::columns(right);
	const ca::usz room = _width > 1U ? _width - 1U : 0U;
	if (!right.empty() && wanted + 8U <= room) {
		if (used + 2U + wanted <= room) {
			static_cast<void>(line.append(left));
			for (ca::usz i = used + wanted; i < room; ++i)
				static_cast<void>(line.push(' '));
			static_cast<void>(line.append(right));
		} else {
			fb::impl::clip(line, left, room - wanted - 3U);
			static_cast<void>(ca::format_append(line, "{}…  {}", fb::impl::reset, right));
		}
	} else if (used > room && room != 0U) {
		fb::impl::clip(line, left, room - 1U);
		static_cast<void>(ca::format_append(line, "{}…", fb::impl::reset));
	} else
		static_cast<void>(line.append(left));
	_scratch.push(lines, _scratch.copy(line.view()));
}

auto fb::runner::_render(const node& n, const ca::sv::str prefix, const bool last, const bool root, fb::list<ca::sv::str>& lines) noexcept -> void {

	using fb::impl::grey;
	using fb::impl::reset;
	const ca::sv::str branch = root ? prefix : _scratch.format("{}{}{}{}", prefix, grey, last ? "└─ " : "├─ ", reset);
	const ca::sv::str below = root ? prefix : _scratch.format("{}{}{}{}", prefix, grey, last ? "   " : "│  ", reset);
	if (n.e != nullptr) {
		this->_command(n.e, n.owner, branch, below, "", lines);
		return;
	}
	fb::list<node> kids = _scratch.list<node>();
	this->_children(n, kids);
	const ca::sv::str named = n.c == nullptr ? n.owner->task_labels.data()[n.task] : n.c->label;
	if (kids.size() == 1U && kids.data()[0].e != nullptr) {
		this->_command(kids.data()[0].e, kids.data()[0].owner, branch, below, named, lines);
		return;
	}
	const ca::sv::str label = !named.empty() ? named : ca::sv::str{n.c->parallel ? "parallel" : "run"};
	tally t{0U, 0U, 0U, 0U, 0, 0};
	this->_count(n.c != nullptr ? n.c : n.owner, n.c != nullptr ? fb::impl::no_task : n.task, t);
	bool closed = false;
	bool bad = false;
	ca::i64 started = t.first;
	ca::i64 finished = t.last;
	if (n.c != nullptr) {
		closed = !n.c->open;
		bad = n.c->failed;
		started = n.c->started;
		finished = n.c->finished;
	} else {
		closed = t.failed != 0U || t.done == t.total || !n.owner->open;
		bad = closed && t.done != t.total;
	}
	const ca::i64 now = fb::impl::milliseconds();
	fb::text right{_scratch.allocator()};
	if (!closed) {
		const bool counted = n.c == nullptr || n.c->parallel;
		if (counted)
			static_cast<void>(ca::format_append(right, "{}/{}", t.done + t.failed, t.total));
		if (started != 0 && now - started >= 1000)
			static_cast<void>(ca::format_append(right, "{}{}", counted ? " · " : "", this->_duration(now - started)));
	} else
		static_cast<void>(ca::format_append(right, "{} · {}", this->_summary(t, true), this->_duration(finished - started)));
	const ca::sv::str glyph = !closed ? _scratch.format("{}▾{}", fb::impl::cyan, reset)
		: bad ? _scratch.format("{}✗{}", fb::impl::red, reset) : _scratch.format("{}✓{}", fb::impl::green, reset);
	this->_emit(lines, _scratch.format("{}{} {}{}{}", branch, glyph, fb::impl::bold, label, reset),
		right.empty() ? ca::sv::str{""} : _scratch.format("{}{}{}", grey, right.view(), reset));
	if (closed && !bad && !_settings.verbose)
		return;
	if (_fold) {
		this->_folded(kids, below, lines);
		return;
	}
	for (ca::usz i = 0U; i < kids.size(); ++i)
		this->_render(kids.data()[i], below, i + 1U == kids.size(), false, lines);
}

/* The children of a node when the tree is taller than the terminal: the ones that succeeded
   become one "N done" line on top, the running and failed ones stay, the first few waiting
   ones show and the rest become one "N more waiting" (or "N skipped") line at the end. */
auto fb::runner::_folded(const fb::list<node>& kids, const ca::sv::str below, fb::list<ca::sv::str>& lines) noexcept -> void {

	using fb::impl::grey;
	using fb::impl::reset;
	enum class kind : ca::u8 { ok, waiting, skipped, shown };
	constexpr ca::usz waiting_shown = 2U;
	const auto classify = [&](const node& n) noexcept -> kind {
		if (n.e != nullptr) {
			switch (n.e->state) {
				case fb::impl::status::done: return kind::ok;
				case fb::impl::status::scheduled: return n.owner != nullptr && !n.owner->open ? kind::skipped : kind::waiting;
				default: return kind::shown;
			}
		}
		if (n.c != nullptr)
			return !n.c->open && !n.c->failed ? kind::ok : kind::shown;
		tally t{0U, 0U, 0U, 0U, 0, 0};
		this->_count(n.owner, n.task, t);
		if (t.done == t.total)
			return kind::ok;
		if (t.done + t.failed + t.running == 0U)
			return n.owner->open ? kind::waiting : kind::skipped;
		return kind::shown;
	};

	ca::usz ok = 0U;
	ca::usz waiting = 0U;
	ca::usz skipped = 0U;
	for (const node& n : kids)
		switch (classify(n)) {
			case kind::ok: ++ok; break;
			case kind::waiting: ++waiting; break;
			case kind::skipped: ++skipped; break;
			default: break;
		}

	/* What is drawn, in order: a node, or (n null) a summary line. */
	struct item final {
		const node* n;
		ca::sv::str line;
	};
	fb::list<item> items = _scratch.list<item>();
	if (ok > 1U)
		_scratch.push(items, item{nullptr, _scratch.format("{}✓{} {}{} done{}", fb::impl::green, reset, grey, ok, reset)});
	ca::usz listed = 0U;
	for (const node& n : kids) {
		const kind k = classify(n);
		if ((k == kind::ok && ok > 1U) || k == kind::skipped)
			continue;
		if (k == kind::waiting && waiting > waiting_shown + 1U && listed++ >= waiting_shown)
			continue;
		_scratch.push(items, item{&n, ""});
	}
	if (waiting > waiting_shown + 1U)
		_scratch.push(items, item{nullptr, _scratch.format("{}· {} more waiting{}", grey, waiting - waiting_shown, reset)});
	if (skipped != 0U)
		_scratch.push(items, item{nullptr, _scratch.format("{}· {} skipped{}", grey, skipped, reset)});

	for (ca::usz i = 0U; i < items.size(); ++i) {
		const bool last = i + 1U == items.size();
		const item& it = items.data()[i];
		if (it.n != nullptr) {
			this->_render(*it.n, below, last, false, lines);
			continue;
		}
		this->_emit(lines, _scratch.format("{}{}{}{}{}", below, grey, last ? "└─ " : "├─ ", reset, it.line), "");
	}
}

auto fb::runner::_command(entry* const e, const context* const owner, const ca::sv::str branch, const ca::sv::str below, const ca::sv::str label, fb::list<ca::sv::str>& lines) noexcept -> void {

	using fb::impl::grey;
	using fb::impl::reset;
	const ca::i64 now = fb::impl::milliseconds();
	ca::sv::str glyph = "";
	ca::sv::str right = "";
	switch (e->state) {
		case fb::impl::status::running: {
			/* A frame may render twice (unfolded, then folded); only the first advances the spinner. */
			const ca::u64 frame = _pass == 0U ? e->frames++ : e->frames - 1U;
			const ca::sv::str spin = _settings.spinner != nullptr && _spinner_text != nullptr
				? _spinner_text(_spinner_owner, frame) : fb::impl::spinner(now - e->started);
			glyph = _scratch.format("{}{}{}", fb::impl::cyan, spin, reset);
			if (now - e->started >= 1000)
				right = _scratch.format("{}{}{}", grey, this->_duration(now - e->started), reset);
			break;
		}
		case fb::impl::status::done:
			glyph = _scratch.format("{}✓{}", fb::impl::green, reset);
			right = _scratch.format("{}{}{}", grey, this->_duration(e->finished - e->started), reset);
			break;
		case fb::impl::status::error:
			glyph = _scratch.format("{}✗{}", fb::impl::red, reset);
			right = _scratch.format("{}exit {}{} · {}{}", fb::impl::red, e->code, grey, this->_duration(e->finished - e->started), reset);
			break;
		default:
			glyph = _scratch.format("{}·{}", grey, reset);
			if (owner != nullptr && !owner->open)
				right = _scratch.format("{}skipped{}", grey, reset);
			break;
	}
	ca::sv::str text = e->display;
	if (!label.empty() && label != e->display)
		text = _scratch.format("{}{}{} {}{}{}", fb::impl::bold, label, reset, grey, e->display, reset);
	else if (e->state == fb::impl::status::scheduled)
		text = _scratch.format("{}{}{}", grey, e->display, reset);
	this->_emit(lines, _scratch.format("{}{} {}", branch, glyph, text), right);
	if (e->state == fb::impl::status::running)
		this->_tail(lines, below, e);
}

auto fb::runner::_tail(fb::list<ca::sv::str>& lines, const ca::sv::str below, const entry* const e) noexcept -> void {

	ca::sv::str text = e->output.view();
	if (!text.empty() && text.data()[text.size() - 1U] == '\n')
		text = text.prefix(text.size() - 1U);
	if (text.empty() || _settings.tail_lines <= 0)
		return;
	ca::usz start = text.size();
	int wanted = _settings.tail_lines;
	while (start != 0U) {
		if (text.data()[start - 1U] == '\n' && --wanted == 0)
			break;
		--start;
	}
	for (ca::sv::str piece : text.suffix(text.size() - start).split("\n")) {
		if (const auto cr = piece.find_last('\r'))
			piece = piece.suffix(piece.size() - *cr - 1U);
		this->_emit(lines, _scratch.format("{}  {}{}{}", below, fb::impl::grey, piece, fb::impl::reset), "");
	}
}

auto fb::runner::_finished(const entry* const e) noexcept -> void {

	this->_fresh_scratch();
	const bool named = !_current->label.empty() && _current->label == e->display;
	ca::sv::str path = this->_path(named ? _current->parent : _current);
	if (_current->parallel) {
		const ca::sv::str task = _current->task_labels.data()[e->task];
		if (!task.empty())
			path = path.empty() ? task : _scratch.format("{} / {}", path, task);
	}
	fb::text line{_scratch.allocator()};
	static_cast<void>(ca::format_append(line, "{} {}{}{}", e->state == fb::impl::status::done ? "+" : "x", path, path.empty() ? "" : ": ", e->display));
	if (e->state == fb::impl::status::error)
		static_cast<void>(ca::format_append(line, "  exit {}", e->code));
	static_cast<void>(ca::format_append(line, "  {}\n", this->_duration(e->finished - e->started)));
	this->_print(line.view());
}

auto fb::runner::_draw(void) noexcept -> void {

	if (!this->_live() || _root == nullptr)
		return;
	this->_fresh_scratch();
	_width = 80U;
	_height = 24U;
	if (auto size = ca::sys::get_terminal_size(1); size) {
		if (size->columns() != 0U)
			_width = size->columns();
		if (size->rows() != 0U)
			_height = size->rows();
	}
	/* Redrawing moves the cursor up over the last frame, which only works while that frame
	   fits on the screen (with the line the cursor ends on). A taller tree folds, and stays
	   folded for the rest of the run so the display does not jump between the two; the last
	   frame stays put, so it may be taller, and a verbose one shows everything. */
	const bool final = !_root->open;
	const ca::usz budget = _height > 1U ? _height - 1U : 1U;
	if (final && _settings.verbose)
		_fold = false;
	_pass = 0U;
	fb::list<ca::sv::str> lines = _scratch.list<ca::sv::str>();
	this->_render(node{nullptr, _root, nullptr, 0U}, "", true, true, lines);
	if (!_fold && lines.size() > budget && !(final && _settings.verbose)) {
		_fold = true;
		_pass = 1U;
		lines = _scratch.list<ca::sv::str>();
		this->_render(node{nullptr, _root, nullptr, 0U}, "", true, true, lines);
	}
	if (!final && lines.size() > budget) {
		const ca::usz kept = budget > 1U ? budget - 1U : budget;
		fb::list<ca::sv::str> cut = _scratch.list<ca::sv::str>();
		for (ca::usz i = 0U; i < kept; ++i)
			_scratch.push(cut, lines.data()[i]);
		if (kept != budget)
			_scratch.push(cut, _scratch.format("{}… {} more lines{}", fb::impl::grey, lines.size() - kept, fb::impl::reset));
		lines = ca::move(cut);
	}
	fb::text out{_scratch.allocator()};
	if (_drawn != 0U)
		static_cast<void>(ca::format_append(out, "\033[{}A", _drawn));
	for (const ca::sv::str line : lines) {
		static_cast<void>(out.append("\r\033[K"));
		fb::impl::clip(out, line, _width > 1U ? _width - 1U : 0U);
		static_cast<void>(out.append("\n"));
	}
	if (lines.size() < _drawn)
		static_cast<void>(out.append("\033[J"));
	if (out.view() == _frame.view())
		return;
	_drawn = lines.size();
	this->_print(out.view());
	static_cast<void>(_frame.clear());
	if (!_frame.append(out.view())) [[unlikely]]
		ca::trap("fbuild: out of memory");
}

auto fb::runner::enter(const ca::sv::str label, const bool parallel) noexcept -> void {

	if (!_decided) {
		_tty = _settings.color == 1 || (_settings.color == 0 && ca::sys::isatty(1));
		_decided = true;
	}
	context* const c = _memory.make<context>(parallel, label, 0, _current, _memory.list<entry*>(), _memory.list<ca::sv::str>(), false, true,
		fb::impl::milliseconds(), ca::i64{0});
	if (_current != nullptr) {
		c->depth = _current->depth + 1;
		entry* const spacer = this->_entry(label, 0U);
		spacer->state = fb::impl::status::nested;
		spacer->nested = c;
		_memory.push(_current->entries, spacer);
	} else {
		_root = c;
		_drawn = 0U;
		_fold = false;
		if (this->_live())
			this->_print("\033[?25l");
	}
	_current = c;
	this->_draw();
}

auto fb::runner::leave(void) noexcept -> bool {

	context* const c = _current;
	c->open = false;
	c->finished = fb::impl::milliseconds();
	for (const entry* const e : c->entries)
		if (e->state == fb::impl::status::error || (e->state == fb::impl::status::nested && e->nested->failed))
			c->failed = true;
	if (!_settings.silent && !_tty && c->parallel)
		for (const entry* const e : c->entries)
			if (e->state == fb::impl::status::done || e->state == fb::impl::status::error)
				this->_finished(e);
	_current = c->parent;
	if (!_settings.silent && !_tty && c == _root) {
		this->_fresh_scratch();
		tally t{0U, 0U, 0U, 0U, 0, 0};
		this->_count(c, fb::impl::no_task, t);
		if (t.total > 1U)
			this->_print(_scratch.format("{} {}: {}  {}\n", c->failed ? "x" : "+", c->label.empty() ? ca::sv::str{"run"} : c->label,
			this->_summary(t, false), this->_duration(c->finished - c->started)));
	}
	this->_draw();
	if (c == _root) {
		if (this->_live())
			this->_print("\033[?25h");
		_root = nullptr;
		_drawn = 0U;
	}
	for (entry* const e : c->entries)
		if (e->state != fb::impl::status::nested && !(_settings.silent && e->code == 0) && !e->output.empty())
			_memory.push(_records, e);
	return !c->failed;
}

auto fb::runner::_run(fb::list<entry*>& entries, fb::list<const fb::command*>& commands, fb::list<ca::usz>& task_of, const ca::usz tasks) noexcept -> bool {

	struct task_state final {
		ca::usz next;
		bool running;
		bool stopped;
	};
	fb::list<task_state> state = _memory.list<task_state>();
	for (ca::usz t = 0U; t < tasks; ++t)
		_memory.push(state, task_state{0U, false, false});
	fb::list<slot> running = _memory.list<slot>();
	fb::list<ca::sys::poll_entry> watched = _memory.list<ca::sys::poll_entry>();
	fb::list<ca::usz> watched_slot = _memory.list<ca::usz>();
	fb::list<ca::usz> finished = _memory.list<ca::usz>();
	bool all_ok = true;
	const ca::usz limit = _settings.jobs > 0 ? static_cast<ca::usz>(_settings.jobs) : 0U;

	auto next_of = [&](const ca::usz t) noexcept -> ca::usz {
		ca::usz seen = 0U;
		for (ca::usz i = 0U; i < commands.size(); ++i)
			if (task_of.data()[i] == t && seen++ == state.data()[t].next)
				return i;
		return commands.size();
	};
	auto failed = [&](entry* const e, const int code) noexcept -> void {
		e->code = code;
		e->state = code == 0 ? fb::impl::status::done : fb::impl::status::error;
		e->finished = fb::impl::milliseconds();
		if (!_tty && !_settings.silent && !_current->parallel)
			this->_finished(e);
		task_state& s = state.data()[e->task];
		s.running = false;
		if (code != 0) {
			all_ok = false;
			if (!_settings.keep_going)
				s.stopped = true;
		}
	};

	bool killed = false;
	for (;;) {
		if (fb::interrupt::pending() != 0) {
			all_ok = false;
			for (task_state& s : state)
				s.stopped = true;
			if (!killed) {
				for (const slot& r : running)
					if (r.started)
						static_cast<void>(ca::sys::kill(r.launch->pid, ca::sys::signals::terminate));
				killed = true;
			}
		}
		for (ca::usz t = 0U; t < tasks; ++t) {
			task_state& s = state.data()[t];
			if (s.running || s.stopped || (limit != 0U && running.size() >= limit))
				continue;
			const ca::usz index = next_of(t);
			if (index >= commands.size()) {
				s.stopped = true;
				continue;
			}
			entry* const e = entries.data()[index];
			auto begun = this->_begin(commands.data()[index]->argv, index);
			if (!begun && begun.error() == ca::errc::no_space)
				break;
			e->state = fb::impl::status::running;
			e->started = fb::impl::milliseconds();
			if (_settings.silent)
				this->_print(_memory.format("{}\n", e->display));
			++s.next;
			if (!begun) {
				static_cast<void>(ca::format_append(e->output, "fbuild: cannot run '{}': {}\n", commands.data()[index]->argv[0], ca::sys::error_message(begun.error())));
				failed(e, 127);
				continue;
			}
			s.running = true;
			_memory.push(running, ca::move(*begun));
		}
		if (running.empty())
			break;
		this->_draw();
		static_cast<void>(watched.clear());
		static_cast<void>(watched_slot.clear());
		_memory.push(watched, ca::sys::poll_entry{_launcher->wake(), ca::sys::poll_events::readable});
		_memory.push(watched_slot, ca::usz{0U});
		if (!killed) {
			_memory.push(watched, ca::sys::poll_entry{fb::interrupt::descriptor(), ca::sys::poll_events::readable});
			_memory.push(watched_slot, ca::usz{0U});
		}
		for (ca::usz i = 0U; i < running.size(); ++i)
			if (running.data()[i].started) {
				_memory.push(watched, ca::sys::poll_entry{running.data()[i].output.native(), ca::sys::poll_events::readable});
				_memory.push(watched_slot, i);
			}
		auto ready = ca::sys::poll(ca::span<ca::sys::poll_entry>{watched.data(), watched.size()}, this->_live() ? static_cast<ca::i32>(_settings.spinner_ms) : -1);
		if (!ready)
			continue;
		static_cast<void>(finished.clear());
		for (ca::usz w = killed ? 1U : 2U; w < watched.size(); ++w) {
			if (watched.data()[w].reported() == 0)
				continue;
			slot& r = running.data()[watched_slot.data()[w]];
			entry* const e = entries.data()[r.index];
			ca::byte buffer[4096];
			auto got = r.output.read(ca::span<ca::byte>{buffer, sizeof buffer});
			if (got && *got != 0U) {
				static_cast<void>(e->output.append(ca::sv::str{reinterpret_cast<const char*>(buffer), *got}));
				continue;
			}
			static_cast<void>(r.output.cleanup());
			failed(e, fb::impl::exit_code(r.launch->pid));
			_memory.push(finished, watched_slot.data()[w]);
		}
		if (watched.data()[0].reported() != 0)
			for (auto done = _launcher->collect(); done; done = _launcher->collect())
				for (ca::usz i = 0U; i < running.size(); ++i) {
					slot& r = running.data()[i];
					if (r.launch != *done)
						continue;
					static_cast<void>(r.launch->output.cleanup());
					if (r.launch->error.ok()) {
						r.started = true;
						if (killed)
							static_cast<void>(ca::sys::kill(r.launch->pid, ca::sys::signals::terminate));
						break;
					}
					entry* const e = entries.data()[r.index];
					static_cast<void>(ca::format_append(e->output, "fbuild: cannot run '{}': {}\n", r.launch->argv[0], ca::sys::error_message(r.launch->error)));
					static_cast<void>(r.output.cleanup());
					failed(e, 127);
					_memory.push(finished, i);
					break;
				}
		for (ca::usz k = finished.size(); k != 0U; --k) {
			ca::usz largest = 0U;
			for (ca::usz j = 1U; j < finished.size(); ++j)
				if (finished.data()[j] > finished.data()[largest])
					largest = j;
			static_cast<void>(running.swap_pop(finished.data()[largest]));
			static_cast<void>(finished.swap_pop(largest));
		}
	}
	return all_ok;
}

auto fb::runner::sequence(const fb::list<fb::command>& commands) noexcept -> bool {

	fb::list<entry*> entries = _memory.list<entry*>();
	fb::list<const fb::command*> pointers = _memory.list<const fb::command*>();
	fb::list<ca::usz> task_of = _memory.list<ca::usz>();
	for (const fb::command& c : commands) {
		entry* const e = this->_entry(c.display, 0U);
		_memory.push(_current->entries, e);
		_memory.push(entries, e);
		_memory.push(pointers, &c);
		_memory.push(task_of, ca::usz{0U});
	}
	return this->_run(entries, pointers, task_of, 1U);
}

auto fb::runner::parallel(const fb::list<fb::task>& tasks) noexcept -> bool {

	fb::list<entry*> entries = _memory.list<entry*>();
	fb::list<const fb::command*> pointers = _memory.list<const fb::command*>();
	fb::list<ca::usz> task_of = _memory.list<ca::usz>();
	for (ca::usz t = 0U; t < tasks.size(); ++t) {
		_memory.push(_current->task_labels, tasks.data()[t].label);
		const ca::usz index = _current->task_labels.size() - 1U;
		for (const fb::command& c : tasks.data()[t].commands) {
			entry* const e = this->_entry(c.display, index);
			_memory.push(_current->entries, e);
			_memory.push(entries, e);
			_memory.push(pointers, &c);
			_memory.push(task_of, t);
		}
	}
	return this->_run(entries, pointers, task_of, tasks.size());
}

auto fb::runner::capture(const fb::command& command) noexcept -> fb::captured {

	auto begun = this->_begin(command.argv, 0U);
	if (!begun)
		return fb::captured{"", 127};
	for (bool waiting = true; waiting;) {
		ca::sys::poll_entry wake{_launcher->wake(), ca::sys::poll_events::readable};
		if (!ca::sys::poll(ca::span<ca::sys::poll_entry>{&wake, 1U}, -1))
			continue;
		for (auto done = _launcher->collect(); done; done = _launcher->collect())
			if (*done == begun->launch)
				waiting = false;
	}
	static_cast<void>(begun->launch->output.cleanup());
	if (!begun->launch->error.ok()) {
		static_cast<void>(begun->output.cleanup());
		return fb::captured{"", 127};
	}
	fb::text output{_memory.allocator()};
	ca::byte buffer[4096];
	bool killed = false;
	for (;;) {
		ca::sys::poll_entry watched[2] = {
			ca::sys::poll_entry{begun->output.native(), ca::sys::poll_events::readable},
			ca::sys::poll_entry{fb::interrupt::descriptor(), ca::sys::poll_events::readable},
		};
		if (!ca::sys::poll(ca::span<ca::sys::poll_entry>{watched, killed ? 1U : 2U}, -1))
			continue;
		if (!killed && fb::interrupt::pending() != 0) {
			static_cast<void>(ca::sys::kill(begun->launch->pid, ca::sys::signals::terminate));
			killed = true;
		}
		if (watched[0].reported() == 0)
			continue;
		auto got = begun->output.read(ca::span<ca::byte>{buffer, sizeof buffer});
		if (!got || *got == 0U)
			break;
		static_cast<void>(output.append(ca::sv::str{reinterpret_cast<const char*>(buffer), *got}));
	}
	static_cast<void>(begun->output.cleanup());
	const int code = fb::impl::exit_code(begun->launch->pid);
	return fb::captured{_memory.copy(output.view()), code};
}


auto fb::runner::flush_outputs(void) noexcept -> void {

	using fb::impl::reset;
	for (int pass = 0; pass < 2; ++pass)
		for (const entry* const e : _records) {
			const bool bad = e->code != 0;
			if (bad != (pass == 1))
				continue;
			this->_fresh_scratch();
			fb::text out{_scratch.allocator()};
			if (_tty && bad)
				static_cast<void>(ca::format_append(out, "{}✗{} {}{}{}  {}exit {}{}\n", fb::impl::red, reset, fb::impl::bold, e->display, reset, fb::impl::red, e->code, reset));
			else if (_tty)
				static_cast<void>(ca::format_append(out, "{}✓{} {}{}{}\n", fb::impl::green, reset, fb::impl::bold, e->display, reset));
			else if (bad)
				static_cast<void>(ca::format_append(out, "--- {} (exit {})\n", e->display, e->code));
			else
				static_cast<void>(ca::format_append(out, "--- {}\n", e->display));
			ca::sv::str text = e->output.view();
			if (!text.empty() && text.data()[text.size() - 1U] == '\n')
				text = text.prefix(text.size() - 1U);
			for (const ca::sv::str piece : text.split("\n")) {
				if (_tty)
					static_cast<void>(ca::format_append(out, "{}│{} ", fb::impl::grey, reset));
				static_cast<void>(ca::format_append(out, "{}\n", piece));
			}
			this->_print(out.view());
		}
	static_cast<void>(_records.clear());
}
