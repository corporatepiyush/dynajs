// covers: dyna:structures.SkipList
// SkipList nodes keep a key of up to 16 bytes (a number, or a string's pointer and length) in a fixed slot right
// after the node header, so a descent compares the next node's key without first loading its level count; larger
// keys stay after the level array. Number keys compare through the shared dyn_ds_cmp_f64; the WBTree and SkipList
// descents recognise it (ds_ops_cmp_f64_fast) and compare inline instead of an indirect call, while RBTree keeps the
// plain indirect call (inlining it there measured +3..5% on RB.get at 10K, so it was left out). The oracle is a sorted JS
// model: number keys (negative, fractional, -0 folded to +0, huge) and string keys (shared prefixes, empty, non-ASCII
// in UTF-8 byte order) give the model's get/has/rank/at/keys/floorKey/ceilKey after inserts, overwrites and deletes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_skip_list_inline_key_oracle");
import { SkipList, RBTree, WBTree } from "dyna:structures";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
let seed = 2718;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const enc = new TextEncoder();
function byteCmp(a, b) {
    const x = enc.encode(a), y = enc.encode(b), n = Math.min(x.length, y.length);
    for (let i = 0; i < n; i++) if (x[i] !== y[i]) return x[i] - y[i];
    return x.length - y.length;
}
const numKey = () => { const r = rnd(); return r < 0.1 ? -0 : r < 0.2 ? (rnd() < 0.5 ? -1 : 1) * 1e300 * rnd() : Math.round((rnd() * 2000 - 1000) * 4) / 4; };
const strKey = () => { const r = rnd(); const base = ["", "a", "ab", "abc", "abd", "été", "日本", "zz"][(rnd() * 8) | 0]; return r < 0.15 ? base : base + ((rnd() * 500) | 0); };

function run(Cls, strings, what) {
    const t = strings ? new Cls({ keys: "string" }) : new Cls(), model = new Map();
    const cmp = strings ? byteCmp : (a, b) => a - b;
    const norm = (k) => (Object.is(k, -0) ? 0 : k);
    for (let round = 0; round < 5; round++) {
        for (let i = 0; i < 600; i++) { const k = strings ? strKey() : numKey(), v = { round, i }; t.set(k, v); model.set(norm(k), v); }
        for (let i = 0; i < 200; i++) { const k = strings ? strKey() : numKey(); eq(t.delete(k), model.delete(norm(k)), what + " delete"); }
        const keys = [...model.keys()].sort(cmp);
        eq(t.size, keys.length, what + " r" + round + " size");
        eq(JSON.stringify(t.keys()), JSON.stringify(keys), what + " r" + round + " keys");
        let bad = 0;
        keys.forEach((k, i) => {
            if (t.get(k) !== model.get(k) || !t.has(k)) bad++;
            if (t.rank && t.rank(k) !== i) bad++;
            if (t.at && (t.at(i)[0] !== k || t.at(i)[1] !== model.get(k))) bad++;
        });
        for (let q = 0; q < 200; q++) {
            const k = strings ? strKey() : numKey();
            const fl = keys.filter((x) => cmp(x, k) <= 0).pop(), ce = keys.find((x) => cmp(x, k) >= 0);
            if (t.floorKey(k) !== fl || t.ceilKey(k) !== ce) bad++;
            if (!model.has(norm(k)) && (t.get(k) !== undefined || t.has(k))) bad++;
        }
        eq(bad, 0, what + " r" + round + " lookups");
    }
}

__kitS.section("SkipList number and string keys match the model", () => { run(SkipList, false, "SkipList num"); run(SkipList, true, "SkipList str"); });
__kitS.section("RBTree and WBTree number keys match the model", () => { run(RBTree, false, "RBTree num"); run(WBTree, false, "WBTree num"); run(WBTree, true, "WBTree str"); });

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_structures_skip_list_inline_key_oracle: " + checks + " checks passed");
__kitS.end();
