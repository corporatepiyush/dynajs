// covers: dyna:ml.rocAuc
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_roc_auc_metrics");
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

await __kitS.section("rocAuc equals the concordant-pair fraction", async () => {
    const S = [0.9, 0.8, 0.4, 0.7, 0.3, 0.2, 0.1, 0.05, 0.6, 0.15];
    close(await rocAuc(YT, S), 19 / 21, "rocAuc equals the concordant-pair fraction");

    close(await rocAuc([0, 0, 1, 1], [0.1, 0.2, 0.8, 0.9]), 1, "perfectly separable -> 1");
    close(await rocAuc([0, 0, 1, 1], [0.9, 0.8, 0.2, 0.1]), 0, "perfectly inverted -> 0");
    close(await rocAuc([0, 0, 1, 1], [0.5, 0.5, 0.5, 0.5]), 0.5, "constant scorer -> exactly 0.5");
    close(await rocAuc([0, 1], [0.5, 0.5]), 0.5, "single tied pair -> 0.5");
    close(await rocAuc([0, 0, 1, 1], [0.1, 0.5, 0.5, 0.9]), 0.875, "partial ties are averaged");
    {
        const mono = S.map(v => Math.log(v + 1) * 3 + 7);
        close(await rocAuc(YT, mono), await rocAuc(YT, S), "AUC depends only on rank order");
    }
    close(await rocAuc(YT, S.map(v => -v)), 1 - (await rocAuc(YT, S)), "negating scores gives 1-AUC");
    await throwsRange(async () => await rocAuc([1, 1, 1], [0.1, 0.2, 0.3]), "rocAuc needs a negative sample");
    await throwsRange(async () => await rocAuc([0, 0, 0], [0.1, 0.2, 0.3]), "rocAuc needs a positive sample");
});

await __kitS.section("rocAuc NaN score refused", async () => {
    await throwsRange(async () => await rocAuc([0, 1], [NaN, 0.5]), "rocAuc NaN score refused");
    await throwsRange(async () => await rocAuc([NaN, 1], [0.1, 0.9]), "rocAuc NaN label refused");
    await throwsRange(async () => await averagePrecision([0, 1], [0.5, NaN]),
                "averagePrecision NaN score refused");
    close(await rocAuc(YT, [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.05]), 1.0,
          "rocAuc perfect ranking");
});
__kitS.end();

print("test_ml_metrics: all " + n + " tests passed");
