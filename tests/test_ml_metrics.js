/* test_ml_metrics.js -- the classification metrics beyond accuracy.
 *
 * accuracy is useless on imbalanced data: a classifier that always answers
 * "negative" scores 0.99 on a 1%-positive set. That degenerate classifier is
 * therefore a first-class test case here, not an afterthought -- every metric
 * is asserted to expose it.
 *
 * Expected values are derived from the confusion matrix by hand in the comments
 * (TP/FP/TN/FN are small and countable) rather than produced by running this
 * code, so a wrong implementation cannot ratify itself. Where a metric has a
 * closed-form identity -- F1 as the harmonic mean, MCC's symmetry, AUC's
 * rank-sum -- the identity is asserted too.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_ml_metrics.js */

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
function throwsRange(fn, m) {
    n++;
    try { fn(); } catch (e) {
        if (e instanceof RangeError) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected RangeError): " + m);
}

/* The worked example used throughout.
 *   yTrue  1 1 1 0 0 0 0 0 0 0     (3 positives, 7 negatives)
 *   yPred  1 1 0 1 0 0 0 0 0 0     (positive at 0, 1, 3)
 * so TP=2, FP=1, FN=1, TN=6.  */
const YT = [1, 1, 1, 0, 0, 0, 0, 0, 0, 0];
const YP = [1, 1, 0, 1, 0, 0, 0, 0, 0, 0];
const TP = 2, FP = 1, FN = 1, TN = 6;

/* ---- 1. the worked example, each value derived from TP/FP/TN/FN ---- */
{
    close(precision(YT, YP), TP / (TP + FP), "precision = TP/(TP+FP) = 2/3");
    close(recall(YT, YP), TP / (TP + FN), "recall = TP/(TP+FN) = 2/3");
    close(specificity(YT, YP), TN / (TN + FP), "specificity = TN/(TN+FP) = 6/7");
    close(f1(YT, YP), 2 * (2 / 3) * (2 / 3) / ((2 / 3) + (2 / 3)), "f1 = harmonic mean = 2/3");
    close(balancedAccuracy(YT, YP), 0.5 * (2 / 3 + 6 / 7), "balancedAccuracy = (rec+spec)/2");
    close(matthewsCorrcoef(YT, YP), (TP * TN - FP * FN) /
          Math.sqrt((TP + FP) * (TP + FN) * (TN + FP) * (TN + FN)), "MCC = 11/21");
    close(matthewsCorrcoef(YT, YP), 11 / 21, "MCC numerically");
    {   /* kappa: po = 8/10, pe = (3*3 + 7*7)/100 = 0.58 */
        const po = (TP + TN) / 10, pe = (3 * 3 + 7 * 7) / 100;
        close(cohenKappa(YT, YP), (po - pe) / (1 - pe), "cohenKappa vs chance agreement");
    }
    close(accuracy(YT, YP), 0.8, "accuracy for reference");
    /* the confusion matrix must agree with the counts the metrics used */
    {
        const cm = confusionMatrix(YT, YP);
        close(cm[1][1], TP, "confusionMatrix TP");
        close(cm[0][1], FP, "confusionMatrix FP");
        close(cm[1][0], FN, "confusionMatrix FN");
        close(cm[0][0], TN, "confusionMatrix TN");
    }
}

/* ---- 2. F1 is the harmonic mean, and fbeta generalises it ---- */
{
    const p = precision(YT, YP), r = recall(YT, YP);
    close(fbeta(YT, YP, 1), f1(YT, YP), "fbeta(beta=1) == f1");
    /* beta > 1 weights RECALL, beta < 1 weights PRECISION -- assert the
     * direction, which is the part people get backwards */
    const lo = fbeta(YT, YP, 0.5), hi = fbeta(YT, YP, 2);
    assert(Math.min(p, r) <= lo && lo <= Math.max(p, r), "f0.5 lies between p and r");
    assert(Math.min(p, r) <= hi && hi <= Math.max(p, r), "f2 lies between p and r");
    {   /* on a case where they differ, check the weighting direction */
        const yt = [1, 1, 1, 1, 0, 0, 0, 0];
        const yp = [1, 0, 0, 0, 0, 0, 0, 0];   /* precision 1.0, recall 0.25 */
        close(precision(yt, yp), 1.0, "high-precision case");
        close(recall(yt, yp), 0.25, "low-recall case");
        assert(fbeta(yt, yp, 2) < fbeta(yt, yp, 0.5),
               "beta=2 (recall-weighted) scores lower when recall is the weak side");
    }
    throwsRange(() => fbeta(YT, YP, 0), "fbeta beta must be positive");
    throwsRange(() => fbeta(YT, YP, -1), "fbeta rejects a negative beta");
}

/* ---- 3. the degenerate classifiers these metrics exist to expose ---- */
{
    /* always-negative on a 10% positive set: accuracy 0.9, everything else 0 */
    const yt = [1, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    const allNeg = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    close(accuracy(yt, allNeg), 0.9, "accuracy flatters the always-negative classifier");
    close(precision(yt, allNeg), 0, "precision exposes it (no predicted positives -> 0)");
    close(recall(yt, allNeg), 0, "recall exposes it");
    close(f1(yt, allNeg), 0, "f1 exposes it");
    close(matthewsCorrcoef(yt, allNeg), 0, "MCC exposes it");
    close(cohenKappa(yt, allNeg), 0, "kappa exposes it");
    close(balancedAccuracy(yt, allNeg), 0.5, "balancedAccuracy exposes it (chance)");

    /* always-positive */
    const allPos = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1];
    close(recall(yt, allPos), 1, "always-positive has perfect recall");
    close(precision(yt, allPos), 0.1, "...and precision equal to the base rate");
    close(specificity(yt, allPos), 0, "...and zero specificity");
    close(balancedAccuracy(yt, allPos), 0.5, "...and chance balanced accuracy");
}

/* ---- 4. perfect and inverted predictions ---- */
{
    const yt = [1, 1, 0, 0];
    close(precision(yt, yt), 1, "perfect precision");
    close(recall(yt, yt), 1, "perfect recall");
    close(f1(yt, yt), 1, "perfect f1");
    close(matthewsCorrcoef(yt, yt), 1, "perfect MCC is 1");
    close(cohenKappa(yt, yt), 1, "perfect kappa is 1");
    const inv = [0, 0, 1, 1];
    close(matthewsCorrcoef(yt, inv), -1, "fully inverted MCC is -1");
    close(cohenKappa(yt, inv), -1, "fully inverted kappa is -1");
    close(f1(yt, inv), 0, "fully inverted f1 is 0");
}

/* ---- 5. the `positive` label argument ---- */
{
    /* relabelling 0/1 as 1/0 and flipping `positive` must give identical
     * numbers -- the metric must not privilege the literal value 1 */
    const yt = [1, 1, 1, 0, 0], yp = [1, 1, 0, 1, 0];
    const yt2 = yt.map(v => 1 - v), yp2 = yp.map(v => 1 - v);
    close(precision(yt2, yp2, 0), precision(yt, yp, 1), "precision is label-agnostic");
    close(recall(yt2, yp2, 0), recall(yt, yp, 1), "recall is label-agnostic");
    close(f1(yt2, yp2, 0), f1(yt, yp, 1), "f1 is label-agnostic");
    /* precision and specificity swap when the positive class flips */
    close(recall(yt, yp, 0), specificity(yt, yp, 1),
          "recall of the negative class IS specificity of the positive one");
    /* arbitrary labels, not just 0/1 */
    const a = [7, 7, 7, -3, -3], b = [7, 7, -3, 7, -3];
    close(precision(a, b, 7), precision(yt, yp, 1), "works with arbitrary label values");
}

/* ---- 6. ROC AUC via the rank identity ---- */
{
    /* scores: positives 0.9, 0.8, 0.4 ; negatives 0.7, 0.3, 0.2, 0.1, 0.05, 0.6, 0.15
     * concordant pairs: 7 + 7 + 5 = 19 out of 3*7 = 21  */
    const S = [0.9, 0.8, 0.4, 0.7, 0.3, 0.2, 0.1, 0.05, 0.6, 0.15];
    close(rocAuc(YT, S), 19 / 21, "rocAuc equals the concordant-pair fraction");

    close(rocAuc([0, 0, 1, 1], [0.1, 0.2, 0.8, 0.9]), 1, "perfectly separable -> 1");
    close(rocAuc([0, 0, 1, 1], [0.9, 0.8, 0.2, 0.1]), 0, "perfectly inverted -> 0");
    /* a constant scorer must be exactly 0.5 -- this is what tie-averaged ranks
     * buy, and a naive threshold sweep gets it wrong */
    close(rocAuc([0, 0, 1, 1], [0.5, 0.5, 0.5, 0.5]), 0.5, "constant scorer -> exactly 0.5");
    close(rocAuc([0, 1], [0.5, 0.5]), 0.5, "single tied pair -> 0.5");
    /* partial ties */
    close(rocAuc([0, 0, 1, 1], [0.1, 0.5, 0.5, 0.9]), 0.875, "partial ties are averaged");
    /* AUC is invariant under any monotonic rescoring -- it depends only on rank */
    {
        const mono = S.map(v => Math.log(v + 1) * 3 + 7);
        close(rocAuc(YT, mono), rocAuc(YT, S), "AUC depends only on rank order");
    }
    /* reversing the scores gives 1 - AUC */
    close(rocAuc(YT, S.map(v => -v)), 1 - rocAuc(YT, S), "negating scores gives 1-AUC");
    /* undefined without both classes */
    throwsRange(() => rocAuc([1, 1, 1], [0.1, 0.2, 0.3]), "rocAuc needs a negative sample");
    throwsRange(() => rocAuc([0, 0, 0], [0.1, 0.2, 0.3]), "rocAuc needs a positive sample");
}

/* ---- 7. average precision ---- */
{
    /* sorted by descending score: P P N N P N N N N N
     * AP = (1/1 + 2/2 + 3/5) / 3 = 0.8666...  */
    const S = [0.9, 0.8, 0.4, 0.7, 0.3, 0.2, 0.1, 0.05, 0.6, 0.15];
    close(averagePrecision(YT, S), (1 / 1 + 2 / 2 + 3 / 5) / 3,
          "averagePrecision is the step-function AP");
    close(averagePrecision([0, 0, 1, 1], [0.1, 0.2, 0.8, 0.9]), 1,
          "perfectly separable AP is 1");
    /* a random-quality ranking cannot beat a perfect one */
    assert(averagePrecision(YT, S) <= 1, "AP is bounded above by 1");
    assert(averagePrecision(YT, S) >= 3 / 10, "AP is bounded below by the base rate");
    throwsRange(() => averagePrecision([1, 1], [0.1, 0.2]), "AP needs both classes");
}

/* ---- 8. input validation shared with the existing metrics ---- */
{
    n++;
    let threw = false;
    try { precision([1, 0], [1]); } catch (e) { threw = true; }
    if (!threw) throw new Error("mismatched lengths must throw");
    n++;
    threw = false;
    try { precision([], []); } catch (e) { threw = true; }
    if (!threw) throw new Error("empty input must throw");
    /* TypedArray input works like Array input */
    close(precision(new Float64Array(YT), new Float64Array(YP)), 2 / 3,
          "accepts Float64Array");
    /* the optional `positive` is coerced BEFORE the vectors are read, so a
     * throwing valueOf cannot strand the native buffers */
    n++;
    threw = false;
    try { precision(YT, YP, { valueOf() { throw new Error("boom"); } }); }
    catch (e) { threw = true; }
    if (!threw) throw new Error("a throwing positive-label coercion must propagate");
    /* and the module still works afterwards */
    close(precision(YT, YP), 2 / 3, "metrics still work after a failed coercion");
}

/* ==================================================================== *
 *  logLoss reads BOTH shapes a classifier can hand it
 *
 *  Every classifier's predictProba is rows x classes. logLoss used to take only
 *  a flat vector of P(class 1), so the composition it exists for --
 *  logLoss(y, model.predictProba(X)) -- returned NaN: each row coerced to a
 *  number and a two-element array is not one. NaN is the worst possible answer
 *  because it looks like a number until something divides by it.
 * ==================================================================== */
{
    /* Uniform predictions over K classes score exactly log(K) -- the closed
     * form, so this is an identity and not a regression baseline. */
    const yk = [0, 1, 2, 0, 1, 2];
    const uniform = yk.map(() => [1 / 3, 1 / 3, 1 / 3]);
    close(logLoss(yk, uniform), Math.log(3), "uniform over 3 classes is log 3");

    const uniform2 = [0, 1, 0, 1].map(() => [0.5, 0.5]);
    close(logLoss([0, 1, 0, 1], uniform2), Math.log(2), "uniform over 2 is log 2");

    /* A perfect prediction costs nothing; a confident wrong one is clipped to a
     * large finite penalty rather than an infinity that swallows the mean. */
    close(logLoss([0, 1], [[1, 0], [0, 1]]), 0, "a perfect prediction costs 0");
    n++;
    if (!Number.isFinite(logLoss([0, 1], [[0, 1], [1, 0]])))
        throw new Error("a confidently wrong prediction must stay finite");

    /* The two-column matrix and the P(class 1) column are the same metric, and
     * the binary path is the one that already existed -- so this pins that
     * adding the matrix form did not change what it computed. */
    const P = [[0.9, 0.1], [0.3, 0.7], [0.6, 0.4], [0.2, 0.8]];
    const yb = [0, 1, 0, 1];
    close(logLoss(yb, P), logLoss(yb, P.map(r => r[1])),
          "the matrix and column forms agree");

    /* A shape it cannot read is a TypeError, never a NaN. */
    n++;
    let threw2 = false;
    try { logLoss([0, 1, 2], [[0.5, 0.5], [0.5, 0.5], [0.5, 0.5]]); }
    catch (e) { threw2 = e instanceof TypeError; }
    if (!threw2) throw new Error("a column count that does not match the labels must throw");
}

print("test_ml_metrics: all " + n + " tests passed");
