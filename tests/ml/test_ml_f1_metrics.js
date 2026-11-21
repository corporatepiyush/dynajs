// covers: dyna:ml.f1
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_f1_metrics");
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

await __kitS.section("perfect precision", async () => {
    const yt = [1, 1, 0, 0];
    close(await precision(yt, yt), 1, "perfect precision");
    close(await recall(yt, yt), 1, "perfect recall");
    close(await f1(yt, yt), 1, "perfect f1");
    close(await matthewsCorrcoef(yt, yt), 1, "perfect MCC is 1");
    close(await cohenKappa(yt, yt), 1, "perfect kappa is 1");
    const inv = [0, 0, 1, 1];
    close(await matthewsCorrcoef(yt, inv), -1, "fully inverted MCC is -1");
    close(await cohenKappa(yt, inv), -1, "fully inverted kappa is -1");
    close(await f1(yt, inv), 0, "fully inverted f1 is 0");
});
__kitS.end();

print("test_ml_metrics: all " + n + " tests passed");
