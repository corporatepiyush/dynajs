// timeout: 180
// Split from tests/test_data_low_fixes.js: the structures sections.
import { IntervalTree } from "dyna:structures";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
print("");
print("== COMPAT-252: reversed intervals are refused ==");
{
    const t = new IntervalTree();
    throws(() => t.insert(10, 1, "x"), "insert(lo > hi) is refused", /lo must not exceed/);
    t.insert(1, 10, "ok");
    ok(t.size === 1, "a normal interval still inserts", String(t.size));
}
print("");
print("test_structures_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_structures_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
