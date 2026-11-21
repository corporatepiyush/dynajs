// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_production");
import * as ml from "dyna:ml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function near(got, want, tol, msg) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
async function throws(fn, msg) {
    n++;
    try { await fn(); } catch (e) { return e; }
    throw new Error("assertion failed: " + msg + " did not throw");
}

function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s / 4294967296; };
}
function blobs(rows, k, seed, spread) {
    const r = rng(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const c = i % k;
        X.push([c * spread + r() * 2, c * spread + r() * 2, r()]);
        y.push(c);
    }
    return [X, y];
}

await __kitS.section("classes are recorded", async () => {
    const [X, y] = blobs(300, 3, 11, 3);
    const m = new ml.LogisticRegression({ maxIter: 4000 });
    await m.fit(X, y);
    eq(JSON.stringify(Array.from(m.classes)), "[0,1,2]", "classes are recorded");
    eq(m.coef.length, 3, "one weight vector per class");
    eq(m.coef[0].length, 3, "each of n_features wide");
    eq(m.intercept.length, 3, "one intercept per class");
    assert((await ml.accuracy(y, await m.predict(X))) > 0.95, "3-class fit works");

    const proba = await m.predictProba(X);
    eq(proba[0].length, 3, "predictProba is rows x n_classes");
    for (let i = 0; i < 20; i++) {
        let s = 0, best = 0;
        for (let k = 0; k < 3; k++) {
            s += proba[i][k];
            assert(proba[i][k] >= 0 && proba[i][k] <= 1, "probability in [0,1]");
            if (proba[i][k] > proba[i][best]) best = k;
        }
        near(s, 1, 1e-12, "row " + i + " sums to 1");
        eq(m.classes[best], (await m.predict(X))[i], "predict === argmax(predictProba)");
    }
    const [X4, y4] = blobs(200, 4, 13, 3);
    const relabel = y4.map((c) => [10, 20, 30, 40][c]);
    const m4 = new ml.LogisticRegression({ maxIter: 3000 });
    await m4.fit(X4, relabel);
    eq(JSON.stringify(Array.from(m4.classes)), "[10,20,30,40]",
       "arbitrary labels are preserved, not renumbered");
    assert((await m4.predict(X4)).every((v) => [10, 20, 30, 40].includes(v)),
           "and predictions come back in those labels");
});

await __kitS.section("two classes: one weight vector", async () => {
    const [X, y] = blobs(200, 2, 7, 3);
    const m = new ml.LogisticRegression();
    await m.fit(X, y);
    eq(m.coef.length, 1, "two classes: one weight vector");
    eq(typeof m.intercept, "number", "and a scalar intercept");
    eq((await m.predictProba(X))[0].length, 2, "but still two probability columns");
    const m2 = new ml.LogisticRegression();
    await m2.fit(X, y.map((c) => c + 1));
    eq(JSON.stringify(Array.from(m2.classes)), "[1,2]", "labels 1 and 2");
    assert((await ml.accuracy(y.map((c) => c + 1), await m2.predict(X))) > 0.95,
           "and it separates them (reading `y != 0` could not)");
});

await __kitS.section("a regularised fit on overlapping data converges", async () => {
    const [Xo, yo] = blobs(200, 2, 7, 0.2);
    const [Xs, ys] = blobs(200, 2, 7, 3);

    const conv = new ml.LogisticRegression({ l2: 0.1 });
    await conv.fit(Xo, yo);
    assert(conv.converged, "a regularised fit on overlapping data converges");
    assert(conv.nIter < 3000, "and stops early (" + conv.nIter + " iterations)");

    const sep = new ml.LogisticRegression();
    await sep.fit(Xs, ys);
    eq(sep.converged, false,
       "separable data with no penalty has NO finite optimum, so it runs out");
    eq(sep.nIter, 3000, "to exactly maxIter");
    assert((await ml.accuracy(ys, await sep.predict(Xs))) === 1,
           "which is not a failure: it still separates perfectly");

    const capped = new ml.LogisticRegression({ maxIter: 25 });
    await capped.fit(Xo, yo);
    assert(capped.nIter <= 25, "maxIter caps the run");
    const loose = new ml.LogisticRegression({ l2: 0.1, tol: 1e-2 });
    await loose.fit(Xo, yo);
    const tight = new ml.LogisticRegression({ l2: 0.1, tol: 1e-6 });
    await tight.fit(Xo, yo);
    assert(loose.nIter < tight.nIter,
           "a looser tol stops sooner (" + loose.nIter + " vs " + tight.nIter + ")");
});

await __kitS.section("L2 shrinks the weights", async () => {
    const [X, y] = blobs(200, 2, 7, 3);
    const norm = (m) => m.coef[0].reduce((a, b) => a + b * b, 0);
    const plain = new ml.LogisticRegression({ maxIter: 500 });
    await plain.fit(X, y);
    const l2 = new ml.LogisticRegression({ maxIter: 500, l2: 1.0 });
    await l2.fit(X, y);
    assert(norm(l2) < norm(plain),
           "L2 shrinks the weights (" + norm(l2).toFixed(3) + " < " + norm(plain).toFixed(3) + ")");
    const l1 = new ml.LogisticRegression({ maxIter: 500, l1: 1.0 });
    await l1.fit(X, y);
    assert(norm(l1) < norm(plain), "L1 shrinks them too");

    const strong = new ml.LogisticRegression({ maxIter: 500, penalty: "l2", C: 0.01 });
    await strong.fit(X, y);
    const weak = new ml.LogisticRegression({ maxIter: 500, penalty: "l2", C: 100 });
    await weak.fit(X, y);
    assert(norm(strong) < norm(weak),
           "a smaller C penalises harder (" + norm(strong).toFixed(4) +
           " < " + norm(weak).toFixed(4) + ")");
    const none = new ml.LogisticRegression({ maxIter: 500, penalty: "none" });
    await none.fit(X, y);
    near(norm(none), norm(plain), 1e-9, 'penalty:"none" equals no penalty');
    await throws(() => new ml.LogisticRegression({ penalty: "quadratic" }), "an unknown penalty");
});

await __kitS.section("balancing does not reduce minority recall", async () => {
    const r = rng(31), X = [], y = [];
    for (let i = 0; i < 400; i++) {
        const minority = i % 40 === 0;
        X.push([minority ? 2 + r() : r(), r(), r()]);
        y.push(minority ? 1 : 0);
    }
    const plain = new ml.LogisticRegression({ maxIter: 800 });
    await plain.fit(X, y);
    const bal = new ml.LogisticRegression({ maxIter: 800, classWeight: "balanced" });
    await bal.fit(X, y);
    const recall = async (m) => await ml.recall(y, await m.predict(X));
    assert((await recall(bal)) >= (await recall(plain)),
           "balancing does not reduce minority recall (" +
           (await recall(plain)).toFixed(3) + " -> " + (await recall(bal)).toFixed(3) + ")");
    await throws(() => new ml.LogisticRegression({ classWeight: "inverse" }),
           "an unknown classWeight");
});

await __kitS.section("-class row", async () => {
    for (const K of [2, 3, 5]) {
        const [X, y] = blobs(150, K, 17 + K, 3);
        const m = new ml.LogisticRegression({ maxIter: 400, l2: 0.05,
                                              classWeight: "balanced" });
        await m.fit(X, y);
        const before = await m.predictProba(X);
        const back = ml.LogisticRegression.deserialize(m.serialize());
        const after = await back.predictProba(X);
        for (let i = 0; i < before.length; i++)
            for (let k = 0; k < K; k++)
                eq(after[i][k], before[i][k],
                   K + "-class row " + i + " col " + k + " is BIT-identical");
        eq(JSON.stringify(Array.from(back.classes)),
           JSON.stringify(Array.from(m.classes)), "classes survive");
        eq(back.nIter, m.nIter, "the iteration count survives");
        eq(back.converged, m.converged, "and so does whether it converged");
        const a = m.serialize(), b = back.serialize();
        eq(a.length, b.length, "re-encode length");
        let same = true;
        for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) same = false;
        assert(same, K + "-class record re-encodes byte-identically");
    }
});

await __kitS.section("a single-label y has no log-odds to fit", async () => {
    const [X, y] = blobs(40, 2, 5, 3);
    await throws(async () => await new ml.LogisticRegression().fit(X, y.map(() => 1)),
           "a single-label y has no log-odds to fit");
    await throws(async () => await new ml.LogisticRegression().predict(X), "predict before fit");
    await throws(() => new ml.LogisticRegression({ maxIter: 0 }), "maxIter 0");
    await throws(() => new ml.LogisticRegression(5), "a non-object option");
    const m = new ml.LogisticRegression();
    await m.fit(X, y);
    await throws(async () => await m.predict([[1, 2]]), "the wrong feature count");
});
__kitS.end();
console.log("test_ml_production.js: " + n + " assertions passed");
