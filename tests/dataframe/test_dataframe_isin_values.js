// DataFrame#ISIN values, found by the signature-driven input generator
// (tests/lib/sigfuzz.js): a hole or undefined entry is refused at its index,
// so a sparse list of length 2^32-1 fails at index 0 instead of being walked
// (and nothing is sized by the declared length first). Other values are still
// coerced to the column's type, and a coercion error propagates (the M1a-05
// pin in test_dataframe_audit_w2_native_1.js).
import { DataFrame } from "dyna:dataframe";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
async function aerr(f) { try { await f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const huge = () => { const a = []; a.length = 2 ** 32 - 1; return a; };
function fast(label, f, re) {
    const t0 = performance.now();
    const m = err(f);
    const dt = performance.now() - t0;
    ok(re.test(m), label + " (" + m + ")");
    ok(dt < 500, label + ": refused before any work (" + dt.toFixed(1) + " ms)");
}
const d = new DataFrame({ x: new Float64Array([1, 2, 3]), s: ["a", "b", "c"] });
fast("a sparse values list on a number column", () => d.ISIN("x", huge()), /^TypeError: ISIN: values\[0\] is missing \(a hole or undefined\)$/);
fast("a sparse values list on a string column", () => d.ISIN("s", huge()), /^TypeError: ISIN: values\[0\] is missing \(a hole or undefined\)$/);
eq(err(() => d.ISIN("s", ["a", , "c"])), "TypeError: ISIN: values[1] is missing (a hole or undefined)", "a hole is refused by index");
eq(err(() => d.ISIN("x", [2, undefined])), "TypeError: ISIN: values[1] is missing (a hole or undefined)", "an explicit undefined is refused by index");
eq(err(() => d.ISIN("x", [{ valueOf() { throw new RangeError("boom"); } }])), "RangeError: boom", "a coercion error propagates");
eq(JSON.stringify(Array.from(d.ISIN("x", [{ valueOf() { return 2; } }]))), "[0,1,0]", "an object value is coerced (control)");
eq(JSON.stringify(Array.from(d.ISIN("x", [2, 3]))), "[0,1,1]", "numbers on a number column (control)");
eq(JSON.stringify(Array.from(d.ISIN("s", ["c"]))), "[0,0,1]", "strings on a string column (control)");

print("test_dataframe_isin_values" + ": " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
