// covers: dyna:ml.accuracy
// dyna:ml length checks, found by the signature-driven input generator
// (tests/lib/sigfuzz.js): a metric compares the two target lengths BEFORE it
// reads either (a sparse array of length 2^32-1 was walked first), and CSR
// checks columns against values and the rowPointers endpoints before it
// copies anything.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_accuracy_length_checks");
import * as ml from "dyna:ml";
import { bestMs, cpuNow } from "../lib/cpuclock.js";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
async function aerr(f) { try { await f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const huge = () => { const a = []; a.length = 2 ** 32 - 1; return a; };
function fast(label, f, re) {
    const { ms: dt, value: m } = bestMs(() => err(f));
    ok(re.test(m), label + " (" + m + ")");
    ok(dt < 500, label + ": refused before any work (" + dt.toFixed(1) + " ms)");
}

await __kitS.section("a sparse yTrue", async () => {
for (const [a, b, w] of [[huge(), [1], "a sparse yTrue"], [[1], huge(), "a sparse yPred"], [[1, 0], [1], "unequal lengths"]]) {
    const t0 = cpuNow();
    const m = await aerr(() => ml.accuracy(a, b));
    ok(/^TypeError: .*length/.test(m), "accuracy: " + w + " is refused (" + m + ")");
    ok(cpuNow() - t0 < 500, "accuracy: " + w + " refused before any work");
}
});

await __kitS.section("accuracy on equal lengths (control)", async () => {
eq(await ml.accuracy([1, 0, 1], [1, 1, 1]), 2 / 3, "accuracy on equal lengths (control)");
});
__kitS.end();

print("test_ml_length_checks" + ": " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
