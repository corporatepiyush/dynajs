/* Differential oracle for object-literal shape pre-sizing
 * (CONFIG_PRESIZE_LITERAL; analysis in parser.inc.c, runtime in
 * shapes_objects_gc.inc.c, hook in the OP_object interpreter case).
 *
 * A literal whose key set is statically known is created directly at its final
 * shape. That is only sound if the resulting object is INDISTINGUISHABLE from
 * one built by incremental field stores, and the ways it could differ are all
 * silent -- no crash, just a subtly wrong object:
 *
 *   - property ENUMERATION ORDER (insertion order for string keys, but ascending
 *     numeric order for array-index keys, which must sort first);
 *   - property ATTRIBUTES (a literal creates configurable/writable/enumerable);
 *   - duplicate keys (last value wins, first position kept);
 *   - NESTING -- `{a:{b:1}}` emits the inner OP_object and its define_field for
 *     `b` BEFORE the outer's for `a`, so an analysis that mis-attributed the
 *     inner field would give the outer object a phantom `b`;
 *   - the forms that do not emit OP_define_field at all: computed keys,
 *     `__proto__:`, methods, accessors, spread.
 *
 * So the proof is that the entire output is byte-identical to a build with the
 * optimisation compiled out:
 *
 *   make CONFIG_NATIVE_MODULES=y
 *   rm .obj/dynajs.o && <same clang cmd> -DCONFIG_PRESIZE_LITERAL=0 ...
 *   ./dynajs tests/test_object_literal_presize.js | shasum   # must match
 */

let out = [];
function emit(s) { out.push(s); }

/* Full structural rendering: keys in enumeration order, each with its
   descriptor. Comparing JSON alone would hide an attribute or order change. */
function show(label, o) {
    if (o === null || typeof o !== "object") { emit(label + " = " + String(o)); return; }
    let parts = [];
    for (const k of Reflect.ownKeys(o)) {
        const d = Object.getOwnPropertyDescriptor(o, k);
        const kn = typeof k === "symbol" ? "@@" + k.description : k;
        if ("value" in d)
            parts.push(kn + ":" + JSON.stringify(d.value) +
                       (d.writable ? "w" : "-") + (d.enumerable ? "e" : "-") +
                       (d.configurable ? "c" : "-"));
        else
            parts.push(kn + ":<" + (d.get ? "g" : "") + (d.set ? "s" : "") + ">" +
                       (d.enumerable ? "e" : "-") + (d.configurable ? "c" : "-"));
    }
    let proto = Object.getPrototypeOf(o);
    emit(label + " {" + parts.join(" ") + "} proto=" +
         (proto === null ? "null" : proto === Object.prototype ? "Object" : "other") +
         " forin=" + (function () { let a = []; for (const k in o) a.push(k); return a.join(","); })() +
         " keys=" + Object.keys(o).join(",") +
         " json=" + JSON.stringify(o));
}

/* ---- the shapes the analysis is allowed to match (leaf values) ---------- */
{
    const a = 1, b = "two", c = null;
    show("2 fields", { a: 1, b: 2 });
    show("3 fields", { a: 1, b: 2, c: 3 });
    show("8 fields", { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8 });
    show("shorthand", { a, b, c });
    show("mixed leaf", { u: undefined, n: null, t: true, f: false, s: "", z: 0, m: -1 });
    show("locals", { p: a, q: b, r: c });
    show("one field", { only: 1 });
    show("empty", {});
}

/* ---- duplicates: last value wins, FIRST position is kept --------------- */
{
    show("dup2", { x: 1, y: 2, x: 3 });
    show("dup3", { x: 1, x: 2, x: 3 });
    show("dup-mid", { a: 1, b: 2, a: 3, c: 4 });
    show("dup-adjacent", { a: 1, a: 2, b: 3 });
}

/* ---- numeric / array-index keys sort FIRST, ascending ------------------ */
{
    show("num only", { 2: "two", 1: "one", 10: "ten" });
    show("num+str", { 2: "two", b: "bee", 1: "one", a: "ay" });
    show("str+num", { z: 1, 0: 2, y: 3, 1: 4 });
    show("numeric strings", { "2": 1, "01": 2, "1": 3 });
    show("big index", { 4294967294: 1, 4294967295: 2, a: 3 });
    show("negative/float keys", { "-1": 1, "1.5": 2, a: 3 });
}

/* ---- nesting: the inner literal must not leak into the outer ----------- */
{
    show("nested-1", { a: { b: 1, c: 2 }, d: 3 });
    show("nested-1.inner", { a: { b: 1, c: 2 }, d: 3 }.a);
    show("nested-2", { a: 1, b: { c: 2, d: 3 }, e: 4 });
    show("nested-3", { a: { b: { c: 1, d: 2 }, e: 3 }, f: 4 });
    show("nested-arr", { a: [1, 2], b: 3 });
    const deep = { l1: { l2: { l3: { x: 1, y: 2 }, m: 3 }, n: 4 }, o: 5 };
    show("deep", deep);
    show("deep.l1", deep.l1);
    show("deep.l1.l2", deep.l1.l2);
    show("deep.l1.l2.l3", deep.l1.l2.l3);
}

/* ---- forms that emit something other than OP_define_field -------------- */
{
    const k = "computed";
    show("computed", { [k]: 1, a: 2 });
    show("computed-first", { a: 1, [k]: 2, b: 3 });
    show("proto-null", { __proto__: null, a: 1, b: 2 });
    show("proto-obj", { a: 1, __proto__: { inherited: 9 }, b: 2 });
    show("method", { m() { return 1; }, a: 2 });
    show("getter", { get g() { return 1; }, a: 2 });
    show("setter", { set s(v) {}, a: 2 });
    show("getset", { get g() { return 1; }, set g(v) {}, a: 2 });
    show("spread", { ...{ s1: 1, s2: 2 }, a: 3 });
    show("spread-after", { a: 1, b: 2, ...{ c: 3 } });
    show("symbol-key", { [Symbol.iterator]: 1, a: 2 });
    show("async-method", { async am() {}, a: 1 });
    show("gen-method", { *gm() {}, a: 1 });
}

/* ---- non-leaf values: not matched by the analysis, must still be right -- */
{
    function f() { return 42; }
    let i = 0;
    show("call value", { a: f(), b: 2 });
    show("expr value", { a: 1 + 1, b: 2 * 3 });
    show("side effects", { a: i++, b: i++, c: i++ });
    show("ternary", { a: true ? 1 : 2, b: 3 });
    show("nested call", { a: { b: f() }, c: 2 });
}

/* ---- mutation after construction: the pre-created shape must behave ----- */
{
    const o = { a: 1, b: 2, c: 3 };
    delete o.b;
    show("after delete", o);
    o.d = 4;
    show("after add", o);
    o.b = 9;
    show("after re-add", o);
    Object.defineProperty(o, "a", { enumerable: false });
    show("after non-enum", o);
    const s = { a: 1, b: 2 };
    Object.seal(s);
    show("sealed", s);
    const fz = { a: 1, b: 2 };
    Object.freeze(fz);
    show("frozen", fz);
    const p = { a: 1, b: 2 };
    Object.preventExtensions(p);
    show("non-extensible", p);
}

/* ---- identity/equality of separately built objects --------------------- */
{
    const a1 = { x: 1, y: 2 }, a2 = { x: 1, y: 2 };
    emit("same keys equal? " + (JSON.stringify(a1) === JSON.stringify(a2)) +
         " same object? " + (a1 === a2));
    /* built the slow way must be indistinguishable from the pre-sized way */
    const slow = {}; slow.x = 1; slow.y = 2;
    emit("slow vs literal keys: " + (Object.keys(slow).join(",") === Object.keys(a1).join(",")));
    emit("slow vs literal json: " + (JSON.stringify(slow) === JSON.stringify(a1)));
    emit("Object.assign: " + JSON.stringify(Object.assign({}, a1)));
    emit("entries: " + JSON.stringify(Object.entries(a1)));
    emit("spread copy: " + JSON.stringify({ ...a1 }));
}

/* ---- a literal in a hot loop: the cached shape is reused every time ----- */
{
    let last = null, allSame = true;
    for (let i = 0; i < 5000; i++) {
        const o = { alpha: i, beta: i * 2, gamma: i * 3 };
        if (last && Object.keys(o).join(",") !== last) allSame = false;
        last = Object.keys(o).join(",");
    }
    emit("hot-loop key order stable: " + allSame + " (" + last + ")");
}

/* ---- exception mid-literal leaves nothing observable ------------------- */
{
    function boom() { throw new Error("boom"); }
    try { const o = { a: 1, b: boom(), c: 3 }; emit("no throw?!"); }
    catch (e) { emit("threw mid-literal: " + e.message); }
    /* the pre-created shape must not have leaked into the shape cache in a way
       that changes a later, identical literal */
    show("after mid-literal throw", { a: 1, b: 2, c: 3 });
}

/* ---- many fields: past the analysis cap, must degrade cleanly ---------- */
{
    const wide = { f0:0,f1:1,f2:2,f3:3,f4:4,f5:5,f6:6,f7:7,f8:8,f9:9,
                   f10:10,f11:11,f12:12,f13:13,f14:14,f15:15,f16:16,f17:17,
                   f18:18,f19:19,f20:20,f21:21,f22:22,f23:23,f24:24,f25:25,
                   f26:26,f27:27,f28:28,f29:29,f30:30,f31:31,f32:32,f33:33 };
    emit("wide keys=" + Object.keys(wide).length + " order=" + Object.keys(wide).join(","));
}

console.log(out.join("\n"));
console.log("LINES " + out.length);

/* build-independent invariants */
function eq(a, b, what) {
    if (a !== b) { console.log("SELFCHECK FAIL " + what + ": " + a + " !== " + b); throw new Error(what); }
}
{
    eq(Object.keys({ 2: 1, b: 2, 1: 3 }).join(","), "1,2,b", "integer keys sort first");
    eq(Object.keys({ a: 1, b: 2, a: 3 }).join(","), "a,b", "duplicate keeps first position");
    eq(JSON.stringify({ a: 1, b: 2, a: 3 }), '{"a":3,"b":2}', "duplicate last value wins");
    eq(Object.keys({ a: { b: 1 }, c: 2 }).join(","), "a,c", "nested does not leak");
    eq(Object.keys({ a: { b: 1 }, c: 2 }.a).join(","), "b", "inner intact");
    eq(Object.getPrototypeOf({ __proto__: null, a: 1, b: 2 }), null, "__proto__ honoured");
    const d = Object.getOwnPropertyDescriptor({ a: 1, b: 2 }, "a");
    eq(d.writable && d.enumerable && d.configurable, true, "literal props are C/W/E");
    console.log("SELFCHECK ok");
}
