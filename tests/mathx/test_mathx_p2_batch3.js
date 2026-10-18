// Split from tests/test_p2_batch3.js: the mathx sections.
import * as mathx from "dyna:mathx";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
{
    throws(() => mathx.linspace(0, 1, 2000000), /1000000|must be/, "linspace cap");
    assert(mathx.linspace(0, 1, 5).length === 5, "linspace small ok");
}
if (fails) { print("test_mathx_p2_batch3: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_mathx_p2_batch3 failed"); }
print("test_mathx_p2_batch3: " + n + " assertions, 0 failures");
