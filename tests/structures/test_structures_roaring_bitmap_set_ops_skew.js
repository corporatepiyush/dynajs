// covers: dyna:structures.RoaringBitmap
// P11 container kernels against a JS Set. Two sorted-array containers whose sizes differ by 6x or more (for "and"
// where the 8-wide block kernel exists, 128+ values up to 16x take it instead, see test_structures_roaring_bitmap_set_ops_simd.js) intersect by
// galloping (exponential then binary search of the larger for each element of the smaller) instead of a linear merge,
// for and/andNot and their cardinalities, with either side the smaller; an array container
// filtered by a bitset container runs branch-free; bitset x bitset runs one loop per operation (no per-word
// operation select) so the word loop and its popcount vectorize. Sizes straddle the 6x switch (5x, 6x, 7x, 64x,
// 1 vs 4096), the array/bitset boundary (4096/4097), empty and full (65536) containers, and keys spread over
// several containers with gaps, so every container pairing meets every operation in both the in-place and the
// cardinality-only form. The last case pins the speed as CPU ratios inside one run (no wall clock): on the
// product binary, best of 5, us per call base -> new: 40-vs-4000 and 570 -> 51 (or unchanged 535), 2000-in-30000
// and 544 -> 51, bitset and-cardinality 16.2 -> 3.6, bitset and 20.8 -> 8.2; balanced arrays unchanged (847 -> 845).
import { suite } from "../lib/kit.js";
import { RoaringBitmap } from "dyna:structures";
import { Random } from "dyna:random";
import { threadCpuNow } from "dyna:sys";

const t = suite("structures.RoaringBitmap set operations: galloping, branch-free and per-op word kernels");
const rng = new Random(20261120);

function sample(n, hi, base) {
    const s = new Set();
    while (s.size < n) s.add(base + rng.nextBounded(hi));
    return s;
}
function shape(spec) {
    const s = new Set();
    for (const [key, n] of spec) {
        if (n === 65536) for (let i = 0; i < 65536; i++) s.add(key * 65536 + i);
        else for (const v of sample(n, 65536, key * 65536)) s.add(v);
    }
    return s;
}
const ref = {
    and: (a, b) => new Set([...a].filter((x) => b.has(x))),
    or: (a, b) => new Set([...a, ...b]),
    xor: (a, b) => new Set([...[...a].filter((x) => !b.has(x)), ...[...b].filter((x) => !a.has(x))]),
    andNot: (a, b) => new Set([...a].filter((x) => !b.has(x))),
};
const sorted = (s) => Uint32Array.from([...s].sort((x, y) => x - y));
function check(a, label, A, B) {
    const ra = new RoaringBitmap(sorted(A)), rb = new RoaringBitmap(sorted(B));
    let bad = 0;
    for (const op of ["and", "or", "xor", "andNot"]) {
        const want = ref[op](A, B);
        const got = ra.clone()[op](rb).toArray();
        const w = sorted(want);
        if (got.length !== w.length || got.some((v, i) => v !== w[i])) {
            if (bad++ < 3) a.ok(false, `${label} ${op}: ${got.length} values, want ${w.length}`);
        }
        const card = ra[op + "Cardinality"](rb);
        if (card !== want.size && bad++ < 3) a.ok(false, `${label} ${op}Cardinality ${card}, want ${want.size}`);
    }
    a.eq(bad, 0, label);
}
const PAIRS = [
    ["1 vs 4096", [[0, 1]], [[0, 4096]]],
    ["40 vs 4000", [[0, 40]], [[0, 4000]]],
    ["5x", [[0, 700]], [[0, 3500]]],
    ["6x", [[0, 600]], [[0, 3600]]],
    ["7x", [[0, 500]], [[0, 3500]]],
    ["64x", [[0, 60]], [[0, 3840]]],
    ["balanced", [[0, 3000]], [[0, 3000]]],
    ["array vs bitset", [[0, 2000]], [[0, 30000]]],
    ["array vs 4097", [[0, 300]], [[0, 4097]]],
    ["bitset vs bitset", [[0, 40000]], [[0, 30000]]],
    ["bitset vs full", [[0, 20000]], [[0, 65536]]],
    ["array vs full", [[0, 100]], [[0, 65536]]],
    ["empty vs array", [], [[0, 1000]]],
    ["multi-container with gaps", [[0, 50], [1, 5000], [3, 65536], [7, 10]], [[0, 3000], [1, 200], [2, 77], [3, 300], [7, 4096]]],
];

t.test("each container pairing, in both argument orders, agrees with the Set reference for every operation", (a) => {
    for (const [label, x, y] of PAIRS) {
        const A = shape(x), B = shape(y);
        check(a, label, A, B);
        check(a, label + " (swapped)", B, A);
    }
});

t.test("galloping meets equal values at the ends and runs of misses in the larger side", (a) => {
    const big = new Set(Array.from({ length: 4000 }, (_, i) => i * 16));
    for (const small of [[0], [63984], [1], [63985], [0, 16, 32, 63984], [5, 6, 7, 8, 9, 10], [65535], [0, 65535]]) {
        const S = new Set(small);
        check(a, "edges " + small.join(","), S, big);
        check(a, "edges swapped " + small.join(","), big, S);
    }
});

t.test("random skew ratios from 1x to 200x across several containers", (a) => {
    for (let i = 0; i < 40; i++) {
        const nb = 200 + rng.nextBounded(3800), ratio = 1 + rng.nextBounded(200);
        const na = Math.max(1, Math.floor(nb / ratio));
        const keys = [0, 1 + rng.nextBounded(3)];
        check(a, `case ${i} ${na} vs ${nb}`, shape(keys.map((k) => [k, na])), shape(keys.map((k) => [k, nb])));
    }
});

t.test("a skewed intersection and an array-in-bitset intersection cost less than the union of the same pair", (a) => {
    const mk = (per, cts) => {
        const out = [];
        for (let c = 0; c < cts; c++) for (const v of sample(per, 65536, c * 65536)) out.push(v);
        return new RoaringBitmap(Uint32Array.from(out.sort((x, y) => x - y)));
    };
    const cpu = (fn) => {
        let b = Infinity;
        for (let r = 0; r < 7; r++) {
            const t0 = threadCpuNow();
            for (let i = 0; i < 15; i++) fn();
            b = Math.min(b, threadCpuNow() - t0);
        }
        return b;
    };
    const s1 = mk(40, 24), s2 = mk(4000, 24), b1 = mk(2000, 16), b2 = mk(30000, 16);
    const skew = cpu(() => s1.andCardinality(s2)) / cpu(() => s1.orCardinality(s2));
    const skew2 = cpu(() => s2.andCardinality(s1)) / cpu(() => s2.orCardinality(s1));
    const inbmp = cpu(() => b1.andCardinality(b2)) / cpu(() => b1.orCardinality(b2));
    a.ok(skew < 0.25, `40 vs 4000 per container: and costs ${skew.toFixed(2)}x or (a linear merge is about 1x)`);
    a.ok(skew2 < 0.25, `4000 vs 40 per container: and costs ${skew2.toFixed(2)}x or (the smaller side must drive the gallop)`);
    a.ok(inbmp < 1.5, `2000 in a 30000 bitset: and costs ${inbmp.toFixed(2)}x or (a branchy filter is about 3x)`);
});

await t.run();
