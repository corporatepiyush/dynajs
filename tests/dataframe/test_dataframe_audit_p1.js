// flags: --std
// Split from tests/test_audit_p1.js: the dataframe sections.
import { DataFrame } from "dyna:dataframe";

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
    const N = 2000;
    const ids = new Int32Array(N);
    const vals = new Float64Array(N);
    for (let i=0;i<N;i++) { ids[i]=i; vals[i]=i*1.5; }
    const df = new DataFrame({ id: ids, v: vals });
    const g = df.GROUP_BY_SUM("id", "v");
    assert(g && typeof g === "object", "dataframe GROUP_BY_SUM with 2k distinct keys completes (hash seeded)");
}
if (fails) { print("test_dataframe_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_dataframe_audit_p1 failed"); }
print("test_dataframe_audit_p1: " + n + " assertions, 0 failures");
