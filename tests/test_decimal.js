/* test_decimal.js -- exact decimal arithmetic and money (design 21).
 *
 * THE ORACLE IS BigInt, which is an independent exact-integer implementation
 * already in the engine: a decimal is (coefficient, exponent), so add, sub and
 * mul are BigInt operations after aligning the exponents, and division is a
 * BigInt division plus a rounding decision written here from the definitions.
 * A hand-written table of expected answers would only test my imagination.
 *
 * The property that justifies Money.allocate is that the shares sum EXACTLY to
 * the original, so that is asserted on every split, not sampled.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_decimal.js
 */
import { Decimal, Money } from "dyna:decimal";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const D = (x) => new Decimal(x);

/* ---------------------------------------------------------- the oracle */

/* Text -> { c: BigInt coefficient, e: exponent }, exactly. */
function toCE(s) {
    let sign = 1n, i = 0;
    if (s[0] === "-") { sign = -1n; i = 1; } else if (s[0] === "+") { i = 1; }
    let digits = "", e = 0, seenDot = false;
    for (; i < s.length; i++) {
        const ch = s[i];
        if (ch >= "0" && ch <= "9") { digits += ch; if (seenDot) e--; }
        else if (ch === ".") seenDot = true;
        else if (ch === "e" || ch === "E") { e += parseInt(s.slice(i + 1), 10); break; }
        else throw new Error("bad literal " + s);
    }
    return { c: sign * BigInt(digits === "" ? "0" : digits), e };
}
/* { c, e } -> the canonical plain text this module produces. */
function fromCE(c, e) {
    if (c === 0n) return "0";
    let neg = c < 0n;
    let d = (neg ? -c : c).toString();
    while (d.length > 1 && d.endsWith("0") && e < 0) { d = d.slice(0, -1); e++; }
    while (e > 0) { d += "0"; e--; }
    let out;
    if (e === 0) out = d;
    else {
        const k = -e;
        out = k >= d.length ? "0." + "0".repeat(k - d.length) + d
                            : d.slice(0, d.length - k) + "." + d.slice(d.length - k);
    }
    return (neg ? "-" : "") + out;
}
function align(a, b) {
    const e = Math.min(a.e, b.e);
    const sa = a.c * 10n ** BigInt(a.e - e), sb = b.c * 10n ** BigInt(b.e - e);
    return [sa, sb, e];
}
const oracleAdd = (x, y) => { const [a, b, e] = align(toCE(x), toCE(y)); return fromCE(a + b, e); };
const oracleSub = (x, y) => { const [a, b, e] = align(toCE(x), toCE(y)); return fromCE(a - b, e); };
const oracleMul = (x, y) => {
    const a = toCE(x), b = toCE(y);
    return fromCE(a.c * b.c, a.e + b.e);
};

/* ------------------------------------------------------------- parsing */

eq(D("0").toString(), "0", "zero");
eq(D("-0").toString(), "0", "negative zero is zero");
eq(D("1.5").toString(), "1.5", "a simple value");
eq(D("1.500").toString(), "1.5", "trailing zeros are not part of the value");
eq(D("000123").toString(), "123", "leading zeros");
eq(D(".5").toString(), "0.5", "a leading point");
eq(D("5.").toString(), "5", "a trailing point");
eq(D("1e3").toString(), "1000", "an exponent");
eq(D("1E-3").toString(), "0.001", "a negative exponent");
eq(D("-1.5e2").toString(), "-150", "sign, fraction and exponent");
eq(D(1.5).toString(), "1.5", "a JS number arrives through its own text");
eq(D(0.1).toString(), "0.1", "so 0.1 is 0.1, not its binary expansion");
eq(D(D("7.25")).toString(), "7.25", "a Decimal copies");
for (const bad of ["", " 1", "1 ", "abc", "1.2.3", "1e", "1e+", "0x10", "1,5",
                   "--1", "1e999999999999", "+", ".", "Infinity", "NaN"])
    throws(() => D(bad), "refuses " + JSON.stringify(bad));
throws(() => D(NaN), "refuses the number NaN");
throws(() => D(Infinity), "refuses Infinity");
throws(() => D({}), "refuses an object");
throws(() => D(), "a value is required");

/* THE HEADLINE: this is what the type exists for. */
eq(D("0.1").add(D("0.2")).toString(), "0.3", "0.1 + 0.2 is 0.3");
assert(D("0.1").add(D("0.2")).equals(D("0.3")), "and equals 0.3");
assert(0.1 + 0.2 !== 0.3, "which the double sum does NOT (that is the point)");

/* -------------------------------------------- add, sub, mul vs BigInt */

{
    const lits = ["0", "1", "-1", "0.5", "-0.5", "3.14159", "-2.718281828",
                  "1e10", "1e-10", "123456789.987654321", "-0.000001",
                  "99999999999999999999", "0.1", "0.2", "0.3", "7", "-1000000",
                  "12345678901234567890.12345678901234567890",
                  /* PAST THE INLINE BUFFER: 40 digits is the boundary, and a
                   * corpus that stops there never reallocates. */
                  "1." + "123456789".repeat(11),
                  "-" + "9".repeat(60),
                  "0." + "0".repeat(50) + "7",
                  "1".repeat(120)];
    let checked = 0, bad = 0;
    for (const x of lits) for (const y of lits) {
        const a = D(x), b = D(y);
        const got = [a.add(b).toString(), a.sub(b).toString(), a.mul(b).toString()];
        const want = [oracleAdd(x, y), oracleSub(x, y), oracleMul(x, y)];
        for (let k = 0; k < 3; k++) {
            checked++;
            if (got[k] !== want[k]) {
                bad++;
                if (bad < 5)
                    print("  " + ["add", "sub", "mul"][k] + "(" + x + ", " + y +
                          ") got " + got[k] + " want " + want[k]);
            }
        }
    }
    assert(bad === 0, "add/sub/mul match BigInt exactly (" + (checked - bad) +
                      "/" + checked + ")");
    assert(checked === lits.length * lits.length * 3,
           "the sweep ran every pair (" + checked + ")");
}

/* ------------------------------------------------- division and rounding */

eq(D("10").div(D("4")).toString(), "2.5", "an exact quotient is exact");
eq(D("1").div(D("8")).toString(), "0.125", "and so is this one");
eq(D("1").div(D("3")).toString(), "0." + "3".repeat(34),
   "an inexact one gets 34 significant digits, which is decimal128");
eq(D("1").div(D("3"), { precision: 5 }).toString(), "0.33333", "precision is a choice");
eq(D("2").div(D("3"), { precision: 5 }).toString(), "0.66667", "and it rounds");
eq(D("2").div(D("3"), { precision: 5, rounding: "down" }).toString(), "0.66666",
   "as the mode says");
eq(D("-1").div(D("3"), { precision: 3 }).toString(), "-0.333", "sign survives");
eq(D("100").div(D("10")).toString(), "10", "a whole quotient");
throws(() => D("1").div(D("0")), "division by zero is refused");
throws(() => D("1").div(D("3"), { precision: 0 }), "precision must be positive");
throws(() => D("1").div(D("3"), { rounding: "sideways" }), "an unknown rounding mode");

{
    /* Every mode on the same tie, checked against the definitions. */
    const cases = [
        ["0.5", "up", "1"], ["0.5", "down", "0"], ["0.5", "ceil", "1"],
        ["0.5", "floor", "0"], ["0.5", "halfUp", "1"], ["0.5", "halfDown", "0"],
        ["0.5", "halfEven", "0"], ["0.5", "halfOdd", "1"],
        ["1.5", "halfEven", "2"], ["2.5", "halfEven", "2"], ["2.5", "halfOdd", "3"],
        ["-0.5", "ceil", "0"], ["-0.5", "floor", "-1"], ["-0.5", "up", "-1"],
        ["-0.5", "down", "0"], ["-2.5", "halfEven", "-2"],
        ["0.6", "halfDown", "1"], ["0.4", "halfUp", "0"],
    ];
    let bad = 0;
    for (const [v, mode, want] of cases)
        if (D(v).round(0, mode).toString() !== want) {
            bad++;
            print("  round(" + v + ", " + mode + ") = " +
                  D(v).round(0, mode).toString() + ", want " + want);
        }
    assert(bad === 0, "all eight rounding modes agree with their definitions (" +
                      (cases.length - bad) + "/" + cases.length + ")");
}
eq(D("3.14159").round(2).toString(), "3.14", "round to places");
eq(D("3.14159").round(4).toString(), "3.1416", "and it rounds up");
eq(D("1234").round(-2).toString(), "1200", "a negative place rounds to hundreds");
eq(D("9.99").round(1).toString(), "10", "a carry through every digit");
eq(D("9.99").toFixed(1), "10.0", "toFixed pads to exactly that many places");
eq(D("1").toFixed(3), "1.000", "including with nothing to show");
eq(D("1.5").toFixed(0), "2", "and none at all");
eq(D("-0.004").toFixed(2), "-0.00", "a value that rounds to zero keeps its sign in the text");
eq(D("0").toFixed(2), "0.00", "zero");
{
    /* Long literals must survive the growth path, digit for digit. */
    const long1 = "1." + "123456789".repeat(20);          /* 181 digits */
    eq(D(long1).toString(), long1, "a 181-digit literal round-trips");
    eq(D(long1).digits(), 181, "and keeps every digit");
    eq(D("9".repeat(100)).toString(), "9".repeat(100), "100 nines");
    eq(D("9".repeat(100)).add(D("1")).toString(), "1" + "0".repeat(100),
       "and a carry through all of them");
}

/* Division rounding, against a BigInt oracle for half-even. */
{
    let bad = 0, checked = 0;
    for (let a = -20; a <= 20; a++) {
        for (let b = 1; b <= 12; b++) {
            const prec = 6;
            const got = D(String(a)).div(D(String(b)), { precision: prec }).toString();
            /* scale so the quotient has prec+1 digits, then round half-even */
            const A = BigInt(a), B = BigInt(b);
            let scale = 0n, mag = A < 0n ? -A : A;
            if (mag === 0n) { checked++; if (got !== "0") bad++; continue; }
            let digitsA = mag.toString().length, digitsB = B.toString().length;
            const k = BigInt(prec + 1 + digitsB - digitsA > 0 ? prec + 1 + digitsB - digitsA : 0);
            scale = 10n ** k;
            const num = (A < 0n ? -A : A) * scale;
            let q = num / B, r = num % B;
            /* round q to prec digits, half-even, with r as the sticky bit */
            const qs = q.toString();
            let want;
            if (qs.length > prec) {
                const drop = qs.length - prec;
                let head = BigInt(qs.slice(0, prec));
                const guard = Number(qs[prec]);
                const rest = qs.slice(prec + 1).replace(/0/g, "") !== "" || r !== 0n;
                if (guard > 5 || (guard === 5 && rest) ||
                    (guard === 5 && !rest && head % 2n === 1n)) head += 1n;
                want = fromCE((A < 0n ? -head : head), -Number(k) + drop);
            } else {
                want = fromCE((A < 0n ? -q : q), -Number(k));
            }
            checked++;
            if (got !== want) {
                bad++;
                if (bad < 4) print("  " + a + "/" + b + " got " + got + " want " + want);
            }
        }
    }
    assert(bad === 0, "division matches the BigInt oracle (" + (checked - bad) +
                      "/" + checked + ")");
}

/* --------------------------------------------------- other operations */

eq(D("7").mod(D("3")).toString(), "1", "mod");
eq(D("-7").mod(D("3")).toString(), "-1", "the remainder takes the dividend's sign");
eq(D("7.5").mod(D("2")).toString(), "1.5", "a fractional remainder");
throws(() => D("1").mod(D("0")), "mod by zero is refused");
eq(D("2").pow(10).toString(), "1024", "an integer power");
eq(D("1.1").pow(2).toString(), "1.21", "exactly");
eq(D("2").pow(0).toString(), "1", "the zeroth power");
eq(D("2").pow(-2).toString(), "0.25", "a negative power divides");
throws(() => D("2").pow(1e9), "a huge exponent is refused");
eq(D("-3").abs().toString(), "3", "abs");
eq(D("3").neg().toString(), "-3", "neg");
eq(D("0").neg().toString(), "0", "negating zero stays zero");
eq(D("1").cmp(D("2")), -1, "cmp less");
eq(D("2").cmp(D("1")), 1, "cmp greater");
eq(D("1.0").cmp(D("1")), 0, "cmp equal across spellings");
eq(D("-1").cmp(D("1")), -1, "cmp across signs");
assert(D("1.50").equals(D("1.5")), "equality is by VALUE, not by text");
eq(D("0").isZero(), true, "isZero");
eq(D("0.0").isZero(), true, "however it was written");
eq(D("-5").sign(), -1, "sign");
eq(D("0").sign(), 0, "of zero");
eq(D("12.34").toNumber(), 12.34, "toNumber");
eq(JSON.stringify({ v: D("1.25") }), '{"v":"1.25"}', "toJSON is the exact text");
throws(() => Decimal.prototype.add.call({}, D("1")), "a foreign receiver is refused");

/* THE ROUND TRIP IS EXACT for decimals, unlike floats: assert equality. */
{
    const vals = ["0", "1", "-1", "0.5", "3.14159265358979323846", "1e20",
                  "0.00000000001", "-99999.99999", "123456789012345678901234567890"];
    let bad = 0;
    for (const v of vals) {
        const once = D(v).toString();
        if (D(once).toString() !== once) bad++;
    }
    assert(bad === 0, "parse(format(x)) is x EXACTLY, for every value");
}

/* ------------------------------------------------------------- Money */

{
    const m = new Money(1999, "USD");
    eq(m.toString(), "19.99", "money prints its major units");
    eq(m.format(), "$19.99", "and formats with a symbol");
    eq(m.amount(), 1999, "the amount is the MINOR unit count");
    eq(m.currency(), "USD", "and the currency is normalised");
    eq(new Money(1999, "usd").currency(), "USD", "from any case");
    eq(m.toDecimal().toString(), "19.99", "and it converts to a Decimal");
}
eq(new Money(1999, "JPY").toString(), "1999", "JPY has no minor unit");
eq(new Money(1999, "BHD").toString(), "1.999", "BHD has three");
eq(new Money(-500, "USD").toString(), "-5.00", "a negative amount");
eq(new Money(-5, "USD").toString(), "-0.05", "and a small one keeps its sign");
eq(new Money(0, "USD").format(), "$0.00", "zero");
eq(new Money(1999, "XYZ").format(), "19.99 XYZ", "an unknown currency prints its code");
eq(new Money(100, "USD", { minorDigits: 4 }).toString(), "0.0100",
   "minorDigits overrides: 100 units of a 4-digit currency");
throws(() => new Money(19.99, "USD"),
       "a FRACTIONAL amount is refused -- 1999 is $19.99, not 19.99");
throws(() => new Money(100, "US"), "a two-letter currency");
throws(() => new Money(100, "US1"), "a non-letter currency");
throws(() => new Money(100), "a currency is required");
throws(() => new Money(100, "USD").add(new Money(100, "EUR")),
       "USD plus EUR is a missing exchange rate, not arithmetic");
throws(() => new Money(100, "USD").add(5), "and the operand must be a Money");
eq(new Money(100, "USD").add(new Money(50, "USD")).amount(), 150, "add");
eq(new Money(100, "USD").sub(new Money(150, "USD")).amount(), -50, "sub goes negative");
eq(new Money(100, "USD").mul(3).amount(), 300, "mul by an integer");
throws(() => new Money(100, "USD").mul(1.5), "but not by a fraction");
eq(new Money(100, "USD").cmp(new Money(200, "USD")), -1, "cmp");
assert(new Money(100, "USD").equals(new Money(100, "USD")), "equals");

/* THE INVARIANT THAT JUSTIFIES allocate: the shares sum to the original. */
{
    let bad = 0, checked = 0;
    const weights = [[1, 1, 1], [1, 1], [70, 30], [1, 2, 3], [1], [0, 1, 1],
                     [5, 5, 5, 5, 5, 5, 5]];
    for (let amount = -13; amount <= 101; amount++) {
        for (const w of weights) {
            const parts = new Money(amount, "USD").allocate(w);
            let sum = 0;
            for (const p of parts) sum += p.amount();
            checked++;
            if (sum !== amount || parts.length !== w.length) {
                bad++;
                if (bad < 4)
                    print("  allocate(" + amount + ", " + JSON.stringify(w) +
                          ") sums to " + sum);
            }
        }
    }
    assert(bad === 0, "every allocation sums EXACTLY to the original (" +
                      (checked - bad) + "/" + checked + ")");
    assert(checked === 115 * weights.length, "the sweep ran every case");
}
eq(JSON.stringify(new Money(100, "USD").allocate([1, 1, 1]).map((m) => m.amount())),
   "[34,33,33]", "the remainder goes to the earliest shares");
eq(JSON.stringify(new Money(5, "USD").allocate([70, 30]).map((m) => m.amount())),
   "[4,1]", "a weighted split");
throws(() => new Money(100, "USD").allocate([]), "an empty share list");
throws(() => new Money(100, "USD").allocate([0, 0]), "weights that sum to zero");
throws(() => new Money(100, "USD").allocate([1, -1]), "a negative weight");
throws(() => new Money(100, "USD").allocate([1.5]), "a fractional weight");
throws(() => new Money(100, "USD").allocate("no"), "a non-array");

if (fails) {
    print("test_decimal: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal failed");
}
print("test_decimal: " + n + " assertions, 0 failures");
