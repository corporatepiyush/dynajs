// Parametric black-box contract test for dyna:ml, generated from dynajs.d.ts (pre-split slice). Engine sources not consulted.
//
// Style: every exported function/class gets a CASE TABLE of [label, inputs..., expected] rows;
// one loop per table; failures name the row label. All expectations are hand-computed from the
// contract only (arithmetic cited in row comments).
import {
    LinearRegression, LogisticRegression, KMeans, SVC, GaussianMixture, GaussianNB,
    DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier, RandomForestRegressor,
    GradientBoostingRegressor, GradientBoostingClassifier, XGBRegressor, XGBClassifier,
    PCA, KNClassifier, KNRegressor, DBScan, StandardScaler, MinMaxScaler, CSR, Pipeline,
    meanSquaredError, meanAbsoluteError, r2Score, accuracy, logLoss, confusionMatrix,
    precision, recall, f1, specificity, balancedAccuracy, matthewsCorrcoef, cohenKappa,
    fbeta, rocAuc, averagePrecision,
    trainTestSplit, kFold, stratifiedKFold, crossValScore, gridSearch, randomSearch,
    imputeMean, dropMissing,
} from "dyna:ml";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertClose(actual, expected, eps, msg) { n++; if (!(Math.abs(actual - expected) <= eps)) throw new Error("assertion failed (close): " + msg + " — got |" + actual + "| expected |" + expected + "±" + eps + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function deepClose(a, b, eps) {
    if (typeof a === "number" && typeof b === "number") return Math.abs(a - b) <= eps;
    if (!a || !b || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!deepClose(a[i], b[i], eps)) return false;
    return true;
}
function assertDeepClose(a, b, eps, msg) { n++; if (!deepClose(a, b, eps)) throw new Error("assertion failed (deepClose): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "±" + eps + "|"); }

/* ================================================================== *
 * S1. Metrics — exact hand-computed rows.
 * Canonical confusion for the binary rows:
 *   yTrue=[0,0,1,1], yPred=[0,1,1,1]  ->  CM = [[1,1],[0,2]]  (indexed [true][pred])
 *   TP(class1)=2, FN=0, FP=1, TN=1.
 * ================================================================== */
{
    // meanSquaredError = mean((t-p)^2)
    const T = [
        // [label, yTrue, yPred, expected]
        ["mse: exact predictions -> 0", [1, 2, 3], [1, 2, 3], 0],
        ["mse: (1^2+3^2)/2 = 5", [0, 0], [1, 3], 5],
        ["mse: each off by 1 -> (1+1+1+1)/4 = 1", [1, 2, 3, 4], [2, 3, 4, 5], 1],
    ];
    for (const [label, yt, yp, exp] of T) assertEq(meanSquaredError(yt, yp), exp, label);
}
{
    const T = [
        ["mae: (1+3)/2 = 2", [0, 0], [1, 3], 2],
        ["mae: zero", [4, 5], [4, 5], 0],
    ];
    for (const [label, yt, yp, exp] of T) assertEq(meanAbsoluteError(yt, yp), exp, label);
}
{
    const T = [
        // r2 = 1 - SSres/SStot
        ["r2: perfect -> 1", [1, 2, 3], [1, 2, 3], 1],
        ["r2: SSres=1 (0+0+1), SStot=2 (1+0+1) -> 0.5", [1, 2, 3], [1, 2, 4], 0.5],
        // doc-pinned: "a constant yTrue scores 1.0 for exact, else 0.0"
        ["r2: constant yTrue, exact -> 1 (doc-pinned)", [5, 5, 5], [5, 5, 5], 1],
        ["r2: constant yTrue, inexact -> 0 (doc-pinned)", [5, 5, 5], [5, 6, 5], 0],
    ];
    for (const [label, yt, yp, exp] of T) assertEq(r2Score(yt, yp), exp, label);
}
{
    const T = [
        ["accuracy: 3/4 correct -> 0.75", [1, 0, 1, 1], [1, 0, 0, 1], 0.75],
        ["accuracy: perfect -> 1", [7, 7], [7, 7], 1],
    ];
    for (const [label, yt, yp, exp] of T) assertEq(accuracy(yt, yp), exp, label);
}
{
    const rows = [
        // binary vector form: mean of -ln p(correct class). Both entries 0.8 -> -ln(0.8)
        ["logLoss: binary vector [0.8,0.2] -> -ln(0.8)", [1, 0], [0.8, 0.2], -Math.log(0.8)],
        ["logLoss: perfect binary -> 0 (ln 1 = 0)", [1, 0], [1, 0], 0],
        // class-matrix form: rows give class probabilities.
        // row0 true 0 -> p=0.7; row1 true 1 -> p=0.8; row2 true 2 -> p=0.5
        // mean(-ln0.7, -ln0.8, -ln0.5)
        ["logLoss: class matrix -> mean(-ln p_true)", [0, 1, 2],
            [[0.7, 0.2, 0.1], [0.1, 0.8, 0.1], [0.2, 0.3, 0.5]],
            (-Math.log(0.7) - Math.log(0.8) - Math.log(0.5)) / 3],
    ];
    for (const [label, yt, yp, exp] of rows) assertClose(logLoss(yt, yp), exp, 1e-12, label);
}
{
    // CM = [[1,1],[0,2]]
    const T = [
        ["confusion: binary [[1,1],[0,2]]", [0, 0, 1, 1], [0, 1, 1, 1], [[1, 1], [0, 2]]],
        // 3 classes: true0->{0,1}, true1->{1,2}, true2->{2,2,2}
        ["confusion: 3-class [[1,1,0],[0,1,1],[0,0,3]]", [0, 0, 1, 1, 2, 2, 2], [0, 1, 1, 2, 2, 2, 2], [[1, 1, 0], [0, 1, 1], [0, 0, 3]]],
    ];
    for (const [label, yt, yp, exp] of T) assertDeepEq(confusionMatrix(yt, yp), exp, label);
}
{
    const T = [
        // positive=1 (default): TP/(TP+FP) = 2/3
        ["precision: binary pos=1 -> 2/3", [0, 0, 1, 1], [0, 1, 1, 1], 2 / 3],
        // from 3-class CM [[1,1,0],[0,1,1],[0,0,3]]: class2: TP=3, FP=1 (the true-1/pred-2 cell) -> 3/4
        ["precision: 3-class positive=2 -> 3/4", [0, 0, 1, 1, 2, 2, 2], [0, 1, 1, 2, 2, 2, 2], 3 / 4],
    ];
    for (const [label, yt, yp, exp] of T) {
        const pos = label.includes("3-class") ? 2 : undefined;
        assertEq(pos === undefined ? precision(yt, yp) : precision(yt, yp, pos), exp, label);
    }
}
{
    const T = [
        ["recall: binary pos=1 -> TP/(TP+FN) = 2/2 = 1", [0, 0, 1, 1], [0, 1, 1, 1], 1],
        ["recall: 3-class positive=2 -> 3/3 = 1", [0, 0, 1, 1, 2, 2, 2], [0, 1, 1, 2, 2, 2, 2], 1],
    ];
    for (const [label, yt, yp, exp] of T) {
        const pos = label.includes("3-class") ? 2 : undefined;
        assertEq(pos === undefined ? recall(yt, yp) : recall(yt, yp, pos), exp, label);
    }
}
{
    // f1 = 2PR/(P+R) with P=2/3, R=1 -> (4/3)/(5/3) = 4/5
    assertClose(f1([0, 0, 1, 1], [0, 1, 1, 1]), 4 / 5, 1e-12, "f1: binary -> 0.8");
    // beta=1 must coincide with f1 exactly (same formula).
    assertEq(fbeta([0, 0, 1, 1], [0, 1, 1, 1], 1), f1([0, 0, 1, 1], [0, 1, 1, 1]), "fbeta(beta=1) === f1");
}
{
    // specificity pos=1: TN/(TN+FP) = 1/2
    assertEq(specificity([0, 0, 1, 1], [0, 1, 1, 1]), 0.5, "specificity: TN/(TN+FP) = 1/2");
}
{
    // balancedAccuracy pos=1: (recall_1 + recall_0)/2 = (1 + 1/2)/2 = 0.75
    assertEq(balancedAccuracy([0, 0, 1, 1], [0, 1, 1, 1]), 0.75, "balancedAccuracy: (1 + 0.5)/2 = 0.75");
}
{
    // CM [[1,1],[0,2]] -> TP=2, TN=1, FP=1, FN=0:
    // MCC = (TP*TN - FP*FN)/sqrt((TP+FP)(TP+FN)(TN+FP)(TN+FN)) = 2/sqrt(3*2*2*1) = 1/sqrt(3)
    assertClose(matthewsCorrcoef([0, 0, 1, 1], [0, 1, 1, 1]), 2 / Math.sqrt(12), 1e-12, "matthews: 2/sqrt(12) = 1/sqrt(3)");
}
{
    // kappa: po = 3/4; pe = (rowSums dot colSums)/n^2 = (2*1 + 2*3)/16 = 0.5; (0.75-0.5)/(1-0.5) = 0.5
    assertEq(cohenKappa([0, 0, 1, 1], [0, 1, 1, 1]), 0.5, "cohenKappa: (0.75-0.5)/(0.5) = 0.5");
}
{
    const yt = [0, 0, 1, 1], yp = [0, 1, 1, 1]; // P=2/3, R=1
    const rows = [
        // f2 = (1+4)*P*R/(4P+R) = (10/3)/(8/3+1) = (10/3)/(11/3) = 10/11  (beta>1 weights recall -> f2 > f1 = 0.8)
        ["fbeta: beta=2 -> 10/11", 2, 10 / 11],
        // f0.5 = (1+0.25)PR/(0.25P+R) = (5/6)/(1/6+1) = 5/7  (beta<1 weights precision -> f0.5 < f1)
        ["fbeta: beta=0.5 -> 5/7", 0.5, 5 / 7],
    ];
    for (const [label, beta, exp] of rows) assertClose(fbeta(yt, yp, beta), exp, 1e-12, label);
}
{
    const T = [
        // U = #(pos>neg) pairs / (npos*nneg), 0.5*ties
        ["rocAuc: perfectly separated -> 1", [0, 1], [0.1, 0.9], 1],
        ["rocAuc: inverted -> 0", [0, 1], [0.9, 0.1], 0],
        ["rocAuc: full tie -> 0.5", [0, 1], [0.5, 0.5], 0.5],
        // pos scores {0.35, 0.8}, neg {0.1, 0.4}: wins (0.35>0.1),(0.8>0.1),(0.8>0.4) = 3 of 4 -> 0.75
        ["rocAuc: mixed -> 3/4", [0, 0, 1, 1], [0.1, 0.4, 0.35, 0.8], 0.75],
    ];
    for (const [label, yt, sc, exp] of T) assertEq(rocAuc(yt, sc), exp, label);
    // doc-pinned refusal: "needs both a positive and a negative sample"
    assertThrows(() => rocAuc([1, 1], [0.4, 0.9]), "rocAuc: single-class yTrue refused");
}
{
    const rows = [
        // AP = sum (R_n - R_{n-1}) * P_n over positive ranks, score-descending
        ["averagePrecision: separated -> 1", [0, 1], [0.1, 0.9], 1],
        // ranks: .9(pos)->P1 R.5 -> +0.5; .8(neg); .1(pos)->P=2/3 R1 -> +(2/3)(1/2)=1/3; total 5/6
        ["averagePrecision: mixed -> 5/6", [1, 0, 1], [0.9, 0.8, 0.1], 5 / 6],
        ["averagePrecision: top-ranked positive -> 1", [1, 0, 0], [0.9, 0.8, 0.1], 1],
    ];
    for (const [label, yt, sc, exp] of rows) assertClose(averagePrecision(yt, sc), exp, 1e-12, label);
}

/* ================================================================== *
 * S2. Splits — index sets only; exact random assignments never pinned.
 * ================================================================== */
{
    // testSize 0.2 * 10 = 2 (exact in f64: 0.2*10 rounds to 2.000...004, floor/round both give 2)
    const s = trainTestSplit(10, { testSize: 0.2, shuffle: false });
    assert(s.test.length === 2 && s.train.length === 8, "trainTestSplit: n=10 testSize=0.2 -> sizes 8/2");
    const all = s.train.concat(s.test).sort((a, b) => a - b);
    assertDeepEq(all, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9], "trainTestSplit: indices are a complete disjoint cover");
    assert(s.train.every(i => !s.test.includes(i)), "trainTestSplit: train/test disjoint");
}
{
    const a = trainTestSplit(10, { testSize: 0.2, shuffle: true, seed: 42 });
    const b = trainTestSplit(10, { testSize: 0.2, shuffle: true, seed: 42 });
    assert(a.test.length === 2 && a.train.length === 8, "trainTestSplit: shuffle+seed keeps sizes 8/2");
    assert(eqArr(a.train, b.train) && eqArr(a.test, b.test), "trainTestSplit: same seed -> identical split (doc determinism)");
    // NOTE: different-seed-must-differ is deliberately NOT asserted: the contract
    // pins only same-seed reproducibility, and two seeds may legally collide.
}
{
    // "Indices, not data": a Target input still yields positions 0..n-1. 4 * 0.5 = 2 exact.
    const s = trainTestSplit([50, 60, 70, 80], { testSize: 0.5, shuffle: false });
    assert(s.test.length === 2 && s.train.length === 2, "trainTestSplit: Target input -> index counts 2/2");
    const all = s.train.concat(s.test).sort((a, b) => a - b);
    assertDeepEq(all, [0, 1, 2, 3], "trainTestSplit: Target input -> positions 0..3");
}
{
    const folds = kFold(6, { k: 3 });
    assert(folds.length === 3, "kFold: k=3 -> 3 folds");
    assert(folds.every(f => f.test.length === 2 && f.train.length === 4), "kFold: n=6,k=3 -> test 2 / train 4 per fold");
    const seen = folds.flatMap(f => f.test).sort((a, b) => a - b);
    assertDeepEq(seen, [0, 1, 2, 3, 4, 5], "kFold: test folds partition 0..5 exactly once");
    const folds2 = kFold(6, { k: 3 });
    assert(folds.every((f, i) => eqArr(f.test, folds2[i].test)), "kFold: deterministic without shuffle");
}
{
    // doc-pinned budget: "kFold refuses k * n above 2e7"
    assertThrows(() => kFold(10000001, { k: 2 }), "kFold: k*n = 20000002 > 2e7 refused");
}
{
    const y = [0, 0, 1, 1];
    const folds = stratifiedKFold(y, { k: 2 });
    assert(folds.length === 2, "stratifiedKFold: k=2 -> 2 folds");
    for (let i = 0; i < folds.length; i++) {
        const cls = folds[i].test.map(j => y[j]).sort((a, b) => a - b);
        assertDeepEq(cls, [0, 1], "stratifiedKFold: fold " + i + " test keeps 1 of each class (test size 2)");
        assert(folds[i].train.length === 2, "stratifiedKFold: fold " + i + " train size 2");
    }
    const seen = folds.flatMap(f => f.test).sort((a, b) => a - b);
    assertDeepEq(seen, [0, 1, 2, 3], "stratifiedKFold: tests partition 0..3");
    const again = stratifiedKFold(y, { k: 2 });
    assert(folds.every((f, i) => eqArr(f.test, again[i].test)), "stratifiedKFold: deterministic without shuffle");
}

/* ================================================================== *
 * S3. crossValScore / gridSearch / randomSearch on an exact line.
 * X=[[0],[1],[2],[3]], y=[1,3,5,7] (y = 2x + 1): any 2-point fold fits
 * the line by OLS; scoring accepts a tight 1e-9 tolerance (the contract
 * says "closed-form OLS", not bit-exact predictions).
 * ================================================================== */
{
    const X = [[0], [1], [2], [3]], y = [1, 3, 5, 7];
    const exact = (yt, yp) => (yt.every((v, i) => Math.abs(v - yp[i]) < 1e-9) ? 1 : 0);
    const progress = [];
    const scores = crossValScore(() => new LinearRegression(), X, y, {
        k: 2, scoring: exact,
        onProgress: p => progress.push({ done: p.done, total: p.total }),
    });
    assert(Array.isArray(scores) && scores.length === 2, "crossValScore: returns one score per fold (2)");
    assert(scores.every(s => s === 1), "crossValScore: exact-line folds all score 1");
    assert(progress.length === 2 && progress[1].done === 2 && progress[1].total === 2,
        "crossValScore: onProgress fires after each fold, ends at done=total=2");
    // doc-pinned nJobs semantics: ToInt64 (2.5 truncates to 2), still runs sequentially
    const scores2 = crossValScore(() => new LinearRegression(), X, y, { k: 2, scoring: exact, nJobs: 2.5 });
    assert(scores2.length === 2 && scores2.every(s => s === 1), "crossValScore: nJobs=2.5 accepted (ToInt64), results unchanged");
}
{
    const X = [[0], [1], [2], [3]], y = [1, 3, 5, 7];
    const exact = (yt, yp) => (yt.every((v, i) => Math.abs(v - yp[i]) < 1e-9) ? 1 : 0);
    const r = gridSearch(params => new LinearRegression(), X, y, { alpha: [1, 2] }, { k: 2, scoring: exact });
    assert(r.results.length === 2, "gridSearch: 2 grid points -> 2 results");
    assert(r.results.every(res => res.scores.length === 2 && res.mean === 1), "gridSearch: each result has 2 fold scores, mean 1");
    assert(r.bestScore === 1, "gridSearch: bestScore 1 (every combo is exact on the line)");
    assert(r.best.alpha === 1 || r.best.alpha === 2, "gridSearch: best params drawn from the grid");
}
{
    const X = [[0], [1], [2], [3]], y = [1, 3, 5, 7];
    const exact = (yt, yp) => (yt.every((v, i) => Math.abs(v - yp[i]) < 1e-9) ? 1 : 0);
    const a = randomSearch(params => new LinearRegression(), X, y, { alpha: [0, 1, 2, 3] }, { nIter: 2, k: 2, scoring: exact, seed: 11 });
    const b = randomSearch(params => new LinearRegression(), X, y, { alpha: [0, 1, 2, 3] }, { nIter: 2, k: 2, scoring: exact, seed: 11 });
    assert(a.results.length === 2, "randomSearch: nIter=2 -> 2 sampled results");
    assert(a.bestScore === 1, "randomSearch: bestScore 1 on exact-line data");
    assert(a.best.alpha === b.best.alpha, "randomSearch: same seed -> same sampled best (doc determinism)");
}

/* ================================================================== *
 * S4. LinearRegression — closed-form OLS.
 * X=[[0],[1],[2]], y=[1,3,5]: mean x=1, mean y=3; Sxy=4, Sxx=2 ->
 * slope 2, intercept 3-2*1 = 1 (all sums exact in f64).
 * predict([[3],[4]]) = [7,9].
 * ================================================================== */
{
    const m = new LinearRegression();
    assert(m.fit([[0], [1], [2]], [1, 3, 5]) === m, "LinearRegression: fit returns this");
    assertDeepEq(m.coef, [2], "LinearRegression: exact-line slope = 2");
    assertEq(m.intercept, 1, "LinearRegression: intercept = 1");
    assert(Array.isArray(m.coef) && m.coef.length === 1 && typeof m.coef[0] === "number", "LinearRegression: coef is number[] of length cols");
    const p = m.predict([[3], [4]]);
    assert(Array.isArray(p) && eqArr(p, [7, 9]), "LinearRegression: predict default -> number[] [7,9]");
    const pf = m.predict([[3], [4]], undefined, undefined, { as: "f64" });
    assert(pf instanceof Float64Array && eqArr(pf, [7, 9]), "LinearRegression: predict {as:'f64'} -> Float64Array [7,9]");
    const out = new Float64Array(2);
    assertEq(m.predictInto(out, [[3], [4]]), 2, "LinearRegression: predictInto returns rows written (2)");
    assert(eqArr(out, [7, 9]), "LinearRegression: predictInto writes [7,9]");
    const short = new Float64Array(1);
    assertThrows(() => m.predictInto(short, [[3], [4]]), "LinearRegression: predictInto short out -> RangeError (doc)", RangeError);
    assertEq(short[0], 0, "LinearRegression: predictInto short out writes NOTHING");
    // determinism: closed form, no randomness
    const m2 = new LinearRegression().fit([[0], [1], [2]], [1, 3, 5]);
    assert(eqArr(m2.coef, m.coef) && m2.intercept === m.intercept, "LinearRegression: refit is bit-identical (closed form)");
}
{
    // flat Float64Array X form of the same matrix
    const m = new LinearRegression().fit(new Float64Array([0, 1, 2]), [1, 3, 5], 3, 1);
    assertDeepEq(m.coef, [2], "LinearRegression: flat-X fit -> slope 2");
    assert(eqArr(m.predict(new Float64Array([3, 4]), 2, 1), [7, 9]), "LinearRegression: flat-X predict -> [7,9]");
}
{
    // doc: CSR fit is BIT-IDENTICAL to the dense fit; CSR predict is BIT-IDENTICAL to dense predict
    const cfit = new LinearRegression().fit(CSR.fromDense([[0], [1], [2]]), [1, 3, 5]);
    assertDeepEq(cfit.coef, [2], "LinearRegression: CSR fit bit-identical -> slope 2");
    const m = new LinearRegression().fit([[0], [1], [2]], [1, 3, 5]);
    const pd = m.predict([[3], [4]]);
    const ps = m.predict(CSR.fromDense([[3], [4]]), 2, 1);
    assert(eqArr(pd, ps) && eqArr(ps, [7, 9]), "LinearRegression: CSR predict bit-identical to dense [7,9]");
}
{
    const m = new LinearRegression().fit([[0], [1], [2]], [1, 3, 5]);
    const bytes = m.serialize();
    assert(bytes instanceof Uint8Array && bytes.length > 0, "LinearRegression: serialize -> nonempty Uint8Array");
    const r1 = LinearRegression.deserialize(bytes);
    assert(eqArr(r1.predict([[3], [4]]), [7, 9]), "LinearRegression: deserialize(bytes) round-trips predictions");
    const r2 = LinearRegression.deserialize(bytes.slice().buffer);
    assert(eqArr(r2.predict([[3], [4]]), [7, 9]), "LinearRegression: deserialize(ArrayBuffer) round-trips predictions");
    m.close();
    assert(m.closed === true, "LinearRegression: close -> closed === true");
}

/* ================================================================== *
 * S5. LogisticRegression — full-batch GD.
 * Separable 1D: X=[[0],[1],[2],[3]], y=[0,0,1,1]; boundary between 1
 * and 2 -> training rows classify exactly (discrete outputs).
 * ================================================================== */
{
    const X = [[0], [1], [2], [3]], y = [0, 0, 1, 1];
    const m = new LogisticRegression({ learningRate: 0.5, maxIter: 3000, tol: 1e-10 }).fit(X, y);
    assert(eqArr(m.predict(X), [0, 0, 1, 1]), "LogisticRegression: separable training rows classified exactly");
    assertDeepEq(m.classes, [0, 1], "LogisticRegression: classes [0,1]");
    assert(Array.isArray(m.coef) && Array.isArray(m.coef[0]) && m.coef[0][0] > 0,
        "LogisticRegression: coef is number[][], positive weight (P(y=1) grows with x)");
    assert(typeof m.nIter === "number" && m.nIter >= 1, "LogisticRegression: nIter >= 1");
    assert(typeof m.converged === "boolean", "LogisticRegression: converged is boolean");
    assert(typeof m.intercept === "number" || Array.isArray(m.intercept), "LogisticRegression: intercept is number | number[]");
    const proba = m.predictProba(X);
    assert(proba.length === 4 && proba.every(r => r.length === 2), "LogisticRegression: predictProba -> 4x2");
    assert(proba.every(r => Math.abs(r[0] + r[1] - 1) <= 1e-9), "LogisticRegression: predictProba rows sum to 1");
    const pred = m.predict(X);
    assert(proba.every((r, i) => (r[1] > r[0]) === (pred[i] === 1)), "LogisticRegression: predictProba argmax === predict");
    const m2 = new LogisticRegression({ learningRate: 0.5, maxIter: 3000, tol: 1e-10 }).fit(X, y);
    assert(m2.coef.flat()[0] === m.coef.flat()[0] && m2.intercept === m.intercept,
        "LogisticRegression: deterministic full-batch GD refit");
    const r = LogisticRegression.deserialize(m.serialize());
    assert(eqArr(r.predict([[0], [3]]), [0, 1]), "LogisticRegression: serialize round-trip predictions");
    m.close();
    assert(m.closed === true, "LogisticRegression: close -> closed === true");
}
{
    // multiclass: 3 separated clusters in 1D
    const X = [[0], [1], [10], [11], [20], [21]], y = [0, 0, 1, 1, 2, 2];
    const m = new LogisticRegression({ learningRate: 0.5, maxIter: 5000, tol: 1e-10 }).fit(X, y);
    assert(eqArr(m.predict(X), y), "LogisticRegression: 3-class separable training rows classified exactly");
    assertDeepEq(m.classes, [0, 1, 2], "LogisticRegression: 3 classes");
    const proba = m.predictProba(X);
    assert(proba.length === 6 && proba.every(r => r.length === 3), "LogisticRegression: multiclass proba 6x3");
    assert(proba.every(r => Math.abs(r[0] + r[1] + r[2] - 1) <= 1e-9), "LogisticRegression: multiclass proba rows sum to 1");
}

/* ================================================================== *
 * S6. KMeans — Lloyd + k-means++ (documented seed determinism; the
 * default seed is NOT documented, so only seeded fits are pinned).
 * X=[[0],[1],[100],[101]]: separation 100 makes the correct 2-clustering
 * the only attractor (any same-cluster seeding is corrected by Lloyd);
 * converged centroids 0.5 / 100.5 -> inertia = 4 * 0.25 = 1.
 * ================================================================== */
{
    const X = [[0], [1], [100], [101]];
    const k = new KMeans(2, 7).fit(X);
    const lab = k.predict(X);
    assert(lab.length === 4, "KMeans: predict -> one label per row");
    assert(lab[0] === lab[1] && lab[2] === lab[3], "KMeans: tight clusters share a label");
    assert(lab[0] !== lab[2], "KMeans: separated clusters get different labels");
    assertClose(k.inertia, 1.0, 1e-9, "KMeans: inertia = 4*(0.5^2) = 1 (centroids 0.5/100.5)");
    const one = k.predict([[50]]);
    assert((one[0] === lab[0] || one[0] === lab[2]) && typeof one[0] === "number",
        "KMeans: midpoint gets a valid cluster id");
    const out = new Float64Array(4);
    assertEq(k.predictInto(out, X), 4, "KMeans: predictInto returns 4 rows written");
    assert(eqArr(out, lab), "KMeans: predictInto writes the same labels");
    const k2 = new KMeans(2, 7).fit(X);
    assert(eqArr(k2.predict(X), lab), "KMeans: same seed -> identical labels (doc determinism)");
    const kf = new KMeans(2, 7).fit(new Float64Array([0, 1, 100, 101]), 4, 1);
    assert(eqArr(kf.predict(X), lab), "KMeans: flat Float64Array fit -> same clustering");
    const ks = k.predict(CSR.fromDense(X), 4, 1);
    assert(eqArr(ks, lab), "KMeans: CSR predict bit-identical to dense labels");
    const r = KMeans.deserialize(k.serialize());
    assert(eqArr(r.predict([[0], [100]]), [lab[0], lab[2]]), "KMeans: serialize round-trip labels");
    k.close();
    assert(k.closed === true, "KMeans: close -> closed === true");
}

/* ================================================================== *
 * S7. SVC — SMO. Linearly separable 1D, margin between 1 and 2 ->
 * training rows classified exactly; a separable problem has >= 1
 * support vector per class -> nSupportVectors >= 2.
 * ================================================================== */
{
    const X = [[0], [1], [2], [3]], y = [0, 0, 1, 1];
    const m = new SVC({ kernel: "linear", C: 1000 }).fit(X, y);
    assert(eqArr(m.predict(X), [0, 0, 1, 1]), "SVC: linear separable training rows classified exactly");
    assert(typeof m.nSupportVectors === "number" && m.nSupportVectors >= 2, "SVC: nSupportVectors >= 2 (one per class)");
    assertDeepEq(m.classes, [0, 1], "SVC: classes [0,1]");
    const d = m.decisionFunction(X);
    assert(Array.isArray(d) && !Array.isArray(d[0]) && d.length === 4, "SVC: binary decisionFunction -> number[] of n rows");
    const pred = m.predict(X);
    assert(d.every((v, i) => (v > 0) === (pred[i] === 1)), "SVC: predict === sign(decisionFunction)");
    const m2 = new SVC({ kernel: "linear", C: 1000 }).fit(X, y);
    assertEq(m2.nSupportVectors, m.nSupportVectors, "SVC: deterministic refit (same SV count)");
    const r = SVC.deserialize(m.serialize());
    assert(eqArr(r.predict([[0], [3]]), [0, 1]), "SVC: serialize round-trip predictions");
    const ps = m.predict(CSR.fromDense(X), 4, 1);
    assert(eqArr(ps, pred), "SVC: CSR predict bit-identical to dense");
    m.close();
    assert(m.closed === true, "SVC: close -> closed === true");
}
{
    // other documented kernels on the same separable data
    const X = [[0], [1], [2], [3]], y = [0, 0, 1, 1];
    const rbf = new SVC({ kernel: "rbf", gamma: 10, C: 1000 }).fit(X, y);
    assert(eqArr(rbf.predict(X), [0, 0, 1, 1]), "SVC: rbf kernel separates training rows");
    const poly = new SVC({ kernel: "poly", degree: 2, coef0: 1, C: 1000 }).fit(X, y);
    assert(eqArr(poly.predict(X), [0, 0, 1, 1]), "SVC: poly kernel separates training rows (linear term present via coef0)");
}

/* ================================================================== *
 * S8. GaussianMixture — EM, diagonal Gaussians.
 * X=[[0],[1],[100],[101]], k=2: separation 100 pins the grouping;
 * converged means lie inside their clusters; weights sum to 1.
 * ================================================================== */
{
    const X = [[0], [1], [100], [101]];
    const g = new GaussianMixture(2, { seed: 3, maxIter: 200 }).fit(X);
    const lab = g.predict(X);
    assert(lab.length === 4 && lab[0] === lab[1] && lab[2] === lab[3] && lab[0] !== lab[2],
        "GaussianMixture: tight separated clusters grouped consistently");
    assert(Array.isArray(g.means) && g.means.length === 2 && g.means.every(mn => mn.length === 1),
        "GaussianMixture: means is k x cols (2x1)");
    const lo = g.means[lab[0]][0], hi = g.means[lab[2]][0];
    assert(lo >= 0 && lo <= 1 && hi >= 100 && hi <= 101, "GaussianMixture: means fall inside their clusters");
    assert(g.variances.length === 2 && g.variances.every(v => v.length === 1 && v[0] > 0),
        "GaussianMixture: variances k x cols, positive");
    assert(g.weights.length === 2 && Math.abs(g.weights[0] + g.weights[1] - 1) <= 1e-9,
        "GaussianMixture: weights length k, sum to 1");
    const proba = g.predictProba(X);
    assert(proba.length === 4 && proba.every(r => r.length === 2 && Math.abs(r[0] + r[1] - 1) <= 1e-9),
        "GaussianMixture: predictProba rows sum to 1");
    assert(proba.every((r, i) => (r[1] > r[0]) === (lab[i] === 1)), "GaussianMixture: predictProba argmax === predict");
    assert(typeof g.logLikelihood === "number" && Number.isFinite(g.logLikelihood), "GaussianMixture: logLikelihood finite number");
    assert(typeof g.nIter === "number" && g.nIter >= 1, "GaussianMixture: nIter >= 1");
    const g2 = new GaussianMixture(2, { seed: 3, maxIter: 200 }).fit(X);
    assert(eqArr(g2.means.flat(), g.means.flat()), "GaussianMixture: same seed -> identical means (doc determinism)");
    const r = GaussianMixture.deserialize(g.serialize());
    assert(eqArr(r.predict([[0], [100]]), [lab[0], lab[2]]), "GaussianMixture: serialize round-trip predictions");
    g.close();
    assert(g.closed === true, "GaussianMixture: close -> closed === true");
}

/* ================================================================== *
 * S9. GaussianNB — per-class Gaussian densities.
 * class 0 rows {1,2}: mean 1.5, population variance 0.25;
 * class 1 rows {10,12}: mean 11, variance 1. Equal priors.
 *   predict(1.5): class0 density 1/(0.5*sqrt(2pi)) ~ 0.8, class1 ~ e^-45 -> 0
 *   predict(11): symmetric -> 1
 *   predict(2): class0 ~ e^-0.125, class1 ~ e^-40.5 -> 0
 *   predict(5): class0 quadratic term (3.5)^2/0.5 = 24.5 vs class1
 *     (6)^2/2 = 18; class1's extra log-det penalty is only
 *     0.5*ln(1/0.25) ~ 0.69 < 6.5 -> 1
 * ================================================================== */
{
    const X = [[1], [2], [10], [12]], y = [0, 0, 1, 1];
    const m = new GaussianNB().fit(X, y);
    assertDeepEq(m.classes, [0, 1], "GaussianNB: classes [0,1]");
    const T = [
        ["x=1.5 -> 0 (own class mean)", [[1.5]], 0],
        ["x=11 -> 1 (own class mean)", [[11]], 1],
        ["x=2 -> 0 (training row)", [[2]], 0],
        ["x=10 -> 1 (training row)", [[10]], 1],
        ["x=5 -> 1 (quadratic term 24.5 vs 18 dominates the 0.69 log-det gap)", [[5]], 1],
    ];
    for (const [label, probe, exp] of T) assertEq(m.predict(probe)[0], exp, "GaussianNB: " + label);
    const proba = m.predictProba([[1.5], [11]]);
    assert(proba.length === 2 && proba.every(r => r.length === 2 && Math.abs(r[0] + r[1] - 1) <= 1e-9),
        "GaussianNB: predictProba rows sum to 1");
    assert(proba[0][0] > proba[0][1] && proba[1][1] > proba[1][0], "GaussianNB: predictProba argmax matches densities");
    const ps = m.predict(CSR.fromDense(X), 4, 1);
    assert(eqArr(ps, [0, 0, 1, 1]), "GaussianNB: CSR predict bit-identical to dense");
    const m2 = new GaussianNB().fit(X, y);
    assert(eqArr(m2.predict(X), m.predict(X)), "GaussianNB: refit deterministic (closed-form moments)");
    const r = GaussianNB.deserialize(m.serialize());
    assert(eqArr(r.predict([[5]]), [1]), "GaussianNB: serialize round-trip predictions");
    m.close();
    assert(m.closed === true, "GaussianNB: close -> closed === true");
}

/* ================================================================== *
 * S10. DecisionTreeClassifier — CART/Gini.
 * X=[[0,0],[0,0],[10,10],[11,11]], y=[0,0,1,1]: separable in feature 0.
 * depth-1 fit splits between 0 and 10 -> pure leaves, depth exactly 1.
 * Doc refusals: maxDepth above 1024 REFUSED at construction;
 * maxDepth 0/absent = 1024 floor.
 * ================================================================== */
{
    const X = [[0, 0], [0, 0], [10, 10], [11, 11]], y = [0, 0, 1, 1];
    const m = new DecisionTreeClassifier().fit(X, y);
    assert(eqArr(m.predict(X), [0, 0, 1, 1]), "DecisionTreeClassifier: separable training rows classified exactly");
    const proba = m.predictProba(X);
    assert(proba.length === 4 && proba.every(r => r.length === 2), "DecisionTreeClassifier: predictProba 4x2");
    assertEq(proba[0][0], 1, "DecisionTreeClassifier: pure leaf proba[0] = 1 exactly");
    assertEq(proba[0][1], 0, "DecisionTreeClassifier: pure leaf proba[1] = 0 exactly");
    const flat = m.apply(X).flat();
    assert(flat.length === 4, "DecisionTreeClassifier: apply -> one leaf id per row");
    assertEq(flat[0], flat[1], "DecisionTreeClassifier: duplicate rows land in the same leaf");
    assert(flat[0] !== flat[flat.length - 1], "DecisionTreeClassifier: rows of different classes land in different leaves");
    const imp = m.featureImportances;
    assert(imp.length === 2 && imp[0] > 0 && imp[1] >= 0, "DecisionTreeClassifier: importances length cols; used feature > 0");
    assert(typeof m.depth === "number" && m.depth >= 1, "DecisionTreeClassifier: depth >= 1");
    const m2 = new DecisionTreeClassifier().fit(X, y);
    assert(eqArr(m2.predict(X), m.predict(X)) && eqArr(m2.featureImportances, m.featureImportances),
        "DecisionTreeClassifier: deterministic refit (exact splitter, no seed)");
    const r = DecisionTreeClassifier.deserialize(m.serialize());
    assert(eqArr(r.predict(X), [0, 0, 1, 1]), "DecisionTreeClassifier: serialize round-trip predictions");
    m.close();
    assert(m.closed === true, "DecisionTreeClassifier: close -> closed === true");
}
{
    const X = [[0], [1], [10], [11]], y = [0, 0, 1, 1];
    const d1 = new DecisionTreeClassifier({ maxDepth: 1 }).fit(X, y);
    assert(eqArr(d1.predict(X), [0, 0, 1, 1]), "DecisionTreeClassifier: maxDepth=1 one split separates the classes");
    assertEq(d1.depth, 1, "DecisionTreeClassifier: maxDepth=1 -> depth === 1");
    // doc: "0, or absent, means the 1024 hard floor"
    const d0 = new DecisionTreeClassifier({ maxDepth: 0 }).fit(X, y);
    assert(eqArr(d0.predict(X), [0, 0, 1, 1]), "DecisionTreeClassifier: maxDepth=0 acts as the 1024 floor (fits fine)");
    // doc: a value above 1024 is REFUSED at construction
    assertThrows(() => new DecisionTreeClassifier({ maxDepth: 1025 }), "DecisionTreeClassifier: maxDepth=1025 refused (doc)");
    assertThrows(() => new RandomForestClassifier({ nEstimators: 100001 }), "TreeOpts: nEstimators above 100000 refused at construction (doc)");
}

/* ================================================================== *
 * S11. DecisionTreeRegressor — variance-minimising splits.
 * X=[[0],[1],[10],[11]], y=[0,1,10,11]: best depth-1 split is at 5.5
 * (residual variance 0.25+0.25 is minimal), leaves average to exactly
 * 0.5 / 10.5 -> predictions [0.5,0.5,10.5,10.5] exact in f64.
 * Unrestricted depth fits the 4 training rows exactly.
 * ================================================================== */
{
    const X = [[0], [1], [10], [11]], y = [0, 1, 10, 11];
    const d1 = new DecisionTreeRegressor({ maxDepth: 1 }).fit(X, y);
    assert(eqArr(d1.predict(X), [0.5, 0.5, 10.5, 10.5]), "DecisionTreeRegressor: maxDepth=1 -> leaf means [0.5,0.5,10.5,10.5] exact");
    assertEq(d1.depth, 1, "DecisionTreeRegressor: maxDepth=1 -> depth === 1");
    const full = new DecisionTreeRegressor().fit(X, y);
    assert(eqArr(full.predict(X), y), "DecisionTreeRegressor: unlimited depth interpolates training rows exactly");
    assert(full.featureImportances.length === 1, "DecisionTreeRegressor: importances length cols");
    assert(full.depth >= 1, "DecisionTreeRegressor: depth >= 1");
    const again = new DecisionTreeRegressor().fit(X, y);
    assert(eqArr(again.predict(X), full.predict(X)), "DecisionTreeRegressor: deterministic refit");
    const r = DecisionTreeRegressor.deserialize(full.serialize());
    assert(eqArr(r.predict(X), y), "DecisionTreeRegressor: serialize round-trip predictions");
    full.close();
    assert(full.closed === true, "DecisionTreeRegressor: close -> closed === true");
}

/* ================================================================== *
 * S12. RandomForestClassifier — doc: "a fixed seed reproduces a forest
 * exactly". 8 rows (4 per class) make a one-class bootstrap sample
 * (p = 2*(4/8)^8 = 1/128 per tree) far too rare to flip a 16-tree vote.
 * ================================================================== */
{
    const X = [[0], [1], [2], [3], [10], [11], [12], [13]], y = [0, 0, 0, 0, 1, 1, 1, 1];
    const m = new RandomForestClassifier({ nEstimators: 16, seed: 7 }).fit(X, y);
    assert(eqArr(m.predict(X), y), "RandomForestClassifier: majority vote classifies separable training rows");
    const proba = m.predictProba(X);
    assert(proba.length === 8 && proba.every(r => r.length === 2), "RandomForestClassifier: predictProba 8x2");
    assert(proba.every(r => Math.abs(r[0] + r[1] - 1) <= 1e-12), "RandomForestClassifier: vote fractions sum to 1");
    assert(m.apply(X).flat().length === 8 * 16, "RandomForestClassifier: apply -> 8 rows x 16 trees leaf ids");
    assert(m.featureImportances.length === 1 && m.featureImportances[0] >= 0, "RandomForestClassifier: importances length cols, non-negative");
    assert(typeof m.depth === "number" && m.depth >= 1, "RandomForestClassifier: forest depth >= 1");
    const m2 = new RandomForestClassifier({ nEstimators: 16, seed: 7 }).fit(X, y);
    assert(eqArr(m2.predict(X), m.predict(X)) && eqArr(m2.featureImportances, m.featureImportances),
        "RandomForestClassifier: same seed -> identical predictions AND importances (doc determinism)");
    const r = RandomForestClassifier.deserialize(m.serialize());
    assert(eqArr(r.predict(X), y), "RandomForestClassifier: serialize round-trip predictions");
    m.close();
    assert(m.closed === true, "RandomForestClassifier: close -> closed === true");
}

/* ================================================================== *
 * S13. RandomForestRegressor.
 * Leaf predictions are means of bootstrap targets, so for x in the left
 * cluster every tree predicts a value in [0,3] (+ slack for the rare
 * all-right bootstrap, p=1/128 per tree) and [10,13] on the right.
 * ================================================================== */
{
    const X = [[0], [1], [2], [3], [10], [11], [12], [13]], y = [0, 1, 2, 3, 10, 11, 12, 13];
    const m = new RandomForestRegressor({ nEstimators: 16, seed: 3 }).fit(X, y);
    const p = m.predict(X);
    assert(Array.isArray(p) && p.length === 8, "RandomForestRegressor: predict -> number[] of 8");
    assert(p.slice(0, 4).every(v => v >= 0 && v <= 3.5), "RandomForestRegressor: left-cluster predictions in [0,3.5]");
    assert(p.slice(4).every(v => v >= 9.5 && v <= 13), "RandomForestRegressor: right-cluster predictions in [9.5,13]");
    const m2 = new RandomForestRegressor({ nEstimators: 16, seed: 3 }).fit(X, y);
    assert(eqArr(m2.predict(X), p), "RandomForestRegressor: same seed -> identical predictions");
    const r = RandomForestRegressor.deserialize(m.serialize());
    assert(eqArr(r.predict(X), p), "RandomForestRegressor: serialize round-trip predictions");
    m.close();
    assert(m.closed === true, "RandomForestRegressor: close -> closed === true");
}

/* ================================================================== *
 * S14. GradientBoostingRegressor — starts at mean(y)=5.5 and fits
 * gradients; 50 rounds at lr 0.5 on 4 points lands within 2 of each y.
 * ================================================================== */
{
    const X = [[0], [1], [10], [11]], y = [0, 1, 10, 11];
    const m = new GradientBoostingRegressor({ nEstimators: 50, maxDepth: 2, learningRate: 0.5, seed: 1 }).fit(X, y);
    const p = m.predict(X);
    assert(Array.isArray(p) && p.length === 4, "GradientBoostingRegressor: predict -> number[] of 4");
    assert(p.every((v, i) => Math.abs(v - y[i]) <= 2), "GradientBoostingRegressor: each training residual shrinks to <= 2");
    assert(m.depth <= 2, "GradientBoostingRegressor: depth <= maxDepth");
    assert(m.featureImportances.length === 1, "GradientBoostingRegressor: importances length cols");
    const m2 = new GradientBoostingRegressor({ nEstimators: 50, maxDepth: 2, learningRate: 0.5, seed: 1 }).fit(X, y);
    assert(eqArr(m2.predict(X), p), "GradientBoostingRegressor: same seed -> identical predictions");
    const r = GradientBoostingRegressor.deserialize(m.serialize());
    assert(eqArr(r.predict(X), p), "GradientBoostingRegressor: serialize round-trip predictions");
    m.close();
    assert(m.closed === true, "GradientBoostingRegressor: close -> closed === true");
}

/* ================================================================== *
 * S15. GradientBoostingClassifier — log-odds boosting on separable data.
 * ================================================================== */
{
    const X = [[0], [1], [10], [11]], y = [0, 0, 1, 1];
    const m = new GradientBoostingClassifier({ nEstimators: 30, maxDepth: 2, learningRate: 0.5, seed: 1 }).fit(X, y);
    assert(eqArr(m.predict(X), y), "GradientBoostingClassifier: separable training rows classified exactly");
    const proba = m.predictProba(X);
    assert(proba.length === 4 && proba.every(r => r.length === 2 && Math.abs(r[0] + r[1] - 1) <= 1e-6),
        "GradientBoostingClassifier: predictProba rows sum to 1");
    const m2 = new GradientBoostingClassifier({ nEstimators: 30, maxDepth: 2, learningRate: 0.5, seed: 1 }).fit(X, y);
    assert(eqArr(m2.predict(X), m.predict(X)), "GradientBoostingClassifier: same seed -> identical predictions");
    m.close();
    assert(m.closed === true, "GradientBoostingClassifier: close -> closed === true");
}

/* ================================================================== *
 * S16. XGBRegressor — doc: "NaN means missing", so a NaN feature value
 * is ACCEPTED and routed down the missing branch; predictions stay
 * finite. Clean rows land near their targets.
 * ================================================================== */
{
    const X = [[0], [1], [NaN], [10], [11]], y = [0, 1, 6, 10, 11];
    const m = new XGBRegressor({ nEstimators: 20, maxDepth: 2, seed: 1 }).fit(X, y);
    const p = m.predict(X);
    assert(p.length === 5 && p.every(v => Number.isFinite(v)), "XGBRegressor: NaN feature accepted (missing); predictions finite");
    assert(Math.abs(p[0] - 0) <= 3 && Math.abs(p[1] - 1) <= 3, "XGBRegressor: left-cluster predictions near targets");
    assert(Math.abs(p[3] - 10) <= 3 && Math.abs(p[4] - 11) <= 3, "XGBRegressor: right-cluster predictions near targets");
    assert(typeof m.bestRounds === "number" && m.bestRounds >= 0 && m.bestRounds <= 20, "XGBRegressor: bestRounds within [0, nEstimators]");
    assert(m.featureImportances.length === 1, "XGBRegressor: importances length cols");
    const m2 = new XGBRegressor({ nEstimators: 20, maxDepth: 2, seed: 1 }).fit(X, y);
    assert(eqArr(m2.predict(X), p), "XGBRegressor: same seed -> identical predictions");
    const r = XGBRegressor.deserialize(m.serialize());
    assert(eqArr(r.predict(X), p), "XGBRegressor: serialize round-trip predictions");
    m.close();
    assert(m.closed === true, "XGBRegressor: close -> closed === true");
}

/* ================================================================== *
 * S17. XGBClassifier — the classifier twin.
 * ================================================================== */
{
    const X = [[0], [1], [NaN], [10], [11]], y = [0, 0, 0, 1, 1];
    // minChildWeight is a floor on the child HESSIAN (doc, default 1); with
    // h = p(1-p) <= 0.25 per row a 5-row binary fit can never clear it (the
    // base-rate tree is the correct xgboost-style answer), so pass a floor
    // sized to the data to pin actual learning.
    const m = new XGBClassifier({ nEstimators: 20, maxDepth: 2, seed: 1, minChildWeight: 0.1 }).fit(X, y);
    const p = m.predict(X);
    assert(p.length === 5 && p.every(v => v === 0 || v === 1), "XGBClassifier: NaN accepted, labels are class ids");
    assert(eqArr(p.slice(3), [1, 1]) && p[0] === 0, "XGBClassifier: separated rows classified correctly");
    const proba = m.predictProba(X);
    assert(proba.length === 5 && proba.every(r => r.length === 2 && Math.abs(r[0] + r[1] - 1) <= 1e-6),
        "XGBClassifier: predictProba rows sum to 1");
    const m2 = new XGBClassifier({ nEstimators: 20, maxDepth: 2, seed: 1, minChildWeight: 0.1 }).fit(X, y);
    assert(eqArr(m2.predict(X), p), "XGBClassifier: same seed -> identical predictions");
    m.close();
    assert(m.closed === true, "XGBClassifier: close -> closed === true");
}

/* ================================================================== *
 * S18. PCA — cyclic Jacobi.
 * X=[[0,0],[2,0],[4,0]]: mean [2,0]; column 1 is constant so the
 * covariance is exactly diag(8/3, 0) (population: 4+0+4)/3). Principal
 * axis = feature 0, eigenvalue 8/3, total variance 8/3 -> ratio 1.
 * transform(X) = (+-) [-2, 0, 2] along that axis (sign is free).
 * whitened: |t| = 2/sqrt(8/3).
 * ================================================================== */
{
    const X = [[0, 0], [2, 0], [4, 0]];
    const pca = new PCA(1).fit(X);
    assertDeepEq(pca.mean, [2, 0], "PCA: mean [2,0] exact");
    assert(pca.components.length === 1 && pca.components[0].length === 2, "PCA: components is nComponents x cols");
    assertClose(Math.abs(pca.components[0][0]), 1, 1e-12, "PCA: first component is the x-axis (|c0| = 1)");
    assertClose(Math.abs(pca.components[0][1]), 0, 1e-12, "PCA: zero loading on the constant column");
    // explainedVariance = retained eigenvalues of the SAMPLE covariance
    // (n-1; sklearn's convention — the doc does not pin a flavor): 4 for [0,2,4]
    assertClose(pca.explainedVariance[0], 4, 1e-12, "PCA: explainedVariance = 4 (sample variance of [0,2,4])");
    assertClose(pca.explainedVarianceRatio[0], 1, 1e-12, "PCA: ratio 1 (all variance on component 0)");
    const t = pca.transform(X);
    assert(t.length === 3 && t[0].length === 1, "PCA: transform -> 3 rows of nComponents");
    assertEq(t[1][0], 0, "PCA: center row transforms to 0 exactly");
    assertClose(Math.abs(t[0][0]), 2, 1e-9, "PCA: |t| = 2 (2 from the mean)");
    assertEq(Math.abs(t[0][0]), Math.abs(t[2][0]), "PCA: symmetric rows map to equal magnitude");
    const inv = pca.inverseTransform(t);
    assertDeepClose(inv, X, 1e-9, "PCA: inverseTransform round-trips X");
    const ft = pca.fitTransform(X);
    assertDeepClose(ft, t, 1e-12, "PCA: fitTransform === transform(fit(X))");
    // DOC-TENSION: the contract names `whiten` without defining it; the standard
    // reading (divide by sqrt(explainedVariance)) is pinned here (sqrt(4) = 2).
    const wp = new PCA(1, true).fit(X);
    const wt = wp.transform(X);
    assertClose(Math.abs(wt[0][0]), 2 / Math.sqrt(4), 1e-9, "PCA: whiten divides by sqrt(explainedVariance)");
    // flat Float64Array input (rows=3, cols=2)
    const pf = new PCA(1).fit(new Float64Array([0, 0, 2, 0, 4, 0]), 3, 2);
    assertClose(Math.abs(pf.transform(X)[0][0]), 2, 1e-9, "PCA: flat-X fit -> same transform");
    const out = new Float64Array(3);
    assertEq(pca.transformInto(out, X), 3, "PCA: transformInto returns rows written (3)");
    assertClose(Math.abs(out[0]), 2, 1e-9, "PCA: transformInto writes the transform");
    assertThrows(() => pca.transformInto(new Float64Array(2), X), "PCA: transformInto short out -> RangeError (doc)", RangeError);
    const p2 = new PCA(2).fit(X);
    const t2 = p2.transform(X);
    assert(t2.length === 3 && t2[0].length === 2, "PCA: nComponents=2 -> 3x2 transform");
    assertEq(t2[1][1], 0, "PCA: constant column's score is exactly 0");
    assertDeepClose(p2.inverseTransform(t2), X, 1e-9, "PCA: 2-component round trip recovers X");
    const again = new PCA(1).fit(X);
    assertDeepClose(again.components, pca.components, 1e-12, "PCA: deterministic Jacobi refit");
    const r = PCA.deserialize(pca.serialize());
    assertDeepClose(r.transform(X), t, 1e-12, "PCA: serialize round-trip transform");
    pca.close();
    assert(pca.closed === true, "PCA: close -> closed === true");
    // doc: scaler/PCA transforms are dense-only and throw a TypeError naming toDense()
    assertThrows(() => pca.transform(CSR.fromDense(X)), "PCA: transform(CSR) -> TypeError naming toDense() (doc)", TypeError, /toDense/);
}

/* ================================================================== *
 * S19. KNClassifier — lazy k-NN, squared distances.
 * Flip fixture: X=[[0],[10],[11]], y=[0,1,1], query x=1.
 *   3 nearest: 0 (d=1), 10 (d=9), 11 (d=10).
 *   uniform  -> votes 1,1,1? no: classes {0,1,1} -> majority 1.
 *   distance -> w0 = 1/1 = 1 vs w1 = 1/9 + 1/10 = 19/90 ~ 0.211 -> 0.
 * ================================================================== */
{
    const X = [[0], [10], [11]], y = [0, 1, 1];
    assertEq(new KNClassifier(3, "uniform").fit(X, y).predict([[1]])[0], 1, "KNClassifier: uniform majority {0,1,1} -> 1");
    assertEq(new KNClassifier(3, "distance").fit(X, y).predict([[1]])[0], 0, "KNClassifier: distance weights 1 vs 19/90 -> 0 (uniform/distance flip)");
    const m = new KNClassifier(3, "uniform").fit([[0], [1], [2], [10], [11], [12]], [0, 0, 0, 1, 1, 1]);
    assertEq(m.predict([[0.4]])[0], 0, "KNClassifier: 3 nearest of 0.4 are {0,1,2} -> 0");
    assertEq(m.predict([[10.6]])[0], 1, "KNClassifier: 3 nearest of 10.6 are {10,11,12} -> 1");
    // 6.5: 3 nearest are {10 (3.5), 2 (4.5), 11 (4.5)} -> majority 1 whichever
    // way the d=4.5 tie between rows 2 and 11 breaks (6 itself has a
    // label-flipping tie at d=5, which the contract leaves open)
    assertEq(m.predict([[6.5]])[0], 1, "KNClassifier: 3 nearest of 6.5 are {10,2,11} -> majority 1");
    const pf = m.predict([[0.4], [6]], undefined, undefined, { as: "f64" });
    assert(pf instanceof Float64Array && pf.length === 2, "KNClassifier: predict {as:'f64'} -> Float64Array");
    const m2 = new KNClassifier(3, "uniform").fit([[0], [1], [2], [10], [11], [12]], [0, 0, 0, 1, 1, 1]);
    assert(eqArr(m2.predict([[0.4], [6]]), m.predict([[0.4], [6]])), "KNClassifier: lazy refit is deterministic");
    const r = KNClassifier.deserialize(m.serialize());
    assert(eqArr(r.predict([[6]]), [0]), "KNClassifier: serialize round-trip predictions");
    m.close();
    assert(m.closed === true, "KNClassifier: close -> closed === true");
}

/* ================================================================== *
 * S20. KNRegressor — mean of neighbour targets.
 * X=[[0],[2]], y=[0,4], k=2: any query averages both -> exactly 2.
 * ================================================================== */
{
    const m = new KNRegressor(2, "uniform").fit([[0], [2]], [0, 4]);
    assertEq(m.predict([[1]])[0], 2, "KNRegressor: uniform mean (0+4)/2 = 2 exact");
    assert(eqArr(m.predict([[0], [2]]), [2, 2]), "KNRegressor: queries at training points still average both -> [2,2]");
    const d = new KNRegressor(2, "distance").fit([[0], [2]], [0, 4]);
    assertEq(d.predict([[1]])[0], 2, "KNRegressor: equal inverse distances (1/1, 1/1) -> mean 2");
    const r = KNRegressor.deserialize(m.serialize());
    assert(eqArr(r.predict([[1]]), [2]), "KNRegressor: serialize round-trip predictions");
    m.close();
    assert(m.closed === true, "KNRegressor: close -> closed === true");
}

/* ================================================================== *
 * S21. DBScan — eps-neighbourhood clustering.
 * X=[[0],[1],[2],[10],[11],[20]], eps=1.5, minPts=2:
 *   {0,1,2} chain into one cluster, {10,11} into another, 20 has no
 *   neighbour -> noise (sentinel -1; DBSCAN convention, sklearn oracle).
 * ================================================================== */
{
    const d = new DBScan(1.5, 2).fit([[0], [1], [2], [10], [11], [20]]);
    const lab = d.labels;
    assert(lab.length === 6, "DBScan: labels one per row");
    assert(lab[0] === lab[1] && lab[1] === lab[2], "DBScan: chain {0,1,2} shares a label");
    assert(lab[3] === lab[4], "DBScan: pair {10,11} shares a label");
    assert(lab[0] !== lab[3], "DBScan: the two clusters differ");
    assertEq(lab[5], -1, "DBScan: isolated point is noise (-1)"); // DOC-TENSION: sentinel not spelled out in the slice; DBSCAN convention
    assertEq(d.nClusters, 2, "DBScan: nClusters 2");
    assertEq(d.eps, 1.5, "DBScan: eps echoes the constructor");
    const d0 = new DBScan(0.1, 2).fit([[0], [10]]);
    assert(d0.nClusters === 0 && d0.labels.every(l => l === -1), "DBScan: no neighbours anywhere -> 0 clusters, all noise");
    const again = new DBScan(1.5, 2).fit([[0], [1], [2], [10], [11], [20]]);
    assert(eqArr(again.labels, lab), "DBScan: deterministic refit (no randomness)");
    const r = DBScan.deserialize(d.serialize());
    assert(eqArr(r.labels, lab), "DBScan: serialize round-trips labels");
    d.close();
    assert(d.closed === true, "DBScan: close -> closed === true");
}

/* ================================================================== *
 * S22. StandardScaler — per-column z-score.
 * Column [0,2,4]: mean 2; population variance (4+0+4)/3 = 8/3,
 * std = sqrt(8/3); transform -> (+-) sqrt(3/2) = 1.2247448713915892.
 * DOC-TENSION: the slice does not pin ddof; the doc names scikit-learn
 * as the module's oracle, which uses the POPULATION std (ddof 0).
 * Documented pin: constant columns report std 1.0.
 * ================================================================== */
{
    const X = [[0], [2], [4]];
    const s = new StandardScaler().fit(X);
    assertDeepEq(s.mean, [2], "StandardScaler: mean [2] exact");
    assertClose(s.std[0], Math.sqrt(8 / 3), 1e-12, "StandardScaler: population std sqrt(8/3)");
    const t = s.transform(X);
    assertClose(t[0][0], -Math.sqrt(1.5), 1e-12, "StandardScaler: (0-2)/sqrt(8/3) = -sqrt(3/2)");
    assertEq(t[1][0], 0, "StandardScaler: (2-2)/std = 0 exact");
    assertClose(t[2][0], Math.sqrt(1.5), 1e-12, "StandardScaler: (4-2)/sqrt(8/3) = sqrt(3/2)");
    const c = new StandardScaler().fit([[5, 0], [5, 0], [5, 0]]);
    assertEq(c.std[1], 1.0, "StandardScaler: constant column reports std 1.0 (doc-pinned)");
    assertClose(c.transform([[5, 0]])[0][1], 0, 1e-15, "StandardScaler: constant column (mean 0) transforms to 0: (0-0)/1");
    const rt = s.inverseTransform(t);
    assertDeepClose(rt, X, 1e-12, "StandardScaler: inverseTransform round-trips X");
    const ft = new StandardScaler().fitTransform(X);
    assertDeepClose(ft, t, 1e-12, "StandardScaler: fitTransform === fit+transform");
    const out = new Float64Array(3);
    assertEq(s.transformInto(out, X), 3, "StandardScaler: transformInto returns rows written (3)");
    assertClose(Math.abs(out[0]), Math.sqrt(1.5), 1e-12, "StandardScaler: transformInto writes the z-scores");
    assertThrows(() => s.transformInto(new Float64Array(2), X), "StandardScaler: transformInto short out -> RangeError (doc)", RangeError);
    const sf = new StandardScaler().fit(new Float64Array([0, 2, 4]), 3, 1);
    assertClose(sf.transform([[4]])[0][0], Math.sqrt(1.5), 1e-12, "StandardScaler: flat-X fit -> same transform");
    const s2 = new StandardScaler().fit(X);
    assert(eqArr(s2.mean, s.mean), "StandardScaler: refit deterministic");
    const r = StandardScaler.deserialize(s.serialize());
    assertDeepClose(r.transform(X), t, 1e-12, "StandardScaler: serialize round-trip transform");
    s.close();
    assert(s.closed === true, "StandardScaler: close -> closed === true");
    // doc: scaler fits are dense-only and throw a TypeError naming toDense()
    assertThrows(() => s2.fit(CSR.fromDense(X)), "StandardScaler: fit(CSR) -> TypeError naming toDense() (doc)", TypeError, /toDense/);
}

/* ================================================================== *
 * S23. MinMaxScaler — per-column scaling to [0,1].
 * Column [0,2,4]: dataMin 0, dataMax 4; (x-0)/4 -> [0, 0.5, 1] exact;
 * inverse y -> y*4+0.
 * ================================================================== */
{
    const X = [[0], [2], [4]];
    const s = new MinMaxScaler().fit(X);
    assertDeepEq(s.dataMin, [0], "MinMaxScaler: dataMin [0] exact");
    assertDeepEq(s.dataMax, [4], "MinMaxScaler: dataMax [4] exact");
    const t = s.transform(X);
    assertDeepEq(t, [[0], [0.5], [1]], "MinMaxScaler: (x-0)/4 -> [0,0.5,1] exact");
    assertDeepEq(s.inverseTransform([[0.25]]), [[1]], "MinMaxScaler: inverse 0.25 -> 1 exact");
    assertDeepEq(s.inverseTransform([[1]]), [[4]], "MinMaxScaler: inverse 1 -> 4 exact");
    assertDeepClose(s.inverseTransform(t), X, 1e-12, "MinMaxScaler: inverseTransform round-trips X");
    const out = new Float64Array(3);
    assertEq(s.transformInto(out, X), 3, "MinMaxScaler: transformInto returns rows written (3)");
    assertDeepEq(Array.from(out), [0, 0.5, 1], "MinMaxScaler: transformInto writes [0,0.5,1]");
    assertThrows(() => s.transformInto(new Float64Array(2), X), "MinMaxScaler: transformInto short out -> RangeError (doc)", RangeError);
    const r = MinMaxScaler.deserialize(s.serialize());
    assertDeepEq(r.transform(X), t, "MinMaxScaler: serialize round-trip transform");
    s.close();
    assert(s.closed === true, "MinMaxScaler: close -> closed === true");
}

/* ================================================================== *
 * S24. CSR — scipy layout; duplicate column indices SUM in storage
 * order; non-finite values are refused with a RangeError naming the cell.
 * ================================================================== */
{
    // values [1,2,3], columns [0,1,0], rowPointers [0,1,3], cols 2:
    // row0 = [1] at col0 -> [1,0]; row1 = 2@col1, 3@col0 -> [3,2]
    const c = new CSR([1, 2, 3], [0, 1, 0], [0, 1, 3], 2);
    assert(c.rows === 2 && c.cols === 2, "CSR: rows = rowPointers.length-1, cols echoed");
    assertEq(c.nnz, 3, "CSR: nnz = values.length = 3");
    assertEq(c.density, 3 / 4, "CSR: density = nnz/(rows*cols) = 3/4");
    assertDeepEq(c.row(0), [1, 0], "CSR: row(0) = [1,0]");
    assertDeepEq(c.row(1), [3, 2], "CSR: row(1) = [3,2] (free column order)");
    assertDeepEq(c.toDense(), [[1, 0], [3, 2]], "CSR: toDense matches the rows");
    // doc: duplicate column indices within a row SUM (scipy's rule), storage order free
    const dup = new CSR([1, 2], [0, 0], [0, 2], 1);
    assertDeepEq(dup.row(0), [3], "CSR: duplicate columns sum (1+2 = 3)");
    const dup2 = new CSR([2, 1], [0, 0], [0, 2], 1);
    assertDeepEq(dup2.row(0), [3], "CSR: duplicate sum is order-free (2+1 = 3)");
}
{
    // fromDense drops exact zeros
    const c = CSR.fromDense([[0, 5], [0, 0]]);
    assertEq(c.nnz, 1, "CSR.fromDense: drops exact zeros -> nnz 1");
    assertClose(c.density, 0.25, 1e-15, "CSR.fromDense: density 1/4");
    assertDeepEq(c.toDense(), [[0, 5], [0, 0]], "CSR.fromDense: toDense round-trips the dense matrix");
}
{
    // doc-pinned refusals: a CSR never carries a non-finite value
    assertThrows(() => new CSR([Infinity], [0], [0, 1], 1), "CSR: non-finite value refused (doc RangeError)", RangeError);
    assertThrows(() => CSR.fromDense([[1, NaN]]), "CSR.fromDense: NaN refused (doc RangeError)", RangeError);
    const c = new CSR([1], [0], [0, 1], 1);
    c.close();
    assert(c.closed === true, "CSR: close -> closed === true");
}

/* ================================================================== *
 * S25. Pipeline — feature stages + final estimator.
 * StandardScaler -> LinearRegression on X=[[0],[2],[4]], y=[1,3,5]:
 * the scaled fit is the same line, so predictions recover [1,3,5]
 * (scaled-slope * scaled-x reintroduces 1ulp rounding -> close, 1e-9).
 * ================================================================== */
{
    const X = [[0], [2], [4]], y = [1, 3, 5];
    const scaler = new StandardScaler();
    const reg = new LinearRegression();
    const pipe = new Pipeline([scaler, reg]);
    assert(pipe.length === 2, "Pipeline: length 2");
    assert(pipe.fitted === false, "Pipeline: fitted false before fit");
    assert(pipe.fit(X, y) === pipe, "Pipeline: fit returns this");
    assert(pipe.fitted === true, "Pipeline: fitted true after fit");
    const p = pipe.predict(X);
    assert(Array.isArray(p) && p.length === 3, "Pipeline: predict -> number[] of 3");
    assertDeepClose(p, [1, 3, 5], 1e-9, "Pipeline: scaler+OLS round-trips the exact line");
    const t = pipe.transform(X);
    assert(t.length === 3 && t[0].length === 1, "Pipeline: transform applies the pre-estimator stages");
    assertEq(t[1][0], 0, "Pipeline: transform centers the middle row to 0 exactly");
    const st0 = pipe.stage(0);
    assert(st0 instanceof StandardScaler, "Pipeline: stage(0) is the scaler");
    assertDeepEq(st0.mean, [2], "Pipeline: fitted stage exposes scaler mean [2] exact");
    assert(pipe.stage(-1) instanceof LinearRegression, "Pipeline: stage(-1) is the final estimator (negative counts from end)");
    assert(pipe.estimator instanceof LinearRegression, "Pipeline: estimator is the final stage");
    const clf = new Pipeline([new StandardScaler(), new LogisticRegression({ learningRate: 0.5, maxIter: 3000, tol: 1e-10 })])
        .fit([[0], [1], [10], [11]], [0, 0, 1, 1]);
    const proba = clf.predictProba([[0], [11]]);
    assert(proba.length === 2 && proba.every(r => r.length === 2 && Math.abs(r[0] + r[1] - 1) <= 1e-9),
        "Pipeline: predictProba rows sum to 1 through the classifier");
    pipe.close();
    assert(pipe.closed === true, "Pipeline: close -> closed === true");
}

/* ================================================================== *
 * S26. imputeMean / dropMissing.
 * ================================================================== */
{
    const T = [
        // col0 finite [1,3] -> mean 1; col1 finite [6] -> mean 6
        ["imputeMean: per-column finite means", [[1, 2], [NaN, 6]], [[1, 2], [1, 6]]],
        // col1 finite values = [4] only (NaN excluded) -> NaN becomes 4
        ["imputeMean: mean excludes the non-finite entries", [[1, NaN], [3, 4]], [[1, 4], [3, 4]]],
    ];
    for (const [label, X, exp] of T) assertDeepEq(imputeMean(X), exp, label);
}
{
    const r = dropMissing([[1, 2], [NaN, 4], [3, 6]], [0, 1, 2]);
    assertDeepEq(r.X, [[1, 2], [3, 6]], "dropMissing: removes rows holding a non-finite value");
    assert(r.y instanceof Float64Array && eqArr(r.y, [0, 2]), "dropMissing: survivors' targets as Float64Array [0,2]");
    assertDeepEq(r.kept, [0, 2], "dropMissing: kept lists the surviving row indices");
    const ry = dropMissing([[1], [2]]);
    assert(ry.y === undefined, "dropMissing: without y, y is undefined");
    assertDeepEq(ry.X, [[1], [2]], "dropMissing: nothing dropped when all rows are finite");
}

/* ================================================================== *
 *  Predictor input finiteness gates (API consistency: every other
 *  predictor — KMeans, GaussianNB, KNN, SVC, GMM — refuses non-finite
 *  predict input with the standard RangeError; LinearRegression and
 *  LogisticRegression are bound by the same "rejects missing data"
 *  contract dyna:ml applies everywhere else).
 * ================================================================== */
{
    const Xf = [[1], [2], [3], [4]];
    const lin = new LinearRegression().fit(Xf, [2, 4, 6, 8]);
    const log = new LogisticRegression().fit([[0], [1], [0.1], [0.9]], [0, 1, 0, 1]);
    const Xnan = [[1], [NaN]];
    assertThrows(() => lin.predict(Xnan),
        "LinearRegression.predict refuses non-finite rows like every other predictor",
        RangeError, /predict input contains NaN or infinite values/);
    assertThrows(() => log.predict(Xnan),
        "LogisticRegression.predict refuses non-finite rows like every other predictor",
        RangeError, /predict input contains NaN or infinite values/);
    assertThrows(() => lin.predict([[Infinity]]),
        "LinearRegression.predict refuses infinite values too",
        RangeError, /predict input contains NaN or infinite values/);
    assertClose(lin.predict([[2.5]])[0], 5, 1e-9, "finite rows still predict 2*x after the gate");
}
{
    const good = { learningRate: 0.5, maxIter: 10, tol: 1e-3, l1: 0, l2: 0, C: 1.0 };
    assertThrows(() => new LogisticRegression({ ...good, C: 0 }),
        "d.ts LogisticRegression(opts?: {..., C?: number}): C=0 collapses l2 to 1/C=+inf, refused like the other positive options",
        RangeError, /C must be in \[1e-12, 1e\+12\]/);
    assertThrows(() => new LogisticRegression({ ...good, C: -1 }),
        "d.ts LogisticRegression(opts?: {..., C?: number}): negative C inverts the regularizer, refused",
        RangeError, /C must be in \[1e-12, 1e\+12\]/);
    assertThrows(() => new LogisticRegression({ ...good, C: Infinity }),
        "d.ts LogisticRegression(opts?: {..., C?: number}): infinite C refused",
        RangeError, /C must be in \[1e-12, 1e\+12\]/);
    const m = new LogisticRegression({ ...good, C: 2.0 });
    assert(m !== undefined, "C within [1e-12, 1e12] still constructs");
}

print("bb_ml: all tests passed (" + n + " assertions)");
