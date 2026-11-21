// covers: dyna:ml.KNClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_kn_classifier_neighbors");
import { KNClassifier, KNRegressor, DBScan } from "dyna:ml";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function approx(a, b, eps, msg) {
    n++;
    if (!(Math.abs(a - b) <= eps)) throw new Error(`approx failed: ${msg} (${a} vs ${b})`);
}
async function throws(fn, msg, ErrType) {
    n++;
    try { await fn(); } catch (e) {
        if (ErrType && !(e instanceof ErrType))
            throw new Error(`${msg}: wrong error type ${e.constructor.name}: ${e.message}`);
        return;
    }
    throw new Error("expected a throw: " + msg);
}
function refKnn(Xtrain, ytrain, k, weighted, regressor, q) {
    const d = Xtrain.map((row, i) => {
        let s = 0;
        for (let j = 0; j < row.length; j++) { const t = row[j] - q[j]; s += t * t; }
        return { d: s, y: ytrain[i], i };
    });
    d.sort((a, b) => (a.d - b.d) || (a.i - b.i));
    const near = d.slice(0, k);
    if (regressor) {
        if (weighted) {
            const hit = near.find((e) => e.d === 0);
            if (hit) return hit.y;
            let num = 0, den = 0;
            for (const e of near) { const w = 1 / Math.sqrt(e.d); num += w * e.y; den += w; }
            return num / den;
        }
        return near.reduce((s, e) => s + e.y, 0) / k;
    }
    const score = new Map();
    for (const e of near) {
        const w = weighted ? (e.d === 0 ? Infinity : 1 / Math.sqrt(e.d)) : 1;
        score.set(e.y, (score.get(e.y) || 0) + w);
    }
    let bestLab = null, bestScore = -1;
    for (const [lab, sc] of score)
        if (sc > bestScore || (sc === bestScore && lab < bestLab)) { bestScore = sc; bestLab = lab; }
    return bestLab;
}

await __kitS.section("fit returns this", async () => {
    const X = [[0, 0], [0, 1], [1, 0], [10, 10], [10, 11], [11, 10]];
    const y = [0, 0, 0, 1, 1, 1];
    const m = new KNClassifier(3);
    try {
        assert((await m.fit(X, y)) === m, "fit returns this");
        const p = await m.predict([[0.5, 0.5], [10.5, 10.5], [0, 0]]);
        assert(Array.isArray(p) && p.length === 3, "predict returns an Array of 3");
        assert(p[0] === 0, "near blob A -> 0");
        assert(p[1] === 1, "near blob B -> 1");
        assert(p[2] === 0, "an exact training point -> its own label");
        const m1 = new KNClassifier(1);
        try {
            await m1.fit(X, y);
            const self = await m1.predict(X);
            for (let i = 0; i < X.length; i++)
                assert(self[i] === y[i], `k=1 reproduces training label ${i}`);
        } finally { m1.close(); }
    } finally { m.close(); }
});

await __kitS.section("expected 624 differential cases", async () => {
    function rng(seed) {
        let s = seed >>> 0;
        return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    }
    let cases = 0;
    for (const cols of [1, 3, 8, 17]) {
        for (const k of [1, 3, 5]) {
            for (const weighted of [false, true]) {
                const rnd = rng(9000 + cols * 31 + k * 7 + (weighted ? 1 : 0));
                const rows = 40;
                const X = [], y = [];
                for (let i = 0; i < rows; i++) {
                    const row = [];
                    for (let j = 0; j < cols; j++) row.push(Math.round(rnd() * 20) / 2);
                    X.push(row);
                    y.push(Math.floor(rnd() * 3));
                }
                const clf = new KNClassifier(k, weighted ? "distance" : "uniform");
                const reg = new KNRegressor(k, weighted ? "distance" : "uniform");
                try {
                    await clf.fit(X, y);
                    await reg.fit(X, y);
                    const Q = [];
                    for (let t = 0; t < 25; t++) {
                        const row = [];
                        for (let j = 0; j < cols; j++) row.push(Math.round(rnd() * 20) / 2);
                        Q.push(row);
                    }
                    Q.push(X[0]);
                    const gotC = await clf.predict(Q), gotR = await reg.predict(Q);
                    for (let t = 0; t < Q.length; t++) {
                        cases++;
                        const wantC = refKnn(X, y, k, weighted, false, Q[t]);
                        const wantR = refKnn(X, y, k, weighted, true, Q[t]);
                        assert(gotC[t] === wantC,
                            `clf cols=${cols} k=${k} w=${weighted} q=${t}: ${gotC[t]} vs ${wantC}`);
                        approx(gotR[t], wantR, 1e-9,
                            `reg cols=${cols} k=${k} w=${weighted} q=${t}`);
                    }
                } finally { clf.close(); reg.close(); }
            }
        }
    }
    assert(cases === 24 * 26, "expected 624 differential cases, got " + cases);
});

await __kitS.section("k must be positive", async () => {
    await throws(() => new KNClassifier(0), "k must be positive", RangeError);
    await throws(() => new KNClassifier(-1), "negative k", RangeError);
    await throws(() => new KNClassifier(3, "nearest"), "bad weights string", TypeError);
    const m = new KNClassifier(5);
    try {
        await throws(async () => await m.fit([[1], [2]], [0, 1]), "fewer rows than k", RangeError);
        await throws(async () => await m.predict([[1]]), "predict before fit");
        await m.fit([[1], [2], [3], [4], [5]], [0, 0, 1, 1, 1]);
        await throws(async () => await m.predict([[1, 2]]), "feature count mismatch", TypeError);
        await throws(async () => await m.fit([[1], [2], [3], [4], [5]], [0, 1]), "y length mismatch", TypeError);
    } finally { m.close(); }
});

await __kitS.section("reentrant close during predict", async () => {
    const m = new KNClassifier(1);
    await m.fit([[1], [2]], [0, 1]);
    await throws(async () => await m.predict([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during predict");
    assert(m.closed === true, "closed by the hostile valueOf");
});

await __kitS.section("wrong error", async () => {
    const X = [[0], [1], [2], [3]], y = [0, 0, 1, 1];
    const m = new KNClassifier(3); await m.fit(X, y);
    const throwsRange = async (fn, msg) => {
        n++;
        try { await fn(); } catch (e) {
            if (!(e instanceof RangeError))
                throw new Error(msg + ": wrong error " + e.constructor.name);
            return;
        }
        throw new Error("expected a throw: " + msg);
    };
    await throwsRange(async () => await m.predict([[NaN]]), "knn predict NaN refusal");
});
__kitS.end();

print("test_ml_neighbors: all tests passed (" + n + " assertions)");
