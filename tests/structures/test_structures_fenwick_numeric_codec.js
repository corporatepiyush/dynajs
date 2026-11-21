// covers: dyna:structures.Fenwick
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_fenwick_numeric_codec");
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

__kitS.section("Fenwick fresh", () => {
    const a = fenwickTrip(() => new Fenwick(N), "Fenwick fresh", 60);
    const b = fenwickTrip(() => new Fenwick(N * 8), "Fenwick fresh x8", 60);
    eq(a.length, b.length,
       "a constant record is the SAME size for 8x the elements");

    fenwickTrip(() => { const f = new Fenwick(N);
        for (let i = 0; i < N; i++) f.update(i, i % 50); return f; },
        "Fenwick small ints", 8 * N);

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
});

__kitS.section("negative fraction", () => {
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
});

__kitS.section("zigzag value", () => {
    for (const v of [0, 1, -1, 63, 64, -64, -65, 8191, 8192, -8192,
                     1048575, 1048576, 2147483647, -2147483648,
                     9007199254740992, -9007199254740992]) {
        const f = new Fenwick(8);
        f.update(2, v);
        const back = Fenwick.deserialize(f.serialize());
        eq(back.prefixSum(7), f.prefixSum(7), "zigzag value " + v);
    }
});

__kitS.section("no truncation may decode", () => {
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
            try { cls.deserialize(rec.subarray(0, len)); survived++; } catch (e) {  }
        }
        eq(survived, 0, label + ": no truncation may decode");
    }
});
__kitS.end();

if (fails === 0) print("test_structures_numeric_codec: all " + n + " checks passed");
else print("test_structures_numeric_codec: " + fails + " FAILED of " + n);
