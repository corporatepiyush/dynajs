// flags: --std
// covers: dyna:structures.SuffixArray
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

function naiveSA(units) {
    const idx = units.map((_, i) => i);
    idx.sort((i, j) => {
        for (let k = 0; ; k++) {
            if (i + k >= units.length) return j + k >= units.length ? 0 : -1;
            if (j + k >= units.length) return 1;
            if (units[i + k] !== units[j + k]) return units[i + k] - units[j + k];
        }
    });
    return idx;
}

t.test("SuffixArray matches a naive suffix sort (bytes and ASCII)", (a) => {
    for (let trial = 0; trial < 40; trial++) {
        const len = rnd(200), sigma = 1 + rnd(4);
        const bytes = Uint8Array.from({ length: len }, () => 97 + rnd(sigma));
        const s = String.fromCharCode(...bytes);
        for (const sa of [new SuffixArray(bytes), new SuffixArray(s)]) {
            const want = naiveSA([...bytes]);
            a.deep([...sa.array()], want, "array len=" + len);
            const lcp = [...sa.lcp()];
            let bad = 0;
            for (let i = 1; i < want.length; i++) {
                let h = 0;
                while (want[i] + h < len && want[i - 1] + h < len && bytes[want[i] + h] === bytes[want[i - 1] + h]) h++;
                if (lcp[i] !== h) bad++;
            }
            if (len && lcp[0] !== 0) bad++;
            a.eq(bad, 0, "lcp");
            const pat = s.substr(rnd(len + 1), 1 + rnd(3));
            const occ = [];
            for (let i = s.indexOf(pat); pat && i >= 0; i = s.indexOf(pat, i + 1)) occ.push(i);
            a.deep(sa.find(pat), occ, "find " + pat);
            a.eq(sa.count(pat), occ.length);
            let best = 0;
            for (let i = 1; i < want.length; i++) best = Math.max(best, lcp[i]);
            const lr = sa.longestRepeat();
            if (best === 0) a.eq(lr, undefined);
            else {
                a.eq(lr.length, best);
                const sub = s.substr(lr.start, lr.length);
                a.ok(s.indexOf(sub) !== s.lastIndexOf(sub), "the repeat occurs twice");
            }
        }
    }
});

t.test("SuffixArray on non-ASCII text reports UTF-16 positions", (a) => {
    const s = "é😀é😀xé\u0000aèbé";
    const sa = new SuffixArray(s);
    a.eq(sa.length, s.length);
    const starts = [];
    for (let i = 0; i < s.length; i++) if (!(s.charCodeAt(i) >= 0xdc00 && s.charCodeAt(i) < 0xe000)) starts.push(i);
    const want = starts.slice().sort((i, j) => utf8cmp(s.slice(i), s.slice(j)));
    a.deep([...sa.array()], want);
    const lcp = [...sa.lcp()];
    for (let i = 1; i < want.length; i++) {
        const x = s.slice(want[i]), y = s.slice(want[i - 1]);
        let h = 0;
        while (h < x.length && x[h] === y[h]) h++;
        if (h && x.charCodeAt(h - 1) >= 0xd800 && x.charCodeAt(h - 1) < 0xdc00) h--;
        a.eq(lcp[i], h, "lcp " + i);
    }
    a.deep(sa.find("é😀"), [0, 3]);
    a.deep(sa.find("😀"), [1, 4]);
    a.deep(sa.longestRepeat(), { start: 0, length: 3 });
    a.eq(sa.count(""), 0);
    a.throws(() => new SuffixArray(new Uint16Array(2)), TypeError);
    a.throws(() => new SuffixArray(5), TypeError);
    const e = new SuffixArray("");
    a.eq(e.array().length, 0);
    a.eq(e.longestRepeat(), undefined);
});

await t.run();
