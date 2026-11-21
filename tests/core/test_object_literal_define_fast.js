// covers: integration c:src/object/shapes_objects_gc.inc.c c:src/vm/tailcall.inc.c c:src/vm/interpreter.inc.c -- one fast path spans the shape code and both mirrored interpreters
// Literal define_field fast path (js_object_presize_define_fast). Once a literal site has a cached
// shape, a field store into the fresh object goes straight into its slot when the object is still on
// the site shape and that slot holds the same atom with plain C_W_E flags. Anything else (duplicate
// keys, accessors, computed keys, __proto__, spread, a different shape) takes the generic
// JS_DefinePropertyValue. Why: the generic define was 60% of literal creation (ENGINE-PERF.md item 1,
// r01). Every case runs in a hot loop, so the site is recorded and then reused, and each result is
// compared with a descriptor dump against the expected own-property list.
// Bug fixed alongside: computed keys and spreads could make the recorder cache an iteration-specific
// shape ({a, [k]: v, c} with k === "c" cached [a, c]), so later iterations produced key order a, c, b
// instead of a, b, c. define_array_el, copy_data_properties and computed define_method now drop the
// recording when they target the literal under construction (BB_REC_DROP / TC_BB_REC_DROP).
import { suite } from "../lib/kit.js";
const t = suite("test_object_literal_define_fast");

function dump(o) {
    const parts = [];
    for (const k of Reflect.ownKeys(o)) {
        const d = Object.getOwnPropertyDescriptor(o, k);
        const kn = typeof k === "symbol" ? "@@" + k.description : k;
        if ("value" in d) parts.push(kn + ":" + (typeof d.value === "object" && d.value ? "obj" : JSON.stringify(d.value)) + (d.writable ? "w" : "-") + (d.enumerable ? "e" : "-") + (d.configurable ? "c" : "-"));
        else parts.push(kn + ":<" + (d.get ? "g" : "") + (d.set ? "s" : "") + ">" + (d.enumerable ? "e" : "-") + (d.configurable ? "c" : "-"));
    }
    const p = Object.getPrototypeOf(o);
    return parts.join(" ") + " proto=" + (p === null ? "null" : p === Object.prototype ? "Object" : "other");
}

function hot(a, name, make, want, n = 40) {
    let bad = 0;
    for (let i = 0; i < n; i++) {
        const got = dump(make(i));
        const w = typeof want === "function" ? want(i) : want;
        if (got !== w && bad++ < 2) a.eq(got, w, name + " iteration " + i);
    }
    a.ok(bad === 0, name + ": " + bad + " bad iterations");
}

t.test("plain and nested literals keep values, order and flags", (a) => {
    hot(a, "9 fields", (i) => ({ k: 1, t: "x", off: i, end: i + 1, line: 1, col: 1, sl: 0, ws: false, p: "" }),
        (i) => `k:1wec t:"x"wec off:${i}wec end:${i + 1}wec line:1wec col:1wec sl:0wec ws:falsewec p:""wec proto=Object`);
    hot(a, "nested", (i) => ({ a: { b: i, c: [i] }, d: i }), (i) => `a:objwec d:${i}wec proto=Object`);
    let sum = 0;
    for (let i = 0; i < 2000; i++) { const o = { x: i, y: { z: i }, w: "s" + i }; sum += o.x + o.y.z + o.w.length; }
    a.eq(sum, 2 * 1999 * 1000 + [...Array(2000).keys()].reduce((s, i) => s + ("s" + i).length, 0), "values survive in a hot loop");
});

t.test("duplicate keys, accessors, methods and __proto__", (a) => {
    hot(a, "duplicate", (i) => ({ a: 1, b: 2, a: i }), (i) => `a:${i}wec b:2wec proto=Object`);
    hot(a, "dup after getter", (i) => ({ a: 1, get a() { return 2; }, b: i }), (i) => `a:<g>ec b:${i}wec proto=Object`);
    hot(a, "getter in middle", (i) => ({ a: i, get b() { return 1; }, c: 3 }), (i) => `a:${i}wec b:<g>ec c:3wec proto=Object`);
    hot(a, "method", (i) => ({ a: i, m() { return 1; }, c: 3 }), (i) => `a:${i}wec m:undefinedwec c:3wec proto=Object`);
    hot(a, "getter after a recorded prefix", (i) => ({ a: i, b: 2, get g() { return 1; }, c: 3 }), (i) => `a:${i}wec b:2wec g:<g>ec c:3wec proto=Object`);
    hot(a, "method after a recorded prefix", (i) => ({ a: i, b: 2, m() { return 1; }, c: 3 }), (i) => `a:${i}wec b:2wec m:undefinedwec c:3wec proto=Object`);
    const P = { inherited: 1 };
    hot(a, "__proto__", (i) => ({ a: i, __proto__: P, c: 3 }), (i) => `a:${i}wec c:3wec proto=other`);
    hot(a, "null proto", (i) => ({ __proto__: null, a: i, b: 2 }), (i) => `a:${i}wec b:2wec proto=null`);
});

t.test("computed keys and spreads that change per iteration", (a) => {
    const keys = ["b", "a", "c", "1", "b"];
    hot(a, "computed", (i) => ({ a: 1, [keys[i % 5]]: i, c: 3 }), (i) => {
        const k = keys[i % 5];
        if (k === "a") return `a:${i}wec c:3wec proto=Object`;
        if (k === "c") return `a:1wec c:3wec proto=Object`;
        if (k === "1") return `1:${i}wec a:1wec c:3wec proto=Object`;
        return `a:1wec b:${i}wec c:3wec proto=Object`;
    });
    const srcs = [{}, { b: 2 }, { a: 9 }, { c: 7, d: 8 }];
    hot(a, "spread middle", (i) => ({ a: i, ...srcs[i % 4], c: 3 }), (i) => [
        `a:${i}wec c:3wec proto=Object`, `a:${i}wec b:2wec c:3wec proto=Object`,
        `a:9wec c:3wec proto=Object`, `a:${i}wec c:3wec d:8wec proto=Object`][i % 4]);
    hot(a, "numeric key", (i) => ({ a: i, 2: "x", b: 1 }), (i) => `2:"x"wec a:${i}wec b:1wec proto=Object`);
    const sym = Symbol("s");
    hot(a, "symbol key", (i) => ({ a: i, [sym]: 1, b: 2 }), (i) => `a:${i}wec b:2wec @@s:1wec proto=Object`);
});

t.test("setters and frozen state on Object.prototype are not consulted", (a) => {
    let hits = 0;
    Object.defineProperty(Object.prototype, "zz", { set() { hits++; }, configurable: true });
    try {
        hot(a, "inherited setter", (i) => ({ a: i, zz: 2 }), (i) => `a:${i}wec zz:2wec proto=Object`);
        a.eq(hits, 0, "the inherited setter never runs for a literal field");
    } finally {
        delete Object.prototype.zz;
    }
    hot(a, "same site after prototype change", (i) => ({ a: i, zz: 2 }), (i) => `a:${i}wec zz:2wec proto=Object`);
});

t.test("literals mutated after creation do not poison the site", (a) => {
    const made = [];
    for (let i = 0; i < 40; i++) {
        const o = { a: i, b: 2, c: 3 };
        if (i % 3 === 0) delete o.b;
        if (i % 5 === 0) Object.defineProperty(o, "a", { writable: false });
        if (i % 7 === 0) o.extra = 1;
        made.push(o);
    }
    for (let i = 0; i < 40; i++) {
        const o = made[i];
        a.eq(o.a, i, "a of " + i);
        a.eq(o.b, i % 3 === 0 ? undefined : 2, "b of " + i);
        a.eq(Object.getOwnPropertyDescriptor(o, "a").writable, i % 5 !== 0, "a writable of " + i);
    }
    hot(a, "after mutations", (i) => ({ a: i, b: 2, c: 3 }), (i) => `a:${i}wec b:2wec c:3wec proto=Object`);
});

await t.run();
