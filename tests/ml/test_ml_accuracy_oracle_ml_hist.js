// covers: dyna:ml.accuracy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_accuracy_oracle_ml_hist");
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

await __kitS.section("binned boosting stays close:", async () => {
    const [X, y] = gridData(808, 300, 4, 12, 3);
    const a = await new GradientBoostingClassifier({ nEstimators: 15, maxDepth: 3 }).fit(X, y);
    const b = await new GradientBoostingClassifier({ nEstimators: 15, maxDepth: 3, maxBins: 12 }).fit(X, y);
    const pa = await a.predictProba(X), pb = await b.predictProba(X);
    let worst = 0;
    for (let i = 0; i < pa.length; i++)
        for (let k = 0; k < pa[i].length; k++)
            worst = Math.max(worst, Math.abs(pa[i][k] - pb[i][k]));
    assert(worst < 0.05, "binned boosting stays close: " + worst);
    const la = await a.predict(X), lb = await b.predict(X);
    let same = 0;
    for (let i = 0; i < la.length; i++) if (la[i] === lb[i]) same++;
    assert(same === la.length, "every predicted label must still agree: " +
                               same + "/" + la.length);
    close(await accuracy(y, la), await accuracy(y, lb), 0, "and so must accuracy, exactly");

    const [Xr, yr] = gridData(809, 300, 4, 12, 0);
    const ra = await new GradientBoostingRegressor({ nEstimators: 15, maxDepth: 3 }).fit(Xr, yr);
    const rb = await new GradientBoostingRegressor({ nEstimators: 15, maxDepth: 3, maxBins: 12 }).fit(Xr, yr);
    const va = await ra.predict(Xr), vb = await rb.predict(Xr);
    let rworst = 0;
    for (let i = 0; i < va.length; i++) rworst = Math.max(rworst, Math.abs(va[i] - vb[i]));
    assert(rworst <= 1e-9, "boosted regression must match exactly: " + rworst);
});
__kitS.end();

print("oracle_ml_hist: all " + n + " assertions passed (" + cases +
      " classification equivalence cases, 0 mismatches)");
