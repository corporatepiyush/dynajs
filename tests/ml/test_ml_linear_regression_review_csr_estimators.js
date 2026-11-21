// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_review_csr_estimators");
import { CSR, LinearRegression, LogisticRegression, KMeans, PCA,
         DecisionTreeClassifier, DecisionTreeRegressor,
         RandomForestClassifier, RandomForestRegressor,
         GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor,
         GaussianNB, KNClassifier, KNRegressor, SVC, GaussianMixture,
         StandardScaler, MinMaxScaler, imputeMean, dropMissing } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const same = (a, b) => { const A = Array.from(a).flat(9), B = Array.from(b).flat(9);
    return A.length === B.length && A.every((v, i) => Object.is(v, B[i])); };
async function errOf(fn) { try { await fn(); return null; } catch (e) { return e; } }

function mulberry32(seed) { let a = seed >>> 0; return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

const rnd = mulberry32(0xB0B0);
const rows = 24, cols = 7;
const dense = [];
for (let i = 0; i < rows; i++) {
    const r = new Array(cols).fill(0);
    for (let k = 0; k < 3; k++) {
        const c = (rnd() * cols) | 0;
        r[c] = Math.round((rnd() - 0.5) * 2000) / 100;
        if (r[c] === 0) r[c] = 0.07;
    }
    dense.push(r);
}
const ycls = dense.map((r, i) => (r[0] + r[1] > 0 ? 1 : 0) + (i % 3 === 0 ? 1 : 0));
const yreg = dense.map((r) => r[0] * 2 - r[2] + 1);
const S = await CSR.fromDense(dense);
const rv = [], rc = [], rp = [0];
for (let i = 0; i < rows; i++) {
    const es = [];
    for (let c = 0; c < cols; c++) if (dense[i][c] !== 0) es.push([c, dense[i][c]]);
    es.reverse();
    for (const [c, v] of es) { rv.push(v); rc.push(c); }
    rp.push(rv.length);
}
const SU = new CSR(rv, rc, rp, cols);

async function eqAll(name, predict, proba, dec) {
    const a = await predict(S), b = await predict(dense), c = await predict(SU);
    let good = same(a, b) && same(c, b);
    let extra = "";
    if (proba) {
        const pa = await proba(S), pb = await proba(dense);
        good = good && same(pa, pb);
        good = good && Array.isArray(pa[0]) === Array.isArray(pb[0]);
        extra += " proba";
    }
    if (dec) {
        good = good && same(await dec(S), await dec(dense));
        extra += " decision";
    }
    ok(good, "P-eq " + name + ": CSR == unsorted-CSR == dense (bitwise)" + extra);
}

await __kitS.section("P39b LinearRegression.predictInto(CSR)", async () => {
P39: {
    const lin = await new LinearRegression().fit(dense, yreg);
    const log = await new LogisticRegression().fit(dense, ycls);
    await eqAll("LinearRegression.predict", async (X) => await lin.predict(X));
    const out = new Float64Array(rows);
    await lin.predictInto(out, S);
    ok(same(out, await lin.predict(dense)), "P39b LinearRegression.predictInto(CSR)");
    await eqAll("LogisticRegression.predict", async (X) => await log.predict(X),
          async (X) => await log.predictProba(X));
    lin.close(); log.close();
    ok(true, "P39 (family: linear)");
}
});

await __kitS.section("INFO P49 predict-side NaN policy (dense):", async () => {
P49: {
    const Xfit = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const yf = [1, 2, 3, 2, 2, 3], yfr = [1, 2, 3, 2, 2, 3];
    const Xn = [[1, 0, 2], [NaN, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const map = [];
    const probe = async (name, fn) => {
        const e = await errOf(fn);
        map.push(name + "=" + (e ? "refused(" + e.constructor.name + ")" : "ACCEPTED"));
    };
    await probe("lin", async () => await (await new LinearRegression().fit(Xfit, yfr)).predict(Xn));
    await probe("tree", async () => await (await new DecisionTreeRegressor({ seed: 1 }).fit(Xfit, yfr)).predict(Xn));
    await probe("forest", async () => await (await new RandomForestRegressor({ nEstimators: 3, seed: 1 }).fit(Xfit, yfr)).predict(Xn));
    await probe("gb", async () => await (await new GradientBoostingRegressor({ nEstimators: 3, seed: 1 }).fit(Xfit, yfr)).predict(Xn));
    await probe("xgb", async () => await (await new XGBRegressor({ nEstimators: 3, seed: 1 }).fit(Xfit, yfr)).predict(Xn));
    await probe("kmeans", async () => await (await new KMeans(2, 1).fit(Xfit)).predict(Xn));
    await probe("knn", async () => await (await new KNRegressor(2).fit(Xfit, yfr)).predict(Xn));
    await probe("nb", async () => await (await new GaussianNB().fit(Xfit, yf)).predict(Xn));
    await probe("svm", async () => await (await new SVC().fit(Xfit, yf)).predict(Xn));
    await probe("gmm", async () => await (await new GaussianMixture(2, { seed: 1 }).fit(Xfit)).predict(Xn));
    print("  INFO  P49 predict-side NaN policy (dense): " + map.join(", "));
    ok(true, "P49 predict-side non-finite policy mapped (see INFO line)");
}
});
__kitS.end();

S.close(); SU.close();
print("review_csr_estimators: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_estimators: " + fail + " failures");
