// covers: dyna:decimal.Money
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_money_from");
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

__kitS.section("the constructor bag goes strict too", () => {
throws(() => new Money(1, "USD", { minorDigit: 2 }), TypeError,
       /unknown option "minorDigit" \(valid: minorDigits\)/,
       "the constructor bag goes strict too");
});
__kitS.end();

print("test_money_from: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
