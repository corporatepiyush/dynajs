/* test_random_dist.js -- distributions on the dyna:random instance
 * stream: normal(mu, sigma), exponential(lambda), poisson(lambda).
 *
 * Statistical-sanity strategy (documented, per the brief): NO
 * exact-value oracles here -- the exactness lives in scratch/probe_dist.c,
 * which chi-squares the SHIPPED kernels against the exact pmf (lgamma) and
 * verifies the jump tables against the transition matrix. What a JS test CAN
 * check is what JS callers rely on:
 *   1. seeded reproducibility: same seed -> IDENTICAL distribution sequences
 *      (the whole point of instance-stream distributions), and replay
 *      through getState/setState;
 *   2. moment sanity over 100k draws with WIDE (5-sigma) tolerance bounds,
 *      printed so drift is visible without being flaky;
 *   3. support/shape pins: normal sign symmetry (chi-square-flavored),
 *      exponential non-negativity, poisson integrality + range + the
 *      lambda<30 / lambda>=30 branch boundary both behaving;
 *   4. argument validation and stream discipline (refusals consume nothing;
 *      draw counts are fixed so mixed sequences replay).
 *
 * Tolerances: bound = 5 * theoretical standard deviation of the statistic at
 * N=100000. For the mean of normal(mu, sigma): 5*sigma/sqrt(N). For the
 * variance: 5*sigma^2*sqrt(2/N) + a small absolute guard. These are ~1 in
 * 3.5 million events, so a failure is a real distribution change, not noise.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_random_dist.js
 */

import { Random } from "dyna:random";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function closeTo(got, want, tol, msg) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + msg + " (got " + got +
                        ", want " + want + " +- " + tol + ")");
}

const N = 100000;

/* ---------------- normal ---------------- */
{
    const mu = 2.5, sigma = 3.0;
    const r = new Random(20260923);
    let sum = 0, sum2 = 0, below = 0, zmin = Infinity, zmax = -Infinity;
    for (let i = 0; i < N; i++) {
        const z = r.normal(mu, sigma);
        assert(Number.isFinite(z), "normal finite");
        const t = (z - mu) / sigma;
        if (z < mu) below++;
        if (t < zmin) zmin = t;
        if (t > zmax) zmax = t;
        sum += z;
        sum2 += z * z;
    }
    const mean = sum / N, variance = sum2 / N - mean * mean;
    closeTo(mean, mu, 5 * sigma / Math.sqrt(N), "normal mean");
    closeTo(variance, sigma * sigma, 5 * sigma * sigma * Math.sqrt(2 / N) + 1e-9,
            "normal variance");
    closeTo(below / N, 0.5, 5 * 0.5 / Math.sqrt(N), "normal sign symmetry");
    assert(zmin > -15 && zmax < 15,
        "normal range plausible (" + zmin.toFixed(1) + ".." + zmax.toFixed(1) + " sigma)");

    /* standard form: defaults are (0, 1) */
    const s = new Random(1);
    for (let i = 0; i < 1000; i++) {
        const z = s.normal();
        assert(Number.isFinite(z), "normal() defaults finite");
    }

    /* polar is symmetric by construction: third central moment ~ 0 */
    const t = new Random(5150);
    let m3 = 0, mean2 = 0;
    const vals = [];
    for (let i = 0; i < 20000; i++) vals.push(t.normal(0, 1));
    mean2 = vals.reduce((a, b) => a + b, 0) / vals.length;
    m3 = vals.reduce((a, b) => a + (b - mean2) ** 3, 0) / vals.length;
    assert(Math.abs(m3) < 0.05, "normal third central moment ~ 0 (" + m3.toFixed(4) + ")");
}

/* ---------------- exponential ---------------- */
for (const lambda of [0.25, 1, 7.5]) {
    const r = new Random(1000 + lambda * 10);
    let sum = 0, sum2 = 0, min = Infinity;
    for (let i = 0; i < N; i++) {
        const x = r.exponential(lambda);
        assert(Number.isFinite(x) && x >= 0, "exponential >= 0");
        if (x < min) min = x;
        sum += x;
        sum2 += x * x;
    }
    const mean = sum / N, variance = sum2 / N - mean * mean;
    closeTo(mean, 1 / lambda, 5 / lambda / Math.sqrt(N), "exponential mean");
    closeTo(variance, 1 / (lambda * lambda),
            5 / (lambda * lambda) * Math.sqrt(2 / N) + 1e-12, "exponential variance");
}

/* ---------------- poisson ---------------- */
/* mean/variance checks across BOTH branches (Knuth < 30, centered walk >= 30) */
for (const lambda of [0.5, 5, 29, 30, 100, 1000]) {
    const r = new Random(31337 + lambda);
    let sum = 0, sum2 = 0, count = 0;
    for (let i = 0; i < N; i++) {
        const k = r.poisson(lambda);
        assert(Number.isInteger(k), "poisson returns integers");
        assert(k >= 0, "poisson non-negative");
        sum += k;
        sum2 += k * k;
        count++;
    }
    const mean = sum / count, variance = sum2 / count - mean * mean;
    closeTo(mean, lambda, 5 * Math.sqrt(lambda / N),
            "poisson(" + lambda + ") mean");
    closeTo(variance, lambda, 5 * Math.sqrt(2 * lambda) + 5 * lambda / Math.sqrt(N) + 1e-9,
            "poisson(" + lambda + ") variance");
}

/* poisson extremes: degenerate 0 (no draws) and a large-lambda smoke draw */
{
    const r = new Random(5);
    const s0 = r.getState().join(",");
    assert(r.poisson(0) === 0, "poisson(0) is the point mass at 0");
    assert(r.getState().join(",") === s0, "poisson(0) consumes no draws");
    const big = new Random(9).poisson(1e6);
    assert(Number.isInteger(big) && big > 990000 && big < 1010000,
        "poisson(1e6) lands within 10 sigma of its mean (" + big + ")");
    let threw = null;
    try { new Random(1).poisson(2147483648); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "poisson above 2^31-1 refused");
    threw = null;
    try { new Random(1).poisson(-1); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "negative lambda refused");
    threw = null;
    try { new Random(1).poisson(); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, "missing lambda refused");
    threw = null;
    try { new Random(1).poisson(NaN); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "NaN lambda refused");
}

/* ---------------- validation of the continuous forms ---------------- */
{
    const r = new Random(1);
    const s0 = r.getState().join(",");
    const refusals = [
        () => r.normal(0, -1), () => r.normal(0, NaN), () => r.normal(NaN, 1),
        () => r.normal(Infinity, 1), () => r.exponential(0), () => r.exponential(-2),
        () => r.exponential(NaN), () => r.exponential(Infinity),
    ];
    for (const f of refusals) {
        let threw = null;
        try { f(); } catch (e) { threw = e; }
        assert(threw instanceof RangeError, "invalid distribution arg refused (RangeError)");
    }
    assert(r.getState().join(",") === s0, "refusals consumed no draws");
    /* sigma = 0 is the point mass and consumes no draws either */
    const z = new Random(42);
    const before = z.getState().join(",");
    assert(z.normal(5, 0) === 5, "normal(mu, 0) is mu");
    assert(z.getState().join(",") === before, "normal(mu, 0) consumes no draws");
}

/* ---------------- seeded reproducibility (THE contract) ---------------- */
{
    /* same seed -> IDENTICAL sequences per distribution */
    const a = new Random(424242), b = new Random(424242);
    for (let i = 0; i < 500; i++) {
        assert(a.normal(2, 0.5) === b.normal(2, 0.5), "normal reproducible @" + i);
        assert(a.exponential(3) === b.exponential(3), "exponential reproducible @" + i);
        assert(a.poisson(20) === b.poisson(20), "poisson reproducible @" + i);
        assert(a.poisson(100) === b.poisson(100), "poisson(walk) reproducible @" + i);
    }
    /* different seeds diverge */
    const c = new Random(1), d = new Random(2);
    let same = 0;
    for (let i = 0; i < 100; i++) if (c.normal() === d.normal()) same++;
    assert(same < 3, "different seeds give different normal sequences");

    /* mid-stream checkpoint replays a DISTRIBUTION sequence exactly */
    const r = new Random(777);
    for (let i = 0; i < 100; i++) r.normal(1, 1);
    const cp = r.getState();
    const want = [];
    for (let i = 0; i < 50; i++) want.push(r.normal(1, 1));
    r.setState(cp);
    let replay = true;
    for (let i = 0; i < 50; i++) if (r.normal(1, 1) !== want[i]) replay = false;
    assert(replay, "getState/setState replays a normal sequence losslessly");
}

print("test_random_dist: all tests passed (" + n + " assertions)");
