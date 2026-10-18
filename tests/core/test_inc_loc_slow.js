// inc_loc / dec_loc are the fused `x++` / `x--` on a local. Their fast path is
// int32-only; everything else (int32 overflow, doubles, strings, BigInt,
// objects with valueOf, undefined) takes the slow path, which works on a
// temporary so the fast path's operand never has its address taken. Each case
// is checked against the spec result written out by hand.
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + String(a) + ", want " + String(b)); }

function incN(x, k) { for (let i = 0; i < k; i++) x++; return x; }
function decN(x, k) { for (let i = 0; i < k; i++) x--; return x; }
function incVar(x) { var y = x; y++; return y; }
function decVar(x) { var y = x; y--; return y; }

eq(incN(0, 5), 5, "int increments");
eq(decN(0, 5), -5, "int decrements");
eq(incN(2147483646, 3), 2147483649, "crosses INT32_MAX into a double");
eq(decN(-2147483647, 3), -2147483650, "crosses INT32_MIN into a double");
eq(incVar(2147483647), 2147483648, "INT32_MAX + 1");
eq(decVar(-2147483648), -2147483649, "INT32_MIN - 1");
eq(incN(0.5, 2), 2.5, "double increments");
eq(decN(0.25, 1), -0.75, "double decrements");
eq(incVar(-0), 1, "-0 + 1");
eq(decVar(1), 0, "1 - 1 is +0");
check(Object.is(decVar(1), 0) && !Object.is(decVar(1), -0), "1 - 1 is +0, not -0");
eq(incVar("41"), 42, "numeric string coerces");
eq(Number.isNaN(incVar("x")), true, "non-numeric string becomes NaN");
eq(incVar(10n), 11n, "BigInt increments");
eq(decVar(-1n), -2n, "BigInt decrements");
eq(incN(2n ** 64n, 2), 2n ** 64n + 2n, "large BigInt");
eq(Number.isNaN(incVar(undefined)), true, "undefined becomes NaN");
eq(incVar(null), 1, "null becomes 1");
eq(incVar(true), 2, "true becomes 2");
let calls = 0;
eq(incVar({ valueOf() { calls++; return 7; } }), 8, "object valueOf");
eq(calls, 1, "valueOf called once");
let threw = false;
try { incVar(Symbol("s")); } catch (e) { threw = e instanceof TypeError; }
check(threw, "Symbol throws TypeError");
threw = false;
try { incVar({ valueOf() { throw new RangeError("boom"); } }); } catch (e) { threw = e instanceof RangeError && e.message === "boom"; }
check(threw, "a throwing valueOf propagates");
{
    // the slow path must not leak: 100k increments of a BigInt local
    let x = 1n;
    for (let i = 0; i < 100000; i++) x++;
    eq(x, 100001n, "BigInt loop");
}
{
    let s = "a";
    for (let i = 0; i < 3; i++) s--;
    eq(Number.isNaN(s), true, "string decremented to NaN stays NaN");
}

print(`test_inc_loc_slow: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
