// bench: dyna:file sync stat vs file.async.stat on a warm cache (rule 11; the F2 2x decision in file.d.ts). Env: FS_N=calls (default 4096), FS_ROUNDS=repeats.
// One JSON line per mode and round: sync, async (awaited one by one), par (64 in flight; per-call time = batch time / 64). p50/p90 us from a 50 ns histogram, mean us, peak RSS and growth.
import * as file from "dyna:file";
import * as sys from "dyna:sys";
import * as time from "dyna:time";
const env = (k, d) => { try { const v = sys.getEnv(k); return v === undefined || v === null || v === "" ? d : v; } catch { return d; } };
const n = +env("FS_N", "4096"), rounds = +env("FS_ROUNDS", "1");
const p = new file.Path(file.Path.temp(), "dj_fsbench_" + Date.now() + "_" + Math.random().toString(36).slice(2, 7));
file.writeFile(p, "x");
const ns = () => Number(time.monotonicNano());
const rss = () => sys.memoryUsage().peakRss;
const B = 200000;
const h = new Uint32Array(B + 1);
const add = (d) => { h[Math.min(B, Math.floor(d / 50))]++; };
const pct = (q) => { let acc = 0; const want = Math.ceil(n * q); for (let i = 0; i <= B; i++) { acc += h[i]; if (acc >= want) return +((i * 50 + 25) / 1000).toFixed(3); } return B * 0.05; };
for (let i = 0; i < 2000; i++) file.stat(p);
for (let i = 0; i < 2000; i++) await file.async.stat(p);
async function mode(m, r) {
    h.fill(0);
    const r0 = rss(), t0 = ns();
    if (m === "sync") for (let i = 0; i < n; i++) { const a = ns(); file.stat(p); add(ns() - a); }
    else if (m === "async") for (let i = 0; i < n; i++) { const a = ns(); await file.async.stat(p); add(ns() - a); }
    else for (let i = 0; i < n; i += 64) {
        const k = Math.min(64, n - i), a = ns(), b = [];
        for (let j = 0; j < k; j++) b.push(file.async.stat(p));
        await Promise.all(b);
        const per = (ns() - a) / k;
        for (let j = 0; j < k; j++) add(per);
    }
    const ms = (ns() - t0) / 1e6;
    print(JSON.stringify({ mode: m, round: r, n, p50: pct(0.5), p90: pct(0.9), mean: +(ms * 1000 / n).toFixed(3), peakRss: rss(), rssGrowth: rss() - r0, bytesPerOp: +((rss() - r0) / n).toFixed(3) }));
}
for (let r = 0; r < rounds; r++) for (const m of ["sync", "async", "par"]) await mode(m, r);
file.remove(p);
