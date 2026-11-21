// covers: dyna:ml.DecisionTreeClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_classifier_trees");
import { DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingRegressor,
         accuracy, meanSquaredError } from "dyna:ml";

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
function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}
function xorData(perQuadrant, seed) {
    const rnd = rng(seed);
    const X = [], y = [];
    for (const [cx, cy, lab] of [[0, 0, 0], [1, 1, 0], [0, 1, 1], [1, 0, 1]])
        for (let i = 0; i < perQuadrant; i++) {
            X.push([cx + (rnd() - 0.5) * 0.3, cy + (rnd() - 0.5) * 0.3]);
            y.push(lab);
        }
    return { X, y };
}

await __kitS.section("fit returns this", async () => {
    const X = [[1, 1], [1.2, 0.9], [0.8, 1.1], [5, 5], [5.2, 4.8], [4.9, 5.1]];
    const y = [0, 0, 0, 1, 1, 1];
    const m = new DecisionTreeClassifier();
    try {
        assert((await m.fit(X, y)) === m, "fit returns this");
        const p = await m.predict(X);
        for (let i = 0; i < X.length; i++)
            assert(p[i] === y[i], `separable data fitted exactly @${i}`);
        assert(m.depth === 1, "one split suffices here, depth = " + m.depth);
        const q = await m.predict([[1.1, 1], [5.1, 5]]);
        assert(q[0] === 0 && q[1] === 1, "unseen points on the right side");
    } finally { m.close(); }
});

await __kitS.section("XOR forces depth >= 2", async () => {
    const { X, y } = xorData(15, 4242);
    const deep = new DecisionTreeClassifier();
    const stump = new DecisionTreeClassifier({ maxDepth: 1 });
    try {
        await deep.fit(X, y);
        await stump.fit(X, y);
        assert(deep.depth >= 2, "XOR forces depth >= 2, got " + deep.depth);
        assert(stump.depth === 1, "maxDepth caps the depth at 1, got " + stump.depth);
        const accDeep = await accuracy(y, await deep.predict(X));
        const accStump = await accuracy(y, await stump.predict(X));
        approx(accDeep, 1, 1e-12, "an unconstrained tree separates XOR exactly");
        assert(accStump <= 0.75, "a stump cannot separate XOR: " + accStump);
        assert(accDeep > accStump, "depth helps on XOR");
    } finally { deep.close(); stump.close(); }
});

await __kitS.section("a large minSamplesLeaf keeps the tree shallow", async () => {
    const { X, y } = xorData(20, 99);
    const a = new DecisionTreeClassifier({ minSamplesLeaf: 30 });
    const b = new DecisionTreeClassifier({ minSamplesSplit: 200 });
    try {
        await a.fit(X, y);
        await b.fit(X, y);
        assert(a.depth <= 2, "a large minSamplesLeaf keeps the tree shallow: " + a.depth);
        assert(b.depth === 0, "minSamplesSplit above the row count gives a single leaf");
        const p = await b.predict(X);
        assert(p.every((v) => v === p[0]), "a root-only tree is constant");
    } finally { a.close(); b.close(); }
});

await __kitS.section("negative label preserved", async () => {
    const m = new DecisionTreeClassifier();
    try {
        await m.fit([[0], [1], [10], [11]], [-7, -7, 3.5, 3.5]);
        const p = await m.predict([[0.5], [10.5]]);
        approx(p[0], -7, 0, "negative label preserved");
        approx(p[1], 3.5, 0, "fractional label preserved");
    } finally { m.close(); }
});

await __kitS.section("Array and flat forms agree @", async () => {
    const rowsArr = [[1, 1], [1.2, 0.9], [5, 5], [5.2, 4.8]];
    const flat = new Float64Array([1, 1, 1.2, 0.9, 5, 5, 5.2, 4.8]);
    const y = [0, 0, 1, 1];
    const a = new DecisionTreeClassifier(), b = new DecisionTreeClassifier();
    try {
        await a.fit(rowsArr, y);
        await b.fit(flat, new Float64Array(y), 4, 2);
        const pa = await a.predict(rowsArr), pb = await b.predict(flat, 4, 2);
        for (let i = 0; i < 4; i++)
            assert(pa[i] === pb[i], "Array and flat forms agree @" + i);
    } finally { a.close(); b.close(); }
});

await __kitS.section("nEstimators >= 1", async () => {
    await throws(() => new RandomForestClassifier({ nEstimators: 0 }), "nEstimators >= 1", RangeError);
    await throws(() => new DecisionTreeClassifier({ minSamplesSplit: 1 }), "minSamplesSplit >= 2", RangeError);
    await throws(() => new DecisionTreeClassifier({ minSamplesLeaf: 0 }), "minSamplesLeaf >= 1", RangeError);
    await throws(() => new GradientBoostingRegressor({ subsample: 0 }), "subsample > 0", RangeError);
    await throws(() => new GradientBoostingRegressor({ subsample: 2 }), "subsample <= 1", RangeError);
    await throws(() => new DecisionTreeClassifier(42), "options must be an object", TypeError);
    let cc6 = null;
    try { new DecisionTreeClassifier({ someFutureKnob: 5 }); }
    catch (e) { cc6 = e; }
    assert(cc6 !== null && cc6 instanceof TypeError &&
           /unknown option "someFutureKnob" \(valid: /.test(cc6.message),
        "unknown ctor key throws naming key + valid set (got " +
        (cc6 && cc6.message) + ")");
});

await __kitS.section("reentrant close during tree fit", async () => {
    const m = new DecisionTreeClassifier();
    await throws(async () => await m.fit([[{ valueOf() { m.close(); return 1; } }], [2]], [0, 1]),
        "reentrant close during tree fit");
    assert(m.closed === true, "closed");
});
__kitS.end();

print("test_ml_trees: all tests passed (" + n + " assertions)");
