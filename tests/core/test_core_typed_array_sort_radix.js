// covers: c:src/builtins/builtin-typed-array.c
// %TypedArray%.prototype.sort with no comparator sorts by an LSD radix pass on
// order-preserving unsigned keys (signed ints: flip the sign bit; floats: flip
// all bits of negatives, the sign bit of positives) once the array reaches
// TA_RADIX_MIN8 / TA_RADIX_MIN / TA_RADIX_MIN64 elements (64 / 128 / 512 for
// 1 / 2-4 / 8-byte elements); NaNs are moved to the end first with their bit
// patterns intact. Below those sizes the generic rqsort stays faster (the 8 KiB
// histogram dominates). Modperf round 2026-11-15: Float64Array 100K 8.97 ms ->
// 1.46 ms, Int32Array 100K 6.53 -> 0.61 ms, Uint8Array 100K 2.36 -> 0.22 ms.
// The oracle is a plain JS comparator over Array.from, which never reaches the
// radix code: NaN last, -0 before +0, everything else by value.
import { suite } from "../lib/kit.js";

const t = suite("test_core_typed_array_sort_radix");

function refCmp(x, y) {
    if (typeof x === "bigint") return x < y ? -1 : x > y ? 1 : 0;
    const xn = Number.isNaN(x), yn = Number.isNaN(y);
    if (xn || yn) return xn && yn ? 0 : xn ? 1 : -1;
    if (x < y) return -1;
    if (x > y) return 1;
    if (x === 0 && y === 0) return (Object.is(y, -0) ? 1 : 0) - (Object.is(x, -0) ? 1 : 0);
    return 0;
}

let seed = 0x2545f491;
function rnd() {
    seed ^= seed << 13;
    seed ^= seed >>> 17;
    seed ^= seed << 5;
    return seed >>> 0;
}

const SIZES = [0, 1, 2, 63, 64, 65, 127, 128, 129, 511, 512, 513, 1000, 4099];
const INTS = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array];
const FLOATS = [Float32Array, Float64Array];
if (typeof Float16Array !== "undefined") FLOATS.push(Float16Array);
const BIGS = [BigInt64Array, BigUint64Array];

function fill(T, n, mode) {
    const a = new T(n);
    for (let i = 0; i < n; i++) {
        const r = rnd();
        if (BIGS.includes(T)) {
            const hi = BigInt(r) << 32n, lo = BigInt(rnd());
            a[i] = mode === "few" ? BigInt(r % 5) - 2n : T === BigInt64Array ? BigInt.asIntN(64, hi | lo) : hi | lo;
        } else if (FLOATS.includes(T)) {
            const sp = [NaN, -0, 0, Infinity, -Infinity, 5e-324, -5e-324, 1.5, -1.5, 65504, -65504];
            a[i] = mode === "few" ? (r % 5) - 2 : r % 7 === 0 ? sp[r % sp.length] : ((r | 0) / 977) * (r & 1 ? 1e-3 : 1e3);
        } else {
            a[i] = mode === "few" ? (r % 5) - 2 : r | 0;
        }
    }
    return a;
}

function check(eq, T, n, mode) {
    const a = fill(T, n, mode);
    const want = Array.from(a).sort(refCmp);
    const ret = a.sort();
    eq(ret === a, true, T.name + " " + n + " " + mode + " returns this");
    let bad = -1;
    for (let i = 0; i < n; i++)
        if (!Object.is(a[i], want[i])) {
            bad = i;
            break;
        }
    eq(bad, -1, T.name + " n=" + n + " " + mode + " first mismatch index");
}

t.test("integer arrays match the comparator oracle at every size", ({ eq }) => {
    for (const T of INTS) for (const n of SIZES) for (const mode of ["rand", "few"]) check(eq, T, n, mode);
});

t.test("float arrays: NaN last, -0 before +0, infinities and denormals ordered", ({ eq }) => {
    for (const T of FLOATS) for (const n of SIZES) for (const mode of ["rand", "few"]) check(eq, T, n, mode);
});

t.test("bigint arrays match the comparator oracle at every size", ({ eq }) => {
    for (const T of BIGS) for (const n of SIZES) for (const mode of ["rand", "few"]) check(eq, T, n, mode);
});

t.test("integer extremes sort by value, not by raw bits", ({ eq }) => {
    const cases = [
        [Int8Array, [127, -128, 0, -1, 1]],
        [Int16Array, [32767, -32768, 0, -1, 1]],
        [Int32Array, [2147483647, -2147483648, 0, -1, 1]],
        [Uint32Array, [4294967295, 0, 2147483648, 2147483647, 1]],
        [BigInt64Array, [9223372036854775807n, -9223372036854775808n, 0n, -1n, 1n]],
        [BigUint64Array, [18446744073709551615n, 0n, 9223372036854775808n, 9223372036854775807n, 1n]],
    ];
    for (const [T, vals] of cases) {
        const n = 600;
        const a = new T(n);
        for (let i = 0; i < n; i++) a[i] = vals[(i * 7) % vals.length];
        const want = Array.from(a).sort(refCmp);
        a.sort();
        eq(Array.from(a).join(","), want.join(","), T.name + " extremes");
    }
});

t.test("NaN bit patterns survive the sort and sit at the end", ({ eq }) => {
    const n = 700;
    const a = new Float64Array(n);
    const dv = new DataView(a.buffer);
    const payloads = [];
    for (let i = 0; i < n; i++) {
        if (i % 50 === 3) {
            const hi = ((i & 1 ? 0xfff80000 : 0x7ff80000) | (i & 0xffff)) >>> 0;
            dv.setUint32(i * 8 + 4, hi, true);
            dv.setUint32(i * 8, i * 2654435761, true);
            payloads.push(hi.toString(16) + ":" + ((i * 2654435761) >>> 0).toString(16));
        } else a[i] = (n - i) / 3 - 100;
    }
    a.sort();
    const got = [];
    for (let i = n - payloads.length; i < n; i++) got.push(dv.getUint32(i * 8 + 4, true).toString(16) + ":" + dv.getUint32(i * 8, true).toString(16));
    eq(got.sort().join(","), payloads.slice().sort().join(","), "NaN payloads kept");
    let ordered = true;
    for (let i = 1; i < n - payloads.length; i++) if (refCmp(a[i - 1], a[i]) > 0) ordered = false;
    eq(ordered, true, "numbers before the NaNs are ascending");
});

t.test("a comparator still takes the stable generic path", ({ eq }) => {
    const a = new Int32Array(1000);
    for (let i = 0; i < a.length; i++) a[i] = rnd() | 0;
    const want = Array.from(a).sort((x, y) => y - x);
    a.sort((x, y) => y - x);
    eq(Array.from(a).join(","), want.join(","), "descending comparator");
});

t.test("a subarray view sorts only its own window", ({ eq }) => {
    const base = new Int16Array(1200);
    for (let i = 0; i < base.length; i++) base[i] = (rnd() & 0xffff) - 32768;
    const before = Array.from(base);
    const view = base.subarray(100, 1100);
    const want = Array.from(view).sort(refCmp);
    view.sort();
    eq(Array.from(view).join(","), want.join(","), "window sorted");
    eq(Array.from(base.subarray(0, 100)).join(","), before.slice(0, 100).join(","), "prefix untouched");
    eq(Array.from(base.subarray(1100)).join(","), before.slice(1100).join(","), "suffix untouched");
});

await t.run();
