// covers: c:src/serialize/bc_read.inc.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_bc_read_inc_ext_batch8");
let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) { assert(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function threw(fn, Ctor, m) { try { fn(); } catch (e) { assert(!Ctor || e instanceof Ctor, m + " wrong error: " + e); return; } assert(false, m + " did not throw"); }

threw(() => Array.repeat(1, -1), RangeError, "Array.repeat negative");
threw(() => Array.repeat(1, 1e9), RangeError, "Array.repeat too large");

threw(() => [1, 2].sortWith([() => { throw new Error("x"); }]), Error, "sortWith throw propagates");
threw(() => [1, 2].dropRepeatsWith(() => { throw new Error("x"); }), Error, "dropRepeatsWith throw");
threw(() => [1, 2].reduceBy(() => { throw new Error("x"); }, 0, x => x), Error, "reduceBy valueFn throw");
threw(() => [1, 2].differenceWith(() => { throw new Error("x"); }, [1]), Error, "differenceWith throw");

__kitS.section("repeat reentrant count", () => {
eq(Array.repeat("a", { valueOf() { return 2; } }), ["a", "a"], "repeat reentrant count");
});

const mapT = f => xf => ({
  "@@transducer/init": () => xf["@@transducer/init"](),
  "@@transducer/result": a => xf["@@transducer/result"](a),
  "@@transducer/step": (acc, x) => xf["@@transducer/step"](acc, f(x)),
});

threw(() => [1, 2].transduce(mapT(() => { throw new Error("x"); }), (a) => a, []), Error, "transduce throw propagates");

__kitS.section("sequence cartesian", () => {
eq([[1, 2], [3, 4]].sequence(Array), [[1, 3], [1, 4], [2, 3], [2, 4]], "sequence cartesian");
});

const A = ["a", "b", "c", "d", "e"];

__kitS.section("FromIndex requires callback arg", () => {
threw(() => A.mapFromIndex(2), TypeError, "FromIndex requires callback arg");
threw(() => A.reduceFromIndex(0), TypeError, "reduceFromIndex requires callback");
threw(() => A.mapFromIndex(0, x => { throw new Error("x"); }), Error, "mapFromIndex throw propagates");
eq([].mapFromIndex(0, x => x), [], "FromIndex empty map");
});

__kitS.section("identity", () => {
eq(Function.identity(5), 5, "identity");
});

__kitS.section("segment L36", () => {
eq(Function.of(7), [7], "of");
});

__kitS.section("not falsy", () => {
eq(Function.not(0), true, "not falsy");
});

__kitS.section("not truthy", () => {
eq(Function.not(1), false, "not truthy");
});

__kitS.section("negate", () => {
eq(Function.negate(3), -3, "negate");
});

__kitS.section("applyTo", () => {
eq(Function.applyTo(5, x => x * 2), 10, "applyTo");
});

__kitS.section("always ignores args", () => {
{ const k = Function.always(42); eq(k(), 42, "always"); eq(k(1, 2, 3), 42, "always ignores args"); }
});

__kitS.section("cond pos", () => {
{ const c = Function.cond([[x => x > 0, () => "pos"], [x => x < 0, () => "neg"], [() => true, () => "zero"]]);
  eq(c(5), "pos", "cond pos"); eq(c(-5), "neg", "cond neg"); eq(c(0), "zero", "cond default");
  assert(Function.cond([[() => false, () => 1]])(9) === undefined, "cond no match -> undefined"); }
});

__kitS.section("uncurryN", () => {
{ const add3 = a => b => c => a + b + c; eq(Function.uncurryN(3, add3)(1, 2, 3), 6, "uncurryN"); }
});

__kitS.section("uncurryN mixed arity", () => {
{ const g = (a, b) => c => a + b + c; eq(Function.uncurryN(2, g)(1, 2, 10), 13, "uncurryN mixed arity"); }
});

__kitS.section("lift", () => {
eq(Function.lift((a, b) => a * b)([1, 2], [3, 4]), [3, 4, 6, 8], "lift");
});

__kitS.section("liftN", () => {
eq(Function.liftN(2, (a, b) => a + b)([1, 2], [10, 20]), [11, 21, 12, 22], "liftN");
});

__kitS.section("lift empty list -> empty", () => {
eq(Function.lift((a, b) => a + b)([1, 2], []), [], "lift empty list -> empty");
});

__kitS.section("ap list form", () => {
eq(Function.ap([x => x + 1, x => x * 2], [10, 20]), [11, 21, 20, 40], "ap list form");
});

__kitS.section("ap empty fns", () => {
eq(Function.ap([], [1, 2]), [], "ap empty fns");
});

__kitS.section("ap S-combinator", () => {
{ const s = Function.ap(a => b => a + b, x => x * 10); eq(s(5), 55, "ap S-combinator"); }
});

threw(() => Function.cond([[() => { throw new Error("x"); }, () => 1]])(0), Error, "cond throw propagates");

__kitS.section("startsWith non-enumerable", () => {
assert(!Array.prototype.propertyIsEnumerable("startsWith"), "startsWith non-enumerable");
});

__kitS.section("Function.identity non-enumerable", () => {
assert(!Function.propertyIsEnumerable("identity"), "Function.identity non-enumerable");
});
__kitS.end();

print("test_ext_batch8: all " + n + " assertions passed");
