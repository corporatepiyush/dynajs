// covers: dyna:ml.RandomForestClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_random_forest_classifier_tree_proba");
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
async function throws(fn, kind, m) {
    n++;
    try { await fn(); } catch (e) {
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

await __kitS.section("a single tree's vote IS its leaf argmax, row", async () => {
    const dt = new DecisionTreeClassifier({ maxDepth: 5 });
    await dt.fit(X, y);
    const P1 = await dt.predictProba(X), pred1 = await dt.predict(X);
    for (let i = 0; i < X.length; i++)
        if (P1[i][0] !== P1[i][1])
            assert(pred1[i] === (P1[i][0] > P1[i][1] ? 0 : 1),
                   "a single tree's vote IS its leaf argmax, row " + i);

    const rf = new RandomForestClassifier({ nEstimators: 20, maxDepth: 6, seed: 1 });
    await rf.fit(X, y);
    const P = await rf.predictProba(X), pred = await rf.predict(X);
    let disagree = 0;
    for (let i = 0; i < X.length; i++)
        if (pred[i] !== (P[i][0] >= P[i][1] ? 0 : 1)) disagree++;
    assert(disagree === 0,
           "predict() must BE argmax(predictProba); " + disagree + "/" +
           X.length + " rows disagree, so predict() has reverted to a vote");

    const rf100 = new RandomForestClassifier({ nEstimators: 100, maxDepth: 6, seed: 1 });
    await rf100.fit(X, y);
    const P100 = await rf100.predictProba(X), pred100 = await rf100.predict(X);
    let d100 = 0;
    for (let i = 0; i < X.length; i++)
        if (pred100[i] !== (P100[i][0] >= P100[i][1] ? 0 : 1)) d100++;
    assert(d100 === 0, "still argmax at 100 trees (" + d100 + " disagreements)");
    print("  predict() === argmax(predictProba) at 20 and 100 trees (D7)");
});

await __kitS.section("one row per input", async () => {
    const rf = new RandomForestClassifier({ nEstimators: 7, maxDepth: 4, seed: 4 });
    await rf.fit(X, y);
    const A = await rf.apply(Xt);
    assert(A.length === Xt.length, "one row per input");
    assert(A[0].length === 7, "one column per tree");
    for (const row of A)
        for (const v of row)
            assert(Number.isInteger(v) && v >= 0, "a leaf index: " + v);

    const twice = await rf.apply(Xt);
    assert(JSON.stringify(A) === JSON.stringify(twice), "apply is deterministic");
    const dup = await rf.apply([Xt[0], Xt[0]]);
    assert(JSON.stringify(dup[0]) === JSON.stringify(dup[1]),
           "identical rows land in identical leaves");
    assert(JSON.stringify(dup[0]) === JSON.stringify(A[0]),
           "and in the same leaves as in the bigger batch");

    const dt = new DecisionTreeRegressor({ maxDepth: 3 });
    await dt.fit(X, y);
    assert((await dt.apply(Xt))[0].length === 1, "a single tree has one column");
});

await __kitS.section("predictProba on a regressor", async () => {
    const reg = new RandomForestRegressor({ nEstimators: 3, seed: 5 });
    await reg.fit(X, y);
    await throws(async () => await reg.predictProba(Xt), TypeError,
           "predictProba on a regressor");
    const gb = new GradientBoostingRegressor({ nEstimators: 3, seed: 5 });
    await gb.fit(X, y);
    await throws(async () => await gb.predictProba(Xt), TypeError,
           "predictProba on a boosted regressor");

    const unfit = new RandomForestClassifier({ nEstimators: 3 });
    await throws(async () => await unfit.predictProba(Xt), Error, "predictProba before fit");
    await throws(async () => await unfit.apply(Xt), Error, "apply before fit");
    await throws(() => unfit.featureImportances, Error, "featureImportances before fit");

    const fitted = new RandomForestClassifier({ nEstimators: 3, seed: 6 });
    await fitted.fit(X, y);
    await throws(async () => await fitted.predictProba([[1, 2]]), TypeError, "wrong feature count");
    await throws(async () => await fitted.apply([[1, 2]]), TypeError, "wrong feature count for apply");
});
__kitS.end();

print("test_ml_tree_proba: all " + n + " assertions passed");
