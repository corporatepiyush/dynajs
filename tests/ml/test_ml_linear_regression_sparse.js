// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_sparse");
import { CSR, LinearRegression, LogisticRegression, KMeans, PCA, GaussianNB,
         DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor, KNClassifier, KNRegressor, SVC,
         GaussianMixture, StandardScaler, accuracy } from "dyna:ml";

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
        return String(e.message || e);
    }
    throw new Error((m || "expected a throw") + " but none happened");
}
function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function sparseData(seed, rows, cols, nzPerRow) {
    const rnd = lcg(seed), dense = [], y = [], yc = [];
    for (let i = 0; i < rows; i++) {
        const r = new Array(cols).fill(0);
        for (let k = 0; k < nzPerRow; k++)
            r[Math.floor(rnd() * cols)] = rnd() * 2 - 1;
        dense.push(r);
        const s = 3 * r[0] - 2 * r[1] + r[2];
        y.push(s + (rnd() * 2 - 1) * 0.05);
        yc.push(s > 0 ? 1 : 0);
    }
    return { dense, y, yc };
}

await __kitS.section("LinearRegression sparse equals dense", async () => {
    for (const [rows, cols, nz] of [[400, 60, 4], [150, 20, 2], [60, 200, 3]]) {
        const d = sparseData(11 + cols, rows, cols, nz);
        const S = await CSR.fromDense(d.dense);
        const tag = rows + "x" + cols;

        const a = await new LinearRegression().fit(d.dense, d.y);
        const b = await new LinearRegression().fit(S, d.y);
        const pa = await a.predict(d.dense), pb = await b.predict(d.dense);
        let worst = 0;
        for (let i = 0; i < pa.length; i++) worst = Math.max(worst, Math.abs(pa[i] - pb[i]));
        close(worst, 0, 1e-9, tag + ": LinearRegression sparse equals dense");

        const c = await new LogisticRegression({ maxIter: 200 }).fit(d.dense, d.yc);
        const e = await new LogisticRegression({ maxIter: 200 }).fit(S, d.yc);
        const qa = (await c.predictProba(d.dense)).map(r => r[1]);
        const qb = (await e.predictProba(d.dense)).map(r => r[1]);
        let w2 = 0;
        for (let i = 0; i < qa.length; i++) w2 = Math.max(w2, Math.abs(qa[i] - qb[i]));
        close(w2, 0, 1e-9, tag + ": LogisticRegression sparse equals dense");
        a.close(); b.close(); c.close(); e.close(); S.close();
    }
});

await __kitS.section("sampleWeight works on the sparse path too", async () => {
    const d = sparseData(21, 300, 30, 3);
    const S = await CSR.fromDense(d.dense);
    const w = d.y.map((_, i) => (i % 2 ? 2 : 0.5));

    const a = await new LinearRegression().fit(d.dense, d.y, { sampleWeight: w });
    const b = await new LinearRegression().fit(S, d.y, { sampleWeight: w });
    const pa = await a.predict(d.dense), pb = await b.predict(d.dense);
    let worst = 0;
    for (let i = 0; i < pa.length; i++) worst = Math.max(worst, Math.abs(pa[i] - pb[i]));
    close(worst, 0, 1e-9, "sampleWeight works on the sparse path too");

    const y3 = d.dense.map(r => (r[0] > 0.3 ? 2 : (r[1] > 0.3 ? 1 : 0)));
    const c = await new LogisticRegression({ maxIter: 200 }).fit(d.dense, y3);
    const e = await new LogisticRegression({ maxIter: 200 }).fit(S, y3);
    close(await accuracy(y3, await c.predict(d.dense)), await accuracy(y3, await e.predict(d.dense)), 1e-12,
          "multiclass sparse equals dense");

    assert((await b.predict([d.dense[0]])).length === 1, "a sparsely fitted model predicts densely");
    a.close(); b.close(); c.close(); e.close(); S.close();
});
__kitS.end();

print("test_ml_sparse: all " + n + " assertions passed");
