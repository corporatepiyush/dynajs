// covers: dyna:random.Random.shuffle
// A typed array is shuffled in its buffer. The element swap used to be a generic byte loop of the element size; it
// is now a fixed-size swap per element size (1, 2, 4, 8 bytes) with the same draws in the same order (one
// nextBounded(i + 1) for i = n-1 down to 1, no swap when j == i). Oracle: an independent Fisher-Yates in JS driven
// by a second Random with the same seed through nextBounded; every typed-array kind (all four element sizes, plus
// BigInt and Float16 where present), lengths 0..5 and around 64/1000, views at element offsets 0/1/3 inside a larger
// buffer whose bytes outside the view must stay untouched, NaN and -0 payloads kept bit for bit.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_shuffle_typed_swap_oracle");
import { Random } from "dyna:random";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; if (fails < 15) print("FAIL: " + what); } }
const kinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array];
if (typeof Float16Array === "function") kinds.push(Float16Array);
const lengths = [0, 1, 2, 3, 4, 5, 63, 64, 65, 1000, 1001];
function jsShuffle(rng, a) {
    const p = Array.from({ length: a.length }, (_, i) => i);
    for (let i = a.length - 1; i >= 1; i--) {
        const j = rng.nextBounded(i + 1);
        const t = p[i]; p[i] = p[j]; p[j] = t;
    }
    return p;
}
__kitS.section("every element size equals a JS Fisher-Yates over nextBounded", () => {
    let seed = 11;
    for (const C of kinds) for (const n of lengths) for (const off of [0, 1, 3]) {
        const bpe = C.BYTES_PER_ELEMENT, buf = new ArrayBuffer((n + off + 2) * bpe);
        const bytes = new Uint8Array(buf);
        for (let i = 0; i < bytes.length; i++) bytes[i] = (i * 37 + 5) & 255;
        const a = new C(buf, off * bpe, n);
        const before = new Uint8Array(bytes);
        const s = seed++;
        new Random(s).shuffle(a);
        const perm = jsShuffle(new Random(s), a);
        let same = true;
        for (let i = 0; i < n && same; i++) for (let b = 0; b < bpe; b++) if (bytes[(off + i) * bpe + b] !== before[(off + perm[i]) * bpe + b]) { same = false; break; }
        ok(same, C.name + " n=" + n + " off=" + off + ": bytes follow the JS permutation");
        let outside = true;
        for (let i = 0; i < off * bpe; i++) if (bytes[i] !== before[i]) outside = false;
        for (let i = (off + n) * bpe; i < bytes.length; i++) if (bytes[i] !== before[i]) outside = false;
        ok(outside, C.name + " n=" + n + " off=" + off + ": bytes outside the view untouched");
    }
});
__kitS.section("special float payloads move bit for bit", () => {
    const f64 = Float64Array.from([NaN, -0, 0, Infinity, -Infinity, 5e-324, -1.5, 2 ** 53]);
    const u = new BigUint64Array(f64.buffer);
    u[0] = 0x7ff8dead0000beefn;
    const bits = Array.from(u);
    new Random(5).shuffle(f64);
    const perm = jsShuffle(new Random(5), f64);
    ok(Array.from(u).every((v, i) => v === bits[perm[i]]), "Float64 bits follow the permutation");
    const f32 = Float32Array.from([NaN, -0, 1e-45, -3]);
    const u32 = new Uint32Array(f32.buffer);
    u32[0] = 0x7fc0beef;
    const b32 = Array.from(u32);
    new Random(6).shuffle(f32);
    const p32 = jsShuffle(new Random(6), f32);
    ok(Array.from(u32).every((v, i) => v === b32[p32[i]]), "Float32 bits follow the permutation");
});
__kitS.section("the same seed gives the same permutation for every element size", () => {
    for (const n of [2, 17, 1000]) {
        const ref = Array.from(new Random(99).shuffle(Uint8Array.from({ length: n }, (_, i) => i & 255)));
        for (const C of [Uint16Array, Uint32Array, Float64Array]) {
            const got = Array.from(new Random(99).shuffle(C.from({ length: n }, (_, i) => i & 255)));
            ok(got.every((v, i) => v === ref[i]), C.name + " n=" + n + " matches Uint8Array");
        }
    }
});
print("shuffle typed swap oracle: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failed");
__kitS.end();
