// flags: --std
// covers: dyna:structures.PatriciaTrie
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

t.test("serialize/deserialize keeps every observable", (a) => {
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
        return editPayload(cidr, (b) => { const blob = [...rec.subarray(HEADER + 4, rec.length - 4)]; b.length = 2; b[0] &= 0x7f; b.push(...blob); });
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
