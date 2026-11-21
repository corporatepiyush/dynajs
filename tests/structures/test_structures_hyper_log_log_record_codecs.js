// covers: dyna:structures.HyperLogLog
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_hyper_log_log_record_codecs");
import { Table, BloomFilter, CountMinSketch, HyperLogLog } from "dyna:structures";
import { Graph } from "dyna:graph";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }
function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

__kitS.section("a sparse BloomFilter compresses", () => {
    {
        const b = new BloomFilter(1 << 20, 4);
        for (let i = 0; i < 50; i++) b.add("k" + i);
        const rec = b.serialize();
        check(rec.length < (1 << 20) / 8 / 4,
              "a sparse BloomFilter compresses, got " + rec.length);
        const back = BloomFilter.deserialize(rec);
        let bad = null;
        for (let i = 0; i < 50; i++) if (!back.mayContain("k" + i)) { bad = i; break; }
        check(bad === null, "every sparse member survives, lost " + bad);
        for (let i = 0; i < 2000 && bad === null; i++)
            if (b.mayContain("miss" + i) !== back.mayContain("miss" + i)) bad = i;
        check(bad === null, "and every non-member answers identically, differs at " + bad);
        eq(back.bits, b.bits, "bit count");
        eq(back.hashes, b.hashes, "hash count");
    }

    {
        const b = new BloomFilter(1 << 16, 4);
        for (let i = 0; i < 40000; i++) b.add("k" + i);
        const rec = b.serialize();
        check(rec.length < (1 << 16) / 8 * 1.1,
              "a dense BloomFilter takes RAW and costs about the bit array, got " +
              rec.length + " vs " + ((1 << 16) / 8));
        const back = BloomFilter.deserialize(rec);
        let bad = null;
        for (let i = 0; i < 2000 && bad === null; i++)
            if (b.mayContain("k" + i) !== back.mayContain("k" + i)) bad = i;
        check(bad === null, "a dense filter round-trips, differs at " + bad);
    }

    for (const [label, load] of [["sparse", 40], ["loaded", 20000]]) {
        const s = new CountMinSketch(2048, 5);
        for (let i = 0; i < load; i++) s.add("k" + (i % (load / 2 | 0 || 1)), 1 + (i % 3));
        const rec = s.serialize();
        const back = CountMinSketch.deserialize(rec);
        eq(back.width, s.width, label + " CMS: width");
        eq(back.depth, s.depth, label + " CMS: depth");
        eq(back.totalCount, s.totalCount, label + " CMS: total");
        let bad = null;
        for (let i = 0; i < 200 && bad === null; i++) {
            const k = "k" + i;
            if (s.count(k) !== back.count(k)) bad = k;
        }
        check(bad === null, label + " CMS: counts differ at " + bad);
        check(sameBytes(rec, back.serialize()), label + " CMS: re-encode identical");
    }
    {
        const fresh = new CountMinSketch(4096, 5);
        check(fresh.serialize().length < 200,
              "an untouched CountMinSketch is tiny, got " + fresh.serialize().length);
    }

    {
        const fresh = new HyperLogLog(14);
        check(fresh.serialize().length < 100,
              "an untouched HyperLogLog is tiny, got " + fresh.serialize().length);
        eq(HyperLogLog.deserialize(fresh.serialize()).count(), 0,
           "and decodes to a zero estimate");

        const h = new HyperLogLog(14);
        for (let i = 0; i < 200000; i++) h.add("u" + i);
        const rec = h.serialize();
        const back = HyperLogLog.deserialize(rec);
        eq(back.precision, h.precision, "HLL: precision");
        eq(back.registers, h.registers, "HLL: register count");
        eq(back.count(), h.count(), "HLL: the estimate is bit-identical");
        check(sameBytes(rec, back.serialize()), "HLL: re-encode is byte-identical");
        check(rec.length < h.registers * 1.2,
              "a loaded HLL record stays near its register count, got " + rec.length);

        const other = new HyperLogLog(14);
        for (let i = 200000; i < 400000; i++) other.add("u" + i);
        const m1 = HyperLogLog.deserialize(h.serialize()); m1.merge(other);
        const m2 = HyperLogLog.deserialize(h.serialize()); m2.merge(other);
        eq(m1.count(), m2.count(), "a decoded sketch merges deterministically");
    }
});
__kitS.end();

if (fails === 0) print("test_structures_record_codecs: all " + n + " checks passed");
else print("test_structures_record_codecs: " + fails + " FAILED of " + n);
