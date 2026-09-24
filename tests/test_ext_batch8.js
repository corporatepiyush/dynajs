/* test_ext_batch8.js — native extension-method batch 8:
 *   Array batch A  : startsWith/endsWith, unnest, dropRepeats(+With/By), sortWith,
 *                    unionWith, differenceWith, symmetricDifference(+With),
 *                    reduceBy, static Array.repeat.
 *   Array transducers: transduce, into, sequence, traverse (@@transducer protocol).
 *   Array FromIndex : map/forEach/filter/find/findIndex/some/every/reduce/
 *                     reduceRight FromIndex.
 *   Function statics: identity, of, not, negate, applyTo, always, cond,
 *                     uncurryN, lift/liftN, ap.
 * Core-engine builtins (present in every build). Run: dynajs tests/test_ext_batch8.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function threw(fn, Ctor, m) { try { fn(); } catch (e) { assert(!Ctor || e instanceof Ctor, m + " wrong error: " + e); return; } assert(false, m + " did not throw"); }

/* ===== Array batch A ===== */
eq([1, 2, 3].startsWith([1, 2]), true, "startsWith");
eq([1, 2, 3].startsWith([2]), false, "startsWith no");
eq([1, 2, 3].startsWith([1, 2, 3, 4]), false, "startsWith longer");
eq([1, 2, 3].startsWith([]), true, "startsWith empty");
eq([[1], [2]].startsWith([[1]]), true, "startsWith deep");
eq([1, 2, 3].endsWith([2, 3]), true, "endsWith");
eq([1, 2, 3].endsWith([1]), false, "endsWith no");
eq([1, 2, 3].endsWith([]), true, "endsWith empty");

eq([1, [2, 3], [4, [5]]].unnest(), [1, 2, 3, 4, [5]], "unnest one level");
eq([1, 2, 3].unnest(), [1, 2, 3], "unnest flat");

eq([1, 1, 2, 3, 3, 3, 1].dropRepeats(), [1, 2, 3, 1], "dropRepeats");
eq([[1], [1], [2]].dropRepeats(), [[1], [2]], "dropRepeats deep");
eq([1, 2, 2, 3].dropRepeatsWith((a, b) => a === b), [1, 2, 3], "dropRepeatsWith");
eq([{ a: 1 }, { a: 1 }, { a: 2 }].dropRepeatsBy(x => x.a), [{ a: 1 }, { a: 2 }], "dropRepeatsBy");
eq([].dropRepeats(), [], "dropRepeats empty");

eq([{ n: "z", a: 2 }, { n: "a", a: 2 }, { n: "m", a: 1 }]
    .sortWith([(x, y) => x.a - y.a, (x, y) => x.n < y.n ? -1 : 1]).map(x => x.n),
   ["m", "a", "z"], "sortWith");
eq([3, 1, 2].sortWith([]), [3, 1, 2], "sortWith no comparators (stable copy)");
{ const src = [2, 1]; src.sortWith([(a, b) => a - b]); eq(src, [2, 1], "sortWith non-mutating"); }

eq([1, 2, 3].unionWith((a, b) => a === b, [2, 3, 4]), [1, 2, 3, 4], "unionWith");
eq([1, 1, 2].unionWith((a, b) => a === b, [2, 3]), [1, 2, 3], "unionWith dedups");
eq([1, 2, 3, 4].differenceWith((a, b) => a === b, [2, 4]), [1, 3], "differenceWith");
eq([1, 1, 3].differenceWith((a, b) => a === b, [2]), [1, 3], "differenceWith dedups");

eq([1, 2, 3].symmetricDifference([2, 3, 4]), [1, 4], "symmetricDifference");
eq([1, 2, 3, 3].symmetricDifference([3, 4, 4]), [1, 2, 4], "symmetricDifference dedup");
eq([1, 2].symmetricDifference([1, 2]), [], "symmetricDifference equal");
eq([1, 2, 3].symmetricDifferenceWith((a, b) => a === b, [2, 3, 4]), [1, 4], "symmetricDifferenceWith");

eq([{ g: "a", v: 1 }, { g: "b", v: 2 }, { g: "a", v: 3 }]
    .reduceBy((acc, x) => acc + x.v, 0, x => x.g), { a: 4, b: 2 }, "reduceBy sum");
{
  const tmpl = [];
  const r = [1, 2, 3, 4].reduceBy((acc, x) => { acc.push(x); return acc; }, tmpl, x => x % 2 ? "odd" : "even");
  eq(r, { odd: [1, 3], even: [2, 4] }, "reduceBy groups");
  assert(r.odd !== r.even, "reduceBy: per-key accumulators do not alias");
  eq(tmpl, [], "reduceBy: template accumulator not mutated");
}
{ /* @@transducer/reduced short-circuit */
  const r = [1, 2, 3, 4, 5].reduceBy(
    (acc, x) => acc >= 6 ? { "@@transducer/reduced": true, "@@transducer/value": acc } : acc + x,
    0, () => "k");
  eq(r, { k: 6 }, "reduceBy honours @@transducer/reduced");
}

eq(Array.repeat("x", 3), ["x", "x", "x"], "Array.repeat");
eq(Array.repeat(0, 0), [], "Array.repeat 0");
{ const o = {}; const r = Array.repeat(o, 2); assert(r[0] === o && r[1] === o, "Array.repeat same reference"); }
threw(() => Array.repeat(1, -1), RangeError, "Array.repeat negative");
threw(() => Array.repeat(1, 1e9), RangeError, "Array.repeat too large");

/* adversarial: exceptions propagate */
threw(() => [1, 2].sortWith([() => { throw new Error("x"); }]), Error, "sortWith throw propagates");
threw(() => [1, 2].dropRepeatsWith(() => { throw new Error("x"); }), Error, "dropRepeatsWith throw");
threw(() => [1, 2].reduceBy(() => { throw new Error("x"); }, 0, x => x), Error, "reduceBy valueFn throw");
threw(() => [1, 2].differenceWith(() => { throw new Error("x"); }, [1]), Error, "differenceWith throw");

/* reentrancy: valueOf on Array.repeat count / startsWith arg length */
eq(Array.repeat("a", { valueOf() { return 2; } }), ["a", "a"], "repeat reentrant count");

/* ===== Transducers ===== */
const mapT = f => xf => ({
  "@@transducer/init": () => xf["@@transducer/init"](),
  "@@transducer/result": a => xf["@@transducer/result"](a),
  "@@transducer/step": (acc, x) => xf["@@transducer/step"](acc, f(x)),
});
const filterT = p => xf => ({
  "@@transducer/init": () => xf["@@transducer/init"](),
  "@@transducer/result": a => xf["@@transducer/result"](a),
  "@@transducer/step": (acc, x) => p(x) ? xf["@@transducer/step"](acc, x) : acc,
});
const takeT = n => xf => { let i = 0; return {
  "@@transducer/init": () => xf["@@transducer/init"](),
  "@@transducer/result": a => xf["@@transducer/result"](a),
  "@@transducer/step": (acc, x) => { if (i < n) { i++; acc = xf["@@transducer/step"](acc, x); if (i >= n) return { "@@transducer/reduced": true, "@@transducer/value": acc }; } return acc; },
}; };
const compose2 = (f, g) => x => f(g(x));

eq([1, 2, 3, 4].transduce(mapT(x => x * 2), (acc, x) => { acc.push(x); return acc; }, []), [2, 4, 6, 8], "transduce map + reducer fn");
eq([1, 2, 3, 4, 5, 6].into([], filterT(x => x % 2 === 0)), [2, 4, 6], "into filter");
eq([1, 2, 3, 4, 5, 6].into([], compose2(filterT(x => x % 2 === 0), mapT(x => x * 10))), [20, 40, 60], "into composed");
eq([1, 2, 3, 4, 5].into([], takeT(2)), [1, 2], "into reduced early-exit");
eq([1, 2, 3, 4, 5].transduce(takeT(3), (a, x) => { a.push(x); return a; }, []), [1, 2, 3], "transduce reduced");
eq(["a", "b", "c"].into("", mapT(x => x.toUpperCase())), "ABC", "into string");
eq([["a", 1], ["b", 2]].into({}, mapT(p => p)), { a: 1, b: 2 }, "into object");
threw(() => [1, 2].transduce(mapT(() => { throw new Error("x"); }), (a) => a, []), Error, "transduce throw propagates");

/* ===== sequence / traverse (Array applicative) ===== */
eq([[1, 2], [3, 4]].sequence(Array), [[1, 3], [1, 4], [2, 3], [2, 4]], "sequence cartesian");
eq([[1], [2], [3]].sequence(Array), [[1, 2, 3]], "sequence singletons");
eq([].sequence(Array), [[]], "sequence empty");
eq([[1, 2], []].sequence(Array), [], "sequence with empty applicative");
eq([1, 2].traverse(Array, x => [x, x * 10]), [[1, 2], [1, 20], [10, 2], [10, 20]], "traverse");
eq([1, 2, 3].traverse(Array, x => [x]), [[1, 2, 3]], "traverse singletons");

/* ===== sequence / traverse over a NON-Array applicative =====
 *
 * The Array path above is the list applicative written out as two nested
 * loops. Any other type goes through its own map and ap, and both calling
 * conventions are accepted because a value can only carry one: the
 * fantasy-land spelling puts ap on the VALUE, the plain-method spelling puts
 * it on the FUNCTIONS.
 *
 * The short-circuiting case is the one worth having: a Nothing anywhere in the
 * list must make the whole sequence Nothing, which is what distinguishes a
 * real applicative traversal from a map. */
{
    class Just {
        constructor(v) { this.v = v; }
        ["fantasy-land/map"](f) { return new Just(f(this.v)); }
        ["fantasy-land/ap"](fs) { return fs instanceof Nothing ? fs : new Just(fs.v(this.v)); }
    }
    class Nothing {
        ["fantasy-land/map"]() { return this; }
        ["fantasy-land/ap"]() { return this; }
    }
    Just["fantasy-land/of"] = v => new Just(v);

    const j = [new Just(1), new Just(2), new Just(3)].sequence(Just);
    assert(j instanceof Just, "fantasy-land sequence returns the type");
    eq(j.v, [1, 2, 3], "and the values in order");
    assert([new Just(1), new Nothing(), new Just(3)].sequence(Just) instanceof Nothing,
       "one Nothing makes the whole traversal Nothing");
    eq([].sequence(Just).v, [], "an empty traversal is of([])");
    eq([1, 2, 3].traverse(Just, x => new Just(x * 2)).v, [2, 4, 6], "traverse");
    assert([1, 2, 3].traverse(Just, x => (x > 2 ? new Nothing() : new Just(x))) instanceof Nothing,
       "traverse short-circuits too");

    /* The plain-method convention: of/map on the value, ap on the FUNCTIONS. */
    class Box {
        constructor(v) { this.v = v; }
        map(f) { return new Box(f(this.v)); }
        ap(x) { return new Box(this.v(x.v)); }
    }
    Box.of = v => new Box(v);
    eq([new Box(1), new Box(2)].sequence(Box).v, [1, 2], "the ap-on-functions convention");

    /* A type with map but no ap is neither applicative, and saying so beats
     * falling through to the list fold and answering []. */
    class Broken { map() { return this; } }
    Broken.of = () => new Broken();
    let threw = false;
    try { [new Broken()].sequence(Broken); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "a type with no ap is refused");
}

/* ===== FromIndex (reference outputs) ===== */
const A = ["a", "b", "c", "d", "e"];
eq(A.mapFromIndex(2, x => x), ["c", "d", "e"], "mapFromIndex");
eq(A.mapFromIndex(2, true, x => x), ["c", "d", "e", "a", "b"], "mapFromIndex loop");
eq([{ name: "x" }, { name: "y" }, { name: "z" }].mapFromIndex(1, "name"), ["y", "z"], "mapFromIndex property");
{ let o = []; A.forEachFromIndex(2, true, (el, i) => o.push([el, i])); eq(o, [["c", 2], ["d", 3], ["e", 4], ["a", 0], ["b", 1]], "forEachFromIndex loop"); }
eq(A.filterFromIndex(1, /[a-c]/), ["b", "c"], "filterFromIndex regex");
eq(A.filterFromIndex(1, true, /[a-c]/), ["b", "c", "a"], "filterFromIndex loop");
eq(A.findFromIndex(2, /a/), undefined, "findFromIndex none");
eq(A.findFromIndex(2, true, /a/), "a", "findFromIndex loop");
eq(A.findIndexFromIndex(2, "c"), 2, "findIndexFromIndex");
eq(A.findIndexFromIndex(2, true, "a"), 0, "findIndexFromIndex loop remap");
eq(A.findIndexFromIndex(2, "a"), -1, "findIndexFromIndex miss");
eq(A.someFromIndex(2, x => x === "a"), false, "someFromIndex");
eq(A.someFromIndex(2, true, x => x === "a"), true, "someFromIndex loop");
eq(A.everyFromIndex(2, x => x >= "c"), true, "everyFromIndex");
eq(A.everyFromIndex(0, x => x >= "c"), false, "everyFromIndex false");
eq([1, 2, 3, 4].reduceFromIndex(1, (a, v) => a + v, 0), 9, "reduceFromIndex falsy seed dropped");
eq(A.reduceFromIndex(2, (a, v) => a + v, "INIT:"), "INIT:cde", "reduceFromIndex");
eq(A.reduceFromIndex(2, true, (a, v) => a + v, "INIT:"), "INIT:cdeab", "reduceFromIndex loop");
eq(A.reduceRightFromIndex(2, (a, v) => a + v, "INIT:"), "INIT:cba", "reduceRightFromIndex");
eq(A.reduceRightFromIndex(2, true, (a, v) => a + v, "INIT:"), "INIT:edcba", "reduceRightFromIndex loop");
eq(A.reduceRightFromIndex(2, (a, v) => a + v), "cba", "reduceRightFromIndex no init");
{ let idx = []; A.reduceRightFromIndex(2, (a, v, i) => { idx.push(i); return a + v; }, "INIT:"); eq(idx, [4, 3, 2], "reduceRightFromIndex shifted-index quirk"); }
{ let idx = []; A.reduceRightFromIndex(2, true, (a, v, i) => { idx.push(i); return a + v; }, "INIT:"); eq(idx, [1, 0, 4, 3, 2], "reduceRightFromIndex loop index"); }
threw(() => A.mapFromIndex(2), TypeError, "FromIndex requires callback arg");
threw(() => A.reduceFromIndex(0), TypeError, "reduceFromIndex requires callback");
threw(() => A.mapFromIndex(0, x => { throw new Error("x"); }), Error, "mapFromIndex throw propagates");
eq([].mapFromIndex(0, x => x), [], "FromIndex empty map");
eq([].reduceFromIndex(0, (a, v) => a + v, 5), 5, "FromIndex empty reduce seeded");
/* negative startIndex relative to end */
eq(A.mapFromIndex(-2, x => x), ["d", "e"], "mapFromIndex negative index");
/* reentrancy: startIndex via valueOf */
eq(A.mapFromIndex({ valueOf() { return 3; } }, x => x), ["d", "e"], "mapFromIndex reentrant startIndex");

/* ===== Function statics ===== */
eq(Function.identity(5), 5, "identity");
eq(Function.of(7), [7], "of");
eq(Function.not(0), true, "not falsy");
eq(Function.not(1), false, "not truthy");
eq(Function.negate(3), -3, "negate");
eq(Function.applyTo(5, x => x * 2), 10, "applyTo");
{ const k = Function.always(42); eq(k(), 42, "always"); eq(k(1, 2, 3), 42, "always ignores args"); }
{ const c = Function.cond([[x => x > 0, () => "pos"], [x => x < 0, () => "neg"], [() => true, () => "zero"]]);
  eq(c(5), "pos", "cond pos"); eq(c(-5), "neg", "cond neg"); eq(c(0), "zero", "cond default");
  assert(Function.cond([[() => false, () => 1]])(9) === undefined, "cond no match -> undefined"); }
{ const add3 = a => b => c => a + b + c; eq(Function.uncurryN(3, add3)(1, 2, 3), 6, "uncurryN"); }
{ const g = (a, b) => c => a + b + c; eq(Function.uncurryN(2, g)(1, 2, 10), 13, "uncurryN mixed arity"); }
eq(Function.lift((a, b) => a * b)([1, 2], [3, 4]), [3, 4, 6, 8], "lift");
eq(Function.liftN(2, (a, b) => a + b)([1, 2], [10, 20]), [11, 21, 12, 22], "liftN");
eq(Function.lift((a, b) => a + b)([1, 2], []), [], "lift empty list -> empty");
eq(Function.ap([x => x + 1, x => x * 2], [10, 20]), [11, 21, 20, 40], "ap list form");
eq(Function.ap([], [1, 2]), [], "ap empty fns");
{ const s = Function.ap(a => b => a + b, x => x * 10); eq(s(5), 55, "ap S-combinator"); }
threw(() => Function.cond([[() => { throw new Error("x"); }, () => 1]])(0), Error, "cond throw propagates");

/* ===== non-enumerability / no-collision spot checks ===== */
assert(!Array.prototype.propertyIsEnumerable("startsWith"), "startsWith non-enumerable");
assert(!Array.prototype.propertyIsEnumerable("transduce"), "transduce non-enumerable");
assert(!Array.prototype.propertyIsEnumerable("mapFromIndex"), "mapFromIndex non-enumerable");
assert(!Array.propertyIsEnumerable("repeat"), "Array.repeat non-enumerable");
assert(!Function.propertyIsEnumerable("identity"), "Function.identity non-enumerable");
assert(Object.keys([1, 2, 3]).length === 3, "for..in / keys unaffected on arrays");

print("test_ext_batch8: all " + n + " assertions passed");
