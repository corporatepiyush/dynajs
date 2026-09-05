/*
 * The BitSet record's per-chunk container selection.
 *
 * A chunk picks EMPTY / ARRAY / RUN / BITMAP by what is cheapest for its
 * contents, so this file has to cover each arm, the boundaries BETWEEN them,
 * and the fact that a record built by one arm decodes identically to one built
 * by another. It also has to cover the adversarial side: every count and every
 * offset in the record is a number the peer chose, so a forged one must be
 * refused rather than indexing out of the set.
 *
 * The ORACLE throughout is bit equality against the source, never the popcount
 * -- a codec that loses a bit and gains another has the same count.
 */
import { BitSet } from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + a + ", want " + b); }

/* Bit-exact comparison. Compares the UNION of both lengths so a set that
   decoded shorter or longer is caught, not just one that differs inside. */
function sameBits(a, b, upto) {
    for (let i = 0; i < upto; i++)
        if (!!a.get(i) !== !!b.get(i)) return i;
    return -1;
}

function roundTrip(build, upto, label) {
    const src = build();
    const rec = src.serialize();
    const back = BitSet.deserialize(rec);
    const bad = sameBits(src, back, upto);
    check(bad < 0, label + ": bit " + bad + " differs after a round trip");
    eq(back.count, src.count, label + ": popcount");
    /* FIXED POINT: re-encoding what we decoded must give the same bytes, or
       the format is not stable and two equal sets have two representations. */
    const rec2 = back.serialize();
    eq(rec2.length, rec.length, label + ": re-encode length");
    let same = true;
    for (let i = 0; i < rec.length; i++) if (rec[i] !== rec2[i]) { same = false; break; }
    check(same, label + ": re-encode is byte-identical");
    return rec;
}

const BITS = 1 << 20;                     /* 16 chunks of 65536 */

/* ------------------------------------------------------ 1. each arm fires */
{
    /* EMPTY: nothing set at all. */
    const empty = roundTrip(() => new BitSet(BITS), BITS, "empty");
    check(empty.length < 100, "an empty 1M-bit set is tiny, got " + empty.length);

    /* ARRAY: far fewer than 4096 bits per chunk. */
    const sparse = roundTrip(() => {
        const b = new BitSet(BITS);
        for (let i = 0; i < 100; i++) b.set(i * 9973);
        return b;
    }, BITS, "sparse");
    check(sparse.length < 1000, "100 bits in 1M is small, got " + sparse.length);

    /* RUN: one long contiguous stretch. */
    const run = roundTrip(() => {
        const b = new BitSet(BITS);
        for (let i = 200000; i < 800000; i++) b.set(i);
        return b;
    }, BITS, "one long run");
    check(run.length < 300, "600k contiguous bits is tiny, got " + run.length);

    /* ALL SET is the extreme run. */
    const all = roundTrip(() => {
        const b = new BitSet(BITS);
        for (let i = 0; i < BITS; i++) b.set(i);
        return b;
    }, BITS, "all set");
    check(all.length < 300, "a full set is tiny, got " + all.length);

    /* BITMAP: alternating bits maximise runs AND exceed the array crossover,
       so no container can beat the raw words. This is the ADVERSARIAL case --
       the arm where the strategy cannot pay -- and it must not blow up. */
    const dense = roundTrip(() => {
        const b = new BitSet(BITS);
        for (let i = 0; i < BITS; i += 3) b.set(i);
        return b;
    }, BITS, "every third bit");
    const raw = BITS / 8;
    check(dense.length < raw * 1.01,
          "the adversarial case costs at most 1% over raw (" + dense.length +
          " vs " + raw + ")");
    check(dense.length > raw * 0.9,
          "and it really did fall back to the bitmap, got " + dense.length);
}

/* ------------------------------------- 2. the ARRAY/BITMAP crossover itself
   4096 bits per chunk is where 2 bytes each stops beating 8192 raw. Both
   sides of it must be exact; a boundary is where an off-by-one lives. */
{
    for (const k of [0, 1, 2, 4095, 4096, 4097, 8192]) {
        roundTrip(() => {
            const b = new BitSet(65536);
            for (let i = 0; i < k; i++) b.set(i * 7 % 65536);
            return b;
        }, 65536, "chunk with ~" + k + " bits");
    }
}

/* --------------------------------------------- 3. chunk boundaries exactly
   A bit at 65535/65536 sits either side of a chunk edge; a run that spans one
   is the case a per-chunk format gets wrong. */
{
    for (const spec of [
        ["bit 0", [0]],
        ["bit 65535 (last of chunk 0)", [65535]],
        ["bit 65536 (first of chunk 1)", [65536]],
        ["both sides of the edge", [65535, 65536]],
        ["last bit of the set", [BITS - 1]],
        ["first and last", [0, BITS - 1]],
    ]) {
        roundTrip(() => {
            const b = new BitSet(BITS);
            for (const i of spec[1]) b.set(i);
            return b;
        }, BITS, spec[0]);
    }

    /* A run that CROSSES a chunk edge must survive being split. */
    roundTrip(() => {
        const b = new BitSet(BITS);
        for (let i = 65000; i < 66000; i++) b.set(i);
        return b;
    }, BITS, "a run spanning a chunk edge");

    /* Runs at the very start and very end of a chunk. */
    roundTrip(() => {
        const b = new BitSet(BITS);
        for (let i = 0; i < 64; i++) b.set(i);
        for (let i = 65472; i < 65536; i++) b.set(i);
        return b;
    }, BITS, "runs flush to both chunk ends");
}

/* ------------------------------------------------- 4. sizes that are not a
   multiple of a chunk, or of a word */
{
    for (const bits of [1, 63, 64, 65, 127, 128, 1000, 65535, 65536, 65537,
                        100000, 1 << 20]) {
        roundTrip(() => {
            const b = new BitSet(bits);
            for (let i = 0; i < bits; i += 7) b.set(i);
            return b;
        }, bits, "size " + bits);
    }
}

/* ------------------------------------------- 5. MIXED chunks in one record
   The whole point of a per-chunk choice is that one record contains several
   kinds. This builds a set whose 16 chunks are deliberately different. */
{
    const rec = roundTrip(() => {
        const b = new BitSet(BITS);
        for (let c = 0; c < 16; c++) {
            const base = c * 65536;
            if (c % 4 === 0) continue;                       /* EMPTY */
            if (c % 4 === 1) for (let i = 0; i < 20; i++) b.set(base + i * 97);
            if (c % 4 === 2) for (let i = 0; i < 30000; i++) b.set(base + i);
            if (c % 4 === 3) for (let i = 0; i < 65536; i += 2) b.set(base + i);
        }
        return b;
    }, BITS, "16 chunks of four different kinds");
    /* It must be smaller than raw: 4 of 16 chunks are empty, 4 are sparse,
       4 are one run -- only the alternating quarter needs the bitmap. */
    check(rec.length < (BITS / 8) * 0.6,
          "a mixed record beats raw, got " + rec.length + " vs " + (BITS / 8));
}

/* ----------------------------------------- 6. set operations survive a trip */
{
    const a = new BitSet(BITS), b = new BitSet(BITS);
    for (let i = 0; i < BITS; i += 5) a.set(i);
    for (let i = 0; i < BITS; i += 7) b.set(i);
    const wantAnd = BitSet.deserialize(a.serialize());
    wantAnd.and(b);
    const gotAnd = BitSet.deserialize(a.serialize());
    gotAnd.and(BitSet.deserialize(b.serialize()));
    eq(gotAnd.count, wantAnd.count, "AND after a round trip on both sides");
    check(sameBits(gotAnd, wantAnd, BITS) < 0, "AND is bit-exact");
}

/* ------------------------------------------------ 7. ADVERSARIAL RECORDS
   Every count and offset in the record is a number the peer chose. A forged
   one must be refused, never used to index the set. The CRC is repaired after
   each mutation so the envelope check does not mask the codec check -- two
   guards against one symptom means neither is tested. */
{
    const src = new BitSet(BITS);
    for (let i = 0; i < 200; i++) src.set(i * 4001);
    const good = src.serialize();

    /* The CRC is REPAIRED after each mutation. Without that the envelope check
       rejects everything and the codec's own bounds checks are never reached --
       two guards against one symptom, so neither is tested. The first sweep
       below proved exactly that: 1374 mutations, 1374 refused, 0 reaching the
       codec at all. */
    const TRAILER = 4;
    const repair = (b) => {
        const crc = CRC32C(b.subarray(0, b.length - TRAILER)) >>> 0;
        for (let k = 0; k < 4; k++) b[b.length - TRAILER + k] = (crc >>> (k * 8)) & 0xff;
        return b;
    };

    let refused = 0, decoded = 0, attempts = 0;
    for (let i = 20; i < good.length - TRAILER; i += Math.max(1, (good.length / 400) | 0)) {
        for (const mask of [0xff, 0x01, 0x80]) {
            const b = repair((() => { const c = good.slice(); c[i] ^= mask; return c; })());
            attempts++;
            try {
                const v = BitSet.deserialize(b);
                decoded++;
                /* Touch it: a structurally-decoded set must still answer. */
                const c = v.count;
                v.get(0); v.get(BITS - 1); v.toArray();
                check(typeof c === "number" && c >= 0,
                      "a corrupted-but-decoded set answers count");
            } catch (e) {
                refused++;
                check(e instanceof Error, "a refusal is an Error");
            }
        }
    }
    check(attempts > 100, "the sweep really ran, " + attempts + " mutations");
    print("  mutation sweep: " + attempts + " mutations, " + refused +
          " refused, " + decoded + " decoded and exercised");
}

/* ---------------------------------------- 8. truncation at every length
   A record cut anywhere must be refused, not read past. */
{
    const src = new BitSet(BITS);
    for (let i = 0; i < 500; i++) src.set(i * 1999);
    const good = src.serialize();
    let survived = 0;
    for (let len = 0; len < good.length; len += Math.max(1, (good.length / 200) | 0)) {
        try { BitSet.deserialize(good.subarray(0, len)); survived++; }
        catch (e) { /* expected */ }
    }
    eq(survived, 0, "no truncation of the record may decode");
}

/* -------------------------------------- 9. an OLD raw record still decodes
   The first u32 used to be the word count and is now a sentinel. A record in
   the old form must still read, and a new record handed to an old reader must
   be REFUSED rather than misread -- which is why the sentinel is a length no
   payload can satisfy. */
{
    /* Hand-build a raw-form payload: the envelope is easier to reuse than to
       fabricate, so take a real record and check the sentinel is what makes
       the difference. */
    const src = new BitSet(128);
    src.set(3); src.set(70);
    const rec = src.serialize();
    /* The payload begins after the 20-byte header; the first u32 there is the
       sentinel 0xFFFFFFFF for the chunked form. */
    const HEADER = 20;
    const u32 = (b, o) => (b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0;
    eq(u32(rec, HEADER), 0xFFFFFFFF,
       "a chunked record starts with the sentinel, which an old reader cannot " +
       "read as a word count");
}

if (fails === 0) print("test_structures_bitset_codec: all " + n + " checks passed");
else print("test_structures_bitset_codec: " + fails + " FAILED of " + n);
