// timeout: 180
// covers: dyna:time.parseDate
// Split from tests/test_data_low_fixes.js: the time sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_date_data_low_fixes");
import { RRule, parseDate } from "dyna:time";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
print("");
print("== COMPAT-256/257: RRule horizon and COUNT+UNTIL ==");

print("");
print("== COMPAT-258: Temporal parseDate sign grammar ==");
__kitS.section("a sign with a 4-digit year is refused", () => {
    throws(() => parseDate("+2024-01-01"), "a sign with a 4-digit year is refused", /ISO 8601/);
    throws(() => parseDate("-0001-01-01"), "a negative 4-digit year is refused", /ISO 8601/);
    ok(String(parseDate("+002024-01-01")) === "2024-01-01", "a signed 6-digit year parses",
        String(parseDate("+002024-01-01")));
});
__kitS.end();
print("");
print("test_time_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_time_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
