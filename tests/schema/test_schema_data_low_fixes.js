// timeout: 180
// Split from tests/test_data_low_fixes.js: the schema sections.
import { Schema } from "dyna:schema";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
print("");
print("== COMPAT-239: schema numeric keywords must be finite ==");
{
    throws(() => Schema.compile({ minimum: NaN }), "minimum: NaN is refused", /finite/);
    throws(() => Schema.compile({ maximum: Infinity }), "maximum: Infinity is refused", /finite/);
    ok(Schema.validate({ minimum: 1 }, 2).valid === true, "a finite bound still works");
}
print("");
print("test_schema_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_schema_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
