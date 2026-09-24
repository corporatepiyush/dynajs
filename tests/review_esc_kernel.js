/* review_esc_kernel.js -- permanent row of the review lane's esc_kernel.js
 * (verbatim behaviour; the predict-side kernel unification it pins is
 * sustained). Original header: kernel unification under
 * (a) 10 adversarial coefficient sets (extreme/cancellation magnitudes),
 * (b) rows with 10+ duplicate indices, (c) EVERY estimator family, both
 * forms, at Object.is across predict/predictProba/predictInto/decisionFunction.
 */
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

/* k-duplicate stack whose storage-order sum is bitwise v (exact doubling) */
function dupSplit(v, k) {
    const out = [v / Math.pow(2, k - 1), v / Math.pow(2, k - 1)];
    for (let j = k - 2; j >= 1; j--) out.push(v / Math.pow(2, j));
    return out;
}
/* query row of duplicate stacks, ks duplicates per column, reading `r` */
function dupRow(r, ks) {
    const val = [], col = [], ptr = [0];
    for (let c = 0; c < r.length; c++)
        for (const p of dupSplit(r[c], ks[c])) { val.push(p); col.push(c); }
    ptr.push(val.length);
    return new CSR(val, col, ptr, r.length);
}

/* (a) 10 adversarial coefficient sets: fit on exactly-forced systems so the
 * coef land near the wanted magnitudes (ridge 1e-9 shrinks slightly) */
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

{
    let bad = [];
    for (let s = 0; s < 10; s++) {
        const w = coefSets[s];
        /* exactly-determined-ish design: 4 rows, 3 cols + intercept */
        const X = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 1], [2, 1, 0]];
        const y = X.map(r => w[0] * r[0] + w[1] * r[1] + w[2] * r[2]);
        const mid = (Math.max(...y) + Math.min(...y)) / 2;
        let yc = y.map(v => (v > mid ? 1 : 0));
        if (yc.every(v => v === yc[0])) yc = y.map((_, i) => i % 2);
        const lin = new LinearRegression().fit(X, y);
        const log = new LogisticRegression({ maxIter: 500 }).fit(X, yc);
        for (const reading of [readings[s]]) {
            /* 10-, 11-, 12- and 16-duplicate stacks per column */
            for (const k of [10, 11, 12, 16]) {
                const ks = [k, k + 1, k + 2];
                const S = dupRow(reading, ks), D = S.toDense();
                if (!same(lin.predict(S), lin.predict(D)))
                    bad.push(`lin s=${s} k=${k} predict`);
                if (!same(log.predict(S), log.predict(D)))
                    bad.push(`log s=${s} k=${k} predict`);
                if (!same(log.predictProba(S), log.predictProba(D)))
                    bad.push(`log s=${s} k=${k} proba`);
                const a = new Float64Array(1), b = new Float64Array(1);
                lin.predictInto(a, S); lin.predictInto(b, D);
                if (!Object.is(a[0], b[0])) bad.push(`lin s=${s} k=${k} predictInto`);
                log.predictInto(a, S); log.predictInto(b, D);
                if (!Object.is(a[0], b[0])) bad.push(`log s=${s} k=${k} predictInto`);
                S.close();
            }
            /* raw adversarial dups (no exact split): 12 identical hostile entries */
            for (const v of [1, 1e-16, 1e16, 0.1, 1e300]) {
                const val = new Array(12).fill(v), col = new Array(12).fill(0);
                const S = new CSR(val, col, [0, 12], 3), D = S.toDense();
                if (!same(lin.predict(S), lin.predict(D))) bad.push(`lin raw ${v}`);
                if (!same(log.predictProba(S), log.predictProba(D))) bad.push(`log raw ${v}`);
                S.close();
            }
            /* [1, 1e-16] pairs + overflow-class duplicate sums */
            for (const pair of [[1, 1e-16], [1e300, 1e300, -1e300], [1e308, 1e308]]) {
                const S = new CSR(pair, pair.map(() => 0), [0, pair.length], 3), D = S.toDense();
                if (!same(lin.predict(S), lin.predict(D))) bad.push(`lin pair ${pair}`);
                if (!same(log.predictProba(S), log.predictProba(D))) bad.push(`log pair ${pair}`);
                S.close();
            }
        }
        lin.close(); log.close();
    }
    ok(bad.length === 0, "E1 10 adversarial coef sets x 10+ dups: lin/logreg sparse == dense Object.is",
       bad.slice(0, 4).join("; ") + (bad.length > 4 ? ` (+${bad.length - 4} more)` : ""));
}

/* (b)+(c) EVERY family, both forms, dup-heavy queries incl. 10+ dups */
{
    const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];
    const models = [];
    models.push(["LinearRegression.predict", new LinearRegression().fit(Xt, yr), (m, X) => m.predict(X)]);
    {
        const m = new LogisticRegression({ maxIter: 400 }).fit(Xt, yt);
        models.push(["LogisticRegression.predict", m, (m, X) => m.predict(X)]);
        models.push(["LogisticRegression.predictProba", m, (m, X) => m.predictProba(X)]);
    }
    models.push(["KMeans.predict", new KMeans(3, 7).fit(Xt), (m, X) => m.predict(X)]);
    models.push(["DecisionTreeClassifier.predict", new DecisionTreeClassifier({ seed: 1 }).fit(Xt, yt), (m, X) => m.predict(X)]);
    models.push(["DecisionTreeRegressor.predict", new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr), (m, X) => m.predict(X)]);
    models.push(["RandomForestClassifier.predict", new RandomForestClassifier({ nEstimators: 4, seed: 2 }).fit(Xt, yt), (m, X) => m.predict(X)]);
    {
        const m = new RandomForestClassifier({ nEstimators: 4, seed: 2 }).fit(Xt, yt);
        models.push(["RandomForestClassifier.predictProba", m, (m, X) => m.predictProba(X)]);
    }
    models.push(["RandomForestRegressor.predict", new RandomForestRegressor({ nEstimators: 4, seed: 2 }).fit(Xt, yr), (m, X) => m.predict(X)]);
    models.push(["GradientBoostingClassifier.predict", new GradientBoostingClassifier({ nEstimators: 4, seed: 3 }).fit(Xt, yt), (m, X) => m.predict(X)]);
    models.push(["GradientBoostingRegressor.predict", new GradientBoostingRegressor({ nEstimators: 4, seed: 3 }).fit(Xt, yr), (m, X) => m.predict(X)]);
    models.push(["XGBClassifier.predict", new XGBClassifier({ nEstimators: 4, seed: 4 }).fit(Xt, yt), (m, X) => m.predict(X)]);
    models.push(["XGBRegressor.predict", new XGBRegressor({ nEstimators: 4, seed: 4 }).fit(Xt, yr), (m, X) => m.predict(X)]);
    {
        const m = new GaussianNB().fit(Xt, yt);
        models.push(["GaussianNB.predict", m, (m, X) => m.predict(X)]);
        models.push(["GaussianNB.predictProba", m, (m, X) => m.predictProba(X)]);
    }
    {
        const m = new KNClassifier(3).fit(Xt, yt);
        models.push(["KNClassifier.predict", m, (m, X) => m.predict(X)]);
    }
    models.push(["KNRegressor.predict", new KNRegressor(3).fit(Xt, yr), (m, X) => m.predict(X)]);
    {
        const m = new SVC().fit(Xt, yt);
        models.push(["SVC.predict", m, (m, X) => m.predict(X)]);
        models.push(["SVC.decisionFunction", m, (m, X) => m.decisionFunction(X)]);
    }
    {
        const m = new GaussianMixture(2, { seed: 6 }).fit(Xt);
        models.push(["GaussianMixture.predict", m, (m, X) => m.predict(X)]);
        models.push(["GaussianMixture.predictProba", m, (m, X) => m.predictProba(X)]);
    }
    let bad = [];
    for (const [name, m, fn] of models) {
        for (const k of [2, 3, 5, 10, 12]) {
            const S = dupRow([2.3, -1.25, 0.75], [k, k + 1, k + 2]), D = S.toDense();
            if (!same(fn(m, S), fn(m, D))) bad.push(`${name} k=${k}`);
            S.close();
        }
    }
    ok(bad.length === 0, "E2 every family both forms x 2/3/5/10/12-dup queries Object.is",
       bad.slice(0, 5).join("; "));
    /* predictInto for every family that has one, at k=12 */
    {
        const S = dupRow([2.3, -1.25, 0.75], [12, 13, 14]), D = S.toDense();
        let ibad = [];
        for (const [name, m] of models) {
            if (typeof m.predictInto !== "function") continue;
            const a = new Float64Array(1), b = new Float64Array(1);
            try {
                m.predictInto(a, S); m.predictInto(b, D);
                if (!Object.is(a[0], b[0])) ibad.push(name);
            } catch (e) { ibad.push(name + " threw " + e.message); }
        }
        ok(ibad.length === 0, "E2b predictInto over a 12-dup CSR == dense Object.is", ibad.join("; "));
        S.close();
    }
    const seen = new Set();
    for (const [name, m] of models)
        if (typeof m.close === "function" && !seen.has(m)) { seen.add(m); try { m.close(); } catch (e) {} }
}
print("review_esc_kernel: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_kernel: " + fail + " failures");
