// flags: --std
// covers: dyna:structures.RStarTree
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

t.test("RStarTree search and nearest against a linear scan", (a) => {
    for (const dims of [1, 2, 3]) {
        const r = new RStarTree(dims), boxes = [];
        for (let i = 0; i < 1500; i++) {
            const lo = Array.from({ length: dims }, () => rnd(1000)), hi = lo.map((x) => x + rnd(30));
            r.insert(lo, hi, i);
            boxes.push([lo, hi, i]);
        }
        for (let i = 0; i < 500; i++) {
            const b = boxes.splice(rnd(boxes.length), 1)[0];
            a.must(r.remove(b[0], b[1], b[2]) === true, "remove an existing box");
        }
        a.eq(r.remove(new Array(dims).fill(0), new Array(dims).fill(0), 99999), false);
        a.eq(r.size, boxes.length);
        let bad = 0;
        for (let q = 0; q < 100; q++) {
            const lo = Array.from({ length: dims }, () => rnd(1000)), hi = lo.map((x) => x + rnd(80));
            const want = boxes.filter(([l, h]) => l.every((x, d) => x <= hi[d] && h[d] >= lo[d])).map((b) => b[2]).sort((x, y) => x - y);
            const got = r.search(lo, hi).sort((x, y) => x - y);
            if (JSON.stringify(got) !== JSON.stringify(want)) bad++;
            const p = lo;
            const dist = (l, h) => Math.sqrt(l.reduce((s, x, d) => { const c = Math.max(x - p[d], 0, p[d] - h[d]); return s + c * c; }, 0));
            const all = boxes.map(([l, h]) => dist(l, h)).sort((x, y) => x - y);
            const nn = r.nearest(p, 5);
            if (nn.length !== 5) bad++;
            for (let k = 0; k < nn.length; k++) if (Math.abs(nn[k].distance - all[k]) > 1e-9) bad++;
        }
        a.eq(bad, 0, "dims=" + dims);
        a.ok(r.height >= 1);
        a.eq(r.dims, dims);
    }
});

t.test("RStarTree deserialize bulk-builds at 1 and 8 dimensions", (a) => {
    for (const dims of [1, 8]) {
        for (const n of [0, 1, 17, 1000]) {
            const r = new RStarTree(dims);
            const boxes = [];
            for (let i = 0; i < n; i++) {
                const lo = [], hi = [];
                for (let d = 0; d < dims; d++) { const c = rnd(10000); lo.push(c); hi.push(c + (rnd(50))); }
                boxes.push([lo, hi]);
                r.insert(lo, hi, i);
            }
            const u = RStarTree.deserialize(r.serialize());
            a.eq(u.size, n, "size dims " + dims + " n " + n);
            a.eq(u.dims, dims, "dims");
            for (let q = 0; q < 20; q++) {
                const lo = [], hi = [];
                for (let d = 0; d < dims; d++) { const c = rnd(10000); lo.push(c); hi.push(c + 2000); }
                const want = boxes.map((b, i) => [b, i]).filter(([b]) => b[0].every((v, d) => v <= hi[d] && lo[d] <= b[1][d])).map(([, i]) => i);
                a.deep(u.search(lo, hi).slice().sort((x, y) => x - y), want, "search dims " + dims + " n " + n);
            }
        }
    }
});

t.test("RStarTree refusals", (a) => {
    const r = new RStarTree();
    a.throws(() => new RStarTree(0), RangeError);
    a.throws(() => new RStarTree(9), RangeError);
    a.throws(() => r.insert([1, 1], [0, 2], 1), RangeError);
    a.throws(() => r.insert([NaN, 1], [0, 2], 1), RangeError);
    a.throws(() => r.insert([0, 0], [1, 1], -1), RangeError);
    a.throws(() => r.insert(1, 2, 3), TypeError);
    a.throws(() => r.insert([0], [1], 1), RangeError, "a missing coordinate is NaN");
    a.throws(() => r.nearest([0, 0], 0), RangeError);
    a.deep(r.nearest([0, 0], 3), []);
    a.deep(r.search([0, 0], [1, 1]), []);
});

await t.run();
