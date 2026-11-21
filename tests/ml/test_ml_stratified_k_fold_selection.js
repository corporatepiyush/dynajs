// covers: dyna:ml.stratifiedKFold
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_stratified_k_fold_selection");
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

await __kitS.section("3 folds", async () => {
    const y = [1, 1, 1, 0, 0, 0, 0, 0, 0, 0];
    const folds = await stratifiedKFold(y, { k: 3 });
    eq(folds.length, 3, "3 folds");
    for (const f of folds) {
        assertPartition(f.train, f.test, y.length, "stratifiedKFold fold");
        eq(f.test.filter(i => y[i] === 1).length, 1, "each fold gets exactly one positive");
        assert(f.train.filter(i => y[i] === 1).length >= 1,
               "each training set keeps positives too");
    }
    {
        const all = folds.flatMap(f => f.test).sort((a, b) => a - b);
        for (let i = 0; i < y.length; i++)
            if (all[i] !== i) throw new Error("stratified folds do not cover exactly once");
        n++;
    }

    {
        const N = 200, y2 = [];
        for (let i = 0; i < N; i++) y2.push(i < 20 ? 1 : 0);
        const k = 5, fs = await stratifiedKFold(y2, { k, shuffle: true, seed: 4 });
        for (const f of fs) {
            const pos = f.test.filter(i => y2[i] === 1).length;
            assert(Math.abs(pos - 20 / k) <= 1,
                   "each fold's positive count is within 1 of the ideal (" + pos + ")");
            assertPartition(f.train, f.test, N, "stratified large fold");
        }
        const all = fs.flatMap(f => f.test).sort((a, b) => a - b);
        eq(all.length, N, "stratified folds cover every sample");
    }

    {
        const y3 = [];
        for (let i = 0; i < 30; i++) y3.push(i % 3);
        const fs = await stratifiedKFold(y3, { k: 5 });
        for (const f of fs)
            for (const cls of [0, 1, 2])
                eq(f.test.filter(i => y3[i] === cls).length, 2,
                   "each fold has 2 of class " + cls);
    }
    {
        const y4 = [-1, -1, 7.5, 7.5, -1, 7.5];
        const fs = await stratifiedKFold(y4, { k: 3 });
        for (const f of fs) {
            eq(f.test.filter(i => y4[i] === -1).length, 1, "one -1 per fold");
            eq(f.test.filter(i => y4[i] === 7.5).length, 1, "one 7.5 per fold");
        }
    }
    eq(JSON.stringify(await stratifiedKFold(y, { k: 3, shuffle: true, seed: 11 })),
       JSON.stringify(await stratifiedKFold(y, { k: 3, shuffle: true, seed: 11 })),
       "stratifiedKFold is reproducible from its seed");

    await throwsRange(async () => await stratifiedKFold(y, { k: 1 }), "k<2 rejected");
    await throwsRange(async () => await stratifiedKFold(y, { k: 99 }), "k>n rejected");
    await throwsRange(async () => await stratifiedKFold([1], { k: 2 }), "n<2 rejected");
    n++;
    {
        let threw = false;
        try { await stratifiedKFold(5, { k: 2 }); } catch (e) { threw = e instanceof TypeError; }
        if (!threw) throw new Error("stratifiedKFold requires an array, not a count");
    }
});
__kitS.end();

print("test_ml_selection: all " + n + " tests passed");
