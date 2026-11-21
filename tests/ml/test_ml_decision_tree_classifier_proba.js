// covers: dyna:ml.DecisionTreeClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_classifier_proba");
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

await __kitS.section("one row per input", async () => {
    for (const model of [new DecisionTreeClassifier({ maxDepth: 4 }),
                         new RandomForestClassifier({ nEstimators: 20, maxDepth: 5, seed: 1 })]) {
        const name = model.constructor.name;
        await model.fit(X, y);
        const P = await model.predictProba(Xt);
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
});

await __kitS.section("a single tree produced", async () => {
    const dt = new DecisionTreeClassifier({ maxDepth: 3 });
    await dt.fit(X, y);
    const P = await dt.predictProba(X);
    let fractional = 0;
    for (const row of P)
        if (row[0] > 0 && row[0] < 1) fractional++;
    assert(fractional > X.length / 2,
           "a single tree produced " + fractional + " fractional rows of " +
           X.length + "; a vote fraction would produce none");

    const levels = new Set(P.map(r => r[1].toFixed(12)));
    assert(levels.size > 2,
           "only " + levels.size + " distinct probability levels");
});

await __kitS.section("one weight per feature", async () => {
    for (const model of [new DecisionTreeClassifier({ maxDepth: 6 }),
                         new RandomForestClassifier({ nEstimators: 30, seed: 3 }),
                         new RandomForestRegressor({ nEstimators: 30, seed: 3 }),
                         new GradientBoostingRegressor({ nEstimators: 20, seed: 3 })]) {
        const name = model.constructor.name;
        await model.fit(X, y);
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
    await flat.fit(X, y);
    const fi = flat.featureImportances;
    assert(fi.length === 3 && fi.every(v => v === 0),
           "an unsplit tree reports zeros, not a uniform guess: " + JSON.stringify(fi));
});

await __kitS.section("importances/apply/predict changed across a round trip", async () => {
    for (const build of [() => new DecisionTreeClassifier({ maxDepth: 5 }),
                         () => new RandomForestClassifier({ nEstimators: 12, seed: 7 }),
                         () => new RandomForestRegressor({ nEstimators: 6, seed: 7 }),
                         () => new GradientBoostingRegressor({ nEstimators: 8, seed: 7 })]) {
        const m = build();
        await m.fit(X, y);
        const name = m.constructor.name;
        const C = m.constructor;
        const want = JSON.stringify([
            m.classifier === false ? null : null,
            m.featureImportances,
            await m.apply(Xt),
            await m.predict(Xt),
        ]);
        const wantProba = name.indexOf("Classifier") >= 0
            ? JSON.stringify(await m.predictProba(Xt)) : null;

        const back = C.deserialize(m.serialize());
        assert(JSON.stringify([null, back.featureImportances, await back.apply(Xt),
                               await back.predict(Xt)]) === want,
               name + ": importances/apply/predict changed across a round trip");
        if (wantProba !== null)
            assert(JSON.stringify(await back.predictProba(Xt)) === wantProba,
                   name + ": predictProba changed across a round trip");
    }
});
__kitS.end();

print("test_ml_tree_proba: all " + n + " assertions passed");
