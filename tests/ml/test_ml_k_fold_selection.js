// covers: dyna:ml.kFold
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_k_fold_selection");
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

await __kitS.section("train is the complement of test", async () => {
    for (const [N, k] of [[10, 5], [10, 3], [17, 5], [100, 10], [7, 7], [4, 2]]) {
        const folds = await kFold(N, { k, seed: 5 });
        eq(folds.length, k, `kFold(${N},${k}) returns k folds`);
        for (const f of folds)
            assertPartition(f.train, f.test, N, `kFold(${N},${k}) fold`);
        const all = folds.flatMap(f => f.test).sort((a, b) => a - b);
        eq(all.length, N, `kFold(${N},${k}): test folds cover N indices`);
        for (let i = 0; i < N; i++)
            if (all[i] !== i)
                throw new Error(`kFold(${N},${k}): index ${i} not covered exactly once`);
        n++;
        const sizes = folds.map(f => f.test.length);
        assert(Math.max(...sizes) - Math.min(...sizes) <= 1,
               `kFold(${N},${k}) fold sizes differ by at most 1`);
        for (const f of folds)
            eq(f.train.length, N - f.test.length, "train is the complement of test");
    }
    {
        const loo = await kFold(6, { k: 6 });
        eq(loo.length, 6, "k=n gives n folds");
        for (const f of loo) eq(f.test.length, 1, "leave-one-out has single-sample folds");
    }
    await throwsRange(async () => await kFold(10, { k: 1 }), "k<2 rejected");
    await throwsRange(async () => await kFold(10, { k: 11 }), "k>n rejected");
    await throwsRange(async () => await kFold(1, { k: 2 }), "n<2 rejected");

    eq(JSON.stringify((await kFold(6, { k: 3 }))[0].test), "[0,1]", "unshuffled fold 0 is contiguous");
    eq(JSON.stringify(await kFold(20, { k: 4, shuffle: true, seed: 9 })),
       JSON.stringify(await kFold(20, { k: 4, shuffle: true, seed: 9 })),
       "shuffled kFold is reproducible from its seed");
    assert(JSON.stringify(await kFold(20, { k: 4, shuffle: true, seed: 9 })) !==
           JSON.stringify(await kFold(20, { k: 4, shuffle: true, seed: 10 })),
           "a different seed gives different folds");
    eq((await kFold(new Array(12).fill(0), { k: 4 })).length, 4, "kFold accepts an array");
});

await __kitS.section("kfold budget refusal", async () => {
    await throwsRange(async () => await kFold(100000, { k: 1000 }), "kfold budget refusal");
    await throwsRange(async () => await stratifiedKFold([1, 0, 1, 0, NaN, 1]),
                "stratified NaN label refusal");
    eq((await kFold(100, { k: 5 })).length, 5, "kfold below the budget still works");
    eq((await stratifiedKFold([0, 0, 0, 1, 1, 1], { k: 3 })).length, 3,
       "stratified below the budget still works");
});
__kitS.end();

print("test_ml_selection: all " + n + " tests passed");
