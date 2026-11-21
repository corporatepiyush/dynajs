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
//   S waves     the largest swing after P is the S wave; the AIC picker between P and that peak finds the onset; S - P
//               alone gives each station's
//               distance, r = (ts - tp) / (1/Vs - 1/Vp)
//   refine      Geiger's method: Gauss-Newton on (x, y, origin time) over every P and S arrival, started from the
//               grid cell; must beat the grid on every event and recover the origin times
//   oracle      STA/LTA recomputed from sliding window sums (re-summed exactly every 1000 samples) must give the same
//               trigger windows; it caught the float32 prefix sums this used to take from simd cumsum, which drift
//               by ~1.5% over a window 360 000 samples in and missed three noise triggers
//   magnitude   Hutton & Boore (1987) local magnitude ML = log10(A) + 1.11 log10(r) + 0.00189 r - 2.09
//               averaged over stations; must recover the planted magnitudes
//
// MODULES  dyna:simd (mul, cumsum, sub, scale, add, argmin, max, abs) · dyna:structures (RangeSet, IntervalTree)
//          · dyna:random · dyna:cli · Array/Number extensions
//
// RUN      dynajs examples/science/seismic-detection.js

import { mul, sub, scale, add, argmin, argmax, max, abs, addScalar } from "dyna:simd";
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
    // float64 prefix sums: a float32 cumsum over 360 000 samples drifts by ~1.5% over a window and flips borderline
    // ratios (the oracle below caught three missed triggers before this was float64)
    const c = new Float64Array(N);
    let acc = 0;
    for (let i = 0; i < N; i++) c[i] = acc += e[i];
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
{
    // Oracle: the same STA/LTA from sliding window sums (add the new sample, drop the old one) in float64, re-summed
    // exactly every 1000 samples so they cannot drift; it must find the same trigger windows to one sample.
    let same = true;
    traces.forEach((x, s) => {
        const e = Float64Array.from(x, (v) => v * v);
        const mean = e.reduce((a, b) => a + b, 0) / N;
        const sum = (a, b) => {
            let t = 0;
            for (let k = a; k < b; k++) t += e[k];
            return t;
        };
        const wins = [];
        let on = -1,
            sta = 0,
            lta = 0;
        for (let i = LTA; i < N; i++) {
            if ((i - LTA) % 1000 === 0) {
                sta = sum(i - STA + 1, i + 1);
                lta = sum(i - LTA + 1, i - STA + 1);
            } else {
                sta += e[i] - e[i - STA];
                lta += e[i - STA] - e[i - LTA];
            }
            const ratio = sta / mean / STA / Math.max(lta / mean / (LTA - STA), 1e-6);
            if (on < 0 && ratio > ON) on = i;
            else if (on >= 0 && ratio < OFF) {
                wins.push([on, i]);
                on = -1;
            }
        }
        if (on >= 0) wins.push([on, N]);
        const got = trig[s].windows.ranges();
        if (
            got.length !== wins.length ||
            got.some(([a, b], k) => Math.abs(a - wins[k][0]) > 1 || Math.abs(b - wins[k][1]) > 1)
        )
            same = false;
    });
    results["triggers equal an independent sliding-window STA/LTA (every window, to one sample)"] = same;
}
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
    // S: about three times P and later, so the largest swing in the 25 s after P belongs to it; the AIC picker then
    // runs from just after P to just past that peak (past it, the S coda's own decay is the larger variance change)
    for (const p of picks) {
        const at = Math.round(p.onset * HZ),
            coda = traces[p.st].subarray(at + HZ / 2, Math.min(N, at + 25 * HZ)),
            peak = at + HZ / 2 + argmax(abs(new Float32Array(coda.length), coda));
        p.s = aicPick(traces[p.st], at + HZ / 2, peak + HZ / 5) / HZ;
    }
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
// Geiger's method: Gauss-Newton on (x, y, t0) over every P and S arrival, started from the grid cell. Travel time
// to a station is hypot(dx, dy, 8) / v, so d/dx = dx / (r v); the 3x3 normal equations are solved by Cramer's rule.
function geiger(picks, start) {
    let x = start.x,
        y = start.y,
        t0 = picks[0].onset - Math.hypot(STATIONS[picks[0].st].x - x, STATIONS[picks[0].st].y - y, 8) / VP,
        rms = 0,
        iterations = 0;
    for (let it = 0; it < 20; it++) {
        iterations++;
        const A = [
                [0, 0, 0],
                [0, 0, 0],
                [0, 0, 0],
            ],
            g = [0, 0, 0];
        let ss = 0,
            n = 0;
        for (const p of picks)
            for (const [obs, v] of [
                [p.onset, VP],
                [p.s, VS],
            ]) {
                const st = STATIONS[p.st],
                    r = Math.hypot(x - st.x, y - st.y, 8),
                    res = obs - (t0 + r / v),
                    j = [(x - st.x) / (r * v), (y - st.y) / (r * v), 1];
                for (let a = 0; a < 3; a++) {
                    g[a] += j[a] * res;
                    for (let b = 0; b < 3; b++) A[a][b] += j[a] * j[b];
                }
                ss += res * res;
                n++;
            }
        rms = Math.sqrt(ss / n);
        const det = (M) =>
            M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
            M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
            M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
        const D = det(A),
            d = [0, 1, 2].map((c) => det(A.map((row, r) => row.map((v, k) => (k === c ? g[r] : v)))) / D);
        x += d[0];
        y += d[1];
        t0 += d[2];
        if (Math.hypot(d[0], d[1]) < 1e-6) break;
    }
    return { x, y, t0, rms, iterations };
}
const report = [];
let worstKm = 0,
    worstGeiger = 0,
    worstT0 = 0,
    worstS = 0,
    worstSP = 0,
    gridWorse = 0,
    maxIterations = 0,
    worstTwo = 0,
    worstMl = 0,
    worstPick = 0;
for (const ev of events) {
    const loc = locate(ev.picks);
    const quake = QUAKES.reduce((b, q) =>
        Math.hypot(q.x - loc.x, q.y - loc.y) < Math.hypot(b.x - loc.x, b.y - loc.y) ? q : b,
    );
    const km = Math.hypot(quake.x - loc.x, quake.y - loc.y);
    for (const p of ev.picks) {
        const r = dist(STATIONS[p.st], quake);
        worstPick = Math.max(worstPick, Math.abs(p.onset - (quake.t + r / VP)));
        worstS = Math.max(worstS, Math.abs(p.s - (quake.t + r / VS)));
        // S - P alone gives the distance from one station: r = (ts - tp) / (1/Vs - 1/Vp)
        worstSP = Math.max(worstSP, Math.abs((p.s - p.onset) / (1 / VS - 1 / VP) - r));
    }
    const fine = geiger(ev.picks, loc),
        fineKm = Math.hypot(quake.x - fine.x, quake.y - fine.y);
    worstGeiger = Math.max(worstGeiger, fineKm);
    maxIterations = Math.max(maxIterations, fine.iterations);
    // relocating with two stations from the network's grid cell: P alone is 2 equations for 3 unknowns (singular),
    // with S it is 4 (two distance circles still cross twice, so the start decides which crossing)
    const two = geiger(ev.picks.slice(0, 2), loc);
    worstTwo = Math.max(worstTwo, Math.hypot(quake.x - two.x, quake.y - two.y));
    worstT0 = Math.max(worstT0, Math.abs(fine.t0 - quake.t));
    if (fineKm > km) gridWorse++;
    ev.fine = fine;
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
        `${fine.x.toFixed(1)}, ${fine.y.toFixed(1)}`,
        `${quake.x}, ${quake.y}`,
        `${km.toFixed(1)} / ${fineKm.toFixed(2)}`,
        `${(fine.rms * 1000).toFixed(0)} ms`,
        ml.toFixed(2),
        quake.ml.toFixed(1),
        ev.picks.length,
    ]);
}
print(
    Table(report, {
        head: [
            "origin",
            "grid (km)",
            "Geiger (km)",
            "true (km)",
            "error grid/Geiger",
            "rms",
            "ML",
            "true ML",
            "stations",
        ],
    }),
);
results["P picks within 0.1 s of the true arrivals"] = worstPick < 0.1;
results["epicentres within 2 km on a 1 km grid"] = worstKm <= 2;
results[`S picks within 0.15 s (worst ${worstS.toFixed(3)} s)`] = worstS < 0.15;
results[`S-P times give each station's distance within 1.5 km (worst ${worstSP.toFixed(2)} km)`] = worstSP < 1.5;
results[`Geiger's method with P and S refines every epicentre to within 0.5 km (worst ${worstGeiger.toFixed(2)} km)`] =
    worstGeiger < 0.5 && gridWorse === 0;
results[`and recovers each origin time within 0.05 s (worst ${worstT0.toFixed(3)} s)`] = worstT0 < 0.05;
results[`Gauss-Newton converges quadratically: at most 6 iterations from the grid cell (took ${maxIterations})`] =
    maxIterations <= 6;
results[`two stations with P and S still locate every event within 2 km (worst ${worstTwo.toFixed(2)} km)`] =
    worstTwo < 2;
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
