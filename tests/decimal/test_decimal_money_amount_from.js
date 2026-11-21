// covers: dyna:decimal.Money.amount
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_money_amount_from");
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

__kitS.section("fromString '1.500' USD is 150 (trailing zeros are exact text)", () => {
    eq(Money.fromString("1.500", "USD").amount(), 150,
       "fromString '1.500' USD is 150 (trailing zeros are exact text)");
    eq(Money.fromString("5.0", "JPY").amount(), 5,
       "fromString '5.0' JPY is 5 (value whole at 0 minor digits)");
    eq(Money.fromString("1.500", "USD", { minorDigits: 3 }).amount(), 1500,
       "minorDigits 3 scales '1.500' to 1500");
    eq(Money.fromDecimal(new Decimal("1.500"), "USD").amount(), 150,
       "fromDecimal admits the same value (the pinned value rule)");
    eq(Money.fromMinor("5.0", "USD").amount(), 5,
       "fromMinor '5.0' is 5 minor units (value whole)");
    eq(Money.fromMinor("150.00", "USD").amount(), 150,
       "fromMinor '150.00' is 150");
    throws(() => Money.fromString("1.005", "USD"), RangeError,
           /minor unit is not money/, "fromString '1.005' refuses");
    throws(() => Money.fromString("5.007", "JPY"), RangeError,
           /minor unit is not money/, "fromString '5.007' JPY refuses");
    throws(() => Money.fromDecimal(new Decimal("1.005"), "USD"), RangeError,
           /minor unit is not money/, "fromDecimal refuses the same value");
    throws(() => Money.fromMinor("5.5", "USD"), RangeError,
           /minor unit is not money/, "fromMinor '5.5' refuses (5.5 is not whole)");
    throws(() => Money.fromMinor("5.005", "USD"), RangeError,
           /minor unit is not money/, "fromMinor '5.005' refuses");
    const v = Money.fromString("1.500", "USD").amount();
    eq(Money.fromDecimal(new Decimal("1.500"), "USD").amount(), v,
       "fromString == fromDecimal on '1.500'");
    eq(Money.fromMinor("150.00", "USD").amount(), v,
       "fromString == fromMinor on the same value");
    eq(Money.fromString("5.0", "JPY").amount(),
       Money.fromDecimal(new Decimal("5.0"), "JPY").amount(),
       "the 0-minor pair agrees too");
});
__kitS.end();

print("test_money_from: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
