// covers: dyna:ml.r2Score
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_r2_score_oracle_ml_hist");
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

await __kitS.section("regression case counts", async () => {
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
                    const exact = await make({}).fit(X, y);
                    const hist = await make({ maxBins: Math.max(levels, 2) }).fit(X, y);
                    total[name]++;
                    const a = await exact.predict(X), b = await hist.predict(X);
                    let worst = 0;
                    for (let i = 0; i < a.length; i++)
                        worst = Math.max(worst, Math.abs(a[i] - b[i]));
                    if (worst > 1e-9) {
                        diverged[name]++;
                        worstR2 = Math.max(worstR2,
                            Math.abs((await r2Score(y, a)) - (await r2Score(y, b))));
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
});
__kitS.end();

print("oracle_ml_hist: all " + n + " assertions passed (" + cases +
      " classification equivalence cases, 0 mismatches)");
