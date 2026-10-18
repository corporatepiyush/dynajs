// timeout: 180
// Split from tests/test_data_low_fixes.js: the matcher sections.
import { Matcher } from "dyna:matcher";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
print("");
print("== SEC-049: new Matcher() has no stale argv read ==");
{
    throws(() => new Matcher(), "new Matcher() throws instead of reading a stale slot", /requires a pattern/);
    ok(new Matcher("ab").test("xabx") === true, "a normal Matcher still works");
}
print("");
print("test_matcher_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_matcher_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
