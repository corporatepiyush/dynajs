// covers: integration c:src/object/property_get.inc.c c:src/vm/tailcall.inc.c c:src/vm/interpreter.inc.c c:src/builtins/builtin-string.c -- one fast path spans the shape code and both mirrored interpreters
// Method lookup on primitive strings (js_fast_lookup_start). get_field/get_field2 and the fused
// call_method opcodes used to send every s.method(...) on a primitive string through the generic
// JS_GetPropertyInternal. They now start the inline prototype walk at String.prototype for any
// non-index, non-length atom, so accessors, exotic objects and misses still fall back to the generic
// path. String.prototype.charCodeAt also answers a string receiver with an int index directly.
// Why: builtin calls on strings were about 9% of the codegraph profile (ENGINE-PERF.md item 5); r14
// charCodeAt went from 82 to 59 ms per 3M calls. This suite pins the observable semantics
// (patching, accessors, inherited Object.prototype methods, coercions, ropes, indices) in hot loops.
import { suite } from "../lib/kit.js";
const t = suite("test_string_method_lookup_fast");

t.test("charCodeAt edge cases match the spec", (a) => {
    const s = "aé€😀z";
    const want = [97, 233, 8364, 55357, 56832, 122];
    for (let r = 0; r < 3; r++) {
        for (let i = 0; i < want.length; i++) a.eq(s.charCodeAt(i), want[i], "index " + i);
        a.ok(Number.isNaN(s.charCodeAt(-1)), "negative index is NaN");
        a.ok(Number.isNaN(s.charCodeAt(6)), "index past the end is NaN");
        a.eq(s.charCodeAt(), 97, "missing index is 0");
        a.eq(s.charCodeAt(1.9), 233, "fraction truncates");
        a.eq(s.charCodeAt("2"), 8364, "string index coerces");
        a.eq(s.charCodeAt(-0.5), 97, "-0.5 truncates to 0");
        a.ok(Number.isNaN(s.charCodeAt(2 ** 32)), "huge index is NaN");
        a.eq(s.charCodeAt({ valueOf() { return 5; } }), 122, "valueOf on the index runs");
        a.eq(new String("xy").charCodeAt(1), 121, "String object receiver");
        a.eq(String.prototype.charCodeAt.call(12, 1), 50, "number receiver coerces");
        a.throws(() => String.prototype.charCodeAt.call(null, 0), TypeError, "null receiver throws");
        let rope = "";
        for (let k = 0; k < 50; k++) rope += "ab";
        a.eq(rope.charCodeAt(99), 98, "rope or concatenated receiver");
    }
});

t.test("patched, accessor and inherited methods are observed", (a) => {
    const s = "hello";
    const orig = String.prototype.charCodeAt;
    let calls = 0;
    for (let i = 0; i < 20; i++) s.charCodeAt(0);
    String.prototype.charCodeAt = function (i) { calls++; return orig.call(this, i) + 1000; };
    try {
        for (let i = 0; i < 20; i++) a.eq(s.charCodeAt(0), 1104, "patched method result");
        a.eq(calls, 20, "patched method called every time");
    } finally {
        String.prototype.charCodeAt = orig;
    }
    a.eq(s.charCodeAt(0), 104, "restored");
    let getterHits = 0;
    Object.defineProperty(String.prototype, "probe", { get() { getterHits++; return typeof this; }, configurable: true });
    try {
        for (let i = 0; i < 10; i++) a.eq(s.probe, "string", "accessor sees the primitive this in sloppy-less module code");
        a.eq(getterHits, 10, "accessor runs each time");
    } finally {
        delete String.prototype.probe;
    }
    a.eq(s.probe, undefined, "missing property is undefined");
    a.eq(s.hasOwnProperty("length"), true, "Object.prototype method through the String.prototype miss");
    a.eq(s.hasOwnProperty(1), true, "index own property");
    Object.prototype.fromObjectProto = function () { return this.length; };
    try { a.eq(s.fromObjectProto(), 5, "method found on Object.prototype"); }
    finally { delete Object.prototype.fromObjectProto; }
    a.throws(() => s.noSuchMethod(), TypeError, "calling a missing method throws");
    a.eq(s.length, 5, "length is not looked up on the prototype");
    a.eq(s[1], "e", "index access");
    a.eq(s["1"], "e", "string index access");
    String.prototype.at2 = function () { return this; };
    try { a.eq(typeof s.at2(), "string", "strict (module) method gets the primitive this"); }
    finally { delete String.prototype.at2; }
});

t.test("other string methods through the fused call path", (a) => {
    const w = "unsigned long";
    let n = 0;
    for (let i = 0; i < 200; i++) {
        n += w.indexOf("l") + w.slice(2, 5).length + w.toUpperCase().length + (w.startsWith("un") ? 1 : 0) + w.split(" ").length;
    }
    a.eq(n, 200 * (9 + 3 + 13 + 1 + 2), "results");
    const sym = Symbol.iterator;
    a.eq([..."ab"].join(","), "a,b", "symbol-keyed lookup still works");
    a.eq(typeof "x"[sym], "function", "symbol property read");
});

await t.run();
