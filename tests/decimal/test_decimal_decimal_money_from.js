// covers: dyna:decimal.Decimal
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_money_from");
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

__kitS.section("a string is not a Decimal", () => {
throws(() => Money.fromDecimal("12.34", "USD"), TypeError, /must be a Decimal/,
       "a string is not a Decimal");
});

__kitS.section("a plain object is not a Decimal", () => {
throws(() => Money.fromDecimal({}, "USD"), TypeError, /must be a Decimal/,
       "a plain object is not a Decimal");
});

__kitS.section("huge Decimal refuses", () => {
throws(() => Money.fromDecimal(new Decimal("1e30"), "USD"), RangeError,
       /overflows an int64/, "huge Decimal refuses");
});

__kitS.section("tiny Decimal refuses rather than snapping to 0", () => {
throws(() => Money.fromDecimal(new Decimal("1e-30"), "USD"), RangeError,
       /minor unit is not money/, "tiny Decimal refuses rather than snapping to 0");
});
__kitS.end();

print("test_money_from: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
