/* test_object_ext2.js — native Object deep clone/equals/merge/path (batch 2).
 * Run: dynajs tests/test_object_ext2.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) {
    n++;
    const A = JSON.stringify(a), B = JSON.stringify(b);
    if (A !== B) throw new Error("eq failed: " + m + " (got " + A + ", want " + B + ")");
}

/* ---- clone (deep) ---- */
{
    const src = { a: 1, b: { c: [1, 2, { d: 3 }] }, e: new Date(1000) };
    const c = Object.clone(src);
    eq(c, src, "clone equal by value");
    assert(c !== src, "clone is a new object");
    assert(c.b !== src.b, "nested object cloned");
    assert(c.b.c !== src.b.c, "nested array cloned");
    assert(c.b.c[2] !== src.b.c[2], "deep object cloned");
    assert(c.e !== src.e && c.e.getTime() === 1000, "Date cloned by value");
    c.b.c[0] = 99;
    assert(src.b.c[0] === 1, "mutating clone does not touch source");
}
eq(Object.clone(42), 42, "clone primitive");
eq(Object.clone(null), null, "clone null");
eq(Object.clone([1, [2, [3]]]), [1, [2, [3]]], "clone nested array");
{
    const re = Object.clone(/ab+c/gi);
    assert(re instanceof RegExp && re.source === "ab+c" && re.flags === "gi", "clone RegExp");
}

/* ---- equals (deep) ---- */
assert(Object.equals(1, 1) === true, "equals prim");
assert(Object.equals(NaN, NaN) === true, "equals NaN");
assert(Object.equals({ a: 1 }, { a: 1 }) === true, "equals obj");
assert(Object.equals({ a: 1 }, { a: 2 }) === false, "equals obj diff val");
assert(Object.equals({ a: 1 }, { a: 1, b: 2 }) === false, "equals obj diff keys");
assert(Object.equals([1, 2, 3], [1, 2, 3]) === true, "equals arr");
assert(Object.equals([1, 2], [1, 2, 3]) === false, "equals arr len");
assert(Object.equals({ a: { b: [1, { c: 2 }] } }, { a: { b: [1, { c: 2 }] } }) === true, "equals deep");
assert(Object.equals({ a: { b: [1, { c: 2 }] } }, { a: { b: [1, { c: 9 }] } }) === false, "equals deep diff");
assert(Object.equals(new Date(5), new Date(5)) === true, "equals Date");
assert(Object.equals(new Date(5), new Date(6)) === false, "equals Date diff");
assert(Object.equals(/x/g, /x/g) === true, "equals RegExp");
assert(Object.equals(/x/g, /x/i) === false, "equals RegExp flags");
assert(Object.equals(0, -0) === false, "equals distinguishes +0/-0");
assert(Object.equals(0, 0) === true, "equals +0/+0");

/* ---- identical (SameValue) ---- */
assert(Object.identical(1, 1) === true, "identical");
assert(Object.identical(NaN, NaN) === true, "identical NaN");
assert(Object.identical(0, -0) === false, "identical +0/-0");
{ const o = {}; assert(Object.identical(o, o) === true, "identical ref"); }
assert(Object.identical({}, {}) === false, "identical distinct objs");

/* ---- prop / propOr / props ---- */
eq(Object.prop("a", { a: 5 }), 5, "prop");
eq(Object.prop("z", { a: 5 }), undefined, "prop missing");
eq(Object.propOr(0, "a", { a: 5 }), 5, "propOr present");
eq(Object.propOr(0, "z", { a: 5 }), 0, "propOr default");
eq(Object.props(["a", "c"], { a: 1, b: 2, c: 3 }), [1, 3], "props");
eq(Object.props(["a", "z"], { a: 1 }), [1, undefined], "props missing");

/* ---- path / pathOr / paths (array + dotted string) ---- */
const nested = { a: { b: { c: 42 } }, list: [{ x: 1 }, { x: 2 }] };
eq(Object.path(["a", "b", "c"], nested), 42, "path array");
eq(Object.path("a.b.c", nested), 42, "path dotted");
eq(Object.path(["a", "z", "c"], nested), undefined, "path missing");
eq(Object.path("a.z.c", nested), undefined, "path dotted missing");
eq(Object.path(["list", 1, "x"], nested), 2, "path with array index");
eq(Object.pathOr(-1, ["a", "b", "c"], nested), 42, "pathOr present");
eq(Object.pathOr(-1, ["a", "z"], nested), -1, "pathOr default");
eq(Object.paths([["a", "b", "c"], ["list", 0, "x"]], nested), [42, 1], "paths");

/* ---- assocPath / dissocPath (immutable) ---- */
{
    const o = { a: { b: 1 } };
    eq(Object.assocPath(["a", "b"], 9, o), { a: { b: 9 } }, "assocPath overwrite");
    eq(Object.assocPath(["a", "c"], 2, o), { a: { b: 1, c: 2 } }, "assocPath add");
    eq(Object.assocPath(["x", "y"], 7, o), { a: { b: 1 }, x: { y: 7 } }, "assocPath create");
    eq(o, { a: { b: 1 } }, "assocPath did not mutate");
    eq(Object.assocPath("a.b", 5, o), { a: { b: 5 } }, "assocPath dotted");
    /* preserve array container */
    const arrObj = { list: [10, 20, 30] };
    const r = Object.assocPath(["list", 1], 99, arrObj);
    assert(Array.isArray(r.list), "assocPath keeps array container");
    eq(r.list, [10, 99, 30], "assocPath into array");
    eq(arrObj.list, [10, 20, 30], "assocPath did not mutate array");
}
{
    const o = { a: { b: 1, c: 2 }, d: 3 };
    eq(Object.dissocPath(["a", "b"], o), { a: { c: 2 }, d: 3 }, "dissocPath");
    eq(Object.dissocPath(["a", "z"], o), { a: { b: 1, c: 2 }, d: 3 }, "dissocPath missing");
    eq(Object.dissocPath(["d"], o), { a: { b: 1, c: 2 } }, "dissocPath top");
    eq(o, { a: { b: 1, c: 2 }, d: 3 }, "dissocPath did not mutate");
}

/* ---- has / hasIn / hasPath ---- */
assert(Object.has("a", { a: 1 }) === true, "has own");
assert(Object.has("toString", {}) === false, "has not inherited");
assert(Object.hasIn("toString", {}) === true, "hasIn inherited");
assert(Object.hasPath(["a", "b", "c"], nested) === true, "hasPath yes");
assert(Object.hasPath(["a", "z"], nested) === false, "hasPath no");
assert(Object.hasPath("a.b.c", nested) === true, "hasPath dotted");

/* ---- keysIn / valuesIn ---- */
{
    function Parent() { this.own = 1; }
    Parent.prototype.inherited = 2;
    const p = new Parent();
    const ks = Object.keysIn(p).sort();
    assert(ks.indexOf("own") >= 0 && ks.indexOf("inherited") >= 0, "keysIn incl inherited");
    const vs = Object.valuesIn(p);
    assert(vs.indexOf(1) >= 0 && vs.indexOf(2) >= 0, "valuesIn incl inherited");
}

/* ---- propEq / pathEq / eqProps ---- */
assert(Object.propEq(5, "a", { a: 5 }) === true, "propEq");
assert(Object.propEq(5, "a", { a: 6 }) === false, "propEq false");
assert(Object.propEq({ x: 1 }, "a", { a: { x: 1 } }) === true, "propEq deep");
assert(Object.pathEq(42, ["a", "b", "c"], nested) === true, "pathEq");
assert(Object.pathEq(1, ["a", "b", "c"], nested) === false, "pathEq false");
assert(Object.eqProps("a", { a: 1, b: 9 }, { a: 1, b: 8 }) === true, "eqProps eq");
assert(Object.eqProps("b", { b: 9 }, { b: 8 }) === false, "eqProps neq");

/* ---- where / whereEq ---- */
const spec = { a: (v) => v > 0, b: (v) => v === "x" };
assert(Object.where(spec, { a: 5, b: "x", c: 9 }) === true, "where pass");
assert(Object.where(spec, { a: -1, b: "x" }) === false, "where fail pred");
assert(Object.whereEq({ a: 1, b: 2 }, { a: 1, b: 2, c: 3 }) === true, "whereEq pass");
assert(Object.whereEq({ a: 1 }, { a: 9 }) === false, "whereEq fail");
assert(Object.whereEq({ a: { x: 1 } }, { a: { x: 1 } }) === true, "whereEq deep");

/* ---- merge family ---- */
eq(Object.mergeRight({ a: 1, b: 2 }, { b: 3, c: 4 }), { a: 1, b: 3, c: 4 }, "mergeRight (right wins)");
eq(Object.mergeLeft({ a: 1, b: 2 }, { b: 3, c: 4 }), { a: 1, b: 2, c: 4 }, "mergeLeft (left wins)");
eq(Object.merge({ a: 1 }, { a: 2 }), { a: 2 }, "merge alias = mergeRight");
eq(Object.mergeDeepRight({ a: { x: 1, y: 2 } }, { a: { y: 9, z: 3 } }),
   { a: { x: 1, y: 9, z: 3 } }, "mergeDeepRight");
eq(Object.mergeDeepLeft({ a: { x: 1, y: 2 } }, { a: { y: 9, z: 3 } }),
   { a: { x: 1, y: 2, z: 3 } }, "mergeDeepLeft");
eq(Object.mergeDeepRight({ a: [1, 2] }, { a: [3] }), { a: [3] }, "mergeDeep replaces arrays");
/* non-mutating */
{
    const l = { a: { x: 1 } }, r = { a: { y: 2 } };
    Object.mergeDeepRight(l, r);
    eq(l, { a: { x: 1 } }, "mergeDeep did not mutate left");
    eq(r, { a: { y: 2 } }, "mergeDeep did not mutate right");
}

/* ---- non-enumerable on ctor ---- */
{
    const d = Object.getOwnPropertyDescriptor(Object, "clone");
    assert(d && d.enumerable === false, "clone non-enumerable");
    assert(Object.getOwnPropertyNames(Object.prototype).indexOf("clone") < 0,
           "clone not on Object.prototype");
}

console.log("test_object_ext2.js OK — " + n + " assertions");
