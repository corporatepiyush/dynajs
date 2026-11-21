// covers: dyna:decimal.Money.fromMinor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_money_from_minor");
import { Decimal, Money } from "dyna:decimal";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { ok(a === b, what + " (got " + a + ", want " + b + ")"); }
function throws(fn, ErrType, re, what) {
    let t = null;
    try { fn(); } catch (e) { t = e; }
    ok(t !== null, "expected throw: " + what);
    if (t === null) return;
    ok(t instanceof ErrType,
       what + " (wrong type: " + (t && t.constructor && t.constructor.name) + ")");
    ok(!re || re.test(String(t.message)),
       what + " (message [" + String(t && t.message) + "] !~ " + re + ")");
}

eq(Money.fromMinor(1999, "USD").amount(), 1999, "number");

eq(Money.fromMinor(-1, "USD").amount(), -1, "negative number");

eq(Money.fromMinor(0, "USD").amount(), 0, "zero");

eq(Money.fromMinor("1999", "USD").amount(), 1999, "digit string");

eq(Money.fromMinor("-1999", "USD").amount(), -1999, "negative digit string");

__kitS.section("bigint minor count is exact past 2^53", () => {
    const big = 9007199254740993n;
    eq(Money.fromMinor(big, "USD").toString(), "90071992547409.93",
       "bigint minor count is exact past 2^53");
    eq(Money.fromMinor("9007199254740993", "USD").toString(), "90071992547409.93",
       "string minor count is exact past 2^53");
    eq(Money.fromMinor("9223372036854775807", "USD").toString(),
       "92233720368547758.07", "INT64_MAX as a string");
    eq(Money.fromMinor("-9223372036854775808", "USD").toString(),
       "-92233720368547758.08", "INT64_MIN as a string");
    throws(() => Money.fromMinor(2n ** 63n, "USD"), RangeError, /overflows an int64/,
           "2^63 bigint refuses instead of wrapping mod 2^64 to INT64_MIN");
    throws(() => Money.fromMinor(-(2n ** 63n) - 1n, "USD"), RangeError,
           /overflows an int64/, "INT64_MIN - 1 bigint refuses");
    throws(() => Money.fromMinor("9223372036854775808", "USD"), RangeError,
           /overflows an int64/, "INT64_MAX + 1 string refuses");
});

throws(() => Money.fromMinor(1999.5, "USD"), RangeError, /integer/,
       "a fractional minor count is refused");

throws(() => Money.fromMinor(true, "USD"), TypeError, /number, bigint or digit string/,
       "a boolean refuses");

throws(() => Money.fromMinor("1.5", "USD"), RangeError, /minor unit is not money/,
       "a minor count cannot have a fraction");

eq(Money.fromMinor("1,999", "USD").amount(), 1999,
   "the one amount grammar: grouping parses in raw counts too");

throws(() => Money.fromMinor(1, "USD", { precision: 2 }), TypeError,
       /unknown option "precision" \(valid: minorDigits\)/,
       "fromMinor bag is strict");

__kitS.section("a string opts refuses (fromString)", () => {
    throws(() => Money.fromString("1.55", "USD", "minorDigits=3"), TypeError,
           /options must be an object/, "a string opts refuses (fromString)");
    throws(() => Money.fromDecimal(new Decimal("1"), "USD", 3), TypeError,
           /options must be an object/, "a number opts refuses (fromDecimal)");
    throws(() => Money.fromMinor(1, "USD", true), TypeError,
           /options must be an object/, "a boolean opts refuses (fromMinor)");
    throws(() => new Money(1, "USD", "minorDigits=2"), TypeError,
           /options must be an object/, "a string opts refuses (the constructor)");
    eq(Money.fromString("1.55", "USD", null).amount(), 155,
       "null opts is the no-options reading");
    eq(Money.fromMinor(1, "USD", undefined).amount(), 1,
       "undefined opts is the no-options reading");
    eq(Money.fromMinor(9007199254740992n, "USD").amount(), 9007199254740992,
       "amount() is exact at 2^53");
    eq(Money.fromMinor(9007199254740993n, "USD").amount(), 9007199254740992,
       "amount() rounds at 2^53 + 1 (the documented Number boundary)");
    eq(Money.fromMinor(9007199254740993n, "USD").toString(), "90071992547409.93",
       "toString() stays exact where amount() cannot");
    eq(Money.fromMinor(2n ** 63n - 1n, "USD").toString(), "92233720368547758.07",
       "the int64 ceiling reads back exactly via toString()");
    eq(Money.fromMinor(2n ** 63n - 1n, "USD").toDecimal().toString(),
       "92233720368547758.07", "and via toDecimal()");
});
__kitS.end();

print("test_money_from: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
