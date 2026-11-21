// covers: dyna:ml.gridSearch
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_grid_search_production");
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

await __kitS.section("3 x 2 = 6 points", async () => {
    const [X, y] = blobs(200, 2, 11, 1.2);
    const g = await ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                            { maxDepth: [1, 2, 4], minSamplesLeaf: [1, 5] },
                            { folds: 3 });
    eq(g.results.length, 6, "3 x 2 = 6 points");
    assert("maxDepth" in g.best && "minSamplesLeaf" in g.best,
           "best names both parameters");
    assert(g.bestScore >= Math.max(...g.results.map((r) => r.mean)) - 1e-12,
           "bestScore is the best mean");
    for (const r of g.results) {
        eq(r.scores.length, 3, "every point has a score per fold");
        near(r.mean, Array.from(r.scores).reduce((a, b) => a + b, 0) / 3, 1e-12,
             "mean matches its scores");
    }
    const shallow = g.results.find((r) => r.params.maxDepth === 1);
    assert(g.bestScore >= shallow.mean, "the search actually compares points");

    await throws(async () => await ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), X, y, {}),
           "an empty grid");
    await throws(async () => await ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                               { maxDepth: 4 }), "a non-array grid value");
    await throws(async () => await ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                               { maxDepth: [] }), "an empty value list");
});
__kitS.end();
console.log("test_ml_production.js: " + n + " assertions passed");
