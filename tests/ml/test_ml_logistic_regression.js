// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression");
import { LinearRegression, LogisticRegression, KMeans, SVC, trainTestSplit, kFold, gridSearch } from "dyna:ml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function approx(a, b, eps, msg) {
    n++;
    if (Math.abs(a - b) > eps)
        throw new Error("approx failed: " + msg + " (" + a + " vs " + b + ")");
}

await __kitS.section("origin -> class 0", async () => {
    const m = new LogisticRegression();
    try {
        const X = [], y = [];
        const c0 = [[0, 0], [1, 0], [0, 1], [1, 1], [0.5, 0.5]];
        const c1 = [[5, 5], [4, 5], [5, 4], [4, 4], [4.5, 4.5]];
        for (const p of c0) { X.push(p); y.push(0); }
        for (const p of c1) { X.push(p); y.push(1); }
        await m.fit(X, y);
        const labels = await m.predict([[0, 0], [5, 5], [1, 0.5], [4.5, 5]]);
        assert(labels[0] === 0, "origin -> class 0");
        assert(labels[1] === 1, "far corner -> class 1");
        assert(labels[2] === 0, "near-origin -> class 0");
        assert(labels[3] === 1, "near-far -> class 1");
        const proba = await m.predictProba([[0, 0], [5, 5]]);
        assert(proba[0][1] >= 0 && proba[0][1] < 0.5, "P(class1|origin) < 0.5");
        assert(proba[1][1] > 0.5 && proba[1][1] <= 1, "P(class1|far) > 0.5");
        assert(Math.abs(proba[0][0] + proba[0][1] - 1) < 1e-12, "row sums to 1");
    } finally { m.close(); }
});

await __kitS.section("predict before fit throws", async () => {
    const m = new LogisticRegression();
    try {
        let threw = false;
        try { await m.predict([[1, 2]]); } catch { threw = true; }
        assert(threw, "predict before fit throws");
    } finally { m.close(); }
});

await __kitS.section("logreg.predict coerce-then-close is caught (no UAF)", async () => {
    const m = new LogisticRegression();
    await m.fit([[0], [1], [5], [6]], [0, 0, 1, 1]);
    let threw = false;
    try {
        await m.predict([[{ valueOf() { m.close(); return 1; } }]]);
    } catch { threw = true; }
    assert(threw, "logreg.predict coerce-then-close is caught (no UAF)");
    m.close();
});

await __kitS.section("(got", async () => {
    async function throwsMsgC6(fn, re, m) {
        let threw = null;
        try { await fn(); } catch (e) { threw = e; }
        assert(threw !== null && threw instanceof TypeError && re.test(threw.message),
               m + " (got " + (threw ? threw.name + ": " + threw.message : "no throw") + ")");
    }
    await throwsMsgC6(() => new LogisticRegression({ C: 1, l2: 0.1, classWieght: "balanced" }),
                /unknown option "classWieght" \(valid: learningRate, tol, l1, l2, maxIter, C, penalty, classWeight\)/,
                "LogisticRegression bag");
    await throwsMsgC6(() => new SVC({ kernel: "rbf", gamm: 0.1 }),
                /unknown option "gamm" \(valid: kernel, C, gamma, coef0, tol, degree, maxIter\)/,
                "SVC bag");
    await throwsMsgC6(async () => await trainTestSplit(10, { testSize: 0.2, seeds: 1 }),
                /unknown option "seeds" \(valid: seed, shuffle, testSize\)/,
                "trainTestSplit bag");
    await throwsMsgC6(async () => await kFold(10, { k: 3, scoring: () => 0 }),
                /unknown option "scoring" \(valid: seed, shuffle, k, folds\)/,
                "kFold bag refuses crossValScore-only keys");
    await throwsMsgC6(async () => await new LinearRegression().fit([[1], [2]], [0, 1], { sampleWeight2: [1, 1] }),
                /unknown option "sampleWeight2" \(valid: sampleWeight\)/,
                "fit() bag");
    await throwsMsgC6(async () => await gridSearch(() => new KMeans(2), [[1], [2], [3]], [0, 1, 0],
                                 { nClusters: [2] }, { nItr: 3 }),
                /unknown option "nItr" \(valid: scoring, seed, nIter, shuffle, k, folds, nJobs, onProgress\)/,
                "gridSearch bag");
    const lr = new LogisticRegression({ C: 2, penalty: "l2", maxIter: 100 });
    await lr.fit([[0], [1], [2]], [0, 1, 1]);
    assert((await lr.predict([[1.5]]))[0] === 1, "valid LogisticRegression bag still trains");
    const sp = await trainTestSplit(10, { testSize: 0.25, seed: 7 });
    assert(sp.train.length + sp.test.length === 10, "valid trainTestSplit bag unchanged");
});
__kitS.end();

print("test_ml: all tests passed (" + n + " assertions)");
