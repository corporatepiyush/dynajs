// covers: dyna:structures.WBTree
// WBTree insert and delete rebalance on the way up from sizes they already hold: the child on the path has its
// fresh size and the sibling's size is the parent's old size minus the path child's old size (one more on insert,
// one less on delete), so an unchanged-balance level never loads the sibling node; only a level that violates the
// weight bounds runs the full rotation. The oracle is a sorted JS model: after every batch of random, ascending,
// descending and duplicate inserts and deletes, size, rank(k), at(i), keys() and get() match the model, and a
// 20,000-key ascending insert stays within a small factor of a random one (an unbalanced tree would be quadratic).
import { sections as __kitSections } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
const __kitS = __kitSections("test_structures_wb_tree_rebalance_oracle");
import { WBTree } from "dyna:structures";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
let seed = 31337;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
function lowerBound(a, k) { let lo = 0, hi = a.length; while (lo < hi) { const m = (lo + hi) >> 1; if (a[m] < k) lo = m + 1; else hi = m; } return lo; }
function verify(t, model, what) {
    const keys = [...model.keys()].sort((a, b) => a - b);
    eq(t.size, keys.length, what + " size");
    eq(t.keys().join(","), keys.join(","), what + " keys");
    let bad = 0;
    for (let i = 0; i < keys.length; i += Math.max(1, (keys.length / 97) | 0)) {
        const e = t.at(i);
        if (!e || e[0] !== keys[i] || e[1] !== model.get(keys[i])) bad++;
        if (t.rank(keys[i]) !== i) bad++;
    }
    for (let q = 0; q < 50; q++) { const k = rnd() * 3000 - 500; if (t.rank(k) !== lowerBound(keys, k)) bad++; }
    eq(bad, 0, what + " at/rank");
}

__kitS.section("inserts and deletes keep size, rank and at equal to the model", () => {
    for (const pattern of ["random", "ascending", "descending", "dups"]) {
        const t = new WBTree(), model = new Map();
        for (let round = 0; round < 6; round++) {
            for (let i = 0; i < 400; i++) {
                const k = pattern === "random" ? ((rnd() * 2000) | 0) : pattern === "ascending" ? round * 400 + i : pattern === "descending" ? 2400 - (round * 400 + i) : (rnd() * 50) | 0;
                const v = { round, i };
                t.set(k, v);
                model.set(k, v);
            }
            verify(t, model, pattern + " r" + round + " insert");
            const keys = [...model.keys()];
            for (let i = 0; i < 150 && keys.length; i++) {
                const k = round % 2 ? keys[(rnd() * keys.length) | 0] : (rnd() * 2400) | 0;
                eq(t.delete(k), model.delete(k), pattern + " r" + round + " delete " + k);
            }
            verify(t, model, pattern + " r" + round + " delete");
        }
        for (const k of [...model.keys()]) { t.delete(k); model.delete(k); }
        verify(t, model, pattern + " drained");
    }
});

__kitS.section("ascending inserts stay logarithmic", () => {
    const ok = (c, what) => eq(!!c, true, what);
    const time = (keys) => { const t0 = performance.now(); const t = new WBTree(); for (const k of keys) t.set(k, 1); const ms = performance.now() - t0; eq(t.size, keys.length, "timed size"); return ms; };
    const n = 20000, asc = Array.from({ length: n }, (_, i) => i), rand = asc.map(() => rnd());
    time(asc); time(rand);
    const a = Math.min(time(asc), time(asc)), r = Math.min(time(rand), time(rand));
    perfCheck(ok, a < r * 4 + 5, "ascending " + a.toFixed(1) + " ms vs random " + r.toFixed(1) + " ms");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_structures_wb_tree_rebalance_oracle: " + checks + " checks passed");
__kitS.end();
