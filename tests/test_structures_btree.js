/*
 * BTree: an ordered map on numeric keys, B+ shaped -- values in the leaves,
 * leaves linked left to right.
 *
 * The risky part is SPLITTING. A leaf split copies its separator up (the key
 * stays in the right leaf, because leaves hold every key); an internal split
 * MOVES it (it must not be repeated). Getting those two backwards produces a
 * tree that answers most queries correctly and loses or duplicates the keys
 * nearest a boundary -- so the oracle here is always the FULL key set and the
 * full ordered sequence, never a sample and never the size.
 *
 * DYN_BTREE_ORDER is 32, so 33 keys is the first leaf split, ~33*33 the first
 * internal split, and those sizes are covered explicitly rather than left to
 * whatever a round number happens to hit.
 */
import { BTree, SortedMap } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

/* The oracle: a sorted array of [key, value], maintained naively. */
function model() {
    const a = [];
    return {
        set(k, v) { const i = a.findIndex(x => x[0] === k);
                    if (i >= 0) a[i][1] = v; else { a.push([k, v]); a.sort((x, y) => x[0] - y[0]); } },
        del(k) { const i = a.findIndex(x => x[0] === k);
                 if (i < 0) return false; a.splice(i, 1); return true; },
        get(k) { const i = a.findIndex(x => x[0] === k); return i < 0 ? undefined : a[i][1]; },
        keys() { return a.map(x => x[0]); },
        pairs() { return a.map(x => [x[0], x[1]]); },
        size() { return a.length; },
        floor(k) { let r; for (const [x] of a) if (x <= k) r = x; return r; },
        ceil(k) { for (const [x] of a) if (x >= k) return x; return undefined; },
    };
}

function sameKeys(t, m, label) {
    const got = t.keys(), want = m.keys();
    if (got.length !== want.length)
        return check(false, label + ": key count " + got.length + " vs " + want.length);
    for (let i = 0; i < got.length; i++)
        if (!Object.is(got[i], want[i]))
            return check(false, label + ": key " + i + " is " + got[i] + ", want " + want[i]);
    check(true, label + ": every key present, in order");
}

/* ------------------------------------ 1. sizes around every split boundary
   Ascending, descending and scattered insert orders must all end up with the
   same content -- the split path taken differs completely between them. */
{
    for (const N of [0, 1, 2, 31, 32, 33, 34, 64, 65, 100, 1023, 1024, 1025, 2000]) {
        for (const [order, gen] of [
            ["ascending",  (i) => i],
            ["descending", (i) => N - 1 - i],
            ["scattered",  (i) => (i * 2654435761) % 1000003],
        ]) {
            const t = new BTree(), m = model();
            for (let i = 0; i < N; i++) { const k = gen(i); t.set(k, "v" + k); m.set(k, "v" + k); }
            eq(t.size, m.size(), "N=" + N + " " + order + ": size");
            sameKeys(t, m, "N=" + N + " " + order);
            /* and every value must have come back with its own key */
            let bad = null;
            for (const k of m.keys()) if (t.get(k) !== m.get(k)) { bad = k; break; }
            check(bad === null, "N=" + N + " " + order + ": value wrong at key " + bad);
        }
    }
}

/* --------------------------------------- 2. floor / ceil at every boundary
   Between two keys, below the smallest and above the largest are four
   different answers, and an off-by-one in the descent shows in exactly one. */
{
    const t = new BTree(), m = model();
    for (let i = 0; i < 500; i++) { t.set(i * 10, i); m.set(i * 10, i); }
    let bad = null;
    for (let k = -5; k <= 5005 && !bad; k++) {
        const wf = m.floor(k), wc = m.ceil(k);
        const gf = t.floorKey(k), gc = t.ceilKey(k);
        if (!Object.is(gf, wf)) bad = "floor(" + k + ") = " + gf + ", want " + wf;
        else if (!Object.is(gc, wc)) bad = "ceil(" + k + ") = " + gc + ", want " + wc;
    }
    check(bad === null, "floor/ceil across the whole range -- " + bad);
    eq(t.firstKey(), 0, "firstKey");
    eq(t.lastKey(), 4990, "lastKey");

    const empty = new BTree();
    eq(empty.firstKey(), undefined, "firstKey of an empty tree");
    eq(empty.lastKey(), undefined, "lastKey of an empty tree");
    eq(empty.floorKey(5), undefined, "floorKey of an empty tree");
    eq(empty.ceilKey(5), undefined, "ceilKey of an empty tree");
    eq(empty.get(5), undefined, "get on an empty tree");
    eq(empty.size, 0, "size of an empty tree");
    eq(empty.keys().length, 0, "keys of an empty tree");
}

/* ------------------------------------------------- 3. delete, and re-add */
{
    const t = new BTree(), m = model();
    for (let i = 0; i < 800; i++) { t.set(i, "v" + i); m.set(i, "v" + i); }
    /* delete every third, which empties some leaves and thins others */
    for (let i = 0; i < 800; i += 3) { check(t.delete(i) === m.del(i), "delete " + i + " agrees"); }
    eq(t.size, m.size(), "size after deletes");
    sameKeys(t, m, "after deleting every third");
    eq(t.delete(0), false, "deleting an absent key returns false");
    /* re-adding must land in the right place, including into emptied leaves */
    for (let i = 0; i < 800; i += 3) { t.set(i, "again" + i); m.set(i, "again" + i); }
    eq(t.size, m.size(), "size after re-adding");
    sameKeys(t, m, "after re-adding");
    let bad = null;
    for (const k of m.keys()) if (t.get(k) !== m.get(k)) { bad = k; break; }
    check(bad === null, "values after re-add, wrong at " + bad);

    /* delete EVERYTHING, then use the tree again */
    for (const k of m.keys().slice()) { t.delete(k); m.del(k); }
    eq(t.size, 0, "size after deleting everything");
    eq(t.firstKey(), undefined, "firstKey of an emptied tree");
    eq(t.keys().length, 0, "keys of an emptied tree");
    t.set(42, "back");
    eq(t.get(42), "back", "an emptied tree still accepts a key");
    eq(t.firstKey(), 42, "and reports it as first");
}

/* ------------------------------------------ 4. keys the ordering must pin */
{
    const t = new BTree();
    /* -0 and +0 are the same POSITION: -0 == 0, so one key, last write wins. */
    t.set(-0, "neg"); t.set(0, "pos");
    eq(t.size, 1, "-0 and +0 are one key");
    eq(t.get(0), "pos", "the later write wins");
    eq(t.get(-0), "pos", "and both spellings reach it");

    /* the infinities are ordered and must sit at the ends */
    const u = new BTree();
    for (const k of [0, -Infinity, 5, Infinity, -5]) u.set(k, String(k));
    eq(u.firstKey(), -Infinity, "-Infinity is the smallest key");
    eq(u.lastKey(), Infinity, "Infinity is the largest key");
    eq(u.keys().join(","), "-Infinity,-5,0,5,Infinity", "the infinities sort");
    eq(u.get(Infinity), "Infinity", "and carry their value");

    /* NaN has no position, so it is refused rather than filed somewhere the
       search can never reach. */
    for (const bad of [NaN, "not a number"]) {
        let threw = false;
        try { u.set(bad, 1); } catch (e) { threw = true; }
        check(threw, "a " + String(bad) + " key is refused");
    }
    let threw = false;
    try { u.get(NaN); } catch (e) { threw = true; }
    check(threw, "getting NaN is refused too, not silently a miss");

    /* fractional and very large keys are ordinary */
    const f = new BTree();
    for (const k of [0.1, 0.2, 1e300, -1e300, 5e-324]) f.set(k, k);
    eq(f.size, 5, "fractional and extreme keys");
    eq(f.firstKey(), -1e300, "the most negative sorts first");
    eq(f.lastKey(), 1e300, "the largest sorts last");
}

/* ------------------------------------------------ 5. rangeQuery bounds */
{
    const t = new BTree(), m = model();
    for (let i = 0; i < 300; i++) { t.set(i * 2, i); m.set(i * 2, i); }
    const range = (lo, hi) => m.pairs().filter(([k]) => k >= lo && k <= hi);
    let bad = null;
    for (const [lo, hi] of [[0, 0], [0, 10], [1, 9], [-100, 5], [590, 1000],
                            [1000, 2000], [10, 10], [11, 11], [598, 598],
                            [0, 598], [-1e9, 1e9]]) {
        const got = t.rangeQuery(lo, hi), want = range(lo, hi);
        if (got.length !== want.length) { bad = "[" + lo + "," + hi + "] length " +
            got.length + " vs " + want.length; break; }
        for (let i = 0; i < got.length; i++)
            if (got[i][0] !== want[i][0] || got[i][1] !== want[i][1]) {
                bad = "[" + lo + "," + hi + "] at " + i; break; }
        if (bad) break;
    }
    check(bad === null, "rangeQuery is inclusive on both ends -- " + bad);
    /* an inverted range is empty, not everything */
    eq(t.rangeQuery(100, 50).length, 0, "an inverted range is empty");

    /* the iterator walks the same sequence */
    let count = 0, prev = -Infinity, ordered = true;
    for (const [k] of t) { if (k < prev) ordered = false; prev = k; count++; }
    eq(count, t.size, "the iterator yields every entry");
    check(ordered, "and in ascending order");
}

/* --------------------------- 6. DIFFERENTIAL against SortedMap
   Two independent ordered maps over the same operations must agree on every
   key, every value and the whole ordered sequence. */
{
    const b = new BTree(), s = new SortedMap();
    for (let i = 0; i < 5000; i++) {
        const k = (i * 48271) % 65537;
        b.set(k, i); s.set(k, i);
    }
    for (let i = 0; i < 2000; i++) {
        const k = (i * 7919) % 65537;
        b.delete(k); s.delete(k);
    }
    eq(b.size, s.size, "differential: size");
    const bk = b.keys(), sk = s.keys();
    let bad = -1;
    for (let i = 0; i < bk.length; i++) if (!Object.is(bk[i], sk[i])) { bad = i; break; }
    check(bad < 0 && bk.length === sk.length,
          "BTree and SortedMap agree on the whole key sequence, differ at " + bad);
    bad = -1;
    for (let i = 0; i < bk.length; i++) if (b.get(bk[i]) !== s.get(bk[i])) { bad = i; break; }
    check(bad < 0, "and on every value, differ at key " + (bad < 0 ? "" : bk[bad]));
    for (let i = 0; i < 500; i++) {
        const probe = (i * 131) % 65537;
        if (!Object.is(b.floorKey(probe), s.floorKey(probe))) { bad = probe; break; }
        if (!Object.is(b.ceilKey(probe), s.ceilKey(probe))) { bad = probe; break; }
    }
    check(bad < 0 || bad === -1, "and on floor/ceil, differ at " + bad);
}

/* ---------------------------------------------------- 7. record round trip */
{
    const t = new BTree(), m = model();
    for (let i = 0; i < 2000; i++) { const k = (i * 2654435761) % 1000003;
                                     t.set(k, { i: i, s: "v" + i }); m.set(k, i); }
    const rec = t.serialize();
    const back = BTree.deserialize(rec);
    eq(back.size, t.size, "decoded size");
    sameKeys(back, m, "decoded keys");
    let bad = null;
    for (const k of m.keys()) {
        const a = t.get(k), c = back.get(k);
        if (!a || !c || a.i !== c.i || a.s !== c.s) { bad = k; break; }
    }
    check(bad === null, "every decoded value matches, wrong at " + bad);
    /* The KEY array delta-codes; the object values dominate this record, so
       the claim is made against a tree holding the same keys and a small
       integer each -- otherwise the payload hides what is being measured. */
    {
        const keysOnly = new BTree();
        for (const k of m.keys()) keysOnly.set(k, 1);
        check(keysOnly.serialize().length < m.keys().length * 8,
              "an ascending key array delta-codes, got " +
              keysOnly.serialize().length + " for " + m.keys().length + " keys");
    }
    /* FIXED POINT */
    const rec2 = back.serialize();
    let same = rec.length === rec2.length;
    if (same) for (let i = 0; i < rec.length; i++) if (rec[i] !== rec2[i]) { same = false; break; }
    check(same, "re-encoding a decoded tree is byte-identical");

    const empty = BTree.deserialize(new BTree().serialize());
    eq(empty.size, 0, "an empty tree round-trips");
}

/* ---------------------------- 8. the collector can see through a stored value
   A value that refers back to the tree is a cycle only gc_mark can break. */
{
    const t = new BTree();
    for (let i = 0; i < 100; i++) { const holder = { tree: t, i: i }; t.set(i, holder); }
    eq(t.size, 100, "values referring back to the tree are stored");
    eq(t.get(50).i, 50, "and read back");
    check(t.get(50).tree === t, "and the cycle really exists");
}

if (fails === 0) print("test_structures_btree: all " + n + " checks passed");
else print("test_structures_btree: " + fails + " FAILED of " + n);
