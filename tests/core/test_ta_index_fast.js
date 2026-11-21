// covers: c:src/builtins/builtin-typed-array.c
// Element access fast paths for typed arrays and integral float keys (the
// tail-call interpreter): get_array_el3 (the read half of a[i] op= v, a[i]++)
// reads typed arrays without the generic [[Get]], and get_array_el/el2/el3 and
// put_array_el turn a float64 key that is an integer in [0, 2^31) (Math.floor
// results, i * 0.5 * 2, -0) into an int key before their fast tiers. The
// computed-goto interpreter keeps its generic paths: the same tiers inside
// its one large function cost 6-13% more instructions on unrelated controls
// (graph-analytics, a plain counting loop), so it runs this file as the
// reference for the same answers. Every
// case checks what the spec lookup returns: the key a Proxy trap sees, holes
// and prototype getters behind an out-of-range index, non-integral, negative,
// NaN, infinite and >= 2^31 keys staying property names, out-of-bounds and
// detached typed-array read-modify-writes, clamping, BigInt and Float16
// elements. Loops run the handlers hot.
// Proven by planting faults in js_int_key_from_float (accepting a
// non-integral key) and in the el3 typed-array tier (reading the element as
// Float64 regardless of the array kind); each turns a line below red. The
// 2^31 bound is not observable (an out-of-range conversion never round-trips
// to the same double on the hosts we run); it is there because that
// conversion is undefined behaviour in C.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ta_index_fast");
let fails = 0;
function eq(name, got, want) {
    const g = typeof got === "object" ? JSON.stringify(got) : String(got);
    const w = typeof want === "object" ? JSON.stringify(want) : String(want);
    if (g !== w) {
        fails++;
        console.log("FAIL " + name + ": got " + g + " want " + w);
    }
}
const fl = (i) => Math.floor(i * 1.0000001 + 0.25);

const kinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
    Float32Array, Float64Array];
if (typeof Float16Array === "function") kinds.push(Float16Array);
for (const K of kinds) {
    const a = new K(16);
    for (let r = 0; r < 20; r++) {
        for (let i = 0; i < 16; i++) {
            a[i] += 3;
            a[fl(i)] += 1;
            a[i * 0.5 * 2]++;
        }
    }
    let s = 0;
    for (let i = 0; i < 16; i++) s += a[i * 1.0] + a[fl(i)];
    eq(K.name + " rmw", [a[0], a[15], s], [100, 100, 3200]);
}
const clamp = new Uint8ClampedArray(2);
for (let i = 0; i < 100; i++) { clamp[0] += 7; clamp[1.0] -= 7; }
eq("clamped", [clamp[0], clamp[1]], [255, 0]);
const u8 = new Uint8Array(1);
for (let i = 0; i < 100; i++) u8[0] += 7;
eq("uint8 wrap", u8[0], 700 % 256);
const big = new BigInt64Array(4);
for (let i = 0; i < 50; i++) { big[i & 3] += 2n; big[(i & 3) * 1.0]++; }
eq("bigint rmw", String(big[0]) + "," + String(big[3.0]), "39,36");
const bu = new BigUint64Array(1);
bu[0.0] -= 1n;
eq("biguint wrap", String(bu[0]), "18446744073709551615");

__kitS.section("oob rmw", () => {
const oob = new Float64Array(4);
let r1;
for (let i = 0; i < 10; i++) r1 = (oob[4] += 1);
eq("oob rmw", [r1, oob.length, Object.keys(oob).length], [NaN, 4, 4]);
const oobf = new Int32Array(4);
for (let i = 0; i < 10; i++) oobf[4.0]++;
eq("oob float key", [oobf[4], Object.keys(oobf).length], [undefined, 4]);
const det = new Float64Array(new ArrayBuffer(16));
det.buffer.transfer();
let r2;
for (let i = 0; i < 5; i++) r2 = (det[1] += 1);
eq("detached rmw", [r2, det[1], det.length], [NaN, undefined, 0]);
});

__kitS.section("array float keys", () => {
const arr = [10, 20, 30];
let as = 0;
for (let i = 0; i < 30; i++) as += arr[(i % 3) * 1.0];
for (let i = 0; i < 30; i++) arr[(i % 3) + 0.0] += 1;
eq("array float keys", [as, arr], [600, [20, 30, 40]]);
const grow = [];
for (let i = 0; i < 8; i++) grow[i * 1.0] = i;
eq("array append float key", grow, [0, 1, 2, 3, 4, 5, 6, 7]);
const holes = [1, , 3];
Object.defineProperty(Array.prototype, 4, { get() { return "proto4"; }, configurable: true });
eq("hole and proto getter", [holes[1.0], holes[4.0], holes[2.0]], [undefined, "proto4", 3]);
delete Array.prototype[4];
});

__kitS.section("object keys", () => {
const o = {};
for (let i = 0; i < 4; i++) {
    o[1.5] = "a";
    o[-1.0] = "neg";
    o[-0] = "zero";
    o[2147483648.0] = "big";
    o[4294967296.0] = "huge";
    o[NaN] = "nan";
    o[Infinity] = "inf";
    o[3.0] = "three";
}
eq("object keys", Object.keys(o).sort(), ["-1", "0", "1.5", "2147483648", "3", "4294967296", "Infinity", "NaN"].sort());
const ta = new Int16Array(4);
ta[1.5] = 9;
ta[-0] = 5;
ta[-1.0] = 7;
ta[2147483648.0] = 1;
eq("typed array non-index keys", [ta[0], ta[1.5], ta[-1], Object.keys(ta).length, ta[2147483648]], [5, undefined, undefined, 4, undefined]);
const a2 = [];
a2[2147483648.0] = "x";
a2[4294967295.0] = "y";
eq("array big float keys", [a2.length, a2[2147483648], a2["4294967295"]], [2147483649, "x", "y"]);
});

__kitS.section("set", () => {
const seen = [];
const px = new Proxy([1, 2, 3], {
    get(t, k, r) { seen.push(typeof k + ":" + String(k)); return Reflect.get(t, k, r); },
    set(t, k, v, r) { seen.push("set " + typeof k + ":" + String(k)); return Reflect.set(t, k, v, r); },
});
px[2.0];
px[-0] += 1;
px[1.0] = 5;
eq("proxy keys", seen, ["string:2", "string:0", "set string:0", "set string:1"]);
eq("proxy result", [...px], [2, 5, 3]);
});

__kitS.section("object key converted once per op", () => {
let calls = 0;
const keyObj = { valueOf() { calls++; return 1; }, toString() { calls++; return "1"; } };
const ka = new Float64Array(3);
for (let i = 0; i < 5; i++) ka[keyObj] += 2;
eq("object key converted once per op", [ka[1], calls], [10, 5]);
});
__kitS.end();

if (fails) throw new Error(fails + " element fast-path checks failed");
console.log("ok test_ta_index_fast");
