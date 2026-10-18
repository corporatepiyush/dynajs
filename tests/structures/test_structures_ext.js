// flags: --std
// SwissMap, RBTree, WBTree, SkipList, XArray, PatriciaTrie, CSRGraph, RStarTree,
// SuffixArray: every query is checked against a brute-force model written here
// (a native Map, a sorted array, a linear scan, Bellman-Ford, naive suffix sort).
import * as std from "std";
import { memoryUsage, setNativeMemoryLimit } from "dyna:sys";
import { CRC32C } from "dyna:hash";
import { suite } from "../lib/kit.js";
import { Deque, SwissMap, RBTree, WBTree, SkipList, XArray, PatriciaTrie, CSRGraph, RStarTree, SuffixArray } from "dyna:structures";

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

t.test("SwissMap agrees with Map under random set/delete (SameValueZero keys)", (a) => {
    const objs = Array.from({ length: 8 }, (_, i) => ({ i }));
    const pool = [0, -0, NaN, 1, 1.5, "1", "", "a", "é", null, undefined, true, false, 10n, 2n ** 70n, ...objs];
    for (let i = 0; i < 300; i++) pool.push("k" + i, i * 7);
    const m = new SwissMap(), ref = new Map();
    let bad = 0;
    for (let step = 0; step < 20000; step++) {
        const k = pool[rnd(pool.length)], op = rnd(10);
        if (op < 6) { m.set(k, step); ref.set(k, step); }
        else if (op < 9) { if (m.delete(k) !== ref.delete(k)) bad++; }
        else if (m.get(k) !== ref.get(k) || m.has(k) !== ref.has(k)) bad++;
        if (m.size !== ref.size) bad++;
    }
    a.eq(bad, 0, "mismatches");
    let seen = 0;
    for (const [k, v] of m) { if (ref.get(k) !== v) bad++; seen++; }
    a.eq(seen, ref.size, "iteration visits every entry once");
    a.eq(bad, 0, "iterated values");
    a.eq(m.keys().length, ref.size);
    a.eq(m.get(-0), ref.get(0), "-0 and 0 are one key");
    const z = new SwissMap([[-0, 1]]);
    a.ok(Object.is(z.keys()[0], 0), "-0 is stored as +0, as Map does");
    m.clear();
    a.eq(m.size, 0);
    a.eq(m.get(1), undefined);
});

t.test("SwissMap constructor, reserve, forEach, refusals", (a) => {
    const m = new SwissMap([[1, "a"], ["x", 2]]);
    a.eq(m.size, 2);
    a.eq(m.reserve(1000), m, "reserve returns this");
    const got = [];
    m.forEach(function (v, k, self) { got.push(String(k) + "=" + v); a.eq(self, m); a.eq(this, 7); }, 7);
    a.deep(got.sort(), ["1=a", "x=2"]);
    a.throws(() => new SwissMap([1]), TypeError);
    a.throws(() => new SwissMap({}), TypeError);
    a.throws(() => m.reserve(-1), RangeError);
    a.throws(() => m.forEach(1), TypeError);
    a.ok(["neon16", "sse2x16", "swar8"].includes(m.groupImpl), "groupImpl names the probe: " + m.groupImpl);
});

t.test("SwissMap holding itself is collected (gc_mark)", (a) => {
    for (let i = 0; i < 50; i++) { const m = new SwissMap(); m.set(m, m); m.set("o", { m }); }
    std.gc();
    a.ok(true);
});

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

for (const T of [RBTree, WBTree, SkipList]) {
    t.test(T.name + " number keys agree with a sorted array", (a) => {
        const tr = new T(), mod = treeModel((x, y) => x - y);
        let bad = 0;
        for (let step = 0; step < 6000; step++) {
            const k = rnd(800) - 400 + (rnd(4) === 0 ? 0.5 : 0), op = rnd(10);
            if (op < 5) { tr.set(k, step); mod.set(k, step); }
            else if (op < 8) { if (tr.delete(k) !== mod.del(k)) bad++; }
            else {
                if (tr.get(k) !== mod.get(k) || tr.has(k) !== (mod.get(k) !== undefined)) bad++;
                if (tr.floorKey(k) !== mod.floor(k) || tr.ceilKey(k) !== mod.ceil(k)) bad++;
                if (tr.rank && tr.rank(k) !== mod.rank(k)) bad++;
            }
        }
        a.eq(bad, 0, "mismatches");
        a.deep(tr.keys(), mod.keys, "in-order keys");
        a.eq(tr.size, mod.keys.length);
        a.eq(tr.firstKey(), mod.keys[0]);
        a.eq(tr.lastKey(), mod.keys[mod.keys.length - 1]);
        if (tr.at) {
            for (let i = 0; i < mod.keys.length; i += 37) a.deep(tr.at(i), [mod.keys[i], mod.get(mod.keys[i])]);
            a.deep(tr.at(-1), [mod.keys[mod.keys.length - 1], mod.get(mod.keys[mod.keys.length - 1])]);
            a.eq(tr.at(mod.keys.length), undefined);
            a.eq(tr.at(0.5), undefined);
        }
        a.deep([...tr].map((e) => e[0]), mod.keys, "iterator yields entries in order");
    });

    t.test(T.name + " string keys order by UTF-8 bytes", (a) => {
        const tr = new T({ keys: "string" }), mod = treeModel(utf8cmp);
        const alpha = ["", "a", "b", "ab", "é", "￿", "😀", "\ud800", "z", "aé"];
        for (let i = 0; i < 2000; i++) {
            const k = alpha[rnd(alpha.length)] + alpha[rnd(alpha.length)];
            if (rnd(4) === 0) { a.eq(tr.delete(k), mod.del(k)); } else { tr.set(k, i); mod.set(k, i); }
        }
        a.deep(tr.keys(), mod.keys);
        a.eq(tr.get("😀"), mod.get("😀"));
        a.eq(tr.has(42), false, "a number key is coerced to its string");
    });

    t.test(T.name + " refusals and -0", (a) => {
        const tr = new T();
        a.throws(() => tr.set(NaN, 1), RangeError);
        a.eq(tr.get(NaN), undefined);
        a.eq(tr.has(NaN), false);
        a.eq(tr.delete(NaN), false);
        tr.set(-0, "z");
        a.eq(tr.get(0), "z");
        a.ok(Object.is(tr.firstKey(), 0), "-0 is stored as +0");
        a.throws(() => new T({ keys: "date" }), TypeError);
        a.throws(() => new T(5), TypeError);
        a.throws(() => T.prototype.set.call({}, 1, 1), TypeError);
        let calls = 0;
        const k = { valueOf() { calls++; tr.delete(0); return 7; } };
        tr.set(k, "seven");
        a.eq(calls, 1, "the key is coerced once");
        a.eq(tr.get(7), "seven");
        a.eq(tr.get(0), undefined, "mutation inside coercion is honoured");
    });

    t.test(T.name + " string key copies are freed on replace, delete and collection", (a) => {
        setNativeMemoryLimit(1 << 30);
        std.gc();
        const base = memoryUsage().nativeSize;
        let tr = new T({ keys: "string" });
        for (let i = 0; i < 200; i++) tr.set("key-" + (i % 50) + "-padding-to-make-it-longer", i);
        const held = memoryUsage().nativeSize;
        a.ok(held > base, "the tree holds native memory");
        for (let i = 0; i < 5000; i++) tr.set("key-" + (i % 50) + "-padding-to-make-it-longer", i);
        a.eq(memoryUsage().nativeSize, held, "replacing a value allocates nothing that stays");
        for (let i = 0; i < 25; i++) tr.delete("key-" + i + "-padding-to-make-it-longer");
        a.ok(memoryUsage().nativeSize < held, "delete releases the key");
        tr = null;
        std.gc();
        a.eq(memoryUsage().nativeSize, base, "collection releases everything");
        setNativeMemoryLimit(0);
    });

    t.test(T.name + " holding itself is collected", (a) => {
        for (let i = 0; i < 50; i++) { const tr = new T(); tr.set(1, tr); tr.set(2, { tr }); }
        std.gc();
        a.ok(true);
    });
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

t.test("PatriciaTrie string keys: longest prefix and prefix walk against a scan", (a) => {
    const p = new PatriciaTrie(), ref = new Map();
    const parts = ["a", "ab", "b", "é", "", "abc", "x"];
    for (let i = 0; i < 400; i++) {
        const k = parts[rnd(7)] + parts[rnd(7)] + parts[rnd(7)];
        if (rnd(5) === 0) a.eq(p.delete(k), ref.delete(k)); else { p.set(k, i); ref.set(k, i); }
    }
    a.eq(p.size, ref.size);
    let bad = 0;
    for (let i = 0; i < 300; i++) {
        const q = parts[rnd(7)] + parts[rnd(7)] + parts[rnd(7)] + parts[rnd(7)];
        let best;
        for (const k of ref.keys()) if (q.startsWith(k) && (best === undefined || k.length > best.length)) best = k;
        const got = p.longestPrefix(q);
        if (best === undefined ? got !== undefined : !(got && got[0] === best && got[1] === ref.get(best))) bad++;
        const pre = parts[rnd(7)];
        const want = [...ref.keys()].filter((k) => enc.encode(k).length >= enc.encode(pre).length && k.startsWith(pre)).sort();
        const walk = p.withPrefix(pre).map((e) => e[0]).sort();
        if (JSON.stringify(walk) !== JSON.stringify(want)) bad++;
    }
    a.eq(bad, 0);
});

t.test("PatriciaTrie byte keys and CIDR", (a) => {
    const p = new PatriciaTrie({ keys: "bytes" });
    p.set(new Uint8Array([1, 2]), "12").set(new Uint8Array([1, 2, 3]), "123").set(new Uint8Array([]), "root");
    const lp = p.longestPrefix(new Uint8Array([1, 2, 3, 4]));
    a.deep([...lp[0]], [1, 2, 3]);
    a.eq(lp[1], "123");
    a.eq(p.longestPrefix(new Uint8Array([9]))[1], "root");
    a.throws(() => p.set("str", 1), TypeError);
    a.throws(() => p.set(new Uint16Array([1, 2]), 1), TypeError, "byte keys are Uint8Array only");
    const c = new PatriciaTrie();
    c.addCIDR("10.0.0.0/8", "a").addCIDR("10.1.0.0/16", "b").addCIDR("0.0.0.0/0", "any4").addCIDR("2001:db8::/32", "doc6");
    a.eq(c.matchIP("10.1.200.3"), "b");
    a.eq(c.matchIP("10.2.0.1"), "a");
    a.eq(c.matchIP("192.168.0.1"), "any4");
    a.eq(c.matchIP("2001:db8::1"), "doc6");
    a.eq(c.matchIP("2001:db9::1"), undefined, "IPv4 /0 does not cover IPv6");
    a.eq(c.matchIP("::ffff:10.1.0.1"), undefined, "a mapped address is IPv6");
    a.eq(c.deleteCIDR("10.1.0.0/16"), true);
    a.eq(c.matchIP("10.1.200.3"), "a");
    a.throws(() => c.addCIDR("10.0.0.0/33", 1), RangeError);
    a.throws(() => c.addCIDR("10.0.0.0/", 1), RangeError);
    a.throws(() => c.addCIDR("10.0.0.0/8x", 1), RangeError);
    a.throws(() => c.addCIDR("300.0.0.0/8", 1), TypeError);
    a.throws(() => c.matchIP("a".repeat(100)), TypeError);
    a.throws(() => new PatriciaTrie({ keys: "x" }), TypeError);
    a.throws(() => c.set("a", 1), TypeError, "a CIDR trie refuses string keys");
    a.throws(() => c.withPrefix(""), TypeError);
    const k = new PatriciaTrie().set("a", 1);
    a.throws(() => k.addCIDR("10.0.0.0/12", 1), TypeError, "a string trie refuses CIDR blocks");
    a.throws(() => k.matchIP("10.0.0.1"), TypeError);
    k.delete("a");
    k.addCIDR("10.0.0.0/12", "x");
    a.eq(k.matchIP("10.15.0.1"), "x", "an emptied trie takes either kind again");
    a.eq(k.matchIP("10.16.0.1"), undefined, "/12 is not rounded to a byte");
});

function bellmanFord(n, edges, directed, src) {
    const d = new Array(n).fill(Infinity);
    d[src] = 0;
    for (let r = 0; r < n; r++)
        for (const [u, v, w = 1] of edges) {
            if (d[u] + w < d[v]) d[v] = d[u] + w;
            if (!directed && d[v] + w < d[u]) d[u] = d[v] + w;
        }
    return d;
}

t.test("CSRGraph BFS, Dijkstra, components against naive references", (a) => {
    for (const directed of [true, false]) {
        const n = 60, edges = [];
        for (let i = 0; i < 150; i++) edges.push([rnd(n), rnd(n), 1 + rnd(9)]);
        const g = new CSRGraph(n, edges, { directed });
        a.eq(g.nodes, n);
        a.eq(g.edgeCount, directed ? edges.length : edges.length * 2);
        const bf = bellmanFord(n, edges, directed, 0);
        const dj = g.dijkstra(0);
        let bad = 0;
        for (let v = 0; v < n; v++) {
            if (dj.dist[v] !== bf[v]) bad++;
            if (Number.isFinite(bf[v]) && v !== 0) {
                const u = dj.prev[v];
                const ok = edges.some(([x, y, w]) => (x === u && y === v || !directed && x === v && y === u) && bf[u] + w === bf[v]);
                if (!ok) bad++;
            }
        }
        a.eq(bad, 0, "dijkstra (directed=" + directed + ")");
        const hops = bellmanFord(n, edges.map(([u, v]) => [u, v, 1]), directed, 0);
        const bfs = g.bfs(0);
        a.deep([...bfs], hops.map((h) => (Number.isFinite(h) ? h : -1)), "bfs");
        const parent = Array.from({ length: n }, (_, i) => i);
        const find = (x) => (parent[x] === x ? x : (parent[x] = find(parent[x])));
        for (const [u, v] of edges) parent[find(u)] = find(v);
        const roots = new Set(parent.map((_, i) => find(i)));
        const comp = g.components();
        a.eq(comp.count, roots.size, "weak component count");
        let split = 0;
        for (const [u, v] of edges) if (comp.labels[u] !== comp.labels[v]) split++;
        a.eq(split, 0, "every edge stays inside one label");
        let deg = 0;
        for (let v = 0; v < n; v++) { deg += g.degree(v); a.eq(g.neighbors(v).length, g.weights(v).length); }
        a.eq(deg, g.edgeCount);
    }
});

t.test("CSRGraph pagerank matches a power iteration", (a) => {
    const n = 7, edges = [[0, 1], [1, 2], [2, 0], [3, 2], [4, 3], [4, 5], [1, 6]];
    const g = new CSRGraph(n, edges);
    const pr = g.pagerank({ damping: 0.85, iterations: 1000, tolerance: 0 });
    const out = new Array(n).fill(0);
    for (const [u] of edges) out[u]++;
    let r = new Array(n).fill(1 / n);
    for (let it = 0; it < 1000; it++) {
        let dangling = 0;
        for (let v = 0; v < n; v++) if (!out[v]) dangling += r[v];
        const nx = new Array(n).fill((1 - 0.85) / n + 0.85 * dangling / n);
        for (const [u, v] of edges) nx[v] += 0.85 * r[u] / out[u];
        r = nx;
    }
    for (let v = 0; v < n; v++) a.near(pr[v], r[v], 1e-9, "rank of " + v);
    a.near(pr.reduce((s, x) => s + x, 0), 1, 1e-12);
});

t.test("CSRGraph refusals", (a) => {
    a.throws(() => new CSRGraph(3, [[0, 3]]), RangeError);
    a.throws(() => new CSRGraph(3, [[0, 1, NaN]]), RangeError);
    a.throws(() => new CSRGraph(-1, []), RangeError);
    a.throws(() => new CSRGraph(2 ** 26 + 1, []), RangeError, "the node count is bounded");
    a.throws(() => new CSRGraph(3, {}), TypeError);
    a.throws(() => new CSRGraph(3, [5]), TypeError);
    const g = new CSRGraph(2, [[0, 1, -1]]);
    a.throws(() => g.dijkstra(0), RangeError);
    a.throws(() => g.bfs(2), RangeError);
    a.throws(() => g.pagerank({ damping: 2 }), RangeError);
    const e = new CSRGraph(0, []);
    a.eq(e.components().count, 0);
    a.eq(e.pagerank().length, 0);
});

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

t.test("serialize/deserialize keeps every observable", (a) => {
    const m = new SwissMap([[1, "a"], ["k", { deep: [1, 2] }], [NaN, 3], [5n, "big"]]);
    const m2 = SwissMap.deserialize(m.serialize());
    a.eq(m2.size, 4);
    a.deep(m2.get("k"), { deep: [1, 2] });
    a.eq(m2.get(NaN), 3);
    a.eq(m2.get(5n), "big");
    for (const T of [RBTree, WBTree, SkipList]) {
        const tr = new T();
        for (let i = 0; i < 100; i++) tr.set(rnd(1000) - 500, i);
        a.deep(T.deserialize(tr.serialize()).entries(), tr.entries(), T.name + " numbers");
        const ts = new T({ keys: "string" }).set("é", 1).set("a", 2);
        const back = T.deserialize(ts.serialize());
        a.deep(back.entries(), ts.entries(), T.name + " strings");
        a.throws(() => back.set(Symbol(), 1), TypeError, "the string key mode survives");
    }
    const x = new XArray();
    x.set(0, "z").set(2n ** 64n - 1n, "top").set(77, "m");
    x.mark(77, 0); x.mark(77, 2); x.mark(0, 1);
    const x2 = XArray.deserialize(x.serialize());
    a.deep(x2.entries(), x.entries());
    a.deep([0, 1, 2].map((k) => x2.isMarked(77, k)), [true, false, true]);
    a.eq(x2.isMarked(0, 1), true);
    const p = new PatriciaTrie({ keys: "bytes" }).set(new Uint8Array([1, 2]), "a").set(new Uint8Array([]), "e");
    const p2 = PatriciaTrie.deserialize(p.serialize());
    a.eq(p2.get(new Uint8Array([1, 2])), "a");
    a.eq(p2.get(new Uint8Array([])), "e");
    a.throws(() => p2.set("s", 1), TypeError, "byte mode survives");
    const c = new PatriciaTrie().addCIDR("10.0.0.0/12", "a").addCIDR("::/0", "six").addCIDR("1.2.3.4", "host");
    const c2 = PatriciaTrie.deserialize(c.serialize());
    a.eq(c2.matchIP("10.15.1.1"), "a");
    a.eq(c2.matchIP("10.16.1.1"), undefined);
    a.eq(c2.matchIP("fe80::1"), "six");
    a.eq(c2.matchIP("1.2.3.4"), "host");
    a.throws(() => c2.set("x", 1), TypeError, "CIDR space survives");
    a.eq(PatriciaTrie.deserialize(new PatriciaTrie().serialize()).size, 0);
    {
        const emptied = new PatriciaTrie().addCIDR("10.0.0.0/8", 1);
        emptied.deleteCIDR("10.0.0.0/8");
        const back = PatriciaTrie.deserialize(emptied.serialize());
        a.eq(back.size, 0, "a trie emptied by delete round-trips");
        back.set("k", 1);
        a.eq(back.get("k"), 1, "and takes either key space afterwards");
    }
    const g = new CSRGraph(5, [[0, 1, 2], [1, 2, 3], [3, 4, 1]], { directed: false });
    const g2 = CSRGraph.deserialize(g.serialize());
    a.eq(g2.edgeCount, g.edgeCount);
    a.deep([...g2.dijkstra(0).dist], [...g.dijkstra(0).dist]);
    a.deep(g2.components().count, 2);
    const r = new RStarTree(3);
    for (let i = 0; i < 300; i++) { const lo = [rnd(100), rnd(100), rnd(100)]; r.insert(lo, lo.map((v) => v + rnd(5)), i); }
    const r2 = RStarTree.deserialize(r.serialize());
    a.eq(r2.size, 300);
    a.eq(r2.dims, 3);
    a.deep(r2.search([10, 10, 10], [60, 60, 60]).sort((p, q) => p - q), r.search([10, 10, 10], [60, 60, 60]).sort((p, q) => p - q));
    for (const src of ["é😀é😀x", new Uint8Array([3, 1, 3, 1, 0])]) {
        const sa = new SuffixArray(src), sb = SuffixArray.deserialize(sa.serialize());
        a.deep([...sb.array()], [...sa.array()]);
        a.eq(sb.length, sa.length);
        a.deep(sb.find(typeof src === "string" ? "😀" : new Uint8Array([3, 1])), sa.find(typeof src === "string" ? "😀" : new Uint8Array([3, 1])));
    }
});

const HEADER = 20;
function resign(rec) {
    const len = rec.length - HEADER - 4;
    const dv = new DataView(rec.buffer, rec.byteOffset, rec.byteLength);
    dv.setUint32(12, len, true);
    dv.setUint32(16, 0, true);
    dv.setUint32(rec.length - 4, CRC32C(rec.subarray(0, rec.length - 4)) >>> 0, true);
    return rec;
}
function editPayload(rec, fn) {
    const body = [...rec.subarray(HEADER, rec.length - 4)];
    fn(body);
    const out = new Uint8Array(HEADER + body.length + 4);
    out.set(rec.subarray(0, HEADER));
    out.set(body, HEADER);
    return resign(out);
}

t.test("forged records reach the readers: payload truncations and flips, re-signed", (a) => {
    const samples = [
        [SwissMap, new SwissMap([[1, 2], ["a", [3]]])],
        [RBTree, new RBTree().set(1, 2).set(3, 4)],
        [WBTree, new WBTree({ keys: "string" }).set("x", 1)],
        [SkipList, new SkipList().set(5, 5)],
        [XArray, new XArray().set(9, 1).set(2n ** 60n, 2)],
        [PatriciaTrie, new PatriciaTrie().addCIDR("10.0.0.0/8", 1)],
        [PatriciaTrie, new PatriciaTrie().set("ab", 1)],
        [CSRGraph, new CSRGraph(3, [[0, 1, 1], [1, 2, 2]])],
        [RStarTree, new RStarTree(2).insert([0, 0], [1, 1], 4)],
        [SuffixArray, new SuffixArray("mississippi")],
    ];
    let threw = 0, decoded = 0, other = 0, crc = 0;
    const tryIt = (C, rec) => {
        try { const b = C.deserialize(rec); b.serialize(); decoded++; }
        catch (e) { if (!(e instanceof Error)) other++; else { threw++; if (/crc|checksum/i.test(e.message)) crc++; } }
    };
    for (const [C, o] of samples) {
        const rec = o.serialize();
        const plen = rec.length - HEADER - 4;
        for (let cut = 0; cut < plen; cut++) tryIt(C, editPayload(rec, (b) => { b.length = cut; }));
        for (let i = 0; i < plen; i++)
            for (const bit of [1, 0x80, 0xff]) tryIt(C, editPayload(rec, (b) => { b[i] ^= bit; }));
    }
    a.eq(other, 0, "every refusal is an Error");
    a.eq(crc, 0, "re-signed records pass the checksum, so the payload readers decide");
    a.ok(threw > 100 && decoded > 100, "both outcomes occur (" + threw + " refused, " + decoded + " decoded)");
});

t.test("forged records: each reader guard refuses its own forgery", (a) => {
    const tree = new RBTree().set(1, 2).serialize();
    a.throws(() => RBTree.deserialize(editPayload(tree, (b) => { b[0] = 1; })), /key 0 is not a string/);
    a.throws(() => RBTree.deserialize(editPayload(tree, (b) => { b[0] = 2; })), Error);
    const g = new CSRGraph(3, [[0, 2, 1]]).serialize();
    a.throws(() => CSRGraph.deserialize(editPayload(g, (b) => { b[0] = 2; })), /truncated DYNS record/, "an arc to node 2 of 2");
    a.throws(() => CSRGraph.deserialize(editPayload(g, (b) => { b.splice(16, 8, 0, 0, 0, 0, 0, 0, 0xf8, 0x7f); })), /truncated DYNS record/, "a NaN weight");
    a.eq(CSRGraph.deserialize(g).neighbors(0)[0], 2, "the unforged record decodes");
    const r = new RStarTree(1).insert([1], [2], 3).serialize();
    a.throws(() => RStarTree.deserialize(editPayload(r, (b) => { b.splice(5, 16, ...new Uint8Array(new Float64Array([5, 2]).buffer)); })), /truncated DYNS record/, "lo > hi");
    a.throws(() => RStarTree.deserialize(editPayload(r, (b) => { b[0] = 9; })), /truncated DYNS record/, "dims 9");
    a.throws(() => RStarTree.deserialize(editPayload(r, (b) => { b[0] = 0; })), /truncated DYNS record/, "dims 0");
    a.throws(() => RStarTree.deserialize(editPayload(r, (b) => { b[b.length - 1] = 0x40; })), /truncated DYNS record/, "an id above 2^53");
    a.eq(RStarTree.deserialize(r).search([0], [9])[0], 3);
    const sa = new SuffixArray("ab").serialize();
    a.throws(() => SuffixArray.deserialize(editPayload(sa, (b) => { b[0] = 2; })), Error);
    a.eq(SuffixArray.deserialize(editPayload(sa, (b) => { b[0] = 0; })).find(new Uint8Array([98]))[0], 1, "a string record re-read as bytes is still bytes");
    const pt = new PatriciaTrie().set("ab", 1).serialize();
    a.throws(() => PatriciaTrie.deserialize(editPayload(pt, (b) => { b[1] = 2; })), /does not fit its key space/, "string keys under a CIDR space");
    a.throws(() => PatriciaTrie.deserialize(editPayload(pt, (b) => { b[1] = 0; })), /key space does not match/, "entries under an empty space");
    a.throws(() => PatriciaTrie.deserialize(editPayload(pt, (b) => { b[1] = 3; })), Error);
    const cidr = new PatriciaTrie().addCIDR("10.0.0.0/12", 1).serialize();
    a.throws(() => PatriciaTrie.deserialize(editPayload(cidr, (b) => { b[1] = 1; })), /does not fit its key space/, "a CIDR key under the string space");
    const triples = (...vals) => {
        const d = new Deque();
        for (const v of vals) d.pushBack(v);
        const rec = d.serialize();
        return editPayload(cidr, (b) => { const blob = [...rec.subarray(HEADER + 4, rec.length - 4)]; b.length = 2; b.push(...blob); });
    };
    a.eq(PatriciaTrie.deserialize(triples(new Uint8Array([4, 10]), 16, "ok")).matchIP("10.0.0.1"), "ok", "the Deque-built record is a valid CIDR trie");
    const long = new Uint8Array(4096).fill(0x41);
    long[0] = 4;
    a.throws(() => PatriciaTrie.deserialize(triples(long, 16, "x")), /does not fit its key space/, "a CIDR key longer than its bit count");
    a.throws(() => PatriciaTrie.deserialize(triples(new Uint8Array([4, 10, 0, 0, 0, 0]), 40, "x")), /does not fit its key space/, "one byte past an IPv4 key");
    a.throws(() => PatriciaTrie.deserialize(triples(new Uint8Array([5, 10]), 16, "x")), /does not fit its key space/, "family 5");
    a.throws(() => PatriciaTrie.deserialize(triples(new Uint8Array([4, 10]), 41, "x")), /does not fit its key space/, "an IPv4 prefix of 33");
});

await t.run();
