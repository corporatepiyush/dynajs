// covers: contract c:src/builtins/builtin-typed-array.c c:src/builtins/builtin-iterator.c -- D04 st1i: the engine numeric fast paths (Iterator.range start, TypedArray set fast path, JS_GetViewInfo) whose JS_To* result was ignored
// st1i: Iterator.range read start with an unchecked JS_ToFloat64, the typed-array set() fast path
// (js_typed_array_store_number) an unchecked JS_ToInt32Free/JS_ToUint8ClampFree, and JS_GetViewInfo (Blob, WritableStream
// bytes) unchecked byteOffset/byteLength conversions. Every operand is already a number there, so the results are now
// checked (a failure takes the generic path or throws) and this suite pins the values each path produces.
// Oracle: ToInt32/ToUint8Clamp per ECMA-262 7.1.6/7.1.12 computed in JS, and the bytes a view covers.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_api_engine_numeric_paths");

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
const same = (a, b) => JSON.stringify(Array.from(a)) === JSON.stringify(Array.from(b));

__kitS.section("Iterator.range with fractional and edge numbers", () => {
    ok(same(Iterator.range(0.5, 3), [0.5, 1.5, 2.5]), "range(0.5, 3)");
    ok(same(Iterator.range(-0.25, 1, 0.5), [-0.25, 0.25, 0.75]), "range(-0.25, 1, 0.5)");
    let e;
    try { Iterator.range(NaN, 1); } catch (x) { e = x; }
    ok(e instanceof RangeError, "range(NaN, 1) RangeError");
});

__kitS.section("TypedArray#set fast path wraps and clamps like ToInt32/ToUint8Clamp", () => {
    const src = [300.7, -(2 ** 40) - 1.5, 2 ** 31 + 0.5, -(2 ** 31) - 7.25, 1e300, -1e300, 4294967296.9, -0.5];
    const toI32 = (d) => (Number.isFinite(d) ? Math.trunc(d) : 0) | 0;
    for (const [C, conv] of [[Int8Array, (d) => (toI32(d) << 24) >> 24], [Uint8Array, (d) => toI32(d) & 255], [Int16Array, (d) => (toI32(d) << 16) >> 16], [Uint16Array, (d) => toI32(d) & 65535], [Int32Array, toI32], [Uint32Array, (d) => toI32(d) >>> 0]]) {
        const t = new C(src.length);
        t.set(src);
        ok(same(t, src.map(conv)), C.name + ".set(doubles): " + JSON.stringify(Array.from(t)));
    }
    const u = new Uint8ClampedArray(8);
    u.set([1.5, 2.5, 300.2, -3.7, 0.5, 254.5, 255.5, 1e-9]);
    ok(same(u, [2, 2, 255, 0, 0, 254, 255, 0]), "Uint8ClampedArray.set rounds half to even and clamps: " + JSON.stringify(Array.from(u)));
});

await __kitS.section("JS_GetViewInfo reports the view's own window", async () => {
    const buf = new Uint8Array([10, 11, 12, 13, 14, 15, 16, 17]).buffer;
    const b1 = new Blob([new DataView(buf, 2, 3)]);
    ok(b1.size === 3 && same(new Uint8Array(await b1.arrayBuffer()), [12, 13, 14]), "Blob(DataView(2,3))");
    const b2 = new Blob([new Uint16Array(buf, 4, 2)]);
    ok(b2.size === 4 && same(new Uint8Array(await b2.arrayBuffer()), [14, 15, 16, 17]), "Blob(Uint16Array(4,2))");
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_api_engine_numeric_paths: " + n + " checks passed");
