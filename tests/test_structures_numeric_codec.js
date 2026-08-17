/*
 * The numeric-array record: Fenwick, SegTree and UnionFind.
 *
 * All three stored a fixed-width element unconditionally, so a fresh structure
 * cost hundreds of kilobytes to say nothing. Each now picks an arm from its
 * own data, and each arm is a way to be silently WRONG rather than to crash:
 *
 *   CONSTANT  every element bit-identical. Compared as bits, not with ==, so
 *             -0 and +0 stay distinct -- Object.is is what can see that, and a
 *             codec that conflated them round-trips its length perfectly.
 *   INT       every element an exact integer, zigzag varint. A value outside
 *             the exact-integer range must fall back, not truncate.
 *   RAW       neither holds. This arm must stay reachable, or a fraction is
 *             quietly rounded and every query returns a plausible wrong sum.
 *
 * UnionFind stores `parent[i] - i`, which is 0 for a root. A forged delta is
 * the dangerous input: it would make find() walk out of the array.
 *
 * The oracle is always the structure's own queries over the whole range, never
 * the record length -- a codec that shifts every value by one has the same size.
 */
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

/* Compare across the WHOLE index range, not a sample: an arm that mis-decodes
   one element in ten thousand passes any sample and is still broken. */
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
    /* find() PATH-COMPRESSES, so it rewrites the parent array. The fixed point
       has to be taken before the queries below, or it compares a record from
       before compression against one from after and fails for that reason. */
    check(sameBytes(rec, back.serialize()), label + ": re-encode is byte-identical");
    let bad = -1;
    for (let i = 0; i < src.size; i++)
        if (src.find(i) !== back.find(i)) { bad = i; break; }
    check(bad < 0, label + ": find differs at " + bad);
    /* And after identical compression on both sides they must AGREE again --
       which the first check cannot see, since it runs before any find(). */
    check(sameBytes(src.serialize(), back.serialize()),
          label + ": identical path compression gives identical records");
    if (expectUnder !== undefined)
        check(rec.length < expectUnder, label + ": " + rec.length + " bytes, want < " + expectUnder);
    return rec;
}

const N = 4000;

/* -------------------------------------------------- 1. each arm fires */
{
    /* CONSTANT: a fresh tree is all zeros, and its record must be a fixed size
       whatever the count -- that IS the property, so two sizes are compared. */
    const a = fenwickTrip(() => new Fenwick(N), "Fenwick fresh", 60);
    const b = fenwickTrip(() => new Fenwick(N * 8), "Fenwick fresh x8", 60);
    eq(a.length, b.length,
       "a constant record is the SAME size for 8x the elements");

    /* INT */
    fenwickTrip(() => { const f = new Fenwick(N);
        for (let i = 0; i < N; i++) f.update(i, i % 50); return f; },
        "Fenwick small ints", 8 * N);

    /* RAW: a fraction cannot be an integer delta, so the arm must fall back
       rather than round. This is the case that would be silently WRONG. */
    const raw = fenwickTrip(() => { const f = new Fenwick(N);
        for (let i = 0; i < N; i++) f.update(i, i + 0.5); return f; },
        "Fenwick fractions");
    check(raw.length > 8 * N * 0.9,
          "the fraction case really fell back to raw, got " + raw.length);

    segTrip(() => new SegTree(N, "sum"), "SegTree fresh", 60);
    segTrip(() => { const s = new SegTree(N, "sum");
        for (let i = 0; i < N; i++) s.update(i, i % 97); return s; },
        "SegTree ints", 16 * N);
    segTrip(() => { const s = new SegTree(N, "sum");
        for (let i = 0; i < N; i++) s.update(i, i * 1.5 + 0.25); return s; },
        "SegTree fractions");

    ufTrip(() => new UnionFind(N), "UnionFind fresh", 60);
    ufTrip(() => { const u = new UnionFind(N);
        for (let i = 1; i < N; i++) u.union(i - 1, i);
        for (let i = 0; i < N; i++) u.find(i); return u; },
        "UnionFind one compressed set", 5 * N);
}

/* -------------------------------- 2. the CONSTANT arm compares BITS
   -0 and +0 are == but not the same value, and a CONSTANT arm using == would
   collapse them while round-tripping the length perfectly.

   It has to be a SegTree, and specifically a "min" one. Fenwick ACCUMULATES --
   its cells start at +0 and `+0 + -0` is `+0`, so a Fenwick tree can never
   hold a negative zero and cannot see this bug at all. A sum SegTree has the
   same problem. Only min/max propagate the value itself. */
{
    const s = new SegTree(64, "min");
    for (let i = 0; i < 64; i++) s.update(i, -0);
    check(Object.is(s.rangeQuery(0, 63), -0),
          "the source really holds -0, or this section proves nothing");
    const back = SegTree.deserialize(s.serialize());
    /* rangeQuery(i, i) returns element i ALONE. A reduction cannot see this:
       -0 < +0 is false, so min() of the two is a tie and may return either. */
    for (let i = 0; i < 64; i++)
        check(Object.is(s.rangeQuery(i, i), back.rangeQuery(i, i)),
              "a tree of -0 keeps the sign at " + i +
              " -- got " + back.rangeQuery(i, i));

    /* All -0 except one +0: the array is no longer constant, so the arm must
       change AND the two zeros must stay distinguishable. */
    const g = new SegTree(64, "min");
    for (let i = 0; i < 64; i++) g.update(i, -0);
    g.update(7, 0);
    const gb = SegTree.deserialize(g.serialize());
    for (let i = 0; i < 64; i++)
        check(Object.is(g.rangeQuery(i, i), gb.rangeQuery(i, i)),
              "mixed -0/+0 survives at " + i + " -- got " + gb.rangeQuery(i, i));

    /* NaN is the other value == cannot compare: NaN != NaN, so an == scan
       would call an all-NaN array non-constant. Either arm must round-trip. */
    const h = new SegTree(32, "min");
    for (let i = 0; i < 32; i++) h.update(i, NaN);
    const hb = SegTree.deserialize(h.serialize());
    for (let i = 0; i < 32; i++)
        check(Object.is(h.rangeQuery(i, i), hb.rangeQuery(i, i)),
              "an all-NaN tree round-trips at " + i);
}

/* ------------------------ 3. values the INT arm must REFUSE
   Each is representable as a double but not as an exact integer, or is beyond
   the range where a double and an int64 agree. */
{
    const nasty = [
        ["fraction", 0.5],
        ["negative fraction", -0.25],
        ["past 2^53", 9007199254740994],
        ["large negative", -9007199254740994],
        ["huge", 1e300],
        ["Infinity", Infinity],
        ["-Infinity", -Infinity],
        ["NaN", NaN],
        ["tiny", 5e-324],
    ];
    for (const [label, v] of nasty) {
        const f = new Fenwick(16);
        f.update(3, v);
        const back = Fenwick.deserialize(f.serialize());
        for (let i = 0; i < 16; i++)
            check(Object.is(f.prefixSum(i), back.prefixSum(i)),
                  label + " survives exactly at " + i + " -- got " +
                  back.prefixSum(i) + " want " + f.prefixSum(i));
    }
}

/* --------------------------------- 4. varint boundaries in the INT arm
   Zigzag turns a signed value into a varint whose length steps at 63/64,
   8191/8192 and so on. Each side of a step must be exact. */
{
    for (const v of [0, 1, -1, 63, 64, -64, -65, 8191, 8192, -8192,
                     1048575, 1048576, 2147483647, -2147483648,
                     9007199254740992, -9007199254740992]) {
        const f = new Fenwick(8);
        f.update(2, v);
        const back = Fenwick.deserialize(f.serialize());
        eq(back.prefixSum(7), f.prefixSum(7), "zigzag value " + v);
    }
}

/* ------------------------------ 4b. the DELTA arm
   An ascending array has small differences and large values, so delta coding
   can be several times smaller than plain varints. The arm is chosen by
   summing BOTH encodings exactly, so a sequence delta coding would make WORSE
   still takes the flat arm -- and either way it must decode identically. */
{
    /* A RangeSet's bounds ARE strictly ascending, which a SegTree's array is
       not -- its internal nodes are subtree sums and jump around, so it could
       not show this even though it uses the same codec. */
    const asc = new RangeSet();
    for (let i = 0; i < 4096; i++) asc.add(i * 1000, i * 1000 + 500);
    const ra = asc.serialize();
    check(ra.length < 4096 * 2 * 3,
          "an ascending bound array delta-codes small, got " + ra.length +
          " for " + (4096 * 2) + " values");
    const ba = RangeSet.deserialize(ra);
    let bad = -1;
    for (let i = 0; i < 4096; i++)
        if (asc.contains(i * 1000 + 1) !== ba.contains(i * 1000 + 1) ||
            asc.contains(i * 1000 + 700) !== ba.contains(i * 1000 + 700)) { bad = i; break; }
    check(bad < 0, "ascending round-trips exactly, differs at " + bad);
    check(ba.size === asc.size, "ascending: range count");

    /* ALTERNATING extremes: every delta is huge. A "monotone means delta"
       heuristic would get this wrong; summing both sizes does not. */
    const alt = new SegTree(2048, "sum");
    for (let i = 0; i < 2048; i++) alt.update(i, (i % 2) ? 1 : 1000000000);
    const rl = alt.serialize();
    const bl = SegTree.deserialize(rl);
    bad = -1;
    for (let i = 0; i < 2048; i++)
        if (!Object.is(alt.rangeQuery(i, i), bl.rangeQuery(i, i))) { bad = i; break; }
    check(bad < 0, "alternating extremes round-trip exactly, differs at " + bad);
    check(rl.length < 2048 * 8,
          "the alternating case still beats raw f64, got " + rl.length);

    /* DESCENDING: every delta is negative, which zigzag has to carry. */
    const desc = new SegTree(2048, "sum");
    for (let i = 0; i < 2048; i++) desc.update(i, (2048 - i) * 10000);
    const bd = SegTree.deserialize(desc.serialize());
    bad = -1;
    for (let i = 0; i < 2048; i++)
        if (!Object.is(desc.rangeQuery(i, i), bd.rangeQuery(i, i))) { bad = i; break; }
    check(bad < 0, "descending round-trips exactly, differs at " + bad);

    /* THE CHOICE ITSELF. A min-tree's internal nodes are all the minimum, so
       the array is a run of equal values then leaves. With ALTERNATING leaves
       every delta is a huge jump and flat wins; with ASCENDING leaves delta
       wins. One bound both must clear, which only holds if each picks the
       smaller: forcing delta everywhere costs 1.50x on the first, and
       disabling it costs 1.81x on the second. */
    {
        const altMin = new SegTree(4096, "min");
        for (let i = 0; i < 4096; i++) altMin.update(i, (i % 2) ? 1 : 1000000000);
        const ascMin = new SegTree(4096, "min");
        for (let i = 0; i < 4096; i++) ascMin.update(i, i * 1000);
        const ra = altMin.serialize(), rb = ascMin.serialize();
        check(ra.length < 20000,
              "alternating leaves take the FLAT arm, got " + ra.length);
        check(rb.length < 20000,
              "ascending leaves take the DELTA arm, got " + rb.length);
        /* and both still decode exactly */
        const da = SegTree.deserialize(ra), db = SegTree.deserialize(rb);
        let bad = -1;
        for (let i = 0; i < 4096; i++)
            if (!Object.is(altMin.rangeQuery(i, i), da.rangeQuery(i, i)) ||
                !Object.is(ascMin.rangeQuery(i, i), db.rangeQuery(i, i))) { bad = i; break; }
        check(bad < 0, "both arms decode exactly, differs at " + bad);
    }

    /* deltas straddling a varint boundary in both directions */
    {
        const s = new SegTree(64, "sum");
        const vals = [0, 63, 64, -64, -65, 8191, 8192, -8192, 1 << 20, -(1 << 20)];
        for (let i = 0; i < vals.length; i++) s.update(i, vals[i]);
        const b = SegTree.deserialize(s.serialize());
        for (let i = 0; i < vals.length; i++)
            check(Object.is(s.rangeQuery(i, i), b.rangeQuery(i, i)),
                  "delta boundary value " + vals[i] + " survives");
    }
}

/* ------------------------------------------------- 5. sizes and edges */
{
    for (const k of [1, 2, 3, 7, 8, 9, 63, 64, 65, 1000]) {
        fenwickTrip(() => { const f = new Fenwick(k);
            for (let i = 0; i < k; i++) f.update(i, i); return f; }, "Fenwick n=" + k);
        segTrip(() => { const s = new SegTree(k, "sum");
            for (let i = 0; i < k; i++) s.update(i, i); return s; }, "SegTree n=" + k);
        ufTrip(() => { const u = new UnionFind(k);
            for (let i = 1; i < k; i++) if (i % 3) u.union(i - 1, i); return u; },
            "UnionFind n=" + k);
    }
    /* the min/max SegTree ops travel in the same record as sum */
    for (const op of ["sum", "min", "max"]) {
        segTrip(() => { const s = new SegTree(100, op);
            for (let i = 0; i < 100; i++) s.update(i, (i * 37) % 100); return s; },
            "SegTree op=" + op);
    }
}

/* ------------------------------ 6. UnionFind rank run-length
   The rank stream is run-length coded, so a structure whose ranks CHANGE often
   is the case that exercises the run boundaries rather than one long run. */
{
    ufTrip(() => { const u = new UnionFind(2000);
        /* union in a pattern that leaves many distinct ranks */
        for (let s = 1; s < 2000; s *= 2)
            for (let i = 0; i + s < 2000; i += s * 2) u.union(i, i + s);
        return u; }, "UnionFind with many distinct ranks");

    ufTrip(() => { const u = new UnionFind(500);
        for (let i = 0; i < 500; i++) u.union(i, (i * 271) % 500);
        return u; }, "UnionFind scattered parents");
}

/* --------------------------------------------- 7. ADVERSARIAL RECORDS
   Every count, delta and run length is a number the peer chose. The CRC is
   repaired after each mutation so the envelope does not mask the codec. */
{
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
                /* Both branches check, so the total is `attempts` whatever the
                   split -- a count that moves with the record bytes cannot be
                   compared between runs and hides a case that stopped running. */
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
      /* find() is the method a forged parent would walk out of the array. */
      sweep(u.serialize(), UnionFind, (o) => { for (let i = 0; i < o.size; i++) o.find(i); }, "UnionFind"); }

    /* A FORGED COUNT on the CONSTANT arm: the payload is one value whatever
       the count, so nothing but an explicit cap stops a huge allocation. */
    const HEADER = 20;                    /* then u32 sentinel, u32 count, u8 kind */
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

    /* A FORGED PARENT DELTA. The random sweep above cannot catch this: an
       out-of-range parent is a silent read past the array, not a crash, so
       only a record built to point somewhere specific can. The first delta
       sits right after the sentinel, n, sets and the kind byte; zigzag 0xFE
       is +127, and the structure has far fewer than 127 elements. */
    {
        const u = new UnionFind(8);
        for (let i = 1; i < 8; i++) u.union(i - 1, i);
        const b = u.serialize();
        eq(b[HEADER + 12], 1, "the unioned set really took the DELTA arm");
        /* Element 0 stays the root, so its delta is 0 -- a single zero byte.
           Replacing it with another SINGLE-byte varint keeps the rest of the
           stream aligned, so the only thing left to refuse is the bound. A
           byte with the high bit set would be a continuation and desync the
           stream, which throws for a different reason entirely. */
        eq(b[HEADER + 13], 0x00, "parent[0] is a root, so its delta is one zero byte");
        b[HEADER + 13] = 0x7E;             /* zigzag 126 -> +63, and n is 8 */
        repair(b);
        let threw = false;
        try { UnionFind.deserialize(b); } catch (e) { threw = true; }
        check(threw, "a parent delta pointing outside the array is refused");

        /* and the negative direction */
        const c = u.serialize();
        c[HEADER + 13] = 0x7F;             /* zigzag 127 -> -64 */
        repair(c);
        threw = false;
        try { UnionFind.deserialize(c); } catch (e) { threw = true; }
        check(threw, "a negative parent delta is refused");
    }

    /* A FORGED UnionFind IDENTITY count: same shape, no payload at all. */
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
}

/* ------------------------------------------- 8. truncation at every length */
{
    const f = new Fenwick(200);
    for (let i = 0; i < 200; i++) f.update(i, i);
    const u = new UnionFind(200);
    for (let i = 1; i < 200; i++) u.union(i - 1, i);
    const s = new SegTree(200, "sum");
    for (let i = 0; i < 200; i++) s.update(i, i);
    for (const [cls, rec, label] of [[Fenwick, f.serialize(), "Fenwick"],
                                     [UnionFind, u.serialize(), "UnionFind"],
                                     [SegTree, s.serialize(), "SegTree"]]) {
        let survived = 0;
        for (let len = 0; len < rec.length; len += Math.max(1, (rec.length / 120) | 0)) {
            try { cls.deserialize(rec.subarray(0, len)); survived++; } catch (e) { /* expected */ }
        }
        eq(survived, 0, label + ": no truncation may decode");
    }
}

if (fails === 0) print("test_structures_numeric_codec: all " + n + " checks passed");
else print("test_structures_numeric_codec: " + fails + " FAILED of " + n);
