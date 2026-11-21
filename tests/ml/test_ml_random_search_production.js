// covers: dyna:ml.randomSearch
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_random_search_production");
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

await __kitS.section("nIter points, not the full 20", async () => {
    const [X, y] = blobs(150, 2, 13, 1.2);
    const grid = { maxDepth: [1, 2, 4, 8, 16], minSamplesLeaf: [1, 3, 5, 9] };
    const a = await ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y, grid,
                              { folds: 3, nIter: 5, seed: 7 });
    eq(a.results.length, 5, "nIter points, not the full 20");
    const b = await ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y, grid,
                              { folds: 3, nIter: 5, seed: 7 });
    eq(JSON.stringify(a.results.map((r) => r.params)),
       JSON.stringify(b.results.map((r) => r.params)),
       "the same seed samples the same points");
    const c = await ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y, grid,
                              { folds: 3, nIter: 5, seed: 99 });
    assert(JSON.stringify(a.results.map((r) => r.params)) !==
           JSON.stringify(c.results.map((r) => r.params)),
           "a different seed samples different points");
    const full = await ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                                 { maxDepth: [1, 2] }, { folds: 2, nIter: 100 });
    eq(full.results.length, 2, "nIter is capped at the grid size");
    await throws(async () => await ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                                 grid, { nIter: 0 }), "nIter 0");
});
__kitS.end();
console.log("test_ml_production.js: " + n + " assertions passed");
