// covers: dyna:structures.RoaringBitmap
// P11: two sorted-array containers of similar size intersect 8 values at a time (NEON on arm64, SSE2 on x86-64, the
// scalar merge elsewhere): each block of 8 from one side is compared with all 8 rotations of a block from the other,
// the matches are compacted in order, and the block with the smaller last value advances (both on a tie); the scalar
// merge finishes the tails. With the block kernel, "and" still gallops from a 6x size ratio unless the smaller side
// holds 128+ values and the ratio is at most 16x (a first cut that moved the switch to 24x for every size lost
// 1.3-3.4x on small sides: 4 x 64, 7 x 150, 12 x 250, 40 x 800). Micro-bench, best of 5 interleaved, us per
// and-cardinality over 128 containers, scalar -> shipped: balanced 3000 x 3000 1624 -> 762, 1000 x 4000 1347 -> 544,
// 500 x 4000 879 -> 453, 250 x 4000 499 -> 430; 120 x 4000, 100 x 2000, 40 x 800 and smaller stay galloping (ties).
// Oracle: a JS Set over every pair of lengths 0..40 drawn from a narrow range (many matches, matches on block edges,
// a side running out mid-block), identical arrays, disjoint interleaved arrays, and random ratios 1x..40x around both
// switches, for and / andCardinality in both argument orders. The last case pins the speed as a CPU ratio inside one
// run: balanced and-cardinality against or-cardinality of the same pair (scalar merge 1.03-1.11x, blocks
// 0.43-0.48x; the bound is 0.6x).
import { suite } from "../lib/kit.js";
import { RoaringBitmap } from "dyna:structures";
import { Random } from "dyna:random";
import { threadCpuNow } from "dyna:sys";
import { perfCheck } from "../lib/perf.js";

const t = suite("structures.RoaringBitmap block intersection of array containers");
const rng = new Random(0x51d);
const sorted = (s) => Uint32Array.from([...s].sort((x, y) => x - y));
function pick(n, hi, base = 0) {
    const s = new Set();
    while (s.size < n) s.add(base + rng.nextBounded(hi));
    return s;
}
function check(a, label, A, B) {
    const ra = new RoaringBitmap(sorted(A)), rb = new RoaringBitmap(sorted(B));
    const want = sorted(new Set([...A].filter((x) => B.has(x))));
    for (const [x, y, tag] of [[ra, rb, ""], [rb, ra, " swapped"]]) {
        const got = x.clone().and(y).toArray();
        if (got.length !== want.length || got.some((v, i) => v !== want[i])) return a.ok(false, label + tag + ": and has " + got.length + " values, want " + want.length);
        if (x.andCardinality(y) !== want.length) return a.ok(false, label + tag + ": andCardinality " + x.andCardinality(y) + ", want " + want.length);
    }
    return a.ok(true);
}

t.test("every pair of lengths 0..40 from a narrow range", (a) => {
    for (let na = 0; na <= 40; na++)
        for (let nb = 0; nb <= 40; nb++)
            if (!check(a, na + " x " + nb, pick(na, 64), pick(nb, 64))) return;
});

t.test("identical, interleaved, block-edge and multi-container arrays", (a) => {
    for (const n of [8, 16, 17, 64, 1000, 4096]) {
        const s = pick(n, 65536);
        check(a, "identical " + n, s, new Set(s));
        const ev = new Set(Array.from({ length: n }, (_, i) => 2 * i)), od = new Set(Array.from({ length: n }, (_, i) => 2 * i + 1));
        check(a, "interleaved " + n, ev, od);
        const edges = new Set([...ev].filter((x, i) => i % 8 === 0 || i % 8 === 7));
        check(a, "block edges " + n, ev, new Set([...edges, ...od]));
    }
    const multi = (k) => { const s = new Set(); for (let c = 0; c < 5; c++) for (const v of pick(k, 65536, c * 65536)) s.add(v); return s; };
    check(a, "5 containers", multi(3000), multi(2900));
});

t.test("random ratios 1x..40x around both gallop switches", (a) => {
    for (let i = 0; i < 120; i++) {
        const nb = 100 + rng.nextBounded(3900), ratio = 1 + rng.nextBounded(40), na = Math.max(1, Math.floor(nb / ratio));
        const hi = 2048 + rng.nextBounded(63488);
        if (!check(a, `case ${i} ${na} vs ${nb} in ${hi}`, pick(na, hi), pick(nb, Math.max(hi, nb)))) return;
    }
});

t.test("balanced and-cardinality costs well under or-cardinality of the same pair", (a) => {
    const { skip } = a;
    const mk = () => { const out = []; for (let c = 0; c < 32; c++) for (const v of pick(3000, 65536, c * 65536)) out.push(v); return new RoaringBitmap(Uint32Array.from(out.sort((x, y) => x - y))); };
    const x = mk(), y = mk();
    const cpu = (fn) => {
        let b = Infinity;
        for (let r = 0; r < 7; r++) {
            const t0 = threadCpuNow();
            for (let i = 0; i < 20; i++) fn();
            b = Math.min(b, threadCpuNow() - t0);
        }
        return b;
    };
    const ratio = cpu(() => x.andCardinality(y)) / cpu(() => x.orCardinality(y));
    perfCheck(a.ok, ratio < 0.6, `and costs ${ratio.toFixed(2)}x or (the scalar merge is about 1.1x)`, skip);
});

await t.run();
