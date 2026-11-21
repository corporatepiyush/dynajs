// covers: dyna:structures.BTree
// BTree.rangeQuery builds each [key, value] entry with JS_NewArrayFrom (one fast array of two, the value
// duplicated) instead of an empty array plus two indexed defines. The oracle is a sorted JS model of the same
// inserts: every range over random, duplicate-overwritten, negative and fractional keys returns exactly the model's
// [key, value] pairs in key order, each value being the very object last stored under that key.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_b_tree_range_query_oracle");
import { BTree } from "dyna:structures";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; print("FAIL: " + what + ": got " + got + ", want " + want); }
}
let seed = 4242;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);

__kitS.section("rangeQuery returns the model's [key, value] pairs in key order", () => {
    for (const n of [0, 1, 50, 3000]) {
        const t = new BTree(), model = new Map();
        for (let i = 0; i < n; i++) {
            const k = i % 3 === 0 ? ((rnd() * 200) | 0) - 100 : Math.round(rnd() * 4000) / 4 - 500;
            const v = { i };
            t.set(k, v);
            model.set(k, v);
        }
        const keys = [...model.keys()].sort((a, b) => a - b);
        for (let q = 0; q < 60; q++) {
            const lo = rnd() * 1200 - 600, hi = lo + rnd() * 300;
            const got = t.rangeQuery(lo, hi), want = keys.filter((k) => k >= lo && k <= hi);
            eq(got.length, want.length, "n=" + n + " q=" + q + " count");
            let bad = 0;
            got.forEach((e, j) => {
                if (!Array.isArray(e) || e.length !== 2 || e[0] !== want[j] || e[1] !== model.get(want[j])) bad++;
            });
            eq(bad, 0, "n=" + n + " q=" + q + " pairs");
        }
    }
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_structures_b_tree_range_query_oracle: " + checks + " checks passed");
__kitS.end();
