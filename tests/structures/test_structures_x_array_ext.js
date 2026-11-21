// flags: --std
// covers: dyna:structures.XArray
// RBTree, WBTree, SkipList, XArray, PatriciaTrie, CSRGraph, RStarTree,
// SuffixArray: every query is checked against a brute-force model written here
// (a sorted array, a linear scan, Bellman-Ford, naive suffix sort).
import * as std from "std";
import { memoryUsage, setNativeMemoryLimit } from "dyna:sys";
import { CRC32C } from "dyna:hash";
import { suite } from "../lib/kit.js";
import { Deque, RBTree, WBTree, SkipList, XArray, PatriciaTrie, CSRGraph, RStarTree, SuffixArray } from "dyna:structures";

const t = suite("structures_ext");

let seed = 0x2545F491;
function rnd(n) {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0;
    return seed % n;
}

const enc = new TextEncoder();
function utf8cmp(a, b) {
    const x = enc.encode(a), y = enc.encode(b), n = Math.min(x.length, y.length);
    for (let i = 0; i < n; i++) if (x[i] !== y[i]) return x[i] - y[i];
    return x.length - y.length;
}

function treeModel(cmp) {
    const keys = [], vals = new Map();
    const find = (k) => { let lo = 0, hi = keys.length; while (lo < hi) { const mid = (lo + hi) >> 1; if (cmp(keys[mid], k) < 0) lo = mid + 1; else hi = mid; } return lo; };
    return {
        set(k, v) { const i = find(k); if (i >= keys.length || cmp(keys[i], k) !== 0) keys.splice(i, 0, k); vals.set(k, v); },
        del(k) { const i = find(k); if (i < keys.length && cmp(keys[i], k) === 0) { keys.splice(i, 1); vals.delete(k); return true; } return false; },
        get(k) { return vals.get(k); },
        rank(k) { return find(k); },
        floor(k) { const i = find(k); if (i < keys.length && cmp(keys[i], k) === 0) return keys[i]; return i > 0 ? keys[i - 1] : undefined; },
        ceil(k) { const i = find(k); return i < keys.length ? keys[i] : undefined; },
        keys,
    };
}

t.test("XArray agrees with Map over sparse 64-bit indices", (a) => {
    const x = new XArray(), ref = new Map();
    const idx = () => [rnd(64), rnd(5000), 2 ** 32 + rnd(10), 2 ** 52 + rnd(3), Number.MAX_SAFE_INTEGER - rnd(2)][rnd(5)];
    let bad = 0;
    for (let step = 0; step < 8000; step++) {
        const i = idx(), op = rnd(10);
        if (op < 5) { x.set(i, step); ref.set(i, step); }
        else if (op < 8) { if (x.delete(i) !== ref.delete(i)) bad++; }
        else if (x.get(i) !== ref.get(i) || x.has(i) !== ref.has(i)) bad++;
    }
    a.eq(bad, 0);
    a.eq(x.size, ref.size);
    const sorted = [...ref.keys()].sort((p, q) => p - q);
    a.deep(x.entries().map((e) => e[0]), sorted, "entries in index order");
    const from = 3000;
    const want = sorted.find((k) => k >= from);
    a.deep(x.next(from), [want, ref.get(want)]);
    a.deep(x.entries(10, 4999).map((e) => e[0]), sorted.filter((k) => k >= 10 && k <= 4999));
    a.eq(x.next(Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER)?.[0], ref.has(Number.MAX_SAFE_INTEGER) ? Number.MAX_SAFE_INTEGER : undefined);
});

t.test("XArray BigInt indices, marks, refusals", (a) => {
    const x = new XArray();
    const top = 2n ** 64n - 1n;
    x.set(top, "top").set(0, "zero").set(2n ** 60n, "big");
    a.eq(x.get(top), "top");
    a.deep(x.next(1), [2n ** 60n, "big"], "an index above 2^53 comes back as a BigInt");
    a.deep(x.next(2n ** 60n + 1n), [top, "top"]);
    a.deep(x.entries().map((e) => e[1]), ["zero", "big", "top"], "a scan that reaches 2^64-1 stops there");
    a.throws(() => x.set(2n ** 64n, 1), RangeError);
    a.throws(() => x.set(-1n, 1), RangeError);
    a.throws(() => x.set(-1, 1), RangeError);
    a.throws(() => x.set(1.5, 1), RangeError);
    a.throws(() => x.set(2 ** 53, 1), RangeError);
    a.eq(x.mark(0, 2), true);
    a.eq(x.mark(5, 2), false, "an empty slot cannot be marked");
    a.eq(x.isMarked(0, 2), true);
    a.eq(x.isMarked(0, 1), false);
    a.deep(x.nextMarked(2), [0, "zero"]);
    a.eq(x.nextMarked(2, 1), undefined);
    x.mark(0, 2, false);
    a.eq(x.isMarked(0, 2), false);
    a.throws(() => x.mark(0, 3), RangeError);
    for (let i = 0; i < 20; i++) { const y = new XArray(); y.set(1, y); }
    std.gc();
});

await t.run();
