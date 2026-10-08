import {
    DecisionTreeClassifier, DecisionTreeRegressor,
    RandomForestClassifier, RandomForestRegressor,
    GradientBoostingRegressor, GradientBoostingClassifier,
    accuracy, r2Score,
} from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(a, b, eps, m) {
    n++;
    if (!(Math.abs(a - b) <= eps))
        throw new Error((m || "not close") + ": " + a + " vs " + b);
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function gridData(seed, rows, cols, levels, classes) {
    const rnd = lcg(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const row = [];
        let s = 0;
        for (let j = 0; j < cols; j++) {
            const v = Math.floor(rnd() * levels);
            row.push(v * 0.25 - 1);
            s += (j + 1) * v;
        }
        X.push(row);
        y.push(classes ? Math.floor((s + rnd() * levels) % classes)
                       : s + rnd() * 2 - 1);
    }
    return [X, y];
}

function contData(seed, rows, cols, classes) {
    const rnd = lcg(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const row = [];
        let s = 0;
        for (let j = 0; j < cols; j++) {
            const v = rnd() * 4 - 2;
            row.push(v);
            s += (j + 1) * v;
        }
        X.push(row);
        y.push(classes ? (1 / (1 + Math.exp(-s)) > rnd() ? 1 : 0) : s + rnd() * 0.2);
    }
    return [X, y];
}

function fingerprint(model, X, proba) {
    const parts = [];
    const leaves = model.apply(X);
    for (const row of leaves) parts.push(Array.isArray(row) ? row.join(",") : String(row));
    const imp = model.featureImportances;
    for (const v of imp) parts.push(v.toExponential(17));
    const pred = proba ? model.predictProba(X) : model.predict(X);
    for (const row of pred)
        parts.push(Array.isArray(row) ? row.map(v => v.toExponential(17)).join(",")
                                      : row.toExponential(17));
    parts.push("depth=" + model.depth);
    return parts.join("|");
}

let cases = 0, mismatches = 0;
for (const levels of [2, 5, 16, 40]) {
    for (const cols of [1, 3, 7]) {
        for (const seed of [1, 7, 4242]) {
            for (const classes of [2, 3]) {
                const [X, y] = gridData(seed, 240, cols, levels, classes);
                for (const make of [
                    o => new DecisionTreeClassifier({ maxDepth: 6, ...o }),
                    o => new DecisionTreeClassifier({ maxDepth: 0, minSamplesLeaf: 3, ...o }),
                    o => new RandomForestClassifier({ nEstimators: 8, maxDepth: 5, seed: 3, ...o }),
                ]) {
                    const exact = make({}).fit(X, y);
                    const hist = make({ maxBins: Math.max(levels, 2) }).fit(X, y);
                    cases++;
                    if (fingerprint(exact, X, true) !== fingerprint(hist, X, true))
                        mismatches++;
                }
            }
        }
    }
}
assert(cases === 216, "case count: " + cases);
assert(mismatches === 0, "classification equivalence: " + mismatches + "/" + cases + " differ");

{
    const [X, y] = gridData(11, 240, 3, 40, 3);
    const exact = new DecisionTreeClassifier({ maxDepth: 6 }).fit(X, y);
    const coarse = new DecisionTreeClassifier({ maxDepth: 6, maxBins: 2 }).fit(X, y);
    assert(fingerprint(exact, X, true) !== fingerprint(coarse, X, true),
           "maxBins:2 over 40 levels must NOT reproduce the exact tree");
}

{
    const makers = {
        tree:     o => new DecisionTreeRegressor({ maxDepth: 6, ...o }),
        boosting: o => new GradientBoostingRegressor({ nEstimators: 10, maxDepth: 3, ...o }),
        forest:   o => new RandomForestRegressor({ nEstimators: 8, maxDepth: 5, seed: 3, ...o }),
    };
    const diverged = { tree: 0, boosting: 0, forest: 0 };
    const total = { tree: 0, boosting: 0, forest: 0 };
    let worstR2 = 0;
    for (const levels of [2, 5, 16, 40]) {
        for (const cols of [1, 3, 7]) {
            for (const seed of [2, 9, 777]) {
                const [X, y] = gridData(seed, 240, cols, levels, 0);
                for (const [name, make] of Object.entries(makers)) {
                    const exact = make({}).fit(X, y);
                    const hist = make({ maxBins: Math.max(levels, 2) }).fit(X, y);
                    total[name]++;
                    const a = exact.predict(X), b = hist.predict(X);
                    let worst = 0;
                    for (let i = 0; i < a.length; i++)
                        worst = Math.max(worst, Math.abs(a[i] - b[i]));
                    if (worst > 1e-9) {
                        diverged[name]++;
                        worstR2 = Math.max(worstR2,
                            Math.abs(r2Score(y, a) - r2Score(y, b)));
                    }
                }
            }
        }
    }
    assert(total.tree === 36 && total.boosting === 36 && total.forest === 36,
           "regression case counts");
    assert(diverged.tree === 0, "single trees must agree exactly: " + diverged.tree);
    assert(diverged.boosting === 0, "boosting must agree exactly: " + diverged.boosting);
    assert(diverged.forest <= 6,
           "forest divergence is a few near-ties out of 36, saw " + diverged.forest);
    assert(diverged.forest > 0,
           "and it is not zero on this data -- a zero here means the two "
           + "finders stopped being compared at all");
    assert(worstR2 < 0.01,
           "a flipped near-tie must not change what the model is worth: " + worstR2);
}

{
    const [X, y] = contData(5150, 600, 5, 2);
    const exact = new RandomForestClassifier({ nEstimators: 25, maxDepth: 8, seed: 4 }).fit(X, y);
    const hist = new RandomForestClassifier({ nEstimators: 25, maxDepth: 8, seed: 4, maxBins: 32 }).fit(X, y);
    const ae = accuracy(y, exact.predict(X)), ah = accuracy(y, hist.predict(X));
    assert(ah >= ae - 0.05, "32 bins over 600 distinct values: " + ah + " vs " + ae);

    const [Xr, yr] = contData(6161, 600, 5, 0);
    const er = new RandomForestRegressor({ nEstimators: 25, maxDepth: 8, seed: 4 }).fit(Xr, yr);
    const hr = new RandomForestRegressor({ nEstimators: 25, maxDepth: 8, seed: 4, maxBins: 32 }).fit(Xr, yr);
    assert(r2Score(yr, hr.predict(Xr)) >= r2Score(yr, er.predict(Xr)) - 0.05,
           "binned regressor r2 must track the exact one");
}

{
    const [X, y] = contData(31337, 300, 4, 3);
    const a = new RandomForestClassifier({ nEstimators: 12, maxDepth: 6, seed: 99, maxBins: 24 }).fit(X, y);
    const b = new RandomForestClassifier({ nEstimators: 12, maxDepth: 6, seed: 99, maxBins: 24 }).fit(X, y);
    assert(fingerprint(a, X, true) === fingerprint(b, X, true),
           "same seed and bins must reproduce the fit exactly");

    const rec = a.serialize();
    const back = RandomForestClassifier.deserialize(rec);
    assert(fingerprint(back, X, true) === fingerprint(a, X, true),
           "a binned model must survive the round trip bit-identically");
    const again = back.serialize();
    assert(again.length === rec.length, "re-encode length");
    let same = true;
    for (let i = 0; i < rec.length; i++) if (rec[i] !== again[i]) same = false;
    assert(same, "re-encoding a decoded binned model must be byte-identical");
}

{
    const X = [], y = [];
    for (let i = 0; i < 60; i++) { X.push([1.5, i % 7, 0]); y.push(i % 2); }
    const m = new DecisionTreeClassifier({ maxDepth: 4, maxBins: 16 }).fit(X, y);
    assert(m.featureImportances[0] === 0, "a constant column earns no importance");
    assert(m.predict([[1.5, 3, 0]]).length === 1, "predicts");

    const tiny = new DecisionTreeRegressor({ maxBins: 8 }).fit([[1], [1]], [2, 2]);
    close(tiny.predict([[1]])[0], 2, 0, "constant target");
    const adj = Number.MIN_VALUE * 4;
    const near = new DecisionTreeRegressor({ maxBins: 8 })
        .fit([[adj], [adj * 2], [adj * 3], [adj * 4]], [0, 0, 1, 1]);
    assert(Number.isFinite(near.predict([[adj]])[0]), "adjacent doubles bin cleanly");
}

{
    let threw = 0;
    for (const bad of [1, 256, 100000]) {
        try { new DecisionTreeClassifier({ maxBins: bad }); } catch (e) { threw++; }
    }
    assert(threw === 3, "out-of-range maxBins must throw, not clamp");
    assert(new DecisionTreeClassifier({ maxBins: 0 }) instanceof DecisionTreeClassifier,
           "0 selects the exact splitter");
    assert(new DecisionTreeClassifier({ maxBins: 255 }) instanceof DecisionTreeClassifier,
           "255 is the maximum, because a bin code and its missing slot are one byte");
}

{
    const [X, y] = gridData(808, 300, 4, 12, 3);
    const a = new GradientBoostingClassifier({ nEstimators: 15, maxDepth: 3 }).fit(X, y);
    const b = new GradientBoostingClassifier({ nEstimators: 15, maxDepth: 3, maxBins: 12 }).fit(X, y);
    const pa = a.predictProba(X), pb = b.predictProba(X);
    let worst = 0;
    for (let i = 0; i < pa.length; i++)
        for (let k = 0; k < pa[i].length; k++)
            worst = Math.max(worst, Math.abs(pa[i][k] - pb[i][k]));
    assert(worst < 0.05, "binned boosting stays close: " + worst);
    const la = a.predict(X), lb = b.predict(X);
    let same = 0;
    for (let i = 0; i < la.length; i++) if (la[i] === lb[i]) same++;
    assert(same === la.length, "every predicted label must still agree: " +
                               same + "/" + la.length);
    close(accuracy(y, la), accuracy(y, lb), 0, "and so must accuracy, exactly");

    const [Xr, yr] = gridData(809, 300, 4, 12, 0);
    const ra = new GradientBoostingRegressor({ nEstimators: 15, maxDepth: 3 }).fit(Xr, yr);
    const rb = new GradientBoostingRegressor({ nEstimators: 15, maxDepth: 3, maxBins: 12 }).fit(Xr, yr);
    const va = ra.predict(Xr), vb = rb.predict(Xr);
    let rworst = 0;
    for (let i = 0; i < va.length; i++) rworst = Math.max(rworst, Math.abs(va[i] - vb[i]));
    assert(rworst <= 1e-9, "boosted regression must match exactly: " + rworst);
}

print("oracle_ml_hist: all " + n + " assertions passed (" + cases +
      " classification equivalence cases, 0 mismatches)");
