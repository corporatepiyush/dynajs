import {
    DecisionTreeClassifier, DecisionTreeRegressor,
    RandomForestClassifier, RandomForestRegressor, GradientBoostingRegressor,
    logLoss, rocAuc, averagePrecision, accuracy,
} from "dyna:ml";

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
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

const rnd = lcg(31337);
const X = [], y = [];
for (let i = 0; i < 400; i++) {
    const a = rnd() * 4 - 2, b = rnd() * 4 - 2, noise = rnd() * 4 - 2;
    const p = 1 / (1 + Math.exp(-(a + 2 * b)));
    X.push([a, b, noise]);
    y.push(rnd() < p ? 1 : 0);
}
const Xt = X.slice(0, 60), yt = y.slice(0, 60);

{
    for (const model of [new DecisionTreeClassifier({ maxDepth: 4 }),
                         new RandomForestClassifier({ nEstimators: 20, maxDepth: 5, seed: 1 })]) {
        const name = model.constructor.name;
        model.fit(X, y);
        const P = model.predictProba(Xt);
        assert(P.length === Xt.length, name + " one row per input");
        assert(P[0].length === 2, name + " one column per class");
        for (let i = 0; i < P.length; i++) {
            let s = 0;
            for (const v of P[i]) {
                assert(v >= 0 && v <= 1, name + " probability out of range: " + v);
                s += v;
            }
            close(s, 1, 1e-12, name + " row " + i + " does not sum to 1");
        }
    }
}

{
    const dt = new DecisionTreeClassifier({ maxDepth: 5 });
    dt.fit(X, y);
    const P1 = dt.predictProba(X), pred1 = dt.predict(X);
    for (let i = 0; i < X.length; i++)
        if (P1[i][0] !== P1[i][1])
            assert(pred1[i] === (P1[i][0] > P1[i][1] ? 0 : 1),
                   "a single tree's vote IS its leaf argmax, row " + i);

    const rf = new RandomForestClassifier({ nEstimators: 20, maxDepth: 6, seed: 1 });
    rf.fit(X, y);
    const P = rf.predictProba(X), pred = rf.predict(X);
    let disagree = 0;
    for (let i = 0; i < X.length; i++)
        if (pred[i] !== (P[i][0] >= P[i][1] ? 0 : 1)) disagree++;
    assert(disagree === 0,
           "predict() must BE argmax(predictProba); " + disagree + "/" +
           X.length + " rows disagree, so predict() has reverted to a vote");

    const rf100 = new RandomForestClassifier({ nEstimators: 100, maxDepth: 6, seed: 1 });
    rf100.fit(X, y);
    const P100 = rf100.predictProba(X), pred100 = rf100.predict(X);
    let d100 = 0;
    for (let i = 0; i < X.length; i++)
        if (pred100[i] !== (P100[i][0] >= P100[i][1] ? 0 : 1)) d100++;
    assert(d100 === 0, "still argmax at 100 trees (" + d100 + " disagreements)");
    print("  predict() === argmax(predictProba) at 20 and 100 trees (D7)");
}

{
    const dt = new DecisionTreeClassifier({ maxDepth: 3 });
    dt.fit(X, y);
    const P = dt.predictProba(X);
    let fractional = 0;
    for (const row of P)
        if (row[0] > 0 && row[0] < 1) fractional++;
    assert(fractional > X.length / 2,
           "a single tree produced " + fractional + " fractional rows of " +
           X.length + "; a vote fraction would produce none");

    const levels = new Set(P.map(r => r[1].toFixed(12)));
    assert(levels.size > 2,
           "only " + levels.size + " distinct probability levels");
}

{
    const rf = new RandomForestClassifier({ nEstimators: 40, maxDepth: 6, seed: 2 });
    rf.fit(X, y);
    const score = rf.predictProba(Xt).map(r => r[1]);
    const ll = logLoss(yt, score);
    const auc = rocAuc(yt, score);
    const ap = averagePrecision(yt, score);
    assert(ll > 0 && isFinite(ll), "logLoss is finite: " + ll);
    assert(auc > 0.8 && auc <= 1, "rocAuc on separable-ish data: " + auc);
    assert(ap > 0.8 && ap <= 1, "averagePrecision: " + ap);
    assert(accuracy(yt, rf.predict(Xt)) > 0.8, "accuracy sanity");
}

{
    for (const model of [new DecisionTreeClassifier({ maxDepth: 6 }),
                         new RandomForestClassifier({ nEstimators: 30, seed: 3 }),
                         new RandomForestRegressor({ nEstimators: 30, seed: 3 }),
                         new GradientBoostingRegressor({ nEstimators: 20, seed: 3 })]) {
        const name = model.constructor.name;
        model.fit(X, y);
        const imp = model.featureImportances;
        assert(imp.length === 3, name + " one weight per feature");
        let s = 0;
        for (const v of imp) {
            assert(v >= 0, name + " importance is non-negative");
            s += v;
        }
        close(s, 1, 1e-12, name + " importances sum to 1");
        assert(imp[1] > imp[0], name + ": b (weight 2) should outrank a");
        assert(imp[0] > imp[2], name + ": a should outrank pure noise");
        assert(imp[2] < imp[1] / 2, name + ": noise ranked too close to b");
    }

    const flat = new DecisionTreeClassifier({ maxDepth: 1, minSamplesSplit: 1e9 });
    flat.fit(X, y);
    const fi = flat.featureImportances;
    assert(fi.length === 3 && fi.every(v => v === 0),
           "an unsplit tree reports zeros, not a uniform guess: " + JSON.stringify(fi));
}

{
    const rf = new RandomForestClassifier({ nEstimators: 7, maxDepth: 4, seed: 4 });
    rf.fit(X, y);
    const A = rf.apply(Xt);
    assert(A.length === Xt.length, "one row per input");
    assert(A[0].length === 7, "one column per tree");
    for (const row of A)
        for (const v of row)
            assert(Number.isInteger(v) && v >= 0, "a leaf index: " + v);

    const twice = rf.apply(Xt);
    assert(JSON.stringify(A) === JSON.stringify(twice), "apply is deterministic");
    const dup = rf.apply([Xt[0], Xt[0]]);
    assert(JSON.stringify(dup[0]) === JSON.stringify(dup[1]),
           "identical rows land in identical leaves");
    assert(JSON.stringify(dup[0]) === JSON.stringify(A[0]),
           "and in the same leaves as in the bigger batch");

    const dt = new DecisionTreeRegressor({ maxDepth: 3 });
    dt.fit(X, y);
    assert(dt.apply(Xt)[0].length === 1, "a single tree has one column");
}

{
    const reg = new RandomForestRegressor({ nEstimators: 3, seed: 5 });
    reg.fit(X, y);
    throws(() => reg.predictProba(Xt), TypeError,
           "predictProba on a regressor");
    const gb = new GradientBoostingRegressor({ nEstimators: 3, seed: 5 });
    gb.fit(X, y);
    throws(() => gb.predictProba(Xt), TypeError,
           "predictProba on a boosted regressor");

    const unfit = new RandomForestClassifier({ nEstimators: 3 });
    throws(() => unfit.predictProba(Xt), Error, "predictProba before fit");
    throws(() => unfit.apply(Xt), Error, "apply before fit");
    throws(() => unfit.featureImportances, Error, "featureImportances before fit");

    const fitted = new RandomForestClassifier({ nEstimators: 3, seed: 6 });
    fitted.fit(X, y);
    throws(() => fitted.predictProba([[1, 2]]), TypeError, "wrong feature count");
    throws(() => fitted.apply([[1, 2]]), TypeError, "wrong feature count for apply");
}

{
    for (const build of [() => new DecisionTreeClassifier({ maxDepth: 5 }),
                         () => new RandomForestClassifier({ nEstimators: 12, seed: 7 }),
                         () => new RandomForestRegressor({ nEstimators: 6, seed: 7 }),
                         () => new GradientBoostingRegressor({ nEstimators: 8, seed: 7 })]) {
        const m = build();
        m.fit(X, y);
        const name = m.constructor.name;
        const C = m.constructor;
        const want = JSON.stringify([
            m.classifier === false ? null : null,
            m.featureImportances,
            m.apply(Xt),
            m.predict(Xt),
        ]);
        const wantProba = name.indexOf("Classifier") >= 0
            ? JSON.stringify(m.predictProba(Xt)) : null;

        const back = C.deserialize(m.serialize());
        assert(JSON.stringify([null, back.featureImportances, back.apply(Xt),
                               back.predict(Xt)]) === want,
               name + ": importances/apply/predict changed across a round trip");
        if (wantProba !== null)
            assert(JSON.stringify(back.predictProba(Xt)) === wantProba,
                   name + ": predictProba changed across a round trip");
    }
}

print("test_ml_tree_proba: all " + n + " assertions passed");
