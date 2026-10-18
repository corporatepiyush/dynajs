// timeout: 180
// Split from tests/test_data_low_fixes.js: the uuid sections.
import { ULIDTime } from "dyna:uuid";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
print("");
print("== COMPAT-194: Crockford aliases in ULIDTime ==");
{
    const canon = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
    const ms = ULIDTime(canon);
    ok(typeof ms === "number" && ms > 0, "the canonical ULID parses", String(ms));
    ok(ULIDTime("0LARZ3NDEKTSV4RRFFQ69G5FAV") === ms, "I/L alias for 1 decodes");
    ok(ULIDTime("01ARZ3NDEKTSV4RRFFQ69G5FAV".replace("0", "O")) === ms, "O alias for 0 decodes");
}
print("");
print("test_uuid_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_uuid_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
