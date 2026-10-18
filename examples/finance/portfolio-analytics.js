// Portfolio analytics — covariance shrinkage, mean-variance and risk-parity construction, a rebalancing backtest
// with costs, factor regression, PCA risk decomposition, VaR/CVaR, drawdowns, stress tests and attribution.
//
// WHAT IT IS
//   The analysis a multi-asset portfolio team runs every month on twelve ETFs:
//
//   data         ten years of daily prices from a CSV (generated here with a correlated, regime-switching model
//                that includes a crash), loaded into a DataFrame
//   risk model   sample covariance shrunk toward a constant-correlation target (Ledoit–Wolf), annualised; PCA on
//                standardised returns shows how much risk the first factors explain
//   construction minimum-variance and maximum-Sharpe portfolios on the efficient frontier (long-only, weights
//                summing to one, solved by projected gradient descent), and an equal-risk-contribution
//                (risk parity) portfolio by iterating marginal contributions
//   backtest     walk-forward: estimate on a trailing 3-year window, rebalance monthly, pay 5 bp per unit of
//                turnover; compared against 60/40 and equal weight
//   statistics   CAGR, volatility, Sharpe, Sortino, max drawdown and its duration, Calmar, beta to equities,
//                historical and parametric (Cornish–Fisher) 99% VaR and CVaR
//   factors      regression of each strategy on market, rates and commodity factor returns: betas, alpha, R²
//   stress       the portfolio under the simulated crash window and two hypothetical shocks
//   attribution  Brinson allocation vs selection effect of the risk-parity portfolio against 60/40, by asset class
//   HRP          hierarchical risk parity: correlation distances, single-linkage clustering by Kruskal's algorithm
//                over a UnionFind, quasi-diagonal ordering, then recursive bisection by inverse cluster variance
//   kernels      returns are stored column-major as Float64Arrays: a covariance entry is one simd f64Dot of two
//                centred columns, a portfolio's daily return one f64Dot of weights and the day's row, and the
//                rolling 63-day volatility one f64Variance per window — the interpreter never runs the inner loops
//
// MODULES  dyna:dataframe · dyna:ml (LinearRegression, PCA, StandardScaler) · dyna:mathx (erfinv, stats) · dyna:csv
//          · dyna:decimal (Money) · dyna:random · dyna:cli (Table) · dyna:file · dyna:simd (f64 kernels)
//          · dyna:structures (UnionFind) · Array/Number extensions
//
// RUN      dynajs examples/finance/portfolio-analytics.js     simulates 10 years of prices and runs the full analysis

import { DataFrame } from "dyna:dataframe";
import { LinearRegression, PCA, StandardScaler } from "dyna:ml";
import { erfinv, stats } from "dyna:mathx";
import * as csv from "dyna:csv";
import { Money } from "dyna:decimal";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { makeTempDir, writeFile, readFile, removeAll } from "dyna:file";
import * as simd from "dyna:simd";
import { UnionFind } from "dyna:structures";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("portfolio");
const rng = new Random(2008);

// =============================================================================
// Universe and a regime-switching price simulation
// =============================================================================
const ASSETS = [
    ["US_EQ", "equity", 0.08, 0.17],
    ["EU_EQ", "equity", 0.065, 0.19],
    ["EM_EQ", "equity", 0.075, 0.23],
    ["JP_EQ", "equity", 0.05, 0.18],
    ["US_TSY", "bonds", 0.025, 0.06],
    ["EU_GOV", "bonds", 0.018, 0.055],
    ["IG_CORP", "bonds", 0.035, 0.07],
    ["HY_CORP", "bonds", 0.05, 0.1],
    ["GOLD", "real", 0.04, 0.15],
    ["COMMOD", "real", 0.03, 0.2],
    ["REIT", "real", 0.06, 0.21],
    ["TIPS", "bonds", 0.022, 0.05],
];
const N = ASSETS.length,
    DAYS = 2520;
const loadings = ASSETS.map(([, cls]) =>
    cls === "equity" ? [0.9, -0.1, 0.1] : cls === "bonds" ? [0.15, 0.85, -0.05] : [0.4, 0.05, 0.7],
);
const factorCov = [
    [1, -0.25, 0.3],
    [-0.25, 1, -0.1],
    [0.3, -0.1, 1],
];
const chol = (A) => {
    const n = A.length,
        L = A.map(() => new Array(n).fill(0));
    for (let i = 0; i < n; i++)
        for (let j = 0; j <= i; j++) {
            let s = A[i][j];
            for (let k = 0; k < j; k++) s -= L[i][k] * L[j][k];
            L[i][j] = i === j ? Math.sqrt(Math.max(s, 1e-12)) : s / L[j][j];
        }
    return L;
};
const Lf = chol(factorCov);
const CRASH = [1500, 1560];
const prices = ASSETS.map(() => [100]),
    factorReturns = [[], [], []];
for (let t = 1; t < DAYS; t++) {
    const crash = t >= CRASH[0] && t < CRASH[1];
    const z = [rng.normal(), rng.normal(), rng.normal()],
        f = [0, 1, 2].map((i) => Lf[i].reduce((s, l, k) => s + l * z[k], 0));
    if (crash) {
        f[0] -= 0.9;
        f[1] += 0.35;
        f[2] -= 0.4;
    }
    f.forEach((v, i) => factorReturns[i].push(v * 0.01));
    ASSETS.forEach(([, , mu, vol], i) => {
        const sys = loadings[i].reduce((s, l, k) => s + l * f[k], 0) / Math.hypot(...loadings[i]);
        const r = mu / 252 + (vol / Math.sqrt(252)) * (0.85 * sys + 0.53 * rng.normal()) * (crash ? 2.2 : 1);
        prices[i].push(prices[i][t - 1] * Math.exp(r));
    });
}
const day0 = Date.UTC(2016, 0, 4);
const dates = Array.from({ length: DAYS }, (_, t) =>
    new Date(day0 + Math.floor((t * 365.25) / 252) * 86400_000).toISOString().slice(0, 10),
);
writeFile(
    work.join("prices.csv"),
    csv.stringify([
        ["date", ...ASSETS.map((a) => a[0])],
        ...dates.map((d, t) => [d, ...prices.map((p) => p[t].toFixed(4))]),
    ]),
);

// =============================================================================
// Load, returns, risk model
// =============================================================================
const parsed = csv.parse(readFile(work.join("prices.csv")), { hasHeader: true });
const px = new DataFrame(
    Object.fromEntries(
        parsed.headers.slice(1).map((h, j) => [h, Float64Array.from(parsed.rows, (r) => Number(r[j + 1]))]),
    ),
);
const returns = ASSETS.map(([name]) => {
    const c = px.TO_COLUMNS()[name];
    return Float64Array.from({ length: c.length - 1 }, (_, t) => c[t + 1] / c[t] - 1);
});
const T = returns[0].length;
const byDay = new Float64Array(T * N); // row-major copy: one day's returns are contiguous
for (let i = 0; i < N; i++) for (let t = 0; t < T; t++) byDay[t * N + i] = returns[i][t];
const dayRow = (t) => byDay.subarray(t * N, t * N + N);
function covariance(from, to) {
    const n = to - from;
    const centred = returns.map((r) => {
        const c = r.slice(from, to);
        simd.f64Axpy(c, -simd.f64Mean(c), ONES.subarray(0, n));
        return c;
    });
    const S = centred.map((ci) => centred.map((cj) => (simd.f64Dot(ci, cj) / (n - 1)) * 252));
    const sd = S.map((row, i) => Math.sqrt(row[i]));
    let rbar = 0;
    for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) if (i !== j) rbar += S[i][j] / (sd[i] * sd[j]);
    rbar /= N * (N - 1);
    const delta = 0.3; // a fixed shrinkage intensity toward the target
    const shrunk = S.map((row, i) =>
        Float64Array.from(row, (v, j) => delta * (i === j ? S[i][i] : rbar * sd[i] * sd[j]) + (1 - delta) * v),
    );
    return { S: shrunk, mu: returns.map((r) => simd.f64Mean(r.subarray(from, to)) * 252), sd };
}
const ONES = new Float64Array(T).fill(1);
const f64 = (w) => (w instanceof Float64Array ? w : Float64Array.from(w));
const Sw = (S, w) => {
    const wf = f64(w);
    return Float64Array.from(S, (row) => simd.f64Dot(row, wf));
};
const mv = (w, S) => simd.f64Dot(f64(w), Sw(S, w));
function projectSimplex(v) {
    // Euclidean projection onto {w >= 0, sum w = 1}
    const u = [...v].sort((a, b) => b - a);
    let css = 0,
        theta = 0;
    for (let i = 0; i < u.length; i++) {
        css += u[i];
        const t = (css - 1) / (i + 1);
        if (u[i] - t > 0) theta = t;
    }
    return Float64Array.from(v, (x) => Math.max(0, x - theta));
}
function optimise({ S, mu }, lambda, iterations = 3000) {
    // maximise mu.w - lambda/2 w'Sw over the simplex
    let w = new Float64Array(N).fill(1 / N);
    const muF = Float64Array.from(mu);
    for (let it = 0; it < iterations; it++) {
        const g = simd.f64Axpy(
            Sw(S, w).map((x) => -lambda * x),
            1,
            muF,
        ); // mu - lambda * S w
        w = projectSimplex(
            simd.f64Axpy(
                g.map((x) => x * 0.05),
                1,
                w,
            ),
        );
    }
    return [...w];
}
function riskParity(S) {
    let w = S.map((row, i) => 1 / Math.sqrt(row[i]));
    for (let it = 0; it < 500; it++) {
        const wf = Float64Array.from(w),
            sigma = Math.sqrt(mv(wf, S)),
            mrc = Sw(S, wf).map((x) => x / sigma);
        const target = sigma / N;
        w = w.map((wi, i) => wi * Math.pow(target / (wi * mrc[i]), 0.5));
        const s = w.sum();
        w = w.map((x) => x / s);
    }
    return w;
}
// Hierarchical risk parity (López de Prado): cluster on correlation distance, order, bisect.
function hrp({ S, sd }) {
    const dist = (i, j) => Math.sqrt(Math.max(0, (1 - S[i][j] / (Math.sqrt(S[i][i]) * Math.sqrt(S[j][j]))) / 2));
    const edges = [];
    for (let i = 0; i < N; i++) for (let j = i + 1; j < N; j++) edges.push([dist(i, j), i, j]);
    const uf = new UnionFind(N),
        members = new Map([...Array(N).keys()].map((i) => [i, [i]]));
    for (const [, i, j] of edges.sortBy((e) => e[0])) {
        // Kruskal = single linkage
        const a = uf.find(i),
            b = uf.find(j);
        if (a === b) continue;
        uf.union(a, b);
        const root = uf.find(a),
            merged = [...members.get(a), ...members.get(b)];
        members.delete(a);
        members.delete(b);
        members.set(root, merged);
    }
    const order = [...members.values()][0]; // leaves in merge order: quasi-diagonal
    const clusterVar = (idx) => {
        const ivp = idx.map((i) => 1 / S[i][i]),
            s = ivp.sum(),
            w = Float64Array.from(ivp, (x) => x / s);
        const sub = idx.map((i) => Float64Array.from(idx, (j) => S[i][j]));
        return mv(w, sub);
    };
    const w = new Array(N).fill(1);
    const bisect = (idx) => {
        if (idx.length < 2) return;
        const [l, r] = idx.splitAt(idx.length >> 1),
            vl = clusterVar(l),
            vr = clusterVar(r),
            a = 1 - vl / (vl + vr);
        for (const i of l) w[i] *= a;
        for (const i of r) w[i] *= 1 - a;
        bisect(l);
        bisect(r);
    };
    bisect(order);
    void sd;
    return { w, order };
}
const full = covariance(0, T);
const frontier = [0.5, 1, 2, 4, 8, 16, 64].map((lambda) => {
    const w = optimise(full, lambda);
    return { lambda, w, ret: w.reduce((s, x, i) => s + x * full.mu[i], 0), vol: Math.sqrt(mv(w, full.S)) };
});
const maxSharpe = frontier.reduce((a, b) => (b.ret / b.vol > a.ret / a.vol ? b : a));
const minVar = frontier.at(-1);
const rpW = riskParity(full.S);
const hrpFull = hrp(full);

// PCA on standardised returns.
const Xstd = await new StandardScaler().fitTransform(Array.from({ length: T }, (_, t) => [...dayRow(t)]));
const pca = await new PCA(4).fit(Xstd);

// =============================================================================
// Walk-forward backtest with monthly rebalancing and costs
// =============================================================================
const COST = 0.0005,
    WINDOW = 756;
const STRATS = {
    "60/40": () => ASSETS.map(([n]) => (n === "US_EQ" ? 0.6 : n === "US_TSY" ? 0.4 : 0)),
    "equal weight": () => new Array(N).fill(1 / N),
    "min variance": (rm) => optimise(rm, 64, 600),
    "max Sharpe": (rm) => optimise(rm, maxSharpe.lambda, 600),
    "risk parity": (rm) => riskParity(rm.S),
    HRP: (rm) => hrp(rm).w,
};
const backtests = {},
    riskModels = new Map(); // one estimate per rebalance date, shared
const riskModelAt = (t) => {
    if (!riskModels.has(t)) riskModels.set(t, covariance(t - WINDOW, t));
    return riskModels.get(t);
};
for (const [name, rule] of Object.entries(STRATS)) {
    let w = null,
        value = 1,
        turnover = 0;
    const nav = [],
        rets = [],
        weightsLog = [];
    for (let t = WINDOW; t < T; t++) {
        if (w === null || (t - WINDOW) % 21 === 0) {
            const target = rule(riskModelAt(t));
            const tw = w ? target.reduce((s, x, i) => s + Math.abs(x - w[i]), 0) : 1;
            turnover += tw;
            value *= 1 - COST * tw;
            w = target;
            weightsLog.push([t, target]);
        }
        const day = dayRow(t),
            r = simd.f64Dot(Float64Array.from(w), day);
        value *= 1 + r;
        rets.push(r);
        nav.push(value);
        w = w.map((wi, i) => (wi * (1 + day[i])) / (1 + r)); // weights drift between rebalances
    }
    backtests[name] = { nav, rets, turnover, weightsLog };
}
// Rolling 63-day annualised volatility of risk parity: one f64Variance per window.
const rpRets = Float64Array.from(backtests["risk parity"].rets);
const rollingVol = Float64Array.from({ length: rpRets.length - 62 }, (_, k) =>
    Math.sqrt(simd.f64Variance(rpRets.subarray(k, k + 63)) * 252),
);
const crashIdx = CRASH[0] - 1 - WINDOW;
function metrics(rets, nav) {
    const years = rets.length / 252,
        cagr = nav.at(-1) ** (1 / years) - 1,
        vol = stats.stddev(rets) * Math.sqrt(252);
    const down = rets.filter((r) => r < 0),
        sortino =
            (stats.mean(rets) * 252) / (Math.sqrt(down.reduce((s, r) => s + r * r, 0) / rets.length) * Math.sqrt(252));
    let peak = 1,
        mdd = 0,
        ddStart = 0,
        longest = 0,
        cur = 0;
    nav.forEach((v, i) => {
        if (v > peak) {
            peak = v;
            cur = 0;
            ddStart = i;
        } else cur++;
        mdd = Math.min(mdd, v / peak - 1);
        longest = Math.max(longest, cur);
    });
    const sorted = [...rets].sort((a, b) => a - b),
        k = Math.floor(rets.length * 0.01);
    const hVaR = -sorted[k],
        hCVaR = -stats.mean(sorted.slice(0, k + 1));
    const m = stats.mean(rets),
        s = stats.stddev(rets);
    const skew = rets.reduce((a, r) => a + ((r - m) / s) ** 3, 0) / rets.length,
        kurt = rets.reduce((a, r) => a + ((r - m) / s) ** 4, 0) / rets.length - 3;
    const z = Math.SQRT2 * erfinv(2 * 0.01 - 1),
        zcf = z + ((z * z - 1) * skew) / 6 + ((z ** 3 - 3 * z) * kurt) / 24 - ((2 * z ** 3 - 5 * z) * skew * skew) / 36;
    const mkt = [...returns[0].subarray(WINDOW)];
    const beta = stats.cov(rets, mkt) / stats.variance(mkt);
    return {
        cagr,
        vol,
        sharpe: (stats.mean(rets) * 252 - 0.02) / vol,
        sortino,
        mdd,
        longest,
        calmar: cagr / -mdd,
        hVaR,
        hCVaR,
        cfVaR: -(m + zcf * s),
        beta,
        skew,
        kurt,
    };
}
const scorecard = Object.fromEntries(
    Object.entries(backtests).map(([k, b]) => [
        k,
        { ...metrics(b.rets, b.nav), turnover: b.turnover / (b.rets.length / 252) },
    ]),
);

// Factor regression on daily returns.
const factorsX = Array.from({ length: T - WINDOW }, (_, i) => factorReturns.map((f) => f[WINDOW + i]));
const factorFits = {};
for (const [name, b] of Object.entries(backtests)) {
    const lr = await new LinearRegression().fit(factorsX, b.rets);
    const pred = await lr.predict(factorsX),
        mean = stats.mean(b.rets);
    const r2 =
        1 - b.rets.reduce((s, y, i) => s + (y - pred[i]) ** 2, 0) / b.rets.reduce((s, y) => s + (y - mean) ** 2, 0);
    factorFits[name] = { betas: lr.coef, alpha: lr.intercept * 252, r2 };
    lr.close();
}

// Stress tests: the historical crash window and two hypothetical shocks applied to the final weights.
const finalW = (name) => backtests[name].weightsLog.at(-1)[1];
const crashRet = (w) =>
    returns.reduce(
        (s, r, i) => s + w[i] * (r.subarray(CRASH[0] - 1, CRASH[1] - 1).reduce((p, x) => p * (1 + x), 1) - 1),
        0,
    );
const SHOCKS = {
    "equities -25%, bonds +4%": { equity: -0.25, bonds: 0.04, real: -0.08 },
    "rates +200bp": { equity: -0.08, bonds: -0.11, real: -0.03 },
};
const shock = (w, s) => w.reduce((acc, wi, i) => acc + wi * s[ASSETS[i][1]], 0);

// Brinson attribution of risk parity vs 60/40 over the backtest, by asset class.
const classes = ["equity", "bonds", "real"];
const avgW = (name) => {
    const log = backtests[name].weightsLog;
    return ASSETS.map((_, i) => stats.mean(log.map(([, w]) => w[i])));
};
const clsRet = (w, cls) => {
    const idx = ASSETS.map((a, i) => (a[1] === cls ? i : -1)).filter((i) => i >= 0),
        tw = idx.reduce((s, i) => s + w[i], 0);
    return { weight: tw, ret: tw ? idx.reduce((s, i) => s + (w[i] / tw) * full.mu[i], 0) : 0 };
};
const pW = avgW("risk parity"),
    bW = avgW("60/40"),
    bTotal = classes.reduce((s, c) => s + clsRet(bW, c).weight * clsRet(bW, c).ret, 0);
const brinson = classes.map((c) => {
    const p = clsRet(pW, c),
        b = clsRet(bW, c);
    return {
        cls: c,
        allocation: (p.weight - b.weight) * (b.ret - bTotal),
        selection: b.weight ? b.weight * (p.ret - b.ret) : 0,
        interaction: (p.weight - b.weight) * (p.ret - b.ret),
    };
});

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const sumOk = (w) => Math.abs(w.reduce((a, b) => a + b, 0) - 1) < 1e-9 && w.every((x) => x >= -1e-12);
results["every optimised portfolio is long-only and fully invested"] = [maxSharpe.w, minVar.w, rpW].every(sumOk);
results["the frontier's volatility falls as risk aversion rises"] = frontier.every(
    (p, i) => i === 0 || p.vol <= frontier[i - 1].vol + 1e-9,
);
const rc = (w) => {
    const v = mv(w, full.S),
        m = Sw(full.S, w);
    return w.map((wi, i) => (wi * m[i]) / v);
};
results["risk parity equalises risk contributions"] = Math.max(...rc(rpW)) - Math.min(...rc(rpW)) < 0.005;
results[
    `three PCA factors explain ${(pca.explainedVarianceRatio.slice(0, 3).reduce((a, b) => a + b, 0) * 100).toFixed(0)}% of variance`
] = pca.explainedVarianceRatio[0] > 0.3 && pca.explainedVarianceRatio.slice(0, 3).reduce((a, b) => a + b, 0) > 0.55;
results["min-variance realises the lowest volatility out of sample"] =
    scorecard["min variance"].vol < scorecard["equal weight"].vol &&
    scorecard["min variance"].vol < scorecard["60/40"].vol;
results[
    `risk parity's drawdown (${(scorecard["risk parity"].mdd * 100).toFixed(1)}%) is shallower than 60/40's (${(scorecard["60/40"].mdd * 100).toFixed(1)}%)`
] = scorecard["risk parity"].mdd > scorecard["60/40"].mdd;
results["CVaR is never smaller than VaR"] = Object.values(scorecard).every((r) => r.hCVaR >= r.hVaR);
results["60/40 is mostly market beta; risk parity carries less"] =
    factorFits["60/40"].r2 > 0.5 && factorFits["60/40"].betas[0] > factorFits["risk parity"].betas[0];
results["the historical crash hurts 60/40 more than risk parity"] =
    crashRet(finalW("60/40")) < crashRet(finalW("risk parity"));
results["HRP is long-only, fully invested and orders all twelve assets"] =
    sumOk(hrpFull.w) && hrpFull.order.length === N && new Set(hrpFull.order).size === N;
results["HRP's clustering keeps each asset class contiguous in its order"] = ["equity", "bonds"].every((c) => {
    const pos = hrpFull.order.map((i, k) => (ASSETS[i][1] === c ? k : -1)).filter((k) => k >= 0);
    return pos.last() - pos[0] + 1 === pos.length;
});
results["HRP realises lower volatility than equal weight out of sample"] =
    scorecard["HRP"].vol < scorecard["equal weight"].vol;
results["the simd covariance diagonal matches a scalar two-pass variance"] = [0, 4, 9].every((i) => {
    const a = returns[i];
    let m = 0;
    for (let t = 0; t < T; t++) m += a[t];
    m /= T;
    let ss = 0;
    for (let t = 0; t < T; t++) ss += (a[t] - m) * (a[t] - m);
    return Math.abs(full.S[i][i] / ((ss / (T - 1)) * 252) - 1) < 1e-12;
});
results[`rolling volatility peaks inside the crash window (${(simd.f64Max(rollingVol) * 100).toFixed(0)}%)`] = (() => {
    const at = [...rollingVol].indexOf(simd.f64Max(rollingVol));
    return at >= crashIdx - 62 && at <= crashIdx + 62;
})();
const capital = Money.fromString("25000000.00", "EUR");

print("portfolio-analytics");
print(
    `  data       ${N} assets x ${DAYS} days; shrunk covariance; PCA ${pca.explainedVarianceRatio.map((v) => `${(v * 100).toFixed(0)}%`).join(" ")}`,
);
print(
    table([
        ["asset", "class", "max Sharpe", "min var", "risk parity", "RP risk share", "HRP"],
        ...ASSETS.map(([n, c], i) => [
            n,
            c,
            `${(maxSharpe.w[i] * 100).toFixed(1)}%`,
            `${(minVar.w[i] * 100).toFixed(1)}%`,
            `${(rpW[i] * 100).toFixed(1)}%`,
            `${(rc(rpW)[i] * 100).toFixed(1)}%`,
            `${(hrpFull.w[i] * 100).toFixed(1)}%`,
        ]),
    ]),
);
print(`  HRP order  ${hrpFull.order.map((i) => ASSETS[i][0]).join(" ")}`);
print(
    table([
        [
            "strategy",
            "CAGR",
            "vol",
            "Sharpe",
            "Sortino",
            "max DD",
            "Calmar",
            "VaR99",
            "CVaR99",
            "CF VaR",
            "turnover/y",
            "β mkt",
            "α",
            "R²",
        ],
        ...Object.entries(scorecard).map(([k, r]) => [
            k,
            `${(r.cagr * 100).toFixed(2)}%`,
            `${(r.vol * 100).toFixed(1)}%`,
            r.sharpe.toFixed(2),
            r.sortino.toFixed(2),
            `${(r.mdd * 100).toFixed(1)}%`,
            r.calmar.toFixed(2),
            `${(r.hVaR * 100).toFixed(2)}%`,
            `${(r.hCVaR * 100).toFixed(2)}%`,
            `${(r.cfVaR * 100).toFixed(2)}%`,
            `${(r.turnover * 100).toFixed(0)}%`,
            factorFits[k].betas[0].toFixed(2),
            `${(factorFits[k].alpha * 100).toFixed(2)}%`,
            factorFits[k].r2.toFixed(2),
        ]),
    ]),
);
print(
    table([
        ["scenario", ...Object.keys(STRATS)],
        ["historical crash", ...Object.keys(STRATS).map((k) => `${(crashRet(finalW(k)) * 100).toFixed(1)}%`)],
        ...Object.entries(SHOCKS).map(([n, s]) => [
            n,
            ...Object.keys(STRATS).map((k) => `${(shock(finalW(k), s) * 100).toFixed(1)}%`),
        ]),
    ]),
);
const lossOn = (w) => Money.fromString((-crashRet(w) * 25_000_000).toFixed(2), "EUR");
print(
    `  on ${capital.format()}: historical crash loss for risk parity ${lossOn(finalW("risk parity")).format()}, for 60/40 ${lossOn(finalW("60/40")).format()}`,
);
print(
    table([
        ["class", "allocation", "selection", "interaction"],
        ...brinson.map((b) => [
            b.cls,
            `${(b.allocation * 1e4).toFixed(0)} bp`,
            `${(b.selection * 1e4).toFixed(0)} bp`,
            `${(b.interaction * 1e4).toFixed(0)} bp`,
        ]),
    ]),
);
pca.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
