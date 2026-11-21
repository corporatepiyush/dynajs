// covers: dyna:ml.GaussianNB
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gaussian_nb_decomposition");
import { PCA, GaussianNB, StandardScaler } from "dyna:ml";

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

await __kitS.section("fit returns this", async () => {
    const X = [[1, 1], [1.2, 0.9], [0.8, 1.1], [5, 5], [5.2, 4.8], [4.9, 5.1]];
    const y = [0, 0, 0, 1, 1, 1];
    const m = new GaussianNB();
    try {
        assert((await m.fit(X, y)) === m, "fit returns this");
        const cls = m.classes;
        assert(cls.length === 2 && cls[0] === 0 && cls[1] === 1, "classes ascending");
        const p = await m.predict([[1, 1], [5, 5], [1.1, 1]]);
        assert(p[0] === 0 && p[1] === 1 && p[2] === 0, "separated classes predicted");
        const pr = await m.predictProba([[1, 1], [5, 5]]);
        assert(pr.length === 2 && pr[0].length === 2, "proba is rows x classes");
        for (const row of pr)
            approx(row[0] + row[1], 1, 1e-12, "each probability row sums to 1");
        assert(pr[0][0] > 0.99, "confident about class 0");
        assert(pr[1][1] > 0.99, "confident about class 1");
    } finally { m.close(); }
});

await __kitS.section("two classes", async () => {
    const m = new GaussianNB();
    try {
        await m.fit([[0], [0.1], [9], [9.1]], [7, 7, -3.5, -3.5]);
        const cls = m.classes;
        assert(cls.length === 2, "two classes");
        approx(cls[0], -3.5, 0, "classes are sorted ascending: -3.5 first");
        approx(cls[1], 7, 0, "and 7 second");
        const p = await m.predict([[0], [9]]);
        approx(p[0], 7, 0, "predicts the original label value, not an index");
        approx(p[1], -3.5, 0, "and the negative label too");
    } finally { m.close(); }
});

await __kitS.section("block L53", async () => {
    const rows = 60, cols = 40;
    const X = [], y = [];
    let s = 12345;
    const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    for (let i = 0; i < rows; i++) {
        const c = i % 3;
        const row = [];
        for (let j = 0; j < cols; j++) row.push(c * 10 + rnd());
        X.push(row); y.push(c);
    }
    const m = new GaussianNB();
    try {
        await m.fit(X, y);
        const p = await m.predict(X);
        let correct = 0;
        for (let i = 0; i < rows; i++) if (p[i] === y[i]) correct++;
        assert(correct === rows, `all ${rows} training points classified, got ${correct}`);
        const pr = await m.predictProba(X);
        for (let i = 0; i < rows; i++) {
            let sum = 0;
            for (let c = 0; c < 3; c++) {
                assert(Number.isFinite(pr[i][c]), `proba finite at [${i}][${c}]`);
                assert(pr[i][c] >= 0 && pr[i][c] <= 1, `proba in range at [${i}][${c}]`);
                sum += pr[i][c];
            }
            approx(sum, 1, 1e-12, `row ${i} sums to 1 with 40 features`);
        }
    } finally { m.close(); }
});

await __kitS.section("still classifies with a constant feature", async () => {
    const m = new GaussianNB();
    try {
        await m.fit([[1, 5], [1, 6], [2, 50], [2, 60]], [0, 0, 1, 1]);
        const p = await m.predict([[1, 5], [2, 55]]);
        assert(p[0] === 0 && p[1] === 1, "still classifies with a constant feature");
        const pr = await m.predictProba([[1, 5]]);
        assert(Number.isFinite(pr[0][0]) && Number.isFinite(pr[0][1]),
               "probabilities stay finite (no 1/0)");
        approx(pr[0][0] + pr[0][1], 1, 1e-12, "and still normalised");
    } finally { m.close(); }
});

await __kitS.section("one class", async () => {
    const m = new GaussianNB();
    try {
        await m.fit([[1], [2], [3]], [4, 4, 4]);
        assert(m.classes.length === 1, "one class");
        const p = await m.predict([[1], [100]]);
        approx(p[0], 4, 0, "everything is the only class");
        approx(p[1], 4, 0, "even far from the data");
        approx((await m.predictProba([[100]]))[0][0], 1, 1e-12, "its probability is 1");
    } finally { m.close(); }
});

await __kitS.section("predict before fit", async () => {
    const m = new GaussianNB();
    try {
        await throws(async () => await m.predict([[1]]), "predict before fit");
        await m.fit([[1], [2]], [0, 1]);
        await throws(async () => await m.predict([[1, 2]]), "wrong feature count", TypeError);
        await throws(async () => await m.fit([[1], [2]], [0]), "y length mismatch", TypeError);
    } finally { m.close(); }
    await throws(() => new GaussianNB(-1), "negative varSmoothing", RangeError);
});

await __kitS.section("reentrant close during predictProba", async () => {
    const m = new GaussianNB();
    await m.fit([[1], [2]], [0, 1]);
    await throws(async () => await m.predictProba([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during predictProba");
    assert(m.closed === true, "closed");
});

await __kitS.section("gaussianNB variance overflow", async () => {
    await throws(async () => await new GaussianNB().fit([[1e200, 1], [1e200, 2]], [0, 1]),
           "gaussianNB variance overflow", RangeError);
});
__kitS.end();

print("test_ml_decomposition: all tests passed (" + n + " assertions)");
