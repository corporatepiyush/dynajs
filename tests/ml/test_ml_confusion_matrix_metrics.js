// covers: dyna:ml.confusionMatrix
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_confusion_matrix_metrics");
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

await __kitS.section("precision = TP/(TP+FP) = 2/3", async () => {
    close(await precision(YT, YP), TP / (TP + FP), "precision = TP/(TP+FP) = 2/3");
    close(await recall(YT, YP), TP / (TP + FN), "recall = TP/(TP+FN) = 2/3");
    close(await specificity(YT, YP), TN / (TN + FP), "specificity = TN/(TN+FP) = 6/7");
    close(await f1(YT, YP), 2 * (2 / 3) * (2 / 3) / ((2 / 3) + (2 / 3)), "f1 = harmonic mean = 2/3");
    close(await balancedAccuracy(YT, YP), 0.5 * (2 / 3 + 6 / 7), "balancedAccuracy = (rec+spec)/2");
    close(await matthewsCorrcoef(YT, YP), (TP * TN - FP * FN) /
          Math.sqrt((TP + FP) * (TP + FN) * (TN + FP) * (TN + FN)), "MCC = 11/21");
    close(await matthewsCorrcoef(YT, YP), 11 / 21, "MCC numerically");
    {
        const po = (TP + TN) / 10, pe = (3 * 3 + 7 * 7) / 100;
        close(await cohenKappa(YT, YP), (po - pe) / (1 - pe), "cohenKappa vs chance agreement");
    }
    close(await accuracy(YT, YP), 0.8, "accuracy for reference");
    {
        const cm = await confusionMatrix(YT, YP);
        close(cm[1][1], TP, "confusionMatrix TP");
        close(cm[0][1], FP, "confusionMatrix FP");
        close(cm[1][0], FN, "confusionMatrix FN");
        close(cm[0][0], TN, "confusionMatrix TN");
    }
});
__kitS.end();

print("test_ml_metrics: all " + n + " tests passed");
