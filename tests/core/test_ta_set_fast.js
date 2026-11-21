// covers: c:src/builtins/builtin-typed-array.c
// TypedArray.prototype.set fast copy (js_typed_array_store_number,
// src/builtins/builtin-typed-array.c): a dense Array of numbers, or a typed
// array of another Number kind, is converted and stored element by element
// without the generic [[Get]]/[[Set]] per index; anything else (a hole, a
// string, an object, Float16, BigInt kinds) hands the rest of the copy to the
// generic loop at that index. The reference below is the spec's per-element
// assignment dst[offset + i] = src[i], run for every pair of kinds, every
// conversion edge (NaN, infinities, -0, 2^31, half-way clamping, values
// beyond 2^32) and offsets; a source that stops being numeric mid-copy (and
// whose valueOf mutates the source) must see exactly the generic order.
// Overlapping views of one buffer with different kinds copy from a clone of
// the source (SetTypedArrayFromTypedArray clones a same-buffer source); the
// old generic loop read the live bytes and saw its own writes.
// Proven by planting faults in the fast copy: truncating doubles for
// Uint8Clamped instead of rounding, skipping the ToInt32 wrap beyond 2^31,
// resuming the generic loop one index late after a dense Array's first
// non-number, and dropping the same-buffer clone; each turns a line below red.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ta_set_fast");
let fails = 0;
function eq(name, got, want) {
    const g = JSON.stringify(got), w = JSON.stringify(want);
    if (g !== w) {
        fails++;
        console.log("FAIL " + name + ": got " + g + " want " + w);
    }
}
const show = (ta) => Array.from(ta, (x) => typeof x === "bigint" ? String(x) + "n" : Object.is(x, -0) ? "-0" : String(x));
function reference(K, len, src, offset) {
    const d = new K(len);
    for (let i = 0; i < src.length; i++) d[offset + i] = src[i];
    return d;
}
const edge = [0, -0, 1, -1, 1.5, -1.5, 2.5, 254.5, 255.5, 256, -129, 32767.9, 65535.5, 2147483647, 2147483648,
    -2147483649, 4294967296 + 7, 1e20, -1e20, NaN, Infinity, -Infinity, 0.1, 3.4028235e38, 5e-324];
const numberKinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
    Float32Array, Float64Array];
if (typeof Float16Array === "function") numberKinds.push(Float16Array);
for (const D of numberKinds) {
    for (const off of [0, 3]) {
        const got = new D(edge.length + 3);
        for (let r = 0; r < 3; r++) got.set(edge, off);
        eq(D.name + " from Array off " + off, show(got), show(reference(D, edge.length + 3, edge, off)));
        for (const S of numberKinds) {
            const src = new S(edge.length);
            for (let i = 0; i < edge.length; i++) src[i] = edge[i];
            const got2 = new D(edge.length + 3);
            got2.set(src, off);
            eq(D.name + " from " + S.name + " off " + off, show(got2), show(reference(D, edge.length + 3, src, off)));
        }
    }
}
const ints = [];
for (let i = 0; i < 300; i++) ints.push(i * 7 - 500);
for (const D of numberKinds) {
    const g = new D(300);
    g.set(ints);
    eq(D.name + " int Array", show(g), show(reference(D, 300, ints, 0)));
}

__kitS.section("from", () => {
for (const B of [BigInt64Array, BigUint64Array]) {
    for (const S of [Int8Array, Float64Array]) {
        let threw = "";
        try { new B(2).set(new S(2)); } catch (e) { threw = e.constructor.name; }
        eq(B.name + " from " + S.name, threw, "TypeError");
        try { threw = ""; new S(2).set(new B(2)); } catch (e) { threw = e.constructor.name; }
        eq(S.name + " from " + B.name, threw, "TypeError");
    }
    const bb = new B(3);
    bb.set(new BigInt64Array([1n, -2n, 3n]));
    eq(B.name + " from BigInt64Array", show(bb), show(reference(B, 3, [1n, -2n, 3n], 0)));
}
});

__kitS.section("mixed Array", () => {
const log = [];
const mixed = [1, 2.5, "3", , { valueOf() { log.push("v"); mixed[5] = 99; return 4; } }, 5, 6];
const m = new Float64Array(7);
m.set(mixed);
eq("mixed Array", [show(m), log], [["1", "2.5", "3", "NaN", "4", "99", "6"], ["v"]]);
const log2 = [];
const dense = [1, 2.5, "3", { valueOf() { log2.push("v"); dense[4] = 77; return 4; } }, 5, 6];
const dm = new Int16Array(6);
dm.set(dense);
eq("dense mixed Array", [show(dm), log2], [["1", "2", "3", "4", "77", "6"], ["v"]]);
const proto = [10, , 30];
Object.defineProperty(Array.prototype, 1, { get() { return 20; }, configurable: true });
const ph = new Int32Array(3);
ph.set(proto);
delete Array.prototype[1];
eq("hole reads the prototype", show(ph), ["10", "20", "30"]);
});

__kitS.section("range error", () => {
let threw = "";
try { new Uint8Array(3).set([1, 2, 3], 1); } catch (e) { threw = e.constructor.name; }
eq("range error", threw, "RangeError");
const ro = new Uint8Array(4);
try { ro.set({ length: 2, 0: 7, 1: 8 }, 2); } catch (e) { threw = e; }
eq("array-like", show(ro), ["0", "0", "7", "8"]);
});

__kitS.section("overlap copies the source first", () => {
const buf = new ArrayBuffer(16);
const bytes = new Uint8Array(buf);
for (let i = 0; i < 16; i++) bytes[i] = i * 16 + 1;
const before = new Uint16Array(buf.slice(0, 12));
new Uint8Array(buf, 4).set(new Uint16Array(buf, 0, 6));
const want = [];
for (let i = 0; i < 16; i++) want.push(String(i < 4 || i >= 10 ? (i * 16 + 1) & 255 : before[i - 4] & 255));
eq("overlap copies the source first", show(bytes), want);
});
__kitS.end();

if (fails) throw new Error(fails + " TypedArray.set checks failed");
console.log("ok test_ta_set_fast");
