/*
 * The sorted-key record: ARITHMETIC / DELTA_VARINT / RAW.
 *
 * A skiplist walk yields keys ascending, which is what makes them
 * compressible. Three arms are chosen from the sequence itself, so this file
 * has to prove each one fires, that the BOUNDARIES between them are exact, and
 * that a key the integer path cannot represent falls back rather than being
 * mangled -- a set of fractions silently rounded to integers would round-trip
 * its COUNT perfectly and be wrong in every value.
 *
 * The oracle is always the full key sequence compared element by element, in
 * order. Comparing `.size` would pass an implementation that lost one key and
 * invented another.
 */
import { SortedSet, SortedMap } from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + a + ", want " + b); }

function keysOf(s) { return s.toArray(); }

function roundTrip(build, label, expectBytesUnder) {
    const src = build();
    const rec = src.serialize();
    const back = SortedSet.deserialize(rec);
    const a = keysOf(src), b = keysOf(back);
    eq(b.length, a.length, label + ": key count");
    let bad = -1;
    for (let i = 0; i < a.length; i++)
        if (!Object.is(a[i], b[i])) { bad = i; break; }
    check(bad < 0, label + ": key " + bad + " differs (" +
          (bad >= 0 ? a[bad] + " vs " + b[bad] : "") + ")");
    /* FIXED POINT: two equal sets must have one representation. */
    const rec2 = back.serialize();
    eq(rec2.length, rec.length, label + ": re-encode length");
    let same = true;
    for (let i = 0; i < rec.length; i++) if (rec[i] !== rec2[i]) { same = false; break; }
    check(same, label + ": re-encode is byte-identical");
    if (expectBytesUnder !== undefined)
        check(rec.length < expectBytesUnder,
              label + ": expected under " + expectBytesUnder + " bytes, got " + rec.length);
    return rec;
}

const N = 20000;
const RAW = 8 * N + 28;                    /* what the old format cost */

/* ------------------------------------------------------- 1. each arm fires */
{
    /* ARITHMETIC: constant size whatever N is -- that IS the property. */
    const a1 = roundTrip(() => { const s = new SortedSet();
        for (let i = 0; i < N; i++) s.add(i); return s; },
        "dense 0..N", 100);
    const a2 = roundTrip(() => { const s = new SortedSet();
        for (let i = 0; i < N * 4; i++) s.add(i); return s; },
        "dense 0..4N", 100);
    eq(a1.length, a2.length,
       "an arithmetic record is the SAME size for 4x the keys -- the count is " +
       "described, not stored");

    /* negative, descending-magnitude, and non-unit steps are all arithmetic */
    roundTrip(() => { const s = new SortedSet();
        for (let i = 0; i < N; i++) s.add(i - N); return s; }, "negatives", 100);
    roundTrip(() => { const s = new SortedSet();
        for (let i = 0; i < N; i++) s.add(i * 7); return s; }, "step 7", 100);
    roundTrip(() => { const s = new SortedSet();
        for (let i = 0; i < N; i++) s.add(-N * 3 + i * 3); return s; },
        "negative start, step 3", 100);

    /* DELTA: integers whose gaps vary. 1 byte per key while gaps are small. */
    roundTrip(() => { const s = new SortedSet(); let v = 0;
        for (let i = 0; i < N; i++) { v += 1 + ((i * 2654435761) >>> 29); s.add(v); }
        return s; }, "clustered integers", RAW / 2);

    /* RAW: a fraction cannot be delta-coded exactly, so the arm must fall back
       rather than round. This is the case that would be silently WRONG. */
    const raw = roundTrip(() => { const s = new SortedSet();
        for (let i = 0; i < N; i++) s.add(i + 0.5); return s; }, "fractions");
    check(raw.length > RAW * 0.99,
          "fractions really did fall back to raw, got " + raw.length);
}

/* --------------------------------------- 2. the ARITHMETIC/DELTA boundary
   One key out of step must drop the whole sequence to DELTA and still be
   exact. This is the off-by-one that a "looks arithmetic" test would miss. */
{
    for (const breakAt of [1, 2, N / 2 | 0, N - 2, N - 1]) {
        roundTrip(() => {
            const s = new SortedSet();
            for (let i = 0; i < N; i++) s.add(i < breakAt ? i : i + 1);
            return s;
        }, "arithmetic broken at " + breakAt);
    }
    /* Exactly two keys is the smallest arithmetic sequence; one key has no
       step at all and must take DELTA. */
    roundTrip(() => { const s = new SortedSet(); s.add(5); return s; }, "one key");
    roundTrip(() => { const s = new SortedSet(); s.add(5); s.add(9); return s; },
              "two keys");
    roundTrip(() => new SortedSet(), "empty set");
}

/* ------------------------------------- 3. values the integer path must REFUSE
   Each of these is exactly representable as a double but not as an integer
   delta, or is an integer too large for the difference to stay exact. */
{
    const nasty = [
        ["fraction", [0.5, 1.5, 2.5]],
        ["tiny fraction", [0, 1e-300, 1]],
        ["negative zero", [-0, 1, 2]],
        ["past 2^53", [9007199254740994, 9007199254740996]],
        ["huge magnitude", [-1e300, 0, 1e300]],
        ["Infinity", [1, 2, Infinity]],
        ["-Infinity", [-Infinity, 0, 1]],
        ["mixed int and fraction", [1, 2, 2.5, 3]],
    ];
    for (const [label, vals] of nasty) {
        const src = new SortedSet();
        for (const v of vals) src.add(v);
        const back = SortedSet.deserialize(src.serialize());
        const a = keysOf(src), b = keysOf(back);
        eq(b.length, a.length, label + ": count");
        let ok = true;
        for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) ok = false;
        check(ok, label + ": values survive exactly -- got " + JSON.stringify(b) +
              " want " + JSON.stringify(a));
    }
}

/* ------------------------------------------------ 4. SortedMap keeps values
   The keys compress; the values are arbitrary JS and must not. */
{
    const m = new SortedMap();
    for (let i = 0; i < 5000; i++) m.set(i, "v" + i);
    const back = SortedMap.deserialize(m.serialize());
    eq(back.size, m.size, "SortedMap size");
    let ok = true;
    for (let i = 0; i < 5000; i++) if (back.get(i) !== "v" + i) { ok = false; break; }
    check(ok, "every SortedMap value survives a compressed-key record");
    /* mixed value types, including ones the engine stores specially */
    const m2 = new SortedMap();
    const vals = [null, true, false, 0, -0, 1.5, "s", NaN];
    for (let i = 0; i < vals.length; i++) m2.set(i, vals[i]);
    const b2 = SortedMap.deserialize(m2.serialize());
    for (let i = 0; i < vals.length; i++)
        check(Object.is(b2.get(i), vals[i]),
              "SortedMap value " + i + ": got " + b2.get(i) + " want " + vals[i]);
}

/* ------------------------------------------------------ 5. size sweep
   Delta coding has a per-element cost that varint makes size-dependent, so a
   record must be exact at every length, not just at a round one. */
{
    for (const k of [0, 1, 2, 3, 63, 64, 65, 127, 128, 129, 1000, 4096]) {
        roundTrip(() => {
            const s = new SortedSet();
            let v = 0;
            for (let i = 0; i < k; i++) { v += 1 + (i % 13); s.add(v); }
            return s;
        }, "delta set of " + k);
    }
}

/* -------------------------------------- 6. varint boundaries in the DELTA arm
   A gap of 127/128 is where LEB128 grows a byte; 2^14, 2^21 and 2^28 are the
   next boundaries. Each must round-trip exactly. */
{
    for (const gap of [1, 126, 127, 128, 129, 16383, 16384, 2097151, 2097152,
                       268435455, 268435456]) {
        roundTrip(() => {
            const s = new SortedSet();
            s.add(0); s.add(gap); s.add(gap * 2 + 1);   /* +1 breaks arithmetic */
            return s;
        }, "gap " + gap);
    }
}

/* ------------------------------------------------ 7. ADVERSARIAL RECORDS
   Every count and every varint is a number the peer chose. The CRC is repaired
   after each mutation so the ENVELOPE does not mask the codec -- two guards
   against one symptom means neither is tested. */
{
    const src = new SortedSet();
    let v = 0;
    for (let i = 0; i < 300; i++) { v += 1 + (i % 7); src.add(v); }
    const good = src.serialize();
    const TRAILER = 4;
    const repair = (b) => {
        const crc = CRC32C(b.subarray(0, b.length - TRAILER)) >>> 0;
        for (let k = 0; k < 4; k++) b[b.length - TRAILER + k] = (crc >>> (k * 8)) & 0xff;
        return b;
    };

    let attempts = 0, refused = 0, decoded = 0;
    for (let i = 20; i < good.length - TRAILER; i += Math.max(1, (good.length / 300) | 0)) {
        for (const mask of [0xff, 0x01, 0x80]) {
            const b = good.slice(); b[i] ^= mask; repair(b);
            attempts++;
            try {
                const o = SortedSet.deserialize(b);
                decoded++;
                /* Touch it: a decoded-but-corrupt set must still answer. */
                const arr = o.toArray();
                check(Array.isArray(arr) && arr.length === o.size,
                      "a corrupted set's toArray matches its size");
                o.has(0); o.first(); o.last();
            } catch (e) {
                refused++;
                check(e instanceof Error, "a refusal is an Error");
            }
        }
    }
    check(attempts > 100, "the sweep ran, " + attempts + " mutations");
    print("  mutation sweep: " + attempts + " mutations, " + refused +
          " refused, " + decoded + " decoded and exercised");

    /* A FORGED ARITHMETIC COUNT is the one the record cannot bound: the arm is
       25 bytes whatever the count, so only an explicit cap stops it demanding
       a huge allocation. Build a real arithmetic record and raise its count. */
    const ar = new SortedSet();
    for (let i = 0; i < 100; i++) ar.add(i);
    const arec = ar.serialize();
    const HEADER = 20;
    /* payload: u32 sentinel, u8 kind, u32 count, f64 first, f64 step */
    const kind = arec[HEADER + 4];
    check(kind === 1, "the dense record really took the ARITHMETIC arm, got " + kind);
    for (const forged of [0x7fffffff, 0xffffffff, 0x01000001]) {
        const b = arec.slice();
        for (let k = 0; k < 4; k++) b[HEADER + 5 + k] = (forged >>> (k * 8)) & 0xff;
        repair(b);
        let threw = false;
        try { SortedSet.deserialize(b); } catch (e) { threw = true; }
        check(threw, "a forged arithmetic count of " + forged + " must be refused");
    }
}

/* ------------------------------------------- 8. truncation at every length */
{
    const src = new SortedSet();
    let v = 0;
    for (let i = 0; i < 400; i++) { v += 1 + (i % 5); src.add(v); }
    const good = src.serialize();
    let survived = 0;
    for (let len = 0; len < good.length; len += Math.max(1, (good.length / 150) | 0)) {
        try { SortedSet.deserialize(good.subarray(0, len)); survived++; }
        catch (e) { /* expected */ }
    }
    eq(survived, 0, "no truncation of the record may decode");
}

if (fails === 0) print("test_structures_sorted_codec: all " + n + " checks passed");
else print("test_structures_sorted_codec: " + fails + " FAILED of " + n);
