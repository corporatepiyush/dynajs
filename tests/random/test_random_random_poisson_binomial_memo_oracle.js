// covers: contract dyna:random.Random.poisson dyna:random.Random.binomial -- one per-thread memo shared by both samplers: draws must not depend on the memo
// poisson(lambda >= 30) inverts from the mode m = floor(lambda); the mode probability pm and the downward sum it
// starts from were recomputed (exp, log, lgamma and up to m terms) on every draw. binomial(n, p) recomputed its mode
// probability (four lgamma/log calls) likewise, then walked outward from the mode (one step up, one down, two
// divisions each) until the running sum passed u. Each thread now keeps the last lambda's (pm, sum) and, for the
// last (n, p) by exact bits, pm plus the first 512 walk steps (running sum and value): a draw whose u falls inside
// them is a binary search for the first sum above u, a draw past them resumes the walk from step 512 unchanged.
// Oracle: the memo must be invisible.
// Generator A draws a whole parameter sequence first (runs of one parameter let the memo and the step table grow
// across draws; 3000 draws of n = 1e6 and of n = 1e5 run past the 512 stored steps; binomial(30000, ~0.5) is
// redrawn from fresh tables until draws land exactly on the 512th stored step, seed 2026 found by search); generator B
// with the same seed then draws the same sequence while a third generator draws with other parameters (another n
// with the same p, the same n with another p) between every call, evicting the memo each time; A and B must give
// identical values. Plus values in range and sample means near the parameter means.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_poisson_binomial_memo_oracle");
import { Random } from "dyna:random";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; if (fails < 15) print("FAIL: " + what); } }
const lams = [30, 30.000000000000004, 31, 45.5, 99.9, 100, 500, 1e4, 0.5, 29.999, 2.5e5];
const bins = [[1e6, 0.5], [2e9, 0.3], [1, 0.5], [2, 0.999], [3, 1e-300], [20, 0.3], [21, 0.3], [20, 0.30000000000000004], [100, 0.5], [1000, 0.37], [100000, 0.4], [37, 0.9], [1e6, 0.999], [50, 0.02]];
__kitS.section("poisson: draws equal with the memo kept and with it evicted", () => {
    for (const [seed, order] of [[1, "runs"], [2, "alternate"], [3, "random"]]) {
        let x = seed * 7919;
        const pick = (i) => order === "runs" ? lams[(i / 50) | 0] : order === "alternate" ? lams[i % lams.length] : lams[(x = (x * 48271) % 2147483647) % lams.length];
        const seq = Array.from({ length: 550 }, (_, i) => pick(i));
        const a = new Random(seed), b = new Random(seed), noise = new Random(seed + 100);
        const va = seq.map((l) => a.poisson(l));
        let same = 0;
        for (const [i, l] of seq.entries()) {
            noise.poisson(l === 77.25 ? 31.5 : 77.25);
            noise.binomial(64, 0.25);
            const vb = b.poisson(l);
            if (va[i] === vb && Number.isInteger(vb) && vb >= 0) same++;
        }
        ok(same === seq.length, "poisson " + order + ": " + same + "/" + seq.length + " identical");
    }
});
__kitS.section("binomial: draws equal with the memo kept and with it evicted", () => {
    for (const seed of [4, 5, 6]) {
        const seq = seed === 6 ? Array.from({ length: 6000 }, (_, i) => bins[i < 3000 ? 0 : 10]) : Array.from({ length: 50 * bins.length }, (_, i) => bins[seed === 4 ? (i / 50) | 0 : i % bins.length]);
        const a = new Random(seed), b = new Random(seed), noise = new Random(seed + 100);
        const va = seq.map(([n, p]) => a.binomial(n, p));
        let same = 0;
        for (const [i, [n, p]] of seq.entries()) {
            noise.binomial(n + 1, p);
            noise.binomial(n, p / 2);
            noise.poisson(40.75);
            const vb = b.binomial(n, p);
            if (va[i] === vb && Number.isInteger(vb) && vb >= 0 && vb <= n) same++;
        }
        ok(same === seq.length, "binomial seed " + seed + ": " + same + "/" + seq.length + " identical");
    }
});
__kitS.section("binomial: the draw that stores the last table step returns that step", () => {
    const seq = [];
    for (let k = 0; k < 28; k++) for (let i = 0; i < 1500; i++) seq.push(0.5 + k * 1e-9);
    const a = new Random(2026), b = new Random(2026), noise = new Random(7);
    const va = seq.map((p) => a.binomial(30000, p));
    let same = 0;
    for (const [i, p] of seq.entries()) {
        noise.binomial(30001, p);
        if (b.binomial(30000, p) === va[i]) same++;
    }
    ok(same === seq.length, "binomial(30000, 0.5 + k 1e-9) table fill: " + same + "/" + seq.length + " identical");
});
__kitS.section("repeated parameters keep the right distribution", () => {
    const r = new Random(77);
    for (const l of [30, 64.5, 1000]) {
        let s = 0; const k = 20000;
        for (let i = 0; i < k; i++) s += r.poisson(l);
        ok(Math.abs(s / k - l) < 5 * Math.sqrt(l / k), "poisson mean " + l + ": " + s / k);
    }
    for (const [n, p] of [[40, 0.3], [5000, 0.37], [200, 0.95]]) {
        let s = 0; const k = 20000;
        for (let i = 0; i < k; i++) s += r.binomial(n, p);
        ok(Math.abs(s / k - n * p) < 5 * Math.sqrt(n * p * (1 - p) / k), "binomial mean " + n + "/" + p + ": " + s / k);
    }
});
print("poisson/binomial memo oracle: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failed");
__kitS.end();
