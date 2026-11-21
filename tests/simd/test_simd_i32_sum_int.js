// covers: dyna:simd.i32Sum
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_i32_sum_int");
import {
    i32Sum, i32Min, i32Max, i32Dot, i32Add, i32Mul, i32Scale, cumsum, cummax,
} from "dyna:simd";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

const SIZES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32, 33, 100, 1000];

const geni = (len, seed) =>
    Int32Array.from({ length: len }, (_, i) => (((i * 2654435761 + seed * 40503) >>> 0) % 2000001) - 1000000);
const genf = (len, seed) =>
    Float32Array.from({ length: len }, (_, i) => ((i * 7 + seed) % 2001) - 1000);

assert(i32Sum(new Int32Array(0)) === 0, "i32Sum empty = 0");

assert(i32Sum(Int32Array.of(2147483647, 2147483647, 2147483647)) === 6442450941, "i32Sum > INT32_MAX");

assert(i32Sum(Int32Array.of(-2147483648, -2147483648)) === -4294967296, "i32Sum negatives");

function throws(fn) { try { fn(); return false; } catch { return true; } }

__kitS.section("i32Dot length mismatch throws", () => {
    assert(throws(() => i32Dot(new Int32Array(3), new Int32Array(4))), "i32Dot length mismatch throws");
    assert(throws(() => i32Add(new Int32Array(3), new Int32Array(3), new Int32Array(4))), "i32Add length mismatch throws");
    assert(throws(() => i32Sum(new Float32Array(4))), "i32Sum on Float32Array throws (strict type)");
    assert(throws(() => i32Sum(new Uint32Array(4))), "i32Sum on Uint32Array throws (strict type)");
    assert(throws(() => i32Sum(new Int16Array(4))), "i32Sum on Int16Array throws");
    assert(throws(() => i32Sum([1, 2, 3])), "i32Sum on a plain array throws");
    assert(throws(() => cumsum(new BigInt64Array(4))), "cumsum on BigInt64Array throws (8 bytes per element, not a Float64Array)");
    assert(throws(() => cumsum(new Uint32Array(4))), "cumsum on Uint32Array throws");
    assert(throws(() => cummax([1, 2, 3])), "cummax on a plain array throws");
});
__kitS.end();

print("test_simd_int: all tests passed (" + n + " assertions)");
