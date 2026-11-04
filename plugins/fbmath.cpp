// fbmath: the `math` namespace object (abs, sign, min, max, pow, sqrt, floor, ceil, round; pi, e).
// Ported from cparser's plugins/fbmath.c. Methods receive the namespace object first.
#include "3-system/math/impl/exact_math.hpp"
#include "support.hpp"

namespace {

	namespace s = fb::support;

	/* x to the power y without libm, while ca's runtime has no pow: 2^(y log2 x) on the x87, in
	   80-bit precision, then rounded to double (within an ulp). */
	auto power(const ca::f64 x, const ca::f64 y) noexcept -> ca::f64 {

		if (y == 0.0)
			return 1.0;
		if (x == 0.0)
			return y > 0.0 ? 0.0 : 1.0 / 0.0;
		bool negate = false;
		ca::f64 base = x;
		if (x < 0.0) {
			if (ca::sys::impl::trunc(y) != y)
				return 0.0 / 0.0;
			negate = ca::sys::impl::fmod(y, 2.0) != 0.0;
			base = -x;
		}
		long double exponent;
		__asm__("fyl2x" : "=t"(exponent) : "0"(static_cast<long double>(base)), "u"(static_cast<long double>(y)) : "st(1)");
		long double whole;
		__asm__("frndint" : "=t"(whole) : "0"(exponent));
		long double fraction;
		__asm__("f2xm1" : "=t"(fraction) : "0"(exponent - whole));
		long double scaled;
		__asm__("fscale" : "=t"(scaled) : "0"(fraction + 1.0L), "u"(whole));
		const ca::f64 r = static_cast<ca::f64>(scaled);
		return negate ? -r : r;
	}

	auto numeric(fb::interpreter& in, const fb::arguments a, const ca::usz count, const ca::sv::str name) noexcept -> ca::result<void, fb::diagnostic> {

		if (a.size() < count + 1U)
			return in.fail("math.{}: expected {} argument{}", name, count, count == 1U ? "" : "s");
		for (ca::usz i = 1U; i < a.size(); ++i)
			if (!s::is_number(a[i]))
				return in.fail("math.{}: expected Int or Float", name);
		return {};
	}

	auto m_abs(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = numeric(in, a, 1U, "abs"); !ok)
			return ok.propagate();
		if (a[1U]->is(fb::kind::floating))
			return in.floating(a[1U]->floating < 0.0 ? -a[1U]->floating : a[1U]->floating);
		return in.integer(a[1U]->integer < 0 ? -a[1U]->integer : a[1U]->integer);
	}

	auto m_sign(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = numeric(in, a, 1U, "sign"); !ok)
			return ok.propagate();
		if (a[1U]->is(fb::kind::floating)) {
			const ca::f64 v = a[1U]->floating;
			return in.floating(v > 0.0 ? 1.0 : v < 0.0 ? -1.0 : 0.0);
		}
		const ca::i64 v = a[1U]->integer;
		return in.integer(v > 0 ? 1 : v < 0 ? -1 : 0);
	}

	auto extreme(fb::interpreter& in, const fb::arguments a, const bool largest) noexcept -> fb::outcome {

		if (a.size() < 2U)
			return in.fail("math.{}: expected at least 1 argument", largest ? "max" : "min");
		if (auto ok = numeric(in, a, 1U, largest ? "max" : "min"); !ok)
			return ok.propagate();
		fb::value* best = a[1U];
		for (ca::usz i = 2U; i < a.size(); ++i)
			if (largest ? s::floating(a[i]) > s::floating(best) : s::floating(a[i]) < s::floating(best))
				best = a[i];
		return best;
	}

	auto m_min(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return extreme(in, a, false);
	}

	auto m_max(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return extreme(in, a, true);
	}

	auto m_pow(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = numeric(in, a, 2U, "pow"); !ok)
			return ok.propagate();
		if (!a[1U]->is(fb::kind::floating) && !a[2U]->is(fb::kind::floating) && a[2U]->integer >= 0) {
			ca::u64 r = 1U;
			for (ca::i64 i = 0; i < a[2U]->integer; ++i)
				r *= static_cast<ca::u64>(a[1U]->integer);
			return in.integer(static_cast<ca::i64>(r));
		}
		return in.floating(power(s::floating(a[1U]), s::floating(a[2U])));
	}

	auto m_sqrt(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {

		if (auto ok = numeric(in, a, 1U, "sqrt"); !ok)
			return ok.propagate();
		return in.floating(ca::sys::impl::sqrt(s::floating(a[1U])));
	}

	template <auto (*rounding)(ca::f64) noexcept -> ca::f64>
	auto rounded(fb::interpreter& in, const fb::arguments a, const ca::sv::str name) noexcept -> fb::outcome {

		if (auto ok = numeric(in, a, 1U, name); !ok)
			return ok.propagate();
		if (!a[1U]->is(fb::kind::floating))
			return in.integer(a[1U]->integer);
		return in.integer(static_cast<ca::i64>(rounding(a[1U]->floating)));
	}

	auto m_floor(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return rounded<ca::sys::impl::floor<ca::f64>>(in, a, "floor");
	}

	auto m_ceil(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return rounded<ca::sys::impl::ceil<ca::f64>>(in, a, "ceil");
	}

	auto m_round(fb::interpreter& in, const fb::arguments a) noexcept -> fb::outcome {
		return rounded<ca::sys::impl::round<ca::f64>>(in, a, "round");
	}

	auto not_callable(fb::interpreter& in, const fb::arguments) noexcept -> fb::outcome {
		return in.fail("math is a namespace object; call e.g. math.sqrt(2), not math(...)");
	}

} // namespace

auto fb::plugin::api(void) noexcept -> ca::u32 {
	return fb::plugin_api;
}

auto fb::plugin::open(fb::interpreter& in) noexcept -> ca::result<void, fb::diagnostic> {

	fb::class_info* const math = in.class_named("math");
	in.method(math, "abs", m_abs);
	in.method(math, "sign", m_sign);
	in.method(math, "min", m_min);
	in.method(math, "max", m_max);
	in.method(math, "pow", m_pow);
	in.method(math, "sqrt", m_sqrt);
	in.method(math, "floor", m_floor);
	in.method(math, "ceil", m_ceil);
	in.method(math, "round", m_round);
	fb::value* const object = in.object(math, nullptr);
	in.set(object, "pi", in.floating(3.14159265358979323846));
	in.set(object, "e", in.floating(2.71828182845904523536));
	in.define({"math", not_callable, 0, -1, fb::ret::any, fb::global,
		"math namespace (fbmath): abs, sign, min, max, pow, sqrt, floor, ceil, round; math.pi, math.e"});
	in.bind("math", object);
	return {};
}
