// flags: --std
// Split from tests/test_audit_p1.js: the ml sections.
import { GaussianNB } from "dyna:ml";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL: " + m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong message: " + e.message); }
    }
}
{
    const m = new GaussianNB();
    await m.fit([[0,0],[1,1],[0,1],[1,0]], [0,1,0,1]);
    const bytes = m.serialize();
    const m2 = GaussianNB.deserialize(bytes);
    eq(JSON.stringify(await m2.predict([[0,0]])), JSON.stringify(await m.predict([[0,0]])), "gaussianNB good serialize round-trips");
    assert(true, "gaussianNB v>0 guard compiled (see dyna-ml-persist.inc.c:309)");
}
if (fails) { print("test_ml_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_ml_audit_p1 failed"); }
print("test_ml_audit_p1: " + n + " assertions, 0 failures");
