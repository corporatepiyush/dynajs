// covers: dyna:ml.RandomForestClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_random_forest_classifier_review_esc_kernel");
import { CSR, LinearRegression, LogisticRegression, KMeans, GaussianNB,
         DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor, KNClassifier, KNRegressor, SVC,
         GaussianMixture } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const flat = (a) => Array.from(a).flat(9);
const same = (a, b) => { const A = flat(a), B = flat(b);
    return A.length === B.length && A.every((v, i) => Object.is(v, B[i])); };

function dupSplit(v, k) {
    const out = [v / Math.pow(2, k - 1), v / Math.pow(2, k - 1)];
    for (let j = k - 2; j >= 1; j--) out.push(v / Math.pow(2, j));
    return out;
}
function dupRow(r, ks) {
    const val = [], col = [], ptr = [0];
    for (let c = 0; c < r.length; c++)
        for (const p of dupSplit(r[c], ks[c])) { val.push(p); col.push(c); }
    ptr.push(val.length);
    return new CSR(val, col, ptr, r.length);
}

const coefSets = [
    [1e16, 1, -1e16],
    [1, 1e-16, 1e-17],
    [1e300, -1e300, 1],
    [0.1, 0.2, 0.30000000000000004],
    [3, 5, 7],
    [1e8, -1e-8, 2],
    [-1, 2, -3],
    [Math.PI, Math.E, 1 / 3],
    [1e-300, 1e-300, 1e-300],
    [5, 5, 5],
];
const readings = [
    [1, 1e-16, 1],
    [1e16, 1, -1e16],
    [0.1, 3, 7],
    [1e8, -1e-8, 2],
    [2.3, -1.25, 0.75],
    [1e300, 1e300, -1e300],
    [Math.PI, Math.E, 1 / 3],
    [-1, 2, -3],
    [1e-300, 1e-300, 1e-300],
    [5, 5, 5],
];

await __kitS.section("E2 every family both forms x 2/3/5/10/12-dup queries Object.is", async () => {
    const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];
    const models = [];
    models.push(["LinearRegression.predict", await new LinearRegression().fit(Xt, yr), async (m, X) => await m.predict(X)]);
    {
        const m = await new LogisticRegression({ maxIter: 400 }).fit(Xt, yt);
        models.push(["LogisticRegression.predict", m, async (m, X) => await m.predict(X)]);
        models.push(["LogisticRegression.predictProba", m, async (m, X) => await m.predictProba(X)]);
    }
    models.push(["KMeans.predict", await new KMeans(3, 7).fit(Xt), async (m, X) => await m.predict(X)]);
    models.push(["DecisionTreeClassifier.predict", await new DecisionTreeClassifier({ seed: 1 }).fit(Xt, yt), async (m, X) => await m.predict(X)]);
    models.push(["DecisionTreeRegressor.predict", await new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr), async (m, X) => await m.predict(X)]);
    models.push(["RandomForestClassifier.predict", await new RandomForestClassifier({ nEstimators: 4, seed: 2 }).fit(Xt, yt), async (m, X) => await m.predict(X)]);
    {
        const m = await new RandomForestClassifier({ nEstimators: 4, seed: 2 }).fit(Xt, yt);
        models.push(["RandomForestClassifier.predictProba", m, async (m, X) => await m.predictProba(X)]);
    }
    models.push(["RandomForestRegressor.predict", await new RandomForestRegressor({ nEstimators: 4, seed: 2 }).fit(Xt, yr), async (m, X) => await m.predict(X)]);
    models.push(["GradientBoostingClassifier.predict", await new GradientBoostingClassifier({ nEstimators: 4, seed: 3 }).fit(Xt, yt), async (m, X) => await m.predict(X)]);
    models.push(["GradientBoostingRegressor.predict", await new GradientBoostingRegressor({ nEstimators: 4, seed: 3 }).fit(Xt, yr), async (m, X) => await m.predict(X)]);
    models.push(["XGBClassifier.predict", await new XGBClassifier({ nEstimators: 4, seed: 4 }).fit(Xt, yt), async (m, X) => await m.predict(X)]);
    models.push(["XGBRegressor.predict", await new XGBRegressor({ nEstimators: 4, seed: 4 }).fit(Xt, yr), async (m, X) => await m.predict(X)]);
    {
        const m = await new GaussianNB().fit(Xt, yt);
        models.push(["GaussianNB.predict", m, async (m, X) => await m.predict(X)]);
        models.push(["GaussianNB.predictProba", m, async (m, X) => await m.predictProba(X)]);
    }
    {
        const m = await new KNClassifier(3).fit(Xt, yt);
        models.push(["KNClassifier.predict", m, async (m, X) => await m.predict(X)]);
    }
    models.push(["KNRegressor.predict", await new KNRegressor(3).fit(Xt, yr), async (m, X) => await m.predict(X)]);
    {
        const m = await new SVC().fit(Xt, yt);
        models.push(["SVC.predict", m, async (m, X) => await m.predict(X)]);
        models.push(["SVC.decisionFunction", m, async (m, X) => await m.decisionFunction(X)]);
    }
    {
        const m = await new GaussianMixture(2, { seed: 6 }).fit(Xt);
        models.push(["GaussianMixture.predict", m, async (m, X) => await m.predict(X)]);
        models.push(["GaussianMixture.predictProba", m, async (m, X) => await m.predictProba(X)]);
    }
    let bad = [];
    for (const [name, m, fn] of models) {
        for (const k of [2, 3, 5, 10, 12]) {
            const S = dupRow([2.3, -1.25, 0.75], [k, k + 1, k + 2]), D = await S.toDense();
            if (!same(await fn(m, S), await fn(m, D))) bad.push(`${name} k=${k}`);
            S.close();
        }
    }
    ok(bad.length === 0, "E2 every family both forms x 2/3/5/10/12-dup queries Object.is",
       bad.slice(0, 5).join("; "));
    {
        const S = dupRow([2.3, -1.25, 0.75], [12, 13, 14]), D = await S.toDense();
        let ibad = [];
        for (const [name, m] of models) {
            if (typeof m.predictInto !== "function") continue;
            const a = new Float64Array(1), b = new Float64Array(1);
            try {
                await m.predictInto(a, S); await m.predictInto(b, D);
                if (!Object.is(a[0], b[0])) ibad.push(name);
            } catch (e) { ibad.push(name + " threw " + e.message); }
        }
        ok(ibad.length === 0, "E2b predictInto over a 12-dup CSR == dense Object.is", ibad.join("; "));
        S.close();
    }
    const seen = new Set();
    for (const [name, m] of models)
        if (typeof m.close === "function" && !seen.has(m)) { seen.add(m); try { m.close(); } catch (e) {} }
});
__kitS.end();
print("review_esc_kernel: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_kernel: " + fail + " failures");
