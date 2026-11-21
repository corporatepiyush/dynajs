// covers: dyna:serialize.structuredClone
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_structured_clone_vserialize");
import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode,
         CBORCanonical, ValueHash, structuredClone } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g) ? h.match(/../g).map((x) => parseInt(x, 16)) : []);
const j = (v) => JSON.stringify(v);

__kitS.section("a deep clone matches", () => {
    const src = { a: 1, b: [1, 2, { c: "three" }], d: new Uint8Array([1, 2]) };
    const c = structuredClone(src);
    eq(j({ a: c.a, b: c.b }), j({ a: src.a, b: src.b }), "a deep clone matches");
    assert(c !== src, "and is a different object");
    assert(c.b !== src.b, "at every level");
    assert(c.b[2] !== src.b[2], "including the leaves");
    assert(c.d instanceof Uint8Array && c.d[1] === 2, "bytes are copied");
    c.b[0] = 99;
    eq(src.b[0], 1, "so mutating the clone does not touch the original");
});

__kitS.section("a self-reference points at the CLONE, not the original", () => {
    const a = { name: "a" };
    a.self = a;
    a.list = [a, a];
    const c = structuredClone(a);
    assert(c.self === c, "a self-reference points at the CLONE, not the original");
    assert(c.list[0] === c, "and so does one through an array");
    assert(c.list[0] === c.list[1], "shared identity inside the graph survives");
    assert(c !== a && c.self !== a, "nothing points back at the source");
});

__kitS.section("a node reached twice is cloned ONCE", () => {
    const shared = { v: 1 };
    const c = structuredClone({ x: shared, y: shared });
    assert(c.x === c.y, "a node reached twice is cloned ONCE");
    assert(c.x !== shared, "and it is a clone");
});

eq(structuredClone(42), 42, "a primitive clones to itself");

eq(structuredClone("s"), "s", "a string too");

eq(structuredClone(null), null, "and null");

throws(() => structuredClone(() => 1), "a function cannot be cloned");

throws(() => structuredClone(), "a value is required");

__kitS.section("a Date clones to a Date with its time", () => {
    const d = new Date(1234567890123);
    const cd = structuredClone({ d }).d;
    assert(cd instanceof Date && cd.getTime() === 1234567890123,
           "a Date clones to a Date with its time");
    const cr = structuredClone({ r: /ab+c/gi }).r;
    assert(cr instanceof RegExp && cr.source === "ab+c" && cr.flags === "gi",
           "a RegExp clones with source and flags");
    const cm = structuredClone({ m: new Map([["k", 1]]) }).m;
    assert(cm instanceof Map && cm.get("k") === 1, "a Map clones with its entries");
    const cs = structuredClone({ s: new Set([1, 2]) }).s;
    assert(cs instanceof Set && cs.size === 2 && cs.has(2), "a Set clones with its values");
    const d2 = new Date(7), m2 = new Map(), s2 = new Set(), r2 = /x/g;
    const o = structuredClone({ a: d2, b: d2, c: m2, d: m2, e: s2, f: s2, g: r2, h: r2 });
    assert(o.a === o.b && o.a !== d2, "two refs to a Date clone to ONE Date");
    assert(o.c === o.d, "Maps share");
    assert(o.e === o.f, "Sets share");
    assert(o.g === o.h, "Regexps share");
    const cyc = new Map();
    cyc.set("self", cyc);
    const cc = structuredClone(cyc);
    assert(cc.get("self") === cc, "a cycle through a Map closes on the clone");
    const scyc = new Set();
    scyc.add(scyc);
    assert(structuredClone(scyc).has(scyc) === false, "the set no longer contains the SOURCE");
    const inner = { v: 1 };
    const sh = structuredClone(new Map([["a", inner], ["b", inner]]));
    assert(sh.get("a") === sh.get("b"), "map values reached twice clone once");
    class MyDate extends Date {}
    assert(structuredClone(new MyDate(5000)).getTime() === 5000,
           "a Date subclass instance clones its time");
    class MyMap extends Map {}
    assert(structuredClone(new MyMap([["k", 1]])).get("k") === 1,
           "a Map subclass instance clones its entries");
});

__kitS.section("an enumerable accessor is read once into a data property (HTML StructuredSerializeInternal uses [[Get]])", () => {
    let calls = 0;
    const o = {};
    Object.defineProperty(o, "g", { enumerable: true, configurable: true,
                                    get() { calls++; return 42; } });
    o.plain = 1;
    const c = structuredClone(o);
    eq(c.g, 42, "the getter's value is cloned");
    eq(calls, 1, "and the getter ran exactly once");
    eq(typeof Object.getOwnPropertyDescriptor(c, "g").get, "undefined", "the clone holds a data property");
});
__kitS.end();

if (fails) {
    print("test_vserialize: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_vserialize failed");
}
print("test_vserialize: " + n + " assertions, 0 failures");
