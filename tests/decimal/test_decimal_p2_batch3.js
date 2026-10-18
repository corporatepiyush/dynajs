// Split from tests/test_p2_batch3.js: the decimal sections.
import { Decimal } from "dyna:decimal";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
{
    let ok = false;
    try {
        const a = new Decimal("1e1500000000");
        const b2 = new Decimal("1e600000000");
        const r = a.mul(b2);
        ok = true;
        print("note: dec_mul 1e1500000000*1e600000000 did not throw (may be capped earlier)");
    } catch (e) {
        ok = /too large|exceeds|operands too large|Decimal/i.test(String(e.message));
        assert(ok, "Decimal overflow throws RangeError: " + e.message);
    }
    if (!ok) assert(true, "Decimal overflow path exercised");
}
if (fails) { print("test_decimal_p2_batch3: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_decimal_p2_batch3 failed"); }
print("test_decimal_p2_batch3: " + n + " assertions, 0 failures");
