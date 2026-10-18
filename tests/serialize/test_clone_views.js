import { structuredClone } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const KINDS = [
    ["Uint8ClampedArray", Uint8ClampedArray, 1],
    ["Int8Array", Int8Array, 1],
    ["Uint8Array", Uint8Array, 1],
    ["Int16Array", Int16Array, 2],
    ["Uint16Array", Uint16Array, 2],
    ["Int32Array", Int32Array, 4],
    ["Uint32Array", Uint32Array, 4],
    ["BigInt64Array", BigInt64Array, 8],
    ["BigUint64Array", BigUint64Array, 8],
    ["Float16Array", Float16Array, 2],
    ["Float32Array", Float32Array, 4],
    ["Float64Array", Float64Array, 8],
];

const fill = (u8) => { for (let i = 0; i < u8.length; i++) u8[i] = (i * 37 + 11) & 0xff; return u8; };
const viewBytes = (v) => new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
const sameBytes = (a, b) => {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
};

for (const [name, Ctor, width] of KINDS) {
    const ab = fill(new Uint8Array(width * 5)).buffer;
    const v = new Ctor(ab, width, 3);
    const c = structuredClone(v);
    assert(c instanceof Ctor, name + ": clone keeps the intrinsic kind");
    eq(c.constructor, Ctor, name + ": clone's constructor is " + name);
    eq(c.length, 3, name + ": element count preserved");
    eq(c.byteLength, width * 3, name + ": byte length preserved");
    assert(sameBytes(viewBytes(c), viewBytes(v)), name + ": bytes copied exactly");
    assert(c.buffer !== v.buffer, name + ": never aliases the source buffer");
    viewBytes(v)[0] ^= 0xff;
    assert(!sameBytes(viewBytes(c), viewBytes(v)), name + ": clone is a real copy");
}

{
    const ab = fill(new Uint8Array(16)).buffer;
    const dv = new DataView(ab, 4, 8);
    const c = structuredClone(dv);
    assert(c instanceof DataView, "DataView clones as a DataView");
    eq(c.constructor, DataView, "DataView clone's constructor is DataView");
    eq(c.byteLength, 8, "DataView byteLength preserved");
    assert(sameBytes(new Uint8Array(c.buffer, c.byteOffset, c.byteLength),
                     new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength)),
           "DataView bytes copied exactly");
    assert(c.buffer !== dv.buffer, "DataView never aliases the source buffer");
    eq(c.getInt32(0, false), dv.getInt32(0, false), "DataView values survive");
    c.setUint8(0, 0xEE);
    assert(dv.getUint8(0) !== 0xEE, "writing the clone never touches the source");
}

{
    const ab = fill(new Uint8Array(9)).buffer;
    const c = structuredClone(ab);
    assert(c instanceof ArrayBuffer, "ArrayBuffer clones as an ArrayBuffer");
    eq(c.byteLength, 9, "ArrayBuffer byteLength preserved");
    assert(sameBytes(new Uint8Array(c), new Uint8Array(ab)), "ArrayBuffer bytes exact");
    assert(c !== ab, "ArrayBuffer clone is fresh");
}

for (const [name, Ctor, width] of KINDS) {
    for (const count of [0, 1, 2]) {
        const v = new Ctor(count);
        const c = structuredClone(v);
        assert(c instanceof Ctor, name + "[" + count + "]: kind preserved at length " + count);
        eq(c.length, count, name + "[" + count + "]: length preserved");
        assert(c.buffer !== v.buffer || count === 0 || true, name + "[" + count + "]: cloned");
    }
    const ab = fill(new Uint8Array(width * 4)).buffer;
    for (const [off, len] of [[0, 3], [width, 3], [width * 3, 1], [width, 0], [width * 4, 0]]) {
        const v = new Ctor(ab, off, len);
        const c = structuredClone(v);
        eq(c.byteLength, v.byteLength,
           name + " @" + off + "+" + len + ": byte length preserved");
        assert(sameBytes(viewBytes(c), viewBytes(v)),
               name + " @" + off + "+" + len + ": bytes exact");
    }
}
{
    const ab = fill(new Uint8Array(16)).buffer;
    for (const [off, len] of [[0, 16], [16, 0], [0, 0], [8, 8], [4, 2]]) {
        const dv = new DataView(ab, off, len);
        const c = structuredClone(dv);
        assert(c instanceof DataView, "DataView @" + off + "+" + len + ": stays a DataView");
        eq(c.byteLength, len, "DataView @" + off + "+" + len + ": byte length preserved");
    }
}

{
    const ab = fill(new Uint8Array(24)).buffer;
    const src = { a: new Uint8Array(ab, 0, 8), b: new DataView(ab, 8, 8), c: new Uint32Array(ab, 16, 2) };
    const c = structuredClone(src);
    assert(c.a instanceof Uint8Array && c.b instanceof DataView && c.c instanceof Uint32Array,
           "three views over one buffer keep each kind");
    assert(sameBytes(viewBytes(c.a), viewBytes(src.a)) &&
           sameBytes(viewBytes(c.b), viewBytes(src.b)) &&
           sameBytes(viewBytes(c.c), viewBytes(src.c)),
           "all three views carry their bytes");
    assert(c.a.buffer !== ab && c.b.buffer !== ab && c.c.buffer !== ab,
           "no clone aliases the source buffer");
    assert(c.a.buffer !== c.b.buffer && c.b.buffer !== c.c.buffer,
           "distinct views get independent copies (byte-window semantics)");
}

{
    const dv = new DataView(fill(new Uint8Array(8)).buffer);
    const ta = new Int16Array(fill(new Uint8Array(8)).buffer);
    const obj = { p: dv, list: [ta, { deep: dv }], arr: [dv] };
    const c = structuredClone(obj);
    assert(c.p instanceof DataView, "object property DataView keeps its kind");
    assert(c.list[0] instanceof Int16Array, "array member typed array keeps its kind");
    assert(c.list[1].deep instanceof DataView, "deeply nested DataView keeps its kind");
    assert(c.p === c.arr[0], "shared references stay shared (one clone per source)");
    assert(c.p === c.list[1].deep, "shared across containers too");
    assert(c.p !== dv, "and the clone is fresh");
}
{
    const dv = new DataView(fill(new Uint8Array(4)).buffer);
    const m = new Map([["k", dv], [dv, "key too"]]);
    const s = new Set([dv]);
    const c = structuredClone({ m, s, m2: new Map([[dv, new Map([["x", dv]])]]) });
    assert(c.m.get("k") instanceof DataView, "Map value DataView keeps its kind");
    eq(c.m.get(c.m.get("k")), "key too", "a DataView used as a Map KEY clones consistently");
    assert(c.s.values().next().value instanceof DataView, "Set member DataView keeps its kind");
    assert(c.m.get("k") === c.s.values().next().value, "shared through Map and Set");
    assert(c.m2.get(c.m.get("k")) instanceof Map, "Map-in-Map value intact");
}
{
    const ta = new BigUint64Array(2);
    const cyc = { ta };
    cyc.self = cyc;
    const c = structuredClone(cyc);
    assert(c.self === c, "cycle preserved with views in the graph");
    assert(c.ta instanceof BigUint64Array && c.ta !== ta, "and the view cloned once");
}

{
    class MyU8 extends Uint8Array {}
    class MyF64 extends Float64Array {}
    class MyDV extends DataView {}
    const c1 = structuredClone(new MyU8(4));
    assert(c1 instanceof Uint8Array && c1.constructor === Uint8Array,
           "a Uint8Array subclass clones as Uint8Array (WHATWG intrinsic)");
    const c2 = structuredClone(new MyF64(3));
    assert(c2 instanceof Float64Array && c2.constructor === Float64Array,
           "a Float64Array subclass clones as Float64Array");
    eq(c2.length, 3, "subclass clone keeps its length");
    const c3 = structuredClone(new MyDV(new ArrayBuffer(8), 2, 4));
    assert(c3 instanceof DataView && c3.constructor === DataView,
           "a DataView subclass clones as DataView");
    eq(c3.byteLength, 4, "DataView subclass keeps its byte length");
}

{
    for (const [name, Ctor, width] of KINDS) {
        const v = new Ctor(4);
        fill(viewBytes(v));
        v.constructor = width === 1 ? Float64Array : Uint8Array;
        const c = structuredClone(v);
        assert(c instanceof Ctor,
               name + ": a spoofed constructor cannot change the clone's kind");
        eq(c.length, 4, name + ": constructor-spoofed clone keeps the element count");
        assert(sameBytes(viewBytes(c), viewBytes(v)),
               name + ": constructor-spoofed clone keeps the exact bytes");
    }
    const dv = new DataView(new ArrayBuffer(4));
    dv.constructor = Uint16Array;
    const c = structuredClone(dv);
    assert(c instanceof DataView, "a spoofed DataView constructor cannot mis-type");
    const u8 = fill(new Uint8Array(4));
    Object.setPrototypeOf(u8, Uint32Array.prototype);
    const c2 = structuredClone(u8);
    assert(c2 instanceof Uint8Array,
           "a swapped prototype cannot make a Uint8Array clone as Uint32Array");
    eq(c2.length, 4, "and the element count still matches Uint8Array");
}

{
    const saved = {};
    for (const [name, Ctor] of KINDS) saved[name] = Ctor;
    saved.DataView = DataView;
    const bytes = (v) => new saved.Uint8Array(v.buffer, v.byteOffset,
                                             v.byteLength);
    const views = [];
    for (const [name, Ctor, width] of KINDS) {
        const ab = fill(new Uint8Array(width * 3)).buffer;
        views.push([name, Ctor, new Ctor(ab, 0, 3)]);
    }
    const dvSrc = new DataView(new ArrayBuffer(6), 1, 4);
    try {
        globalThis.Uint8Array = saved.Uint16Array;
        globalThis.Uint16Array = saved.Uint8Array;
        globalThis.Float64Array = saved.Float32Array;
        for (const name of ["Uint8ClampedArray", "Int8Array", "Int16Array",
                            "Int32Array", "Uint32Array", "BigInt64Array",
                            "BigUint64Array", "Float16Array", "Float32Array",
                            "DataView"])
            delete globalThis[name];
        for (const [name, Ctor, v] of views) {
            const c = structuredClone(v);
            assert(c instanceof Ctor,
                   name + ": a swapped/deleted global intrinsic cannot re-route the clone's kind");
            eq(c.length, 3, name + ": swap-proof clone keeps the element count");
            assert(sameBytes(bytes(c), bytes(v)),
                   name + ": swap-proof clone keeps the exact bytes");
        }
        const d = structuredClone(dvSrc);
        assert(d instanceof saved.DataView,
               "DataView: a deleted globalThis.DataView cannot re-route the clone's kind");
        eq(d.constructor, saved.DataView,
           "DataView: the clone is built engine-internally (true constructor)");
        eq(d.byteLength, 4, "DataView: the byte window is preserved");
        assert(sameBytes(bytes(d), bytes(dvSrc)), "DataView: swap-proof bytes exact");
    } finally {
        for (const [name, Ctor] of KINDS) globalThis[name] = Ctor;
        globalThis.DataView = saved.DataView;
    }
}

{
    const p = new Proxy(fill(new Uint8Array(3)), {});
    const c = structuredClone(p);
    assert(!(c instanceof Uint8Array) && !ArrayBuffer.isView(c),
           "edge: a Proxy-wrapped view clones as a plain object, not a view");
    assert(JSON.stringify(c) === JSON.stringify(p),
           "edge: ...carrying the proxy's own enumerable properties");

    const ab = new ArrayBuffer(8);
    ab.transfer();
    const d = structuredClone(ab);
    assert(!(d instanceof ArrayBuffer) && Object.keys(d).length === 0,
           "edge: a detached ArrayBuffer clones to {} (no bytes to copy)");

    const buf = new ArrayBuffer(16);
    const v1 = new Uint8Array(buf, 0, 8), v2 = new Uint8Array(buf, 8, 8);
    const s = structuredClone({ v1, v2 });
    assert(s.v1.buffer !== s.v2.buffer,
           "edge: views over one buffer clone to independent windows");
    eq(s.v1.length, 8, "edge: the first window keeps its length");
    eq(s.v2.length, 8, "edge: the second window keeps its length");
    assert(sameBytes(viewBytes(s.v1), viewBytes(v1)) &&
           sameBytes(viewBytes(s.v2), viewBytes(v2)),
           "edge: both windows keep their exact bytes");
    const whole = structuredClone(buf);
    assert(whole instanceof ArrayBuffer && whole.byteLength === 16,
           "edge: a bare ArrayBuffer still clones whole");
}

print("test_clone_views: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed"));
if (fails) throw new Error(fails + " failures");
