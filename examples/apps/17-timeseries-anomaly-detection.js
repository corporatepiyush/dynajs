// 17 · Time-series anomaly detection — rolling baselines flag spikes in a metric stream.
//
// WHAT IT SHOWS
//   - dyna:dataframe window verbs: ROLLING_MEAN, ROLLING_STD, SHIFT, PCT_CHANGE
//   - comparing each point with the window BEFORE it, so a spike cannot hide inside its own baseline
//   - grouping consecutive flagged points into incidents
//   - dyna:random: a seeded generator, so the sample data (and the result) is reproducible
//
// RUN      dynajs examples/apps/17-timeseries-anomaly-detection.js

import { DataFrame } from "dyna:dataframe";
import { Random } from "dyna:random";

const WINDOW = 30;            // points in the baseline
const THRESHOLD = 6;          // standard deviations; a 30-point window estimates sigma loosely, so stay well above 3

// ---- sample data: a daily cycle, noise, and three injected incidents --------
const rng = new Random(42);
const N = 1440;               // one reading per minute for a day
const ts = new Int32Array(N);
const latency = new Float64Array(N);
for (let i = 0; i < N; i++) {
    ts[i] = 1767225600 + i * 60;
    latency[i] = 120 + 25 * Math.sin((2 * Math.PI * i) / N) + rng.normal(0, 4);
}
const injected = [[300, 5, 90], [800, 12, 60], [1200, 3, 150]];     // [start, length, extra ms]
for (const [start, len, extra] of injected)
    for (let i = start; i < start + len; i++) latency[i] += extra;

const df = new DataFrame({ ts, latency });

// ---- detection -------------------------------------------------------------
// The rolling statistics at row i include row i itself. Shifting them by one
// gives "the baseline as it stood just before this reading".
const mean = df.ROLLING_MEAN("latency", WINDOW);
const std = df.ROLLING_STD("latency", WINDOW);
const score = new Float64Array(N);
for (let i = WINDOW; i < N; i++) {
    const baselineMean = mean[i - 1], baselineStd = std[i - 1];
    score[i] = baselineStd > 0 ? (latency[i] - baselineMean) / baselineStd : 0;
}

// Merge flagged points that sit close together into one incident; an
// operator wants three pages, not twenty.
const incidents = [];
for (let i = 0; i < N; i++) {
    if (score[i] < THRESHOLD) continue;
    const last = incidents[incidents.length - 1];
    if (last && i - last.end <= 15) { last.end = i; last.peak = Math.max(last.peak, latency[i]); }
    else incidents.push({ start: i, end: i, peak: latency[i] });
}

const clock = (i) => new Date(ts[i] * 1000).toISOString().slice(11, 16);
console.log(`${N} readings, baseline ${df.MEAN("latency").toFixed(1)}ms, ${incidents.length} incident(s)`);
for (const inc of incidents)
    console.log(`  ${clock(inc.start)} UTC  peak ${inc.peak.toFixed(0)}ms  first seen at index ${inc.start}`);

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
check(incidents.length === injected.length, "exactly the injected incidents are found: " + incidents.length);
for (const [start] of injected)
    check(incidents.some((inc) => inc.start === start), "the incident at " + start + " is caught at its first point");
// The same seed must give the same series, which is what makes this testable.
check(Math.abs(new Random(42).normal(0, 4) - (latency[0] - 120)) < 1e-9, "the generator is reproducible");
console.log("self-test passed");
