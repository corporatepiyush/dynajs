// covers: dyna:ml.GaussianNB
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gaussian_nb_weights");
import { LinearRegression, LogisticRegression, KMeans, PCA, GaussianNB, DecisionTreeClassifier, DecisionTreeRegressor, RandomForestRegressor, GradientBoostingRegressor, KNClassifier, KNRegressor, DBScan, StandardScaler, MinMaxScaler, SVC, GaussianMixture } from "dyna:ml";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function close(a, b, tol, msg) {
    n++;
    if (!(Math.abs(a - b) <= tol))
        throw new Error("assertion failed: " + msg + " (" + a + " vs " + b + ")");
}
async function throwsWith(fn, needle, msg) {
    n++;
    let e = null;
    try { await fn(); } catch (err) { e = err; }
    if (e === null) throw new Error("assertion failed: " + msg + " (expected a throw)");
    if (!String(e.message).includes(needle))
        throw new Error("assertion failed: " + msg + " (message was: " + e.message + ")");
}

await __kitS.section("maxBins=", async () => {
    function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }
    const rnd = lcg(5);
    const X = [], yr = [], yc = [];
    for (let i = 0; i < 200; i++) {
        const a = rnd() * 4 - 2, b = rnd() * 4 - 2;
        X.push([a, b]);
        yr.push(2 * a - b + rnd() * 0.2);
        yc.push(a + b > 0 ? 1 : 0);
    }
    const wint = [], Xd = [], yrd = [], ycd = [];
    for (let i = 0; i < 200; i++) {
        const k = (i % 3) + 1;
        wint.push(k);
        for (let j = 0; j < k; j++) { Xd.push(X[i]); yrd.push(yr[i]); ycd.push(yc[i]); }
    }
    const keep = X.map((_, i) => i % 4 !== 0);
    const w0 = keep.map(k => (k ? 1 : 0));
    const Xk = X.filter((_, i) => keep[i]);
    const yrk = yr.filter((_, i) => keep[i]);
    const yck = yc.filter((_, i) => keep[i]);

    function worst(a, b) {
        let m = 0;
        for (let i = 0; i < a.length; i++) m = Math.max(m, Math.abs(a[i] - b[i]));
        return m;
    }

    const trees = [
        ["DecisionTreeRegressor", o => new DecisionTreeRegressor({ maxDepth: 5, ...o }), yr, yrd, yrk],
        ["DecisionTreeClassifier", o => new DecisionTreeClassifier({ maxDepth: 5, ...o }), yc, ycd, yck],
        ["RandomForestRegressor", o => new RandomForestRegressor({ nEstimators: 6, maxDepth: 4, seed: 2, ...o }), yr, yrd, yrk],
        ["GradientBoostingRegressor", o => new GradientBoostingRegressor({ nEstimators: 8, maxDepth: 3, ...o }), yr, yrd, yrk],
    ];
    for (const [name, make, target, targetDup, targetKeep] of trees) {
        for (const bins of [0, 64]) {
            const tag = name + " maxBins=" + bins;
            const bagged = name.startsWith("RandomForest");
            if (!bagged) {
                const a = await make({ maxBins: bins }).fit(X, target, { sampleWeight: wint });
                const b = await make({ maxBins: bins }).fit(Xd, targetDup);
                close(worst(await a.predict(X), await b.predict(X)), 0, 1e-9,
                      tag + ": an integer weight equals duplicating the row");
            }
            const u0 = await make({ maxBins: bins }).fit(X, target);
            const u1 = await make({ maxBins: bins }).fit(X, target, { sampleWeight: new Array(200).fill(1) });
            close(worst(await u0.predict(X), await u1.predict(X)), 0, 0,
                  tag + ": all-ones is bit-identical to unweighted");
            const u7 = await make({ maxBins: bins }).fit(X, target, { sampleWeight: new Array(200).fill(3.5) });
            close(worst(await u0.predict(X), await u7.predict(X)), 0, 1e-9,
                  tag + ": a uniform scale changes nothing");
            if (!bagged) {
                const z = await make({ maxBins: bins }).fit(X, target, { sampleWeight: w0 });
                const d = await make({ maxBins: bins }).fit(Xk, targetKeep);
                close(worst(await z.predict(Xk), await d.predict(Xk)), 0, 1e-9,
                      tag + ": a zero weight equals deleting the row");
            } else {
                const z = await make({ maxBins: bins }).fit(X, target, { sampleWeight: w0 });
                assert(Number.isFinite((await z.predict(Xk))[0]),
                       tag + ": a bagged fit with zero weights still predicts");
            }
        }
    }

    {
        const rare = X.map((_, i) => (i < 20 ? 1 : 0));
        const plain = await new DecisionTreeClassifier({ maxDepth: 3 }).fit(X, rare);
        const bal = await new DecisionTreeClassifier({ maxDepth: 3 })
            .fit(X, rare, { sampleWeight: rare.map(v => (v ? 9 : 1)) });
        const found = async p => (await p.predict(X)).filter((v, i) => v === 1 && rare[i] === 1).length;
        assert((await found(bal)) > (await found(plain)),
               "weighting the minority class must recover more of it: "
               + (await found(plain)) + " -> " + (await found(bal)));
    }

    {
        const a = await new KMeans(3, { seed: 1 }).fit(X, { sampleWeight: new Array(200).fill(1) });
        const b = await new KMeans(3, { seed: 1 }).fit(X);
        close(worst(await a.predict(X), await b.predict(X)), 0, 0, "KMeans all-ones is unweighted");
        close(a.inertia, b.inertia, 0, "and so is its inertia");
        const c = await new KMeans(3, { seed: 1 }).fit(X, { sampleWeight: w0 });
        const d = await new KMeans(3, { seed: 1 }).fit(Xk);
        assert((await c.predict(Xk)).length === (await d.predict(Xk)).length, "KMeans zero weight fits");
        close((await new KMeans(2, { seed: 1 }).fit([[0], [0], [10], [10]],
              { sampleWeight: [1, 1, 0, 0] })).inertia, 0, 1e-9,
              "a cluster of zero-weight points contributes no inertia");
    }

    {
        const a = await new GaussianNB().fit(X, yc, { sampleWeight: wint });
        const b = await new GaussianNB().fit(Xd, ycd);
        close(worst(await a.predict(X), await b.predict(X)), 0, 0,
              "GaussianNB: an integer weight equals duplicating the row");
        const u0 = await new GaussianNB().fit(X, yc);
        const u1 = await new GaussianNB().fit(X, yc, { sampleWeight: new Array(200).fill(1) });
        close(worst((await u0.predictProba(X)).map(r => r[0]), (await u1.predictProba(X)).map(r => r[0])),
              0, 0, "GaussianNB all-ones is bit-identical");
        const z = await new GaussianNB().fit(X, yc, { sampleWeight: w0 });
        const d = await new GaussianNB().fit(Xk, yck);
        close(worst(await z.predict(Xk), await d.predict(Xk)), 0, 0,
              "GaussianNB: a zero weight equals deleting the row");
    }

    {
        const a = await new StandardScaler().fit(X, { sampleWeight: wint });
        const b = await new StandardScaler().fit(Xd);
        close(worst(a.mean, b.mean), 0, 1e-9, "scaler weighted mean equals duplication");
        close(worst(a.std, b.std), 0, 1e-9, "scaler weighted std equals duplication");
        const u0 = await new StandardScaler().fit(X);
        const u1 = await new StandardScaler().fit(X, { sampleWeight: new Array(200).fill(1) });
        close(worst(u0.mean, u1.mean), 0, 0, "scaler all-ones is bit-identical");
        const u7 = await new StandardScaler().fit(X, { sampleWeight: new Array(200).fill(7) });
        close(worst(u0.mean, u7.mean), 0, 1e-12, "a uniform scale changes no moment");
        const z = await new StandardScaler().fit(X, { sampleWeight: w0 });
        const d = await new StandardScaler().fit(Xk);
        close(worst(z.mean, d.mean), 0, 1e-12, "scaler zero weight equals deletion");
    }
});
__kitS.end();

print("test_ml_weights: all " + n + " assertions passed");
