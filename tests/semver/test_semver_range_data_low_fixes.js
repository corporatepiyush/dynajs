// timeout: 180
// covers: dyna:semver.Range
// Split from tests/test_data_low_fixes.js: the semver sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_semver_range_data_low_fixes");
import { Range, parse as parseSemver } from "dyna:semver";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
print("");
print("== COMPAT-254/255: semver whitespace and numeric-id overflow ==");

__kitS.section("a newline-separated range evaluates", () => {
    ok(new Range(">=1.0.0\n<2.0.0").test("1.5.0"), "a newline-separated range evaluates");
    throws(() => parseSemver("1.0.0-18446744073709551617"), "an overflowing numeric prerelease id is refused",
        /invalid|range|version/);
});
__kitS.end();
print("");
print("test_semver_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_semver_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
