// timeout: 180
// covers: dyna:dataframe.DataFrame
// Split from tests/test_data_low_fixes.js: the dataframe sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dataframe_data_frame_low_fixes");
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
__kitS.section("RESAMPLE(Infinity) is refused", () => {
    const f = new DataFrame({ v: new Int32Array([1, 2, 3, 4]) });
    throws(() => f.RESAMPLE("v", Infinity, "sum"), "RESAMPLE(Infinity) is refused", /finite/);
    ok(f.SLICE(-0.5).ROWS === 4, "SLICE(-0.5) truncates to 0 like Array.prototype.slice",
        String(f.SLICE(-0.5).ROWS));
    ok(f.SLICE(-1.5).ROWS === 1, "SLICE(-1.5) drops one row like Array.prototype.slice",
        String(f.SLICE(-1.5).ROWS));
    const wide = new DataFrame({ v: new Float64Array([-1e308, 0, 1e308]) });
    throws(() => wide.HISTOGRAM("v", 4), "HISTOGRAM over an overflowing span is refused", /too wide/);
});
print("");
print("== COMPAT-242: explicit undefined weight column ==");
__kitS.section("TOP_K_WEIGHTED(col, undefined, k) resolves k from argv[2]", () => {
    const f = new DataFrame({ v: new Float64Array([3, 1, 2]), w: new Float64Array([1, 5, 2]) });
    const a = f.TOP_K_WEIGHTED("v", undefined, 2);
    ok(a.keys && a.keys.length === 2, "TOP_K_WEIGHTED(col, undefined, k) resolves k from argv[2]",
        JSON.stringify(a.keys));
    const b = f.TOP_K_WEIGHTED("v", "w", 1);
    ok(b.keys.length === 1 && b.keys[0] === 1, "an explicit weight column still works",
        JSON.stringify(b.keys));
    throws(() => f.TOP_K_WEIGHTED("v", undefined, 65537), "an out-of-range k is still refused", /k must be/);
});
print("");
__kitS.section("HISTOGRAM edges span the finite values only", () => {
    // +-Infinity never reach a bin: the bin index used to be a uint32 cast of an infinite value (UB; deep
    // line-read wave D-A). The span is taken over the finite values and the infinities are not counted.
    const h = new DataFrame({ x: new Float64Array([1, Infinity, -Infinity, 2]) }).HISTOGRAM("x", 4);
    ok(Array.from(h.edges).join() === "1,1.25,1.5,1.75,2", "HISTOGRAM edges span the finite values only");
    ok(Array.from(h.counts).join() === "1,0,0,1", "HISTOGRAM leaves +-Infinity out of every bin");
});
__kitS.end();

print("test_dataframe_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_dataframe_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
