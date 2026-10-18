// Capacity planning — traffic forecasting with Holt–Winters, queueing models (Erlang C, M/M/c waiting-time tails),
// a discrete-event simulator that validates them, autoscaling policies compared by cost and SLO, Little's law
// checks, failure headroom, and a staffing/server plan per hour of the week.
//
// WHAT IT IS
//   traffic      four weeks of hourly request rates for an API (daily and weekly seasonality, growth, a holiday dip),
//                forecast one week ahead with additive Holt–Winters whose smoothing parameters are chosen by
//                minimising one-step-ahead error; forecast intervals from the residual spread
//   queueing     each server is a worker with exponential service time; for a given arrival rate the M/M/c model
//                gives utilisation, the Erlang C probability of waiting, mean wait, and the wait-time tail
//                P(W > t) — so "p99 latency under 250 ms" becomes a minimum server count
//   simulation   a discrete-event simulator (event heap: arrivals, service completions) with log-normal service
//                times checks where the analytical model is optimistic, and verifies Little's law L = λW
//   policies     for the forecast week: static peak provisioning, the per-hour analytical plan, and a reactive
//                autoscaler with a 10-minute lag; cost in Decimal at an hourly rate, SLO-violation minutes simulated
//   headroom     N+1: the plan must still meet the SLO with one zone (a third of servers) lost at peak
//   engine       the simulator's future-event list is a native IndexedHeap keyed by event time (no JS comparator
//                called O(log n) times per event) and the waiting line is a Deque (O(1) at both ends — an array's
//                shift() is O(n) and turns an overloaded queue quadratic)
//
// MODULES  dyna:structures (IndexedHeap, Deque) · dyna:mathx (stats, lgamma) · dyna:dataframe · dyna:decimal (Money)
//          · dyna:random · dyna:csv · dyna:cli (Table) · dyna:file
//
// RUN      dynajs examples/science/capacity-planning.js     forecasts next week and sizes the fleet for it

import { IndexedHeap, Deque } from "dyna:structures";
import { stats, gammaln } from "dyna:mathx";
import { DataFrame } from "dyna:dataframe";
import { Money } from "dyna:decimal";
import { Random } from "dyna:random";
import * as csv from "dyna:csv";
import { Table } from "dyna:cli";
import { makeTempDir, writeFile, removeAll } from "dyna:file";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("capacity");
const rng = new Random(1917);
const SERVICE_MS = 40,
    SLO_MS = 250,
    SLO_Q = 0.99,
    PRICE = "0.192"; // per server-hour

// =============================================================================
// Traffic history and Holt–Winters forecast
// =============================================================================
const HOURS = 24 * 7 * 4,
    SEASON = 24 * 7;
const daily = (h) =>
    0.35 + 0.65 * Math.exp(-(((h % 24) - 14) ** 2) / 18) + 0.25 * Math.exp(-(((h % 24) - 20) ** 2) / 6);
const weekly = (h) => (Math.floor(h / 24) % 7 >= 5 ? 0.7 : 1);
const truthRate = (h) => 900 * daily(h) * weekly(h) * (1 + 0.004 * (h / 24)) * (Math.floor(h / 24) === 17 ? 0.6 : 1); // req/s; day 17 is a holiday
const history = Array.from({ length: HOURS }, (_, h) => truthRate(h) * Math.exp(rng.normal(0, 0.05)));
function holtWinters(y, alpha, beta, gamma, horizon) {
    let level = stats.mean(y.slice(0, SEASON)),
        trend = (stats.mean(y.slice(SEASON, 2 * SEASON)) - level) / SEASON;
    const season = y.slice(0, SEASON).map((v) => v - level),
        fitted = [];
    for (let t = 0; t < y.length; t++) {
        const s = season[t % SEASON],
            f = level + trend + s;
        fitted.push(f);
        const prevLevel = level;
        level = alpha * (y[t] - s) + (1 - alpha) * (level + trend);
        trend = beta * (level - prevLevel) + (1 - beta) * trend;
        season[t % SEASON] = gamma * (y[t] - level) + (1 - gamma) * s;
    }
    const forecast = Array.from(
        { length: horizon },
        (_, k) => level + (k + 1) * trend + season[(y.length + k) % SEASON],
    );
    return { fitted, forecast };
}
let bestHW = null;
for (const alpha of [0.05, 0.1, 0.2, 0.4])
    for (const beta of [0, 0.01, 0.05])
        for (const gamma of [0.05, 0.1, 0.3]) {
            const { fitted } = holtWinters(history, alpha, beta, gamma, 0);
            const sse = history.slice(SEASON).reduce((s, v, i) => s + (v - fitted[i + SEASON]) ** 2, 0);
            if (!bestHW || sse < bestHW.sse) bestHW = { alpha, beta, gamma, sse };
        }
const hw = holtWinters(history, bestHW.alpha, bestHW.beta, bestHW.gamma, SEASON);
const resid = history.slice(SEASON).map((v, i) => v - hw.fitted[i + SEASON]),
    sigma = stats.stddev(resid);
const forecast = hw.forecast.map((f) => ({ mean: f, p90: f + 1.2816 * sigma }));
const actualNext = Array.from({ length: SEASON }, (_, k) => truthRate(HOURS + k) * Math.exp(rng.normal(0, 0.05)));

// =============================================================================
// M/M/c (Erlang C)
// =============================================================================
function erlangC(lambda, mu, c) {
    // computed in log space for large c
    const a = lambda / mu,
        rho = a / c;
    if (rho >= 1) return { rho, pWait: 1, meanWait: Infinity, tail: () => 1 };
    let sum = 0;
    const logTerm = (k) => k * Math.log(a) - gammaln(k + 1);
    const top = logTerm(c) + Math.log(1 / (1 - rho));
    let maxLog = top;
    for (let k = 0; k < c; k++) maxLog = Math.max(maxLog, logTerm(k));
    for (let k = 0; k < c; k++) sum += Math.exp(logTerm(k) - maxLog);
    const pWait = Math.exp(top - maxLog) / (sum + Math.exp(top - maxLog));
    return { rho, pWait, meanWait: pWait / (c * mu - lambda), tail: (t) => pWait * Math.exp(-(c * mu - lambda) * t) };
}
function serversFor(lambda) {
    // smallest c with P(response > SLO) <= 1 - q
    const mu = 1000 / SERVICE_MS;
    for (let c = Math.ceil(lambda / mu) + 1; c < 10000; c++) {
        const m = erlangC(lambda, mu, c);
        // response = wait + service, so P(R > T) <= P(W > T - s) + P(S > s); s is the service time's 99.5th percentile
        const half = (1 - SLO_Q) / 2,
            s = Math.log(1 / half) / mu;
        if (SLO_MS / 1000 > s && m.tail(SLO_MS / 1000 - s) + half <= 1 - SLO_Q) return { c, ...m };
    }
    return null;
}

// =============================================================================
// Discrete-event simulation
// =============================================================================
function simulate({ ratePerSec, servers, seconds, serviceDist = "exp", schedule = null }) {
    const events = new IndexedHeap(),
        kind = [],
        arrivedAt = [],
        queue = new Deque(),
        lat = [];
    let nextId = 0,
        busy = 0,
        t = 0,
        inSystemArea = 0,
        lastT = 0,
        inSystem = 0,
        arrivals = 0,
        cap = servers;
    const schedule_ = (time, k, arrT = 0) => {
        const id = nextId++;
        kind[id] = k;
        arrivedAt[id] = arrT;
        events.push(id, time);
    };
    const service = () =>
        serviceDist === "exp"
            ? rng.exponential(1000 / SERVICE_MS)
            : Math.exp(rng.normal(Math.log(SERVICE_MS / 1000) - 0.18, 0.6));
    const rateAt = (time) => (typeof ratePerSec === "function" ? ratePerSec(time) : ratePerSec);
    schedule_(rng.exponential(rateAt(0)), 0);
    const start = (arrT) => {
        busy++;
        schedule_(t + service(), 1, arrT);
    };
    for (;;) {
        const e = events.popEntry();
        if (!e || e[1] > seconds) break;
        const [id, time] = e;
        inSystemArea += inSystem * (time - lastT);
        lastT = time;
        t = time;
        if (schedule) cap = schedule(t);
        if (kind[id] === 0) {
            arrivals++;
            inSystem++;
            if (busy < cap) start(t);
            else queue.pushBack(t);
            schedule_(t + rng.exponential(rateAt(t)), 0);
        } else {
            busy--;
            inSystem--;
            lat.push(t - arrivedAt[id]);
            if (queue.length && busy < cap) start(queue.popFront());
        }
    }
    const W = stats.mean(lat),
        L = inSystemArea / lastT,
        lambda = arrivals / lastT;
    return { p99: stats.quantile(lat, 0.99) * 1000, mean: W * 1000, L, lambdaW: lambda * W, served: lat.length, lat };
}

// =============================================================================
// Plans for next week
// =============================================================================
const hourly = forecast.map((f) => serversFor(f.p90));
const peakServers = Math.max(...hourly.map((h) => h.c));
const cost = (serverHours) => new Money(Math.round(serverHours * Number(PRICE) * 100), "USD");
const evaluateWeek = (serversAt, label) => {
    let serverHours = 0,
        violations = 0;
    const sampleHours = Array.from({ length: SEASON }, (_, h) => h).filter((h) => h % 12 === 0 || h % 24 === 14);
    for (let h = 0; h < SEASON; h++) serverHours += serversAt(h, actualNext[h]);
    for (const h of sampleHours) {
        const lagged = serversFor(actualNext[Math.max(0, h - 1)]).c,
            now = serversAt(h, actualNext[h]); // resolved once, not per event
        const sim = simulate({
            ratePerSec: actualNext[h],
            servers: now,
            seconds: 10,
            serviceDist: "lognormal",
            schedule: label === "reactive autoscaler" ? (t) => (t < 10 ? lagged : now) : null,
        });
        if (sim.p99 > SLO_MS) violations++;
    }
    return { serverHours, cost: cost(serverHours), violations, sampled: sampleHours.length };
};
const policies = {
    "static peak": evaluateWeek(() => peakServers, "static peak"),
    "hourly plan": evaluateWeek((h) => hourly[h].c, "hourly plan"),
    "reactive autoscaler": evaluateWeek((h, actual) => Math.ceil(serversFor(actual).c * 0.95), "reactive autoscaler"),
};

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const mape = stats.mean(actualNext.map((a, k) => Math.abs(a - forecast[k].mean) / a));
results[
    `Holt–Winters next-week MAPE ${(mape * 100).toFixed(1)}% (α=${bestHW.alpha}, β=${bestHW.beta}, γ=${bestHW.gamma})`
] = mape < 0.1;
const inBand = actualNext.filter((a, k) => a <= forecast[k].p90).length / SEASON;
results[`${(inBand * 100).toFixed(0)}% of actual hours fall under the p90 forecast`] = inBand > 0.8;
const lam = 600,
    mu = 1000 / SERVICE_MS,
    c = serversFor(lam).c,
    model = erlangC(lam, mu, c);
const sim = simulate({ ratePerSec: lam, servers: c, seconds: 60 });
results[
    `M/M/${c} at λ=${lam}/s: simulated mean ${sim.mean.toFixed(1)} ms vs Erlang C ${((model.meanWait + 1 / mu) * 1000).toFixed(1)} ms; p99 ${sim.p99.toFixed(0)} ms`
] = Math.abs(sim.mean - (model.meanWait + 1 / mu) * 1000) < 3 && sim.p99 <= SLO_MS;
results[`Little's law holds: L=${sim.L.toFixed(2)}, λW=${sim.lambdaW.toFixed(2)}`] =
    Math.abs(sim.L / sim.lambdaW - 1) < 0.03;
const tight = simulate({ ratePerSec: lam, servers: c, seconds: 60, serviceDist: "lognormal" }),
    tighter = simulate({ ratePerSec: lam, servers: Math.ceil(lam / mu), seconds: 60 });
results[
    `at the stability limit (${Math.ceil(lam / mu)} servers, ρ = ${(lam / mu / Math.ceil(lam / mu)).toFixed(2)}) the queue grows and the SLO breaks: p99 ${tighter.p99.toFixed(0)} ms`
] = tighter.p99 > SLO_MS;
const peakHour = forecast.reduce((b, f, k) => (f.p90 > forecast[b].p90 ? k : b), 0);
const planPeak = hourly[peakHour].c,
    nPlus1 = Math.ceil(planPeak * 1.5);
const zoneLoss = simulate({ ratePerSec: forecast[peakHour].p90, servers: Math.floor((planPeak * 2) / 3), seconds: 20 });
const zoneLossN1 = simulate({ ratePerSec: forecast[peakHour].p90, servers: Math.floor((nPlus1 * 2) / 3), seconds: 20 });
results[
    `losing a zone at peak: the plain plan (${planPeak} -> ${Math.floor((planPeak * 2) / 3)}) gives p99 ${zoneLoss.p99.toFixed(0)} ms; the N+1 plan (${nPlus1} -> ${Math.floor((nPlus1 * 2) / 3)}) gives ${zoneLossN1.p99.toFixed(0)} ms`
] = zoneLoss.p99 > SLO_MS && zoneLossN1.p99 <= SLO_MS;
results[
    "the forecast-driven plan is cheaper than static peak and violates the SLO no more than the reactive autoscaler"
] =
    policies["hourly plan"].cost.cmp(policies["static peak"].cost) < 0 &&
    policies["hourly plan"].violations <= policies["reactive autoscaler"].violations;
writeFile(
    work.join("plan.csv"),
    csv.stringify([
        ["hour", "forecast_rps", "p90_rps", "servers", "utilisation"],
        ...forecast.map((f, h) => [
            String(h),
            f.mean.toFixed(0),
            f.p90.toFixed(0),
            String(hourly[h].c),
            hourly[h].rho.toFixed(2),
        ]),
    ]),
);

print("capacity-planning");
print(
    `  forecast   next week peak ${Math.max(...forecast.map((f) => f.p90)).toFixed(0)} req/s (p90), trough ${Math.min(...forecast.map((f) => f.mean)).toFixed(0)} req/s; MAPE ${(mape * 100).toFixed(1)}%`,
);
print(
    table([
        ["λ req/s", "servers", "utilisation", "P(wait)", "mean wait ms", "p99 simulated"],
        ...[200, 400, 600, 800, 1000].map((l) => {
            const s = serversFor(l),
                m = erlangC(l, mu, s.c),
                r = simulate({ ratePerSec: l, servers: s.c, seconds: 20 });
            return [
                String(l),
                String(s.c),
                m.rho.toFixed(2),
                m.pWait.toFixed(3),
                (m.meanWait * 1000).toFixed(2),
                `${r.p99.toFixed(0)} ms`,
            ];
        }),
    ]),
);
print(
    table([
        ["policy", "server-hours", "cost", "SLO-violating sampled hours"],
        ...Object.entries(policies).map(([k, r]) => [
            k,
            r.serverHours.toFixed(0),
            r.cost.format(),
            `${r.violations}/${r.sampled}`,
        ]),
    ]),
);
print(
    `  validation exponential vs log-normal service at M/M/${c}: p99 ${sim.p99.toFixed(0)} vs ${tight.p99.toFixed(0)} ms (the service-time shape moves the tail; the model assumes exponential)`,
);
const hourDf = new DataFrame({ servers: Float64Array.from(hourly.map((h) => h.c)) });
print(
    `  plan.csv   ${SEASON} hours, servers min ${hourDf.MIN("servers")} / mean ${hourDf.MEAN("servers").toFixed(1)} / max ${hourDf.MAX("servers")}`,
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
