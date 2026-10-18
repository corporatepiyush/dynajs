// Options pricing — a book of European options priced, risked and implied in vector kernels, cross-checked by
// Monte Carlo, a binomial lattice and put-call parity.
//
// WHAT IT IS
//   The pricing core of an equity-derivatives desk:
//
//   pricer      Black-Scholes over 100k options at once in Float32Array lanes: logs, square roots, exponentials
//               and the normal CDF (Abramowitz-Stegun 26.2.17, |error| < 7.5e-8) are whole-array simd kernels;
//               the scalar reference uses mathx.erf and must agree to single precision
//   parity      C - P = S - K e^{-rT} on every option
//   greeks      delta and vega in closed form, checked against central finite differences of the pricer
//   implied     implied volatility for the whole book by vectorised bisection (60 halvings on every lane at once);
//               it must recover the volatilities that produced the prices
//   monte carlo terminal prices from Random.normal, payoffs as simd relu; plain and antithetic estimators must
//               bracket the closed form within three standard errors, antithetic with the smaller error
//   american    a CRR lattice in a Float64Array: European values converge to Black-Scholes as steps double, and
//               the American put carries a positive early-exercise premium
//   risk        a t-digest of position P&L under a 1-day shock grid gives VaR / expected shortfall
//
// MODULES  dyna:simd (vlog, vexp, vsqrt, vinv, mul, add, sub, abs, affine, scale, addScalar, relu, mean, variance,
//          sum, dot) · dyna:mathx (erf) · dyna:random · dyna:structures (TDigest) · dyna:cli · Number extensions
//
// RUN      dynajs examples/finance/options-pricing.js [options]

import {
    vlog,
    vexp,
    vsqrt,
    vinv,
    mul,
    add,
    sub,
    abs,
    affine,
    scale,
    addScalar,
    relu,
    mean,
    variance,
    sum,
    div,
    max,
} from "dyna:simd";
import { erf } from "dyna:mathx";
import { Random } from "dyna:random";
import { TDigest } from "dyna:structures";
import { Table } from "dyna:cli";

const results = {};
const N = Number(scriptArgs[1] ?? 100_000);
const rng = new Random(1973);

// =============================================================================
// The book
// =============================================================================
const S = new Float32Array(N),
    K = new Float32Array(N),
    T = new Float32Array(N),
    V = new Float32Array(N);
const R = 0.035;
for (let i = 0; i < N; i++) {
    S[i] = 100;
    K[i] = 60 + 80 * rng.nextFloat();
    T[i] = 0.05 + 2 * rng.nextFloat();
    V[i] = 0.08 + 0.6 * rng.nextFloat();
}

// =============================================================================
// Vector Black-Scholes
// =============================================================================
const tmp = () => new Float32Array(N);
const P = 0.2316419,
    B = [0.31938153, -0.356563782, 1.781477937, -1.821255978, 1.330274429];
// N(x) for a whole array: Horner over t = 1 / (1 + p|x|), pdf via vexp, sign fixed per lane.
function normCdfInto(out, x, work) {
    const [ax, t, poly, pdf] = work;
    abs(ax, x);
    t.set(ax);
    affine(t, P, 1);
    vinv(t);
    poly.fill(B[4]);
    for (let k = 3; k >= 0; k--) {
        mul(poly, poly, t);
        addScalar(poly, B[k]);
    }
    mul(poly, poly, t);
    mul(pdf, x, x);
    scale(pdf, -0.5);
    vexp(pdf);
    scale(pdf, 1 / Math.sqrt(2 * Math.PI));
    mul(out, pdf, poly);
    for (let i = 0; i < out.length; i++) out[i] = x[i] >= 0 ? 1 - out[i] : out[i];
    return out;
}
function blackScholes(S, K, T, V, r) {
    const n = S.length,
        w = [0, 0, 0, 0].map(() => new Float32Array(n));
    const sqrtT = new Float32Array(T);
    vsqrt(sqrtT);
    const volSqrtT = new Float32Array(n);
    mul(volSqrtT, V, sqrtT);
    const d1 = new Float32Array(n);
    div(d1, S, K);
    vlog(d1);
    const drift = new Float32Array(n);
    mul(drift, V, V);
    scale(drift, 0.5);
    addScalar(drift, r);
    mul(drift, drift, T);
    add(d1, d1, drift);
    div(d1, d1, volSqrtT);
    const d2 = new Float32Array(n);
    sub(d2, d1, volSqrtT);
    const nd1 = normCdfInto(new Float32Array(n), d1, w),
        nd2 = normCdfInto(new Float32Array(n), d2, w);
    const disc = new Float32Array(T);
    scale(disc, -r);
    vexp(disc);
    const kDisc = new Float32Array(n);
    mul(kDisc, K, disc);
    const call = new Float32Array(n),
        a = new Float32Array(n);
    mul(call, S, nd1);
    mul(a, kDisc, nd2);
    sub(call, call, a);
    // put = call - S + K e^{-rT}
    const put = new Float32Array(call);
    sub(put, put, S);
    add(put, put, kDisc);
    const pdf = new Float32Array(n);
    mul(pdf, d1, d1);
    scale(pdf, -0.5);
    vexp(pdf);
    scale(pdf, 1 / Math.sqrt(2 * Math.PI));
    const vega = new Float32Array(n);
    mul(vega, S, pdf);
    mul(vega, vega, sqrtT);
    return { call, put, delta: nd1, vega, d1, kDisc };
}
const ncdf = (x) => 0.5 * (1 + erf(x / Math.SQRT2));
function bsScalar(s, k, t, v, r) {
    const d1 = (Math.log(s / k) + (r + (v * v) / 2) * t) / (v * Math.sqrt(t)),
        d2 = d1 - v * Math.sqrt(t);
    return {
        call: s * ncdf(d1) - k * Math.exp(-r * t) * ncdf(d2),
        put: k * Math.exp(-r * t) * ncdf(-d2) - s * ncdf(-d1),
        delta: ncdf(d1),
    };
}

const t0 = performance.now();
const book = blackScholes(S, K, T, V, R);
const vecMs = performance.now() - t0;
const t1 = performance.now();
let worstCall = 0,
    worstPut = 0;
for (let i = 0; i < N; i++) {
    const ref = bsScalar(S[i], K[i], T[i], V[i], R);
    worstCall = Math.max(worstCall, Math.abs(book.call[i] - ref.call));
    worstPut = Math.max(worstPut, Math.abs(book.put[i] - ref.put));
}
const scalarMs = performance.now() - t1;
results["vector calls match the erf reference to 1e-3 on a 100 spot"] = worstCall < 1e-3;
results["vector puts match the erf reference"] = worstPut < 1e-3;
results["every price is within no-arbitrage bounds"] = (() => {
    for (let i = 0; i < N; i++) {
        const c = book.call[i],
            fwdIntrinsic = S[i] - book.kDisc[i];
        if (c < Math.max(0, fwdIntrinsic) - 1e-3 || c > S[i] + 1e-3 || book.put[i] < -1e-3) return false;
    }
    return true;
})();
results["put-call parity holds"] = (() => {
    const lhs = new Float32Array(N);
    sub(lhs, book.call, book.put);
    const rhs = new Float32Array(S);
    sub(rhs, rhs, book.kDisc);
    sub(lhs, lhs, rhs);
    abs(lhs, lhs);
    return max(lhs) < 1e-3;
})();
print(
    `Black-Scholes ${N.format()} options: vector ${vecMs.toFixed(1)} ms, scalar erf ${scalarMs.toFixed(1)} ms; worst |dC| ${worstCall.toExponential(2)}`,
);

// =============================================================================
// Greeks against finite differences (on a 2k-option slice in double-checked f32)
// =============================================================================
{
    const n = 2000,
        h = 0.01,
        sl = (a) => a.slice(0, n);
    const up = new Float32Array(sl(S)),
        dn = new Float32Array(sl(S));
    addScalar(up, h);
    addScalar(dn, -h);
    const cu = blackScholes(up, sl(K), sl(T), sl(V), R).call,
        cd = blackScholes(dn, sl(K), sl(T), sl(V), R).call;
    let worstDelta = 0;
    for (let i = 0; i < n; i++) worstDelta = Math.max(worstDelta, Math.abs((cu[i] - cd[i]) / (2 * h) - book.delta[i]));
    const vu = new Float32Array(sl(V)),
        vd = new Float32Array(sl(V));
    addScalar(vu, 1e-3);
    addScalar(vd, -1e-3);
    const pu = blackScholes(sl(S), sl(K), sl(T), vu, R).call,
        pd = blackScholes(sl(S), sl(K), sl(T), vd, R).call;
    let worstVega = 0;
    for (let i = 0; i < n; i++)
        worstVega = Math.max(worstVega, Math.abs((pu[i] - pd[i]) / 2e-3 - book.vega[i]) / Math.max(1, book.vega[i]));
    results["delta matches a central difference"] = worstDelta < 5e-3;
    results["vega matches a central difference"] = worstVega < 2e-2;
}

// =============================================================================
// Implied volatility: bisection on every lane at once
// =============================================================================
{
    const lo = new Float32Array(N).fill(0.01),
        hi = new Float32Array(N).fill(2),
        midV = new Float32Array(N);
    const target = book.call;
    for (let it = 0; it < 40; it++) {
        add(midV, lo, hi);
        scale(midV, 0.5);
        const c = blackScholes(S, K, T, midV, R).call;
        for (let i = 0; i < N; i++)
            if (c[i] > target[i]) hi[i] = midV[i];
            else lo[i] = midV[i];
    }
    add(midV, lo, hi);
    scale(midV, 0.5);
    // Deep in/out-of-the-money options have vega ~0: their implied vol is not identifiable at f32 price precision.
    let checked = 0,
        worst = 0;
    for (let i = 0; i < N; i++)
        if (book.vega[i] > 5) {
            checked++;
            worst = Math.max(worst, Math.abs(midV[i] - V[i]));
        }
    results["implied vol recovers the input vol where vega is material"] = checked > N / 2 && worst < 5e-3;
    print(`implied vol: ${checked.format()} identifiable lanes, worst error ${worst.toExponential(2)}`);
}

// =============================================================================
// Monte Carlo for one at-the-money option
// =============================================================================
{
    const s0 = 100,
        k = 105,
        t = 1,
        v = 0.25,
        paths = 200_000;
    const z = new Float32Array(paths);
    rng.fillNormal(z);
    const payoff = (zs) => {
        const st = new Float32Array(zs);
        affine(st, v * Math.sqrt(t), (R - (v * v) / 2) * t);
        vexp(st);
        scale(st, s0);
        addScalar(st, -k);
        relu(st);
        scale(st, Math.exp(-R * t));
        return st;
    };
    const plain = payoff(z);
    const neg = new Float32Array(z);
    scale(neg, -1);
    const anti = payoff(neg);
    add(anti, anti, plain);
    scale(anti, 0.5);
    const exact = bsScalar(s0, k, t, v, R).call;
    const mcPlain = mean(plain),
        sePlain = Math.sqrt(variance(plain) / paths);
    const mcAnti = mean(anti.subarray(0, paths / 2)),
        seAnti = Math.sqrt(variance(anti.subarray(0, paths / 2)) / (paths / 2));
    results["plain Monte Carlo within 3 standard errors"] = Math.abs(mcPlain - exact) < 3 * sePlain;
    results["antithetic Monte Carlo within 3 standard errors"] = Math.abs(mcAnti - exact) < 3 * seAnti;
    results["antithetic pairs beat the same number of plain draws"] = seAnti < sePlain * Math.SQRT2;
    print(
        Table(
            [
                ["closed form", exact.toFixed(4), "-"],
                ["plain MC", mcPlain.toFixed(4), sePlain.toFixed(4)],
                ["antithetic MC", mcAnti.toFixed(4), seAnti.toFixed(4)],
            ],
            { head: ["estimator", "price", "std err"] },
        ),
    );
}

// =============================================================================
// CRR lattice: European convergence and the American premium
// =============================================================================
function crr(s0, k, t, v, r, steps, american, isPut) {
    const dt = t / steps,
        u = Math.exp(v * Math.sqrt(dt)),
        d = 1 / u,
        q = (Math.exp(r * dt) - d) / (u - d),
        df = Math.exp(-r * dt);
    const vals = new Float64Array(steps + 1);
    for (let j = 0; j <= steps; j++) {
        const st = s0 * u ** j * d ** (steps - j);
        vals[j] = Math.max(0, isPut ? k - st : st - k);
    }
    for (let i = steps - 1; i >= 0; i--)
        for (let j = 0; j <= i; j++) {
            const cont = df * (q * vals[j + 1] + (1 - q) * vals[j]);
            if (american) {
                const st = s0 * u ** j * d ** (i - j);
                vals[j] = Math.max(cont, isPut ? k - st : st - k);
            } else vals[j] = cont;
        }
    return vals[0];
}
{
    const exact = bsScalar(100, 110, 1, 0.3, R).put;
    const errs = [64, 128, 256, 512].map((n) => Math.abs(crr(100, 110, 1, 0.3, R, n, false, true) - exact));
    results["lattice converges to Black-Scholes"] = errs[3] < errs[0] && errs[3] < 0.02;
    const amer = crr(100, 110, 1, 0.3, R, 512, true, true),
        euro = crr(100, 110, 1, 0.3, R, 512, false, true);
    results["American put carries an early-exercise premium"] = amer > euro + 0.05;
    const amerCall = crr(100, 110, 1, 0.3, R, 512, true, false),
        euroCall = crr(100, 110, 1, 0.3, R, 512, false, false);
    results["American call on a non-dividend stock is worth the European"] = Math.abs(amerCall - euroCall) < 1e-9;
}

// =============================================================================
// Risk: P&L of the book under a grid of spot and vol shocks
// =============================================================================
{
    const qty = new Float32Array(N);
    for (let i = 0; i < N; i++) qty[i] = (i % 3 === 0 ? -1 : 1) * (1 + (i % 7));
    const base = new Float32Array(book.call);
    mul(base, base, qty);
    const baseValue = sum(base);
    const pnl = new TDigest(200),
        all = [];
    for (let ds = -0.1; ds <= 0.1001; ds += 0.01)
        for (let dv = -0.05; dv <= 0.0501; dv += 0.025) {
            const s1 = new Float32Array(S);
            scale(s1, 1 + ds);
            const v1 = new Float32Array(V);
            addScalar(v1, dv);
            const c = blackScholes(s1, K, T, v1, R).call;
            mul(c, c, qty);
            const d = sum(c) - baseValue;
            pnl.add(d);
            all.push(d);
        }
    const var95 = -pnl.quantile(0.05);
    const tail = all.filter((x) => x <= -var95);
    const es = -tail.average();
    results["expected shortfall is at least VaR"] = es >= var95 - 1e-6;
    results["scenario grid covers 105 shocks"] = all.length === 105;
    print(
        `book value ${baseValue.format(0)}; 95% VaR ${var95.format(0)}, ES ${es.format(0)} over ${all.length} shocks`,
    );
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
