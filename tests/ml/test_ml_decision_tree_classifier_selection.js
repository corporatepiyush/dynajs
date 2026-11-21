// covers: dyna:ml.DecisionTreeClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_classifier_selection");
import { trainTestSplit, kFold, stratifiedKFold, crossValScore, gridSearch, DecisionTreeClassifier } from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (got !== want) throw new Error("assertion failed: " + m +
        "\n  got:  " + got + "\n  want: " + want);
}
async function throwsRange(fn, m) {
    n++;
    try { await fn(); } catch (e) {
        if (e instanceof RangeError) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected RangeError): " + m);
}
function assertPartition(train, test, N, label) {
    const all = [...train, ...test].sort((a, b) => a - b);
    eq(all.length, N, label + ": sizes sum to N");
    for (let i = 0; i < N; i++)
        if (all[i] !== i) throw new Error(label + ": not a partition of 0..N-1 at " + i);
    n++;
    const ts = new Set(test);
    for (const t of train)
        if (ts.has(t)) throw new Error(label + ": index " + t + " is in BOTH train and test");
    n++;
    for (const arr of [train, test])
        if (new Set(arr).size !== arr.length) throw new Error(label + ": duplicate indices");
    n++;
}

await __kitS.section("the detaching callback really fired", async () => {
    const rows = 200, cols = 3;
    function flatX(filler) {
        const X = new Float64Array(rows * cols);
        for (let i = 0; i < rows; i++)
            for (let j = 0; j < cols; j++)
                X[i * cols + j] = filler(i, j);
        return X;
    }
    const Xsrc = flatX((i, j) => ((i * (j + 3)) % 7) - 3);
    const y = [];
    for (let i = 0; i < rows; i++) y.push(i % 2 === 0 ? 1 : 0);
    const acc = (a, b) => {
        let hit = 0;
        for (let i = 0; i < a.length; i++) if (a[i] === b[i]) hit++;
        return a.length ? hit / a.length : 0;
    };

    const baseline = await crossValScore(
        () => new DecisionTreeClassifier({ maxDepth: 4 }), flatX((i, j) => Xsrc[i * cols + j]),
        y, { folds: 4, seed: 7, scoring: acc });

    let detached = false, refusal = "none";
    const Xd = flatX((i, j) => Xsrc[i * cols + j]);
    const detaching = await crossValScore(
        () => new DecisionTreeClassifier({ maxDepth: 4 }), Xd, y,
        { folds: 4, seed: 7, scoring: (a, b) => { if (!detached) { detached = true; try { Xd.buffer.transfer(); } catch (e) { refusal = e.name; } } return acc(a, b); } });

    eq(detached, true, "the detaching callback really fired");
    // The search works on a private snapshot taken at the call, so a transfer()
    // from a scoring callback is neither refused nor able to change the result.
    eq(refusal, "none", "X is not held by a running search: transfer() is allowed");
    eq(JSON.stringify(Array.from(detaching)), JSON.stringify(Array.from(baseline)),
       "an attempted detach mid-search must not change the scores");
    eq(Xd.length, 0, "the callback's transfer() detached the caller's X, and the search still finished");

    const b2 = await crossValScore(
        () => new DecisionTreeClassifier({ maxDepth: 4 }), flatX((i, j) => Xsrc[i * cols + j]),
        y, { folds: 4, seed: 7, scoring: acc });
    eq(JSON.stringify(Array.from(b2)), JSON.stringify(Array.from(baseline)),
       "the search is reproducible from its seed on flat X");

    let gDetached = false;
    const Xg = flatX((i, j) => Xsrc[i * cols + j]);
    const g = await gridSearch((p) => new DecisionTreeClassifier(p), Xg, y,
                         { maxDepth: [1, 2] },
                         { folds: 3, seed: 7, scoring: (a, b) => { if (!gDetached) { gDetached = true; try { Xg.buffer.transfer(); } catch (e) { } } return acc(a, b); } });
    eq(gDetached, true, "gridSearch detaching callback fired");
    eq(g.results.length, 2, "2 grid points, both scored after the detach attempt");
    for (const r of g.results) {
        eq(r.scores.length, 3, "every grid point yields a score per fold");
        for (const s of r.scores) assert(s >= 0 && s <= 1, "accuracy in [0,1] after detach");
    }

    const bad = new Float64Array(rows * cols + 1);
    let badThrew = false;
    try { await crossValScore(() => new DecisionTreeClassifier(), bad, y, { folds: 3 }); }
    catch (e) { badThrew = /rows\*cols|positive/.test(String(e.message)); }
    assert(badThrew, "a flat X that is not rows*cols is refused, not misread");
});
__kitS.end();

print("test_ml_selection: all " + n + " tests passed");
