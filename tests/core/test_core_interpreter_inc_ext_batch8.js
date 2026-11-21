// covers: c:src/vm/interpreter.inc.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_interpreter_inc_ext_batch8");
let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function threw(fn, Ctor, m) { try { fn(); } catch (e) { assert(!Ctor || e instanceof Ctor, m + " wrong error: " + e); return; } assert(false, m + " did not throw"); }

eq([1, [2, 3], [4, [5]]].unnest(), [1, 2, 3, 4, [5]], "unnest one level");

eq([1, 2, 3].unnest(), [1, 2, 3], "unnest flat");

eq([1, 1, 2, 3, 3, 3, 1].dropRepeats(), [1, 2, 3, 1], "dropRepeats");

eq([[1], [1], [2]].dropRepeats(), [[1], [2]], "dropRepeats deep");

eq([1, 2, 2, 3].dropRepeatsWith((a, b) => a === b), [1, 2, 3], "dropRepeatsWith");

eq([{ a: 1 }, { a: 1 }, { a: 2 }].dropRepeatsBy(x => x.a), [{ a: 1 }, { a: 2 }], "dropRepeatsBy");

eq([].dropRepeats(), [], "dropRepeats empty");

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

__kitS.section("reduceBy honours @@transducer/reduced", () => {
  const r = [1, 2, 3, 4, 5].reduceBy(
    (acc, x) => acc >= 6 ? { "@@transducer/reduced": true, "@@transducer/value": acc } : acc + x,
    0, () => "k");
  eq(r, { k: 6 }, "reduceBy honours @@transducer/reduced");
});

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
__kitS.end();

const takeT = n => xf => { let i = 0; return {
  "@@transducer/init": () => xf["@@transducer/init"](),
  "@@transducer/result": a => xf["@@transducer/result"](a),
  "@@transducer/step": (acc, x) => { if (i < n) { i++; acc = xf["@@transducer/step"](acc, x); if (i >= n) return { "@@transducer/reduced": true, "@@transducer/value": acc }; } return acc; },
}; };

const compose2 = (f, g) => x => f(g(x));

eq([1, 2, 3, 4, 5, 6].into([], filterT(x => x % 2 === 0)), [2, 4, 6], "into filter");

eq([1, 2, 3, 4, 5, 6].into([], compose2(filterT(x => x % 2 === 0), mapT(x => x * 10))), [20, 40, 60], "into composed");

eq([1, 2, 3, 4, 5].into([], takeT(2)), [1, 2], "into reduced early-exit");

eq([["a", 1], ["b", 2]].into({}, mapT(p => p)), { a: 1, b: 2 }, "into object");

const A = ["a", "b", "c", "d", "e"];

eq(A.mapFromIndex(2, x => x), ["c", "d", "e"], "mapFromIndex");

eq(A.mapFromIndex(2, true, x => x), ["c", "d", "e", "a", "b"], "mapFromIndex loop");

eq([{ name: "x" }, { name: "y" }, { name: "z" }].mapFromIndex(1, "name"), ["y", "z"], "mapFromIndex property");

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

eq([].reduceFromIndex(0, (a, v) => a + v, 5), 5, "FromIndex empty reduce seeded");

eq(A.mapFromIndex(-2, x => x), ["d", "e"], "mapFromIndex negative index");

eq(A.mapFromIndex({ valueOf() { return 3; } }, x => x), ["d", "e"], "mapFromIndex reentrant startIndex");

print("test_ext_batch8: all " + n + " assertions passed");
