/* fbmath -- math functions for fbuild, as a plugin (.so).
 *
 * Build: cc -shared -fPIC -Isrc plugins/fbmath.c -o build/fbmath.so
 * Load:  FBUILD_PLUGINS=build/fbmath.so ./fbuild   (or list several, colon-sep)
 *
 * Bound as the bare namespace object `math` (fb_global_bind, same pattern as
 * fbregex's `regex`) so it needs no construction step: math.sqrt(2), etc.
 * Methods are dispatched as (self, ...args) -- a[0] is always the (stateless)
 * math object itself, real arguments start at a[1].
 *
 * Int/Float handling: every function accepts either kind, and mixes of them
 * across arguments (min/max/pow), following the same promotion rule the core
 * language uses for `+`/`*`/comparisons -- any Float operand makes the whole
 * op Float; pure-Int operands stay Int. abs/sign/min/max are type-preserving
 * (an Int in yields an Int out); sqrt is always Float (irrational in general);
 * floor/ceil/round always narrow to Int (that's their point).
 */
#include <math.h>

#include "fbuild.h"

static int is_num(fb_Value v) { fb_Kind k = fb_kind(v); return k == FB_INT || k == FB_FLOAT; }
static int is_flt(fb_Value v) { return fb_kind(v) == FB_FLOAT; }

static fb_Value m_abs(fb_State *S, fb_Value *a, int n) {
	if (n < 2) return fb_error(S, "math.abs: expected 1 argument");
	if (!is_num(a[1])) return fb_error(S, "math.abs: expected Int or Float");
	if (is_flt(a[1])) { double v = fb_tofloat(a[1]); return fb_float(S, v < 0 ? -v : v); }
	long v = fb_toint(a[1]); return fb_int(S, v < 0 ? -v : v);
}

static fb_Value m_sign(fb_State *S, fb_Value *a, int n) {
	if (n < 2) return fb_error(S, "math.sign: expected 1 argument");
	if (!is_num(a[1])) return fb_error(S, "math.sign: expected Int or Float");
	if (is_flt(a[1])) { double v = fb_tofloat(a[1]); return fb_float(S, v > 0 ? 1.0 : (v < 0 ? -1.0 : 0.0)); }
	long v = fb_toint(a[1]); return fb_int(S, v > 0 ? 1 : (v < 0 ? -1 : 0));
}

/* min/max: variadic (1+ args after self), keeps the winning argument's own
 * value (so an all-Int call stays Int, an all-Float call stays Float, and a
 * mixed call returns whichever original value actually won -- no forced
 * promotion of the result itself, only of the comparison used to pick it). */
static fb_Value m_min(fb_State *S, fb_Value *a, int n) {
	if (n < 2) return fb_error(S, "math.min: expected at least 1 argument");
	if (!is_num(a[1])) return fb_error(S, "math.min: expected Int or Float");
	fb_Value best = a[1];
	for (int i = 2; i < n; i++) {
		if (!is_num(a[i])) return fb_error(S, "math.min: expected Int or Float");
		if (fb_tofloat(a[i]) < fb_tofloat(best)) best = a[i];
	}
	return best;
}
static fb_Value m_max(fb_State *S, fb_Value *a, int n) {
	if (n < 2) return fb_error(S, "math.max: expected at least 1 argument");
	if (!is_num(a[1])) return fb_error(S, "math.max: expected Int or Float");
	fb_Value best = a[1];
	for (int i = 2; i < n; i++) {
		if (!is_num(a[i])) return fb_error(S, "math.max: expected Int or Float");
		if (fb_tofloat(a[i]) > fb_tofloat(best)) best = a[i];
	}
	return best;
}

/* pow: Int**Int with a non-negative exponent stays exact Int (repeated
 * multiplication); anything with a Float operand, or a negative Int
 * exponent, falls back to libm pow() and returns Float. */
static fb_Value m_pow(fb_State *S, fb_Value *a, int n) {
	if (n < 3) return fb_error(S, "math.pow: expected 2 arguments");
	if (!is_num(a[1]) || !is_num(a[2])) return fb_error(S, "math.pow: expected Int or Float");
	if (!is_flt(a[1]) && !is_flt(a[2])) {
		long base = fb_toint(a[1]), exp = fb_toint(a[2]);
		if (exp >= 0) {
			long r = 1;
			for (long i = 0; i < exp; i++) r *= base;
			return fb_int(S, r);
		}
	}
	return fb_float(S, pow(fb_tofloat(a[1]), fb_tofloat(a[2])));
}

static fb_Value m_sqrt(fb_State *S, fb_Value *a, int n) {
	if (n < 2) return fb_error(S, "math.sqrt: expected 1 argument");
	if (!is_num(a[1])) return fb_error(S, "math.sqrt: expected Int or Float");
	return fb_float(S, sqrt(fb_tofloat(a[1])));
}

static fb_Value m_floor(fb_State *S, fb_Value *a, int n) {
	if (n < 2) return fb_error(S, "math.floor: expected 1 argument");
	if (!is_num(a[1])) return fb_error(S, "math.floor: expected Int or Float");
	return fb_int(S, is_flt(a[1]) ? (long)floor(fb_tofloat(a[1])) : fb_toint(a[1]));
}
static fb_Value m_ceil(fb_State *S, fb_Value *a, int n) {
	if (n < 2) return fb_error(S, "math.ceil: expected 1 argument");
	if (!is_num(a[1])) return fb_error(S, "math.ceil: expected Int or Float");
	return fb_int(S, is_flt(a[1]) ? (long)ceil(fb_tofloat(a[1])) : fb_toint(a[1]));
}
static fb_Value m_round(fb_State *S, fb_Value *a, int n) {
	if (n < 2) return fb_error(S, "math.round: expected 1 argument");
	if (!is_num(a[1])) return fb_error(S, "math.round: expected Int or Float");
	return fb_int(S, is_flt(a[1]) ? (long)round(fb_tofloat(a[1])) : fb_toint(a[1]));
}

/* math is a namespace object, not a resource -- calling it directly is a
 * mistake (same convention as fbregex's `regex`). */
static fb_Value m_noncallable(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n;
	return fb_error(S, "math is a namespace object; call e.g. math.sqrt(2), not math(...)");
}

int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	fb_Class cls = fb_class(S, "Math");
	fb_method(S, cls, "abs",   m_abs);
	fb_method(S, cls, "sign",  m_sign);
	fb_method(S, cls, "min",   m_min);
	fb_method(S, cls, "max",   m_max);
	fb_method(S, cls, "pow",   m_pow);
	fb_method(S, cls, "sqrt",  m_sqrt);
	fb_method(S, cls, "floor", m_floor);
	fb_method(S, cls, "ceil",  m_ceil);
	fb_method(S, cls, "round", m_round);

	fb_Value obj = fb_object(S, cls, NULL);
	fb_set(S, obj, "pi", fb_float(S, M_PI));
	fb_set(S, obj, "e",  fb_float(S, M_E));

	/* registered so the type checker recognizes the bare identifier `math`
	 * (FB_GLOBAL: a bound value, not a callable); the real value comes from
	 * fb_global_bind below, which eval() resolves first, so this fn only
	 * runs if someone calls math(...) directly. */
	fb_register(S, &(fb_Reg){ "math", m_noncallable, 0, -1, FB_RET_ANY, FB_GLOBAL,
	            "math namespace (fbmath): abs, sign, min, max, pow, sqrt, floor, "
	            "ceil, round; math.pi, math.e" });
	fb_global_bind(S, "math", obj);
	return 0;
}
