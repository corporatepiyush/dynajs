// timeout: 180
// Split from tests/test_data_low_fixes.js: the dataframe sections.
import { DataFrame } from "dyna:dataframe";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
print("");
print("== COMPAT-244/245/247: dataframe numeric guards ==");
{
    const f = new DataFrame({ v: new Int32Array([1, 2, 3, 4]) });
    throws(() => f.RESAMPLE("v", Infinity, "sum"), "RESAMPLE(Infinity) is refused", /finite/);
    ok(f.SLICE(-0.5).ROWS === 4, "SLICE(-0.5) truncates to 0 like Array.prototype.slice",
        String(f.SLICE(-0.5).ROWS));
    ok(f.SLICE(-1.5).ROWS === 1, "SLICE(-1.5) drops one row like Array.prototype.slice",
        String(f.SLICE(-1.5).ROWS));
    const wide = new DataFrame({ v: new Float64Array([-1e308, 0, 1e308]) });
    throws(() => wide.HISTOGRAM("v", 4), "HISTOGRAM over an overflowing span is refused", /too wide/);
}
print("");
print("== COMPAT-242: explicit undefined weight column ==");
{
    const f = new DataFrame({ v: new Float64Array([3, 1, 2]), w: new Float64Array([1, 5, 2]) });
    const a = f.TOP_K_WEIGHTED("v", undefined, 2);
    ok(a.keys && a.keys.length === 2, "TOP_K_WEIGHTED(col, undefined, k) resolves k from argv[2]",
        JSON.stringify(a.keys));
    const b = f.TOP_K_WEIGHTED("v", "w", 1);
    ok(b.keys.length === 1 && b.keys[0] === 1, "an explicit weight column still works",
        JSON.stringify(b.keys));
    throws(() => f.TOP_K_WEIGHTED("v", undefined, 65537), "an out-of-range k is still refused", /k must be/);
}
print("");
print("test_dataframe_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_dataframe_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
