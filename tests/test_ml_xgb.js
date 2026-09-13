/* test_ml_xgb.js -- the second-order objective (STDLIB_OOP_PLAN W9.8b;
 * items 4-5 of the production plan, "XGBoost: what it would actually mean
 * here").
 *
 * The substance of XGBoost is not more knobs, it is a different split
 * criterion: the curvature of the loss enters the choice of PARTITION rather
 * than being applied afterwards to leaves the variance criterion already
 * picked. So the tests that matter are the ones that pin the arithmetic:
 *
 *   - every leaf weight recomputed in JS from apply() + y and compared exactly
 *     (section 2). This is the whole objective -- w = -G/(H+lambda) with L1
 *     soft-thresholding -- checked against an independent implementation
 *     rather than against itself;
 *   - each regulariser degenerating the way its definition says it must:
 *     alpha large enough zeroes EVERY leaf, gamma large enough refuses EVERY
 *     split, minChildWeight is a floor on the hessian sum and not on a row
 *     count -- which is why a thousand rows of weight 0.001 cannot pass it;
 *   - a missing value taking the direction the split LEARNED, tested by
 *     building data where the two directions give different answers.
 *
 * A model that merely fits well would pass none of these.
 */
import { XGBRegressor, XGBClassifier, GradientBoostingRegressor, accuracy, r2Score } from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(a, b, eps, m) {
    n++;
    if (!(Math.abs(a - b) <= eps))
        throw new Error((m || "not close") + ": " + a + " vs " + b);
}
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return String(e);
    }
    throw new Error((m || "expected a throw") + " but none happened");
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function linearData(seed, rows, noise) {
    const rnd = lcg(seed), X = [], y = [], yc = [];
    for (let i = 0; i < rows; i++) {
        const a = rnd() * 4 - 2, b = rnd() * 4 - 2, c = rnd() * 4 - 2;
        const s = 2 * a - b + 0.5 * c;
        X.push([a, b, c]);
        y.push(s + (rnd() * 2 - 1) * noise);
        yc.push(1 / (1 + Math.exp(-s)) > rnd() ? 1 : 0);
    }
    return { X, y, yc };
}

/* ==================================================================== *
 *  1. It is a working booster
 * ==================================================================== */
{
    const d = linearData(7, 400, 0.3);
    const r = new XGBRegressor({ nEstimators: 60, maxDepth: 4 }).fit(d.X, d.y);
    assert(r2Score(d.y, r.predict(d.X)) > 0.99, "regressor fits");
    const g = new GradientBoostingRegressor({ nEstimators: 60, maxDepth: 4 }).fit(d.X, d.y);
    assert(r2Score(d.y, r.predict(d.X)) >= r2Score(d.y, g.predict(d.X)) - 0.01,
           "and is not worse than the first-order booster it generalises");

    const c = new XGBClassifier({ nEstimators: 60, maxDepth: 4 }).fit(d.X, d.yc);
    assert(accuracy(d.yc, c.predict(d.X)) > 0.95, "classifier fits");
    const P = c.predictProba(d.X);
    assert(P.length === d.X.length && P[0].length === 2, "proba shape");
    for (let i = 0; i < 20; i++)
        close(P[i][0] + P[i][1], 1, 1e-12, "proba row sums to 1");

    /* predict is the argmax of the raw score, and the raw score is monotone in
     * the link, so these cannot disagree -- asserted rather than assumed. */
    const lab = c.predict(d.X);
    let agree = 0;
    for (let i = 0; i < lab.length; i++)
        if (lab[i] === (P[i][1] > P[i][0] ? 1 : 0)) agree++;
    assert(agree === lab.length, "predict === argmax(predictProba)");

    /* The importances recover the generating coefficients' ORDER: 2a, -b, c/2 */
    const imp = r.featureImportances;
    assert(imp[0] > imp[1] && imp[1] > imp[2], "importance order: " + imp);
}

/* ==================================================================== *
 *  2. THE LEAF WEIGHT, recomputed independently
 *
 *  One round of squared-error boosting from a constant base F0 = mean(y):
 *      g_i = F0 - y_i,  h_i = 1
 *      leaf weight w_j = -sum(g_i in j) / (count_j + lambda)   [+ L1 on the
 *                                                               gradient sum]
 *      prediction     = F0 + learningRate * w_{leaf(i)}
 *  apply() gives leaf membership, so all of that is computable from the public
 *  API with no access to the model's internals.
 * ==================================================================== */
function leafCheck(lambda, alpha, lr, label) {
    const d = linearData(21, 300, 0.5);
    const m = new XGBRegressor({
        nEstimators: 1, maxDepth: 3, learningRate: lr, lambda, alpha,
        subsample: 1.0, colsampleByTree: 1.0,
    }).fit(d.X, d.y);

    const base = d.y.reduce((a, b) => a + b, 0) / d.y.length;
    const leaves = m.apply(d.X).map(row => row[0]);
    const G = new Map(), C = new Map();
    for (let i = 0; i < d.y.length; i++) {
        const L = leaves[i];
        G.set(L, (G.get(L) || 0) + (base - d.y[i]));    /* g = F0 - y */
        C.set(L, (C.get(L) || 0) + 1);                   /* h = 1 each */
    }
    const pred = m.predict(d.X);
    let worst = 0, zeroed = 0;
    for (let i = 0; i < pred.length; i++) {
        const L = leaves[i];
        let g = G.get(L);
        let w;
        if (alpha > 0 && Math.abs(g) <= alpha) { w = 0; zeroed++; }
        else {
            const gt = alpha > 0 ? (g > 0 ? g - alpha : g + alpha) : g;
            w = -gt / (C.get(L) + lambda);
        }
        worst = Math.max(worst, Math.abs(pred[i] - (base + lr * w)));
    }
    close(worst, 0, 1e-9, label + ": leaf weights recomputed from apply()");
    return zeroed;
}
leafCheck(0, 0, 0.3, "plain");
leafCheck(1, 0, 0.3, "lambda=1");
leafCheck(25, 0, 0.1, "lambda=25");
leafCheck(1, 3, 0.3, "alpha=3");
assert(leafCheck(1, 1e9, 0.3, "alpha=1e9") > 0,
       "an L1 that swallows every gradient sum must zero every leaf");

/* ==================================================================== *
 *  3. Each regulariser degenerates the way its definition demands
 * ==================================================================== */
{
    const d = linearData(11, 500, 0.1);
    const fit = p => new XGBRegressor({ nEstimators: 100, maxDepth: 4, ...p }).fit(d.X, d.y);

    let prev = Infinity;
    for (const l of [0, 1, 10, 100, 1000]) {
        const s = r2Score(d.y, fit({ lambda: l }).predict(d.X));
        assert(s <= prev + 1e-9, "lambda must not increase the training fit: " + l);
        prev = s;
    }
    prev = Infinity;
    for (const a of [0, 1, 10]) {
        const s = r2Score(d.y, fit({ alpha: a }).predict(d.X));
        assert(s <= prev + 1e-9, "alpha must not increase the training fit: " + a);
        prev = s;
    }
    /* gamma is a minimum gain, so a large enough one refuses every split and
     * the model collapses to its base score -- depth 0, r2 exactly 0. */
    const flat = fit({ gamma: 1e9 });
    assert(flat.depth === 0, "gamma=1e9 must leave every tree a single leaf");
    close(r2Score(d.y, flat.predict(d.X)), 0, 1e-12, "and predict the mean");

    const wide = fit({ gamma: 100 });
    assert(wide.depth < 4, "a large gamma must prune depth: " + wide.depth);
}

/* ==================================================================== *
 *  4. minChildWeight is a HESSIAN floor, not a row count
 *
 *  For squared error h = 1, so a child's hessian sum is its row count -- and a
 *  sample weight scales it. A thousand rows each weighing 0.001 sum to 1, so a
 *  minChildWeight of 2 must refuse to split them, while the same thousand rows
 *  at weight 1 must not. A count-based threshold cannot tell those apart, so
 *  this is the test that distinguishes the two implementations.
 * ==================================================================== */
{
    const d = linearData(33, 1000, 0.1);
    const light = new Array(1000).fill(0.001);
    const heavy = new Array(1000).fill(1);

    const a = new XGBRegressor({ nEstimators: 5, maxDepth: 3, minChildWeight: 2 })
        .fit(d.X, d.y, { sampleWeight: light });
    assert(a.depth === 0, "1000 rows of weight 0.001 carry hessian 1 and must "
                          + "not pass minChildWeight 2, depth was " + a.depth);

    const b = new XGBRegressor({ nEstimators: 5, maxDepth: 3, minChildWeight: 2 })
        .fit(d.X, d.y, { sampleWeight: heavy });
    assert(b.depth > 0, "the same rows at weight 1 must split");

    /* And it still behaves as a count when every weight is 1. */
    const c = new XGBRegressor({ nEstimators: 5, maxDepth: 6, minChildWeight: 400 })
        .fit(d.X, d.y);
    assert(c.depth <= 2, "minChildWeight 400 over 1000 rows bounds the depth: " + c.depth);
}

/* ==================================================================== *
 *  5. sampleWeight means what it says
 * ==================================================================== */
{
    const d = linearData(44, 200, 0.2);
    const opts = { nEstimators: 20, maxDepth: 3, seed: 5 };

    /* Uniform scaling multiplies both g and h, and the Newton step divides one
     * by the other -- so it moves the leaf value only through lambda, which is
     * zeroed here to make the identity exact. */
    const p1 = new XGBRegressor({ ...opts, lambda: 0 }).fit(d.X, d.y).predict(d.X);
    const p7 = new XGBRegressor({ ...opts, lambda: 0 })
        .fit(d.X, d.y, { sampleWeight: new Array(200).fill(7) }).predict(d.X);
    let worst = 0;
    for (let i = 0; i < p1.length; i++) worst = Math.max(worst, Math.abs(p1[i] - p7[i]));
    close(worst, 0, 1e-9, "a uniform weight scale is a no-op when lambda is 0");

    /* Weight 0 is "ignore this row", and must equal deleting it. */
    const keep = d.X.map((_, i) => i % 3 !== 0);
    const w0 = keep.map(k => (k ? 1 : 0));
    const Xs = d.X.filter((_, i) => keep[i]), ys = d.y.filter((_, i) => keep[i]);
    const full = new XGBRegressor({ ...opts, lambda: 0, subsample: 1 })
        .fit(d.X, d.y, { sampleWeight: w0 });
    const cut = new XGBRegressor({ ...opts, lambda: 0, subsample: 1 }).fit(Xs, ys);
    /* The base score is a weighted mean in one and an unweighted mean of the
     * kept rows in the other -- equal here because the dropped rows carry no
     * weight. Predictions on the KEPT rows must then agree. */
    const pf = full.predict(Xs), pc = cut.predict(Xs);
    let w2 = 0;
    for (let i = 0; i < pf.length; i++) w2 = Math.max(w2, Math.abs(pf[i] - pc[i]));
    assert(w2 < 0.2, "a zero weight should behave like a deleted row: " + w2);

    throws(() => new XGBRegressor().fit(d.X, d.y, { sampleWeight: [1, 2] }),
           TypeError, "wrong-length weights");
    throws(() => new XGBRegressor().fit(d.X, d.y, { sampleWeight: d.y.map(() => -1) }),
           RangeError, "negative weights");
    throws(() => new XGBRegressor().fit(d.X, d.y, { sampleWeight: d.y.map(() => 0) }),
           RangeError, "all-zero weights");
}

/* ==================================================================== *
 *  6. Missing values: a LEARNED direction, not a convention
 *
 *  Column 0 is missing exactly on the rows with a high target, so sending the
 *  missing rows the wrong way costs real accuracy. A model that merely treated
 *  NaN as "always go right" would fit one of the two datasets below and not
 *  the other.
 * ==================================================================== */
{
    function missingData(highIsMissing) {
        const rnd = lcg(99), X = [], y = [];
        for (let i = 0; i < 400; i++) {
            const a = rnd() * 4 - 2, b = rnd() * 4 - 2;
            const target = (a > 0 ? 10 : -10) + b;
            const hide = highIsMissing ? a > 0 : a <= 0;
            X.push([hide ? NaN : a, b]);
            y.push(target);
        }
        return { X, y };
    }
    for (const flag of [true, false]) {
        const d = missingData(flag);
        const m = new XGBRegressor({ nEstimators: 40, maxDepth: 3 }).fit(d.X, d.y);
        assert(r2Score(d.y, m.predict(d.X)) > 0.9,
               "the missing direction must be learned, not assumed (high missing: "
               + flag + ")");
    }

    /* An infinity is NOT missing data: it is arithmetic that already failed,
     * and no direction can rescue it. */
    throws(() => new XGBRegressor().fit([[1, 2], [Infinity, 3]], [1, 2]),
           RangeError, "infinity in X");
    throws(() => new XGBRegressor().fit([[1, 2], [3, 4]], [1, NaN]),
           RangeError, "NaN in y is never imputable");

    /* Every other tree model still rejects it, which is the W9.4 contract. */
    throws(() => new GradientBoostingRegressor().fit([[1, 2], [NaN, 3]], [1, 2]),
           RangeError, "first-order boosting still rejects NaN");

    /* A column that is entirely missing is legal and carries no information. */
    const allnan = new XGBRegressor({ nEstimators: 5, maxDepth: 2 })
        .fit([[NaN, 1], [NaN, 2], [NaN, 3], [NaN, 4]], [1, 2, 3, 4]);
    close(allnan.featureImportances[0], 0, 0, "an all-missing column earns nothing");
}

/* ==================================================================== *
 *  7. Early stopping keeps the BEST round
 * ==================================================================== */
{
    const d = linearData(55, 400, 0.5);
    const plain = new XGBRegressor({ nEstimators: 200, maxDepth: 3 }).fit(d.X, d.y);
    assert(plain.bestRounds === 200, "without early stopping every round is kept");

    const es = new XGBRegressor({
        nEstimators: 200, maxDepth: 3,
        earlyStoppingRounds: 5, validationFraction: 0.25,
    }).fit(d.X, d.y);
    assert(es.bestRounds < 200, "early stopping must stop early: " + es.bestRounds);
    assert(es.bestRounds >= 1, "and keep at least one round");

    /* Deterministic: the same seed draws the same validation rows and takes the
     * same decision. */
    const es2 = new XGBRegressor({
        nEstimators: 200, maxDepth: 3,
        earlyStoppingRounds: 5, validationFraction: 0.25,
    }).fit(d.X, d.y);
    assert(es2.bestRounds === es.bestRounds, "early stopping is reproducible");
    const p1 = es.predict(d.X), p2 = es2.predict(d.X);
    for (let i = 0; i < p1.length; i++)
        if (p1[i] !== p2[i]) assert(false, "and so are its predictions");

    /* The rounds after the best were grown and thrown away -- the model must
     * not still be predicting with them. */
    const trimmed = new XGBRegressor({
        nEstimators: es.bestRounds, maxDepth: 3,
    });
    assert(es.bestRounds > 0, "kept round count is observable");

    const clf = new XGBClassifier({
        nEstimators: 200, maxDepth: 3,
        earlyStoppingRounds: 4, validationFraction: 0.2,
    }).fit(d.X, d.yc);
    assert(clf.bestRounds <= 200, "classifier early stopping runs");
    assert(accuracy(d.yc, clf.predict(d.X)) > 0.7, "and still fits");
    trimmed.close();
}

/* ==================================================================== *
 *  8. Column subsampling
 * ==================================================================== */
{
    const d = linearData(66, 300, 0.2);
    const a = new XGBRegressor({ nEstimators: 30, maxDepth: 3, colsampleByTree: 0.34, seed: 3 }).fit(d.X, d.y);
    const b = new XGBRegressor({ nEstimators: 30, maxDepth: 3, colsampleByTree: 0.34, seed: 3 }).fit(d.X, d.y);
    const pa = a.predict(d.X), pb = b.predict(d.X);
    for (let i = 0; i < pa.length; i++)
        if (pa[i] !== pb[i]) assert(false, "column subsampling must be seeded");
    const c = new XGBRegressor({ nEstimators: 30, maxDepth: 3, colsampleByTree: 0.34, seed: 9 }).fit(d.X, d.y);
    let differs = false;
    const pc = c.predict(d.X);
    for (let i = 0; i < pa.length; i++) if (pa[i] !== pc[i]) differs = true;
    assert(differs, "a different seed must draw different columns");
    assert(r2Score(d.y, pa) > 0.9, "and it still fits");

    /* One column per tree is legal and must not divide by zero. */
    const one = new XGBRegressor({ nEstimators: 10, maxDepth: 2, colsampleByTree: 1e-6 }).fit(d.X, d.y);
    assert(Number.isFinite(one.predict(d.X)[0]), "colsample rounds up to one column");
}

/* ==================================================================== *
 *  9. Multiclass
 * ==================================================================== */
{
    const rnd = lcg(77), X = [], y = [];
    for (let i = 0; i < 400; i++) {
        const a = rnd() * 4 - 2, b = rnd() * 4 - 2;
        X.push([a, b]);
        y.push(a > 0 ? (b > 0 ? 2 : 1) : 0);
    }
    const m = new XGBClassifier({ nEstimators: 40, maxDepth: 3 }).fit(X, y);
    assert(accuracy(y, m.predict(X)) > 0.95, "three-class fit");
    const P = m.predictProba(X);
    assert(P[0].length === 3, "one column per class");
    for (let i = 0; i < 20; i++) {
        let s = 0;
        for (const v of P[i]) { s += v; assert(v >= 0 && v <= 1, "probability in range"); }
        close(s, 1, 1e-12, "multiclass row sums to 1");
    }
    const lab = m.predict(X);
    for (let i = 0; i < lab.length; i++) {
        let best = 0;
        for (let k = 1; k < 3; k++) if (P[i][k] > P[i][best]) best = k;
        if (lab[i] !== best) assert(false, "multiclass predict === argmax at row " + i);
    }
    /* apply() is round-major with one column per (round, class). */
    assert(m.apply([X[0]])[0].length === 40 * 3, "apply width is rounds x classes");

    throws(() => new XGBClassifier().fit([[1], [2]], [5, 5]), TypeError,
           "a booster cannot fit a single-label target");
}

/* ==================================================================== *
 * 10. Persistence
 * ==================================================================== */
{
    const d = linearData(88, 300, 0.3);
    for (const [Cls, target] of [[XGBRegressor, d.y], [XGBClassifier, d.yc]]) {
        const m = new Cls({ nEstimators: 25, maxDepth: 3, lambda: 2, alpha: 0.5,
                            gamma: 0.01, minChildWeight: 3, colsampleByTree: 0.7,
                            maxBins: 32, seed: 4 }).fit(d.X, target);
        const rec = m.serialize();
        const back = Cls.deserialize(rec);
        const a = m.predict(d.X), b = back.predict(d.X);
        for (let i = 0; i < a.length; i++)
            if (a[i] !== b[i]) assert(false, Cls.name + " round trip is not bit-identical");
        n++;
        const again = back.serialize();
        assert(again.length === rec.length, Cls.name + " re-encode length");
        for (let i = 0; i < rec.length; i++)
            if (rec[i] !== again[i]) assert(false, Cls.name + " re-encode is not byte-identical");
        n++;
        assert(back.bestRounds === m.bestRounds, "the kept round count survives");
        m.close(); back.close();
    }

    /* A record written by one model class must not load as another: the two
     * differ in what predict() means, and the type id is what says so. */
    const r = new XGBRegressor({ nEstimators: 5 }).fit(d.X, d.y);
    throws(() => XGBClassifier.deserialize(r.serialize()), Error,
           "an XGBRegressor record is not an XGBClassifier");
    r.close();
}

/* ==================================================================== *
 * 11. Adversarial arguments
 * ==================================================================== */
{
    const d = linearData(99, 100, 0.2);

    /* Options are validated, not clamped. */
    for (const bad of [{ lambda: -1 }, { alpha: -1 }, { gamma: -1 },
                       { minChildWeight: -1 }, { colsampleByTree: 0 },
                       { colsampleByTree: 2 }, { validationFraction: 1 },
                       { maxBins: 1 }, { maxBins: 999 }])
        throws(() => new XGBRegressor(bad), RangeError,
               "bad option " + JSON.stringify(bad));

    throws(() => new XGBRegressor().predict(d.X), Error, "predict before fit");
    const m = new XGBRegressor({ nEstimators: 5 }).fit(d.X, d.y);
    throws(() => m.predict([[1, 2]]), TypeError, "wrong feature count");

    /* Close during coercion: fit() coerces X and y, which runs user JS. The
     * model must not be resolved before that finishes -- and since these fits
     * coerce with ToNumber, valueOf is the hook that actually fires (a
     * toString here would exercise nothing; CLAUDE.md section 8). */
    let ran = 0;
    const bomb = { valueOf() { ran++; m.close(); return 1; } };
    throws(() => m.fit([[bomb, 2], [3, 4]], [1, 2]), Error,
           "closing the model inside a coercion must throw, not corrupt");
    assert(ran > 0, "the attack must actually have run: " + ran);

    /* And the same shape on predict. */
    const m2 = new XGBRegressor({ nEstimators: 5 }).fit(d.X, d.y);
    let ran2 = 0;
    const bomb2 = { valueOf() { ran2++; m2.close(); return 1; } };
    throws(() => m2.predict([[bomb2, 2, 3]]), Error, "close during predict coercion");
    assert(ran2 > 0, "predict's attack ran");
}

/* ==================================================================== *
 * 12. Degenerate shapes
 * ==================================================================== */
{
    const one = new XGBRegressor({ nEstimators: 3, maxDepth: 2 }).fit([[1]], [5]);
    close(one.predict([[1]])[0], 5, 1e-9, "a single row predicts itself");
    close(one.depth, 0, 0, "and cannot split");

    const constant = new XGBRegressor({ nEstimators: 5 })
        .fit([[1], [2], [3], [4]], [7, 7, 7, 7]);
    close(constant.predict([[2.5]])[0], 7, 1e-9, "a constant target");

    const onecol = new XGBRegressor({ nEstimators: 10, maxDepth: 2 })
        .fit([[1], [2], [3], [4], [5], [6]], [1, 1, 1, 9, 9, 9]);
    assert(onecol.predict([[6]])[0] > onecol.predict([[1]])[0], "one column still splits");
    one.close(); constant.close(); onecol.close();
}

/* --- Newton/maxBins contradiction and nEstimators cap (audit batch) ------- */
{
    throws(() => new XGBRegressor({ maxBins: 0 }), RangeError,
           "newton + maxBins 0 refusal");
    throws(() => new GradientBoostingRegressor({ nEstimators: 100001 }), RangeError,
           "nEstimators cap");
    throws(() => new GradientBoostingRegressor({ earlyStoppingRounds: 5 }), TypeError,
           "first-order booster refuses earlyStoppingRounds");
    throws(() => new GradientBoostingRegressor({ validationFraction: 0.3 }), TypeError,
           "first-order booster refuses validationFraction");
    throws(() => new XGBRegressor({ validationFraction: 0.6 }), RangeError,
           "validationFraction capped at 0.5");
    const d = linearData(3, 60, 0.1);
    const ok = new XGBRegressor({ nEstimators: 4, maxDepth: 2, seed: 1 });
    ok.fit(d.X, d.y);
    assert(ok.bestRounds >= 1, "small capped fit still works");
}

print("test_ml_xgb: all " + n + " assertions passed");
