// covers: dyna:structures.SegTree
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_seg_tree_numeric_codec");
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

__kitS.section("the source really holds -0, or this section proves nothing", () => {
    const s = new SegTree(64, "min");
    for (let i = 0; i < 64; i++) s.update(i, -0);
    check(Object.is(s.rangeQuery(0, 63), -0),
          "the source really holds -0, or this section proves nothing");
    const back = SegTree.deserialize(s.serialize());
    for (let i = 0; i < 64; i++)
        check(Object.is(s.rangeQuery(i, i), back.rangeQuery(i, i)),
              "a tree of -0 keeps the sign at " + i +
              " -- got " + back.rangeQuery(i, i));

    const g = new SegTree(64, "min");
    for (let i = 0; i < 64; i++) g.update(i, -0);
    g.update(7, 0);
    const gb = SegTree.deserialize(g.serialize());
    for (let i = 0; i < 64; i++)
        check(Object.is(g.rangeQuery(i, i), gb.rangeQuery(i, i)),
              "mixed -0/+0 survives at " + i + " -- got " + gb.rangeQuery(i, i));

    const h = new SegTree(32, "min");
    for (let i = 0; i < 32; i++) h.update(i, NaN);
    const hb = SegTree.deserialize(h.serialize());
    for (let i = 0; i < 32; i++)
        check(Object.is(h.rangeQuery(i, i), hb.rangeQuery(i, i)),
              "an all-NaN tree round-trips at " + i);
});

__kitS.section("an ascending bound array delta-codes small", () => {
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

    const desc = new SegTree(2048, "sum");
    for (let i = 0; i < 2048; i++) desc.update(i, (2048 - i) * 10000);
    const bd = SegTree.deserialize(desc.serialize());
    bad = -1;
    for (let i = 0; i < 2048; i++)
        if (!Object.is(desc.rangeQuery(i, i), bd.rangeQuery(i, i))) { bad = i; break; }
    check(bad < 0, "descending round-trips exactly, differs at " + bad);

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
        const da = SegTree.deserialize(ra), db = SegTree.deserialize(rb);
        let bad = -1;
        for (let i = 0; i < 4096; i++)
            if (!Object.is(altMin.rangeQuery(i, i), da.rangeQuery(i, i)) ||
                !Object.is(ascMin.rangeQuery(i, i), db.rangeQuery(i, i))) { bad = i; break; }
        check(bad < 0, "both arms decode exactly, differs at " + bad);
    }

    {
        const s = new SegTree(64, "sum");
        const vals = [0, 63, 64, -64, -65, 8191, 8192, -8192, 1 << 20, -(1 << 20)];
        for (let i = 0; i < vals.length; i++) s.update(i, vals[i]);
        const b = SegTree.deserialize(s.serialize());
        for (let i = 0; i < vals.length; i++)
            check(Object.is(s.rangeQuery(i, i), b.rangeQuery(i, i)),
                  "delta boundary value " + vals[i] + " survives");
    }
});

__kitS.section("Fenwick n", () => {
    for (const k of [1, 2, 3, 7, 8, 9, 63, 64, 65, 1000]) {
        fenwickTrip(() => { const f = new Fenwick(k);
            for (let i = 0; i < k; i++) f.update(i, i); return f; }, "Fenwick n=" + k);
        segTrip(() => { const s = new SegTree(k, "sum");
            for (let i = 0; i < k; i++) s.update(i, i); return s; }, "SegTree n=" + k);
        ufTrip(() => { const u = new UnionFind(k);
            for (let i = 1; i < k; i++) if (i % 3) u.union(i - 1, i); return u; },
            "UnionFind n=" + k);
    }
    for (const op of ["sum", "min", "max"]) {
        segTrip(() => { const s = new SegTree(100, op);
            for (let i = 0; i < 100; i++) s.update(i, (i * 37) % 100); return s; },
            "SegTree op=" + op);
    }
});
__kitS.end();

if (fails === 0) print("test_structures_numeric_codec: all " + n + " checks passed");
else print("test_structures_numeric_codec: " + fails + " FAILED of " + n);
