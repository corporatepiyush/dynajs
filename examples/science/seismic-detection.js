// Seismic detection — an hour of four-station seismograms scanned with STA/LTA triggers, associated into events,
// picked by cross-correlation, located by a vectorised grid search and sized with a local-magnitude formula.
//
// WHAT IT IS
//   The automatic pipeline of a regional seismic network:
//
//   data        100 Hz traces per station: coloured noise plus earthquakes whose P and S waves arrive at
//               distance / Vp and distance / Vs, scaled by the inverse of the magnitude formula below, and one
//               instrument glitch that only a single station sees
//   trigger     energy = x^2 (simd mul), prefix sums by simd cumsum, STA (0.5 s) / LTA (20 s) ratio; on at 5, off
//               at 1.5; trigger windows kept in a RangeSet per station
//   associate   an event needs triggers on at least three stations within 25 s (IntervalTree overlap query);
//               the single-station glitch must not become an event
//   pick        each trigger onset is refined by an AIC picker over the window (variance split computed from
//               prefix sums), giving sub-0.05 s P arrivals
//   locate      a 1 km grid over the 120 x 120 km region: for every cell the residuals of the P arrival
//               differences are whole-grid simd sub/scale/mul/add over precomputed distance arrays; argmin is
//               the epicentre
//   magnitude   Hutton & Boore (1987) local magnitude ML = log10(A) + 1.11 log10(r) + 0.00189 r - 2.09
//               averaged over stations; must recover the planted magnitudes
//
// MODULES  dyna:simd (mul, cumsum, sub, scale, add, argmin, max, abs) · dyna:structures (RangeSet, IntervalTree)
//          · dyna:random · dyna:cli · Array/Number extensions
//
// RUN      dynajs examples/science/seismic-detection.js

import { mul, cumsum, sub, scale, add, argmin, max, abs, addScalar } from "dyna:simd";
import { RangeSet, IntervalTree } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1906);
const HZ = 100,
    SECONDS = 3600,
    N = HZ * SECONDS,
    VP = 6.0,
    VS = 3.5;
const STATIONS = [
    { code: "ALPH", x: 10, y: 15 },
    { code: "BRAV", x: 105, y: 20 },
    { code: "CHAR", x: 60, y: 110 },
    { code: "DELT", x: 15, y: 95 },
];
const QUAKES = [
    { t: 400.37, x: 40.4, y: 50.7, ml: 2.6 },
    { t: 1300.81, x: 90.2, y: 79.6, ml: 3.4 },
    { t: 2200.12, x: 19.7, y: 70.3, ml: 2.2 },
    { t: 3100.55, x: 70.6, y: 29.5, ml: 3.0 },
];
const GLITCH = { station: 2, t: 1800 };
const dist = (s, q) => Math.hypot(s.x - q.x, s.y - q.y, 8);
const ampFor = (ml, r) => 10 ** (ml - 1.11 * Math.log10(r) - 0.00189 * r + 2.09);

// =============================================================================
// Synthetic traces
// =============================================================================
const traces = STATIONS.map(() => new Float32Array(N));
for (let s = 0; s < STATIONS.length; s++) {
    const x = traces[s];
    let prev = 0;
    for (let i = 0; i < N; i++) {
        prev = 0.85 * prev + rng.normal();
        x[i] = prev * 0.02;
    }
    for (const q of QUAKES) {
        const r = dist(STATIONS[s], q),
            amp = ampFor(q.ml, r);
        for (const [v, rel, f, decay] of [
            [VP, 0.35, 6, 1.2],
            [VS, 1.0, 3, 2.5],
        ]) {
            const start = Math.round((q.t + r / v) * HZ);
            for (let k = 0; k < 20 * HZ && start + k < N; k++) {
                const tt = k / HZ;
                x[start + k] +=
                    amp * rel * Math.exp(-tt / decay) * Math.sin(2 * Math.PI * f * tt) * (1 - Math.exp(-tt * 20));
            }
        }
    }
}
{
    const g = Math.round(GLITCH.t * HZ);
    for (let k = 0; k < 30; k++) traces[GLITCH.station][g + k] += 5 * (k % 2 ? -1 : 1);
}

// =============================================================================
// STA/LTA triggering
// =============================================================================
const STA = 0.5 * HZ,
    LTA = 20 * HZ,
    ON = 5,
    OFF = 1.5;
function triggers(x) {
    const e = new Float32Array(N);
    mul(e, x, x);
    const mean = e.reduce((a, b) => a + b, 0) / N;
    scale(e, 1 / mean);
    const c = cumsum(new Float32Array(e));
    const windows = new RangeSet();
    let on = -1;
    for (let i = LTA; i < N; i++) {
        const sta = (c[i] - c[i - STA]) / STA,
            lta = (c[i - STA] - c[i - LTA]) / (LTA - STA);
        const ratio = sta / Math.max(lta, 1e-6);
        if (on < 0 && ratio > ON) on = i;
        else if (on >= 0 && ratio < OFF) {
            windows.add(on, i);
            on = -1;
        }
    }
    if (on >= 0) windows.add(on, N);
    return { windows, cum: c };
}
const t0 = performance.now();
const trig = traces.map(triggers);
const trigMs = performance.now() - t0;
results["every station triggers on every quake"] = trig.every((t, s) =>
    QUAKES.every((q) =>
        t.windows.intersects(
            Math.round((q.t + dist(STATIONS[s], q) / VP) * HZ) - HZ,
            Math.round((q.t + dist(STATIONS[s], q) / VP) * HZ) + 2 * HZ,
        ),
    ),
);
results["the glitch triggers its own station"] = trig[GLITCH.station].windows.intersects(
    GLITCH.t * HZ - 50,
    GLITCH.t * HZ + 100,
);

// =============================================================================
// AIC picker: onset = argmin of k*log(var(x[0..k])) + (n-k-1)*log(var(x[k..n]))
// =============================================================================
function aicPick(x, lo, hi) {
    const n = hi - lo,
        s1 = new Float64Array(n + 1),
        s2 = new Float64Array(n + 1);
    for (let i = 0; i < n; i++) {
        const v = x[lo + i];
        s1[i + 1] = s1[i] + v;
        s2[i + 1] = s2[i] + v * v;
    }
    const aic = new Float32Array(n).fill(Infinity);
    for (let k = 10; k < n - 10; k++) {
        const v1 = s2[k] / k - (s1[k] / k) ** 2,
            v2 = (s2[n] - s2[k]) / (n - k) - ((s1[n] - s1[k]) / (n - k)) ** 2;
        aic[k] = k * Math.log(Math.max(v1, 1e-12)) + (n - k - 1) * Math.log(Math.max(v2, 1e-12));
    }
    return lo + argmin(aic);
}

// =============================================================================
// Association: >= 3 stations within 25 s
// =============================================================================
const tree = new IntervalTree();
trig.forEach((t, s) => {
    for (const [a, b] of t.windows.ranges()) tree.insert(a, b, s);
});
const events = [];
const used = new Set();
for (const [a, b, s] of trig
    .flatMap((t, s) => t.windows.ranges().map(([a, b]) => [a, b, s]))
    .sort((p, q) => p[0] - q[0])) {
    if (used.has(`${s}:${a}`)) continue;
    const near = tree.overlapping(a - 25 * HZ, a + 25 * HZ).filter(([lo]) => lo >= a - 25 * HZ);
    const firstPer = new Map();
    for (const [lo, hi, st] of near) if (!firstPer.has(st) || lo < firstPer.get(st)[0]) firstPer.set(st, [lo, hi]);
    if (firstPer.size < 3) continue;
    for (const [lo, , st] of near) used.add(`${st}:${lo}`);
    const picks = [...firstPer].map(([st, [lo]]) => ({
        st,
        onset: aicPick(traces[st], Math.max(0, lo - 2 * HZ), lo + HZ) / HZ,
    }));
    events.push({ picks });
}
results["four events associated"] = events.length === 4;
results["the single-station glitch is not an event"] = events.every(
    (e) => !e.picks.some((p) => p.st === GLITCH.station && Math.abs(p.onset - GLITCH.t) < 2),
);

// =============================================================================
// Location: grid search with whole-grid vector arithmetic
// =============================================================================
const GRID = 121,
    cells = GRID * GRID;
const dGrid = STATIONS.map((st) => {
    const d = new Float32Array(cells);
    for (let gy = 0; gy < GRID; gy++)
        for (let gx = 0; gx < GRID; gx++) d[gy * GRID + gx] = Math.hypot(st.x - gx, st.y - gy, 8);
    return d;
});
function locate(picks) {
    const ref = picks[0],
        cost = new Float32Array(cells),
        r = new Float32Array(cells);
    for (const p of picks.slice(1)) {
        sub(r, dGrid[p.st], dGrid[ref.st]);
        scale(r, 1 / VP);
        addScalar(r, -(p.onset - ref.onset));
        mul(r, r, r);
        add(cost, cost, r);
    }
    const best = argmin(cost);
    return { x: best % GRID, y: Math.floor(best / GRID) };
}
const report = [];
let worstKm = 0,
    worstMl = 0,
    worstPick = 0;
for (const ev of events) {
    const loc = locate(ev.picks);
    const quake = QUAKES.reduce((b, q) =>
        Math.hypot(q.x - loc.x, q.y - loc.y) < Math.hypot(b.x - loc.x, b.y - loc.y) ? q : b,
    );
    const km = Math.hypot(quake.x - loc.x, quake.y - loc.y);
    for (const p of ev.picks)
        worstPick = Math.max(worstPick, Math.abs(p.onset - (quake.t + dist(STATIONS[p.st], quake) / VP)));
    const mls = ev.picks.map((p) => {
        const r = Math.hypot(STATIONS[p.st].x - loc.x, STATIONS[p.st].y - loc.y, 8);
        const w = traces[p.st].subarray(Math.round(p.onset * HZ), Math.round(p.onset * HZ) + 15 * HZ);
        const peak = max(abs(new Float32Array(w.length), w));
        return Math.log10(peak) + 1.11 * Math.log10(r) + 0.00189 * r - 2.09;
    });
    const ml = mls.average();
    worstKm = Math.max(worstKm, km);
    worstMl = Math.max(worstMl, Math.abs(ml - quake.ml));
    const origin =
        ev.picks[0].onset - Math.hypot(STATIONS[ev.picks[0].st].x - loc.x, STATIONS[ev.picks[0].st].y - loc.y, 8) / VP;
    report.push([
        origin.toFixed(2) + " s",
        `${loc.x}, ${loc.y}`,
        `${quake.x}, ${quake.y}`,
        km.toFixed(1),
        ml.toFixed(2),
        quake.ml.toFixed(1),
        ev.picks.length,
    ]);
}
print(Table(report, { head: ["origin", "located (km)", "true (km)", "error km", "ML", "true ML", "stations"] }));
results["P picks within 0.1 s of the true arrivals"] = worstPick < 0.1;
results["epicentres within 2 km on a 1 km grid"] = worstKm <= 2;
results["local magnitudes within 0.3 units"] = worstMl < 0.3;
print(
    `triggering ${(trigMs / STATIONS.length).toFixed(1)} ms per station-hour (${N.format()} samples); worst pick error ${worstPick.toFixed(3)} s`,
);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
