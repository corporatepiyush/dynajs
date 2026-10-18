// Split from tests/test_p2_batch3.js: the net sections.
import { Metrics } from "dyna:net";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
{
    const name = "test_hist_batch3_" + Date.now();
    Metrics.histogram(name, 1.0);
    throws(() => Metrics.histogram(name, NaN), /finite/, "histogram NaN");
    throws(() => Metrics.histogram(name, Infinity), /finite/, "histogram Infinity");
    throws(() => Metrics.histogram(name, -Infinity), /finite/, "histogram -Infinity");
}
if (fails) { print("test_net_p2_batch3: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_net_p2_batch3 failed"); }
print("test_net_p2_batch3: " + n + " assertions, 0 failures");
