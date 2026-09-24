// flags: --std
/* test_money_from.js --: Money.fromString / Money.fromDecimal /
 * Money.fromMinor, the three exact input forms.
 *
 * What is pinned here is ARITHMETIC PROPERTIES and REFUSALS, not foreign
 * vectors: a minor-unit count is defined by this module's own contract
 * (the amount string is decimal text with ',' grouping; the value is exact;
 * a value that is not a whole count of the currency's minor unit is refused,
 * never rounded). Every expected value below is plain base-10 conversion a
 * reader can verify by eye or by BigInt arithmetic in the assertions
 * themselves -- the >= 2^53 rows are cross-checked with BigInt literals so
 * the test never stores an expected double it cannot represent.
 */
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

/* ---- 1. Money.fromString: the human decimal form ---- */
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

/* ---- 2. Money.fromString refusals ---- */
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

/* ---- 3. overflow is exact, never wrapped ---- */
{
    /* the int64 is a count of MINOR units: INT64_MAX cents is
       92,233,720,368,547,758.07, and one cent more refuses */
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
}

/* ---- 4. Money.fromDecimal: exact Decimal conversion ---- */
eq(Money.fromDecimal(new Decimal("1234.56"), "USD").amount(), 123456, "plain");
eq(Money.fromDecimal(new Decimal("12.3"), "USD").amount(), 1230, "short scale pads");
eq(Money.fromDecimal(new Decimal("-1.05"), "USD").amount(), -105, "negative");
eq(Money.fromDecimal(new Decimal("0"), "USD").amount(), 0, "zero");
eq(Money.fromDecimal(new Decimal("1.500"), "USD").amount(), 150,
   "trailing decimal zeros are exact");
eq(Money.fromDecimal(new Decimal("1.5e3"), "USD").amount(), 150000,
   "exponent form lands on the same minor count");
eq(Money.fromDecimal(new Decimal("1.2345"), "USD", { minorDigits: 4 }).amount(),
   12345, "minorDigits override");
throws(() => Money.fromDecimal(new Decimal("0.001"), "USD"), RangeError,
       /minor unit is not money/, "a fraction finer than the currency refuses");
eq(Money.fromDecimal(new Decimal("0.001"), "USD", { minorDigits: 3 }).amount(), 1,
   "and the minorDigits override admits it");
throws(() => Money.fromDecimal(new Decimal("1.5"), "JPY"), RangeError,
       /minor unit is not money/, "0-minor currency refuses any fraction");
throws(() => Money.fromDecimal("12.34", "USD"), TypeError, /must be a Decimal/,
       "a string is not a Decimal");
throws(() => Money.fromDecimal({}, "USD"), TypeError, /must be a Decimal/,
       "a plain object is not a Decimal");
throws(() => Money.fromDecimal(new Decimal("1e30"), "USD"), RangeError,
       /overflows an int64/, "huge Decimal refuses");
throws(() => Money.fromDecimal(new Decimal("1e-30"), "USD"), RangeError,
       /minor unit is not money/, "tiny Decimal refuses rather than snapping to 0");
/* zero is the one value at or below the scale that is whole: it is admitted */
eq(Money.fromDecimal(new Decimal("0.0000000"), "USD").amount(), 0,
   "a zero with scale is still zero");

/* ---- 5. Money.fromMinor: the exact raw count ---- */
eq(Money.fromMinor(1999, "USD").amount(), 1999, "number");
eq(Money.fromMinor(-1, "USD").amount(), -1, "negative number");
eq(Money.fromMinor(0, "USD").amount(), 0, "zero");
eq(Money.fromMinor("1999", "USD").amount(), 1999, "digit string");
eq(Money.fromMinor("-1999", "USD").amount(), -1999, "negative digit string");
/* the exactness rows: BigInt and string carry values a double cannot */
{
    const big = 9007199254740993n;               /* 2^53 + 1 */
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
}
throws(() => Money.fromMinor(1999.5, "USD"), RangeError, /integer/,
       "a fractional minor count is refused");
throws(() => Money.fromMinor(true, "USD"), TypeError, /number, bigint or digit string/,
       "a boolean refuses");
throws(() => Money.fromMinor("1.5", "USD"), RangeError, /minor unit is not money/,
       "a minor count cannot have a fraction");
eq(Money.fromMinor("1,999", "USD").amount(), 1999,
   "the one amount grammar: grouping parses in raw counts too");

/* ---- 6.: {minorDigits} is the whole bag, everywhere ---- */
throws(() => Money.fromString("1", "USD", { minorDigit: 2 }), TypeError,
       /unknown option "minorDigit" \(valid: minorDigits\)/,
       "fromString bag is strict");
throws(() => Money.fromDecimal(new Decimal("1"), "USD", { rounding: "up" }), TypeError,
       /unknown option "rounding" \(valid: minorDigits\)/,
       "fromDecimal bag is strict");
throws(() => Money.fromMinor(1, "USD", { precision: 2 }), TypeError,
       /unknown option "precision" \(valid: minorDigits\)/,
       "fromMinor bag is strict");
throws(() => new Money(1, "USD", { minorDigit: 2 }), TypeError,
       /unknown option "minorDigit" \(valid: minorDigits\)/,
       "the constructor bag goes strict too");
throws(() => Money.fromString("1", "USD", { minorDigits: 2.5 }), RangeError,
       /integer/, "fractional minorDigits refuses");
throws(() => Money.fromString("1", "USD", { minorDigits: 7 }), RangeError,
       /0 to 6/, "minorDigits bound");

/* ---- 7. adversarial shapes (probe-derived; see scratch/e5_probe1.js) ---- */
{
    /* traps propagate: the bag check runs user code and must not swallow it */
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
}

/* ---- 8. the value rule, all three factories, both directions ---- */
{
    /* trailing zeros are TEXT, not finer value */
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
    /* a NONZERO digit past the scale is finer value: refused by all three */
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
    /* and the three factories agree on one value */
    const v = Money.fromString("1.500", "USD").amount();
    eq(Money.fromDecimal(new Decimal("1.500"), "USD").amount(), v,
       "fromString == fromDecimal on '1.500'");
    eq(Money.fromMinor("150.00", "USD").amount(), v,
       "fromString == fromMinor on the same value");
    eq(Money.fromString("5.0", "JPY").amount(),
       Money.fromDecimal(new Decimal("5.0"), "JPY").amount(),
       "the 0-minor pair agrees too");
}

/* ---- 9. a bag is an object or absent; amount() is a Number by contract -- */
{
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
    /* amount() is a Number: exact through 2^53 minor units and rounding
       beyond -- the STORE and toString()/toDecimal() never round (that
       boundary is the documented one) */
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
}

/* ---- 10. differential vs an independent BigInt model of the documented
 * rule (the review's digit-boundary matrix, with the model written from the
 * contract: grouping is structural, trailing zeros past the scale are exact
 * text, a nonzero digit past the scale refuses, the scaled value must fit
 * int64). ~1500 rows across every digit-count boundary. ---- */
{
    const I64_MAX = (1n << 63n) - 1n;
    /* returns [tag, BigInt-or-errorName] per the contract's pass order */
    function moneyRef(str, minor) {
        if (str.length === 0) return ["E", "TypeError"];
        const neg = str[0] === "-";
        let i = neg ? 1 : 0;
        let acc = 0n, frac = 0n, nfrac = 0;
        let seenDot = false, seenDigit = false, comma = false;
        for (; i < str.length; i++) {
            const c = str[i];
            if (c === ".") {
                if (seenDot || nfrac === -1) return ["E", "TypeError"];
                if (!seenDigit) return ["E", "TypeError"];
                seenDot = true;
                nfrac = -1;
            } else if (c === ",") {
                if (seenDot) return ["E", "TypeError"];
                comma = true;
            } else if (c < "0" || c > "9") {
                return ["E", "TypeError"];
            } else if (seenDot) {
                if (nfrac < 0) nfrac = 0;
                nfrac++;
                if (nfrac <= minor) {
                    frac = frac * 10n + BigInt(c.charCodeAt(0) - 48);
                } else if (c !== "0") {
                    return ["E", "RangeError"];   /* finer value than the minor unit */
                }
            } else {
                seenDigit = true;
                acc = acc * 10n + BigInt(c.charCodeAt(0) - 48);
                if (acc > I64_MAX + 1n) return ["E", "RangeError"];
            }
        }
        if (nfrac === -1 || !seenDigit) return ["E", "TypeError"];
        if (comma) {                     /* grouping is structural: 1-3, then 3s */
            let g = 0, glen = 0;
            for (let k = neg ? 1 : 0; k < str.length && str[k] !== "."; k++) {
                if (str[k] === ",") {
                    if (glen === 0 || glen > 3 || (g && glen !== 3))
                        return ["E", "TypeError"];
                    g++; glen = 0;
                } else glen++;
            }
            if (glen === 0 || glen > 3 || (g && glen !== 3))
                return ["E", "TypeError"];
        }
        for (let k = 0; k < minor; k++) acc *= 10n;
        for (let k = nfrac; k < minor; k++) frac *= 10n;
        const val = acc + frac;
        if (neg) {
            if (val > I64_MAX + 1n) return ["E", "RangeError"];
            return ["V", -val];
        }
        if (val > I64_MAX) return ["E", "RangeError"];
        return ["V", val];
    }
    function amountExact(m, minor) {
        const s = m.toDecimal().toString();
        const [ip, fp = ""] = s.split(".");
        return BigInt(ip + fp.padEnd(minor, "0"));
    }
    let rows = 0, mism = 0;
    function check(str, minor) {
        rows++;
        const exp = moneyRef(str, minor);
        let act;
        try {
            const m = Money.fromString(str, "USD",
                                       minor === 2 ? undefined : { minorDigits: minor });
            act = ["V", amountExact(m, minor)];
        } catch (e) {
            act = ["E", e.constructor.name];
        }
        if (act[0] !== exp[0] || (act[0] === "V" && act[1] !== exp[1]) ||
            (act[0] === "E" && act[1] !== exp[1])) {
            mism++;
            if (mism <= 5)
                print("FAIL: ref " + JSON.stringify(str) + " minor=" + minor +
                      ": got " + act.join(" ") + ", want " + exp.join(" "));
        }
    }
    /* every digit-count boundary: integer 17..21 x fraction 0..8 x minor */
    for (let intLen = 17; intLen <= 21; intLen++) {
        for (let fracLen = 0; fracLen <= 8; fracLen++) {
            for (const lead of ["1", "9"]) {
                for (const minor of [0, 1, 2, 3, 6]) {
                    const i = lead + "0".repeat(intLen - 2) + "7";
                    const f = fracLen ? "." + "0".repeat(fracLen - 1) + "3" : "";
                    check(i + f, minor);
                    check("-" + i + f, minor);
                }
            }
        }
    }
    /* the int64 minor-unit ceiling neighborhood */
    for (const s of ["92233720368547758.07", "92233720368547758.08", "92233720368547758.06",
                     "-92233720368547758.08", "-92233720368547758.09",
                     "9223372036.854775807", "9223372036.854775808",
                     "0.000001", "0.0000001", "184467440737095516.15",
                     "9223372036854775807", "9223372036854775808",
                     "-9223372036854775808", "-9223372036854775809",
                     "1.500", "5.0", "242448628.0", "0.0000000", "1.000000"])
        for (const minor of [0, 1, 2, 3, 6]) check(s, minor);
    /* deterministic digit storms (xorshift32), incl. grouping forms and
       trailing-zero fractions (the value rule's exact-text side) */
    let xs = 0x9e3779b9;
    function rnd() {
        xs ^= xs << 13; xs >>>= 0;
        xs ^= xs >>> 17;
        xs ^= xs << 5;  xs >>>= 0;
        return xs;
    }
    for (let t = 0; t < 600; t++) {
        const intLen = 1 + (rnd() % 20);
        const fracLen = rnd() % 9;
        const minor = rnd() % 7;
        let i = String(1 + (rnd() % 9));
        for (let k = 1; k < intLen; k++) i += String(rnd() % 10);
        let f = "";
        if (fracLen) {
            const digits = Array.from({ length: fracLen }, () => rnd() % 10);
            if (t % 3 === 2) digits[digits.length - 1] = 0;   // trailing zero runs
            f = "." + digits.join("");
        }
        const s = (t % 4 === 0 ? "-" : "") + i + f;
        if (t % 5 === 1) check(s.replace(/^(-?)(\d{1,3})/, "$1$2,"), minor);
        check(s, minor);
    }
    ok(rows >= 1500, "the differential ran " + rows + " rows");
    eq(mism, 0, "money factories == independent BigInt model of the value rule");
}

print("test_money_from: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
