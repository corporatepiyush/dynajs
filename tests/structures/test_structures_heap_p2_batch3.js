// covers: dyna:structures.Heap
// Split from tests/test_p2_batch3.js: the structures sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_heap_p2_batch3");
import { Heap } from "dyna:structures";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}

__kitS.section("Heap push throw", () => {
    const h = new Heap((a, b) => { if (a === 999 || b === 999) throw new Error("bad"); return a - b; });
    h.push(1); h.push(2);
    let threw = false;
    try { h.push(999); } catch (e) { threw = true; }
    assert(threw, "Heap push throw");
    assert(h.size === 2, "Heap size 2 after failed push " + h.size);
    h.push(0);
    assert(h.pop() === 0, "Heap valid after rollback");
});
__kitS.end();
if (fails) { print("test_structures_p2_batch3: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_structures_p2_batch3 failed"); }
print("test_structures_p2_batch3: " + n + " assertions, 0 failures");
