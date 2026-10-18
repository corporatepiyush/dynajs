// timeout: 180
// Split from tests/test_data_low_fixes.js: the time sections.
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
{
    throws(() => RRule.fromString("RRULE:FREQ=DAILY;COUNT=3;UNTIL=20240101T000000Z"),
        "COUNT+UNTIL in an RRULE string is refused", /COUNT and UNTIL/);
    throws(() => new RRule({ freq: "DAILY", count: 3, until: E(2024, 1, 1) }),
        "COUNT+UNTIL in the options object is refused", /COUNT and UNTIL/);
    const yearly = new RRule({ freq: "YEARLY", dtstart: E(2024, 1, 1) });
    ok(yearly.between(E(2024, 1, 1), E(2030, 1, 1)).length === 5,
        "an in-horizon range still returns its occurrences");
    throws(() => yearly.between(E(2024, 1, 1), E(12000, 1, 1)),
        "a range past year 9999 reports exhaustion instead of truncating", /budget exhausted/);
}
print("");
print("== COMPAT-258: Temporal parseDate sign grammar ==");
{
    throws(() => parseDate("+2024-01-01"), "a sign with a 4-digit year is refused", /ISO 8601/);
    throws(() => parseDate("-0001-01-01"), "a negative 4-digit year is refused", /ISO 8601/);
    ok(String(parseDate("+002024-01-01")) === "2024-01-01", "a signed 6-digit year parses",
        String(parseDate("+002024-01-01")));
}
print("");
print("test_time_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_time_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
