// covers: dyna:decimal.Money.fromString
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_money_from_string");
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

eq(Money.fromString("-1,234.56", "USD").amount(), -123456, "the brief's example");

eq(Money.fromString("-1,234.56", "USD").toString(), "-1234.56", "toString agrees");

eq(Money.fromString("1,234.56", "USD").amount(), 123456, "positive");

eq(Money.fromString("1234.56", "USD").amount(), 123456, "grouping is optional");

eq(Money.fromString("1,234,567.89", "USD").amount(), 123456789, "two groups");

eq(Money.fromString("1234.5", "USD").amount(), 123450, "short fraction pads");

eq(Money.fromString("1234", "USD").amount(), 123400, "no fraction pads");

eq(Money.fromString("0.01", "USD").amount(), 1, "one cent");

eq(Money.fromString("-0.05", "USD").amount(), -5, "negative fraction");

eq(Money.fromString("-0.05", "USD").toString(), "-0.05", "the leading-zero sign form");

eq(Money.fromString("007.50", "USD").amount(), 750, "leading zeros are value");

eq(Money.fromString("1,234", "JPY").amount(), 1234, "JPY has 0 minor digits");

eq(Money.fromString("1.234", "BHD").amount(), 1234, "BHD has 3 minor digits");

eq(Money.fromString("1.2345", "USD", { minorDigits: 4 }).amount(), 12345,
   "minorDigits override extends the scale");

eq(Money.fromString("19.99", "USD").equals(new Money(1999, "USD")), true,
   "fromString agrees with the constructor");

eq(Money.fromString("99.99", "USD").add(Money.fromString("0.01", "USD")).toString(),
   "100.00", "factory results are ordinary Moneys");

throws(() => Money.fromString("1.2345", "USD"), RangeError, /minor unit is not money/,
       "a fraction finer than the currency refuses");

throws(() => Money.fromString("1,234.5", "JPY"), RangeError, /minor unit is not money/,
       "any fraction on a 0-minor currency refuses");

throws(() => Money.fromString("12,34", "USD"), TypeError, /grouping/,
       "2-digit trailing group is a typo");

throws(() => Money.fromString("1,23.45", "USD"), TypeError, /grouping/,
       "2-digit first group before a 2-group");

throws(() => Money.fromString("1,234,56.78", "USD"), TypeError, /grouping/,
       "short middle group");

throws(() => Money.fromString("1,,234", "USD"), TypeError, /grouping|digits/,
       "empty group");

throws(() => Money.fromString("1,234,", "USD"), TypeError, /grouping/,
       "trailing comma");

throws(() => Money.fromString(",234", "USD"), TypeError, /grouping|digits/,
       "leading comma");

throws(() => Money.fromString("", "USD"), TypeError, /empty/, "empty string");

throws(() => Money.fromString("-", "USD"), TypeError, /digits/, "lone minus");

throws(() => Money.fromString("1.2.3", "USD"), TypeError, /decimal point/,
       "two decimal points");

throws(() => Money.fromString("1.23,45", "USD"), TypeError, /group separator|not a decimal/,
       "comma after the point");

throws(() => Money.fromString("abc", "USD"), TypeError, /not a decimal/, "letters");

throws(() => Money.fromString(" 1.00", "USD"), TypeError, /not a decimal/, "leading space");

throws(() => Money.fromString("1.00 ", "USD"), TypeError, /not a decimal/, "trailing space");

throws(() => Money.fromString("+5.00", "USD"), TypeError, /not a decimal/, "plus sign");

throws(() => Money.fromString("1e3", "USD"), TypeError, /not a decimal/, "exponent form");

throws(() => Money.fromString("NaN", "USD"), TypeError, /not a decimal/, "NaN text");

throws(() => Money.fromString(1234.56, "USD"), TypeError, /fromString/,
       "a number is not a string form");

throws(() => Money.fromString("1.00"), TypeError, /currency/, "missing currency");

throws(() => Money.fromString("1.00", "US"), RangeError, /3-letter/, "bad currency");

__kitS.section("INT64_MAX minor units", () => {
    const max = Money.fromString("92,233,720,368,547,758.07", "USD");
    eq(max.toString(), "92233720368547758.07", "INT64_MAX minor units");
    const min = Money.fromString("-92,233,720,368,547,758.08", "USD");
    eq(min.toString(), "-92233720368547758.08", "INT64_MIN minor units");
    throws(() => Money.fromString("92,233,720,368,547,758.08", "USD"),
           RangeError, /overflows an int64/, "INT64_MAX + 1 minor units refuses");
    throws(() => Money.fromString("-92,233,720,368,547,758.09", "USD"),
           RangeError, /overflows an int64/, "INT64_MIN - 1 minor units refuses");
    throws(() => Money.fromString("9,223,372,036,854,775,807.00", "USD"),
           RangeError, /overflows an int64/,
           "INT64_MAX whole DOLLARS does not fit in int64 cents");
    throws(() => Money.fromString("1" + "0".repeat(30) + ".00", "USD"),
           RangeError, /overflows an int64/, "absurd magnitude refuses (bounded walk)");
});

throws(() => Money.fromString("1", "USD", { minorDigit: 2 }), TypeError,
       /unknown option "minorDigit" \(valid: minorDigits\)/,
       "fromString bag is strict");

throws(() => Money.fromString("1", "USD", { minorDigits: 2.5 }), RangeError,
       /integer/, "fractional minorDigits refuses");

throws(() => Money.fromString("1", "USD", { minorDigits: 7 }), RangeError,
       /0 to 6/, "minorDigits bound");

__kitS.section("a throwing minorDigits getter propagates", () => {
    let trapped = false;
    try { Money.fromString("1.00", "USD",
           { get minorDigits() { throw new Error("trap"); } }); }
    catch (e) { trapped = String(e.message) === "trap"; }
    ok(trapped, "a throwing minorDigits getter propagates");
    throws(() => Money.fromString("1\u06602", "USD"), TypeError, /not a decimal/,
           "non-ASCII-Indic digits refuse");
    throws(() => Money.fromString("1.2\u0663", "USD"), TypeError, /not a decimal/,
           "non-ASCII fraction digits refuse");
    throws(() => Money.fromString("1,234.5\u0000", "USD"), TypeError, /not a decimal/,
           "an embedded NUL refuses");
    throws(() => Money.fromString("--1.00", "USD"), TypeError, /not a decimal/,
           "double minus refuses");
    throws(() => Money.fromString(".5", "USD"), TypeError,
           /digit before the decimal point/, "fraction-only text refuses");
    throws(() => Money.fromString("5.", "USD"), TypeError,
           /no fraction digits/, "an empty fraction refuses");
    eq(Money.fromString("0.0", "USD").amount(), 0, "zero with a fraction digit");
    eq(Money.fromString("000,000.01", "USD").amount(), 1, "all-zero groups");
    throws(() => Money.fromString("123,4567.89", "USD"), TypeError, /grouping/,
           "4-digit second group");
    throws(() => Money.fromString("1234,567.89", "USD"), TypeError, /grouping/,
           "4-digit first group");
    eq(Money.fromMinor(0n, "USD").amount(), 0, "0n");
    eq(Money.fromMinor(-0n, "USD").amount(), 0, "-0n");
    eq(Money.fromMinor("000000000000000000000000005", "USD").amount(), 5,
       "left-padded digit string");
    throws(() => Money.fromMinor("0000000000000000000000009223372036854775808", "USD"),
           RangeError, /overflows an int64/, "left-padding does not smuggle past the bound");
    eq(Money.fromDecimal(new Decimal("-0.000"), "USD").amount(), 0, "negative zero");
    throws(() => Money.fromDecimal(Object.create(new Decimal("5")), "USD"), TypeError,
           /must be a Decimal/, "a forged Decimal (no internal slot) refuses");
    throws(() => Money.fromString("1.00", { toString() { return "USD"; } }), TypeError,
           /currency/, "a currency object refuses");
    eq(Money.fromString("1.00", "USD", { minorDigits: undefined }).amount(), 100,
       "undefined minorDigits is the default");
    eq(new Money(1, "USD", Object.create({ minorDigits: 6 })).amount(), 1,
       "inherited bag keys are not own keys");
});
__kitS.end();

print("test_money_from: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
