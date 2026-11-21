// covers: contract dyna:structures.* -- BloomFilter and CountMinSketch deserialize accept every constructor size and refuse hostile lengths
// deserialize used to cap sketches at 2^22 words while the constructors allow
// 2^24 (BloomFilter 2^30 bits, CountMinSketch width * depth 2^24), so a large
// sketch could serialize but never deserialize. The cap is now the
// constructor's for the raw encoding, which carries every word. The sparse
// encoding keeps the 2^22 cap (test_structures_bomb_budget SEC-048/R5-2: a
// 60-byte record must not decode to 128 MB), so serialize writes anything
// above 2^22 words raw, even when it is nearly empty. Hostile records stay refused with a TypeError, never an
// allocation failure: dimensions beyond the constructor limit, and a raw
// payload shorter than the words it claims. A BloomFilter bit count near 2^32
// once wrapped the 32-bit word count to 0 and slipped past every check into
// the constructor's RangeError. A raw record is checked against the bytes it
// carries BEFORE the counters are allocated: a 60-byte record claiming 2^24
// raw counters used to allocate 128 MB and only then fail; under a 32 MB
// native cap that allocation would surface as out-of-memory, not TypeError.
// The check is in bytes, not words: a record carrying 2^23 bytes for 2^23
// claimed counters (one byte each instead of eight) must not pass it.
// Records are edited in place and
// re-sealed (DYNS trailer crc32c) so only the field under test is wrong.
import { BloomFilter, CountMinSketch } from "dyna:structures";
import { CRC32C } from "dyna:hash";
import { setNativeMemoryLimit } from "dyna:sys";
import { suite } from "../lib/kit.js";

const t = suite("structures_sketch_decode_cap");
const HDR = 20, EXT = 0xffffffff;

function reseal(b) {
    const dv = new DataView(b.buffer, b.byteOffset, b.length);
    dv.setUint32(b.length - 4, CRC32C(b.subarray(0, b.length - 4)) >>> 0, true);
    return b;
}
const dv = (b) => new DataView(b.buffer, b.byteOffset, b.length);

t.test("sizes above the old 2^22-word cap round-trip", (a) => {
    const c = new CountMinSketch((1 << 20) + (1 << 18), 4);
    c.add("x", 7); c.add("y", 3);
    const rec = c.serialize();
    a.eq(rec[HDR + 20], 0, "a nearly empty sketch above 2^22 counters is written raw");
    const c2 = CountMinSketch.deserialize(rec);
    a.eq(c2.count("x"), 7, "CountMinSketch with 5,242,880 counters");
    const b = new BloomFilter(((1 << 22) + 64) * 64, 3);
    b.add("k");
    const brec = b.serialize();
    a.eq(brec[HDR + 12], 0, "a nearly empty filter above 2^22 words is written raw");
    const b2 = BloomFilter.deserialize(brec);
    a.ok(b2.mayContain("k") && b2.bits === b.bits, "BloomFilter with 4,194,368 words");
});

t.test("dimensions beyond the constructor limit are refused", (a) => {
    const c = new CountMinSketch(64, 2).serialize();
    a.eq(dv(c).getUint32(HDR, true), EXT, "CountMinSketch record is the extended form");
    dv(c).setUint32(HDR + 4, 1 << 24, true);
    a.throws(() => CountMinSketch.deserialize(reseal(c)), TypeError, "width 2^24 x depth 2");
    const b = new BloomFilter(128, 3).serialize();
    a.eq(dv(b).getUint32(HDR, true), EXT, "BloomFilter record is the extended form");
    dv(b).setUint32(HDR + 4, 0xffffffff, true);
    a.throws(() => BloomFilter.deserialize(reseal(b)), TypeError, "2^32-1 bits");
    const b3 = new BloomFilter(128, 3).serialize();
    dv(b3).setUint32(HDR + 4, 2 ** 30 + 1, true);
    a.throws(() => BloomFilter.deserialize(reseal(b3)), TypeError, "2^30+1 bits, one word over");
});

t.test("a raw payload shorter than its claimed words is refused", (a) => {
    const c = new CountMinSketch(8, 2);
    for (let i = 0; i < 64; i++) c.add("k" + i, 2 ** 52 + i);
    const bytes = c.serialize();
    a.eq(bytes[HDR + 20], 0, "dense counters are stored raw");
    dv(bytes).setUint32(HDR + 4, 4096, true);
    a.throws(() => CountMinSketch.deserialize(reseal(bytes)), TypeError, "4096 x 2 counters claimed, 16 carried");
});

t.test("a short raw record is refused before its counters are allocated", (a) => {
    const c = new CountMinSketch(8, 2);
    for (let i = 0; i < 64; i++) c.add("k" + i, 2 ** 52 + i);
    const cms = c.serialize();
    dv(cms).setUint32(HDR + 4, 1 << 22, true);
    dv(cms).setUint32(HDR + 8, 4, true);
    const bf = new BloomFilter(64 * 64, 3);
    for (let i = 0; i < 4096; i++) bf.add("b" + i);
    const bloom = bf.serialize();
    a.eq(bloom[HDR + 12], 0, "dense filter words are stored raw");
    dv(bloom).setUint32(HDR + 4, 2 ** 30, true);
    const head = c.serialize().subarray(0, HDR);
    const mid = new Uint8Array(HDR + 21 + (1 << 23) + 4);
    mid.set(head);
    dv(mid).setBigUint64(12, BigInt(21 + (1 << 23)), true);
    dv(mid).setUint32(HDR, EXT, true);
    dv(mid).setUint32(HDR + 4, 1 << 21, true);
    dv(mid).setUint32(HDR + 8, 4, true);
    mid[HDR + 20] = 0;
    setNativeMemoryLimit(32 << 20);
    try {
        a.throws(() => CountMinSketch.deserialize(reseal(mid)), TypeError, "2^23 raw counters claimed, 2^23 bytes carried, under a 32 MB cap");
        a.throws(() => CountMinSketch.deserialize(reseal(cms)), TypeError, "2^24 raw counters claimed under a 32 MB cap");
        a.throws(() => BloomFilter.deserialize(reseal(bloom)), TypeError, "2^30 raw bits claimed under a 32 MB cap");
    } finally {
        setNativeMemoryLimit(0);
    }
});

await t.run();
