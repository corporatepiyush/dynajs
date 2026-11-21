// covers: c:src/builtins/builtin-array.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_array_ext_batch8");
let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function threw(fn, Ctor, m) { try { fn(); } catch (e) { assert(!Ctor || e instanceof Ctor, m + " wrong error: " + e); return; } assert(false, m + " did not throw"); }

eq([{ n: "z", a: 2 }, { n: "a", a: 2 }, { n: "m", a: 1 }]
    .sortWith([(x, y) => x.a - y.a, (x, y) => x.n < y.n ? -1 : 1]).map(x => x.n),
   ["m", "a", "z"], "sortWith");

__kitS.section("reduceBy groups", () => {
  const tmpl = [];
  const r = [1, 2, 3, 4].reduceBy((acc, x) => { acc.push(x); return acc; }, tmpl, x => x % 2 ? "odd" : "even");
  eq(r, { odd: [1, 3], even: [2, 4] }, "reduceBy groups");
  assert(r.odd !== r.even, "reduceBy: per-key accumulators do not alias");
  eq(tmpl, [], "reduceBy: template accumulator not mutated");
});

eq(Array.repeat("x", 3), ["x", "x", "x"], "Array.repeat");

eq(Array.repeat(0, 0), [], "Array.repeat 0");

{ const o = {}; const r = Array.repeat(o, 2); assert(r[0] === o && r[1] === o, "Array.repeat same reference"); }

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

eq([1, 2, 3, 4].transduce(mapT(x => x * 2), (acc, x) => { acc.push(x); return acc; }, []), [2, 4, 6, 8], "transduce map + reducer fn");

eq([1, 2, 3, 4, 5].transduce(takeT(3), (a, x) => { a.push(x); return a; }, []), [1, 2, 3], "transduce reduced");

eq([[1], [2], [3]].sequence(Array), [[1, 2, 3]], "sequence singletons");

eq([].sequence(Array), [[]], "sequence empty");

eq([[1, 2], []].sequence(Array), [], "sequence with empty applicative");

eq([1, 2].traverse(Array, x => [x, x * 10]), [[1, 2], [1, 20], [10, 2], [10, 20]], "traverse");

eq([1, 2, 3].traverse(Array, x => [x]), [[1, 2, 3]], "traverse singletons");

const A = ["a", "b", "c", "d", "e"];

{ let o = []; A.forEachFromIndex(2, true, (el, i) => o.push([el, i])); eq(o, [["c", 2], ["d", 3], ["e", 4], ["a", 0], ["b", 1]], "forEachFromIndex loop"); }

{ let idx = []; A.reduceRightFromIndex(2, (a, v, i) => { idx.push(i); return a + v; }, "INIT:"); eq(idx, [4, 3, 2], "reduceRightFromIndex shifted-index quirk"); }

{ let idx = []; A.reduceRightFromIndex(2, true, (a, v, i) => { idx.push(i); return a + v; }, "INIT:"); eq(idx, [1, 0, 4, 3, 2], "reduceRightFromIndex loop index"); }

assert(!Array.prototype.propertyIsEnumerable("transduce"), "transduce non-enumerable");

assert(!Array.prototype.propertyIsEnumerable("mapFromIndex"), "mapFromIndex non-enumerable");

assert(!Array.propertyIsEnumerable("repeat"), "Array.repeat non-enumerable");

assert(Object.keys([1, 2, 3]).length === 3, "for..in / keys unaffected on arrays");

print("test_ext_batch8: all " + n + " assertions passed");
