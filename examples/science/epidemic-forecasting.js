// Epidemic forecasting — an age-structured SEIR model integrated with RK4, fitted to noisy reported cases by
// Nelder–Mead, the time-varying reproduction number estimated with the Cori et al. Bayesian method, reporting-delay
// nowcasting, an ensemble forecast with prediction intervals, intervention scenarios, and an SVG report.
//
// WHAT IT IS
//   The weekly modelling a public-health team does when cases start rising:
//
//   data         daily reported cases from a CSV — generated here from a hidden "true" epidemic with
//                under-reporting, a weekday reporting pattern, a reporting delay, and negative-binomial noise
//   R0           from a 3-group contact matrix (children, adults, elderly) as the dominant eigenvalue of the
//                next-generation matrix (power iteration)
//   fitting      β, the initial seed and the reporting fraction are fitted by Nelder–Mead on the 7-day smoothed
//                log-cases (the weekday pattern is a nuisance the smoothing removes)
//   Rt           Cori et al. (2013): Gamma posterior for R over a sliding 7-day window, with a discretised
//                serial-interval distribution; the 95% credible interval comes from the inverse incomplete gamma
//   nowcast      the last days are incomplete because of reporting delay; they are corrected by the empirical
//                delay distribution so the trend at the edge is not an artefact
//   forecast     an ensemble of 200 runs drawn from the fit's uncertainty projects 28 days ahead, giving median
//                and 90% intervals; three intervention scenarios change contacts from a future date
//   report       a CSV of everything plus an HTML page with inline SVG charts
//   ensemble     the 200 forecast runs are integrated TOGETHER: every compartment is a Float32Array with one lane per
//                run, so each RK4 stage is a few dozen dyna:simd calls (axpy, mul, scale) over 200 lanes instead of
//                200 separate interpreted integrations; incidence is accumulated from the S-derivatives directly,
//                which also avoids subtracting two large float32 populations
//
// MODULES  dyna:mathx (gammaincinv, gammaln, stats) · dyna:dataframe (ROLLING_MEAN) · dyna:csv · dyna:html (Template)
//          · dyna:random · dyna:cli (Table) · dyna:file · dyna:simd (dot, axpy, mul, scale)
//
// RUN      dynajs examples/science/epidemic-forecasting.js     simulates an outbreak, fits it and forecasts 4 weeks

import { gammaincinv, gammaln, stats } from "dyna:mathx";
import { DataFrame } from "dyna:dataframe";
import * as csv from "dyna:csv";
import { Template } from "dyna:html";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { makeTempDir, writeFile, readFile, removeAll } from "dyna:file";
import * as simd from "dyna:simd";
const { dot } = simd;
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("epi");
const rng = new Random(2020);

// =============================================================================
// Population and contact structure
// =============================================================================
const N = [1.8e6, 5.6e6, 1.9e6]; // children, adults, 65+
const CONTACTS = [
    [7.5, 4.2, 0.6],
    [1.35, 6.8, 0.9],
    [0.55, 2.8, 2.1],
]; // daily contacts of group i with group j
const SUSC = [0.6, 1.0, 1.2],
    LATENT = 3,
    INFECTIOUS = 5;
function r0(beta, contacts = CONTACTS) {
    // dominant eigenvalue of the next-generation matrix
    const K = contacts.map((row, i) => row.map((c) => beta * SUSC[i] * c * INFECTIOUS));
    let v = new Float32Array([1, 1, 1]),
        lambda = 0;
    for (let it = 0; it < 200; it++) {
        const next = new Float32Array(K.map((row) => dot(new Float32Array(row), v)));
        lambda = Math.hypot(...next) / Math.hypot(...v);
        v = next.map((x) => x / Math.hypot(...next));
    }
    return lambda;
}

// =============================================================================
// SEIR model with RK4
// =============================================================================
function simulate({ beta, seed, days, contactsAt = () => CONTACTS }) {
    let y = [N[0], N[1] - seed, N[2], 0, seed, 0, 0, 0, 0, 0, 0, 0]; // S(3) E(3) I(3) R(3), seeded in adults
    const incidence = [];
    const f = (t, s) => {
        const C = contactsAt(t),
            d = new Array(12).fill(0),
            inf = [];
        for (let i = 0; i < 3; i++) {
            let lam = 0;
            for (let j = 0; j < 3; j++) lam += (beta * C[i][j] * s[6 + j]) / N[j];
            inf.push(SUSC[i] * lam * s[i]);
        }
        for (let i = 0; i < 3; i++) {
            d[i] = -inf[i];
            d[3 + i] = inf[i] - s[3 + i] / LATENT;
            d[6 + i] = s[3 + i] / LATENT - s[6 + i] / INFECTIOUS;
            d[9 + i] = s[6 + i] / INFECTIOUS;
        }
        return d;
    };
    const h = 0.25;
    for (let day = 0; day < days; day++) {
        let newInf = 0;
        for (let k = 0; k < 4; k++) {
            const t = day + k * h,
                add = (a, b, c) => a.map((x, i) => x + c * b[i]);
            const k1 = f(t, y),
                k2 = f(t + h / 2, add(y, k1, h / 2)),
                k3 = f(t + h / 2, add(y, k2, h / 2)),
                k4 = f(t + h, add(y, k3, h));
            const next = y.map((x, i) => x + (h / 6) * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]));
            newInf += [0, 1, 2].reduce((s, i) => s + (y[i] - next[i]), 0);
            y = next;
        }
        incidence.push(newInf);
    }
    return { incidence, final: y };
}

// =============================================================================
// The hidden truth and the reported data
// =============================================================================
const TRUE = { beta: 0.034, seed: 40, report: 0.35 };
const LOCKDOWN_DAY = 70;
const truthContacts = (t) => (t >= LOCKDOWN_DAY ? CONTACTS.map((row) => row.map((c) => c * 0.62)) : CONTACTS);
const OBS_DAYS = 84;
const truth = simulate({ ...TRUE, days: OBS_DAYS + 28, contactsAt: truthContacts });
const WEEKDAY = [1.15, 1.1, 1.05, 1.0, 0.95, 0.6, 0.55];
const DELAY = [0.25, 0.35, 0.2, 0.1, 0.06, 0.04]; // share of a day's cases reported d days later
const negbin = (mean, k) => {
    const lam = rng.normal(1, 1 / Math.sqrt(k)) * mean;
    return rng.poisson(Math.max(0, lam));
};
const reported = Array.from({ length: OBS_DAYS }, (_, d) =>
    negbin(truth.incidence[d] * TRUE.report * WEEKDAY[d % 7], 30),
);
// Cases by onset date as known on day OBS_DAYS: recent days miss what has not been reported yet.
const byOnset = reported.map((c, d) => {
    const known = DELAY.slice(0, Math.max(1, OBS_DAYS - d)).reduce((a, b) => a + b, 0);
    return Math.round(c * Math.min(1, known));
});
const start = Date.UTC(2026, 1, 2),
    dateOf = (d) => new Date(start + d * 86400_000).toISOString().slice(0, 10);
writeFile(work.join("cases.csv"), csv.stringify([["date", "cases"], ...byOnset.map((c, d) => [dateOf(d), String(c)])]));

// =============================================================================
// Load, nowcast, smooth
// =============================================================================
const loaded = csv.parse(readFile(work.join("cases.csv")), { hasHeader: true }).rows.map((r) => Number(r[1]));
const nowcast = loaded.map((c, d) => {
    const known = DELAY.slice(0, Math.max(1, OBS_DAYS - d)).reduce((a, b) => a + b, 0);
    return c / Math.min(1, known);
});
const smooth = (xs) =>
    Array.from(new DataFrame({ x: Float64Array.from(xs) }).ROLLING_MEAN("x", 7), (v, i) =>
        Number.isFinite(v) ? v : stats.mean(xs.slice(0, i + 1)),
    );
const smoothed = smooth(nowcast);

// =============================================================================
// Fit by Nelder–Mead on log smoothed cases (before the intervention takes effect)
// =============================================================================
function nelderMead(f, x0, { iters = 400, step = 0.2 } = {}) {
    let simplex = [
        x0,
        ...x0.map((_, i) => x0.map((v, j) => (i === j ? v * (1 + step) + (v === 0 ? step : 0) : v))),
    ].map((x) => ({ x, f: f(x) }));
    for (let it = 0; it < iters; it++) {
        simplex.sort((a, b) => a.f - b.f);
        const n = x0.length,
            best = simplex[0],
            worst = simplex[n],
            centroid = x0.map((_, j) => simplex.slice(0, n).reduce((s, p) => s + p.x[j], 0) / n);
        const pt = (a) => centroid.map((c, j) => c + a * (worst.x[j] - c)),
            tryP = (a) => {
                const x = pt(a);
                return { x, f: f(x) };
            };
        const r = tryP(-1);
        if (r.f < best.f) {
            const e = tryP(-2);
            simplex[n] = e.f < r.f ? e : r;
        } else if (r.f < simplex[n - 1].f) simplex[n] = r;
        else {
            const c = tryP(0.5);
            if (c.f < worst.f) simplex[n] = c;
            else
                simplex = simplex
                    .map((p, i) => (i === 0 ? p : { x: p.x.map((v, j) => best.x[j] + 0.5 * (v - best.x[j])), f: 0 }))
                    .map((p, i) => (i === 0 ? p : { ...p, f: f(p.x) }));
        }
        if (Math.abs(simplex[n].f - simplex[0].f) < 1e-10) break;
    }
    return simplex.sort((a, b) => a.f - b.f)[0];
}
const FIT_DAYS = LOCKDOWN_DAY - 3;
const loss = ([beta, logSeed, report]) => {
    if (beta <= 0 || report <= 0.02 || report > 1) return 1e9;
    const sim = simulate({ beta, seed: Math.exp(logSeed), days: FIT_DAYS }).incidence;
    let s = 0;
    for (let d = 10; d < FIT_DAYS; d++) s += (Math.log(1 + sim[d] * report) - Math.log(1 + smoothed[d])) ** 2;
    return s;
};
const fit = nelderMead(loss, [0.025, Math.log(10), 0.5]);
const [betaHat, logSeedHat, reportHat] = fit.x;
// Contact reduction after the intervention, fitted on the remaining observed days with β fixed.
const lossReduction = ([m]) => {
    if (m <= 0.1 || m > 1.2) return 1e9;
    const sim = simulate({
        beta: betaHat,
        seed: Math.exp(logSeedHat),
        days: OBS_DAYS - 3,
        contactsAt: (t) => (t >= LOCKDOWN_DAY ? CONTACTS.map((r) => r.map((c) => c * m)) : CONTACTS),
    }).incidence;
    let s = 0;
    for (let d = LOCKDOWN_DAY; d < OBS_DAYS - 3; d++)
        s += (Math.log(1 + sim[d] * reportHat) - Math.log(1 + smoothed[d])) ** 2;
    return s;
};
const mHat = nelderMead(lossReduction, [0.8]).x[0];

// =============================================================================
// Rt by Cori et al.
// =============================================================================
const SI_MEAN = 6.5,
    SI_SD = 3.0,
    siShape = (SI_MEAN / SI_SD) ** 2,
    siScale = SI_SD ** 2 / SI_MEAN;
const gammaPdf = (x, k, th) => Math.exp((k - 1) * Math.log(x) - x / th - gammaln(k) - k * Math.log(th));
const w = Array.from({ length: 21 }, (_, s) => (s === 0 ? 0 : gammaPdf(s, siShape, siScale)));
const wsum = w.reduce((a, b) => a + b, 0);
for (let s = 0; s < w.length; s++) w[s] /= wsum;
const PRIOR_A = 1,
    PRIOR_B = 5,
    WINDOW = 7;
const rt = [];
for (let t = WINDOW + 7; t < OBS_DAYS; t++) {
    let a = PRIOR_A,
        invB = 1 / PRIOR_B;
    for (let s = t - WINDOW + 1; s <= t; s++) {
        a += nowcast[s];
        let lam = 0;
        for (let k = 1; k < w.length && s - k >= 0; k++) lam += nowcast[s - k] * w[k];
        invB += lam;
    }
    const scaleP = 1 / invB;
    rt.push({ day: t, mean: a * scaleP, lo: gammaincinv(0.025, a) * scaleP, hi: gammaincinv(0.975, a) * scaleP });
}

// =============================================================================
// Ensemble forecast and scenarios
// =============================================================================
const HORIZON = 28;
const forecastRun = (beta, seed, report, m, scenarioFrom = Infinity, scenarioM = m) =>
    simulate({
        beta,
        seed,
        days: OBS_DAYS + HORIZON,
        contactsAt: (t) =>
            t >= scenarioFrom
                ? CONTACTS.map((r) => r.map((c) => c * scenarioM))
                : t >= LOCKDOWN_DAY
                  ? CONTACTS.map((r) => r.map((c) => c * m))
                  : CONTACTS,
    }).incidence.map((x) => x * report);
// All runs at once: compartment v of run r lives in state[v][r].
function simulateBatch({ betas, seeds, mults, days }) {
    const R = betas.length,
        vec = () => new Float32Array(R),
        nine = () => Array.from({ length: 9 }, vec);
    const state = nine(),
        tmp = nine(),
        k = [nine(), nine(), nine(), nine()],
        P = [vec(), vec(), vec()],
        T = vec(),
        betaNow = vec();
    state[0].fill(N[0]);
    state[2].fill(N[2]);
    for (let r = 0; r < R; r++) {
        state[1][r] = N[1] - seeds[r];
        state[4][r] = seeds[r];
    }
    const deriv = (s, t, out) => {
        for (let r = 0; r < R; r++) betaNow[r] = betas[r] * (t >= LOCKDOWN_DAY ? mults[r] : 1);
        for (let j = 0; j < 3; j++) {
            P[j].set(s[6 + j]);
            simd.scale(P[j], 1 / N[j]);
        }
        for (let i = 0; i < 3; i++) {
            T.fill(0);
            for (let j = 0; j < 3; j++) simd.axpy(T, CONTACTS[i][j], P[j]);
            simd.mul(T, T, betaNow);
            simd.mul(T, T, s[i]);
            simd.scale(T, SUSC[i]); // new infections in group i
            out[i].set(T);
            simd.scale(out[i], -1);
            out[3 + i].set(T);
            simd.axpy(out[3 + i], -1 / LATENT, s[3 + i]);
            out[6 + i].set(s[3 + i]);
            simd.scale(out[6 + i], 1 / LATENT);
            simd.axpy(out[6 + i], -1 / INFECTIOUS, s[6 + i]);
        }
    };
    const stage = (kk, c) => {
        for (let v = 0; v < 9; v++) {
            tmp[v].set(state[v]);
            simd.axpy(tmp[v], c, kk[v]);
        }
        return tmp;
    };
    const h = 0.25,
        incidence = Array.from({ length: days }, vec);
    for (let day = 0; day < days; day++) {
        for (let q = 0; q < 4; q++) {
            const t = day + q * h;
            deriv(state, t, k[0]);
            deriv(stage(k[0], h / 2), t + h / 2, k[1]);
            deriv(stage(k[1], h / 2), t + h / 2, k[2]);
            deriv(stage(k[2], h), t + h, k[3]);
            for (let v = 0; v < 9; v++) [h / 6, h / 3, h / 3, h / 6].forEach((c, n) => simd.axpy(state[v], c, k[n][v]));
            for (let i = 0; i < 3; i++)
                [h / 6, h / 3, h / 3, h / 6].forEach((c, n) => simd.axpy(incidence[day], -c, k[n][i]));
        }
    }
    return incidence; // incidence[day][run]
}
const draws = Array.from({ length: 200 }, () => [
    betaHat * Math.exp(rng.normal(0, 0.02)),
    Math.exp(logSeedHat + rng.normal(0, 0.05)),
    mHat * Math.exp(rng.normal(0, 0.04)),
]);
const batch = simulateBatch({
    betas: draws.map((d) => d[0]),
    seeds: draws.map((d) => d[1]),
    mults: draws.map((d) => d[2]),
    days: OBS_DAYS + HORIZON,
});
const ensemble = draws.map((_, r) => batch.map((day) => negbin(day[r] * reportHat, 30)));
const band = (d) => {
    const vals = ensemble.map((e) => e[d]);
    return [stats.quantile(vals, 0.05), stats.median(vals), stats.quantile(vals, 0.95)];
};
const SCEN_DAY = OBS_DAYS + 7;
const scenarios = { "status quo": mHat, "relax (+30% contacts)": mHat * 1.3, "tighten (-25% contacts)": mHat * 0.75 };
const scenarioPeaks = Object.fromEntries(
    Object.entries(scenarios).map(([k, m]) => {
        const run = forecastRun(betaHat, Math.exp(logSeedHat), reportHat, mHat, SCEN_DAY, m);
        return [k, run.slice(OBS_DAYS).reduce((a, b) => a + b, 0)];
    }),
);

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const R0true = r0(TRUE.beta),
    R0hat = r0(betaHat);
results[`R0 recovered: ${R0hat.toFixed(2)} vs true ${R0true.toFixed(2)}`] = Math.abs(R0hat - R0true) / R0true < 0.1;
results[`reporting fraction ${reportHat.toFixed(2)} vs true ${TRUE.report}`] = Math.abs(reportHat - TRUE.report) < 0.12;
results[`the intervention's contact reduction is estimated at ${((1 - mHat) * 100).toFixed(0)}% (true 38%)`] =
    Math.abs(mHat - 0.62) < 0.1;
const before = rt.filter((r) => r.day < LOCKDOWN_DAY).at(-1),
    after = rt.at(-1);
results[
    `Rt fell from ${before.mean.toFixed(2)} [${before.lo.toFixed(2)}, ${before.hi.toFixed(2)}] to ${after.mean.toFixed(2)} [${after.lo.toFixed(2)}, ${after.hi.toFixed(2)}]`
] = before.mean > 1.1 && after.mean < 1 && after.hi < before.lo;
results["the nowcast corrects the incomplete last days"] =
    Math.abs(nowcast.at(-1) - reported.at(-1)) < Math.abs(loaded.at(-1) - reported.at(-1)) + 1;
const truthFuture = truth.incidence.slice(OBS_DAYS, OBS_DAYS + HORIZON).map((x) => x * TRUE.report);
const covered = truthFuture.filter((x, i) => {
    const [lo, , hi] = band(OBS_DAYS + i);
    return x >= lo * 0.9 && x <= hi * 1.1;
}).length;
results[`the 90% forecast band covers the true future on ${covered}/${HORIZON} days`] = covered >= HORIZON * 0.75;
const scalarRun = forecastRun(draws[7][0], draws[7][1], 1, draws[7][2]);
const worstLane = Math.max(...scalarRun.map((x, d) => Math.abs(batch[d][7] - x) / Math.max(1, x)));
results[
    `a lane of the batched float32 ensemble tracks the scalar float64 integration (worst relative gap ${worstLane.toExponential(1)})`
] = worstLane < 5e-3;
results["scenarios order as expected"] =
    scenarioPeaks["relax (+30% contacts)"] > scenarioPeaks["status quo"] &&
    scenarioPeaks["status quo"] > scenarioPeaks["tighten (-25% contacts)"];

const svg = (series, colors, h = 160, wpx = 640) => {
    const max = Math.max(...series.flat().filter(Number.isFinite)),
        len = Math.max(...series.map((s) => s.length));
    const path = (s) => s.map((v, i) => `${i ? "L" : "M"}${(i / (len - 1)) * wpx},${h - (v / max) * h}`).join("");
    return `<svg width="${wpx}" height="${h}" xmlns="http://www.w3.org/2000/svg">${series.map((s, i) => `<path d="${path(s)}" fill="none" stroke="${colors[i]}" stroke-width="1.5"/>`).join("")}</svg>`;
};
const forecastMedian = Array.from({ length: OBS_DAYS + HORIZON }, (_, d) => band(d)[1]);
const REPORT =
    new Template(`<!doctype html><meta charset="utf-8"><title>Outbreak report</title><h1>Outbreak situation report</h1>
<p>R0 {{r0}}; reporting {{report}}%; current Rt {{rt}} (95% CrI {{lo}}–{{hi}})</p><h2>Cases and forecast</h2>{{{cases}}}<h2>Rt</h2>{{{rtChart}}}`);
writeFile(
    work.join("report.html"),
    REPORT.render({
        r0: R0hat.toFixed(2),
        report: (reportHat * 100).toFixed(0),
        rt: after.mean.toFixed(2),
        lo: after.lo.toFixed(2),
        hi: after.hi.toFixed(2),
        cases: svg([loaded, nowcast, forecastMedian], ["#999", "#06c", "#c30"]),
        rtChart: svg([rt.map((r) => r.mean), rt.map((r) => r.lo), rt.map((r) => r.hi)], ["#000", "#aaa", "#aaa"]),
    }),
);
writeFile(
    work.join("forecast.csv"),
    csv.stringify([
        ["date", "reported", "nowcast", "p05", "median", "p95"],
        ...Array.from({ length: OBS_DAYS + HORIZON }, (_, d) => [
            dateOf(d),
            d < OBS_DAYS ? String(loaded[d]) : "",
            d < OBS_DAYS ? nowcast[d].toFixed(1) : "",
            ...band(d).map((x) => x.toFixed(0)),
        ]),
    ]),
);

print("epidemic-forecasting");
print(
    `  fit        beta ${betaHat.toFixed(4)} (true ${TRUE.beta}), R0 ${R0hat.toFixed(2)} (true ${R0true.toFixed(2)}), seed ${Math.exp(logSeedHat).toFixed(0)}, reporting ${(reportHat * 100).toFixed(0)}%, contacts after day ${LOCKDOWN_DAY}: x${mHat.toFixed(2)}`,
);
print(
    table([
        ["week", "reported", "Rt mean", "95% CrI"],
        ...rt
            .filter((r) => r.day % 7 === 0)
            .map((r) => [
                dateOf(r.day),
                String(loaded.slice(r.day - 6, r.day + 1).reduce((a, b) => a + b, 0)),
                r.mean.toFixed(2),
                `${r.lo.toFixed(2)}–${r.hi.toFixed(2)}`,
            ]),
    ]),
);
print(
    table([
        ["scenario (from " + dateOf(SCEN_DAY) + ")", "reported cases in the next 28 days"],
        ...Object.entries(scenarioPeaks).map(([k, v]) => [k, v.toFixed(0)]),
    ]),
);
print(
    `  report     ${readFile(work.join("report.html")).length} bytes of HTML with SVG charts; forecast.csv ${OBS_DAYS + HORIZON} rows`,
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
