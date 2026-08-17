/* test_array_sorted.js — Array.prototype sorting extensions: sortBy (the smart
 * default order) and sortedIndexOf (binary search). These absorbed the former
 * dyna:sort module, whose `sort` was already `toSorted`/`sortBy` and whose
 * `binarySearch` is `sortedIndexOf`.
 *
 * Core builtins — present in every build, no CONFIG_NATIVE_MODULES needed.
 * Run: dynajs tests/test_array_sorted.js */

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) {
    n++;
    const sa = JSON.stringify(a), sb = JSON.stringify(b);
    if (sa !== sb) throw new Error(`eq failed: ${msg} (${sa} vs ${sb})`);
}
/* NaN does not survive JSON, so compare with a marker */
const mark = (arr) => arr.map((v) => (typeof v === "number" && Number.isNaN(v)) ? "NaN" : v);

/* ===================== sortBy: the default order ===================== */
{
    /* numbers compare NUMERICALLY — the whole reason not to use bare .sort() */
    eq([10, 9, 1, 2, 100, 3].sortBy(), [1, 2, 3, 9, 10, 100], "numeric, not lexicographic");
    eq([10, 9, 1].sort(), [1, 10, 9], "bare .sort() really is lexicographic (contrast)");
    eq([3.5, -1, 0, 2].sortBy(), [-1, 0, 2, 3.5], "fractions and negatives");
    eq([-Infinity, 5, Infinity, 0].sortBy(), [-Infinity, 0, 5, Infinity], "infinities");
    eq([].sortBy(), [], "empty");
    eq([7].sortBy(), [7], "single element");
    eq([1, 1, 1].sortBy(), [1, 1, 1], "all equal");

    /* the input is NOT mutated */
    const orig = [3, 1, 2];
    orig.sortBy();
    eq(orig, [3, 1, 2], "sortBy does not mutate its receiver");

    /* strings compare as strings, and sort after numbers */
    eq(["banana", "apple", "cherry"].sortBy(), ["apple", "banana", "cherry"], "strings");
    eq([3, "apple", 1, "banana"].sortBy(), [1, 3, "apple", "banana"],
       "numbers before strings, and non-numbers are PRESERVED not coerced");

    /* descending */
    eq([1, 3, 2].sortBy(undefined, true), [3, 2, 1], "desc flag reverses");

    /* by key function / property name */
    const people = [{ age: 30, n: "c" }, { age: 10, n: "a" }, { age: 20, n: "b" }];
    eq(people.sortBy("age").map((p) => p.n), ["a", "b", "c"], "sortBy property name");
    eq(people.sortBy((p) => -p.age).map((p) => p.n), ["c", "b", "a"], "sortBy key function");

    /* stability: equal keys keep input order */
    const items = [{ k: 1, i: 0 }, { k: 0, i: 1 }, { k: 1, i: 2 }, { k: 0, i: 3 }];
    eq(items.sortBy("k").map((x) => x.i), [1, 3, 0, 2], "stable on ties");
}
{
    /* NaN. Every comparison with NaN is false, so a three-way numeric test reports
     * "equal" against everything — which is not an ordering, and left the whole
     * output unsorted rather than just misplacing the NaN. NaN sorts LAST. */
    eq(mark([NaN, 1, 2, NaN, 0].sortBy()), [0, 1, 2, "NaN", "NaN"],
       "a NaN does not scramble the rest of the array");
    eq(mark([NaN].sortBy()), ["NaN"], "lone NaN");
    eq(mark([NaN, NaN].sortBy()), ["NaN", "NaN"], "only NaNs");
    eq(mark([5, NaN, 1].sortBy()), [1, 5, "NaN"], "NaN after real numbers");
    /* and still before strings, since it is a numeric key */
    eq(mark([NaN, "a", 1].sortBy()), [1, "NaN", "a"], "NaN sorts with the numbers");
    /* the result really is ordered: every adjacent pair is non-decreasing */
    const big = [];
    let s = 7;
    const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    for (let i = 0; i < 500; i++) big.push(rnd() < 0.2 ? NaN : Math.floor(rnd() * 100));
    const sorted = big.sortBy();
    let seenNaN = false, ordered = true, prev = -Infinity;
    for (const v of sorted) {
        if (Number.isNaN(v)) { seenNaN = true; continue; }
        if (seenNaN) ordered = false;          /* a real number after a NaN */
        if (v < prev) ordered = false;
        prev = v;
    }
    assert(ordered, "500 elements with ~20% NaN come out fully ordered, NaNs last");
    assert(seenNaN, "the fixture did contain NaNs");
    assert(sorted.length === 500, "no elements lost");
}

/* ===================== sortedIndexOf ===================== */
{
    const a = [1, 3, 5, 7, 9, 11];
    assert(a.sortedIndexOf(1) === 0, "first element");
    assert(a.sortedIndexOf(7) === 3, "middle element");
    assert(a.sortedIndexOf(11) === 5, "last element");
    assert(a.sortedIndexOf(4) === -1, "absent, interior");
    assert(a.sortedIndexOf(0) === -1, "absent, below the range");
    assert(a.sortedIndexOf(99) === -1, "absent, above the range");
    assert([].sortedIndexOf(1) === -1, "empty array");
    assert([5].sortedIndexOf(5) === 0, "single element, hit");
    assert([5].sortedIndexOf(4) === -1, "single element, miss");

    /* strings, using the same order sortBy produces */
    const s = ["apple", "banana", "cherry"];
    assert(s.sortedIndexOf("banana") === 1, "string hit");
    assert(s.sortedIndexOf("date") === -1, "string miss");

    /* with a comparator, for an array sorted some other way */
    const desc = [9, 7, 5, 3, 1];
    const cmp = (el, target) => target - el;      /* reversed */
    assert(desc.sortedIndexOf(5, cmp) === 2, "descending array via comparator");
    assert(desc.sortedIndexOf(9, cmp) === 0, "descending, first");
    assert(desc.sortedIndexOf(1, cmp) === 4, "descending, last");
    assert(desc.sortedIndexOf(4, cmp) === -1, "descending, absent");

    /* duplicates: SOME matching index, and it must really match */
    const dup = [1, 2, 2, 2, 3];
    const i = dup.sortedIndexOf(2);
    assert(i >= 1 && i <= 3 && dup[i] === 2, "a duplicate returns a matching index");

    /* a comparator returning NaN counts as equal, like Array.prototype.sort */
    assert([1, 2, 3].sortedIndexOf(2, () => NaN) >= 0, "NaN comparator result is 'equal'");
}
{
    /* differential: sortBy then sortedIndexOf must find every element it holds
     * and reject every value it does not, over many random arrays */
    let s = 12345;
    const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    let cases = 0;
    for (let t = 0; t < 3000; t++) {
        const len = 1 + Math.floor(rnd() * 24);
        const arr = [];
        for (let k = 0; k < len; k++) arr.push(Math.floor(rnd() * 40));
        const srt = arr.sortBy();
        for (const v of arr) {
            cases++;
            const idx = srt.sortedIndexOf(v);
            assert(idx >= 0 && srt[idx] === v,
                   `found ${v} in ${JSON.stringify(srt)} (got ${idx})`);
        }
        for (const v of [-1, 40, 1000]) {
            cases++;
            assert(srt.sortedIndexOf(v) === -1, `absent ${v} rejected`);
        }
        /* linear indexOf agrees on membership */
        for (const v of [0, 7, 39]) {
            cases++;
            assert((srt.indexOf(v) >= 0) === (srt.sortedIndexOf(v) >= 0),
                   `membership agrees with indexOf for ${v}`);
        }
    }
    assert(cases > 40000, "ran a substantial differential sweep: " + cases);
}
{
    /* sortedIndexOf is a binary search: it TRUSTS the array to be sorted. An
     * unsorted array is the caller's error and may legitimately miss — pinned
     * here so the contract is not mistaken for a bug later. */
    const unsorted = [5, 1, 9, 3];
    const r = unsorted.sortedIndexOf(9);
    assert(r === -1 || unsorted[r] === 9,
           "on unsorted input it either misses or returns a true match, never a wrong index");
}

/* ===================== toSorted with a comparator (the other former mode) ===================== */
{
    /* dyna:sort.sort(arr, cmp) was exactly this */
    eq([5, 3, 9, 1].toSorted((a, b) => a - b), [1, 3, 5, 9], "ascending comparator");
    eq([5, 3, 9, 1].toSorted((a, b) => b - a), [9, 5, 3, 1], "descending comparator");
    const orig = [3, 1, 2];
    orig.toSorted((a, b) => a - b);
    eq(orig, [3, 1, 2], "toSorted does not mutate either");
}

print("test_array_sorted: all tests passed (" + n + " assertions)");
