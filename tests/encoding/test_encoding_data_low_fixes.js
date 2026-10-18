// timeout: 180
// Split from tests/test_data_low_fixes.js: the encoding sections.
import { JSON5Parse } from "dyna:encoding";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
{
    for (const bad of [".", "+", "-", "+.", "-.", ".e5", "+e1"])
        throws(() => JSON5Parse(bad), "JSON5 " + JSON.stringify(bad) + " is refused", /bad number/);
    ok(JSON5Parse(".5") === 0.5, "JSON5 .5 parses", String(JSON5Parse(".5")));
    ok(JSON5Parse("5.") === 5, "JSON5 5. parses", String(JSON5Parse("5.")));
    ok(JSON5Parse("5.e2") === 500, "JSON5 5.e2 parses", String(JSON5Parse("5.e2")));
    ok(JSON5Parse("0x20000000000001") === 9007199254740992,
        "a hex literal above 2^53 rounds instead of throwing",
        String(JSON5Parse("0x20000000000001")));
}
print("");
print("test_encoding_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_encoding_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
