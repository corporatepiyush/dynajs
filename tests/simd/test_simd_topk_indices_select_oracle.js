// covers: dyna:simd.topkIndices -- candidate-buffer selection on 64-bit keys, then a radix sort of the k winners
// topkIndices maps each value to a 64-bit key (value bits flipped so that larger values sort first, -0 and +0 one
// key, every NaN one key after -Infinity, the index in the low 32 bits) and keeps a buffer of 2k candidates: blocks of
// 16 values are compared with the current k-th value at once and only the lanes that beat it are appended; a full
// buffer is cut back to its k smallest keys by quickselect. The k survivors are radix sorted (8-bit digits, digits
// that are equal in every key skipped; insertion sort up to 32). Memory: 16 bytes per k (was 4). The answer is the
// documented order, so the oracle is a JS sort: descending value, NaN last, equal values (incl. -0/+0) by lower
// index, over random, tied, signed-zero/NaN/Infinity/subnormal, ascending and descending inputs, unaligned views,
// and k from 0 to past n.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_topk_indices_select_oracle");
import { topkIndices } from "dyna:simd";

let checks = 0, fails = 0;
let seed = 777;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
function oracle(a, k) {
    const ix = Array.from({ length: a.length }, (_, i) => i);
    ix.sort((i, j) => {
        const x = a[i], y = a[j];
        const xn = x !== x, yn = y !== y;
        if (xn || yn) return xn && yn ? i - j : xn ? 1 : -1;
        if (x !== y) return x > y ? -1 : 1;
        return i - j;
    });
    return ix.slice(0, Math.min(k, a.length));
}
function check(a, k, what) {
    checks++;
    const got = Array.from(topkIndices(a, k)), want = oracle(a, k);
    if (got.length !== want.length || got.some((v, i) => v !== want[i])) {
        fails++;
        const p = got.findIndex((v, i) => v !== want[i]);
        if (fails < 15) print("FAIL: " + what + " n=" + a.length + " k=" + k + " at " + p + ": got " + got[p] + ", want " + want[p]);
    }
}
const specials = [NaN, -0, 0, Infinity, -Infinity, 1e-45, -1e-45, 1.17e-38, 3.4e38, -3.4e38, 1, -1, 2];
const gens = {
    random: (n) => Float32Array.from({ length: n }, () => rnd() * 200 - 100),
    ties: (n) => Float32Array.from({ length: n }, () => ((rnd() * 7) | 0) - 3),
    specials: (n) => Float32Array.from({ length: n }, () => specials[(rnd() * specials.length) | 0]),
    zerosNaN: (n) => Float32Array.from({ length: n }, () => { const r = rnd(); return r < 0.3 ? NaN : r < 0.5 ? -0 : r < 0.7 ? 0 : rnd() - 0.5; }),
    ascending: (n) => Float32Array.from({ length: n }, (_, i) => i * 0.5),
    descending: (n) => Float32Array.from({ length: n }, (_, i) => n - i),
    allNaN: (n) => new Float32Array(n).fill(NaN),
};
__kitS.section("every generator, every small and block-edge size", () => {
    for (const [g, f] of Object.entries(gens))
        for (const n of [1, 2, 3, 15, 16, 17, 31, 32, 33, 47, 64, 65, 100, 257])
            for (const k of [0, 1, 2, 5, 16, 17, 32, 33, n >> 1, n - 1, n, n + 3])
                if (k >= 0) check(f(n), k, g);
});
__kitS.section("large inputs: buffer refills, radix passes and unaligned views", () => {
    for (const [g, f] of Object.entries(gens))
        for (const n of [1000, 4099, 30011])
            for (const k of [1, 10, 100, 1000, 2047, n >> 1, n]) {
                const base = f(n + 3);
                check(base.subarray(1, n + 1), k, g + "/off1");
            }
});
__kitS.section("the k-th value moves late (ascending tail) and NaN-only heads", () => {
    const n = 20000;
    const a = Float32Array.from({ length: n }, (_, i) => (i < n / 2 ? NaN : i));
    for (const k of [1, 50, 5000, 15000]) check(a, k, "nanHead");
    const b = Float32Array.from({ length: n }, (_, i) => (i % 3 ? -0 : 0));
    for (const k of [1, 64, 6000]) check(b, k, "signedZeros");
});
if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_simd_topk_indices_select_oracle: " + checks + " checks passed");
__kitS.end();
