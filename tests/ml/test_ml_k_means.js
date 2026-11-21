// covers: dyna:ml.KMeans
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_k_means");
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

await __kitS.section("fit returns this", async () => {
    const m = new KMeans(2, 7);
    try {
        const X = [
            [0, 0], [0.2, -0.1], [-0.1, 0.2], [0.1, 0.1],
            [10, 10], [10.2, 9.9], [9.8, 10.1], [10.1, 10.0],
        ];
        assert((await m.fit(X)) === m, "fit returns this");
        const labels = await m.predict(X);
        const a = labels[0], b = labels[4];
        assert(a !== b, "the two blobs get different clusters");
        for (let i = 0; i < 4; i++) assert(labels[i] === a, "blob A cohesive @" + i);
        for (let i = 4; i < 8; i++) assert(labels[i] === b, "blob B cohesive @" + i);
        assert(typeof m.inertia === "number", "inertia is a number");
        assert(m.inertia >= 0 && m.inertia < 1, "tight blobs => small inertia");
        const q = await m.predict([[9.9, 10.0]]);
        assert(q[0] === b, "query near B -> B");
    } finally { m.close(); }
});

__kitS.section("inertia 0 before fit", () => {
    const m = new KMeans();
    try {
        assert(m.inertia === 0, "inertia 0 before fit");
    } finally { m.close(); }
});

await __kitS.section("fewer rows than clusters throws", async () => {
    const m = new KMeans(5);
    try {
        let threw = false;
        try { await m.fit([[1], [2]]); } catch { threw = true; }
        assert(threw, "fewer rows than clusters throws");
    } finally { m.close(); }
});

await __kitS.section("kmeans.fit coerce-then-close is caught (no UAF)", async () => {
    const m = new KMeans(2, 1);
    let threw = false;
    try {
        await m.fit([[{ valueOf() { m.close(); return 1; } }], [2], [3]]);
    } catch { threw = true; }
    assert(threw, "kmeans.fit coerce-then-close is caught (no UAF)");
    m.close();
});

await __kitS.section("kmeans flat predict close-during-coerce is caught (no UAF)", async () => {
    const m = new KMeans(2, 1);
    await m.fit([[0], [1], [10], [11]], undefined);
    const Xf = new Float64Array([0, 10, 1]);
    let threw = false;
    try {
        await m.predict(Xf, { valueOf() { m.close(); return 3; } }, 1);
    } catch { threw = true; }
    assert(threw, "kmeans flat predict close-during-coerce is caught (no UAF)");
    m.close();
});

await __kitS.section("wrong error", async () => {
    const X = [[1], [2], [3], [4]], y = [1, 2, 3, 4];
    const throwsRange = async (fn, msg) => {
        n++;
        try { await fn(); } catch (e) {
            if (!(e instanceof RangeError))
                throw new Error(msg + ": wrong error " + e.constructor.name);
            return;
        }
        throw new Error("expected a throw: " + msg);
    };
    await throwsRange(() => new LogisticRegression({ maxIter: 100001 }),
                "logreg maxIter cap");
    await throwsRange(async () => await new KMeans(80).fit(Array.from({ length: 100 }, (_, i) => [i])),
                "kmeans k near rows refusal");
    assert((await new KMeans(50).fit(Array.from({ length: 100 }, (_, i) => [i]))).inertia >= 0,
           "kmeans k <= rows/2 still fits");
    await throwsRange(async () => {
        const w = new LinearRegression();
        await w.fit(X, y, { sampleWeight: [1e308, 1e308, 1e308, 1e308] });
    }, "sampleWeight sum overflow refusal");
    const km = new KMeans(2, { seed: 1 }); await km.fit(X);
    await throwsRange(async () => await km.predict([[NaN]]), "kmeans predict NaN refusal");
});
__kitS.end();

print("test_ml: all tests passed (" + n + " assertions)");
