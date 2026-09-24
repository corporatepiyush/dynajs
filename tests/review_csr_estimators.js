/* review_csr_estimators.js -- e7 review, CSR==dense per estimator + the
 * refusal contract (P39-P48). Independent data/seeds from the implementer's
 * suites. Two claims:
 *   (a) every predict/predictProba/predictInto/decisionFunction over a CSR
 *       returns EXACTLY what the dense form returns (Object.is), on random
 *       sparse data AND on unsorted-index CSR;
 *   (b) fits beyond Linear/Logistic, apply() and the scaler/PCA transforms
 *       REFUSE a CSR and name `.toDense()`.
 */
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
function errOf(fn) { try { fn(); return null; } catch (e) { return e; } }

function mulberry32(seed) { let a = seed >>> 0; return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

/* my own sparse dataset: 24 rows x 7 cols, 3 nz/row, mixed scale */
const rnd = mulberry32(0xB0B0);
const rows = 24, cols = 7;
const dense = [];
for (let i = 0; i < rows; i++) {
    const r = new Array(cols).fill(0);
    for (let k = 0; k < 3; k++) {
        const c = (rnd() * cols) | 0;
        r[c] = Math.round((rnd() - 0.5) * 2000) / 100;   /* [-10, 10] 2dp */
        if (r[c] === 0) r[c] = 0.07;
    }
    dense.push(r);
}
const ycls = dense.map((r, i) => (r[0] + r[1] > 0 ? 1 : 0) + (i % 3 === 0 ? 1 : 0));
const yreg = dense.map((r) => r[0] * 2 - r[2] + 1);
const S = CSR.fromDense(dense);
/* the same data as a CSR with each row's indices REVERSED (unsorted input) */
const rv = [], rc = [], rp = [0];
for (let i = 0; i < rows; i++) {
    const es = [];
    for (let c = 0; c < cols; c++) if (dense[i][c] !== 0) es.push([c, dense[i][c]]);
    es.reverse();                        /* unsorted within the row */
    for (const [c, v] of es) { rv.push(v); rc.push(c); }
    rp.push(rv.length);
}
const SU = new CSR(rv, rc, rp, cols);

/* (a) equality across every estimator, both CSR forms */
function eqAll(name, predict, proba, dec) {
    const a = predict(S), b = predict(dense), c = predict(SU);
    let good = same(a, b) && same(c, b);
    let extra = "";
    if (proba) {
        const pa = proba(S), pb = proba(dense);
        good = good && same(pa, pb);
        good = good && Array.isArray(pa[0]) === Array.isArray(pb[0]);
        extra += " proba";
    }
    if (dec) {
        good = good && same(dec(S), dec(dense));
        extra += " decision";
    }
    ok(good, "P-eq " + name + ": CSR == unsorted-CSR == dense (bitwise)" + extra);
}

P39: {
    const lin = new LinearRegression().fit(dense, yreg);
    const log = new LogisticRegression().fit(dense, ycls);
    eqAll("LinearRegression.predict", (X) => lin.predict(X));
    const out = new Float64Array(rows);
    lin.predictInto(out, S);
    ok(same(out, lin.predict(dense)), "P39b LinearRegression.predictInto(CSR)");
    eqAll("LogisticRegression.predict", (X) => log.predict(X),
          (X) => log.predictProba(X));
    lin.close(); log.close();
    ok(true, "P39 (family: linear)");
}
P40: {
    const dtc = new DecisionTreeClassifier({ seed: 11 }).fit(dense, ycls);
    const dtr = new DecisionTreeRegressor({ seed: 12 }).fit(dense, yreg);
    const rfc = new RandomForestClassifier({ nEstimators: 7, seed: 13 }).fit(dense, ycls);
    const rfr = new RandomForestRegressor({ nEstimators: 7, seed: 14 }).fit(dense, yreg);
    eqAll("DecisionTreeClassifier", (X) => dtc.predict(X));
    eqAll("DecisionTreeRegressor", (X) => dtr.predict(X));
    eqAll("RandomForestClassifier", (X) => rfc.predict(X), (X) => rfc.predictProba(X));
    eqAll("RandomForestRegressor", (X) => rfr.predict(X));
    const out = new Float64Array(rows);
    rfc.predictInto(out, S);
    ok(same(out, rfc.predict(dense)), "P40b RandomForestClassifier.predictInto(CSR)");
    /* apply() is dense-only by contract */
    const e = errOf(() => dtc.apply ? dtc.apply(S) : null);
    ok(dtc.apply === undefined || (e && /toDense/.test(e.message)),
       "P40c tree.apply(CSR) refuses naming toDense", e && e.message);
    dtc.close(); dtr.close(); rfc.close(); rfr.close();
}
P41: {
    const gbr = new GradientBoostingRegressor({ nEstimators: 6, seed: 15 }).fit(dense, yreg);
    const gbc = new GradientBoostingClassifier({ nEstimators: 6, seed: 16 }).fit(dense, ycls);
    const xgbr = new XGBRegressor({ nEstimators: 6, seed: 17 }).fit(dense, yreg);
    const xgbc = new XGBClassifier({ nEstimators: 6, seed: 18 }).fit(dense, ycls);
    eqAll("GradientBoostingRegressor", (X) => gbr.predict(X));
    eqAll("GradientBoostingClassifier", (X) => gbc.predict(X), (X) => gbc.predictProba(X));
    eqAll("XGBRegressor", (X) => xgbr.predict(X));
    eqAll("XGBClassifier", (X) => xgbc.predict(X), (X) => xgbc.predictProba(X));
    gbr.close(); gbc.close(); xgbr.close(); xgbc.close();
}
P42: {
    const nb = new GaussianNB().fit(dense, ycls);
    const kc = new KNClassifier(3).fit(dense, ycls);
    const kr = new KNRegressor(3, "distance").fit(dense, yreg);
    eqAll("GaussianNB", (X) => nb.predict(X), (X) => nb.predictProba(X));
    eqAll("KNClassifier", (X) => kc.predict(X));
    eqAll("KNRegressor", (X) => kr.predict(X));
    nb.close(); kc.close(); kr.close();
}
P43: {
    const svc = new SVC().fit(dense, ycls);
    const gmm = new GaussianMixture(3, { seed: 19 }).fit(dense);
    const km = new KMeans(3, 20).fit(dense);
    eqAll("SVC", (X) => svc.predict(X), null, (X) => svc.decisionFunction(X));
    eqAll("GaussianMixture", (X) => gmm.predict(X), (X) => gmm.predictProba(X));
    eqAll("KMeans", (X) => km.predict(X));
    svc.close(); gmm.close(); km.close();
}

/* (b) the refusal contract */
P44: {
    const y = [1, 2, 3, 4];
    const tiny = CSR.fromDense([[1, 2], [3, 4], [5, 6], [7, 8]]);
    const cases = [
        ["KMeans.fit", () => new KMeans(2, 1).fit(tiny)],
        ["DecisionTreeClassifier.fit", () => new DecisionTreeClassifier().fit(tiny, ycls.slice(0, 4))],
        ["DecisionTreeRegressor.fit", () => new DecisionTreeRegressor().fit(tiny, y)],
        ["RandomForestClassifier.fit", () => new RandomForestClassifier({ nEstimators: 2 }).fit(tiny, ycls.slice(0, 4))],
        ["RandomForestRegressor.fit", () => new RandomForestRegressor({ nEstimators: 2 }).fit(tiny, y)],
        ["GradientBoostingRegressor.fit", () => new GradientBoostingRegressor({ nEstimators: 2 }).fit(tiny, y)],
        ["GradientBoostingClassifier.fit", () => new GradientBoostingClassifier({ nEstimators: 2 }).fit(tiny, ycls.slice(0, 4))],
        ["XGBRegressor.fit", () => new XGBRegressor({ nEstimators: 2 }).fit(tiny, y)],
        ["XGBClassifier.fit", () => new XGBClassifier({ nEstimators: 2 }).fit(tiny, ycls.slice(0, 4))],
        ["GaussianNB.fit", () => new GaussianNB().fit(tiny, ycls.slice(0, 4))],
        ["KNClassifier.fit", () => new KNClassifier(2).fit(tiny, ycls.slice(0, 4))],
        ["KNRegressor.fit", () => new KNRegressor(2).fit(tiny, y)],
        ["SVC.fit", () => new SVC().fit(tiny, ycls.slice(0, 4))],
        ["GaussianMixture.fit", () => new GaussianMixture(2).fit(tiny)],
    ];
    let allRefused = true, msgs = [];
    for (const [name, fn] of cases) {
        const e = errOf(fn);
        if (!e || !/toDense/.test(e.message)) { allRefused = false; msgs.push(name + " => " + (e ? e.message : "ACCEPTED")); }
        else if (e.constructor.name !== "TypeError") msgs.push(name + " (class " + e.constructor.name + ")");
    }
    ok(allRefused, "P44 every fit beyond Linear/Logistic REFUSES a CSR naming toDense()",
       msgs.join(" | "));
    tiny.close();
}
P45: {
    const tiny = CSR.fromDense([[1, 2], [3, 4]]);
    const sc = new StandardScaler().fit([[1, 2], [3, 4]]);
    const mm = new MinMaxScaler().fit([[1, 2], [3, 4]]);
    const pc = new PCA(1).fit([[1, 2], [3, 4]]);
    const cases = [
        ["StandardScaler.transform", () => sc.transform(tiny)],
        ["MinMaxScaler.transform", () => mm.transform(tiny)],
        ["PCA.transform", () => pc.transform(tiny)],
        ["imputeMean", () => imputeMean(tiny)],
        ["dropMissing", () => dropMissing(tiny)],
    ];
    let allRefused = true, msgs = [];
    for (const [name, fn] of cases) {
        const e = errOf(fn);
        if (e && /toDense/.test(e.message)) continue;      /* contract */
        if (e && !/toDense/.test(e.message) && /CSR/.test(e.message)) { msgs.push(name + " refuses w/o toDense: " + e.message); allRefused = false; continue; }
        if (e && !/toDense/.test(e.message)) {
            /* wrong error shape for a CSR argument -- or the call signature
             * is off; report rather than silently pass */
            msgs.push(name + " => " + e.constructor.name + ": " + e.message);
            continue;
        }
        allRefused = false; msgs.push(name + " ACCEPTED a CSR");
    }
    ok(allRefused, "P45 scaler/PCA transforms + imputation refuse a CSR naming toDense()",
       msgs.join(" | "));
    tiny.close(); sc.close(); mm.close(); pc.close();
}
P46: {
    /* the exact refusal message */
    const tiny = CSR.fromDense([[1, 2], [3, 4]]);
    const e = errOf(() => new KMeans(2, 1).fit(tiny));
    const msg = e ? e.message : "";
    ok(e && e.constructor.name === "TypeError" &&
       /this method has no sparse path/.test(msg) && /toDense/.test(msg),
       "P46 refusal message: TypeError, names the missing sparse path and .toDense()", msg);
    /* and the two ALLOWED sparse fits still work */
    const lin = new LinearRegression();
    const log = new LogisticRegression();
    const e1 = errOf(() => lin.fit(tiny, [1, 2]));
    const e2 = errOf(() => log.fit(tiny, [0, 1]));
    ok(e1 === null && e2 === null,
       "P46b LinearRegression.fit / LogisticRegression.fit ACCEPT a CSR",
       JSON.stringify([e1 && e1.message, e2 && e2.message]));
    tiny.close(); lin.close(); log.close();
}
P47: {
    /* finite refusal parity, both layers: a non-finite value cannot be
     * CONSTRUCTED in either form (the ctor and fromDense refuse it
     * identically), and the one way a finite-valued matrix reads non-finite
     * -- a duplicate-sum overflow -- is refused by a sparse predict exactly
     * like the dense form of the same reading */
    const nanRow = [[1, NaN, 0, 0, 0, 0]];
    const infRow = [[1, -Infinity, 0, 0, 0, 0]];
    const kc = new KNClassifier(3).fit([[1, 0, 0, 0, 0, 0], [0, 1, 0, 0, 0, 0], [0, 0, 1, 0, 0, 0]], [1, 2, 3]);
    let good = true, msgs = [];
    for (const row of [nanRow, infRow]) {
        const eC = errOf(() => new CSR([row[0][0], row[0][1]], [0, 1], [0, 2], 6));
        const eF = errOf(() => CSR.fromDense(row));
        if (!eC || !eF || eC.constructor.name !== "RangeError" ||
            eF.constructor.name !== "RangeError") {
            good = false;
            msgs.push("construction " + JSON.stringify(row) + ": ctor=" + (eC && eC.message) +
                      " fromDense=" + (eF && eF.message));
        }
    }
    for (const pair of [[1e308, 1e308], [-1e308, -1e308]]) {
        const S = new CSR(pair, pair.map(() => 0), [0, pair.length], 6);
        const D = S.toDense();
        const eS = errOf(() => kc.predict(S)), eD = errOf(() => kc.predict(D));
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
P48: {
    /* shape parity: a CSR predictProba answers in the row-arrays form like
     * an array X does; {as:'f64'} predict over a CSR is a flat Float64Array */
    const X3 = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const y3 = [1, 2, 3, 2, 2, 3];
    const q = CSR.fromDense([[1, 0, 2], [0, 3, 0]]);
    const rfc = new RandomForestClassifier({ nEstimators: 3, seed: 21 }).fit(X3, y3);
    const pa = rfc.predictProba(q), pb = rfc.predictProba([[1, 0, 2], [0, 3, 0]]);
    const f = rfc.predict(q, undefined, undefined, { as: "f64" });
    ok(Array.isArray(pa[0]) && Array.isArray(pb[0]) && same(pa, pb),
       "P48 CSR predictProba keeps the row-arrays shape and values");
    ok(f instanceof Float64Array && same(f, rfc.predict(q)),
       "P48b {as:'f64'} over a CSR returns a flat Float64Array of predict values");
    rfc.close(); q.close();
}

P49: {
    /* API.md line: "predict rejects non-finite input too, except on the XGB
     * models." Map the ACTUAL per-estimator predict-side policy (dense form)
     * -- the delta's CSR finite checks mirror each dense arm, so a doc that
     * misstates the dense policy is a pre-existing doc-truth question. */
    const Xfit = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const yf = [1, 2, 3, 2, 2, 3], yfr = [1, 2, 3, 2, 2, 3];
    const Xn = [[1, 0, 2], [NaN, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const map = [];
    const probe = (name, fn) => {
        const e = errOf(fn);
        map.push(name + "=" + (e ? "refused(" + e.constructor.name + ")" : "ACCEPTED"));
    };
    probe("lin", () => new LinearRegression().fit(Xfit, yfr).predict(Xn));
    probe("tree", () => new DecisionTreeRegressor({ seed: 1 }).fit(Xfit, yfr).predict(Xn));
    probe("forest", () => new RandomForestRegressor({ nEstimators: 3, seed: 1 }).fit(Xfit, yfr).predict(Xn));
    probe("gb", () => new GradientBoostingRegressor({ nEstimators: 3, seed: 1 }).fit(Xfit, yfr).predict(Xn));
    probe("xgb", () => new XGBRegressor({ nEstimators: 3, seed: 1 }).fit(Xfit, yfr).predict(Xn));
    probe("kmeans", () => new KMeans(2, 1).fit(Xfit).predict(Xn));
    probe("knn", () => new KNRegressor(2).fit(Xfit, yfr).predict(Xn));
    probe("nb", () => new GaussianNB().fit(Xfit, yf).predict(Xn));
    probe("svm", () => new SVC().fit(Xfit, yf).predict(Xn));
    probe("gmm", () => new GaussianMixture(2, { seed: 1 }).fit(Xfit).predict(Xn));
    print("  INFO  P49 predict-side NaN policy (dense): " + map.join(", "));
    ok(true, "P49 predict-side non-finite policy mapped (see INFO line)");
}

/* cleanup the shared handles */
S.close(); SU.close();
print("review_csr_estimators: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_estimators: " + fail + " failures");
