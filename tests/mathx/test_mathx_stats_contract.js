// covers: contract dyna:mathx.stats -- argument contract, definitional brute-force checks and bootstrap determinism of the stats tests
// Independent of scipy: exact Mann-Whitney and Kolmogorov-Smirnov p-values are checked by enumerating every split of the
// pooled sample, bootstrap against a BigInt re-implementation of its documented resampling stream.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_stats_contract");
import { stats } from "dyna:mathx";

let pass = 0, fail = 0;
function ok(c, msg) {
    if (c) pass++;
    else { fail++; print("FAIL " + msg); }
}
function throws(fn, type, re, msg) {
    try { fn(); ok(false, msg + " (did not throw)"); }
    catch (e) { ok(e instanceof type && re.test(e.message), `${msg} (${e.name}: ${e.message})`); }
}
async function rejects(fn, type, re, msg) {
    try { await fn(); ok(false, msg + " (did not reject)"); }
    catch (e) { ok(e instanceof type && re.test(e.message), `${msg} (${e.name}: ${e.message})`); }
}
const near = (a, b, tol = 1e-12) => Math.abs(a - b) <= tol * Math.max(1, Math.abs(b));

function* combos(n, k, start = 0, acc = []) {
    if (acc.length === k) { yield acc.slice(); return; }
    for (let i = start; i <= n - (k - acc.length); i++) { acc.push(i); yield* combos(n, k, i + 1, acc); acc.pop(); }
}
function u1Of(a, b) {
    let u = 0;
    for (const x of a) for (const y of b) u += x > y ? 1 : x === y ? 0.5 : 0;
    return u;
}
__kitS.section("exact greater p = P(U >= u) by enumeration", () => {
    const a = [0.3, 2.2, 5.1, 7.7], b = [1.1, 1.9, 3.4, 4.0, 9.5];
    const pooled = [...a, ...b], obs = u1Of(a, b), n1 = a.length, n2 = b.length;
    let ge = 0, le = 0, two = 0, total = 0;
    const far = Math.abs(obs - (n1 * n2) / 2);
    for (const idx of combos(pooled.length, n1)) {
        const s = new Set(idx), x = idx.map((i) => pooled[i]), y = pooled.filter((_, i) => !s.has(i));
        const u = u1Of(x, y);
        total++;
        if (u >= obs) ge++;
        if (u <= obs) le++;
        if (Math.abs(u - (n1 * n2) / 2) >= far) two++;
    }
    const r = stats.mannWhitneyU(a, b, { method: "exact" });
    ok(r.statistic === obs, `mannWhitneyU statistic is U1 = #(a > b) (${r.statistic} vs ${obs})`);
    ok(near(r.pvalue, Math.min(1, two / total)), `exact two-sided p equals the enumerated permutation p (${r.pvalue} vs ${two / total})`);
    ok(near(stats.mannWhitneyU(a, b, { method: "exact", alternative: "greater" }).pvalue, ge / total), "exact greater p = P(U >= u) by enumeration");
    ok(near(stats.mannWhitneyU(a, b, { method: "exact", alternative: "less" }).pvalue, le / total), "exact less p = P(U <= u) by enumeration");
});
function ksD(x, y) {
    let d = 0;
    for (const v of [...x, ...y]) {
        const fx = x.filter((t) => t <= v).length / x.length, fy = y.filter((t) => t <= v).length / y.length;
        d = Math.max(d, Math.abs(fx - fy));
    }
    return d;
}
for (const [a, b] of [[[0.1, 0.5, 2.5, 3.1], [1.0, 1.2, 1.4, 4.4, 5.0, 6.1]], [[1, 2, 3, 4, 5], [2.5, 6, 7, 8, 9]]]) {
    const pooled = [...a, ...b], obs = ksD(a, b), L = a.length * b.length;
    let hit = 0, total = 0;
    for (const idx of combos(pooled.length, a.length)) {
        const s = new Set(idx);
        const d = ksD(idx.map((i) => pooled[i]), pooled.filter((_, i) => !s.has(i)));
        total++;
        if (Math.round(d * L) >= Math.round(obs * L)) hit++;
    }
    const r = stats.ks2Samp(a, b, { method: "exact" });
    ok(near(r.statistic, obs, 1e-15), `ks2Samp D is the largest ECDF gap (${r.statistic} vs ${obs})`);
    ok(near(r.pvalue, hit / total, 1e-12), `exact two-sided KS p equals the enumerated permutation p (${r.pvalue} vs ${hit / total}), n=${a.length},${b.length}`);
}
for (const [fn, args, inv] of [["norm", [], "normPpf"], ["t", [7], "tPpf"], ["chi2", [4], "chi2Ppf"], ["f", [3, 11], "fPpf"]]) {
    for (const x of [0.2, 1, 2.5]) {
        const c = stats[fn + "Cdf"](x, ...args), s = stats[fn + "Sf"](x, ...args);
        ok(near(c + s, 1, 1e-14), `${fn}: cdf + sf = 1 at ${x}`);
        ok(near(stats[inv](c, ...args), x, 1e-10), `${fn}: ppf inverts cdf at ${x}`);
    }
}
__kitS.section("a large array (split across the worker pool) maps element-wise like the scalar form", () => {
    const xs = Float64Array.from({ length: 60000 }, (_, i) => (i - 30000) / 3000), v = stats.tSf(xs, 6.5);
    ok(v.length === xs.length && xs.every((x, i) => Object.is(v[i], stats.tSf(x, 6.5))), "a large array (split across the worker pool) maps element-wise like the scalar form");
});
ok(stats.normPpf(0) === -Infinity && stats.normPpf(1) === Infinity && Number.isNaN(stats.normPpf(1.5)), "normPpf: 0 -> -Inf, 1 -> Inf, outside [0,1] -> NaN");
ok(Number.isNaN(stats.tCdf(1, 0)) && Number.isNaN(stats.chi2Sf(1, -1)), "non-positive degrees of freedom give NaN");
ok(stats.binomCdf(2.7, 5, 0.3) === stats.binomCdf(2, 5, 0.3) && stats.binomPmf(2.5, 5, 0.3) === 0, "binomial k is floored by cdf/sf; pmf of a non-integer is 0");
__kitS.section("bh-adjusted p-values keep the order of the raw p-values", () => {
    const adj = stats.pAdjust([0.04, 0.001, 0.03, 0.2, 0.01], "bh"), holm = stats.pAdjust([0.04, 0.001, 0.03, 0.2, 0.01], "holm");
    const order = [1, 4, 2, 0, 3];
    ok(order.every((i, k) => k === 0 || adj[i] >= adj[order[k - 1]]), "bh-adjusted p-values keep the order of the raw p-values");
    ok([0.04, 0.001, 0.03, 0.2, 0.01].every((p, i) => holm[i] >= p && adj[i] >= p && holm[i] <= 1), "adjusted p-values are >= raw and <= 1");
    ok(stats.pAdjust([], "holm").length === 0, "pAdjust of an empty list is empty");
});
ok(stats.psiCounts([5, 5], [5, 5]) === 0 && stats.psi([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]) === 0, "psi of identical distributions is 0");
__kitS.section("a one-row table has 0 degrees of freedom, statistic 0 and p 1", () => {
    const r = stats.chi2Contingency([[3, 5, 7]]);
    ok(r.df === 0 && r.statistic === 0 && r.pvalue === 1, "a one-row table has 0 degrees of freedom, statistic 0 and p 1");
});

function splitmix(state) {
    state.s = (state.s + 0x9E3779B97F4A7C15n) & 0xFFFFFFFFFFFFFFFFn;
    let z = state.s;
    z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & 0xFFFFFFFFFFFFFFFFn;
    z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & 0xFFFFFFFFFFFFFFFFn;
    return z ^ (z >> 31n);
}
function median(v) {
    const s = v.slice().sort((p, q) => p - q), h = s.length >> 1;
    return s.length % 2 ? s[h] : (s[h - 1] + s[h]) / 2;
}
const mean = (v) => v.reduce((p, q) => p + q, 0) / v.length;
function bootRef(a, b, statistic, compare, R, level, seed) {
    const f = statistic === "median" ? median : mean, out = [];
    for (let r = 0; r < R; r++) {
        const st = { s: splitmix({ s: (BigInt(seed) + 0x9E3779B97F4A7C15n * BigInt(r)) & 0xFFFFFFFFFFFFFFFFn }) };
        const ra = a.map(() => a[Number((splitmix(st) * BigInt(a.length)) >> 64n)]);
        let v = f(ra);
        if (b) {
            const rb = b.map(() => b[Number((splitmix(st) * BigInt(b.length)) >> 64n)]);
            v = compare === "ratio" ? v / f(rb) : v - f(rb);
        }
        out.push(v);
    }
    out.sort((p, q) => p - q);
    const pct = (q) => { const pos = q * (R - 1), k = Math.floor(pos); return k + 1 >= R ? out[R - 1] : out[k] + (pos - k) * (out[k + 1] - out[k]); };
    return { low: pct((1 - level) / 2), high: pct((1 + level) / 2) };
}
await __kitS.section("bootstrap returns a Promise", async () => {
    const a = [3.1, 4.7, 2.2, 9.9, 5.5, 6.0, 1.4, 7.3, 8.8, 4.1, 2.9], b = [2.0, 3.3, 4.4, 1.8, 2.6, 5.1, 3.9];
    for (const [stat, cmp, withB] of [["mean", "diff", false], ["median", "diff", true], ["median", "ratio", true], ["mean", "ratio", true]]) {
        const opts = { statistic: stat, compare: cmp, resamples: 300, level: 0.9, seed: 42, ...(withB ? { b } : {}) };
        const p = stats.bootstrap(a, opts);
        ok(p instanceof Promise, "bootstrap returns a Promise");
        const r = await p, want = bootRef(a, withB ? b : null, stat, cmp, 300, 0.9, 42);
        ok(r.low === want.low && r.high === want.high, `bootstrap ${stat}/${cmp}${withB ? " two-sample" : ""} reproduces the documented resampling stream exactly ([${r.low}, ${r.high}] vs [${want.low}, ${want.high}])`);
    }
    const big = Array.from({ length: 5000 }, (_, i) => Math.sin(i) * 10 + (i % 7));
    const r1 = await stats.bootstrap(big, { resamples: 400, seed: 9 }), r2 = await stats.bootstrap(big, { resamples: 400, seed: 9 });
    ok(r1.low === r2.low && r1.high === r2.high && r1.low < r1.estimate && r1.estimate < r1.high, "bootstrap is deterministic per seed and brackets the estimate (offloaded size)");
    const sd = Math.sqrt(stats.variance(big)), r4 = await stats.bootstrap(big, { resamples: 4000, level: 0.9, seed: 5 });
    const theory = 2 * stats.normPpf(0.95) * sd / Math.sqrt(big.length), width = r4.high - r4.low;
    ok(width > 0.85 * theory && width < 1.15 * theory, `bootstrap 90% interval of the mean has the normal-theory width (${width} vs ${theory}): resamples are independent`);
    const r3 = await stats.bootstrap(big, { resamples: 400, seed: 10 });
    ok(r3.low !== r1.low || r3.high !== r1.high, "a different seed gives a different interval");
});
__kitS.end();

throws(() => stats.ttestInd(1, [1, 2]), TypeError, /array of numbers/, "a non-array sample is a TypeError");
throws(() => stats.ttestInd([1, 2], [1, 2], { alternative: "bigger" }), RangeError, /unknown alternative 'bigger'/, "unknown alternative is a RangeError naming it");
throws(() => stats.ttestInd([1, 2], [1, 2], { equalvar: false }), TypeError, /unknown option 'equalvar'/, "a misspelled option is rejected");
throws(() => stats.ttestInd([1, 2], [1, 2], { equalVar: "no" }), TypeError, /equalVar must be a boolean/, "equalVar must be a boolean");
throws(() => stats.mannWhitneyU([1, NaN], [1, 2]), RangeError, /NaN/, "NaN input is a RangeError");
throws(() => stats.mannWhitneyU([], [1, 2]), RangeError, /sample size/, "an empty sample is a RangeError");
throws(() => stats.ks2Samp([1], [2], { method: "fast" }), RangeError, /unknown method 'fast'/, "unknown ks method");
throws(() => stats.ttestRel([1, 2, 3], [1, 2]), RangeError, /length mismatch/, "ttestRel needs paired samples");
throws(() => stats.pearson([1, 2, 3], [1, 2]), RangeError, /length mismatch/, "pearson needs equal lengths");
throws(() => stats.chi2Contingency([[1, 2], [3]]), RangeError, /row 1 has 1 cells, expected 2/, "ragged contingency table");
throws(() => stats.chi2Contingency([[0, 0], [3, 4]]), RangeError, /out of domain/, "a zero row makes an expected frequency 0");
throws(() => stats.chiSquare([1, 2, 3], [1, 2, 4]), RangeError, /out of domain/, "chiSquare observed and expected sums must agree");
throws(() => stats.pAdjust([0.1, 1.2], "bh"), RangeError, /out of domain/, "p-values above 1 are rejected");
throws(() => stats.pAdjust([0.1], "fdr"), RangeError, /unknown method 'fdr'/, "unknown pAdjust method");
throws(() => stats.proportionsZTest(5, 4, 1, 10), RangeError, /out of domain/, "a count above its total is rejected");
throws(() => stats.tCdf(1), TypeError, /parameter 1 must be a number/, "missing distribution parameter");
throws(() => stats.psi([1, 2], [1, 2], { bins: 1 }), RangeError, /bins must be an integer/, "psi needs at least two bins");
throws(() => stats.mannWhitneyU(new Array(400).fill(0).map((_, i) => i), new Array(400).fill(0).map((_, i) => i + 0.5), { method: "exact" }), RangeError, /sample size/, "explicit exact Mann-Whitney is bounded (n1*n2 <= 2^17)");
await rejects(() => stats.bootstrap([1, 2, 3], { resamples: 1 }), RangeError, /resamples must be an integer/, "bootstrap rejects (not throws) on bad resamples");
await rejects(() => stats.bootstrap([1, 2, 3], { level: 1 }), RangeError, /level must be in/, "bootstrap level in (0, 1)");
await rejects(() => stats.bootstrap([1, NaN]), RangeError, /NaN/, "bootstrap rejects NaN input");
await rejects(() => stats.bootstrap([1, 2], { b: [] }), RangeError, /sample size/, "bootstrap rejects an empty second sample");

print(`test_mathx_stats_contract: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(`${fail} contract failures`);
