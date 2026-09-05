/* test_structures_codec_forge.js -- hand-built DYNS records that a mutation
 * sweep cannot produce.
 *
 * test_structures_serialize.js corrupts bytes OF REAL RECORDS. Every record a
 * container writes is the NEW form (an EXT sentinel + a checked header), so
 * two whole surfaces never see a byte of that sweep:
 *
 *   1. the OLD-form readers (raw u32 count + f64s), still kept so records
 *      written before the codec rework still read -- and, as measured, the
 *      place where `count * per` wrapped in 32-bit arithmetic and walked a
 *      consumer loop off an 8-byte allocation (a deterministic OOB read from
 *      a 4-byte payload);
 *   2. forgeries that are STRUCTURALLY legal -- correct counts, correct CRC,
 *      wrong relationships -- which byte corruption can only hit by accident.
 *
 * Every record here is built by hand, CRC-repaired, and must throw or decode
 * exactly as stated. Under ASan/UBSan the reject rows are the bounds test.
 */
import {
    BitSet, RangeSet, RangeMap, IntervalTree, MinMaxHeap, BTree,
    Multimap, Graph,
} from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind))
            { print("FAIL: " + m + " (wrong error: " + e + ")"); fails++; return; }
        return;
    }
    print("FAIL: " + m + " (no throw)"); fails++;
}

/* ---- byte assembly ------------------------------------------------------ */
const B = [];
function u8(v)  { B.push(v & 0xff); }
function u16(v) { u8(v); u8(v >>> 8); }
function u32(v) { u8(v); u8(v >>> 8); u8(v >>> 16); u8(v >>> 24); }
function u64(v) { let l = Number(v & 0xFFFFFFFFn), h = Number(v >> 32n);
                  u32(l); u32(h); }
function f64(x) {
    const b = new Uint8Array(new Float64Array([x]).buffer);
    for (let i = 0; i < 8; i++) u8(b[i]);
}
function blob(s) { u32(s.length); for (let i = 0; i < s.length; i++) u8(s.charCodeAt(i)); }
function uvarint(v) {
    do { let b = Number(v & 0x7Fn); v >>= 7n;
         if (v) b |= 0x80; u8(b); } while (v);
}
function svarint(v) { uvarint((v < 0 ? ((~BigInt(v)) << 1n) | 1n : BigInt(v) << 1n)); }

/* Envelope: "DYNS" | u16 version | u16 type_id | u32 flags | u64 len | payload
 * | u32 crc32c(everything before it). */
function forge(typeId, payload) {
    B.length = 0;
    u8(0x44); u8(0x59); u8(0x4E); u8(0x53);          /* DYNS */
    u16(1); u16(typeId); u32(0);
    u64(BigInt(payload.length));
    for (let i = 0; i < payload.length; i++) u8(payload[i]);
    const crc = CRC32C(new Uint8Array(B)) >>> 0;
    u32(crc);
    return new Uint8Array(B);
}

/* The sentinels and codec arms, from the source (NOT the runtime, so a test
 * cannot drift with an accidental renumber). */
const EXT = 0xFFFFFFFF;
const TID = { RANGESET: 24, RANGEMAP: 25, INTERVALTREE: 26,
              MINMAXHEAP: 27, BTREE: 30, MULTIMAP: 21, BITSET: 1, GRAPH: 14 };
const DS_BS_EMPTY = 0;
const BS_CHUNKW = 1024;

/* ==================================================================== *
 *  1. The old-form count overflow (the CRITICAL): RangeSet
 * ==================================================================== */
{
    /* Old form: the first u32 IS the pair count, raw f64s follow. n >= 2^31
     * made `n * 2` wrap in 32-bit arithmetic; the wrapped element count passed
     * the payload check and the loop read 2^31 pairs from an 8-byte buffer. */
    for (const first of [0x80000000, 0x80000001, 0xFFFFFFFF - 1,
                         0x7FFFFFFF, 0xC0000000]) {
        const p = [];
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(first);
        for (let i = 0; i < 64; i++) p.push(i);      /* bait for the OOB read */
        throws(() => RangeSet.deserialize(forge(TID.RANGESET, p)), TypeError,
               "old-form RangeSet count 0x" + first.toString(16) + " must be refused");
    }
    /* A legal old-form record still decodes: two RAW pairs, nothing else. */
    {
        const p = [];
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(2);
        for (const x of [1, 5, 20, 30]) {
            const b = new Uint8Array(new Float64Array([x]).buffer);
            for (let i = 0; i < 8; i++) p.push(b[i]);
        }
        const rs = RangeSet.deserialize(forge(TID.RANGESET, p));
        check(JSON.stringify(rs.ranges()) === JSON.stringify([[1, 5], [20, 30]]),
              "a legal old-form RangeSet record still decodes");
        check(rs.contains(3) && !rs.contains(10) && rs.contains(25),
              "and its ranges answer contains() correctly");
    }
}

/* ==================================================================== *
 *  2. The same overflow through every other pair reader
 * ==================================================================== */
{
    function pairs32(first) {
        const p = [];
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(first);
        for (let i = 0; i < 32; i++) p.push(0);
        return p;
    }
    throws(() => RangeMap.deserialize(forge(TID.RANGEMAP, pairs32(0x80000000))),
           TypeError, "old-form RangeMap count 2^31 refused");
    throws(() => IntervalTree.deserialize(forge(TID.INTERVALTREE, pairs32(0x80000000))),
           TypeError, "old-form IntervalTree count 2^31 refused");
    /* per=1 readers: the payload check alone must refuse 2^31 doubles. */
    throws(() => MinMaxHeap.deserialize(forge(TID.MINMAXHEAP, pairs32(0x80000000))),
           TypeError, "old-form MinMaxHeap count 2^31 refused");
    throws(() => BTree.deserialize(forge(TID.BTREE, pairs32(0x80000000))),
           TypeError, "old-form BTree count 2^31 refused");
    /* 2^30 elements is 8 GiB of f64s: still refused, this time by length. */
    throws(() => RangeSet.deserialize(forge(TID.RANGESET, pairs32(0x40000000))),
           TypeError, "old-form RangeSet count 2^30 refused by the payload bound");
}

/* ==================================================================== *
 *  3. BitSet chunked: both counts validated BEFORE the eager allocation
 * ==================================================================== */
{
    /* n words beyond the module ceiling: the old cap accepted 2^26 words
     * (512 MB) and allocated it before noticing the chunk stream was
     * 16 KB of EMPTY markers. 2^24 is the ctor's ceiling. */
    function chunked(nwords, nchunks, fill) {
        const p = [];
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(EXT);
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(nwords);
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(nchunks);
        while (fill-- > 0) p.push(DS_BS_EMPTY);
        return p;
    }
    const nBig = (1 << 24) + 1;
    throws(() => BitSet.deserialize(forge(TID.BITSET,
            chunked(nBig, Math.ceil(nBig / BS_CHUNKW), Math.ceil(nBig / BS_CHUNKW)))),
           TypeError, "chunked BitSet past the 2^24-word ceiling refused");
    /* A forged nchunks that does not match n must be refused BEFORE the
     * ensure() allocation, not after it. */
    throws(() => BitSet.deserialize(forge(TID.BITSET, chunked(1024, 0, 1))),
           TypeError, "chunked BitSet with a wrong chunk count refused");
    throws(() => BitSet.deserialize(forge(TID.BITSET, chunked(2048, 3, 2))),
           TypeError, "chunked BitSet with a forged smaller chunk count refused");
    /* A legal chunked record (two words, one EMPTY chunk) still decodes. */
    {
        const bs = BitSet.deserialize(forge(TID.BITSET, chunked(2, 1, 1)));
        check(bs.count === 0 && bs.toArray().length === 0,
              "a legal chunked BitSet record still decodes");
        bs.set(70);
        check(bs.get(70) === true, "and the decoded set is usable");
    }
}

/* ==================================================================== *
 *  4. Multimap: the per-key counts sum to more than the container ceiling
 * ==================================================================== */
{
    /* 257 keys x 65536 counts = 16.8M > DYN_MAX_CAPACITY (2^24). The old
     * reader accumulated the sum in a u32 -- which wrapped for large sums and
     * then drove a put loop of the unwrapped length. The cap must fire first. */
    const p = [];
    (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(257);
    for (let i = 0; i < 257; i++) {
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(3);
        p.push(0x6b, i & 0xff, (i >> 8) & 0xff);         /* "k" + u16 i */
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(65536);
    }
    while (p.length < 70000) p.push(0);                  /* counts must fit left */
    const t0 = Date.now();
    throws(() => Multimap.deserialize(forge(TID.MULTIMAP, p)), TypeError,
           "a Multimap record whose counts sum past the ceiling is refused");
    check(Date.now() - t0 < 2000, "and refused promptly, not after a 16M-put loop");
    /* A legal small record still decodes. */
    {
        const mm = new Multimap();
        mm.put("a", 1); mm.put("a", 2); mm.put("b", 3);
        const back = Multimap.deserialize(mm.serialize());
        check(JSON.stringify(back.entries().sort()) ===
              JSON.stringify([["a", 1], ["a", 2], ["b", 3]]),
              "a legal Multimap record still decodes");
    }
}

/* ==================================================================== *
 *  5. Graph: a non-finite weight smuggled through the wire
 * ==================================================================== */
{
    function graph(nanWeight) {
        const p = [1, 1];                  /* directed, weighted */
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(EXT);
        /* The node count is read with dyn_de_count (a u32); the per-node
         * DEGREE and the edge deltas are varints. */
        B.length = 0; u32(2); uvarint(1n); svarint(1);
        if (nanWeight) {
            const b = new Uint8Array(new Float64Array([NaN]).buffer);
            for (let i = 0; i < 8; i++) u8(b[i]);
        } else {
            f64(2.5);
        }
        uvarint(0n);
        for (let i = 0; i < B.length; i++) p.push(B[i]);
        return p;
    }
    throws(() => Graph.deserialize(forge(TID.GRAPH, graph(true))), TypeError,
           "a Graph record carrying a NaN weight is refused");
    {
        const g = Graph.deserialize(forge(TID.GRAPH, graph(false)));
        check(g.nodeCount === 2 && g.edgeCount === 1 &&
              g.dijkstra(0)[1] === 2.5,
              "a legal weighted Graph record still decodes");
    }
}

/* ==================================================================== *
 *  6. Wrong class: a valid record reached through the wrong deserialize
 * ==================================================================== */
{
    const rs = new RangeSet();
    rs.add(1, 5);
    const bytes = rs.serialize();
    throws(() => BitSet.deserialize(bytes), TypeError,
           "BitSet.deserialize refuses a RangeSet record");
    throws(() => BTree.deserialize(bytes), TypeError,
           "BTree.deserialize refuses a RangeSet record");
    check(RangeSet.deserialize(bytes).contains(3),
          "the same record reached through its own class still decodes");
}

/* ==================================================================== *
 *  7. IntervalTree wire bounds and count mismatches
 * ==================================================================== */
{
    /* Extract the trailing values blob from a real record's payload. */
    function payloadBytes(cls) {
        const b = cls.serialize();
        return Array.from(b.slice(20, b.length - 4));
    }
    function put32(p, at, v) {
        p[at] = v & 0xff; p[at + 1] = (v >>> 8) & 0xff;
        p[at + 2] = (v >>> 16) & 0xff; p[at + 3] = (v >>> 24) & 0xff;
    }
    function putF64(p, at, x) {
        const b = new Uint8Array(new Float64Array([x]).buffer);
        for (let i = 0; i < 8; i++) p[at + i] = b[i];
    }

    /* A NaN bound in a forged itree record must be SKIPPED, not stored: a
     * stored NaN lo makes the sort non-total and every query silently wrong. */
    {
        const real = new IntervalTree();
        real.insert(1.5, 2.5, "a");
        const p = payloadBytes(real);
        /* new form: EXT(4) | u32 e=2(4) | u8 kind=RAW(1) | 2 x f64(16) | blob */
        const bad = p.slice(0, 25);           /* header + pair slots, no blob */
        putF64(bad, 9, 1.5); putF64(bad, 17, NaN);
        bad.push.apply(bad, p.slice(25));     /* original 1-cell blob */
        const t = IntervalTree.deserialize(forge(TID.INTERVALTREE, bad));
        check(t.size === 0, "a NaN interval bound in a forged record is skipped");
        const inv = p.slice(0, 25);
        putF64(inv, 9, 2.5); putF64(inv, 17, 1.5);   /* inverted */
        inv.push.apply(inv, p.slice(25));
        const t2 = IntervalTree.deserialize(forge(TID.INTERVALTREE, inv));
        check(t2.size === 0, "an inverted interval in a forged record is skipped");
    }

    /* BTree: the values blob must hold EXACTLY n entries. */
    {
        const real = new BTree();
        real.set(1.5, "a");
        const p = payloadBytes(real);
        /* new form: EXT(4) | u32 e=1(4) | u8 kind(1) | f64(8) | blob(4+len).
         * A single key writes kind=CONSTANT (one f64); force RAW so the two
         * forged keys read as two f64s. */
        const forged = p.slice(0, 9);         /* EXT + e slot + kind */
        put32(forged, 4, 2);                  /* claim TWO keys */
        forged[8] = 0;                        /* kind = RAW */
        putF64(forged, 9, 1.5); putF64(forged, 17, 2.5);
        forged.push.apply(forged, p.slice(17)); /* original one-cell blob */
        throws(() => BTree.deserialize(forge(TID.BTREE, forged)), TypeError,
               "a BTree record whose values count disagrees with its keys is refused");
    }

    /* Graph: a varint degree past u32 used to wrap the guard's multiply and
     * demand a ~30 GB adjacency array before failing. */
    {
        B.length = 0;
        u8(1); u8(0);                                  /* directed, unweighted */
        u32(EXT); u32(2);                              /* 2 nodes */
        uvarint(2049638230412172402n);                 /* degree bomb */
        const p = Array.from(B);
        const t0 = Date.now();
        throws(() => Graph.deserialize(forge(TID.GRAPH, p)), TypeError,
               "a forged graph degree past 2^32 is refused");
        check(Date.now() - t0 < 2000, "and refused promptly, not after a giant allocation");
    }
}

if (fails === 0) print("test_structures_codec_forge: all " + n + " checks passed");
else print("test_structures_codec_forge: " + fails + " FAILED of " + n);
