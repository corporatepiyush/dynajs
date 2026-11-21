// covers: dyna:ml.KMeans
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_k_means_sparse");
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

await __kitS.section("KMeans refuses a CSR", async () => {
    const S = await CSR.fromDense([[1, 0], [0, 2], [3, 0], [0, 4]]);
    const y = [1, 2, 3, 4], yc = [0, 1, 0, 1];

    const msg = await throws(async () => await new KMeans(2).fit(S), TypeError, "KMeans refuses a CSR");
    assert(msg.includes("toDense"), "and names toDense(): " + msg);

    await throws(async () => await new PCA(1).fit(S), TypeError, "PCA refuses");
    await throws(async () => await new GaussianNB().fit(S, yc), TypeError, "GaussianNB refuses");
    await throws(async () => await new DecisionTreeClassifier().fit(S, yc), TypeError, "the tree refuses");
    await throws(async () => await new RandomForestRegressor({ nEstimators: 2 }).fit(S, y), TypeError,
           "the forest refuses");
    await throws(async () => await new XGBRegressor({ nEstimators: 2 }).fit(S, y), TypeError, "XGB refuses");
    await throws(async () => await new StandardScaler().fit(S), TypeError, "the scaler refuses");

    assert((await (await new KMeans(2).fit(await S.toDense())).predict(await S.toDense())).length === 4,
           "toDense() makes it work");
    S.close();
});
__kitS.end();

print("test_ml_sparse: all " + n + " assertions passed");
