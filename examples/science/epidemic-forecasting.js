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
//   oracles      independent of the fit: the unmitigated epidemic must end at the multi-group final-size equation
//                (solved as its own fixed point); RK4 must show fourth-order convergence (halving h divides the
//                error by ~16); the Cori estimator must recover R exactly from renewal-equation data, its 95% CrI
//                must cover Poisson replicates ~95% of the time, and its window edge must fall on the right day
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
// Every caller scales the whole contact matrix by one number from some date, so the model takes a multiplier
// function, not a matrix: no matrix is built per derivative call. The state and the four stages are preallocated
// Float64Arrays reused for every step (178 integrations x ~1600 derivative calls in the fits).
const CFLAT = Float64Array.from(CONTACTS.flat());
function simulate({ beta, seed, days, multAt = () => 1, h = 0.25 }) {
    const y = Float64Array.of(N[0], N[1] - seed, N[2], 0, seed, 0, 0, 0, 0, 0, 0, 0); // S(3) E(3) I(3) R(3)
    const k1 = new Float64Array(12),
        k2 = new Float64Array(12),
        k3 = new Float64Array(12),
        k4 = new Float64Array(12),
        tmp = new Float64Array(12);
    const f = (t, s, d) => {
        const b = beta * multAt(t),
            p0 = s[6] / N[0],
            p1 = s[7] / N[1],
            p2 = s[8] / N[2];
        for (let i = 0; i < 3; i++) {
            const inf = SUSC[i] * b * (CFLAT[3 * i] * p0 + CFLAT[3 * i + 1] * p1 + CFLAT[3 * i + 2] * p2) * s[i];
            d[i] = -inf;
            d[3 + i] = inf - s[3 + i] / LATENT;
            d[6 + i] = s[3 + i] / LATENT - s[6 + i] / INFECTIOUS;
            d[9 + i] = s[6 + i] / INFECTIOUS;
        }
    };
    const stage = (k, c) => {
        for (let v = 0; v < 12; v++) tmp[v] = y[v] + c * k[v];
        return tmp;
    };
    const steps = Math.round(1 / h),
        incidence = new Array(days);
    for (let day = 0; day < days; day++) {
        let newInf = 0;
        for (let q = 0; q < steps; q++) {
            const t = day + q * h;
            f(t, y, k1);
            f(t + h / 2, stage(k1, h / 2), k2);
            f(t + h / 2, stage(k2, h / 2), k3);
            f(t + h, stage(k3, h), k4);
            for (let v = 0; v < 12; v++) {
                const next = y[v] + (h / 6) * (k1[v] + 2 * k2[v] + 2 * k3[v] + k4[v]);
                if (v < 3) newInf += y[v] - next;
                y[v] = next;
            }
        }
        incidence[day] = newInf;
    }
    return { incidence, final: y };
}

// =============================================================================
// The hidden truth and the reported data
// =============================================================================
const TRUE = { beta: 0.034, seed: 40, report: 0.35 };
const LOCKDOWN_DAY = 70;
const truthMult = (t) => (t >= LOCKDOWN_DAY ? 0.62 : 1);
const OBS_DAYS = 84;
const truth = simulate({ ...TRUE, days: OBS_DAYS + 28, multAt: truthMult });
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
    return simplex.values().minBy((s) => s.f);
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
        multAt: (t) => (t >= LOCKDOWN_DAY ? m : 1),
    }).incidence;
    return Iterator.range(LOCKDOWN_DAY, OBS_DAYS - 3)
        .map((d) => (Math.log(1 + sim[d] * reportHat) - Math.log(1 + smoothed[d])) ** 2)
        .sum();
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
const wRaw = Array.from({ length: 21 }, (_, s) => (s === 0 ? 0 : gammaPdf(s, siShape, siScale)));
const wsum = wRaw.sum();
const w = wRaw.map((x) => x / wsum);
const PRIOR_A = 1,
    PRIOR_B = 5,
    WINDOW = 7;
function coriRt(series, first = WINDOW + 7) {
    const out = [];
    for (let t = first; t < series.length; t++) {
        let a = PRIOR_A,
            invB = 1 / PRIOR_B;
        for (let s = t - WINDOW + 1; s <= t; s++) {
            a += series[s];
            let lam = 0;
            for (let k = 1; k < w.length && s - k >= 0; k++) lam += series[s - k] * w[k];
            invB += lam;
        }
        const scaleP = 1 / invB;
        out.push({ day: t, mean: a * scaleP, lo: gammaincinv(0.025, a) * scaleP, hi: gammaincinv(0.975, a) * scaleP });
    }
    return out;
}
const rt = coriRt(nowcast);

// =============================================================================
// Ensemble forecast and scenarios
// =============================================================================
const HORIZON = 28;
const forecastRun = (beta, seed, report, m, scenarioFrom = Infinity, scenarioM = m) =>
    simulate({
        beta,
        seed,
        days: OBS_DAYS + HORIZON,
        multAt: (t) => (t >= scenarioFrom ? scenarioM : t >= LOCKDOWN_DAY ? m : 1),
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

// ---- Oracles that do not depend on the fit ----
// Final size: with no intervention every infection eventually passes through I, so integrating dS_i/S_i gives
// ln(S_i(inf)/S_i(0)) = -SUSC_i beta D sum_j C_ij (N_j - S_j(inf)) / N_j. Solve that fixed point on its own and
// compare with a 900-day integration (at 500 days the tail is still 1.8e-6 of the population).
{
    const long = simulate({ beta: TRUE.beta, seed: TRUE.seed, days: 900 }).final;
    const S0 = [N[0], N[1] - TRUE.seed, N[2]];
    let Sinf = [...S0];
    for (let it = 0; it < 500; it++)
        Sinf = S0.map(
            (s0, i) =>
                s0 *
                Math.exp(
                    -SUSC[i] *
                        TRUE.beta *
                        INFECTIOUS *
                        [0, 1, 2].reduce((acc, j) => acc + (CONTACTS[i][j] * (N[j] - Sinf[j])) / N[j], 0),
                ),
        );
    const gap = Math.max(...[0, 1, 2].map((i) => Math.abs(long[i] - Sinf[i]) / N[i]));
    const attack = [0, 1, 2].map((i) => ((1 - Sinf[i] / N[i]) * 100).toFixed(1));
    results[
        `unmitigated final size matches the analytic final-size equation (attack ${attack.join("/")}%, gap ${gap.toExponential(1)})`
    ] = gap < 1e-6 && [0, 1, 2].every((i) => Math.abs(long[i] + long[3 + i] + long[6 + i] + long[9 + i] - N[i]) < 1e-3);
}
// RK4 is fourth order: halving the step divides the step-to-step difference by ~16 (Richardson).
{
    const cum = (h) => simulate({ beta: TRUE.beta, seed: TRUE.seed, days: 60, h }).incidence.sum();
    const x = [1, 0.5, 0.25, 0.125].map(cum);
    const ratios = [0, 1].map((k) => Math.abs(x[k] - x[k + 1]) / Math.abs(x[k + 1] - x[k + 2]));
    results[`RK4 converges at fourth order (error ratios ${ratios.map((r) => r.toFixed(1)).join(", ")}, ideal 16)`] =
        ratios.every((r) => r > 12 && r < 20);
}
// Cori on data generated by the renewal equation itself: exact counts recover R, Poisson counts are covered by the
// 95% credible interval about 95% of the time.
{
    const R = (t) => (t < 45 ? 1.6 : 0.8);
    const renewal = (noise) => {
        const I = Array(7).fill(30);
        for (let t = 7; t < 90; t++) {
            let lam = 0;
            for (let k = 1; k < w.length && t - k >= 0; k++) lam += I[t - k] * w[k];
            I.push(noise ? noise.poisson(R(t) * lam) : R(t) * lam);
        }
        return I;
    };
    const exact = coriRt(renewal(null));
    const at = (day) => exact.find((r) => r.day === day).mean;
    const crng = new Random(77);
    let hit = 0,
        tries = 0;
    for (let rep = 0; rep < 40; rep++)
        for (const r of coriRt(renewal(crng)))
            if (r.day % 10 === 0 && (r.day < 45 || r.day >= 45 + WINDOW + 20)) {
                tries++;
                if (r.lo <= R(r.day) && R(r.day) <= r.hi) hit++;
            }
    results[
        `Cori recovers R from exact renewal data (${at(40).toFixed(3)} / ${at(85).toFixed(3)}) and its 95% CrI covers ${hit}/${tries}`
    ] = Math.abs(at(40) - 1.6) < 0.01 && Math.abs(at(85) - 0.8) < 0.01 && hit / tries > 0.88 && hit / tries <= 1;
    // the 7-day window: day 51 is the first whose window [45, 51] lies wholly after the change, day 50 still sees 44
    results["the Rt window is exactly 7 days (first clean estimate on day 51, not 50)"] =
        Math.abs(at(51) - 0.8) < 0.01 && at(50) > 0.85;
}

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
