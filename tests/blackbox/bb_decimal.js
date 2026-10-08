// Black-box contract test for dyna:decimal, generated from dynajs.d.ts lines 1152-1297 (plus RoundingMode, line 90). Engine sources not consulted.
// PARAMETRIC: case tables driven through one loop per table; rows are [label, args, expected] and every
// failure message names its row. Expected values are exact decimal results computed by hand from the
// contract (IEEE 754-2008 decimal128 context, 34 significant digits) and the documented examples in
// dynajs.d.ts / the module's API contract; rounding-mode semantics are the IEEE 754-2008 mode names of
// d.ts line 90. Money minor-digit defaults (JPY 0, KWD 3) and the symbol table come from the module docs.
import { Decimal, Money } from "dyna:decimal";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true, e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
// One loop per table: row = [label, thunk, expected]; expected may be a value (deep-compared when an
// array), a predicate, or THROWS.
// Two row conventions: [label, thunk, expected] (args IS the thunk) and
// [label, argsArray, expected] driven through fn(...args), e.g. (thunk) => thunk().
const THROWS = (t, p) => ({ __throws: true, t, p });
function runs(table, fn) {
    for (const [label, args, expected] of table) {
        const call = fn ? () => fn(...args) : args;
        if (expected && expected.__throws) assertThrows(call, label, expected.t, expected.p);
        else if (typeof expected === "function" && !fn) { n++; let v; try { v = call(); } catch (e) { throw new Error("assertion failed (threw " + e + "): " + label); } if (!expected(v)) throw new Error("assertion failed (predicate): " + label + " — got |" + v + "|"); }
        else if (Array.isArray(expected)) assertDeepEq(call(), expected, label);
        else assertEq(call(), expected, label);
    }
}
const D = (v) => new Decimal(v);

/* ==========================================================================
 * TABLE 1 — construction (d.ts lines 1172-1174: string, number, or Decimal).
 * ========================================================================== */
runs([
    ["zero", () => D("0").toString(), "0"],
    ["plain negative", () => D("-2.5").toString(), "-2.5"],
    ["integer string", () => D("42").toString(), "42"],
    ["number 0.5 via shortest round-trip text", () => D(0.5).toString(), "0.5"],
    ["number 42", () => D(42).toString(), "42"],
    ["copy of a Decimal", () => D(D("7.25")).toString(), "7.25"],
    ["exponent notation equals its value", () => D("1e3"), (d) => d.equals(1000)],
    ["copy equals the original", () => D(D("1.5")), (d) => d.equals(D("1.5"))],
    ["NaN refused with RangeError", () => D(NaN), THROWS(RangeError)],
    ["Infinity refused with RangeError", () => D(Infinity), THROWS(RangeError)],
    ["malformed string refused with SyntaxError", () => D("abc"), THROWS(SyntaxError)],
    ["empty string refused with SyntaxError", () => D(""), THROWS(SyntaxError)],
    ["two decimal points refused with SyntaxError", () => D("1.2.3"), THROWS(SyntaxError)],
]); // NOTE: this table was generated in a [label, args, fn] convention the file's one-arg runs() driver does not implement; converted to the same [label, thunk, expected] form as every other table

/* ==========================================================================
 * TABLE 2 — exact arithmetic: add/sub/mul/mod are EXACT, opts accepted AND
 * ignored (d.ts lines 1175-1182). Expected results computed by hand.
 * ========================================================================== */
runs([
    ["0.1 + 0.2 == 0.3 exactly (contract example)", [() => D("0.1").add("0.2").toString()], "0.3"],
    ["1 - 0.9 == 0.1 exactly", [() => D("1").sub("0.9").toString()], "0.1"],
    ["19.99 * 3 == 59.97 exactly (contract example)", [() => D("19.99").mul("3").toString()], "59.97"],
    ["(0.1 + 0.2) * 3 == 0.9 exactly", [() => D("0.1").add("0.2").mul("3").toString()], "0.9"],
    ["38 nines squared == (10^38-1)^2 = 10^76 - 2*10^38 + 1 (exact, beyond the 34-digit context)", [() => D("9".repeat(38)).mul("9".repeat(38)).toString()], "9".repeat(37) + "8" + "0".repeat(37) + "1"],
    ["add is NOT rounded to the 34-digit context (40-digit sum exact)", [() => D("999999999999999999999999999999999999999").add("1").toString()], "1000000000000000000000000000000000000000"],
    ["add accepts and ignores opts (d.ts line 1175)", [() => D("2").add("1", { precision: 2, rounding: "up" }).toString()], "3"],
    ["mul accepts and ignores opts", [() => D("2").mul("3", { precision: 1 }).toString()], "6"],
    ["mod accepts and ignores opts", [() => D("7").mod("2", { precision: 2 }).toString()], "1"],
    ["mixed operand types: Decimal + string + number chain", [() => D("1").add(D("2")).add("3").add(4).toString()], "10"],
    ["Decimal.TEN * TEN == 100 (constants are ordinary Decimals)", [() => Decimal.TEN.mul(Decimal.TEN).toString()], "100"],
    ["NEG_ONE + ONE is zero (contract example)", [() => Decimal.NEG_ONE.add(Decimal.ONE).isZero()], true],
    ["19.99 + ZERO keeps its text", [() => D("19.99").add(Decimal.ZERO).toString()], "19.99"],
    ["abs of a negative", [() => D("-5").abs().toString()], "5"],
    ["neg of 5", [() => D("5").neg().toString()], "-5"],
    ["neg of 0 is 0, never -0 (d.ts line 1228)", [() => D("0").neg().toString()], "0"],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 3 — div: THE only arithmetic that rounds (d.ts lines 1179-1180).
 * Default precision 34 (decimal128), rounding halfEven.
 * ========================================================================== */
runs([
    ["1/3 at precision 10 (contract example)", [() => D("1").div("3", { precision: 10 }).toString()], "0.3333333333"],
    ["10/3 at precision 12 (contract showcase)", [() => D("10").div("3", { precision: 12 }).toString()], "3.33333333333"],
    ["1/3 at default precision 34", [() => D("1").div("3").toString()], "0." + "3".repeat(34)],
    ["exact division needs no rounding", [() => D("1").div("8").toString()], "0.125"],
    ["div by zero throws RangeError", [() => D("1").div("0")], THROWS(RangeError)],
    ["STRICT bag: unknown key on div throws", [() => D("1").div("2", { bogus: 1 })], THROWS(TypeError, "bogus")],
], (thunk) => thunk());

// Rounding modes on a true tie: 1/8 = 0.125 -> 2 significant digits (0.12 vs 0.13).
const MODES_125 = [
    ["up", "0.13"], ["down", "0.12"], ["ceil", "0.13"], ["floor", "0.12"],
    ["halfUp", "0.13"], ["halfDown", "0.12"], ["halfEven", "0.12"], ["halfOdd", "0.13"],
];
for (const [mode, exp] of MODES_125) assertEq(D("0.125").div("1", { precision: 2, rounding: mode }).toString(), exp, "div 0.125 -> 2 sig digits, mode " + mode + " (IEEE 754-2008 names)");

/* ==========================================================================
 * TABLE 4 — mod / divmod: truncated remainder, sign follows the dividend
 * (like JS %), exact pair with a == q*b + r (d.ts lines 1181-1182, 1211-1214).
 * ========================================================================== */
runs([
    ["-7 mod 2 follows the dividend's sign", [() => D("-7").mod("2").toString()], "-1"],
    ["7 mod -2 keeps the dividend's sign", [() => D("7").mod("-2").toString()], "1"],
    ["7 mod 2", [() => D("7").mod("2").toString()], "1"],
    ["mod by zero throws", [() => D("7").mod("0")], THROWS(RangeError)],
    ["divmod(-7, 2) == [-3, -1] (contract example)", [() => D("-7").divmod("2").map((d) => d.toString())], ["-3", "-1"]],
    ["divmod(7.5, 2) == [3, 1.5] (contract example)", [() => D("7.5").divmod("2").map((d) => d.toString())], ["3", "1.5"]],
    ["divmod invariant a == q*b + r on the nose", [() => { const [q, r] = D("-7.5").divmod("2"); return q.mul("2").add(r).toString(); }], "-7.5"],
    ["divmod by zero throws", [() => D("7").divmod("0")], THROWS(RangeError)],
    ["divmod opts shape checked even though keys are unread (d.ts 1165-1167)", [() => D("7").divmod("2", { bogus: 1 })], THROWS(TypeError, "bogus")],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 5 — pow: integer exponent, binary powering (d.ts lines 1183-1184).
 * ========================================================================== */
runs([
    ["2^10 == 1024", [() => D("2").pow(10).toString()], "1024"],
    ["x^0 == 1", [() => D("12345").pow(0).toString()], "1"],
    ["4^-2 == 0.0625 exactly", [() => D("4").pow(-2).toString()], "0.0625"],
    ["3^-1 takes the reciprocal at context precision", [() => D("3").pow(-1).toString()], "0." + "3".repeat(34)],
    ["2^10000 has 3011 digits (log10(2^10000) = 3010.3)", [() => D("2").pow(10000).toString().length], 3011],
    ["(1e100)^2 expands exactly to 1 followed by 200 zeros (contract showcase)", [() => D("1e100").pow(2).toString()], "1" + "0".repeat(200)],
    ["exponent above 10000 refused", [() => D("2").pow(10001)], THROWS()],
    ["exponent below -10000 refused", [() => D("2").pow(-10001)], THROWS()],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 6 — sqrt/exp/ln/log10: correctly rounded, ALWAYS halfEven, rounding
 * accepted and ignored (d.ts lines 1185-1204). Documented default-precision
 * values are quoted from the module's own contract examples.
 * ========================================================================== */
runs([
    ["sqrt(2) at 34 digits, half-even (contract example)", [() => D("2").sqrt().toString()], "1.414213562373095048801688724209698"],
    ["sqrt(2.1025) == 1.45 exactly (contract example)", [() => D("2.1025").sqrt().toString()], "1.45"],
    ["sqrt(4) == 2", [() => D("4").sqrt().toString()], "2"],
    ["sqrt(0) == 0", [() => D("0").sqrt().toString()], "0"],
    ["sqrt of a negative throws RangeError", [() => D("-1").sqrt()], THROWS(RangeError)],
    ["sqrt ignores `rounding` (still correctly rounded half-even)", [() => D("2").sqrt({ rounding: "down" }).toString()], "1.414213562373095048801688724209698"],
    ["exp(0) == 1 (contract example)", [() => D("0").exp().toString()], "1"],
    ["exp(1) = e rounded half-even to 34 significant digits", [() => D("1").exp().toString()], "2.718281828459045235360287471352662"],
    ["exp(-1) (contract example)", [() => D("-1").exp().toString()], "0.3678794411714423215955237701614609"],
    ["exp overflows past Emax = 999999 (x >= 10^6 ln 10) with RangeError", [() => D("2302586").exp()], THROWS(RangeError)],
    ["exp underflow past Etiny is 0, not an error (e^-3000000 ~ 10^-1302884 < Etiny)", [() => D("-3000000").exp().toString()], "0"],
    ["ln(1) == 0", [() => D("1").ln().toString()], "0"],
    ["ln(0) throws RangeError (no -Infinity here)", [() => D("0").ln()], THROWS(RangeError)],
    ["ln of a negative throws RangeError", [() => D("-1").ln()], THROWS(RangeError)],
    ["ln ignores `rounding` (accepted and ignored)", [() => D("1").ln({ rounding: "up" }).toString()], "0"],
    ["log10(1e100) == 100 exactly (contract example)", [() => D("1e100").log10().toString()], "100"],
    ["log10(1000) == 3 exactly", [() => D("1000").log10().toString()], "3"],
    ["log10(0) throws RangeError", [() => D("0").log10()], THROWS(RangeError)],
    ["log10 of a negative throws RangeError", [() => D("-5").log10()], THROWS(RangeError)],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 7 — floor/ceil/trunc: exact, no rounding context, zero is never -0
 * (d.ts lines 1205-1210). Rows: [value, [floor, ceil, trunc]].
 * ========================================================================== */
const INTS = [
    ["-2.5", ["-3", "-2", "-2"]],
    ["2.5", ["2", "3", "2"]],
    ["-0.5", ["-1", "0", "0"]],
    ["0.5", ["0", "1", "0"]],
    ["7", ["7", "7", "7"]],
    ["-7", ["-7", "-7", "-7"]],
];
for (const [v, [f, c, t]] of INTS) {
    assertEq(D(v).floor().toString(), f, "floor(" + v + ") — toward -Infinity");
    assertEq(D(v).ceil().toString(), c, "ceil(" + v + ") — toward +Infinity");
    assertEq(D(v).trunc().toString(), t, "trunc(" + v + ") — toward zero");
}

/* ==========================================================================
 * TABLE 8 — round(): new Decimal at dp decimal places, default halfEven
 * (d.ts line 1233). All 8 mode names of d.ts line 90, on ties.
 * ========================================================================== */
const ROUND_MODES = [
    // [mode, round(2.5), round(-2.5)] at dp 0 — IEEE 754-2008 semantics.
    ["up", "3", "-3"],       // away from zero
    ["down", "2", "-2"],     // toward zero
    ["ceil", "3", "-2"],     // toward +Infinity
    ["floor", "2", "-3"],    // toward -Infinity
    ["halfUp", "3", "-3"],   // ties away from zero
    ["halfDown", "2", "-2"], // ties toward zero
    ["halfEven", "2", "-2"], // banker's rounding (the round() default)
    ["halfOdd", "3", "-3"],  // ties to the odd neighbour
];
for (const [mode, pos, neg] of ROUND_MODES) {
    assertEq(D("2.5").round(0, mode).toString(), pos, "round(2.5, " + mode + ")");
    assertEq(D("-2.5").round(0, mode).toString(), neg, "round(-2.5, " + mode + ")");
}
runs([
    ["halfEven(3.5) -> 4 (ties to even)", [() => D("3.5").round(0, "halfEven").toString()], "4"],
    ["halfOdd(3.5) -> 3 (ties to odd)", [() => D("3.5").round(0, "halfOdd").toString()], "3"],
    ["halfEven(4.5) -> 4", [() => D("4.5").round(0, "halfEven").toString()], "4"],
    ["halfOdd(4.5) -> 5", [() => D("4.5").round(0, "halfOdd").toString()], "5"],
    ["round(2.567, 1, halfUp) == 2.6 (contract example)", [() => D("2.567").round(1, "halfUp").toString()], "2.6"],
    ["round to negative dp: 1234.567 at dp -2 -> 1200", [() => D("1234.567").round(-2).toString()], "1200"],
    ["negative-dp tie: 1250 halfEven -> 1200 (last kept digit 2 is even)", [() => D("1250").round(-2, "halfEven").toString()], "1200"],
    ["negative-dp tie: 1250 halfUp -> 1300", [() => D("1250").round(-2, "halfUp").toString()], "1300"],
    ["negative-dp tie: 1250 halfOdd -> 1300 (last kept digit 3 is odd)", [() => D("1250").round(-2, "halfOdd").toString()], "1300"],
    ["round() returns a NEW Decimal (original untouched)", [() => { const d = D("2.567"); d.round(0); return d.equals("2.567"); }], true],
    ["round default mode is halfEven: 2.5 -> 2", [() => D("2.5").round().toString()], "2"],
    ["round(1.005, 2, halfUp) == 1.01 (contract showcase)", [() => D("1.005").round(2, "halfUp").toString()], "1.01"],
    ["dp out of range refused (|dp| <= 1000)", [() => D("1").round(1001)], THROWS()],
    ["unknown rounding mode name refused", [() => D("1").round(0, "bogus")], THROWS()],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 9 — toFixed(): string with exactly dp digits after the point, DEFAULT
 * halfUp (the human-facing default; d.ts lines 1235-1236).
 * ========================================================================== */
runs([
    ["-1234.5678 toFixed(2) == '-1234.57' (halfUp default, contract example)", [() => D("-1234.5678").toFixed(2)], "-1234.57"],
    ["negative rounding to zero still prints '-0.00' (d.ts line 1235)", [() => D("-0.001").toFixed(2)], "-0.00"],
    ["1.005 toFixed(2) == '1.01' (halfUp default)", [() => D("1.005").toFixed(2)], "1.01"],
    ["1.005 toFixed(2, halfEven) == '1.00' (contract showcase)", [() => D("1.005").toFixed(2, "halfEven")], "1.00"],
    ["2.5 toFixed(0) == '3' (halfUp ties away)", [() => D("2.5").toFixed(0)], "3"],
    ["0.5 toFixed(2) == '0.50' pads with zeros", [() => D("0.5").toFixed(2)], "0.50"],
    ["toFixed(0) emits no decimal point", [() => D("7.25").toFixed(0)], "7"],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 10 — conversions and queries (d.ts lines 1215-1246).
 * ========================================================================== */
runs([
    ["toBigInt(42) == 42n", [() => D("42").toBigInt()], 42n],
    ["toBigInt(-42) == -42n", [() => D("-42").toBigInt()], -42n],
    ["toBigInt refuses a fractional part (RangeError)", [() => D("-42.9").toBigInt()], THROWS(RangeError)],
    ["toBigInt refuses magnitude >= 2^63 (int64 bridge)", [() => D("1e100").toBigInt()], THROWS(RangeError)],
    ["toNumber(0.1) === 0.1 (ToNumber over exact text)", [() => D("0.1").toNumber()], 0.1],
    ["toNumber(TEN^30) === 1e30", [() => Decimal.TEN.pow(30).toNumber()], 1e30],
    ["toJSON == toString (JSON round-trips exactly)", [() => D("19.99").toJSON() === D("19.99").toString()], true],
    ["equals: 1.5 equals 1.50 (d.ts line 1231)", [() => D("1.5").equals("1.50")], true],
    ["equals accepts a number", [() => D("1.5").equals(1.5)], true],
    ["equals accepts a Decimal", [() => D("1.5").equals(D("1.50"))], true],
    ["equals false on a different value", [() => D("1.5").equals("1.5001")], false],
    ["cmp(2.50, 2.5) == 0", [() => D("2.50").cmp("2.5")], 0],
    ["cmp less", [() => D("-1").cmp("1")], -1],
    ["cmp greater", [() => D("1").cmp("-1")], 1],
    ["sign of a negative", [() => D("-5").sign()], -1],
    ["sign of zero", [() => Decimal.ZERO.sign()], 0],
    ["sign of a positive", [() => D("5").sign()], 1],
    ["isZero true for zero", [() => Decimal.ZERO.isZero()], true],
    ["isZero false for one", [() => Decimal.ONE.isZero()], false],
    ["digits(123.45) == 5 (contract example)", [() => D("123.45").digits()], 5],
    ["digits(5) == 1", [() => D("5").digits()], 1],
    ["constants read back as their values", [() => [Decimal.ZERO, Decimal.ONE, Decimal.TWO, Decimal.TEN, Decimal.NEG_ONE].map((d) => d.toString())], ["0", "1", "2", "10", "-1"]],
    ["STRICT bag on add: unknown key throws TypeError naming it", [() => D("1").add("1", { bogus: 1 })], THROWS(TypeError, "bogus")],
    ["accepted-and-ignored keys stay accepted on exact ops (d.ts 1164-1167)", [() => D("2").add("1", { precision: 34, rounding: "halfEven" }).toString()], "3"],
], (thunk) => thunk());

/* ==========================================================================
 * TABLE 11 — Money construction and factories (d.ts lines 1249-1273).
 * ========================================================================== */
{
    const M = (minor, cur, opts) => new Money(minor, cur, opts);
    runs([
        ["currency is upper-cased", [() => M(1999, "usd").currency()], "USD"],
        ["toString(1999 USD) == '19.99'", [() => M(1999, "USD").toString()], "19.99"],
        ["format(1999 USD) == '$19.99'", [() => M(1999, "USD").format()], "$19.99"],
        ["amount() is the minor-unit integer", [() => M(1999, "USD").amount()], 1999],
        ["negative amount prints without grouping", [() => M(-123456, "USD").toString()], "-1234.56"],
        ["JPY has 0 minor digits", [() => M(100, "JPY").toString()], "100"],
        ["JPY format uses the currency symbol (contract example)", [() => M(100, "JPY").format()], "\u00a5100"],
        ["KWD has 3 minor digits (doc minor-digit table)", [() => M(123456, "KWD").toString()], "123.456"],
        ["non-symbol currency formats as 'amount CODE'", [() => M(1999, "CHF").format()], "19.99 CHF"],
        ["toDecimal is the exact amount", [() => M(1999, "USD").toDecimal().toString()], "19.99"],
        ["toJSON == toString", [() => M(1999, "USD").toJSON() === M(1999, "USD").toString()], true],
        ["add same currency (1999+1 == '20.00', contract example)", [() => M(1999, "USD").add(M(1, "USD")).toString()], "20.00"],
        ["sub keeps the minor scale", [() => M(1999, "USD").sub(M(1, "USD")).toString()], "19.98"],
        ["mul by an integer", [() => M(1999, "USD").mul(3).toString()], "59.97"],
        ["mul by a negative integer", [() => M(1999, "USD").mul(-2).toString()], "-39.98"],
        ["cmp by amount", [() => M(1998, "USD").cmp(M(1999, "USD"))], -1],
        ["equals by amount", [() => M(1999, "USD").equals(M(1999, "USD"))], true],
        ["allocate: 1999 into [1,1,1] == [667,666,666] (contract example)", [() => M(1999, "USD").allocate([1, 1, 1]).map((m) => m.amount())], [667, 666, 666]],
        ["allocate: remainder goes to the earliest shares", [() => M(1000, "USD").allocate([1, 1, 1, 1]).map((m) => m.amount())], [250, 250, 250, 250]],
        ["allocate sum is exactly the original (nothing created, nothing lost)", [() => { const parts = M(1001, "USD").allocate([1, 1, 1]); return parts.map((m) => m.amount()).reduce((a, b) => a + b, 0); }], 1001],
        ["allocate a zero share", [() => M(100, "USD").allocate([1, 0]).map((m) => m.amount())], [100, 0]],
        ["fromString('-1,234.56') == Money(-123456) (contract example)", [() => Money.fromString("-1,234.56", "USD").equals(M(-123456, "USD"))], true],
        ["grouping is structural: '1,234.56' == '1234.56'", [() => Money.fromString("1,234.56", "USD").equals(Money.fromString("1234.56", "USD"))], true],
        ["trailing zeros are TEXT: '1.500' USD is 150 minor units", [() => Money.fromString("1.500", "USD").amount()], 150],
        ["'5.0' on JPY is 5 (doc example)", [() => Money.fromString("5.0", "JPY").amount()], 5],
        ["fromDecimal(1234.56) is 123456 cents", [() => Money.fromDecimal(D("1234.56"), "USD").amount()], 123456],
        ["fromDecimal(12.3) is 1230 (trailing zero at scale is exact)", [() => Money.fromDecimal(D("12.3"), "USD").amount()], 1230],
        ["fromDecimal(1.500) is 150 (contract example)", [() => Money.fromDecimal(D("1.500"), "USD").amount()], 150],
        ["zero is admitted at any scale", [() => Money.fromDecimal(D("0.000"), "USD").amount()], 0],
        ["fromMinor bigint is exact past 2^53 (contract example)", [() => Money.fromMinor(9007199254740993n, "USD").toString()], "90071992547409.93"],
        ["fromMinor string with grouping", [() => Money.fromMinor("1,234", "USD").amount()], 1234],
        ["fromMinor string '5.0' is 5 (doc example)", [() => Money.fromMinor("5.0", "USD").amount()], 5],
        ["explicit minorDigits override: 4-digit currency", [() => Money.fromString("1.2345", "USD", { minorDigits: 4 }).toString()], "1.2345"],
        ["amount() past 2^53 rounds the READ, not the store (INT64_MAX minor units)", [() => Money.fromString("92,233,720,368,547,758.07", "USD").amount()], 9223372036854775808],
        // BAD-TEST FIX: Money stores MINOR units; INT64_MAX minor units (9223372036854775807 cents)
        // renders as $92,233,720,368,547,758.07. The original expected "9223372036854775807.00"
        // misread the minor-unit count as the major form (that value is 100x past INT64).
        ["the STORE stays exact past 2^53", [() => Money.fromString("92,233,720,368,547,758.07", "USD").toString()], "92233720368547758.07"],
    ], (thunk) => thunk());

    /* ----------------------------------------------------------------------
     * TABLE 12 — Money refusals: currency discipline, scale rule, strict bags.
     * ---------------------------------------------------------------------- */
    runs([
        ["cross-currency add is a TypeError (missing exchange rate)", [() => M(100, "USD").add(M(100, "EUR"))], THROWS(TypeError)],
        ["cross-currency sub is a TypeError", [() => M(100, "USD").sub(M(100, "EUR"))], THROWS(TypeError)],
        ["fractional multiplier refused — use allocate", [() => M(100, "USD").mul(2.5)], THROWS(RangeError)],
        ["fractional minorUnits refused", [() => M(1999.5, "USD")], THROWS(RangeError)],
        ["2-letter currency refused", [() => M(100, "US")], THROWS()],
        ["4-letter currency refused", [() => M(100, "USDD")], THROWS()],
        ["non-object options refused (TypeError)", [() => M(100, "USD", "bogus")], THROWS(TypeError)],
        ["STRICT bag: unknown key refused", [() => M(100, "USD", { minorDigits: 2, junk: 1 })], THROWS(TypeError, "junk")],
        ["'12,34' is a structural grouping typo that throws", [() => Money.fromString("12,34", "USD")], THROWS()],
        ["'1.005' USD is finer than the minor unit — refused, not rounded", [() => Money.fromString("1.005", "USD")], THROWS(RangeError)],
        ["'92,233,720,368,547,758.08' is one cent past INT64 — refused", [() => Money.fromString("92,233,720,368,547,758.08", "USD")], THROWS(RangeError)],
        ["'5.5' on JPY (0 minor digits) throws", [() => Money.fromString("5.5", "JPY")], THROWS()],
        ["fromDecimal('0.001') on USD refused, never snapped", [() => Money.fromDecimal(D("0.001"), "USD")], THROWS(RangeError)],
        ["fromMinor '5.5' is not whole minor units — throws", [() => Money.fromMinor("5.5", "USD")], THROWS()],
        ["fromMinor past INT64 refuses instead of wrapping", [() => Money.fromMinor(2n ** 63n, "USD")], THROWS()],
        ["allocate needs shares summing to at least 1", [() => M(100, "USD").allocate([0])], THROWS()],
        ["allocate refuses fractional shares", [() => M(100, "USD").allocate([1.5])], THROWS()],
    ], (thunk) => thunk());
}

print("bb_decimal: all tests passed (" + n + " assertions)");
