// covers: dyna:ml.DBScan
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_db_scan_neighbors");
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
    const X = [
        [0, 0], [0, 0.2], [0.2, 0], [0.1, 0.1],
        [5, 5], [5, 5.2], [5.2, 5], [5.1, 5.1],
        [50, 50],
    ];
    const m = new DBScan(1.0, 3);
    try {
        assert((await m.fit(X)) === m, "fit returns this");
        assert(m.nClusters === 2, "two clusters found, got " + m.nClusters);
        approx(m.eps, 1.0, 0, "eps getter");
        const L = m.labels;
        assert(L.length === 9, "one label per point");
        assert(L[8] === -1, "the far point is noise (-1)");
        for (let i = 1; i < 4; i++) assert(L[i] === L[0], "blob A is one cluster @" + i);
        for (let i = 5; i < 8; i++) assert(L[i] === L[4], "blob B is one cluster @" + i);
        assert(L[0] !== L[4], "the blobs are different clusters");
        assert(L[0] === 0 && L[4] === 1, "cluster ids are assigned in scan order");
    } finally { m.close(); }
});

await __kitS.section("no clusters", async () => {
    const m = new DBScan(0.1, 2);
    try {
        await m.fit([[0, 0], [10, 10], [20, 20]]);
        assert(m.nClusters === 0, "no clusters");
        assert(m.labels.every((l) => l === -1), "every point is noise");
    } finally { m.close(); }
});

await __kitS.section("each point its own cluster", async () => {
    const m = new DBScan(0.1, 1);
    try {
        await m.fit([[0, 0], [10, 10], [20, 20]]);
        assert(m.nClusters === 3, "each point its own cluster");
        assert(m.labels.every((l) => l >= 0), "nothing is noise when minPts is 1");
    } finally { m.close(); }
});

await __kitS.section("the chain is a single cluster", async () => {
    const X = [];
    for (let i = 0; i < 20; i++) X.push([i * 0.5, 0]);
    const m = new DBScan(0.6, 2);
    try {
        await m.fit(X);
        assert(m.nClusters === 1, "the chain is a single cluster, got " + m.nClusters);
        assert(m.labels.every((l) => l === 0), "every link in the chain is labelled 0");
        approx(X[19][0] - X[0][0], 9.5, 0, "the chain really is longer than eps");
    } finally { m.close(); }
});

__kitS.section("no labels before fit", () => {
    const m = new DBScan();
    try {
        assert(m.labels.length === 0, "no labels before fit");
        assert(m.nClusters === 0, "no clusters before fit");
        approx(m.eps, 0.5, 0, "default eps");
    } finally { m.close(); }
});

await __kitS.section("same cluster count", async () => {
    const rowsArr = [[0, 0], [0, 0.1], [5, 5], [5, 5.1]];
    const flat = new Float64Array([0, 0, 0, 0.1, 5, 5, 5, 5.1]);
    const a = new DBScan(1, 2), b = new DBScan(1, 2);
    try {
        await a.fit(rowsArr);
        await b.fit(flat, 4, 2);
        assert(a.nClusters === b.nClusters, "same cluster count");
        for (let i = 0; i < 4; i++)
            assert(a.labels[i] === b.labels[i], "same label at " + i);
    } finally { a.close(); b.close(); }
});

await __kitS.section("eps must be positive", async () => {
    await throws(() => new DBScan(0), "eps must be positive", RangeError);
    await throws(() => new DBScan(-1), "negative eps", RangeError);
    await throws(() => new DBScan(1, 0), "minPts at least 1", RangeError);
});

await __kitS.section("reentrant close during DBScan fit", async () => {
    const m = new DBScan(1, 1);
    await throws(async () => await m.fit([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during DBScan fit");
    assert(m.closed === true, "closed");
});
__kitS.end();

print("test_ml_neighbors: all tests passed (" + n + " assertions)");
