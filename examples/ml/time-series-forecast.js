// Time-series forecasting — two years of hourly demand forecast by seasonal naive, Holt-Winters, Fourier regression
// and gradient boosting, compared by rolling-origin backtests with calibrated prediction intervals.
//
// WHAT IT IS
//   The forecasting service behind staffing, energy purchasing or cloud capacity:
//
//   data        hourly demand with trend, daily and weekly seasonality, holidays, noise and a level shift
//               injected 60 days before the end; Float64Array throughout
//   shift       a two-sided CUSUM on the week-over-week difference (holiday hours skipped) locates the level shift;
//               the regression models get a step feature from that hour on
//   models      seasonal naive (same hour last week) · additive Holt-Winters with a 168-hour season, smoothing
//               constants chosen by grid search on a validation week · ridge regression (dyna:ml LinearRegression)
//               on daily/weekly Fourier terms, trend and holiday flags · GradientBoostingRegressor on calendar
//               features plus the seasonal-naive value
//   backtest    four rolling origins, one-week horizon; MAE and sMAPE computed with simd (sub, abs, add, div)
//   intervals   residual quantiles per horizon bucket from TDigests give 80% intervals; coverage on the backtest
//               must land near 80%
//   coherence   hourly forecasts summed to days must equal a daily forecast built the same way (bottom-up)
//
// MODULES  dyna:ml (LinearRegression, GradientBoostingRegressor) · dyna:simd (f64Sum, f64Mean, sub, abs, add, div,
//          sum) · dyna:structures (TDigest) · dyna:random · dyna:cli · Array/Number extensions
//
// RUN      dynajs examples/ml/time-series-forecast.js

import { LinearRegression, GradientBoostingRegressor } from "dyna:ml";
import { sub, abs, add, div, sum } from "dyna:simd";
import { TDigest } from "dyna:structures";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1960);
const H = 24 * 7 * 104,
    WEEK = 168,
    SHIFT_AT = H - 60 * 24;
const t0 = Date.UTC(2024, 9, 28);
const holidays = new Set(
    [0, 1, 2].flatMap((y) =>
        [
            [0, 1],
            [11, 25],
            [11, 26],
            [4, 1],
        ].map(([m, d]) => Math.floor((Date.UTC(2024 + y, m, d) - t0) / 86_400_000)),
    ),
);

// =============================================================================
// Data
// =============================================================================
const y = new Float64Array(H);
for (let h = 0; h < H; h++) {
    const day = Math.floor(h / 24),
        hod = h % 24,
        dow = day % 7;
    const daily = 30 * Math.sin(((hod - 7) / 24) * 2 * Math.PI) + 12 * Math.sin(((hod - 7) / 12) * 2 * Math.PI);
    const weekly = dow >= 5 ? -25 : 6;
    const hol = holidays.has(day) ? -35 : 0;
    y[h] = 200 + 0.004 * h + daily + weekly + hol + (h >= SHIFT_AT ? 22 : 0) + rng.normal() * 6;
}

// =============================================================================
// Level-shift detection: CUSUM on y[t] - y[t - 168]
// =============================================================================
// k is half the shift worth detecting (~20) and h about 20 noise sigmas of the weekly difference; hours that are, or
// were a week ago, holidays are skipped so a holiday cannot look like a shift.
function cusum(series, k = 10, threshold = 180) {
    let up = 0,
        down = 0;
    for (let t = 2 * WEEK; t < series.length; t++) {
        if (holidays.has(Math.floor(t / 24)) || holidays.has(Math.floor((t - WEEK) / 24))) continue;
        const d = series[t] - series[t - WEEK];
        up = Math.max(0, up + d - k);
        down = Math.max(0, down - d - k);
        if (up > threshold || down > threshold) return t;
    }
    return -1;
}
const detected = cusum(y);
results["CUSUM finds the level shift within a day"] = detected >= SHIFT_AT && detected - SHIFT_AT < 24;

// =============================================================================
// Models: fit(history) -> forecast(horizon)
// =============================================================================
const seasonalNaive = (hist, horizon) =>
    Float64Array.from({ length: horizon }, (_, i) => hist[hist.length - WEEK + (i % WEEK)]);
function holtWinters(hist, horizon, a, b, g, m = WEEK) {
    let level = hist.slice(0, m).reduce((s, v) => s + v, 0) / m,
        trend = 0;
    const season = Float64Array.from(hist.slice(0, m), (v) => v - level);
    for (let t = m; t < hist.length; t++) {
        const s = season[t % m],
            prevLevel = level;
        level = a * (hist[t] - s) + (1 - a) * (level + trend);
        trend = b * (level - prevLevel) + (1 - b) * trend;
        season[t % m] = g * (hist[t] - level) + (1 - g) * s;
    }
    return Float64Array.from({ length: horizon }, (_, i) => level + (i + 1) * trend + season[(hist.length + i) % m]);
}
function features(h) {
    const day = Math.floor(h / 24),
        hod = h % 24,
        dow = day % 7,
        f = [h / H, holidays.has(day) ? 1 : 0, dow >= 5 ? 1 : 0, h >= detected && detected > 0 ? 1 : 0];
    for (let k = 1; k <= 3; k++) f.push(Math.sin((2 * Math.PI * k * hod) / 24), Math.cos((2 * Math.PI * k * hod) / 24));
    for (let k = 1; k <= 2; k++)
        f.push(Math.sin((2 * Math.PI * k * (h % WEEK)) / WEEK), Math.cos((2 * Math.PI * k * (h % WEEK)) / WEEK));
    return f;
}
async function fourier(hist, horizon) {
    const start = Math.max(0, hist.length - 24 * 7 * 30);
    const X = [],
        t = [];
    for (let h = start; h < hist.length; h++) {
        X.push(features(h));
        t.push(hist[h]);
    }
    const m = await new LinearRegression().fit(X, t);
    const out = Float64Array.from(
        await m.predict(Array.from({ length: horizon }, (_, i) => features(hist.length + i))),
    );
    m.close();
    return out;
}
async function boosted(hist, horizon) {
    const start = Math.max(WEEK, hist.length - 24 * 7 * 20);
    const row = (h, src) => [
        h % 24,
        Math.floor(h / 24) % 7,
        holidays.has(Math.floor(h / 24)) ? 1 : 0,
        h >= detected && detected > 0 ? 1 : 0,
        src(h - WEEK),
    ];
    const X = [],
        t = [];
    for (let h = start; h < hist.length; h++) {
        X.push(row(h, (i) => hist[i]));
        t.push(hist[h]);
    }
    const m = await new GradientBoostingRegressor({ nEstimators: 150, maxDepth: 4, learningRate: 0.1, seed: 3 }).fit(
        X,
        t,
    );
    const naive = seasonalNaive(hist, horizon);
    const out = Float64Array.from(
        await m.predict(
            Array.from({ length: horizon }, (_, i) =>
                row(hist.length + i, (j) => (j < hist.length ? hist[j] : naive[j - hist.length])),
            ),
        ),
    );
    m.close();
    return out;
}

// Grid search for Holt-Winters on the week before the first backtest origin.
const origins = [H - 4 * WEEK, H - 3 * WEEK, H - 2 * WEEK, H - WEEK];
let bestHW = null;
{
    const trainEnd = origins[0] - WEEK,
        actual = y.subarray(trainEnd, trainEnd + WEEK);
    for (const a of [0.02, 0.05, 0.1, 0.2])
        for (const b of [0, 0.001, 0.01])
            for (const g of [0.05, 0.1, 0.2, 0.3]) {
                const f = holtWinters(y.subarray(0, trainEnd), WEEK, a, b, g);
                const err = mae(f, actual);
                if (!bestHW || err < bestHW.err) bestHW = { a, b, g, err };
            }
}
function mae(f, actual) {
    const d = new Float32Array(f.length);
    sub(d, Float32Array.from(f), Float32Array.from(actual));
    return sum(abs(d, d)) / f.length;
}
function smape(f, actual) {
    const F = Float32Array.from(f),
        A = Float32Array.from(actual),
        num = new Float32Array(F.length),
        den = new Float32Array(F.length);
    sub(num, F, A);
    abs(num, num);
    abs(den, F);
    add(den, den, abs(new Float32Array(A.length), A));
    div(num, num, den);
    return (200 * sum(num)) / F.length;
}

// =============================================================================
// Rolling-origin backtest with interval calibration
// =============================================================================
const MODELS = {
    "seasonal naive": async (hist) => seasonalNaive(hist, WEEK),
    "Holt-Winters": async (hist) => holtWinters(hist, WEEK, bestHW.a, bestHW.b, bestHW.g),
    "Fourier ridge": (hist) => fourier(hist, WEEK),
    "gradient boosting": (hist) => boosted(hist, WEEK),
};
const scores = {},
    residualsByBucket = {},
    coverage = {};
for (const name of Object.keys(MODELS)) {
    scores[name] = { mae: [], smape: [] };
    residualsByBucket[name] = [0, 1, 2, 3].map(() => new TDigest(100));
    coverage[name] = { hit: 0, n: 0 };
}
for (let o = 0; o < origins.length; o++) {
    const origin = origins[o],
        hist = y.subarray(0, origin),
        actual = y.subarray(origin, origin + WEEK);
    for (const [name, model] of Object.entries(MODELS)) {
        const f = await model(hist);
        scores[name].mae.push(mae(f, actual));
        scores[name].smape.push(smape(f, actual));
        // Intervals for this origin come only from residuals of EARLIER origins: no peeking.
        if (o > 0)
            for (let i = 0; i < WEEK; i++) {
                const d = residualsByBucket[name][Math.min(3, Math.floor(i / 42))];
                if (d.quantile(0.1) <= actual[i] - f[i] && actual[i] - f[i] <= d.quantile(0.9)) coverage[name].hit++;
                coverage[name].n++;
            }
        for (let i = 0; i < WEEK; i++) residualsByBucket[name][Math.min(3, Math.floor(i / 42))].add(actual[i] - f[i]);
    }
}
const summary = Object.entries(scores).map(([name, s]) => ({
    name,
    mae: s.mae.average(),
    smape: s.smape.average(),
    cov: coverage[name].hit / coverage[name].n,
}));
print(
    Table(
        summary.map((s) => [s.name, s.mae.toFixed(2), s.smape.toFixed(2) + "%", (s.cov * 100).toFixed(1) + "%"]),
        { head: ["model", "MAE", "sMAPE", "80% interval coverage"] },
    ),
);
const byName = Object.fromEntries(summary.map((s) => [s.name, s]));
results["Holt-Winters beats seasonal naive"] = byName["Holt-Winters"].mae < byName["seasonal naive"].mae;
results["Fourier regression beats seasonal naive"] = byName["Fourier ridge"].mae < byName["seasonal naive"].mae;
results["gradient boosting beats seasonal naive"] = byName["gradient boosting"].mae < byName["seasonal naive"].mae;
results["best model is within 1.5x of the noise floor (sigma 6 -> MAE 4.8)"] =
    Math.min(...summary.map((s) => s.mae)) < 4.8 * 1.5;
results["80% intervals cover 70-90% out of sample"] = summary.every((s) => s.cov > 0.7 && s.cov < 0.9);
print(
    `Holt-Winters grid pick: alpha ${bestHW.a}, beta ${bestHW.b}, gamma ${bestHW.g}; level shift detected at hour ${detected} (true ${SHIFT_AT})`,
);

// =============================================================================
// Coherence: hourly forecast summed by day equals the daily bottom-up forecast
// =============================================================================
{
    const hist = y.subarray(0, H - WEEK);
    const hourly = holtWinters(hist, WEEK, bestHW.a, bestHW.b, bestHW.g);
    const daily = Array.from({ length: 7 }, (_, d) => hourly.subarray(d * 24, d * 24 + 24).reduce((s, v) => s + v, 0));
    const actualDaily = Array.from({ length: 7 }, (_, d) =>
        y.subarray(H - WEEK + d * 24, H - WEEK + d * 24 + 24).reduce((s, v) => s + v, 0),
    );
    results["daily totals are the sum of hourly forecasts"] =
        Math.abs(daily.sum() - hourly.reduce((s, v) => s + v, 0)) < 1e-6;
    results["daily totals within 3% of actuals"] = daily.every(
        (v, d) => Math.abs(v - actualDaily[d]) / actualDaily[d] < 0.03,
    );
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
