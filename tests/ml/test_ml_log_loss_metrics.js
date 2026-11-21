// covers: dyna:ml.logLoss
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_log_loss_metrics");
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

await __kitS.section("uniform over 3 classes is log 3", async () => {
    const yk = [0, 1, 2, 0, 1, 2];
    const uniform = yk.map(() => [1 / 3, 1 / 3, 1 / 3]);
    close(await logLoss(yk, uniform), Math.log(3), "uniform over 3 classes is log 3");

    const uniform2 = [0, 1, 0, 1].map(() => [0.5, 0.5]);
    close(await logLoss([0, 1, 0, 1], uniform2), Math.log(2), "uniform over 2 is log 2");

    close(await logLoss([0, 1], [[1, 0], [0, 1]]), 0, "a perfect prediction costs 0");
    n++;
    if (!Number.isFinite(await logLoss([0, 1], [[0, 1], [1, 0]])))
        throw new Error("a confidently wrong prediction must stay finite");

    const P = [[0.9, 0.1], [0.3, 0.7], [0.6, 0.4], [0.2, 0.8]];
    const yb = [0, 1, 0, 1];
    close(await logLoss(yb, P), await logLoss(yb, P.map(r => r[1])),
          "the matrix and column forms agree");

    n++;
    let threw2 = false;
    try { await logLoss([0, 1, 2], [[0.5, 0.5], [0.5, 0.5], [0.5, 0.5]]); }
    catch (e) { threw2 = e instanceof TypeError; }
    if (!threw2) throw new Error("a column count that does not match the labels must throw");
});

await __kitS.section("logLoss NaN refusal", async () => {
    await throwsRange(async () => await logLoss([0, 1], [NaN, 0.5]), "logLoss NaN refusal");
    await throwsRange(async () => await logLoss([NaN, 1], [[0.9, 0.1], [0.1, 0.9]]),
                "logLoss NaN label refusal");
    close(await rocAuc(YT, [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.05]), 1.0,
          "rocAuc finite path unchanged");
});
__kitS.end();

print("test_ml_metrics: all " + n + " tests passed");
