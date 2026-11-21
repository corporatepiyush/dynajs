// covers: contract dyna:structures.WBTree dyna:structures.SkipList dyna:structures.RBTree -- counted B-tree layout D4: per-node allocations, +Infinity key-slot sentinels with a branch-free read search, a SIMD rank prefix sum and edge-aware leaf splits
// WBTree (and SkipList and RBTree, which share ds-cbtree.c) after the D4 layout fix: every node is its own
// allocation (no 32-node chunk slack), unused number-key slots hold +Infinity so get/rank/floorKey/ceilKey count
// all 31 slots without a data-dependent branch, rank sums the per-child counts left of the descent with one masked
// vector add, and a full leaf that receives a key past its last (or before its first) key on the tree's right (left)
// edge keeps its 30 keys and starts a new edge leaf, so ascending and descending fills stay near 30 of 31 slots.
// The oracle is a sorted JS array. Six key patterns (random fractions and negatives, ascending, descending,
// two-ended, ascending runs that restart at random keys, and specials: -0, +-Infinity, +-Number.MAX_VALUE,
// Number.MIN_VALUE, 2^53) grow to 20 sizes around the small-array limit (128/129), node fills (31/32/33) and
// pool edges, then drain in pattern order (front-first for ascending, back-first for descending, random
// otherwise). At every checkpoint size, keys(), rank of every key and of absent probes (including +-Infinity),
// at(i) and at(-i), get/has and floorKey/ceilKey match the model, and a SkipList and an RBTree fed the same
// operations agree. String keys run the same patterns on fewer sizes. Infinity is a legal key and also the
// sentinel value: get(Infinity) on a map without it must stay undefined and rank(Infinity) must equal size.
import { suite } from "../lib/kit.js";
import { WBTree, SkipList, RBTree } from "dyna:structures";

const t = suite("wb_tree_layout_oracle");
const SIZES = [1, 2, 16, 31, 32, 33, 64, 127, 128, 129, 130, 192, 256, 257, 511, 513, 769, 1025, 2049, 9973];
const SSIZES = [1, 33, 128, 129, 257, 1025, 4099];
const PATTERNS = ["random", "ascending", "descending", "two-ended", "runs", "specials"];
const SPECIAL = [-0, Infinity, -Infinity, Number.MAX_VALUE, -Number.MAX_VALUE, Number.MIN_VALUE, -Number.MIN_VALUE, 2 ** 53, -(2 ** 53)];

function rng(seed) {
    let s = seed >>> 0;
    return () => {
        s = (s + 0x6D2B79F5) >>> 0;
        let x = Math.imul(s ^ (s >>> 15), 1 | s);
        x = (x + Math.imul(x ^ (x >>> 7), 61 | x)) ^ x;
        return ((x ^ (x >>> 14)) >>> 0) / 4294967296;
    };
}

function lowerBound(a, k, lt) {
    let lo = 0, hi = a.length;
    while (lo < hi) { const m = (lo + hi) >> 1; if (lt(a[m], k)) lo = m + 1; else hi = m; }
    return lo;
}

function maker(pattern, strings, rnd) {
    let lo = 0, hi = 0, i = 0;
    const num = () => {
        i++;
        switch (pattern) {
        case "random": return Math.round((rnd() * 2e6 - 1e6)) / 7;
        case "ascending": return (hi += 1 + ((rnd() * 4) | 0) / 4);
        case "descending": return (lo -= 1 + ((rnd() * 4) | 0) / 4);
        case "two-ended": return i & 1 ? (hi += 0.5 + ((rnd() * 3) | 0)) : (lo -= 0.5 + ((rnd() * 3) | 0));
        case "runs": if (rnd() < 0.01) hi = Math.round(rnd() * 1e6) / 3; return (hi += 1 / 64);
        default: return rnd() < 0.4 ? SPECIAL[(rnd() * SPECIAL.length) | 0] : Math.round((rnd() * 2e6 - 1e6)) / 7;
        }
    };
    if (!strings) return num;
    return () => {
        const n = num();
        if (pattern === "specials") return rnd() < 0.3 ? ["", "ÿ", "\u{1F600}", "a\u0000b", "~"][(rnd() * 5) | 0] : "s" + n;
        const v = Math.round(Math.abs(n) * 64);
        return (n < 0 ? "-" + (1e15 - v) : "+" + (1e15 + v)).padStart(18, "0");
    };
}

function check(a, maps, model, strings, what, rnd) {
    const [wb, sl, rb] = maps, n = model.length;
    const enc = new TextEncoder();
    const lt = strings ? (x, y) => { const p = enc.encode(x), q = enc.encode(y); const m = Math.min(p.length, q.length); for (let i = 0; i < m; i++) if (p[i] !== q[i]) return p[i] < q[i]; return p.length < q.length; } : (x, y) => x < y;
    a.eq(wb.size, n, what + " size");
    const keys = wb.keys();
    let bad = keys.length !== n ? 1 : 0;
    for (let i = 0; i < n && !bad; i++) if (!Object.is(keys[i], model[i])) bad++;
    a.eq(bad, 0, what + " keys() order");
    bad = 0;
    for (let i = 0; i < n; i++) {
        const k = model[i];
        if (wb.rank(k) !== i || sl.rank(k) !== i) bad++;
        const e = wb.at(i), f = wb.at(i - n);
        if (!e || !Object.is(e[0], k) || !f || !Object.is(f[0], k) || e[1] !== f[1]) bad++;
        if (wb.get(k) === undefined || !wb.has(k) || wb.get(k) !== rb.get(k) || wb.get(k) !== sl.get(k)) bad++;
    }
    a.eq(bad, 0, what + " rank/at/get of every key");
    bad = 0;
    const probes = strings ? ["", "\u0000", "\u{10FFFF}", "+", "-"] : [Infinity, -Infinity, 0, -0, Number.MAX_VALUE, -Number.MIN_VALUE];
    for (let q = 0; q < 60; q++) {
        const base = n ? model[(rnd() * n) | 0] : 0;
        probes.push(strings ? base + (q & 1 ? "\u0000" : "") : Number.isFinite(base) ? base + (q & 1 ? 1e-7 : -1e-7) * Math.max(1, Math.abs(base)) : base);
    }
    for (const q of probes) {
        const p = lowerBound(model, q, lt), hit = p < n && Object.is(model[p], strings ? q : (q === 0 ? 0 : q));
        const fl = hit ? model[p] : (p ? model[p - 1] : undefined), ce = p < n ? model[p] : undefined;
        if (wb.rank(q) !== p || sl.rank(q) !== p) bad++;
        if ((wb.get(q) !== undefined) !== hit || wb.has(q) !== hit) bad++;
        if (!Object.is(wb.floorKey(q), fl) || !Object.is(wb.ceilKey(q), ce)) bad++;
        if (!Object.is(rb.floorKey(q), fl) || !Object.is(sl.ceilKey(q), ce)) bad++;
    }
    a.eq(bad, 0, what + " absent and edge probes");
    if (!strings) {
        const hasInf = n && model[n - 1] === Infinity;
        a.eq(wb.get(Infinity) === undefined, !hasInf, what + " get(Infinity) follows membership, not the slot sentinel");
        a.eq(wb.rank(Infinity), hasInf ? n - 1 : n, what + " rank(Infinity)");
    }
    a.eq(wb.at(n), undefined, what + " at(size)");
    a.eq(wb.at(-n - 1), undefined, what + " at(-size-1)");
}

function run(a, pattern, strings, size, seed) {
    const rnd = rng(seed), next = maker(pattern, strings, rnd);
    const opts = strings ? { keys: "string" } : undefined;
    const maps = [new WBTree(opts), new SkipList(opts), new RBTree(opts)];
    const set = new Set();
    let tries = 0;
    while (set.size < size && tries++ < size * 40 + 400) {
        let k = next();
        if (Object.is(k, -0)) k = 0;
        set.add(k);
        for (const m of maps) m.set(k, strings ? k.length : tries);
    }
    const cmp = strings ? undefined : (x, y) => (x < y ? -1 : x > y ? 1 : 0);
    let model = [...set];
    if (strings) {
        const enc = new TextEncoder();
        model.sort((x, y) => { const p = enc.encode(x), q = enc.encode(y); const m = Math.min(p.length, q.length); for (let i = 0; i < m; i++) if (p[i] !== q[i]) return p[i] - q[i]; return p.length - q.length; });
    } else model.sort(cmp);
    const what = (strings ? "string " : "number ") + pattern + " " + size;
    check(a, maps, model, strings, what + " grown", rnd);
    const marks = new Set([129, 128, 127, 33, 31, 16, 1, 0, size >> 1, size >> 2]);
    while (model.length) {
        const i = pattern === "ascending" ? 0 : pattern === "descending" ? model.length - 1 : (rnd() * model.length) | 0;
        const k = model[i];
        let ok = true;
        for (const m of maps) ok = m.delete(k) && ok;
        if (!ok) a.ok(false, what + " delete " + String(k));
        model.splice(i, 1);
        if (marks.has(model.length)) check(a, maps, model, strings, what + " drained to " + model.length, rnd);
    }
    for (const m of maps) a.eq(m.delete(strings ? "x" : 1), false, what + " empty delete");
}

let seed = 0xD4;
for (const pattern of PATTERNS) {
    t.test("number keys, " + pattern + ": grow to each size and drain back", (a) => {
        for (const size of SIZES) run(a, pattern, false, size, seed++);
    });
    t.test("string keys, " + pattern + ": grow to each size and drain back", (a) => {
        for (const size of SSIZES) run(a, pattern, true, size, seed++);
    });
}

t.test("refill after drain and interleaved edge appends keep answers", (a) => {
    const w = new WBTree();
    for (let round = 0; round < 3; round++) {
        for (let i = 0; i < 3000; i++) w.set(i & 1 ? 1e6 + i : -1e6 - i, i);
        a.eq(w.size, 3000, "round " + round + " size");
        a.eq(w.rank(0), 1500, "round " + round + " rank(0)");
        a.eq(w.at(1499)[0], -1e6, "round " + round + " last negative");
        a.eq(w.at(1500)[0], 1e6 + 1, "round " + round + " first positive");
        for (let i = 0; i < 3000; i++) w.delete(i & 1 ? 1e6 + i : -1e6 - i);
        a.eq(w.size, 0, "round " + round + " drained");
    }
});

await t.run();
