/* test_ml_selection.js -- trainTestSplit / kFold / stratifiedKFold.
 *
 * These return INDICES, not data. So the properties worth testing are set
 * properties, and they are the ones that actually break in a bad splitter:
 *
 *   PARTITION   train and test are disjoint and together cover 0..n-1 exactly
 *               once. A splitter that leaks one training row into the test set
 *               inflates every score it is used for, silently.
 *   COVERAGE    across k folds, every index is in exactly one test fold. Losing
 *               a sample is invisible in the score; duplicating one is worse.
 *   STRATIFIED  every fold's class proportions match the whole set to within
 *               one sample. An unstratified split on a 5%-positive target can
 *               hand a fold zero positives, making that fold's score noise.
 *   DETERMINISM a seed reproduces a split exactly, or cross-validation numbers
 *               are not comparable between runs.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_ml_selection.js */

import { trainTestSplit, kFold, stratifiedKFold, crossValScore, gridSearch, DecisionTreeClassifier } from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (got !== want) throw new Error("assertion failed: " + m +
        "\n  got:  " + got + "\n  want: " + want);
}
function throwsRange(fn, m) {
    n++;
    try { fn(); } catch (e) {
        if (e instanceof RangeError) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected RangeError): " + m);
}
/* the core invariant: two index sets partition 0..N-1 */
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

/* ==================================================================== *
 *  trainTestSplit
 * ==================================================================== */
{
    for (const N of [2, 3, 10, 17, 100, 1000]) {
        const s = trainTestSplit(N, { seed: 42 });
        assertPartition(s.train, s.test, N, "trainTestSplit(" + N + ")");
        assert(s.train.length >= 1 && s.test.length >= 1,
               "both sides non-empty at N=" + N);
    }
    /* testSize is honoured, and both sides stay non-empty even when the
     * requested fraction would empty one */
    eq(trainTestSplit(10, { testSize: 0.3 }).test.length, 3, "testSize 0.3 of 10");
    eq(trainTestSplit(10, { testSize: 0.5 }).test.length, 5, "testSize 0.5 of 10");
    eq(trainTestSplit(100, { testSize: 0.2 }).test.length, 20, "testSize 0.2 of 100");
    eq(trainTestSplit(2, { testSize: 0.01 }).test.length, 1, "test set never empties");
    eq(trainTestSplit(2, { testSize: 0.99 }).train.length, 1, "train set never empties");
    throwsRange(() => trainTestSplit(10, { testSize: 0 }), "testSize 0 rejected");
    throwsRange(() => trainTestSplit(10, { testSize: 1 }), "testSize 1 rejected");
    throwsRange(() => trainTestSplit(10, { testSize: -0.5 }), "negative testSize rejected");
    throwsRange(() => trainTestSplit(1), "N<2 rejected");

    /* determinism: the seed is the whole contract */
    const a = trainTestSplit(50, { seed: 7 }), b = trainTestSplit(50, { seed: 7 });
    eq(JSON.stringify(a), JSON.stringify(b), "same seed gives an identical split");
    const c = trainTestSplit(50, { seed: 8 });
    assert(JSON.stringify(a) !== JSON.stringify(c), "a different seed gives a different split");

    /* shuffle:false is a contiguous head/tail, which is what makes it useful
     * for time-ordered data where shuffling would leak the future */
    const ns = trainTestSplit(10, { shuffle: false, testSize: 0.3 });
    eq(JSON.stringify(ns.test), "[0,1,2]", "shuffle:false takes a contiguous test block");
    eq(JSON.stringify(ns.train), "[3,4,5,6,7,8,9]", "...and the rest as train");

    /* shuffling must actually shuffle */
    {
        const sh = trainTestSplit(200, { seed: 3, shuffle: true });
        const sorted = sh.test.every((v, i, arr) => i === 0 || arr[i - 1] <= v);
        assert(!sorted, "a shuffled test set is not in ascending order");
    }
    /* an array may be passed instead of a count */
    {
        const y = new Array(20).fill(0);
        eq(trainTestSplit(y, { seed: 1 }).train.length +
           trainTestSplit(y, { seed: 1 }).test.length, 20, "accepts an array for n");
        eq(JSON.stringify(trainTestSplit(y, { seed: 1 })),
           JSON.stringify(trainTestSplit(20, { seed: 1 })), "array and count agree");
    }
}

/* ==================================================================== *
 *  kFold
 * ==================================================================== */
{
    for (const [N, k] of [[10, 5], [10, 3], [17, 5], [100, 10], [7, 7], [4, 2]]) {
        const folds = kFold(N, { k, seed: 5 });
        eq(folds.length, k, `kFold(${N},${k}) returns k folds`);
        /* every fold is itself a partition */
        for (const f of folds)
            assertPartition(f.train, f.test, N, `kFold(${N},${k}) fold`);
        /* and the test folds together cover every index exactly once */
        const all = folds.flatMap(f => f.test).sort((a, b) => a - b);
        eq(all.length, N, `kFold(${N},${k}): test folds cover N indices`);
        for (let i = 0; i < N; i++)
            if (all[i] !== i)
                throw new Error(`kFold(${N},${k}): index ${i} not covered exactly once`);
        n++;
        /* fold sizes differ by at most one */
        const sizes = folds.map(f => f.test.length);
        assert(Math.max(...sizes) - Math.min(...sizes) <= 1,
               `kFold(${N},${k}) fold sizes differ by at most 1`);
        /* each training set is everything else */
        for (const f of folds)
            eq(f.train.length, N - f.test.length, "train is the complement of test");
    }
    /* k=n is leave-one-out */
    {
        const loo = kFold(6, { k: 6 });
        eq(loo.length, 6, "k=n gives n folds");
        for (const f of loo) eq(f.test.length, 1, "leave-one-out has single-sample folds");
    }
    throwsRange(() => kFold(10, { k: 1 }), "k<2 rejected");
    throwsRange(() => kFold(10, { k: 11 }), "k>n rejected");
    throwsRange(() => kFold(1, { k: 2 }), "n<2 rejected");

    /* unshuffled folds are contiguous blocks; shuffled ones are reproducible */
    eq(JSON.stringify(kFold(6, { k: 3 })[0].test), "[0,1]", "unshuffled fold 0 is contiguous");
    eq(JSON.stringify(kFold(20, { k: 4, shuffle: true, seed: 9 })),
       JSON.stringify(kFold(20, { k: 4, shuffle: true, seed: 9 })),
       "shuffled kFold is reproducible from its seed");
    assert(JSON.stringify(kFold(20, { k: 4, shuffle: true, seed: 9 })) !==
           JSON.stringify(kFold(20, { k: 4, shuffle: true, seed: 10 })),
           "a different seed gives different folds");
    /* accepts an array */
    eq(kFold(new Array(12).fill(0), { k: 4 }).length, 4, "kFold accepts an array");
}

/* ==================================================================== *
 *  stratifiedKFold -- the reason the other two are not enough
 * ==================================================================== */
{
    /* the motivating case: 3 positives in 10 samples, 3 folds. An unstratified
     * split can put all 3 positives in one fold; stratified must not. */
    const y = [1, 1, 1, 0, 0, 0, 0, 0, 0, 0];
    const folds = stratifiedKFold(y, { k: 3 });
    eq(folds.length, 3, "3 folds");
    for (const f of folds) {
        assertPartition(f.train, f.test, y.length, "stratifiedKFold fold");
        eq(f.test.filter(i => y[i] === 1).length, 1, "each fold gets exactly one positive");
        assert(f.train.filter(i => y[i] === 1).length >= 1,
               "each training set keeps positives too");
    }
    /* coverage across folds */
    {
        const all = folds.flatMap(f => f.test).sort((a, b) => a - b);
        for (let i = 0; i < y.length; i++)
            if (all[i] !== i) throw new Error("stratified folds do not cover exactly once");
        n++;
    }

    /* proportions hold on a larger, more skewed set */
    {
        const N = 200, y2 = [];
        for (let i = 0; i < N; i++) y2.push(i < 20 ? 1 : 0);   /* 10% positive */
        const k = 5, fs = stratifiedKFold(y2, { k, shuffle: true, seed: 4 });
        for (const f of fs) {
            const pos = f.test.filter(i => y2[i] === 1).length;
            assert(Math.abs(pos - 20 / k) <= 1,
                   "each fold's positive count is within 1 of the ideal (" + pos + ")");
            assertPartition(f.train, f.test, N, "stratified large fold");
        }
        const all = fs.flatMap(f => f.test).sort((a, b) => a - b);
        eq(all.length, N, "stratified folds cover every sample");
    }

    /* more than two classes */
    {
        const y3 = [];
        for (let i = 0; i < 30; i++) y3.push(i % 3);      /* 10 each of 0,1,2 */
        const fs = stratifiedKFold(y3, { k: 5 });
        for (const f of fs)
            for (const cls of [0, 1, 2])
                eq(f.test.filter(i => y3[i] === cls).length, 2,
                   "each fold has 2 of class " + cls);
    }
    /* non-integer and negative labels are just values */
    {
        const y4 = [-1, -1, 7.5, 7.5, -1, 7.5];
        const fs = stratifiedKFold(y4, { k: 3 });
        for (const f of fs) {
            eq(f.test.filter(i => y4[i] === -1).length, 1, "one -1 per fold");
            eq(f.test.filter(i => y4[i] === 7.5).length, 1, "one 7.5 per fold");
        }
    }
    /* determinism */
    eq(JSON.stringify(stratifiedKFold(y, { k: 3, shuffle: true, seed: 11 })),
       JSON.stringify(stratifiedKFold(y, { k: 3, shuffle: true, seed: 11 })),
       "stratifiedKFold is reproducible from its seed");

    throwsRange(() => stratifiedKFold(y, { k: 1 }), "k<2 rejected");
    throwsRange(() => stratifiedKFold(y, { k: 99 }), "k>n rejected");
    throwsRange(() => stratifiedKFold([1], { k: 2 }), "n<2 rejected");
    n++;
    {
        let threw = false;
        try { stratifiedKFold(5, { k: 2 }); } catch (e) { threw = e instanceof TypeError; }
        if (!threw) throw new Error("stratifiedKFold requires an array, not a count");
    }
}

/* ==================================================================== *
 *  the indices are usable against real data
 * ==================================================================== */
{
    /* the point of returning indices: they work against an Array of rows AND a
     * flat Float64Array, without the splitter knowing which */
    const rows = [[1, 2], [3, 4], [5, 6], [7, 8], [9, 10], [11, 12]];
    const flat = new Float64Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
    const s = trainTestSplit(rows.length, { seed: 2, testSize: 0.5 });
    const trainRows = s.train.map(i => rows[i]);
    eq(trainRows.length, 3, "indices slice an Array of rows");
    const cols = 2;
    const trainFlat = new Float64Array(s.train.length * cols);
    s.train.forEach((src, d) => {
        for (let c = 0; c < cols; c++) trainFlat[d * cols + c] = flat[src * cols + c];
    });
    eq(trainFlat.length, 6, "the same indices gather from a flat buffer");
    /* and the two agree */
    for (let d = 0; d < s.train.length; d++)
        for (let c = 0; c < cols; c++)
            eq(trainFlat[d * cols + c], trainRows[d][c], "row and flat gathers agree");
}

/* --- Output budget and label validation (audit batch, 2026-08) ------------ */
{
    /* k*n output indices are budgeted at 2e7; this pair would emit 1e8 */
    throwsRange(() => kFold(100000, { k: 1000 }), "kfold budget refusal");
    throwsRange(() => stratifiedKFold([1, 0, 1, 0, NaN, 1]),
                "stratified NaN label refusal");
    eq(kFold(100, { k: 5 }).length, 5, "kfold below the budget still works");
    eq(stratifiedKFold([0, 0, 0, 1, 1, 1], { k: 3 }).length, 3,
       "stratified below the budget still works");
}

/* ==================================================================== *
 *  P1a-1 regression: the search drivers must not hold a zero-copy X
 *  alias across per-fold user JS.
 *
 *  crossValScore / gridSearch ingest X and then run arbitrary callbacks:
 *  the estimator factory, fit(), predict() and the scorer. For a flat
 *  Float64Array X the ingest aliases the backing buffer zero-copy
 *  (owned=0) and there is no (rows, cols) shape argument, so the old code
 *  either rejected the shape outright or -- once routed through a
 *  shape-aware path -- re-read mx->data (via dyn_ml_rows_subset) after a
 *  callback had ArrayBuffer.transfer()ed the caller's buffer. The fix
 *  copies X into an owned matrix ONCE per search, before any callback, so
 *  a scoring callback that detaches the caller's X buffer cannot change
 *  the features the remaining folds read.
 *
 *  What must NOT happen: silent wrong scores, crash, or reading detached
 *  memory. We prove correctness (option (a) in the audit): the detaching
 *  run returns the SAME scores as the non-detaching baseline.
 * ==================================================================== */
{
    const rows = 200, cols = 3;
    function flatX(filler) {
        const X = new Float64Array(rows * cols);
        for (let i = 0; i < rows; i++)
            for (let j = 0; j < cols; j++)
                X[i * cols + j] = filler(i, j);
        return X;
    }
    /* deterministic, finite, not constant: a tree can learn it */
    const Xsrc = flatX((i, j) => ((i * (j + 3)) % 7) - 3);
    const y = [];
    for (let i = 0; i < rows; i++) y.push(i % 2 === 0 ? 1 : 0);
    const acc = (a, b) => {
        let hit = 0;
        for (let i = 0; i < a.length; i++) if (a[i] === b[i]) hit++;
        return a.length ? hit / a.length : 0;
    };

    /* baseline: the NON-detaching run, with a fixed seed & fixed scorer so a
     * detaching run is directly comparable */
    const baseline = crossValScore(
        () => new DecisionTreeClassifier({ maxDepth: 4 }), flatX((i, j) => Xsrc[i * cols + j]),
        y, { folds: 4, seed: 7, scoring: acc });

    /* detaching run: the caller's X buffer is transferred on the FIRST scoring
     * callback. The scores must not change -- the copy happened up front. */
    let detached = false;
    const Xd = flatX((i, j) => Xsrc[i * cols + j]);
    const detaching = crossValScore(
        () => new DecisionTreeClassifier({ maxDepth: 4 }), Xd, y,
        { folds: 4, seed: 7, scoring: (a, b) => { if (!detached) { Xd.buffer.transfer(); detached = true; } return acc(a, b); } });

    eq(detached, true, "the detaching callback really fired");
    eq(JSON.stringify(Array.from(detaching)), JSON.stringify(Array.from(baseline)),
       "detaching the caller's X mid-search must not change the scores (X was copied)");
    eq(Xd.length, 0, "the caller's X was genuinely detached by the callback");

    /* the NORMAL path still works, and a seed reproduces it exactly. */
    const b2 = crossValScore(
        () => new DecisionTreeClassifier({ maxDepth: 4 }), flatX((i, j) => Xsrc[i * cols + j]),
        y, { folds: 4, seed: 7, scoring: acc });
    eq(JSON.stringify(Array.from(b2)), JSON.stringify(Array.from(baseline)),
       "the search is reproducible from its seed on flat X");

    /* gridSearch: same discipline. The factory is parameterised, so this also
     * proves the copied X is shared across every grid point without a detach
     * invalidating a later one. */
    let gDetached = false;
    const Xg = flatX((i, j) => Xsrc[i * cols + j]);
    const g = gridSearch((p) => new DecisionTreeClassifier(p), Xg, y,
                         { maxDepth: [1, 2] },
                         { folds: 3, seed: 7, scoring: (a, b) => { if (!gDetached) { Xg.buffer.transfer(); gDetached = true; } return acc(a, b); } });
    eq(gDetached, true, "gridSearch detaching callback fired");
    eq(g.results.length, 2, "2 grid points, both scored after the detach");
    for (const r of g.results) {
        eq(r.scores.length, 3, "every grid point yields a score per fold");
        for (const s of r.scores) assert(s >= 0 && s <= 1, "accuracy in [0,1] after detach");
    }

    /* a detached flat X is still refused cleanly if it cannot be copied -- we
     * assert a flat buffer whose shape does not divide evenly throws, matching
     * the pre-existing "rows*cols" contract, rather than silently misreading. */
    const bad = new Float64Array(rows * cols + 1);
    let badThrew = false;
    try { crossValScore(() => new DecisionTreeClassifier(), bad, y, { folds: 3 }); }
    catch (e) { badThrew = /rows\*cols|positive/.test(String(e.message)); }
    assert(badThrew, "a flat X that is not rows*cols is refused, not misread");
}

print("test_ml_selection: all " + n + " tests passed");
