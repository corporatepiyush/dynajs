// covers: dyna:ml.averagePrecision
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_average_precision_metrics");
import {
    precision, recall, f1, fbeta, specificity, balancedAccuracy,
    matthewsCorrcoef, cohenKappa, rocAuc, averagePrecision,
    accuracy, confusionMatrix, logLoss,
} from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(got, want, m, tol = 1e-12) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + m + "\n  got:  " + got + "\n  want: " + want);
}
async function throwsRange(fn, m) {
    n++;
    try { await fn(); } catch (e) {
        if (e instanceof RangeError) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected RangeError): " + m);
}

const YT = [1, 1, 1, 0, 0, 0, 0, 0, 0, 0];
const YP = [1, 1, 0, 1, 0, 0, 0, 0, 0, 0];
const TP = 2, FP = 1, FN = 1, TN = 6;

await __kitS.section("averagePrecision is the step-function AP", async () => {
    const S = [0.9, 0.8, 0.4, 0.7, 0.3, 0.2, 0.1, 0.05, 0.6, 0.15];
    close(await averagePrecision(YT, S), (1 / 1 + 2 / 2 + 3 / 5) / 3,
          "averagePrecision is the step-function AP");
    close(await averagePrecision([0, 0, 1, 1], [0.1, 0.2, 0.8, 0.9]), 1,
          "perfectly separable AP is 1");
    assert((await averagePrecision(YT, S)) <= 1, "AP is bounded above by 1");
    assert((await averagePrecision(YT, S)) >= 3 / 10, "AP is bounded below by the base rate");
    await throwsRange(async () => await averagePrecision([1, 1], [0.1, 0.2]), "AP needs both classes");
});
__kitS.end();

print("test_ml_metrics: all " + n + " tests passed");
