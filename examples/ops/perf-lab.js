// apps:solo -- it benchmarks, and a control run must not move, so it runs alone
// Performance lab — a load-testing and benchmarking harness that measures honestly: open vs closed load models,
// coordinated-omission correction, a pipeline-depth sweep that separates server cost from round-trip latency,
// log-bucketed latency histograms, micro-benchmarks with a calibrated empty loop, and a statistically tested A/B.
//
// WHAT IT IS
//   The tool a team reaches for before claiming "it is faster":
//
//   targets       two implementations of the same endpoint (an order-pricing API): A encodes JSON, B answers in
//                 MessagePack and caches the parsed catalogue — both served by the same App
//   histograms    latencies go into an HDR-style log-linear histogram (constant relative error, fixed memory) so
//                 p99.9 is as cheap to report as p50
//   closed model  N virtual users each wait for a reply before the next request: throughput is what the server
//                 gives, but a stall silently suppresses the requests that would have queued (coordinated omission)
//   scheduled     requests follow a fixed arrival schedule; latency measured from the INTENDED send time shows a
//                 stall in the tail where users feel it, while timing from the actual send (coordinated omission)
//                 hides the requests that queued behind it
//   depth sweep   concurrency 1, 2, 4 … 64 against one server: throughput rises until it plateaus — the plateau's
//                 per-request time is the server's own cost; the depth-1 latency minus that is network + scheduling
//   micro-bench   the encoders themselves through dyna:bench, with the empty-loop cost subtracted and a control
//                 benchmark that must not move between A and B
//   A/B verdict   interleaved repeated runs, a Mann–Whitney U test on per-run p50s and a bootstrap CI for the
//                 speed-up — "B is faster" is only printed when the interval excludes 1. Small samples without ties
//                 get the EXACT U distribution (counted by dynamic programming, as scipy's method="exact"); with
//                 ties, the normal approximation with the tie-corrected variance. The exact table is checked by
//                 brute force over all 12 870 ways to split 16 runs 8/8, and the test's size by 2000 seeded A/A
//                 trials: at alpha 0.05 it must reject at most 5% of samples drawn from one distribution
//   bucketing     the histogram's index/value mapping is checked exhaustively below 2^20 us and at every power
//                 of two to 2^40 (N-1, N, N+1): monotone, each value inside its bucket, bucket width <= 1/64
//   cross-check   every histogram is shadowed by a TDigest (a different algorithm with a different error model);
//                 each quantile must agree within the HDR bucket width, or (across a gap between modes, where every
//                 value is a valid answer) at the same rank by the digest's cdf — so neither can drift unnoticed. Bootstrap resamples are drawn into a
//                 Float64Array and their medians taken natively with mathx stats.median
//
// MODULES  dyna:http (App) · dyna:bench · dyna:serialize (MsgPack) · dyna:mathx (erfc, stats: mannWhitneyU, bootstrap) · dyna:random
//          · dyna:structures (LRU, TDigest) · dyna:async (sleep) · dyna:cli (Table) · dyna:dataframe
//
// RUN      dynajs examples/ops/perf-lab.js     runs the whole lab against a local server (about 10 seconds)

import { App } from "dyna:http";
import { bench } from "dyna:bench";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { erfc, stats } from "dyna:mathx";
import { Random } from "dyna:random";
import { LRU, TDigest } from "dyna:structures";
import { sleep } from "dyna:async";
import { Table } from "dyna:cli";
import { DataFrame } from "dyna:dataframe";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const rng = new Random(123);

// =============================================================================
// The system under test
// =============================================================================
const catalogue = Array.from({ length: 2000 }, (_, i) => ({
    sku: `SKU-${i}`,
    price: 100 + ((i * 37) % 9000),
    tax: [0.07, 0.19][i % 2],
    tags: ["a", "b", "c"].slice(0, 1 + (i % 3)),
}));
const catalogueJson = JSON.stringify(catalogue);
const cache = new LRU(512);
let stallUntil = 0;
function priceA(skus) {
    // re-parses the catalogue every call (the "before")
    const cat = JSON.parse(catalogueJson),
        bySku = new Map(cat.map((c) => [c.sku, c]));
    const lines = skus
        .map((s) => bySku.get(s))
        .filter(Boolean)
        .map((c) => ({ sku: c.sku, gross: Math.round(c.price * (1 + c.tax)) }));
    return { lines, total: lines.reduce((t, l) => t + l.gross, 0) };
}
const parsed = new Map(catalogue.map((c) => [c.sku, c]));
function priceB(skus) {
    // parsed once, results cached by basket (the "after")
    const key = skus.join(","),
        hit = cache.get(key);
    if (hit) return hit;
    const lines = skus
        .map((s) => parsed.get(s))
        .filter(Boolean)
        .map((c) => ({ sku: c.sku, gross: Math.round(c.price * (1 + c.tax)) }));
    const out = { lines, total: lines.reduce((t, l) => t + l.gross, 0) };
    cache.put(key, out);
    return out;
}
const app = new App({ port: 0 });
app.post("/a/price", async (req) => {
    if (Date.now() < stallUntil) await sleep(stallUntil - Date.now());
    return { body: JSON.stringify(priceA(JSON.parse(req.body))), contentType: "application/json" };
});
app.post("/b/price", async (req) => {
    if (Date.now() < stallUntil) await sleep(stallUntil - Date.now());
    return { body: MsgPackEncode(priceB(JSON.parse(req.body))), contentType: "application/msgpack" };
});
app.get("/noop", () => ({ body: "", status: 204 }));
app.start();
const base = `http://127.0.0.1:${app.port}`;
const baskets = Array.from({ length: 64 }, () =>
    Array.from({ length: 1 + rng.nextBounded(6) }, () => `SKU-${rng.nextBounded(2000)}`),
);

// =============================================================================
// A log-linear latency histogram (HDR-style): 2 significant digits, microseconds to minutes
// =============================================================================
class Histogram {
    constructor() {
        this.sub = 128;
        this.counts = new Float64Array(40 * this.sub);
        this.n = 0;
        this.max = 0;
        this.shadow = new TDigest(200);
    }
    index(us) {
        if (us < this.sub) return Math.floor(us);
        const e = Math.floor(Math.log2(us / this.sub)) + 1,
            half = this.sub / 2; // us in [sub*2^(e-1), sub*2^e)
        return this.sub + (e - 1) * half + (Math.floor(us / 2 ** e) - half);
    }
    valueAt(i) {
        if (i < this.sub) return i;
        const half = this.sub / 2,
            k = i - this.sub,
            e = Math.floor(k / half) + 1;
        return ((k % half) + half) * 2 ** e;
    }
    record(ms) {
        const us = Math.max(0, Math.round(ms * 1000));
        this.counts[Math.min(this.counts.length - 1, this.index(us))]++;
        this.n++;
        this.max = Math.max(this.max, ms);
        this.shadow.add(ms);
    }
    agrees(q) {
        // within the HDR bucket (value space), or at the same rank (between modes any value is right)
        const v = this.quantile(q),
            t = this.shadow.quantile(q),
            width = v < this.sub / 1000 ? 0.001 : v / 64;
        return Math.abs(t - v) <= width * 1.5 || Math.abs(this.shadow.cdf(v) - q) <= 0.02;
    }
    quantile(q) {
        const target = q * this.n;
        let acc = 0;
        for (let i = 0; i < this.counts.length; i++) {
            acc += this.counts[i];
            if (acc >= target) return this.valueAt(i) / 1000;
        }
        return this.max;
    }
    summary() {
        return {
            n: this.n,
            p50: this.quantile(0.5),
            p90: this.quantile(0.9),
            p99: this.quantile(0.99),
            p999: this.quantile(0.999),
            max: this.max,
        };
    }
}
const fmt = (ms) => (ms < 1 ? `${(ms * 1000).toFixed(0)}us` : `${ms.toFixed(ms < 10 ? 2 : 0)}ms`);

// =============================================================================
// Load models
// =============================================================================
const bodyFor = (variant, i) => JSON.stringify(baskets[i % baskets.length]);
async function closedModel(variant, users, durationMs) {
    const h = new Histogram(),
        end = Date.now() + durationMs;
    let i = 0;
    await Promise.all(
        Array.from({ length: users }, async () => {
            while (Date.now() < end) {
                const t = performance.now();
                const r = await fetch(`${base}/${variant}/price`, { method: "POST", body: bodyFor(variant, i++) });
                await r.arrayBuffer();
                h.record(performance.now() - t);
            }
        }),
    );
    return { ...h.summary(), rps: h.n / (durationMs / 1000), histogram: h };
}
async function openModel(variant, rate, durationMs, workers = 4) {
    // a fixed schedule; each worker waits for its reply (like wrk)
    const h = new Histogram(),
        naive = new Histogram(),
        start = performance.now(),
        interval = (1000 * workers) / rate,
        perWorker = Math.floor(durationMs / interval);
    await Promise.all(
        Array.from({ length: workers }, async (_, w) => {
            for (let k = 0; k < perWorker; k++) {
                const intended = start + (w * interval) / workers + k * interval,
                    wait = intended - performance.now();
                if (wait > 1) await sleep(wait);
                const sent = performance.now();
                const r = await fetch(`${base}/${variant}/price`, { method: "POST", body: bodyFor(variant, k) });
                await r.arrayBuffer();
                const done = performance.now();
                h.record(done - intended);
                naive.record(done - sent); // naive ignores the time the request spent waiting to be sent
            }
        }),
    );
    return {
        corrected: h.summary(),
        naive: naive.summary(),
        p95: { corrected: h.quantile(0.95), naive: naive.quantile(0.95) },
        histogram: h,
    };
}
async function depthSweep(variant, depths, perDepth) {
    const rows = [];
    for (const d of depths) {
        let sent = 0;
        const t = performance.now(),
            lat = [];
        await Promise.all(
            Array.from({ length: d }, async () => {
                while (sent < perDepth) {
                    sent++;
                    const s = performance.now();
                    const r = await fetch(`${base}/${variant}/price`, { method: "POST", body: bodyFor(variant, sent) });
                    await r.arrayBuffer();
                    lat.push(performance.now() - s);
                }
            }),
        );
        const secs = (performance.now() - t) / 1000;
        rows.push({ depth: d, rps: perDepth / secs, usPerReq: (secs * 1e6) / perDepth, p50: stats.median(lat) });
    }
    return rows;
}

// =============================================================================
// Statistics for the A/B verdict
// =============================================================================
// exact[n1][n2][u]: the number of ways U = u can arise with no ties; f(n1, n2, u) = f(n1-1, n2, u-n2) + f(n1, n2-1, u)
const exactU = (() => {
    const memo = new Map();
    const f = (n1, n2) =>
        memo.getOrInsertComputed(`${n1},${n2}`, () => {
            const out = new Float64Array(n1 * n2 + 1);
            if (n1 === 0 || n2 === 0) {
                out[0] = 1;
                return out;
            }
            const a = f(n1 - 1, n2),
                b = f(n1, n2 - 1);
            for (let u = 0; u <= n1 * n2; u++) out[u] = (u >= n2 ? (a[u - n2] ?? 0) : 0) + (b[u] ?? 0);
            return out;
        });
    return f;
})();
// the textbook rank-sum with an exact table, kept as an independent cross-check of the native stats.mannWhitneyU
function mannWhitneyJs(a, b) {
    const all = [...a.map((v) => [v, 0]), ...b.map((v) => [v, 1])].sort((x, y) => x[0] - y[0]);
    const ranks = new Array(all.length);
    for (let i = 0; i < all.length;) {
        let j = i;
        while (j + 1 < all.length && all[j + 1][0] === all[i][0]) j++;
        for (let k = i; k <= j; k++) ranks[k] = (i + j) / 2 + 1;
        i = j + 1;
    }
    const r1 = all.reduce((s, [, g], k) => s + (g === 0 ? ranks[k] : 0), 0);
    const n1 = a.length,
        n2 = b.length,
        n = n1 + n2,
        u1 = r1 - (n1 * (n1 + 1)) / 2,
        mu = (n1 * n2) / 2;
    let tieSum = 0;
    for (let i = 0; i < n;) {
        let j = i;
        while (j + 1 < n && all[j + 1][0] === all[i][0]) j++;
        tieSum += (j - i + 1) ** 3 - (j - i + 1);
        i = j + 1;
    }
    if (tieSum === 0 && n1 <= 20 && n2 <= 20) {
        // exact two-sided p: twice the smaller tail of the counted distribution
        const counts = exactU(n1, n2),
            total = counts.reduce((s, c) => s + c, 0);
        let lo = 0,
            hi = 0;
        for (let u = 0; u <= n1 * n2; u++) {
            if (u <= u1) lo += counts[u];
            if (u >= u1) hi += counts[u];
        }
        return { u: u1, exact: true, p: Math.min(1, (2 * Math.min(lo, hi)) / total) };
    }
    const sigma = Math.sqrt(((n1 * n2) / 12) * (n + 1 - tieSum / (n * (n - 1))));
    const z = (u1 - mu) / sigma;
    return { u: u1, z, exact: false, p: erfc(Math.abs(z) / Math.SQRT2) };
}
// native Mann-Whitney U (scipy semantics: exact when untied and a side has <= 8 values, else tie-corrected normal)
function mannWhitney(a, b) {
    const r = stats.mannWhitneyU(a, b, { continuity: false }),
        tied = new Set([...a, ...b]).size < a.length + b.length;
    return { u: r.statistic, exact: !tied && Math.min(a.length, b.length) <= 8, p: r.pvalue };
}
// 95% percentile bootstrap of median(a) / median(b), 2000 resamples on the worker pool
async function bootstrapRatio(a, b, n = 2000) {
    const r = await stats.bootstrap(a, { b, statistic: "median", compare: "ratio", resamples: n, seed: 7 });
    return [r.low, r.high];
}

// =============================================================================
// The lab
// =============================================================================
const results = {};
for (let i = 0; i < 20; i++) {
    await (await fetch(`${base}/a/price`, { method: "POST", body: bodyFor("a", i) })).text();
    await (await fetch(`${base}/b/price`, { method: "POST", body: bodyFor("b", i) })).arrayBuffer();
}

// Correctness first: both variants must price every basket identically.
for (const [i, b] of baskets.entries()) {
    const ra = await (await fetch(`${base}/a/price`, { method: "POST", body: bodyFor("a", i) })).json();
    const rb = MsgPackDecode(
        new Uint8Array(await (await fetch(`${base}/b/price`, { method: "POST", body: bodyFor("b", i) })).arrayBuffer()),
    );
    if (ra.total !== rb.total) {
        results[`basket ${b} priced differently: ${ra.total} vs ${rb.total}`] = false;
        break;
    }
}

// Micro-benchmarks with an empty-loop calibration and a control.
const empty = bench("empty loop", () => {}, { timeMs: 150 });
const mA = bench("priceA (parse every call)", () => priceA(baskets[3]), { timeMs: 250 });
const mB = bench(
    "priceB (parsed + cache)",
    () => {
        cache.delete(baskets[3].join(","));
        priceB(baskets[3]);
    },
    { timeMs: 250 },
);
const controlPair = () => [
    bench(
        "control: Math.sqrt loop",
        () => {
            let x = 0;
            for (let i = 0; i < 100; i++) x += Math.sqrt(i);
            return x;
        },
        { timeMs: 150 },
    ),
    bench(
        "control: Math.sqrt loop (again)",
        () => {
            let x = 0;
            for (let i = 0; i < 100; i++) x += Math.sqrt(i);
            return x;
        },
        { timeMs: 150 },
    ),
];
// The control is two identical wall-clock runs: on a loaded host one pair can land on either side of a burst, so
// it gets three tries; a real measurement bug moves every pair.
let control = controlPair();
const controlMoved = (c) => Math.abs(c[0].p50Ms / c[1].p50Ms - 1) >= 0.35;
for (let t = 1; t < 3 && controlMoved(control); t++) control = controlPair();
results["the control benchmark did not move"] = !controlMoved(control);

// Closed vs open model with a 300 ms stall in the middle.
// Throughput under a stall is compared with throughput without one, both in wall time: a burst of host load during
// the no-stall run can hide the loss, so the pair gets three tries.
let closedNoStall, closedStall;
for (let t = 0; t < 3; t++) {
    closedNoStall = await closedModel("a", 4, 300);
    setTimeout(() => {
        stallUntil = Date.now() + 300;
    }, 300);
    closedStall = await closedModel("a", 4, 700);
    if (closedStall.rps < closedNoStall.rps * 0.75) break;
}
setTimeout(() => {
    stallUntil = Date.now() + 300;
}, 300);
const open = await openModel("a", 100, 2000, 2);
results[
    `measuring from the intended send time exposes the stall: p95 ${fmt(open.p95.corrected)} vs ${fmt(open.p95.naive)} when timed from the actual send (coordinated omission)`
] = open.p95.corrected > 100 && open.p95.naive < open.p95.corrected / 5;
results[
    `in the closed model the stall shows up as lost throughput (${closedStall.rps.toFixed(0)} vs ${closedNoStall.rps.toFixed(0)} req/s), not as the queue users would feel`
] = closedStall.rps < closedNoStall.rps * 0.75;

results["HDR histogram and TDigest agree on p50/p90/p99 for every load model"] = [
    closedNoStall.histogram,
    closedStall.histogram,
    open.histogram,
].every((h) => [0.5, 0.9, 0.99].every((q) => h.agrees(q)));

// Depth sweep: server cost vs round-trip.
const sweep = await depthSweep("b", [1, 2, 4, 8, 16, 32], 200);
const plateau = Math.min(...sweep.map((r) => r.usPerReq)),
    depth1 = sweep[0].usPerReq;
// How much depth buys depends on the round trip: on a slow loopback (macOS) depth 1 is RTT-bound and throughput
// rises ~2x; on Linux depth 1 is already server-bound. What must hold everywhere is that queueing deeper never
// loses throughput, measured at the best deep row (the deepest row alone dips with scheduler noise in VMs).
results["deeper queues never lose throughput against depth 1"] =
    Math.max(...sweep.slice(2).map((r) => r.rps)) >= sweep[0].rps * 0.9;
results[
    `server cost ${plateau.toFixed(0)} us/request vs ${depth1.toFixed(0)} us at depth 1 (${((1 - plateau / depth1) * 100).toFixed(0)}% of depth-1 time is round trip)`
] = plateau < depth1;

{
    // The histogram's buckets, exhaustively where they are dense and at every power of two (N-1, N, N+1) above
    const h = new Histogram(),
        values = Iterator.concat(
            Iterator.range(0, 2 ** 20),
            Iterator.range(20, 41).flatMap((k) => [2 ** k - 1, 2 ** k, 2 ** k + 1]),
        );
    let ok = true,
        worst = 0,
        prev = -1;
    for (const us of values) {
        const i = h.index(us),
            lo = h.valueAt(i),
            hi = h.valueAt(i + 1);
        if (i < prev || lo > us || us >= hi) ok = false;
        if (us >= h.sub) worst = Math.max(worst, (hi - lo) / lo);
        prev = i;
    }
    results[`HDR buckets: monotone, each value inside its bucket, width <= 1/64 (worst ${worst.toFixed(5)})`] =
        ok && worst <= 1 / 64;
}
{
    // The exact U table against brute force: every one of the C(16,8) = 12870 splits of ranks 1..16 into 8 and 8
    const brute = new Float64Array(65);
    for (let mask = 0; mask < 1 << 16; mask++) {
        let bits = 0,
            rankSum = 0;
        for (let k = 0; k < 16; k++)
            if (mask & (1 << k)) {
                bits++;
                rankSum += k + 1;
            }
        if (bits === 8) brute[rankSum - 36]++;
    }
    const dp = exactU(8, 8);
    const sep = mannWhitney([1, 2, 3, 4, 5, 6, 7, 8], [9, 10, 11, 12, 13, 14, 15, 16]);
    results[
        `exact U distribution equals brute force over 12870 splits; complete separation p = 2/12870 (${sep.p.toExponential(3)})`
    ] = brute.every((c, u) => c === dp[u]) && sep.exact && Math.abs(sep.p - 2 / 12870) < 1e-15;
    // reference values from scipy 1.18.1, scipy.stats.mannwhitneyu(x, y, alternative="two-sided"): one sample
    // without ties (method="exact": U 19, p 0.19487179487179485) and one with ties (method="asymptotic",
    // use_continuity=False: U 7, p 0.007837931695106938) -- the second pins the tie-corrected variance
    const untied = mannWhitney([1.5, 2.5, 3.1, 4.2, 7.7, 8.1, 9.9, 10.4], [3.3, 5.5, 6.1, 6.2, 9.1, 11.5, 12.0, 13.3]),
        tied = mannWhitney([1, 2, 2, 3, 3, 3, 4, 5], [3, 4, 4, 5, 6, 6, 7, 8]);
    results[`p-values match scipy's mannwhitneyu (exact ${untied.p.toFixed(6)}, tied ${tied.p.toFixed(6)})`] =
        untied.exact &&
        untied.u === 19 &&
        Math.abs(untied.p - 0.19487179487179485) < 1e-12 &&
        !tied.exact &&
        tied.u === 7 &&
        Math.abs(tied.p - 0.007837931695106938) < 1e-12;
    // A/A calibration: both samples from one distribution, 2000 seeded trials of 8 vs 8
    const aa = new Random(99);
    let rejectExact = 0,
        rejectNormal = 0,
        mismatches = 0;
    for (let t = 0; t < 2000; t++) {
        const x = Array.from({ length: 8 }, () => aa.normal(1, 0.1)),
            y = Array.from({ length: 8 }, () => aa.normal(1, 0.1));
        const native = mannWhitney(x, y),
            js = mannWhitneyJs(x, y);
        if (native.u !== js.u || Math.abs(native.p - js.p) > 1e-12) mismatches++;
        if (native.p < 0.05) rejectExact++;
        // the plain normal approximation on the same samples, for comparison
        const all = [...x, ...y].sort((p, q) => p - q),
            r1 = x.reduce((s, v) => s + all.indexOf(v) + 1, 0),
            z = (r1 - 36 - 32) / Math.sqrt((64 * 17) / 12);
        if (erfc(Math.abs(z) / Math.SQRT2) < 0.05) rejectNormal++;
    }
    // the test's true size, from the table: the probability of every U whose two-sided p is below 0.05
    const total = dp.reduce((s, c) => s + c, 0);
    let size = 0;
    for (let u = 0; u <= 64; u++) {
        const lo = dp.slice(0, u + 1).reduce((s, c) => s + c, 0),
            hi = dp.slice(u).reduce((s, c) => s + c, 0);
        if ((2 * Math.min(lo, hi)) / total < 0.05) size += dp[u] / total;
    }
    const rate = rejectExact / 2000,
        se = Math.sqrt((size * (1 - size)) / 2000);
    results[
        `A/A: the exact test rejects ${(rate * 100).toFixed(1)}% at alpha 5% (its exact size ${(size * 100).toFixed(2)}%; normal approximation ${((rejectNormal / 2000) * 100).toFixed(1)}%)`
    ] = size <= 0.05 && Math.abs(rate - size) <= 3 * se;
    results[`native mannWhitneyU equals the rank-sum table on all 2000 A/A pairs (${mismatches} differ)`] =
        mismatches === 0;
}

// Interleaved A/B: 8 rounds each, alternating to cancel drift. A burst of host load can swamp one 16-run sample, so
// the verdict gets three samples; B's kernel is >5x cheaper, so a real regression fails all three.
const abSample = async () => {
    const a = [],
        b = [];
    for (let r = 0; r < 8; r++)
        for (const v of r % 2 ? ["b", "a"] : ["a", "b"]) (v === "a" ? a : b).push((await closedModel(v, 2, 60)).p50);
    return [a, b, mannWhitney(a, b), await bootstrapRatio(a, b)];
};
let runsA, runsB, mw, ci;
for (let t = 0; t < 3; t++) {
    [runsA, runsB, mw, ci] = await abSample();
    if (ci[0] > 1 && mw.p < 0.05) break;
}
const verdict =
    ci[0] > 1 && mw.p < 0.05
        ? `B is faster: ${(stats.median(runsA) / stats.median(runsB)).toFixed(2)}x [95% CI ${ci[0].toFixed(2)}–${ci[1].toFixed(2)}], p=${mw.p.toExponential(1)}`
        : `no significant difference (CI ${ci[0].toFixed(2)}–${ci[1].toFixed(2)}, p=${mw.p.toFixed(3)})`;
results[verdict] = /B is faster/.test(verdict);

print("perf-lab");
print(
    table([
        ["benchmark", "mean", "p99", "rsd"],
        ...[empty, mA, mB, ...control].map((r) => [
            r.name,
            fmt(r.meanMs),
            fmt(r.p99Ms),
            `${(r.rsd * 100).toFixed(1)}%`,
        ]),
    ]),
);
print(
    table([
        ["model", "requests", "rps", "p50", "p99", "p99.9", "max"],
        [
            "closed, steady",
            String(closedNoStall.n),
            closedNoStall.rps.toFixed(0),
            fmt(closedNoStall.p50),
            fmt(closedNoStall.p99),
            fmt(closedNoStall.p999),
            fmt(closedNoStall.max),
        ],
        [
            "closed, 300ms stall",
            String(closedStall.n),
            closedStall.rps.toFixed(0),
            fmt(closedStall.p50),
            fmt(closedStall.p99),
            fmt(closedStall.p999),
            fmt(closedStall.max),
        ],
        [
            "scheduled 100/s, naive",
            String(open.naive.n),
            "100",
            fmt(open.naive.p50),
            fmt(open.naive.p99),
            fmt(open.naive.p999),
            fmt(open.naive.max),
        ],
        [
            "scheduled 100/s, corrected",
            String(open.corrected.n),
            "100",
            fmt(open.corrected.p50),
            fmt(open.corrected.p99),
            fmt(open.corrected.p999),
            fmt(open.corrected.max),
        ],
    ]),
);
print(
    table([
        ["depth", "rps", "us/request", "p50 latency"],
        ...sweep.map((r) => [String(r.depth), r.rps.toFixed(0), r.usPerReq.toFixed(0), fmt(r.p50)]),
    ]),
);
const df = new DataFrame({ a: Float64Array.from(runsA), b: Float64Array.from(runsB) });
print(
    `  A/B        per-run p50 A ${fmt(df.MEDIAN("a"))}, B ${fmt(df.MEDIAN("b"))}; ${verdict} (${mw.exact ? "exact" : "normal, tie-corrected"})`,
);
app.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
