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

{
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
}

{
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
}

{
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
}

{
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
}

{
    const yt = [1, 1, 1, 0, 0], yp = [1, 1, 0, 1, 0];
    const yt2 = yt.map(v => 1 - v), yp2 = yp.map(v => 1 - v);
    close(await precision(yt2, yp2, 0), await precision(yt, yp, 1), "precision is label-agnostic");
    close(await recall(yt2, yp2, 0), await recall(yt, yp, 1), "recall is label-agnostic");
    close(await f1(yt2, yp2, 0), await f1(yt, yp, 1), "f1 is label-agnostic");
    close(await recall(yt, yp, 0), await specificity(yt, yp, 1),
          "recall of the negative class IS specificity of the positive one");
    const a = [7, 7, 7, -3, -3], b = [7, 7, -3, 7, -3];
    close(await precision(a, b, 7), await precision(yt, yp, 1), "works with arbitrary label values");
}

{
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
}

{
    const S = [0.9, 0.8, 0.4, 0.7, 0.3, 0.2, 0.1, 0.05, 0.6, 0.15];
    close(await averagePrecision(YT, S), (1 / 1 + 2 / 2 + 3 / 5) / 3,
          "averagePrecision is the step-function AP");
    close(await averagePrecision([0, 0, 1, 1], [0.1, 0.2, 0.8, 0.9]), 1,
          "perfectly separable AP is 1");
    assert((await averagePrecision(YT, S)) <= 1, "AP is bounded above by 1");
    assert((await averagePrecision(YT, S)) >= 3 / 10, "AP is bounded below by the base rate");
    await throwsRange(async () => await averagePrecision([1, 1], [0.1, 0.2]), "AP needs both classes");
}

{
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
}

{
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
}

{
    await throwsRange(async () => await rocAuc([0, 1], [NaN, 0.5]), "rocAuc NaN score refused");
    await throwsRange(async () => await rocAuc([NaN, 1], [0.1, 0.9]), "rocAuc NaN label refused");
    await throwsRange(async () => await averagePrecision([0, 1], [0.5, NaN]),
                "averagePrecision NaN score refused");
    close(await rocAuc(YT, [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.05]), 1.0,
          "rocAuc perfect ranking");
}

{
    await throwsRange(async () => await logLoss([0, 1], [NaN, 0.5]), "logLoss NaN refusal");
    await throwsRange(async () => await logLoss([NaN, 1], [[0.9, 0.1], [0.1, 0.9]]),
                "logLoss NaN label refusal");
    close(await rocAuc(YT, [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.05]), 1.0,
          "rocAuc finite path unchanged");
}

print("test_ml_metrics: all " + n + " tests passed");
