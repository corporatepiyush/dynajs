// covers: dyna:structures.UnionFind
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_union_find_numeric_codec");
import { Fenwick, SegTree, UnionFind, RangeSet } from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

function fenwickTrip(build, label, expectUnder) {
    const src = build(), rec = src.serialize(), back = Fenwick.deserialize(rec);
    eq(back.size, src.size, label + ": size");
    let bad = -1;
    for (let i = 0; i < src.size; i++)
        if (!Object.is(src.prefixSum(i), back.prefixSum(i))) { bad = i; break; }
    check(bad < 0, label + ": prefixSum differs at " + bad +
          (bad >= 0 ? " (" + src.prefixSum(bad) + " vs " + back.prefixSum(bad) + ")" : ""));
    check(sameBytes(rec, back.serialize()), label + ": re-encode is byte-identical");
    if (expectUnder !== undefined)
        check(rec.length < expectUnder, label + ": " + rec.length + " bytes, want < " + expectUnder);
    return rec;
}

function segTrip(build, label, expectUnder) {
    const src = build(), rec = src.serialize(), back = SegTree.deserialize(rec);
    eq(back.size, src.size, label + ": size");
    let bad = -1;
    for (let i = 0; i < src.size; i++)
        if (!Object.is(src.rangeQuery(i, src.size - 1),
                       back.rangeQuery(i, src.size - 1))) { bad = i; break; }
    check(bad < 0, label + ": rangeQuery differs at " + bad);
    check(sameBytes(rec, back.serialize()), label + ": re-encode is byte-identical");
    if (expectUnder !== undefined)
        check(rec.length < expectUnder, label + ": " + rec.length + " bytes, want < " + expectUnder);
    return rec;
}

function ufTrip(build, label, expectUnder) {
    const src = build(), rec = src.serialize(), back = UnionFind.deserialize(rec);
    eq(back.count, src.count, label + ": set count");
    check(sameBytes(rec, back.serialize()), label + ": re-encode is byte-identical");
    let bad = -1;
    for (let i = 0; i < src.size; i++)
        if (src.find(i) !== back.find(i)) { bad = i; break; }
    check(bad < 0, label + ": find differs at " + bad);
    check(sameBytes(src.serialize(), back.serialize()),
          label + ": identical path compression gives identical records");
    if (expectUnder !== undefined)
        check(rec.length < expectUnder, label + ": " + rec.length + " bytes, want < " + expectUnder);
    return rec;
}

const N = 4000;

__kitS.section("UnionFind with many distinct ranks", () => {
    ufTrip(() => { const u = new UnionFind(2000);
        for (let s = 1; s < 2000; s *= 2)
            for (let i = 0; i + s < 2000; i += s * 2) u.union(i, i + s);
        return u; }, "UnionFind with many distinct ranks");

    ufTrip(() => { const u = new UnionFind(500);
        for (let i = 0; i < 500; i++) u.union(i, (i * 271) % 500);
        return u; }, "UnionFind scattered parents");
});

__kitS.section("a decoded record answered", () => {
    const TRAILER = 4;
    const repair = (b) => {
        const crc = CRC32C(b.subarray(0, b.length - TRAILER)) >>> 0;
        for (let k = 0; k < 4; k++) b[b.length - TRAILER + k] = (crc >>> (k * 8)) & 0xff;
        return b;
    };
    const sweep = (good, cls, touch, label) => {
        let attempts = 0, refused = 0, decoded = 0;
        for (let i = 20; i < good.length - TRAILER; i += Math.max(1, (good.length / 200) | 0)) {
            for (const mask of [0xff, 0x01, 0x80]) {
                const b = good.slice(); b[i] ^= mask; repair(b);
                attempts++;
                try { const o = cls.deserialize(b); decoded++; touch(o);
                      check(true, "a decoded record answered"); }
                catch (e) { refused++; check(e instanceof Error, "a refusal is an Error"); }
            }
        }
        check(attempts > 50, label + ": the sweep ran, " + attempts + " mutations");
        print("  " + label + ": " + attempts + " mutations, " + refused +
              " refused, " + decoded + " decoded and exercised");
    };

    { const f = new Fenwick(300);
      for (let i = 0; i < 300; i++) f.update(i, i % 11);
      sweep(f.serialize(), Fenwick, (o) => { o.prefixSum(0); o.prefixSum(o.size - 1); }, "Fenwick"); }
    { const u = new UnionFind(300);
      for (let i = 1; i < 300; i++) if (i % 4) u.union(i - 1, i);
      sweep(u.serialize(), UnionFind, (o) => { for (let i = 0; i < o.size; i++) o.find(i); }, "UnionFind"); }

    const HEADER = 20;
    const u32at = (b, o) => (b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0;
    const fresh = new Fenwick(100).serialize();
    eq(u32at(fresh, HEADER), 0xFFFFFFFF, "a new-form record starts with the sentinel");
    eq(fresh[HEADER + 8], 1, "a fresh tree really took the CONSTANT arm");
    for (const forged of [0x7fffffff, 0xffffffff, 0x02000000]) {
        const b = fresh.slice();
        for (let k = 0; k < 4; k++) b[HEADER + 4 + k] = (forged >>> (k * 8)) & 0xff;
        repair(b);
        let threw = false;
        try { Fenwick.deserialize(b); } catch (e) { threw = true; }
        check(threw, "a forged constant-arm count of " + forged + " is refused");
    }

    {
        const u = new UnionFind(8);
        for (let i = 1; i < 8; i++) u.union(i - 1, i);
        const b = u.serialize();
        eq(b[HEADER + 12], 1, "the unioned set really took the DELTA arm");
        eq(b[HEADER + 13], 0x00, "parent[0] is a root, so its delta is one zero byte");
        b[HEADER + 13] = 0x7E;
        repair(b);
        let threw = false;
        try { UnionFind.deserialize(b); } catch (e) { threw = true; }
        check(threw, "a parent delta pointing outside the array is refused");

        const c = u.serialize();
        c[HEADER + 13] = 0x7F;
        repair(c);
        threw = false;
        try { UnionFind.deserialize(c); } catch (e) { threw = true; }
        check(threw, "a negative parent delta is refused");
    }

    const ufresh = new UnionFind(100).serialize();
    eq(ufresh[HEADER + 12], 0, "a fresh UnionFind really took the IDENTITY arm");
    for (const forged of [0x7fffffff, 0x02000000]) {
        const b = ufresh.slice();
        for (let k = 0; k < 4; k++) b[HEADER + 4 + k] = (forged >>> (k * 8)) & 0xff;
        repair(b);
        let threw = false;
        try { UnionFind.deserialize(b); } catch (e) { threw = true; }
        check(threw, "a forged identity-arm count of " + forged + " is refused");
    }
});

__kitS.section("a two-node parent cycle is refused at load", () => {
    const good = new UnionFind(2).serialize();
    function craft(parents) {
        const n = parents.length, body = 8 + 5 * n;
        const r = new Uint8Array(20 + body + 4);
        r.set(good.subarray(0, 20));
        const dv = new DataView(r.buffer);
        dv.setBigUint64(12, BigInt(body), true);
        dv.setUint32(20, n, true);
        dv.setUint32(24, 1, true);
        parents.forEach((p, i) => { dv.setUint32(28 + 5 * i, p, true); r[32 + 5 * i] = 0; });
        dv.setUint32(r.length - 4, CRC32C(r.subarray(0, r.length - 4)), true);
        return r;
    }
    let e = null;
    try { UnionFind.deserialize(craft([1, 0])); } catch (x) { e = x; }
    check(e !== null, "a two-node parent cycle is refused at load");
    e = null;
    try { UnionFind.deserialize(craft([1, 2, 0, 3])); } catch (x) { e = x; }
    check(e !== null, "a three-node cycle beside a root is refused at load");
    let u = null;
    try { u = UnionFind.deserialize(craft([1, 1, 1, 3])); } catch (x) { e = x; }
    check(u !== null && u.find(0) === 1 && u.find(3) === 3, "CONTROL: an acyclic raw record loads and finds its roots");
});
__kitS.end();

if (fails === 0) print("test_structures_numeric_codec: all " + n + " checks passed");
else print("test_structures_numeric_codec: " + fails + " FAILED of " + n);
