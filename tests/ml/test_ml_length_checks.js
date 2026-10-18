// dyna:ml length checks, found by the signature-driven input generator
// (tests/lib/sigfuzz.js): a metric compares the two target lengths BEFORE it
// reads either (a sparse array of length 2^32-1 was walked first), and CSR
// checks columns against values and the rowPointers endpoints before it
// copies anything.
import * as ml from "dyna:ml";
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
for (const [a, b, w] of [[huge(), [1], "a sparse yTrue"], [[1], huge(), "a sparse yPred"], [[1, 0], [1], "unequal lengths"]]) {
    const t0 = performance.now();
    const m = await aerr(() => ml.accuracy(a, b));
    ok(/^TypeError: .*length/.test(m), "accuracy: " + w + " is refused (" + m + ")");
    ok(performance.now() - t0 < 500, "accuracy: " + w + " refused before any work");
}
eq(await ml.accuracy([1, 0, 1], [1, 1, 1]), 2 / 3, "accuracy on equal lengths (control)");
eq(err(() => new ml.CSR([1, 2], [0], [0, 2], 2)), "TypeError: ml.CSR: columns has 1 entries but values has 2; there is one column index per value", "CSR: columns shorter than values");
eq(err(() => new ml.CSR([1, 2], [0, 1], [0, 5], 2)), "RangeError: ml.CSR: rowPointers must start at 0 and end at the value count (2)", "CSR: rowPointers ending past the values");
eq(err(() => new ml.CSR([1, 2], [0, 1], [1, 2], 2)), "RangeError: ml.CSR: rowPointers must start at 0 and end at the value count (2)", "CSR: rowPointers not starting at 0");
fast("CSR: a sparse values array", () => new ml.CSR(huge(), [0], [0, 1], 2), /^TypeError: ml\.CSR: columns has 1 entries but values has 4294967295/);
ok(err(() => new ml.CSR([1, 2], [0, 1], [0, 1, 2], 2)) === "no throw", "CSR: a consistent 2x2 matrix (control)");

print("test_ml_length_checks" + ": " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
