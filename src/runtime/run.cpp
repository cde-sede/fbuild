#include "runtime/interpreter.hpp"

#include "ca/flush.hpp"
#include "ca/print.hpp"
#include "core/interrupt.hpp"
#include "runner/runner.hpp"

namespace fb::impl {

	[[nodiscard]] static auto has_command(const fb::value* const v) noexcept -> bool {

		if (v == nullptr)
			return false;
		switch (v->kind) {
			case fb::kind::command:
				return v->size() != 0U;
			case fb::kind::seq:
				return true;
			case fb::kind::list:
			case fb::kind::tuple:
				for (const fb::value* const item : v->items)
					if (fb::impl::has_command(item))
						return true;
				return false;
			default:
				return false;
		}
	}

	[[nodiscard]] static auto command_count(const fb::value* const v) noexcept -> ca::usz {

		if (v == nullptr)
			return 0U;
		switch (v->kind) {
			case fb::kind::command:
				return v->size() != 0U ? 1U : 0U;
			case fb::kind::seq:
				return 2U;
			case fb::kind::list:
			case fb::kind::tuple: {
				ca::usz n = 0U;
				for (ca::usz i = 0U; i < v->size() && n < 2U; ++i)
					n += fb::impl::command_count(v->at(i));
				return n > 2U ? 2U : n;
			}
			default:
				return 0U;
		}
	}

	[[nodiscard]] static auto labeled_pair(const fb::value* const v) noexcept -> bool {
		return v != nullptr && v->is(fb::kind::tuple) && v->size() == 2U && v->at(0U)->is(fb::kind::string) && fb::impl::has_command(v->at(1U));
	}

	static auto spinner_text(void* const owner, const ca::u64 frame) noexcept -> ca::sv::str {

		fb::interpreter& in = *static_cast<fb::interpreter*>(owner);
		auto text = in.apply(in.settings().spinner, in.integer(static_cast<ca::i64>(frame)));
		return text ? in.text(*text) : ca::sv::str{""};
	}

} // namespace fb::impl

auto fb::interpreter::emit(const ca::sv::str line) noexcept -> void {

	if (_in_run) {
		_memory.push(_deferred, _memory.copy(line));
		return;
	}
	static_cast<void>(ca::print("{}\n", line));
}

auto fb::interpreter::_flush_deferred(void) noexcept -> void {

	for (const ca::sv::str line : _deferred)
		static_cast<void>(ca::print("{}\n", line));
	static_cast<void>(_deferred.clear());
	static_cast<void>(ca::flush());
}

auto fb::interpreter::flush_outputs(void) noexcept -> void {

	static_cast<void>(ca::flush());
	_runner->flush_outputs();
}

auto fb::interpreter::cleanup(void) noexcept -> ca::result<void> {
	return _runner->cleanup();
}

auto fb::interpreter::_collect(fb::value* const v, void* const into) noexcept -> ca::result<void, fb::diagnostic> {

	fb::list<fb::command>& commands = *static_cast<fb::list<fb::command>*>(into);
	if (v == nullptr)
		return {};
	switch (v->kind) {
		case fb::kind::command: {
			char** const argv = static_cast<char**>(_memory.bytes((v->size() + 1U) * sizeof(char*), alignof(char*)));
			fb::text display{_memory.allocator()};
			for (ca::usz i = 0U; i < v->size(); ++i) {
				const ca::sv::str word = _memory.copy(this->text(v->at(i)));
				argv[i] = const_cast<char*>(word.data());
				if (i != 0U)
					static_cast<void>(display.push(' '));
				const fb::value* const item = v->at(i);
				static_cast<void>(display.append(item->is(fb::kind::string) && !item->other.empty() ? item->other : word));
			}
			argv[v->size()] = nullptr;
			_memory.push(commands, fb::command{_memory.copy(display.view()), argv});
			return {};
		}
		case fb::kind::tuple:
			if (fb::impl::labeled_pair(v)) {
				const ca::usz before = commands.size();
				auto inner = this->_collect(v->at(1U), into);
				if (!inner)
					return inner;
				if (commands.size() == before + 1U)
					commands.data()[before].display = v->at(0U)->text;
				return {};
			}
			[[fallthrough]];
		case fb::kind::list:
			for (fb::value* const item : v->items) {
				auto inner = this->_collect(item, into);
				if (!inner)
					return inner;
			}
			return {};
		case fb::kind::seq:
			for (const fb::node* const step : v->body->kids) {
				auto evaluated = this->_eval(step, v->scope);
				if (!evaluated)
					return evaluated.propagate();
				auto inner = this->_collect(*evaluated, into);
				if (!inner)
					return inner;
			}
			return {};
		default:
			return {};
	}
}

auto fb::interpreter::_exec(fb::value* v) noexcept -> ca::result<bool, fb::diagnostic> {

	if (!fb::impl::has_command(v))
		return true;
	ca::sv::str label = v->label;
	if (fb::impl::labeled_pair(v) && fb::impl::command_count(v->at(1U)) == 1U) {
		fb::list<fb::command> commands = _memory.list<fb::command>();
		auto collected = this->_collect(v, &commands);
		if (!collected)
			return collected.propagate();
		_runner->enter(label, false);
		const bool ok = _runner->sequence(commands);
		return _runner->leave() && ok;
	}
	if (fb::impl::labeled_pair(v)) {
		label = v->at(0U)->text;
		v = v->at(1U);
	}
	switch (v->kind) {
		case fb::kind::seq: {
			_runner->enter(label, false);
			bool ok = true;
			for (const fb::node* const step : v->body->kids) {
				auto evaluated = this->_eval(step, v->scope);
				if (!evaluated) {
					static_cast<void>(_runner->leave());
					return evaluated.propagate();
				}
				auto ran = this->_exec(*evaluated);
				if (!ran) {
					static_cast<void>(_runner->leave());
					return ran;
				}
				if (!*ran) {
					ok = false;
					if (!_settings.keep_going)
						break;
				}
			}
			return _runner->leave() && ok;
		}
		case fb::kind::list:
		case fb::kind::tuple: {
			fb::list<fb::task> tasks = _memory.list<fb::task>();
			for (fb::value* const item : v->items) {
				if (!fb::impl::has_command(item))
					continue;
				fb::list<fb::command> commands = _memory.list<fb::command>();
				auto collected = this->_collect(item, &commands);
				if (!collected)
					return collected.propagate();
				ca::sv::str shown = "";
				if (fb::impl::labeled_pair(item))
					shown = fb::impl::command_count(item->at(1U)) == 1U ? ca::sv::str{""} : item->at(0U)->text;
				else if (commands.size() > 1U)
					shown = item->is(fb::kind::command) ? this->text(item->at(0U)) : !item->label.empty() ? item->label : ca::sv::str{"job"};
				_memory.push(tasks, fb::task{shown, ca::move(commands)});
			}
			_runner->enter(label, true);
			const bool ok = _runner->parallel(tasks);
			return _runner->leave() && ok;
		}
		case fb::kind::command: {
			fb::list<fb::command> commands = _memory.list<fb::command>();
			auto collected = this->_collect(v, &commands);
			if (!collected)
				return collected.propagate();
			_runner->enter(label, false);
			const bool ok = _runner->sequence(commands);
			return _runner->leave() && ok;
		}
		default:
			return true;
	}
}

auto fb::interpreter::execute(fb::value* const target) noexcept -> ca::result<bool, fb::diagnostic> {

	if (!fb::impl::has_command(target))
		return false;
	_runner->set_spinner(fb::impl::spinner_text, this);
	_in_run = true;
	auto ran = this->_exec(target);
	_in_run = false;
	this->_flush_deferred();
	if (!ran)
		return ran.propagate();
	if (fb::interrupt::pending() != 0)
		return this->fail("interrupted by user");
	if (!*ran)
		_failed = true;
	return true;
}

auto fb::interpreter::_capture_one(fb::value* const v) noexcept -> fb::outcome {

	fb::list<fb::command> commands = _memory.list<fb::command>();
	auto collected = this->_collect(v, &commands);
	if (!collected)
		return collected.propagate();
	fb::text output{_memory.allocator()};
	_capture_failed = false;
	for (const fb::command& c : commands) {
		if (fb::interrupt::pending() != 0)
			return this->fail("interrupted by user");
		const fb::captured got = _runner->capture(c);
		static_cast<void>(output.append(got.output));
		if (got.code != 0)
			_capture_failed = true;
	}
	ca::sv::str text = output.view();
	while (!text.empty() && (text.data()[text.size() - 1U] == '\n' || text.data()[text.size() - 1U] == '\r'))
		text = text.prefix(text.size() - 1U);
	return this->string(text);
}

auto fb::interpreter::capture(fb::value* const v) noexcept -> fb::outcome {

	if (v == nullptr)
		return this->string("");
	switch (v->kind) {
		case fb::kind::tuple:
		case fb::kind::list: {
			fb::value* const out = this->make(v->kind);
			for (fb::value* const item : v->items) {
				auto got = this->capture(item);
				if (!got)
					return got;
				this->push(out, *got);
			}
			return out;
		}
		case fb::kind::seq: {
			fb::value* const out = this->make(fb::kind::tuple);
			for (const fb::node* const step : v->body->kids) {
				auto evaluated = this->_eval(step, v->scope);
				if (!evaluated)
					return evaluated;
				auto got = this->capture(*evaluated);
				if (!got)
					return got;
				this->push(out, *got);
			}
			return out;
		}
		case fb::kind::command:
			return this->_capture_one(v);
		default:
			return this->string("");
	}
}
