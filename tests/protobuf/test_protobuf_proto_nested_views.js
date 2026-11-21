// covers: c:src/modules/protobuf/protobuf-encode.c
// Proto.encode refuses a byte view (TypedArray or DataView) given as a message. It asked for the
// view's buffer on EVERY nested message, which threw and built a backtrace for each plain
// object and then cleared it (~85% of encoding 1K-100K nested messages); it now asks only when
// the value is a view. The oracle is a JS protobuf encoder for int32/string/nested/repeated
// nested fields (a field that is set is written, zero included): random nested messages must
// encode byte for byte the same; every TypedArray kind, a DataView and a subarray must still be
// refused as a message, nested or repeated; an ArrayBuffer (not a view) and a view over a shrunk
// resizable buffer encode as empty messages.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_protobuf_proto_nested_views");
import { Proto } from "dyna:protobuf";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; if (fail < 30) print("FAIL " + m + ": got " + a + ", want " + b); } };
let seed = 5;
const rnd = (n) => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return seed % n; };
const leafSchema = { fields: [{ name: "x", number: 1, type: "int32" }, { name: "s", number: 2, type: "string" }] };
const midSchema = { fields: [{ name: "x", number: 1, type: "int32" }, { name: "leaf", number: 2, type: "message", message: leafSchema }, { name: "leaves", number: 3, type: "message", repeated: true, message: leafSchema }] };
const topSchema = { fields: [{ name: "id", number: 1, type: "int32" }, { name: "mid", number: 2, type: "message", message: midSchema }, { name: "mids", number: 3, type: "message", repeated: true, message: midSchema }] };
const enc = new TextEncoder();
function varint(v) {
    let x = BigInt.asUintN(64, BigInt(v));
    const o = [];
    do { let b = Number(x & 127n); x >>= 7n; if (x) b |= 128; o.push(b); } while (x);
    return o;
}
const key = (n, w) => varint(n * 8 + w);
const ld = (n, b) => [...key(n, 2), ...varint(b.length), ...b];
function encJS(m, schema) {
    const o = [];
    for (const f of schema.fields) {
        const v = m[f.name];
        if (v === undefined || v === null) continue;
        const one = (x) => f.type === "int32" ? [...key(f.number, 0), ...varint(x)] : f.type === "string" ? ld(f.number, [...enc.encode(x)]) : ld(f.number, encJS(x, f.message));
        if (f.repeated) for (const x of v) o.push(...(f.type === "message" ? ld(f.number, encJS(x, f.message)) : one(x)));
        else o.push(...one(v));
    }
    return o;
}
const leaf = () => (rnd(3) ? { x: rnd(1000) - 500, s: "s" + rnd(99) } : {});
const mid = () => ({ x: rnd(50), leaf: rnd(2) ? leaf() : undefined, leaves: Array.from({ length: rnd(5) }, leaf) });
const res = (f) => { try { return Array.from(f()).join(","); } catch (e) { return "ERR:" + e.name + ":" + e.message; } };

__kitS.section("random nested messages against a JS encoder", () => {
    for (let i = 0; i < 300; i++) {
        const m = { id: rnd(100000), mid: rnd(2) ? mid() : undefined, mids: Array.from({ length: rnd(8) }, mid) };
        eq(res(() => Proto.encode(m, topSchema)), encJS(m, topSchema).join(","), "message #" + i);
    }
    const many = { id: 1, mids: Array.from({ length: 3000 }, mid) };
    eq(res(() => Proto.encode(many, topSchema)), encJS(many, topSchema).join(","), "3000 repeated nested messages");
});

__kitS.section("byte views are refused as messages", () => {
    const VIEW = "ERR:TypeError:encode: a byte view is a bytes value, not a message";
    const views = [new Uint8Array(2), new Uint8ClampedArray(1), new Int8Array(3), new Int16Array(1), new Uint16Array(1), new Int32Array(1), new Uint32Array(1),
        new Float32Array(1), new Float64Array(1), new BigInt64Array(1), new BigUint64Array(1), new DataView(new ArrayBuffer(4)), new Uint8Array(8).subarray(2, 5), new Uint8Array(0)];
    if (typeof Float16Array === "function") views.push(new Float16Array(1));
    for (const v of views) {
        const tag = Object.prototype.toString.call(v);
        eq(res(() => Proto.encode({ mid: v }, topSchema)), VIEW, tag + " as a message");
        eq(res(() => Proto.encode({ mids: [{}, v] }, topSchema)), VIEW, tag + " in a repeated message");
        eq(res(() => Proto.encode({ mid: { leaf: v } }, topSchema)), VIEW, tag + " as a nested message");
    }
    eq(res(() => Proto.encode({ mid: new ArrayBuffer(4) }, topSchema)), "18,0", "an ArrayBuffer is an empty message");
    const rab = new ArrayBuffer(8, { maxByteLength: 16 });
    const oob = new Uint8Array(rab, 4, 4);
    rab.resize(2);
    eq(res(() => Proto.encode({ mid: oob }, topSchema)), "18,0", "a view over a shrunk buffer is an empty message");
    eq(res(() => Proto.encode({ mid: [] }, topSchema)), "ERR:TypeError:encode: a message field cannot be an array", "an array is refused");
});

__kitS.end();
print(`test_serialize_proto_nested_views: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
