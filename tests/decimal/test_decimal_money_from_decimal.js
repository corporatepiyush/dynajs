// covers: dyna:decimal.Money.fromDecimal
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_money_from_decimal");
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

__kitS.section("plain", () => {
eq(Money.fromDecimal(new Decimal("1234.56"), "USD").amount(), 123456, "plain");
});

__kitS.section("short scale pads", () => {
eq(Money.fromDecimal(new Decimal("12.3"), "USD").amount(), 1230, "short scale pads");
});

__kitS.section("negative", () => {
eq(Money.fromDecimal(new Decimal("-1.05"), "USD").amount(), -105, "negative");
});

__kitS.section("zero", () => {
eq(Money.fromDecimal(new Decimal("0"), "USD").amount(), 0, "zero");
});

__kitS.section("trailing decimal zeros are exact", () => {
eq(Money.fromDecimal(new Decimal("1.500"), "USD").amount(), 150,
   "trailing decimal zeros are exact");
});

__kitS.section("exponent form lands on the same minor count", () => {
eq(Money.fromDecimal(new Decimal("1.5e3"), "USD").amount(), 150000,
   "exponent form lands on the same minor count");
});

__kitS.section("minorDigits override", () => {
eq(Money.fromDecimal(new Decimal("1.2345"), "USD", { minorDigits: 4 }).amount(),
   12345, "minorDigits override");
});

__kitS.section("a fraction finer than the currency refuses", () => {
throws(() => Money.fromDecimal(new Decimal("0.001"), "USD"), RangeError,
       /minor unit is not money/, "a fraction finer than the currency refuses");
});

__kitS.section("and the minorDigits override admits it", () => {
eq(Money.fromDecimal(new Decimal("0.001"), "USD", { minorDigits: 3 }).amount(), 1,
   "and the minorDigits override admits it");
});

__kitS.section("0-minor currency refuses any fraction", () => {
throws(() => Money.fromDecimal(new Decimal("1.5"), "JPY"), RangeError,
       /minor unit is not money/, "0-minor currency refuses any fraction");
});

__kitS.section("a zero with scale is still zero", () => {
eq(Money.fromDecimal(new Decimal("0.0000000"), "USD").amount(), 0,
   "a zero with scale is still zero");
});

__kitS.section("fromDecimal bag is strict", () => {
throws(() => Money.fromDecimal(new Decimal("1"), "USD", { rounding: "up" }), TypeError,
       /unknown option "rounding" \(valid: minorDigits\)/,
       "fromDecimal bag is strict");
});
__kitS.end();

print("test_money_from: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
