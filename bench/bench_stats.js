// dyna:mathx stats distributions and hypothesis tests, best of N wall-clock runs on seeded data. The same samples are written
// as raw little-endian float64 for bench/bench_stats_scipy.py, which times scipy on them, so the two tables compare like for
// like. Usage: dynajs bench/bench_stats.js [runs] [dataFile]
import { stats } from "dyna:mathx";
import { writeFile, Path } from "dyna:file";

const RUNS = Number(scriptArgs[1] ?? 5), OUT = scriptArgs[2];
let s = 0x9e3779b9;
const u = () => ((s = (Math.imul(s ^ (s >>> 15), 0x2c1b3c6d) + 0x6d2b79f5) >>> 0) / 4294967296);
const normal = (n, mu, sd) => Float64Array.from({ length: n }, () => mu + sd * Math.sqrt(-2 * Math.log(1 - u())) * Math.cos(2 * Math.PI * u()));
const N = 1000000, A = normal(N, 0, 1), B = normal(N, 0.01, 1.1), M1 = normal(200000, 5, 2), M2 = normal(200000, 5.02, 2);
const K1 = normal(100000, 0, 1), K2 = normal(100000, 0.01, 1), E1 = normal(5000, 0, 1), E2 = normal(5000, 0.05, 1);
const BA = B.map(Math.abs), P = Float64Array.from({ length: 100000 }, () => u()), BS = normal(10000, 3, 1);
const TAB = Array.from({ length: 50 }, (_, i) => Array.from({ length: 50 }, (_, j) => 10 + ((i * 31 + j * 17) % 23)));
if (OUT) {
    const parts = [A, B, M1, M2, K1, K2, E1, E2, P, BS], total = parts.reduce((t, p) => t + p.length, 0), all = new Float64Array(total);
    let at = 0;
    for (const p of parts) { all.set(p, at); at += p.length; }
    writeFile(new Path(OUT), new Uint8Array(all.buffer));
}
async function best(f) {
    let b = Infinity;
    for (let r = 0; r < RUNS; r++) { const t = performance.now(); await f(); b = Math.min(b, performance.now() - t); }
    return b;
}
const rows = [
    ["normCdf x1e6", () => stats.normCdf(A)],
    ["normPpf x1e5", () => stats.normPpf(P)],
    ["tSf df=12 x1e6", () => stats.tSf(A, 12)],
    ["chi2Sf k=5 x1e6", () => stats.chi2Sf(BA, 5)],
    ["ttestInd Welch 1e6 vs 1e6", () => stats.ttestInd(A, B, { equalVar: false })],
    ["mannWhitneyU 2e5 vs 2e5", () => stats.mannWhitneyU(M1, M2)],
    ["ks2Samp asymp 1e5 vs 1e5", () => stats.ks2Samp(K1, K2)],
    ["ks2Samp exact 5000 vs 5000", () => stats.ks2Samp(E1, E2)],
    ["pearson 1e6", () => stats.pearson(A, B)],
    ["spearman 1e6", () => stats.spearman(A, B)],
    ["rankData 1e6", () => stats.rankData(A)],
    ["pAdjust bh 1e5", () => stats.pAdjust(P, "bh")],
    ["chi2Contingency 50x50", () => stats.chi2Contingency(TAB)],
    ["bootstrap median n=1e4 R=2000", () => stats.bootstrap(BS, { statistic: "median", resamples: 2000 })],
];
print(`best of ${RUNS}`);
for (const [name, f] of rows) print(name.padEnd(32) + (await best(f)).toFixed(2).padStart(10) + " ms");
