// covers: dyna:ml.precision
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_precision_metrics");
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

await __kitS.section("accuracy flatters the always-negative classifier", async () => {
    const yt = [1, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    const allNeg = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    close(await accuracy(yt, allNeg), 0.9, "accuracy flatters the always-negative classifier");
    close(await precision(yt, allNeg), 0, "precision exposes it (no predicted positives -> 0)");
    close(await recall(yt, allNeg), 0, "recall exposes it");
    close(await f1(yt, allNeg), 0, "f1 exposes it");
    close(await matthewsCorrcoef(yt, allNeg), 0, "MCC exposes it");
    close(await cohenKappa(yt, allNeg), 0, "kappa exposes it");
    close(await balancedAccuracy(yt, allNeg), 0.5, "balancedAccuracy exposes it (chance)");

    const allPos = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1];
    close(await recall(yt, allPos), 1, "always-positive has perfect recall");
    close(await precision(yt, allPos), 0.1, "...and precision equal to the base rate");
    close(await specificity(yt, allPos), 0, "...and zero specificity");
    close(await balancedAccuracy(yt, allPos), 0.5, "...and chance balanced accuracy");
});

await __kitS.section("precision is label-agnostic", async () => {
    const yt = [1, 1, 1, 0, 0], yp = [1, 1, 0, 1, 0];
    const yt2 = yt.map(v => 1 - v), yp2 = yp.map(v => 1 - v);
    close(await precision(yt2, yp2, 0), await precision(yt, yp, 1), "precision is label-agnostic");
    close(await recall(yt2, yp2, 0), await recall(yt, yp, 1), "recall is label-agnostic");
    close(await f1(yt2, yp2, 0), await f1(yt, yp, 1), "f1 is label-agnostic");
    close(await recall(yt, yp, 0), await specificity(yt, yp, 1),
          "recall of the negative class IS specificity of the positive one");
    const a = [7, 7, 7, -3, -3], b = [7, 7, -3, 7, -3];
    close(await precision(a, b, 7), await precision(yt, yp, 1), "works with arbitrary label values");
});

await __kitS.section("mismatched lengths must throw", async () => {
    n++;
    let threw = false;
    try { await precision([1, 0], [1]); } catch (e) { threw = true; }
    if (!threw) throw new Error("mismatched lengths must throw");
    n++;
    threw = false;
    try { await precision([], []); } catch (e) { threw = true; }
    if (!threw) throw new Error("empty input must throw");
    close(await precision(new Float64Array(YT), new Float64Array(YP)), 2 / 3,
          "accepts Float64Array");
    n++;
    threw = false;
    try { await precision(YT, YP, { valueOf() { throw new Error("boom"); } }); }
    catch (e) { threw = true; }
    if (!threw) throw new Error("a throwing positive-label coercion must propagate");
    close(await precision(YT, YP), 2 / 3, "metrics still work after a failed coercion");
});
__kitS.end();

print("test_ml_metrics: all " + n + " tests passed");
