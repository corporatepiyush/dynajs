// covers: dyna:mathx.stats
// The stats argsort (rankData, spearman, mannWhitneyU, ks2Samp, median/quantile helpers, pAdjust, bootstrap) is a
// stable LSD radix sort on the order-preserving bits of each value. Below 32768 values it now uses eight 8-bit passes
// (8 KiB of counters, passes whose byte is uniform skipped) instead of four 16-bit passes that clear and prefix-sum
// 256 KiB of counters each; above it keeps the 16-bit passes. Both are stable on (key, index), so every rank is the
// same. Oracle: a JS stable sort with numeric < (-0 equals +0, as in scipy.stats.rankdata) gives the ordinal, min, max, dense and average
// ranks for sizes around both thresholds (63/64, 32767/32768) with ties, signed zeros, infinities, subnormals, and
// small integers whose keys vary in three bytes only (an odd number of passes ends in the scratch buffer).
// Signed zeros (F-mathx-1): every size treats -0 and +0 as equal and keeps index order; the radix key maps -0 to +0,
// so ordinal ranks no longer change between 63 and 64 values. Pinned below by the 63/64 signed-zero section.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_stats_rank_data_radix8_oracle");
import { stats } from "dyna:mathx";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
let seed = 4242;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const keyLess = (a, b) => a < b;
function refRanks(x) {
    const n = x.length, idx = Array.from({ length: n }, (_, i) => i);
    idx.sort((i, j) => (keyLess(x[i], x[j]) ? -1 : keyLess(x[j], x[i]) ? 1 : i - j));
    const r = { ordinal: new Float64Array(n), min: new Float64Array(n), max: new Float64Array(n), dense: new Float64Array(n), average: new Float64Array(n) };
    let i = 0, dense = 0;
    while (i < n) {
        let j = i;
        while (j + 1 < n && x[idx[j + 1]] === x[idx[i]]) j++;
        dense++;
        for (let k = i; k <= j; k++) { r.ordinal[idx[k]] = k + 1; r.min[idx[k]] = i + 1; r.max[idx[k]] = j + 1; r.dense[idx[k]] = dense; r.average[idx[k]] = 0.5 * (i + j) + 1; }
        i = j + 1;
    }
    return r;
}
function gen(n, kind) {
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) {
        const u = rnd();
        if (kind === "ties") x[i] = Math.floor(u * 17) - 8;
        else if (kind === "zeros") x[i] = u < 0.3 ? -0 : u < 0.6 ? 0 : u < 0.7 ? -Infinity : u < 0.8 ? Infinity : u < 0.9 ? 5e-324 * (i % 7) * (i % 2 ? -1 : 1) : (u - 0.95) * 1e300;
        else if (kind === "int12") x[i] = Math.floor(u * 4000) + 1;
        else if (kind === "wide") x[i] = (u - 0.5) * Math.pow(2, (i % 200) - 100);
        else x[i] = u * 2 - 1;
    }
    return x;
}

__kitS.section("ranks equal a stable JS sort across both radix widths", () => {
    for (const n of [1, 2, 63, 64, 65, 1000, 32767, 32768, 40000]) {
        for (const kind of ["cont", "ties", "zeros", "wide", "int12"]) {
            if (n > 1000 && kind === "wide") continue;
            const x = gen(n, kind), ref = refRanks(x);
            for (const method of ["ordinal", "min", "max", "dense", "average"]) {
                const got = stats.rankData(x, { method });
                let bad = 0, first = -1;
                for (let i = 0; i < n; i++) if (got[i] !== ref[method][i]) { bad++; if (first < 0) first = i; }
                eq(bad, 0, "n=" + n + " " + kind + " " + method + " mismatches (first " + first + ")");
            }
        }
    }
});

__kitS.section("signed zeros keep index order at 63, 64 and 40000 values (scipy rankdata ordinal)", () => {
    for (const n of [4, 63, 64, 40000]) {
        const x = new Float64Array(n);
        for (let i = 0; i < n; i++) x[i] = i % 2 ? 0 : -0;
        const got = stats.rankData(x, { method: "ordinal" });
        let bad = 0;
        for (let i = 0; i < n; i++) if (got[i] !== i + 1) bad++;
        eq(bad, 0, "n=" + n + " alternating -0/+0 ordinal ranks out of index order");
        const avg = stats.rankData(x, { method: "average" });
        eq(avg[0], (n + 1) / 2, "n=" + n + " all zeros tie in average rank");
    }
});

__kitS.section("rank-based tests agree with ranks computed in JS", () => {
    for (const n of [80, 5000]) {
        const a = gen(n, "ties"), b = gen(n, "cont");
        const ra = refRanks(a).average, rb = refRanks(b).average;
        let ma = 0, mb = 0;
        for (let i = 0; i < n; i++) { ma += ra[i]; mb += rb[i]; }
        ma /= n; mb /= n;
        let sab = 0, saa = 0, sbb = 0;
        for (let i = 0; i < n; i++) { sab += (ra[i] - ma) * (rb[i] - mb); saa += (ra[i] - ma) ** 2; sbb += (rb[i] - mb) ** 2; }
        const rho = sab / Math.sqrt(saa * sbb), got = stats.spearman(a, b).statistic;
        eq(Math.abs(got - rho) <= 1e-12 * Math.max(1, Math.abs(rho)), true, "n=" + n + " spearman " + got + " vs " + rho);
        const sorted = Float64Array.from(a).sort();
        eq(stats.median(a), n % 2 ? sorted[(n - 1) / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2, "n=" + n + " median");
    }
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_mathx_stats_rank_data_radix8_oracle: " + checks + " checks passed");
__kitS.end();
