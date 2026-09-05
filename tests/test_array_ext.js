/* test_array_ext.js — native Array extension methods (unprefixed, non-enumerable).
 * Phase 1, batch 1. Run: dynajs tests/test_array_ext.js
 * These are core-engine builtins (present in every build). */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

/* _first / _last */
eq([1, 2, 3, 4, 5].first(), 1, "first() → first element");
eq([1, 2, 3, 4, 5].first(2), [1, 2], "first(n) → first n");
eq([1, 2, 3].first(9), [1, 2, 3], "first(n>len) clamps");
eq([1, 2, 3].first(0), [], "first(0) → []");
eq([].first(), undefined, "first() of empty → undefined");
eq([1, 2, 3, 4, 5].last(), 5, "last() → last element");
eq([1, 2, 3, 4, 5].last(2), [4, 5], "last(n) → last n in order");
eq([1, 2, 3].last(9), [1, 2, 3], "last(n>len) clamps");
eq([].last(), undefined, "last() of empty → undefined");

/* _sum / _average / _mean */
eq([1, 2, 3, 4, 5].sum(), 15, "sum");
eq([].sum(), 0, "sum of empty → 0");
eq([1, 2, 3, 4].average(), 2.5, "average");
eq([1, 2, 3, 4].mean(), 2.5, "mean alias");
eq([].average(), 0, "average of empty → 0");
eq(["1", "2", "3"].sum(), 6, "sum coerces numeric strings");

/* _compact */
eq([1, null, 2, undefined, 3].compact(), [1, 2, 3], "compact removes null/undefined");
eq([0, false, "", NaN].compact(), [0, false, "", NaN], "compact keeps other falsy");
eq([].compact(), [], "compact of empty → []");

/* _isEmpty */
assert([].isEmpty() === true, "isEmpty of []");
assert([1].isEmpty() === false, "isEmpty of non-empty");

/* demarcation: `_` methods are NON-ENUMERABLE (for..in / Object.keys unaffected) */
assert(!Object.keys(Array.prototype).includes("sum"), "sum is non-enumerable");
let seen = []; for (const k in [1, 2, 3]) seen.push(k);
eq(seen, ["0", "1", "2"], "for..in over an array sees only indices, not _methods");
assert(Object.getOwnPropertyNames(Array.prototype).includes("sum"), "sum is an own property (present)");
/* works on array-likes via `this` */
eq(Array.prototype.first.call({ 0: "a", 1: "b", length: 2 }, 2), ["a", "b"], "first on an array-like");

/* a re-entrant valueOf arg must not corrupt the result */
eq([1, 2, 3].first({ valueOf() { return 2; } }), [1, 2], "first coerces an object arg");

/* ---- batch 2: _count / _none / _any / _all (matcher variants) ---- */
/* _count variants: no-arg → length; value (SameValueZero); predicate fn */
eq([1, 2, 2, 3, 2].count(), 5, "count() → length");
eq([1, 2, 2, 3, 2].count(2), 3, "count(value) counts equal");
eq([1, 2, 3, 4, 5].count(x => x % 2 === 0), 2, "count(fn) counts predicate");
eq([NaN, 1, NaN].count(NaN), 2, "count(NaN) uses SameValueZero");
eq([].count(x => true), 0, "count of empty → 0");
/* _none / _any / _all — value variant */
assert([1, 2, 3].none(4) === true, "none(value) true when absent");
assert([1, 2, 3].none(2) === false, "none(value) false when present");
assert([1, 2, 3].any(2) === true, "any(value) true when present");
assert([1, 2, 3].any(9) === false, "any(value) false when absent");
/* _none / _any / _all — predicate variant */
assert([2, 4, 6].all(x => x % 2 === 0) === true, "all(fn) true");
assert([2, 4, 5].all(x => x % 2 === 0) === false, "all(fn) false");
assert([1, 3, 5].none(x => x % 2 === 0) === true, "none(fn) true");
assert([1, 3, 4].any(x => x % 2 === 0) === true, "any(fn) true");
/* empty-array quantifiers (vacuous truth) */
assert([].all(x => false) === true, "all of empty → true (vacuous)");
assert([].any(x => true) === false, "any of empty → false");
assert([].none(x => true) === true, "none of empty → true");

/* ---- batch 2: _min / _max (mapper variants) ---- */
eq([3, 1, 4, 1, 5].min(), 1, "min() numeric");
eq([3, 1, 4, 1, 5].max(), 5, "max() numeric");
eq([].min(), undefined, "min() of empty → undefined");
eq([].max(), undefined, "max() of empty → undefined");
eq([{ a: 3 }, { a: 1 }, { a: 2 }].min("a"), { a: 1 }, "min(prop) returns the element");
eq([{ a: 3 }, { a: 1 }, { a: 2 }].max(p => p.a), { a: 3 }, "max(fn) returns the element");
eq([{ a: 1, id: "x" }, { a: 1, id: "y" }].min("a"), { a: 1, id: "x" }, "min ties → first");

/* ---- batch 2: _take / _drop / _takeLast / _dropLast ---- */
const b = [1, 2, 3, 4, 5];
eq(b.take(2), [1, 2], "take(n)");
eq(b.take(0), [], "take(0) → []");
eq(b.take(99), [1, 2, 3, 4, 5], "take(n>len) clamps");
eq(b.take(-1), [], "take(-1) → []");
eq(b.drop(2), [3, 4, 5], "drop(n)");
eq(b.drop(99), [], "drop(n>len) → []");
eq(b.takeLast(2), [4, 5], "takeLast(n)");
eq(b.takeLast(99), [1, 2, 3, 4, 5], "takeLast(n>len) clamps");
eq(b.dropLast(2), [1, 2, 3], "dropLast(n)");
eq(b.dropLast(99), [], "dropLast(n>len) → []");
eq([].take(3), [], "take of empty → []");
/* _take/_drop do not mutate the receiver */
b.drop(2); eq(b, [1, 2, 3, 4, 5], "drop does not mutate");

/* ---- batch 3: _sortBy (mapper + desc + stability variants) ---- */
eq([3, 1, 2, 10].sortBy(), [1, 2, 3, 10], "sortBy() numeric (not lexical)");
eq(["banana", "apple", "cherry"].sortBy(), ["apple", "banana", "cherry"], "sortBy() string");
eq([{ a: 3 }, { a: 1 }, { a: 2 }].sortBy("a").map(x => x.a), [1, 2, 3], "sortBy(prop)");
eq([{ a: 3 }, { a: 1 }].sortBy(x => x.a).map(x => x.a), [1, 3], "sortBy(fn)");
eq([1, 2, 3].sortBy(undefined, true), [3, 2, 1], "sortBy(_, desc)");
/* stability: equal keys keep original order, asc AND desc */
const st = [{ k: 1, id: "a" }, { k: 0, id: "b" }, { k: 1, id: "c" }, { k: 0, id: "d" }];
eq(st.sortBy("k").map(x => x.id), ["b", "d", "a", "c"], "sortBy stable asc");
eq(st.sortBy("k", true).map(x => x.id), ["a", "c", "b", "d"], "sortBy stable desc");
eq([].sortBy(), [], "sortBy of empty → []");
const so = [3, 1, 2]; so.sortBy(); eq(so, [3, 1, 2], "sortBy does not mutate");

/* ---- batch 3: _groupBy (mapper variants) ---- */
eq([1, 2, 3, 4, 5, 6].groupBy(x => x % 2 ? "odd" : "even"), { odd: [1, 3, 5], even: [2, 4, 6] }, "groupBy(fn)");
eq([{ t: "x", v: 1 }, { t: "y", v: 2 }, { t: "x", v: 3 }].groupBy("t"),
   { x: [{ t: "x", v: 1 }, { t: "x", v: 3 }], y: [{ t: "y", v: 2 }] }, "groupBy(prop) preserves order");
eq([].groupBy(x => x), {}, "groupBy of empty → {}");

/* ---- batch 3: _shuffle (invariants — randomised) ---- */
const src = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
const shuf = src.shuffle();
assert(shuf.length === src.length, "shuffle preserves length");
eq([...shuf].sort((a, b) => a - b), src, "shuffle preserves the multiset");
eq(src, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10], "shuffle does not mutate the original");
eq([].shuffle(), [], "shuffle of empty → []");
eq([7].shuffle(), [7], "shuffle of one → same");

/* ---- batch 3: _sample (single + n variants) ---- */
assert(src.includes(src.sample()), "sample() → an element of the array");
assert([].sample() === undefined, "sample() of empty → undefined");
const s3 = src.sample(3);
assert(s3.length === 3 && new Set(s3).size === 3, "sample(n) → n distinct elements");
assert(s3.every(x => src.includes(x)), "sample(n) elements come from the array");
assert(src.sample(99).length === src.length, "sample(n>len) → all");
eq(src.sample(0), [], "sample(0) → []");
eq(src, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10], "sample does not mutate the original");

/* ---- batch 4: _unique / _uniq / _uniqBy (SameValueZero + mapper variants) ---- */
eq([1, 2, 2, 3, 1, 4, 3].unique(), [1, 2, 3, 4], "unique dedup, first-occurrence order");
eq(["a", "b", "a", "c", "b"].unique(), ["a", "b", "c"], "unique strings by content");
eq([1, 1.0, 2].unique(), [1, 2], "unique: 1 and 1.0 are SameValueZero-equal");
{ const u = [0, -0, 1].unique(); assert(u.length === 2 && Object.is(u[0], 0), "unique: -0 and +0 dedup to +0"); }
{ const u = [NaN, NaN, 1].unique(); assert(u.length === 2 && Number.isNaN(u[0]), "unique: NaN dedups (SameValueZero)"); }
{ const a = {}, b = {}; eq([a, b, a].unique().length, 2, "unique objects by identity"); }
eq([1, 2, 3].uniq(), [1, 2, 3], "uniq alias");
eq([{ id: 1 }, { id: 2 }, { id: 1 }].uniqBy(x => x.id).map(o => o.id), [1, 2], "uniqBy(fn) keeps first");
eq([{ t: "a" }, { t: "b" }, { t: "a" }].unique("t").map(o => o.t), ["a", "b"], "unique(prop)");
eq([].unique(), [], "unique of empty → []");

/* ---- batch 4: _intersect / _intersection / _difference / _without / _union ---- */
eq([1, 2, 3, 4].intersect([2, 4, 6]), [2, 4], "intersect");
eq([1, 2, 3].intersection([2, 3, 9]), [2, 3], "intersection alias");
eq([2, 2, 3].intersect([2, 3]), [2, 3], "intersect dedups the result");
eq([1, 2, 3].intersect([]), [], "intersect with empty → []");
eq([1, 2, 3, 4].difference([2, 4]), [1, 3], "difference");
eq([1, 1, 2, 3].difference([3]), [1, 2], "difference dedups the result");
eq([1, 2, 3].difference([]), [1, 2, 3], "difference with empty → this");
eq([1, 2, 2, 3, 2].without([2]), [1, 3], "without removes all occurrences");
eq([1, 1, 2].without([2]), [1, 1], "without keeps this's own duplicates");
eq([1, 2, 3].union([3, 4, 5]), [1, 2, 3, 4, 5], "union dedup, this-then-other order");
eq([1, 1, 2].union([2, 3]), [1, 2, 3], "union dedups within and across");
eq([].union([1, 1, 2]), [1, 2], "union from empty");
/* set-ops preserve SameValueZero and don't mutate */
const setSrc = [1, 2, 3, 4]; setSrc.difference([2]); eq(setSrc, [1, 2, 3, 4], "difference does not mutate");

/* ---- batch 4: large-array hash-set correctness (O(n), not O(n²)) ---- */
{
    const big = [];
    for (let i = 0; i < 20000; i++) big.push(i % 5000);   /* each value appears 4x */
    const uq = big.unique();
    assert(uq.length === 5000, "unique large: 5000 distinct");
    eq(uq.slice(0, 5), [0, 1, 2, 3, 4], "unique large: order preserved");
    const other = []; for (let i = 0; i < 5000; i++) other.push(i * 2);
    assert(big.intersect(other).length === 2500, "intersect large correct");
}

/* ---- batch 5: _partition / _pluck ---- */
eq([1, 2, 3, 4, 5, 6].partition(x => x % 2 === 0), [[2, 4, 6], [1, 3, 5]], "partition(fn)");
eq([1, 2, 1, 3, 1].partition(1), [[1, 1, 1], [2, 3]], "partition(value)");
eq([].partition(x => true), [[], []], "partition of empty → [[],[]]");
eq([1, 2, 3].partition(x => true), [[1, 2, 3], []], "partition all-match");
eq([{ n: "a", v: 1 }, { n: "b", v: 2 }, { n: "c", v: 3 }].pluck("n"), ["a", "b", "c"], "pluck(key)");
eq([{ v: 1 }, { v: 2 }].pluck("missing"), [undefined, undefined], "pluck missing key → undefined");
eq([].pluck("x"), [], "pluck of empty → []");
eq([[10, 20], [30, 40]].pluck(1), [20, 40], "pluck numeric index");
const pSrc = [1, 2, 3]; pSrc.partition(x => x > 1); eq(pSrc, [1, 2, 3], "partition does not mutate");

/* ---- batch 6: _zip / _zipWith / _intersperse / _flatten / _transpose ---- */
eq([1, 2, 3].zip(["a", "b"]), [[1, "a"], [2, "b"]], "zip truncates to shorter");
eq([1, 2].zip([]), [], "zip with empty → []");
eq([1, 2, 3].zipWith((a, b) => a + b, [10, 20, 30]), [11, 22, 33], "zipWith(fn, other)");
eq([1, 2, 3].zipWith((a, b) => a * b, [10, 20]), [10, 40], "zipWith truncates");
eq([1, 2, 3].intersperse(0), [1, 0, 2, 0, 3], "intersperse");
eq([1].intersperse(0), [1], "intersperse single → no sep");
eq([].intersperse(0), [], "intersperse empty → []");
eq([1, [2, [3, [4]]]].flatten(), [1, 2, 3, 4], "flatten deep (default)");
eq([1, [2, [3]]].flatten(1), [1, 2, [3]], "flatten(1) one level");
eq([1, [2, [3]]].flatten(0), [1, [2, [3]]], "flatten(0) → unchanged copy");
eq([1, 2, 3].flatten(), [1, 2, 3], "flatten of a flat array");
eq([[1, 2, 3], [4, 5, 6]].transpose(), [[1, 4], [2, 5], [3, 6]], "transpose square");
eq([[1, 2], [3], [4, 5, 6]].transpose(), [[1, 3, 4], [2, 5], [6]], "transpose ragged (skips missing)");
eq([].transpose(), [], "transpose of empty → []");
/* non-mutation */
const zSrc = [1, 2, 3]; zSrc.flatten(); zSrc.intersperse(0); eq(zSrc, [1, 2, 3], "structural methods do not mutate");

/* ---- batch 7: _xprod / _aperture / _splitEvery / _splitAt / _adjust / _update / _move / _swap ---- */
eq([1, 2].xprod(["a", "b"]), [[1, "a"], [1, "b"], [2, "a"], [2, "b"]], "xprod cross product");
eq([1, 2, 3].xprod([]), [], "xprod with empty other → []");
eq([].xprod([1, 2]), [], "xprod from empty → []");
eq([1, 2, 3, 4, 5].aperture(2), [[1, 2], [2, 3], [3, 4], [4, 5]], "aperture(2)");
eq([1, 2, 3].aperture(3), [[1, 2, 3]], "aperture(len) → one window");
eq([1, 2, 3].aperture(4), [], "aperture(n>len) → []");
eq([1, 2, 3].aperture(0), [[], [], [], []], "aperture(0) → len+1 empties");
eq([1, 2, 3, 4, 5, 6, 7].splitEvery(3), [[1, 2, 3], [4, 5, 6], [7]], "splitEvery(3)");
eq([1, 2, 3, 4].splitEvery(2), [[1, 2], [3, 4]], "splitEvery even");
eq([].splitEvery(3), [], "splitEvery of empty → []");
let threw = false; try { [1, 2, 3].splitEvery(0); } catch (e) { threw = e instanceof RangeError; }
assert(threw, "splitEvery(0) throws RangeError");
eq([1, 2, 3].splitAt(1), [[1], [2, 3]], "splitAt(1)");
eq([1, 2, 3].splitAt(0), [[], [1, 2, 3]], "splitAt(0)");
eq([1, 2, 3].splitAt(-1), [[1, 2], [3]], "splitAt(-1) from end");
eq([1, 2, 3].splitAt(9), [[1, 2, 3], []], "splitAt(n>len)");
eq([1, 2, 3].adjust(1, x => x * 10), [1, 20, 3], "adjust(idx, fn)");
eq([1, 2, 3].adjust(-1, x => x * 10), [1, 2, 30], "adjust negative idx");
eq([1, 2, 3].adjust(9, x => x * 10), [1, 2, 3], "adjust OOB → unchanged copy");
eq([1, 2, 3].update(1, 99), [1, 99, 3], "update(idx, val)");
eq([1, 2, 3].update(-1, 99), [1, 2, 99], "update negative idx");
eq([1, 2, 3].update(9, 99), [1, 2, 3], "update OOB → unchanged copy");
eq([1, 2, 3, 4].move(0, 2), [2, 3, 1, 4], "move(from, to)");
eq([1, 2, 3, 4].move(-1, 0), [4, 1, 2, 3], "move negative indices");
eq([1, 2, 3, 4].move(2, 2), [1, 2, 3, 4], "move(i, i) → unchanged");
eq([1, 2, 3].move(9, 0), [1, 2, 3], "move OOB → unchanged copy");
eq([1, 2, 3, 4].swap(0, 3), [4, 2, 3, 1], "swap(0, 3)");
eq([1, 2, 3, 4].swap(-1, 0), [4, 2, 3, 1], "swap negative idx");
eq([1, 2, 3].swap(1, 1), [1, 2, 3], "swap(i, i) → unchanged");
eq([1, 2, 3].swap(9, 0), [1, 2, 3], "swap OOB → unchanged copy");
/* non-mutation of the receiver */
const b7 = [1, 2, 3, 4];
b7.xprod([9]); b7.aperture(2); b7.splitEvery(2); b7.splitAt(2);
b7.adjust(0, x => x * 100); b7.update(0, 100); b7.move(0, 3); b7.swap(0, 3);
eq(b7, [1, 2, 3, 4], "batch-7 methods do not mutate the receiver");
/* re-entrancy: a {valueOf} index arg must not corrupt the result */
{
    const arr = [10, 20, 30];
    let hits = 0;
    const eviltoInt = { valueOf() { hits++; return 1; } };
    eq(arr.adjust(eviltoInt, x => x + 1), [10, 21, 30], "adjust with valueOf idx");
    eq(arr.update(eviltoInt, 99), [10, 99, 30], "update with valueOf idx");
    eq(arr.splitAt(eviltoInt), [[10], [20, 30]], "splitAt with valueOf idx");
    assert(hits === 3, "valueOf coerced exactly once per call");
    eq(arr, [10, 20, 30], "receiver intact after valueOf-arg calls");
}
/* works on array-likes via `this` */
eq(Array.prototype.splitAt.call({ 0: "a", 1: "b", 2: "c", length: 3 }, 2), [["a", "b"], ["c"]], "splitAt on array-like");

/* ---- batch 8: _nth / _init / _tail / _head / while-variants / _append / _prepend ---- */
eq([10, 20, 30].nth(1), 20, "nth(1)");
eq([10, 20, 30].nth(-1), 30, "nth(-1) from end");
eq([10, 20, 30].nth(9), undefined, "nth OOB → undefined");
eq([10, 20, 30].nth(-9), undefined, "nth negative OOB → undefined");
eq([1, 2, 3, 4].init(), [1, 2, 3], "init drops last");
eq([1].init(), [], "init of singleton → []");
eq([].init(), [], "init of empty → []");
eq([1, 2, 3, 4].tail(), [2, 3, 4], "tail drops first");
eq([1].tail(), [], "tail of singleton → []");
eq([].tail(), [], "tail of empty → []");
eq([1, 2, 3].head(), 1, "head alias of _first");
eq([1, 2, 3, 4, 1].takeWhile(x => x < 3), [1, 2], "takeWhile(pred)");
eq([1, 2, 3].takeWhile(x => x > 9), [], "takeWhile none");
eq([1, 2, 3].takeWhile(x => true), [1, 2, 3], "takeWhile all");
eq([1, 1, 2, 1].takeWhile(1), [1, 1], "takeWhile(value) via SameValueZero");
eq([1, 2, 3, 4, 1].dropWhile(x => x < 3), [3, 4, 1], "dropWhile(pred)");
eq([1, 2, 3].dropWhile(x => true), [], "dropWhile all");
eq([1, 2, 3, 4].takeLastWhile(x => x > 2), [3, 4], "takeLastWhile(pred)");
eq([1, 2, 3, 4].takeLastWhile(x => x > 9), [], "takeLastWhile none");
eq([1, 2, 3, 4].dropLastWhile(x => x > 2), [1, 2], "dropLastWhile(pred)");
eq([1, 2, 3, 4].dropLastWhile(x => x > 9), [1, 2, 3, 4], "dropLastWhile none → all");
eq([1, 2, 3].append(4), [1, 2, 3, 4], "append");
eq([].append(1), [1], "append to empty");
eq([1, 2, 3].prepend(0), [0, 1, 2, 3], "prepend");
eq([].prepend(1), [1], "prepend to empty");
eq([[1]].append([2]), [[1], [2]], "append keeps element as-is (no spread)");
eq([[1]].prepend([2]), [[2], [1]], "prepend keeps element as-is (no spread)");
/* non-mutation */
const b8 = [1, 2, 3];
b8.init(); b8.tail(); b8.takeWhile(x => true); b8.dropWhile(x => false);
b8.takeLastWhile(x => true); b8.dropLastWhile(x => false); b8.append(9); b8.prepend(0);
eq(b8, [1, 2, 3], "batch-8 methods do not mutate the receiver");
/* re-entrancy: a {valueOf} index arg to _nth must not corrupt the receiver */
{
    const arr = [10, 20, 30];
    const ev = { valueOf() { return -1; } };
    eq(arr.nth(ev), 30, "nth with valueOf idx");
    eq(arr, [10, 20, 30], "receiver intact after _nth valueOf");
}
/* array-like via `this` */
eq(Array.prototype.tail.call({ 0: "a", 1: "b", 2: "c", length: 3 }), ["b", "c"], "tail on array-like");

/* ---- batch 9: _reject / _insert / _insertAll / _removeAt / _zipObj / _fromPairs ---- */
eq([1, 2, 3, 4].reject(x => x % 2 === 0), [1, 3], "reject(pred) complement of filter");
eq([1, 2, 1, 3, 1].reject(1), [2, 3], "reject(value) via SameValueZero");
eq([1, 2, 3].reject(x => false), [1, 2, 3], "reject none → all");
eq([1, 2, 3].reject(x => true), [], "reject all → []");
eq([1, 2, 3, 4].insert(2, "x"), [1, 2, "x", 3, 4], "insert(idx, elt)");
eq([1, 2, 3].insert(0, "x"), ["x", 1, 2, 3], "insert at front");
eq([1, 2, 3].insert(3, "x"), [1, 2, 3, "x"], "insert at len → append");
eq([1, 2, 3].insert(99, "x"), [1, 2, 3, "x"], "insert idx>len → append");
eq([1, 2, 3].insert(-1, "x"), [1, 2, 3, "x"], "insert idx<0 → append, NOT from the end");
eq([1, 2, 3, 4].insertAll(2, ["x", "y"]), [1, 2, "x", "y", 3, 4], "insertAll(idx, elts)");
eq([1, 2, 3].insertAll(0, ["a"]), ["a", 1, 2, 3], "insertAll at front");
eq([1, 2, 3].insertAll(9, ["a", "b"]), [1, 2, 3, "a", "b"], "insertAll idx>len → append");
eq([1, 2, 3].insertAll(1, []), [1, 2, 3], "insertAll empty elts → copy");
eq([1, 2, 3, 4].removeAt(1), [1, 3, 4], "removeAt(idx)");
eq([1, 2, 3, 4].removeAt(-1), [1, 2, 3], "removeAt(-1) from end");
eq([1, 2, 3].removeAt(9), [1, 2, 3], "removeAt OOB → unchanged copy");
eq([5].removeAt(0), [], "removeAt sole element → []");
eq(["a", "b", "c"].zipObj([1, 2, 3]), { a: 1, b: 2, c: 3 }, "zipObj(values)");
eq(["a", "b", "c"].zipObj([1, 2]), { a: 1, b: 2 }, "zipObj truncates to shorter");
eq([].zipObj([1, 2]), {}, "zipObj no keys → {}");
eq([["a", 1], ["b", 2]].fromPairs(), { a: 1, b: 2 }, "fromPairs");
eq([["a", 1], ["a", 2]].fromPairs(), { a: 2 }, "fromPairs later key wins");
eq([].fromPairs(), {}, "fromPairs of empty → {}");
/* non-mutation */
const b9 = [1, 2, 3];
b9.reject(x => true); b9.insert(1, 9); b9.insertAll(1, [9]); b9.removeAt(0); b9.zipObj([7, 8, 9]);
eq(b9, [1, 2, 3], "batch-9 methods do not mutate the receiver");
/* re-entrancy: a {valueOf} idx to _insert/_removeAt must not corrupt the receiver */
{
    const arr = [10, 20, 30];
    eq(arr.insert({ valueOf() { return 1; } }, 99), [10, 99, 20, 30], "insert with valueOf idx");
    eq(arr.removeAt({ valueOf() { return 0; } }), [20, 30], "removeAt with valueOf idx");
    eq(arr, [10, 20, 30], "receiver intact after valueOf-arg calls");
}

/* ---- batch 10: _median / _product / _scan / _countBy / _indexBy ---- */
eq([1, 2, 3].median(), 2, "median odd length");
eq([1, 2, 3, 4].median(), 2.5, "median even length → avg of two middle");
eq([7, 2, 10, 9].median(), 8, "median unsorted even → sorts a copy first");
eq([2, 9, 7].median(), 7, "median unsorted odd");
assert(Number.isNaN([].median()), "median of empty → NaN");
eq([5].median(), 5, "median single");
eq([2, 4, 6].product(), 48, "product");
eq([].product(), 1, "product of empty → 1");
eq([3].product(), 3, "product single");
eq(["2", "3"].product(), 6, "product coerces");
eq([1, 2, 3, 4].scan((a, b) => a + b, 0), [0, 1, 3, 6, 10], "scan sum");
eq([].scan((a, b) => a + b, 0), [0], "scan of empty → [acc]");
eq([1, 2, 3].scan((a, b) => a * b, 1), [1, 1, 2, 6], "scan product");
eq([1.1, 1.2, 2.3].countBy(Math.floor), { 1: 2, 2: 1 }, "countBy(fn)");
eq(["a", "b", "a", "a"].countBy(x => x), { a: 3, b: 1 }, "countBy identity-ish");
eq([].countBy(x => x), {}, "countBy of empty → {}");
eq([{ id: "a", v: 1 }, { id: "b", v: 2 }, { id: "a", v: 3 }].indexBy(x => x.id),
   { a: { id: "a", v: 3 }, b: { id: "b", v: 2 } }, "indexBy last wins");
eq([{ id: "x" }].indexBy("id"), { x: { id: "x" } }, "indexBy by property key");
eq([].indexBy(x => x), {}, "indexBy of empty → {}");
/* non-mutation */
const b10 = [3, 1, 2];
b10.median(); b10.product(); b10.scan((a, b) => a + b, 0); b10.countBy(x => x); b10.indexBy(x => x);
eq(b10, [3, 1, 2], "batch-10 methods do not mutate the receiver");
/* _median must not be corrupted by a re-entrant valueOf element */
{
    let hits = 0;
    const arr = [3, 1, { valueOf() { hits++; return 2; } }];
    eq(arr.median(), 2, "median with a valueOf element");
    assert(hits === 1, "median coerces the element once");
}

/* ---- overloaded matcher dispatch: RegExp matcher across all matcher methods ---- */
eq(["apple", "banana", "cherry", "avocado"].count(/^a/), 2, "count(regex) tests each element");
eq(["apple", "banana", "cherry"].reject(/^a/), ["banana", "cherry"], "reject(regex)");
eq([1, 22, 333, 4444].partition(/../), [[22, 333, 4444], [1]], "partition(regex) coerces to string");
assert(["ab", "cd"].any(/c/) === true, "any(regex)");
assert(["ab", "cd"].all(/[a-d]/) === true, "all(regex)");
assert(["ab", "cd"].none(/z/) === true, "none(regex)");
eq(["a1", "a2", "b3"].takeWhile(/^a/), ["a1", "a2"], "takeWhile(regex)");
eq(["a1", "a2", "b3"].dropWhile(/^a/), ["b3"], "dropWhile(regex)");
/* a function matcher and a value matcher still dispatch correctly (no regression) */
eq([1, 2, 3, 4].count(x => x > 2), 2, "matcher still accepts a predicate fn");
eq([1, 2, 2, 3].count(2), 2, "matcher still accepts a value (SameValueZero)");

/* ---- batch 11: remove/exclude (=reject), removeRange, splitWhen, innerJoin ---- */
eq([1, 2, 3, 4].remove(x => x % 2 === 0), [1, 3], "remove(pred) == reject");
eq([1, 2, 1, 3].remove(1), [2, 3], "remove(value)");
eq([1, 2, 3, 4].exclude(x => x > 2), [1, 2], "exclude(pred) == reject");
eq(["a1", "b2", "a3"].remove(/^a/), ["b2"], "remove(regex)");
eq([1, 2, 3, 4, 5, 6, 7, 8].removeRange(2, 3), [1, 2, 6, 7, 8], "removeRange(start, count)");
eq([1, 2, 3, 4].removeRange(0, 2), [3, 4], "removeRange from front");
eq([1, 2, 3, 4].removeRange(-2, 2), [1, 2], "removeRange negative start");
eq([1, 2, 3].removeRange(1, 99), [1], "removeRange count>len clamps");
eq([1, 2, 3].removeRange(1, 0), [1, 2, 3], "removeRange count 0 → unchanged copy");
eq([1, 2, 3, 4, 1, 2].splitWhen(x => x > 2), [[1, 2], [3, 4, 1, 2]], "splitWhen(pred) at first match");
eq([1, 2, 3].splitWhen(x => x > 9), [[1, 2, 3], []], "splitWhen no match → [all, []]");
eq([1, 2, 3].splitWhen(x => true), [[], [1, 2, 3]], "splitWhen first matches → [[], all]");
eq([1, 2, 3, 4].splitWhen(3), [[1, 2], [3, 4]], "splitWhen(value)");
{
    const records = [{ id: 1, n: "a" }, { id: 2, n: "b" }, { id: 3, n: "c" }];
    eq(records.innerJoin((r, id) => r.id === id, [1, 3]).map(r => r.n), ["a", "c"], "innerJoin keeps matches");
    eq(records.innerJoin((r, id) => r.id === id, []).length, 0, "innerJoin with empty other → []");
    eq([1, 2, 3].innerJoin((a, b) => a === b, [2, 2, 3]), [2, 3], "innerJoin dedups this? no—keeps each this once");
}
/* non-mutation */
const b11 = [1, 2, 3, 4];
b11.remove(x => true); b11.removeRange(0, 2); b11.splitWhen(x => true); b11.innerJoin((a, b) => true, [1]);
eq(b11, [1, 2, 3, 4], "batch-11 methods do not mutate the receiver");

print("test_array_ext: all tests passed (" + n + " assertions)");
