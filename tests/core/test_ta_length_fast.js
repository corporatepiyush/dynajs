// covers: c:src/builtins/builtin-typed-array.c
// Typed-array .length read by get_length without the getter call
// (js_ta_length_fast, src/object/property_get.inc.c; both interpreters).
// The fast path returns the element count only while the lookup would reach
// the intrinsic %TypedArray%.prototype.length getter: up to three ordinary
// prototypes, none owning "length", the third's accessor being the original
// C getter. Every other shape must take the full [[Get]]: an own "length" on
// the instance, a shadowing data or accessor on Float64Array.prototype or a
// subclass prototype, a redefined getter on %TypedArray%.prototype, a Proxy
// or null prototype, a deeper chain. Each case reads .length in a loop so the
// handler runs hot, and checks what the spec lookup returns.
// Proven by planting faults in js_ta_length_fast: accepting any C getter
// (no identity check), accepting any accessor, looking up only one level of
// the chain, and returning the byte length; each turns a line below red.
// The exotic-prototype refusal is defensive only: a Proxy has no shape
// properties and no shape prototype, and every other exotic class owns its
// "length", so dropping that check changes no result here or anywhere.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ta_length_fast");
let fails = 0;
function eq(name, got, want) {
    if (!Object.is(got, want)) {
        fails++;
        console.log("FAIL " + name + ": got " + String(got) + " want " + String(want));
    }
}
function lenSum(a, n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += a.length;
    return s;
}
function lenOnce(a) { return a.length; }

__kitS.section("function", () => {
const kinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
    Float32Array, Float64Array, BigInt64Array, BigUint64Array];
if (typeof Float16Array === "function") kinds.push(Float16Array);
for (const K of kinds) eq(K.name, lenSum(new K(7), 100), 700);
});

__kitS.section("view of buffer", () => {
const buf = new ArrayBuffer(64);
eq("subarray", lenSum(new Float64Array(buf, 8, 3), 10), 30);
eq("view of buffer", lenSum(new Int32Array(buf), 10), 160);
});

__kitS.section("tracking before", () => {
const rab = new ArrayBuffer(16, { maxByteLength: 64 });
const track = new Uint8Array(rab);
eq("tracking before", lenOnce(track), 16);
rab.resize(40);
eq("tracking grown", lenSum(track, 5), 200);
rab.resize(4);
eq("tracking shrunk", lenOnce(track), 4);
const fixed = new Uint8Array(rab, 0, 4);
rab.resize(2);
eq("fixed out of bounds", lenOnce(fixed), 0);
});

__kitS.section("detached", () => {
const dbuf = new ArrayBuffer(32);
const dta = new Float64Array(dbuf);
dbuf.transfer();
eq("detached", lenSum(dta, 10), 0);
});

class Sub extends Float64Array {}
eq("subclass", lenSum(new Sub(5), 10), 50);
class Sub2 extends Sub {}
eq("subclass depth 4", lenSum(new Sub2(6), 10), 60);

__kitS.section("own data length", () => {
const own = new Float64Array(4);
Object.defineProperty(own, "length", { value: 99 });
eq("own data length", lenSum(own, 10), 990);
const ownGet = new Float64Array(4);
Object.defineProperty(ownGet, "length", { get() { return 11; } });
eq("own accessor length", lenSum(ownGet, 10), 110);
});

class Shadow extends Float64Array {
    get length() { return 3 + super.length; }
}
eq("subclass getter", lenSum(new Shadow(2), 10), 50);

const TAProto = Object.getPrototypeOf(Float64Array.prototype);
const orig = Object.getOwnPropertyDescriptor(TAProto, "length");
const f = new Float64Array(8);
eq("intact before", lenSum(f, 10), 80);
Object.defineProperty(TAProto, "length", { get() { return 42; }, configurable: true });
eq("redefined getter", lenSum(f, 10), 420);
eq("redefined getter, Int8", lenSum(new Int8Array(1), 3), 126);
Object.defineProperty(TAProto, "length", Object.getOwnPropertyDescriptor(TAProto, "byteLength"));
eq("another C getter", lenSum(f, 10), 640);
Object.defineProperty(TAProto, "length", { value: 5, configurable: true, writable: true });
eq("redefined as data", lenSum(f, 10), 50);
delete TAProto.length;
eq("deleted getter", lenOnce(f), undefined);
Object.defineProperty(TAProto, "length", orig);
eq("restored", lenSum(f, 10), 80);

__kitS.section("shadow on Float64Array.prototype", () => {
Object.defineProperty(Float64Array.prototype, "length", { value: 7, configurable: true });
eq("shadow on Float64Array.prototype", lenSum(f, 10), 70);
eq("Int32Array unaffected", lenSum(new Int32Array(3), 10), 30);
delete Float64Array.prototype.length;
eq("shadow removed", lenSum(f, 10), 80);
});

__kitS.section("proxy prototype", () => {
const g = new Uint16Array(6);
let trapped = 0;
Object.setPrototypeOf(g, new Proxy(Uint16Array.prototype, {
    get(t, k, r) { if (k === "length") trapped++; return Reflect.get(t, k, r); },
}));
eq("proxy prototype", lenSum(g, 10), 60);
eq("proxy trap ran", trapped, 10);
});

__kitS.section("null prototype", () => {
const h = new Int16Array(3);
Object.setPrototypeOf(h, null);
eq("null prototype", lenOnce(h), undefined);
const k = new Int16Array(3);
Object.setPrototypeOf(k, { length: "plain" });
eq("plain prototype", lenOnce(k), "plain");
const deep = new Int16Array(9);
Object.setPrototypeOf(deep, Object.create(Object.create(Object.create(Int16Array.prototype))));
eq("deep chain", lenSum(deep, 10), 90);
});

__kitS.section("array", () => {
eq("array", lenSum([1, 2, 3], 10), 30);
eq("string", lenSum("abcd", 10), 40);
eq("dataview", lenOnce(new DataView(new ArrayBuffer(8))), undefined);
});
__kitS.end();

if (fails) throw new Error(fails + " typed-array length checks failed");
console.log("ok test_ta_length_fast");
