// covers: dyna:simd.topkIndices
// topkIndices returns its k indices best first: descending value, NaN after every number, equal values by
// lower index. It used to return them in heap order ("unspecified"), and callers that took [0] as the best
// match (a realistic vector-search harness, perf observations O14/O34) silently got another element.
// Every case is checked against a reference: a stable sort of all indices by the same rule, cut to k.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_topk_indices_order");
import * as simd from "dyna:simd";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
function ref(v, k) {
    const idx = Array.from(v, (_, i) => i);
    idx.sort((a, b) => {
        const x = v[a], y = v[b], xn = x !== x, yn = y !== y;
        if (xn !== yn) return xn ? 1 : -1;
        if (!xn && x !== y) return x > y ? -1 : 1;
        return a - b;
    });
    return idx.slice(0, Math.min(k, v.length));
}
function same(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
let seed = 7;
const rnd = () => { seed = (seed * 1103515245 + 12345) >>> 0; return seed / 4294967296; };

__kitS.section("top 3 best first:", () => {
    const v = new Float32Array([0.1, 0.9, 0.5, 0.7, 0.3]);
    const r = simd.topkIndices(v, 3);
    check(r instanceof Uint32Array && same(Array.from(r), [1, 3, 2]), "top 3 best first: " + Array.from(r));
    check(same(Array.from(simd.topkIndices(v, 1)), [1]), "k = 1 is the arg max");
    check(simd.topkIndices(v, 0).length === 0, "k = 0 is empty");
    check(same(Array.from(simd.topkIndices(v, 9)), [1, 3, 2, 4, 0]), "k > n sorts every index");
});

__kitS.section("Infinity first, ties by index, -Infinity before NaN, NaN last", () => {
    const v = new Float32Array([2, NaN, 5, 5, -Infinity, NaN, 5, Infinity]);
    check(same(Array.from(simd.topkIndices(v, 8)), [7, 2, 3, 6, 0, 4, 1, 5]), "Infinity first, ties by index, -Infinity before NaN, NaN last");
    check(same(Array.from(simd.topkIndices(v, 3)), [7, 2, 3]), "a cut inside a run of equal values keeps the lower indices");
});

for (let t = 0; t < 300; t++) {
    const len = 1 + Math.floor(rnd() * 400), k = Math.floor(rnd() * (len + 3));
    const v = new Float32Array(len);
    const levels = 1 + Math.floor(rnd() * 20);
    for (let i = 0; i < len; i++) v[i] = rnd() < 0.05 ? NaN : Math.floor(rnd() * levels) - levels / 2;
    const got = Array.from(simd.topkIndices(v, k)), want = ref(v, k);
    if (!same(got, want)) { check(false, "random " + t + " len " + len + " k " + k + ": " + got.slice(0, 8) + " vs " + want.slice(0, 8)); break; }
}

check(true, "300 random arrays with ties and NaN match the reference order");

__kitS.section("64Ki values: [0] is the maximum and values descend", () => {
    const big = new Float32Array(1 << 16);
    for (let i = 0; i < big.length; i++) big[i] = rnd();
    const r = simd.topkIndices(big, 10);
    let ok = true;
    for (let i = 1; i < r.length; i++) if (big[r[i - 1]] < big[r[i]]) ok = false;
    let mx = -Infinity; for (const x of big) if (x > mx) mx = x;
    check(ok && big[r[0]] === mx, "64Ki values: [0] is the maximum and values descend");
});
__kitS.end();
print("test_simd_topk_order: " + n + " checks, " + fails + " failed");
if (fails) throw new Error("test_simd_topk_order: " + fails + " failures");
