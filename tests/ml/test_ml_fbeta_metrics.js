// covers: dyna:ml.fbeta
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_fbeta_metrics");
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

await __kitS.section("fbeta(beta=1) == f1", async () => {
    const p = await precision(YT, YP), r = await recall(YT, YP);
    close(await fbeta(YT, YP, 1), await f1(YT, YP), "fbeta(beta=1) == f1");
    const lo = await fbeta(YT, YP, 0.5), hi = await fbeta(YT, YP, 2);
    assert(Math.min(p, r) <= lo && lo <= Math.max(p, r), "f0.5 lies between p and r");
    assert(Math.min(p, r) <= hi && hi <= Math.max(p, r), "f2 lies between p and r");
    {
        const yt = [1, 1, 1, 1, 0, 0, 0, 0];
        const yp = [1, 0, 0, 0, 0, 0, 0, 0];
        close(await precision(yt, yp), 1.0, "high-precision case");
        close(await recall(yt, yp), 0.25, "low-recall case");
        assert((await fbeta(yt, yp, 2)) < (await fbeta(yt, yp, 0.5)),
               "beta=2 (recall-weighted) scores lower when recall is the weak side");
    }
    await throwsRange(async () => await fbeta(YT, YP, 0), "fbeta beta must be positive");
    await throwsRange(async () => await fbeta(YT, YP, -1), "fbeta rejects a negative beta");
});
__kitS.end();

print("test_ml_metrics: all " + n + " tests passed");
