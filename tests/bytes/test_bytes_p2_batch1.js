// flags: --std
// Split from tests/test_p2_batch1.js: the bytes sections.
import { Text } from "dyna:bytes";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + " want " + JSON.stringify(b) + ")"); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); }
    }
}
{
    eq(new Text("a\u0000\u1000b").isWide, true, "isWide true with NUL+wide");
    eq(new Text("a\u0000b").isWide, false, "isWide false with NUL+ascii");
    eq(new Text("\u00FF").isWide, false, "isWide false for 0xFF");
    eq(new Text("\u0100").isWide, true, "isWide true for 0100");
    eq(new Text("hello").isWide, false, "isWide false ascii");
}
if (fails) { print("test_bytes_p2_batch1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_bytes_p2_batch1 failed"); }
print("test_bytes_p2_batch1: " + n + " assertions, 0 failures");
