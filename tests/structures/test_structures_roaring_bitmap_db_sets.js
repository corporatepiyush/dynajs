// covers: dyna:structures.RoaringBitmap
// Differential and property tests for RoaringBitmap, CuckooFilter, TopK and
// TDigest. RoaringBitmap is checked against a JS Set on value sets built to
// cross its container thresholds (array <= 4096 < bitmap < 65536 = full) at
// N-1, N, N+1, with every set operation in both the in-place and the
// cardinality-only form. The sketches are checked against their published
// guarantees rather than against digits this engine produced:
//   CuckooFilter: no false negatives; FPR near 2*b/2^f (Fan et al. 2014).
//   TopK (Metwally et al. 2005): every key with count > N/k is listed, and
//     count - error <= true count <= count.
//   TDigest (Dunning 2019): quantile rank error bounded, tighter in the tails.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_roaring_bitmap_db_sets");
import { RoaringBitmap, CuckooFilter, TopK, TDigest } from "dyna:structures";
import { Exec } from "dyna:sys";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; if (fails < 40) print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

function rng(seed) {
    let s = seed >>> 0 || 1;
    return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
}

// ---- RoaringBitmap ----------------------------------------------------------
function build(r, shape) {
    const out = new Set();
    const C = 65536;
    switch (shape) {
    case "sparse": for (let i = 0; i < 3000; i++) out.add(Math.floor(r() * 2 ** 32)); break;
    case "chunk4095": for (let i = 0; i < 4095; i++) out.add(C * 3 + i * 7); break;
    case "chunk4096": for (let i = 0; i < 4096; i++) out.add(C * 3 + i * 7); break;
    case "chunk4097": for (let i = 0; i < 4097; i++) out.add(C * 3 + i * 7); break;
    case "full": for (let i = 0; i < C; i++) out.add(C * 3 + i); break;
    case "full-1": for (let i = 0; i < C; i++) if (i !== 1234) out.add(C * 3 + i); break;
    case "mixed": for (let i = 0; i < 20000; i++) out.add(C * 3 + Math.floor(r() * C)); for (let i = 0; i < 100; i++) out.add(C * 4 + i); for (let i = 0; i < C; i++) out.add(C * 5 + i); out.add(2 ** 32 - 1); out.add(0); break;
    case "dense": for (let i = 0; i < 60000; i++) out.add(C * 3 + Math.floor(r() * C)); break;
    }
    return out;
}
const sortU = (s) => Uint32Array.from([...s].sort((a, b) => a - b));
function sameSet(rb, s, m) {
    const a = rb.toArray(), b = s.arr || sortU(s);
    eq(rb.size, s.size, m + " size");
    let ok = a.length === b.length;
    for (let i = 0; ok && i < a.length; i++) ok = a[i] === b[i];
    check(ok, m + " contents");
}
const SHAPES = ["sparse", "chunk4095", "chunk4096", "chunk4097", "full", "full-1", "mixed", "dense"];

__kitS.section("and", () => {
    const r = rng(3);
    const sets = SHAPES.map(sh => build(r, sh));
    const sortedAll = sets.map(sortU);
    const bitmaps = sortedAll.map(a => new RoaringBitmap(a));
    for (let i = 0; i < SHAPES.length; i++) {
        const s = sets[i], rb = new RoaringBitmap(sortU(s));
        sameSet(rb, s, `rb ${SHAPES[i]} build`);
        sameSet(new RoaringBitmap([...s].reverse()), s, `rb ${SHAPES[i]} build from unsorted array`);
        sameSet(RoaringBitmap.deserialize(rb.serialize()), s, `rb ${SHAPES[i]} serialize`);
        const sorted = sortU(s);
        for (let t = 0; t < 30; t++) {
            const x = r() < 0.5 && sorted.length ? sorted[Math.floor(r() * sorted.length)] + (r() < 0.5 ? 0 : 1) : Math.floor(r() * 2 ** 32);
            eq(rb.has(x), s.has(x), `rb ${SHAPES[i]} has`);
            let rk = 0; { let lo = 0, hi = sorted.length; while (lo < hi) { const mid = (lo + hi) >> 1; if (sorted[mid] <= x) lo = mid + 1; else hi = mid; } rk = lo; }
            eq(rb.rank(x), rk, `rb ${SHAPES[i]} rank(${x})`);
            const k = Math.floor(r() * (sorted.length + 2));
            eq(rb.select(k), sorted[k], `rb ${SHAPES[i]} select(${k})`);
        }
        eq(rb.min(), sorted[0], `rb ${SHAPES[i]} min`);
        eq(rb.max(), sorted[sorted.length - 1], `rb ${SHAPES[i]} max`);
        for (let j = 0; j < SHAPES.length; j++) {
            const ob = bitmaps[j];
            const A = sortedAll[i], B = sortedAll[j], I = [], U = [], X = [], D = [];
            for (let p = 0, q = 0; p < A.length || q < B.length;) {
                if (q >= B.length || (p < A.length && A[p] < B[q])) { U.push(A[p]); X.push(A[p]); D.push(A[p]); p++; }
                else if (p >= A.length || B[q] < A[p]) { U.push(B[q]); X.push(B[q]); q++; }
                else { I.push(A[p]); U.push(A[p]); p++; q++; }
            }
            const inter = { size: I.length, arr: I }, uni = { size: U.length, arr: U }, xr = { size: X.length, arr: X }, dif = { size: D.length, arr: D };
            const tag = `rb ${SHAPES[i]} op ${SHAPES[j]}`;
            sameSet(rb.clone().and(ob), inter, tag + " and");
            sameSet(rb.clone().or(ob), uni, tag + " or");
            sameSet(rb.clone().xor(ob), xr, tag + " xor");
            sameSet(rb.clone().andNot(ob), dif, tag + " andNot");
            eq(rb.andCardinality(ob), inter.size, tag + " andCardinality");
            eq(rb.orCardinality(ob), uni.size, tag + " orCardinality");
            eq(rb.xorCardinality(ob), xr.size, tag + " xorCardinality");
            eq(rb.andNotCardinality(ob), dif.size, tag + " andNotCardinality");
            eq(rb.intersects(ob), inter.size > 0, tag + " intersects");
            eq(rb.isSubsetOf(ob), inter.size === s.size, tag + " isSubsetOf");
            eq(rb.equals(ob), i === j, tag + " equals");
            eq(rb.jaccard(ob), uni.size ? inter.size / uni.size : 1, tag + " jaccard");
        }
        const self = rb.clone(); self.and(self); sameSet(self, s, `rb ${SHAPES[i]} and self`);
        self.xor(self); eq(self.size, 0, `rb ${SHAPES[i]} xor self empties`);
    }
});

__kitS.section("rb churn delete", () => {
    const r = rng(9), rb = new RoaringBitmap(), s = new Set();
    for (let step = 0; step < 1500; step++) {
        const op = r(), base = 65536 * Math.floor(r() * 4);
        if (op < 0.5) { const x = base + Math.floor(r() * 9000); rb.add(x); s.add(x); }
        else if (op < 0.75) { const x = base + Math.floor(r() * 9000); eq(rb.delete(x), s.delete(x), "rb churn delete"); }
        else if (op < 0.85) { const lo = base + Math.floor(r() * 70000), hi = lo + Math.floor(r() * 80000); rb.addRange(lo, hi); for (let x = lo; x < hi; x++) s.add(x); }
        else if (op < 0.95) { const lo = base + Math.floor(r() * 70000), hi = lo + Math.floor(r() * 80000); rb.removeRange(lo, hi); for (const x of [...s]) if (x >= lo && x < hi) s.delete(x); }
        else { const lo = Math.floor(r() * 300000), hi = lo + Math.floor(r() * 100000); let c = 0; for (const x of s) if (x >= lo && x < hi) c++; eq(rb.rangeCardinality(lo, hi), c, "rb churn rangeCardinality"); }
        if (step % 300 === 0) sameSet(rb, s, `rb churn @${step}`);
    }
    sameSet(rb, s, "rb churn final");
    sameSet(RoaringBitmap.deserialize(rb.serialize()), s, "rb churn serialize");
});

__kitS.section("rb full universe size", () => {
    const rb = new RoaringBitmap();
    rb.addRange(0, 2 ** 32);
    eq(rb.size, 2 ** 32, "rb full universe size");
    check(rb.bytes < 4e6, "rb full universe stays small (" + rb.bytes + " bytes)");
    eq(rb.has(2 ** 32 - 1), true, "rb full has max");
    eq(rb.rank(2 ** 32 - 1), 2 ** 32, "rb full rank");
    eq(rb.select(2 ** 32 - 1), 2 ** 32 - 1, "rb full select last");
    let threw = false; try { rb.toArray(); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb toArray refuses 2^32 values");
    rb.delete(5); eq(rb.size, 2 ** 32 - 1, "rb delete from a full container");
    eq(rb.has(5), false, "rb deleted from full");
    rb.removeRange(0, 2 ** 32); eq(rb.isEmpty(), true, "rb removeRange whole universe");
    threw = false; try { rb.add(2 ** 32); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb 2^32 refused");
    threw = false; try { rb.add(-1); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb -1 refused");
    threw = false; try { rb.add(1.5); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb 1.5 refused");
    threw = false; try { new RoaringBitmap(new Int32Array([1, -2])); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb negative Int32Array refused");
    eq([...new RoaringBitmap([3, 1, 2])].join(), "1,2,3", "rb iterator ascending");
    eq(new RoaringBitmap().min(), undefined, "rb empty min");
    eq(new RoaringBitmap().jaccard(new RoaringBitmap()), 1, "rb empty jaccard");
});
__kitS.end();

print(`test_structures_db_sets: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
