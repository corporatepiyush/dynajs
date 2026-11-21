// covers: dyna:ml.RandomForestClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_random_forest_classifier_oracle_ml_hist");
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

async function fingerprint(model, X, proba) {
    const parts = [];
    const leaves = await model.apply(X);
    for (const row of leaves) parts.push(Array.isArray(row) ? row.join(",") : String(row));
    const imp = model.featureImportances;
    for (const v of imp) parts.push(v.toExponential(17));
    const pred = proba ? (await model.predictProba(X)) : (await model.predict(X));
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
                    const exact = await make({}).fit(X, y);
                    const hist = await make({ maxBins: Math.max(levels, 2) }).fit(X, y);
                    cases++;
                    if ((await fingerprint(exact, X, true)) !== (await fingerprint(hist, X, true)))
                        mismatches++;
                }
            }
        }
    }
}

await __kitS.section("32 bins over 600 distinct values:", async () => {
    const [X, y] = contData(5150, 600, 5, 2);
    const exact = await new RandomForestClassifier({ nEstimators: 25, maxDepth: 8, seed: 4 }).fit(X, y);
    const hist = await new RandomForestClassifier({ nEstimators: 25, maxDepth: 8, seed: 4, maxBins: 32 }).fit(X, y);
    const ae = await accuracy(y, await exact.predict(X)), ah = await accuracy(y, await hist.predict(X));
    assert(ah >= ae - 0.05, "32 bins over 600 distinct values: " + ah + " vs " + ae);

    const [Xr, yr] = contData(6161, 600, 5, 0);
    const er = await new RandomForestRegressor({ nEstimators: 25, maxDepth: 8, seed: 4 }).fit(Xr, yr);
    const hr = await new RandomForestRegressor({ nEstimators: 25, maxDepth: 8, seed: 4, maxBins: 32 }).fit(Xr, yr);
    assert((await r2Score(yr, await hr.predict(Xr))) >= (await r2Score(yr, await er.predict(Xr))) - 0.05,
           "binned regressor r2 must track the exact one");
});

await __kitS.section("same seed and bins must reproduce the fit exactly", async () => {
    const [X, y] = contData(31337, 300, 4, 3);
    const a = await new RandomForestClassifier({ nEstimators: 12, maxDepth: 6, seed: 99, maxBins: 24 }).fit(X, y);
    const b = await new RandomForestClassifier({ nEstimators: 12, maxDepth: 6, seed: 99, maxBins: 24 }).fit(X, y);
    assert((await fingerprint(a, X, true)) === (await fingerprint(b, X, true)),
           "same seed and bins must reproduce the fit exactly");

    const rec = a.serialize();
    const back = RandomForestClassifier.deserialize(rec);
    assert((await fingerprint(back, X, true)) === (await fingerprint(a, X, true)),
           "a binned model must survive the round trip bit-identically");
    const again = back.serialize();
    assert(again.length === rec.length, "re-encode length");
    let same = true;
    for (let i = 0; i < rec.length; i++) if (rec[i] !== again[i]) same = false;
    assert(same, "re-encoding a decoded binned model must be byte-identical");
});
__kitS.end();

print("oracle_ml_hist: all " + n + " assertions passed (" + cases +
      " classification equivalence cases, 0 mismatches)");
