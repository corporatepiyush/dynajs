// covers: dyna:ml.CSR
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_csr_review_csr_estimators");
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

await __kitS.section("(class", async () => {
P44: {
    const y = [1, 2, 3, 4];
    const tiny = await CSR.fromDense([[1, 2], [3, 4], [5, 6], [7, 8]]);
    const cases = [
        ["KMeans.fit", async () => await new KMeans(2, 1).fit(tiny)],
        ["DecisionTreeClassifier.fit", async () => await new DecisionTreeClassifier().fit(tiny, ycls.slice(0, 4))],
        ["DecisionTreeRegressor.fit", async () => await new DecisionTreeRegressor().fit(tiny, y)],
        ["RandomForestClassifier.fit", async () => await new RandomForestClassifier({ nEstimators: 2 }).fit(tiny, ycls.slice(0, 4))],
        ["RandomForestRegressor.fit", async () => await new RandomForestRegressor({ nEstimators: 2 }).fit(tiny, y)],
        ["GradientBoostingRegressor.fit", async () => await new GradientBoostingRegressor({ nEstimators: 2 }).fit(tiny, y)],
        ["GradientBoostingClassifier.fit", async () => await new GradientBoostingClassifier({ nEstimators: 2 }).fit(tiny, ycls.slice(0, 4))],
        ["XGBRegressor.fit", async () => await new XGBRegressor({ nEstimators: 2 }).fit(tiny, y)],
        ["XGBClassifier.fit", async () => await new XGBClassifier({ nEstimators: 2 }).fit(tiny, ycls.slice(0, 4))],
        ["GaussianNB.fit", async () => await new GaussianNB().fit(tiny, ycls.slice(0, 4))],
        ["KNClassifier.fit", async () => await new KNClassifier(2).fit(tiny, ycls.slice(0, 4))],
        ["KNRegressor.fit", async () => await new KNRegressor(2).fit(tiny, y)],
        ["SVC.fit", async () => await new SVC().fit(tiny, ycls.slice(0, 4))],
        ["GaussianMixture.fit", async () => await new GaussianMixture(2).fit(tiny)],
    ];
    let allRefused = true, msgs = [];
    for (const [name, fn] of cases) {
        const e = await errOf(fn);
        if (!e || !/toDense/.test(e.message)) { allRefused = false; msgs.push(name + " => " + (e ? e.message : "ACCEPTED")); }
        else if (e.constructor.name !== "TypeError") msgs.push(name + " (class " + e.constructor.name + ")");
    }
    ok(allRefused, "P44 every fit beyond Linear/Logistic REFUSES a CSR naming toDense()",
       msgs.join(" | "));
    tiny.close();
}
});

await __kitS.section("refuses w/o toDense:", async () => {
P45: {
    const tiny = await CSR.fromDense([[1, 2], [3, 4]]);
    const sc = await new StandardScaler().fit([[1, 2], [3, 4]]);
    const mm = await new MinMaxScaler().fit([[1, 2], [3, 4]]);
    const pc = await new PCA(1).fit([[1, 2], [3, 4]]);
    const cases = [
        ["StandardScaler.transform", async () => await sc.transform(tiny)],
        ["MinMaxScaler.transform", async () => await mm.transform(tiny)],
        ["PCA.transform", async () => await pc.transform(tiny)],
        ["imputeMean", async () => await imputeMean(tiny)],
        ["dropMissing", async () => await dropMissing(tiny)],
    ];
    let allRefused = true, msgs = [];
    for (const [name, fn] of cases) {
        const e = await errOf(fn);
        if (e && /toDense/.test(e.message)) continue;
        if (e && !/toDense/.test(e.message) && /CSR/.test(e.message)) { msgs.push(name + " refuses w/o toDense: " + e.message); allRefused = false; continue; }
        if (e && !/toDense/.test(e.message)) {
            msgs.push(name + " => " + e.constructor.name + ": " + e.message);
            continue;
        }
        allRefused = false; msgs.push(name + " ACCEPTED a CSR");
    }
    ok(allRefused, "P45 scaler/PCA transforms + imputation refuse a CSR naming toDense()",
       msgs.join(" | "));
    tiny.close(); sc.close(); mm.close(); pc.close();
}
});

await __kitS.section("P46 refusal message: TypeError, names the missing sparse path and .toDense()", async () => {
P46: {
    const tiny = await CSR.fromDense([[1, 2], [3, 4]]);
    const e = await errOf(async () => await new KMeans(2, 1).fit(tiny));
    const msg = e ? e.message : "";
    ok(e && e.constructor.name === "TypeError" &&
       /this method has no sparse path/.test(msg) && /toDense/.test(msg),
       "P46 refusal message: TypeError, names the missing sparse path and .toDense()", msg);
    const lin = new LinearRegression();
    const log = new LogisticRegression();
    const e1 = await errOf(async () => await lin.fit(tiny, [1, 2]));
    const e2 = await errOf(async () => await log.fit(tiny, [0, 1]));
    ok(e1 === null && e2 === null,
       "P46b LinearRegression.fit / LogisticRegression.fit ACCEPT a CSR",
       JSON.stringify([e1 && e1.message, e2 && e2.message]));
    tiny.close(); lin.close(); log.close();
}
});

await __kitS.section("construction", async () => {
P47: {
    const nanRow = [[1, NaN, 0, 0, 0, 0]];
    const infRow = [[1, -Infinity, 0, 0, 0, 0]];
    const kc = await new KNClassifier(3).fit([[1, 0, 0, 0, 0, 0], [0, 1, 0, 0, 0, 0], [0, 0, 1, 0, 0, 0]], [1, 2, 3]);
    let good = true, msgs = [];
    for (const row of [nanRow, infRow]) {
        const eC = await errOf(() => new CSR([row[0][0], row[0][1]], [0, 1], [0, 2], 6));
        const eF = await errOf(async () => await CSR.fromDense(row));
        if (!eC || !eF || eC.constructor.name !== "RangeError" ||
            eF.constructor.name !== "RangeError") {
            good = false;
            msgs.push("construction " + JSON.stringify(row) + ": ctor=" + (eC && eC.message) +
                      " fromDense=" + (eF && eF.message));
        }
    }
    for (const pair of [[1e308, 1e308], [-1e308, -1e308]]) {
        const S = new CSR(pair, pair.map(() => 0), [0, pair.length], 6);
        const D = await S.toDense();
        const eS = await errOf(async () => await kc.predict(S)), eD = await errOf(async () => await kc.predict(D));
        if (!eS || !eD || eS.constructor.name !== eD.constructor.name ||
            eS.constructor.name !== "RangeError") {
            good = false;
            msgs.push("overflow " + pair + ": sparse=" + (eS && eS.message) + " dense=" + (eD && eD.message));
        }
        S.close();
    }
    ok(good, "P47 non-finite construction refused both declarations; overflow-class predict refusals identical for CSR and dense", msgs.join(" | "));
    kc.close();
}
});

await __kitS.section("P48 CSR predictProba keeps the row-arrays shape and values", async () => {
P48: {
    const X3 = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const y3 = [1, 2, 3, 2, 2, 3];
    const q = await CSR.fromDense([[1, 0, 2], [0, 3, 0]]);
    const rfc = await new RandomForestClassifier({ nEstimators: 3, seed: 21 }).fit(X3, y3);
    const pa = await rfc.predictProba(q), pb = await rfc.predictProba([[1, 0, 2], [0, 3, 0]]);
    const f = await rfc.predict(q, undefined, undefined, { as: "f64" });
    ok(Array.isArray(pa[0]) && Array.isArray(pb[0]) && same(pa, pb),
       "P48 CSR predictProba keeps the row-arrays shape and values");
    ok(f instanceof Float64Array && same(f, await rfc.predict(q)),
       "P48b {as:'f64'} over a CSR returns a flat Float64Array of predict values");
    rfc.close(); q.close();
}
});
__kitS.end();

S.close(); SU.close();
print("review_csr_estimators: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_estimators: " + fail + " failures");
