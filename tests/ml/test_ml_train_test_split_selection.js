// covers: dyna:ml.trainTestSplit
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_train_test_split_selection");
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

await __kitS.section("both sides non-empty at N", async () => {
    for (const N of [2, 3, 10, 17, 100, 1000]) {
        const s = await trainTestSplit(N, { seed: 42 });
        assertPartition(s.train, s.test, N, "trainTestSplit(" + N + ")");
        assert(s.train.length >= 1 && s.test.length >= 1,
               "both sides non-empty at N=" + N);
    }
    eq((await trainTestSplit(10, { testSize: 0.3 })).test.length, 3, "testSize 0.3 of 10");
    eq((await trainTestSplit(10, { testSize: 0.5 })).test.length, 5, "testSize 0.5 of 10");
    eq((await trainTestSplit(100, { testSize: 0.2 })).test.length, 20, "testSize 0.2 of 100");
    eq((await trainTestSplit(2, { testSize: 0.01 })).test.length, 1, "test set never empties");
    eq((await trainTestSplit(2, { testSize: 0.99 })).train.length, 1, "train set never empties");
    await throwsRange(async () => await trainTestSplit(10, { testSize: 0 }), "testSize 0 rejected");
    await throwsRange(async () => await trainTestSplit(10, { testSize: 1 }), "testSize 1 rejected");
    await throwsRange(async () => await trainTestSplit(10, { testSize: -0.5 }), "negative testSize rejected");
    await throwsRange(async () => await trainTestSplit(1), "N<2 rejected");

    const a = await trainTestSplit(50, { seed: 7 }), b = await trainTestSplit(50, { seed: 7 });
    eq(JSON.stringify(a), JSON.stringify(b), "same seed gives an identical split");
    const c = await trainTestSplit(50, { seed: 8 });
    assert(JSON.stringify(a) !== JSON.stringify(c), "a different seed gives a different split");

    const ns = await trainTestSplit(10, { shuffle: false, testSize: 0.3 });
    eq(JSON.stringify(ns.test), "[0,1,2]", "shuffle:false takes a contiguous test block");
    eq(JSON.stringify(ns.train), "[3,4,5,6,7,8,9]", "...and the rest as train");

    {
        const sh = await trainTestSplit(200, { seed: 3, shuffle: true });
        const sorted = sh.test.every((v, i, arr) => i === 0 || arr[i - 1] <= v);
        assert(!sorted, "a shuffled test set is not in ascending order");
    }
    {
        const y = new Array(20).fill(0);
        eq((await trainTestSplit(y, { seed: 1 })).train.length +
           (await trainTestSplit(y, { seed: 1 })).test.length, 20, "accepts an array for n");
        eq(JSON.stringify(await trainTestSplit(y, { seed: 1 })),
           JSON.stringify(await trainTestSplit(20, { seed: 1 })), "array and count agree");
    }
});

await __kitS.section("indices slice an Array of rows", async () => {
    const rows = [[1, 2], [3, 4], [5, 6], [7, 8], [9, 10], [11, 12]];
    const flat = new Float64Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
    const s = await trainTestSplit(rows.length, { seed: 2, testSize: 0.5 });
    const trainRows = s.train.map(i => rows[i]);
    eq(trainRows.length, 3, "indices slice an Array of rows");
    const cols = 2;
    const trainFlat = new Float64Array(s.train.length * cols);
    s.train.forEach((src, d) => {
        for (let c = 0; c < cols; c++) trainFlat[d * cols + c] = flat[src * cols + c];
    });
    eq(trainFlat.length, 6, "the same indices gather from a flat buffer");
    for (let d = 0; d < s.train.length; d++)
        for (let c = 0; c < cols; c++)
            eq(trainFlat[d * cols + c], trainRows[d][c], "row and flat gathers agree");
});
__kitS.end();

print("test_ml_selection: all " + n + " tests passed");
