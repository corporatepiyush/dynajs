// covers: c:src/modules/libc/libc-os.c
// Split from tests/test_p2_batch3.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_p2_batch3");
export {};

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
__kitS.section("setTimeout huge delay", () => {
    const id = setTimeout(() => {}, 5e12);
    assert(typeof id === "number", "setTimeout huge delay");
    clearTimeout(id);
    const id2 = setTimeout(() => {}, NaN);
    assert(typeof id2 === "number", "setTimeout NaN");
    clearTimeout(id2);
    const id3 = setTimeout(() => {}, -100);
    assert(typeof id3 === "number", "setTimeout negative");
    clearTimeout(id3);
});
__kitS.end();
if (fails) { print("test_core_p2_batch3: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_core_p2_batch3 failed"); }
print("test_core_p2_batch3: " + n + " assertions, 0 failures");
