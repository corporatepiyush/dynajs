// covers: dyna:ml.PCA
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_pca_decomposition");
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
    const X = [[-2, -2], [-1, -1], [0, 0], [1, 1], [2, 2]];
    const p = new PCA();
    try {
        assert((await p.fit(X)) === p, "fit returns this");
        approx(p.mean[0], 0, 1e-12, "mean[0]");
        approx(p.mean[1], 0, 1e-12, "mean[1]");
        const c = p.components;
        assert(c.length === 2 && c[0].length === 2, "2 components of 2 features");
        const r = Math.SQRT1_2;
        approx(Math.abs(c[0][0]), r, 1e-9, "|component 0 [0]| = 1/sqrt(2)");
        approx(Math.abs(c[0][1]), r, 1e-9, "|component 0 [1]| = 1/sqrt(2)");
        assert(c[0][0] > 0 || c[0][1] > 0, "the leading entry is made positive");
        approx(c[0][0] * c[0][0] + c[0][1] * c[0][1], 1, 1e-9, "component 0 is a unit vector");
        approx(c[1][0] * c[1][0] + c[1][1] * c[1][1], 1, 1e-9, "component 1 is a unit vector");
        approx(c[0][0] * c[1][0] + c[0][1] * c[1][1], 0, 1e-9, "components are orthogonal");
        approx(p.explainedVariance[0], 5, 1e-9, "leading eigenvalue");
        approx(p.explainedVariance[1], 0, 1e-9, "second eigenvalue is 0 (degenerate data)");
        approx(p.explainedVarianceRatio[0], 1, 1e-9, "the first component explains everything");
        approx(p.explainedVarianceRatio[1], 0, 1e-9, "the second explains nothing");
    } finally { p.close(); }
});

await __kitS.section("block L43", async () => {
    const X = [[3, -1], [1, 4], [-2, 2], [5, 5], [0, -3]];
    const a = new PCA(), b = new PCA();
    try {
        await a.fit(X); await b.fit(X);
        for (let i = 0; i < 2; i++)
            for (let j = 0; j < 2; j++)
                approx(a.components[i][j], b.components[i][j], 0,
                       `component ${i}[${j}] is bit-identical across fits`);
    } finally { a.close(); b.close(); }
});

await __kitS.section("5x3 projection", async () => {
    const X = [[1, 2, 3], [4, 5, 7], [-1, 0, 2], [3, -2, 1], [0, 4, -3]];
    const p = new PCA();
    try {
        const t = await p.fitTransform(X);
        assert(t.length === 5 && t[0].length === 3, "5x3 projection");
        const back = await p.inverseTransform(t);
        for (let i = 0; i < 5; i++)
            for (let j = 0; j < 3; j++)
                approx(back[i][j], X[i][j], 1e-8, `round trip [${i}][${j}]`);
        for (let j = 0; j < 3; j++) {
            let s = 0;
            for (let i = 0; i < 5; i++) s += t[i][j];
            approx(s, 0, 1e-8, `projected column ${j} is centred`);
        }
        for (let a2 = 0; a2 < 3; a2++)
            for (let b2 = a2 + 1; b2 < 3; b2++) {
                let cov = 0;
                for (let i = 0; i < 5; i++) cov += t[i][a2] * t[i][b2];
                approx(cov, 0, 1e-7, `projected columns ${a2},${b2} are uncorrelated`);
            }
        const ev = p.explainedVariance;
        for (let a2 = 1; a2 < ev.length; a2++)
            assert(ev[a2] <= ev[a2 - 1] + 1e-12, "eigenvalues are descending");
        approx(p.explainedVarianceRatio.reduce((s, v) => s + v, 0), 1, 1e-9,
               "explained variance ratios sum to 1");
        for (let a2 = 0; a2 < 3; a2++) {
            let ss = 0;
            for (let i = 0; i < 5; i++) ss += t[i][a2] * t[i][a2];
            approx(ss / 4, ev[a2], 1e-8, `projected variance ${a2} = eigenvalue`);
        }
    } finally { p.close(); }
});

await __kitS.section("projected to 2 columns", async () => {
    const X = [[1, 2, 3], [4, 5, 7], [-1, 0, 2], [3, -2, 1], [0, 4, -3], [2, 2, 2]];
    const p = new PCA(2);
    try {
        const t = await p.fitTransform(X);
        assert(t[0].length === 2, "projected to 2 columns");
        assert(p.components.length === 2, "2 components stored");
        assert(p.explainedVariance.length === 2, "2 eigenvalues");
        const s = p.explainedVarianceRatio.reduce((a, v) => a + v, 0);
        assert(s > 0 && s < 1.0000001, "kept ratio is a fraction of the total: " + s);
        const back = await p.inverseTransform(t);
        assert(back[0].length === 3, "inverse returns full feature width");
    } finally { p.close(); }
});

await __kitS.section("block L104", async () => {
    const X = [[-2, 1], [-1, 3], [0, 0], [1, -3], [2, -1], [3, 2]];
    const p = new PCA(2, true);
    try {
        const t = await p.fitTransform(X);
        for (let a2 = 0; a2 < 2; a2++) {
            let ss = 0;
            for (let i = 0; i < 6; i++) ss += t[i][a2] * t[i][a2];
            approx(ss / 5, 1, 1e-7, `whitened column ${a2} has unit variance`);
        }
    } finally { p.close(); }
});

await __kitS.section("flat in -> Float64Array out", async () => {
    const flat = new Float64Array([-2, -2, -1, -1, 0, 0, 1, 1, 2, 2]);
    const p = new PCA(1);
    try {
        const t = await p.fitTransform(flat, 5, 2);
        assert(t instanceof Float64Array, "flat in -> Float64Array out");
        assert(t.length === 5, "5 rows x 1 component");
        approx(Math.abs(t[0]), 2 * Math.SQRT2, 1e-9, "extreme point projects farthest");
        approx(t[2], 0, 1e-9, "the centre point projects to 0");
    } finally { p.close(); }
});

await __kitS.section("more components than features", async () => {
    const p = new PCA(5);
    try {
        await throws(async () => await p.fit([[1, 2], [3, 4]]), "more components than features", RangeError);
    } finally { p.close(); }
    const q = new PCA();
    try {
        await throws(async () => await q.fit([[1, 2]]), "fewer than two rows", RangeError);
        await throws(async () => await q.transform([[1, 2]]), "transform before fit");
        await q.fit([[1, 2], [3, 5]]);
        await throws(async () => await q.transform([[1, 2, 3]]), "wrong feature count", TypeError);
    } finally { q.close(); }
    await throws(() => new PCA(-1), "negative nComponents", RangeError);
});

await __kitS.section("reentrant close during PCA fit", async () => {
    const p = new PCA();
    await throws(async () => await p.fit([[{ valueOf() { p.close(); return 1; } }, 2], [3, 4]]),
        "reentrant close during PCA fit");
    assert(p.closed === true, "closed");
});

await __kitS.section("pca fitTransform rejects NaN", async () => {
    const p = new PCA(1);
    await throws(async () => await p.fitTransform([[NaN, 1], [2, 3], [4, 5]]),
           "pca fitTransform rejects NaN", RangeError);
    const good = await new PCA(1).fitTransform([[1, 2], [3, 4], [5, 6]]);
    assert(Array.isArray(good) && good.length === 3, "clean fitTransform still works");
});
__kitS.end();

print("test_ml_decomposition: all tests passed (" + n + " assertions)");
