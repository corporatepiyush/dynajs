// covers: dyna:ml.stratifiedKFold
// F-ml-1: with shuffle:true the seeded permutation was undone before the folds were dealt (the class/row sort keyed
// on the row index), so every seed, and shuffle:false, gave the same folds. The rule now (dynajs.d.ts): each class's
// members are dealt round-robin to folds 0..k-1, in row order without shuffle and in the order of a seeded
// permutation with it. Oracles: shuffle:false against that rule written in JS (independent of the C sort); shuffle:true
// against the invariants it must keep whatever the permutation is (a partition of 0..n-1 into k disjoint test folds,
// each class's count per fold exactly the round-robin count), plus seed sensitivity and seed reproducibility.
// Not sklearn: StratifiedKFold(shuffle=False) gives each class contiguous blocks; the d.ts does not promise sklearn's
// assignment, only the proportions, so that difference is recorded here rather than changed.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_stratified_k_fold_shuffle");
import { stratifiedKFold } from "dyna:ml";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
const key = (folds) => JSON.stringify(folds.map((f) => Array.from(f.test)));

function roundRobinRef(y, k) {
    const classes = Array.from(new Set(y)).sort((a, b) => a - b);
    const fold = new Array(y.length);
    for (const c of classes) {
        let next = 0;
        for (let i = 0; i < y.length; i++) if (y[i] === c) { fold[i] = next; next = (next + 1) % k; }
    }
    return Array.from({ length: k }, (_, f) => fold.map((g, i) => (g === f ? i : -1)).filter((i) => i >= 0));
}

function checkInvariants(y, k, folds, what) {
    eq(folds.length, k, what + ": k folds");
    const seen = new Array(y.length).fill(0);
    folds.forEach((f, fi) => {
        for (const i of f.test) seen[i]++;
        eq(f.train.length + f.test.length, y.length, what + ": fold " + fi + " train+test covers n");
        const inTest = new Set(f.test);
        ok(Array.from(f.train).every((i) => !inTest.has(i)), what + ": fold " + fi + " train and test are disjoint");
    });
    ok(seen.every((c) => c === 1), what + ": every row is in exactly one test fold");
    const classes = Array.from(new Set(y)).sort((a, b) => a - b);
    for (const c of classes) {
        const total = y.filter((v) => v === c).length;
        folds.forEach((f, fi) => {
            const want = Math.floor(total / k) + (fi < total % k ? 1 : 0);
            eq(Array.from(f.test).filter((i) => y[i] === c).length, want, what + ": class " + c + " in fold " + fi);
        });
    }
}

const ISSUE_Y = Array.from({ length: 20 }, (_, i) => i % 2);
const lcg = (s) => () => ((s = (Math.imul(s, 1103515245) + 12345) >>> 0) / 4294967296);
const r = lcg(7);
const MIXED = Array.from({ length: 97 }, () => [0, 0, 0, 1, 1, 2, 3][(r() * 7) | 0]);

await __kitS.section("shuffle:false follows the documented round-robin rule and ignores seed", async () => {
    for (const [y, k] of [[ISSUE_Y, 2], [ISSUE_Y, 5], [MIXED, 3], [MIXED, 5]]) {
        const ref = JSON.stringify(roundRobinRef(y, k));
        eq(key(await stratifiedKFold(y, { k })), ref, "n=" + y.length + " k=" + k + " default");
        eq(key(await stratifiedKFold(y, { k, shuffle: false, seed: 1 })), ref, "n=" + y.length + " k=" + k + " seed 1");
        eq(key(await stratifiedKFold(y, { k, shuffle: false, seed: 2 })), ref, "n=" + y.length + " k=" + k + " seed 2");
    }
});

await __kitS.section("shuffle:true depends on the seed (the ISSUES repro: seeds 1 and 2 used to agree)", async () => {
    const a = await stratifiedKFold(ISSUE_Y, { k: 2, shuffle: true, seed: 1 });
    const b = await stratifiedKFold(ISSUE_Y, { k: 2, shuffle: true, seed: 2 });
    ok(key(a) !== key(b), "seed 1 and seed 2 give different folds: " + key(a));
    ok(key(a) !== JSON.stringify(roundRobinRef(ISSUE_Y, 2)), "shuffled folds differ from the unshuffled ones");
    const distinct = new Set();
    for (let s = 1; s <= 20; s++) distinct.add(key(await stratifiedKFold(MIXED, { k: 4, shuffle: true, seed: s })));
    ok(distinct.size >= 18, "20 seeds give at least 18 distinct fold sets on 97 rows (got " + distinct.size + ")");
});

await __kitS.section("shuffle:true keeps the partition and the per-class counts, and is reproducible", async () => {
    for (const [y, k] of [[ISSUE_Y, 2], [ISSUE_Y, 10], [MIXED, 3], [MIXED, 5]]) {
        for (const seed of [1, 2, 99, 123456789]) {
            const f = await stratifiedKFold(y, { k, shuffle: true, seed });
            checkInvariants(y, k, f, "n=" + y.length + " k=" + k + " seed=" + seed);
            eq(key(await stratifiedKFold(y, { k, shuffle: true, seed })), key(f), "same seed, same folds (k=" + k + " seed=" + seed + ")");
        }
    }
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_ml_stratified_k_fold_shuffle: " + n + " checks passed");
__kitS.end();
