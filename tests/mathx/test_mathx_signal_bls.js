// covers: contract dyna:mathx.signal.bls -- Box Least Squares transit periodogram
// Three angles: a planted box transit is recovered (period, duration, depth, epoch, SDE), the kernel is differentially
// checked against the hand-written JS BLS that examples/science/astronomy.js used before the native port (same best
// period index, per-period power to 1e-9 relative), and every documented TypeError / RangeError is provoked.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_signal_bls");
import { signal as S } from "dyna:mathx";

let pass = 0, fail = 0;
function ok(c, msg) {
    if (c) pass++;
    else { fail++; print("FAIL " + msg); }
}
function throws(fn, type, re, msg) {
    try { fn(); ok(false, msg + " (did not throw)"); }
    catch (e) { ok(e instanceof type && re.test(e.message), `${msg} (${e.name}: ${e.message})`); }
}
let seed = 4242;
const uni = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
const gauss = () => Math.sqrt(-2 * Math.log(uni() + 1e-300)) * Math.cos(2 * Math.PI * uni());

function jsBls(t, y, periods, durations, nbins = 400) {
    const results = [];
    let meanF = 0;
    for (const v of y) meanF += v;
    meanF /= y.length;
    for (const P of periods) {
        const binSum = new Float64Array(nbins), binN = new Float64Array(nbins);
        for (let i = 0; i < t.length; i++) {
            const b = Math.floor(((t[i] % P) / P) * nbins);
            binSum[b] += y[i] - meanF;
            binN[b]++;
        }
        let best = { power: 0 };
        const cs = new Float64Array(2 * nbins + 1), cn = new Float64Array(2 * nbins + 1);
        for (let b = 0; b < 2 * nbins; b++) {
            cs[b + 1] = cs[b] + binSum[b % nbins];
            cn[b + 1] = cn[b] + binN[b % nbins];
        }
        for (const dur of durations) {
            const w = Math.max(1, Math.round((dur / P) * nbins));
            for (let s = 0; s < nbins; s++) {
                const sr = cs[s + w] - cs[s], n = cn[s + w] - cn[s];
                if (n < 5) continue;
                const r = n / t.length, power = (sr * sr) / (t.length * r * (1 - r));
                if (power > best.power) best = { power, P, dur, depth: -sr / n, epoch: ((s + w / 2) / nbins) * P };
            }
        }
        results.push(best);
    }
    return results;
}

function lightCurve(n, span, truth, noise) {
    const t = new Float64Array(n), y = new Float64Array(n);
    for (let i = 0; i < n; i++) {
        t[i] = (i * span) / n;
        const ph = (((t[i] - truth.t0) % truth.period) + truth.period) % truth.period;
        const inTr = ph < truth.duration / 2 || ph > truth.period - truth.duration / 2;
        y[i] = 1 - (inTr ? truth.depth : 0) + noise * gauss();
    }
    return { t, y };
}
const grid = (lo, hi, n) => Float64Array.from({ length: n }, (_, i) => lo + ((hi - lo) * i) / (n - 1));

__kitS.section("power is a Float64Array per period", () => {
    const truth = { period: 3.8521, t0: 1.317, depth: 0.0021, duration: 0.118 };
    const { t, y } = lightCurve(19440, 27, truth, 0.0009);
    const coarse = S.bls(t, y, grid(1, 7.7, 1000), [0.06, 0.09, 0.12, 0.16]);
    ok(coarse.power instanceof Float64Array && coarse.power.length === 1000, "power is a Float64Array per period");
    ok(coarse.duration.length === 1000 && coarse.depth.length === 1000 && coarse.epoch.length === 1000, "per-period arrays");
    ok(Math.abs(coarse.best.period - truth.period) < 0.01, `coarse grid finds the planted period (${coarse.best.period})`);
    ok(coarse.best.sde > 7, `planted transit is significant (SDE ${coarse.best.sde})`);
    const P0 = coarse.best.period, fine = S.bls(t, y, grid(P0 - 0.01, P0 + 0.01, 201), [0.1, 0.11, 0.12, 0.13]).best;
    ok(Math.abs(fine.period - truth.period) < 0.002, `fine grid recovers P within 0.002 d (${fine.period})`);
    ok(Math.abs(fine.depth / truth.depth - 1) < 0.2, `depth within 20% (${fine.depth})`);
    ok(Math.abs(fine.duration - truth.duration) < 0.015, `duration within 0.015 d (${fine.duration})`);
    const dphi = Math.abs((((fine.epoch - truth.t0) % fine.period) + 1.5 * fine.period) % fine.period - fine.period / 2);
    ok(dphi < 0.02, `epoch within 0.02 d of the planted mid-transit (${fine.epoch})`);
    ok(fine.epoch >= 0 && fine.epoch < fine.period, "epoch lies in [0, period)");
    ok(fine.power === Math.max(...S.bls(t, y, grid(P0 - 0.01, P0 + 0.01, 201), [0.1, 0.11, 0.12, 0.13]).power), "best.power is the maximum");
    const p = coarse.power, m = p.reduce((a, b) => a + b, 0) / p.length;
    const sd = Math.sqrt(p.reduce((a, b) => a + (b - m) ** 2, 0) / (p.length - 1));
    ok(Math.abs(coarse.best.sde - (coarse.best.power - m) / sd) < 1e-9 * coarse.best.sde, "sde = (max - mean) / sample stddev");
    ok(coarse.power[coarse.best.index] === coarse.best.power && coarse.duration[coarse.best.index] === coarse.best.duration, "best.index addresses the arrays");
});

for (let trial = 0; trial < 6; trial++) {
    const n = 1500 + Math.floor(uni() * 2500), span = 10 + uni() * 30;
    const t = new Float64Array(n), y = new Float64Array(n);
    for (let i = 0; i < n; i++) { t[i] = uni() * span; y[i] = 1 + 0.001 * gauss(); }
    if (trial % 2) {
        const P = 1.5 + uni() * 4, d = 0.05 + uni() * 0.1;
        for (let i = 0; i < n; i++) if (t[i] % P < d) y[i] -= 0.002;
    }
    const nb = [400, 200, 997][trial % 3];
    const periods = grid(1, 6, 300), durs = [0.04, 0.08, 0.13], ref = jsBls(t, y, periods, durs, nb);
    const got = S.bls(t, y, periods, durs, { bins: nb });
    const refTop = ref.reduce((a, b, i) => (b.power > ref[a].power ? i : a), 0);
    ok(got.best.index === refTop, `trial ${trial}: same best period index (${got.best.index} vs ${refTop})`);
    let worst = 0, meta = true;
    for (let k = 0; k < periods.length; k++) {
        const r = ref[k];
        worst = Math.max(worst, Math.abs(got.power[k] - r.power) / Math.max(r.power, 1e-300));
        if (r.power > 0) {
            const ep = r.epoch >= r.P ? r.epoch - r.P : r.epoch;
            meta &&= got.duration[k] === r.dur && Math.abs(got.depth[k] - r.depth) <= 1e-9 * Math.abs(r.depth) + 1e-15 && Math.abs(got.epoch[k] - ep) < 1e-12 * r.P;
        }
    }
    ok(worst < 1e-9, `trial ${trial}: per-period power within 1e-9 relative (worst ${worst})`);
    ok(meta, `trial ${trial}: duration, depth and epoch agree with the JS reference`);
}

__kitS.section("unsorted times give the same periodogram", () => {
    const { t, y } = lightCurve(3000, 20, { period: 2.3, t0: 0.4, depth: 0.004, duration: 0.1 }, 0.001);
    const order = Array.from(t, (_, i) => i).sort(() => uni() - 0.5);
    const a = S.bls(t, y, grid(1, 4, 120), [0.08, 0.1]);
    const b = S.bls(order.map((i) => t[i]), order.map((i) => y[i]), grid(1, 4, 120), [0.08, 0.1]);
    ok(a.best.index === b.best.index && a.power.every((v, k) => Math.abs(v - b.power[k]) <= 1e-9 * v), "unsorted times give the same periodogram");
    ok(S.bls(t, y, [2.3, 2.3, 1.7], [0.1]).best.index === 0, "best is the first of tied maximal periods");
    ok(Array.isArray([...a.power]) && S.bls(Array.from(t), Array.from(y), [2.3], [0.1]).power.length === 1, "plain arrays are accepted");
});
__kitS.section("negative times fold into [0, period)", () => {
    const t = Float64Array.from({ length: 1280 }, (_, i) => i / 64), y = Float64Array.from(t, (v) => (v % 2 < 0.25 ? 0.99 : 1));
    const a = S.bls(t, y, [2], [0.25]), b = S.bls(t.map((v) => v - 40), y, [2], [0.25]);
    ok(a.power[0] > 0 && a.power[0] === b.power[0] && a.epoch[0] === b.epoch[0], "negative times fold into [0, period)");
    ok(Math.abs(a.depth[0] - 0.00875) < 1e-10, `a noiseless box gives its depth below the global mean (${a.depth[0]})`);
});
__kitS.section("durations at or above the period are skipped", () => {
    const t = grid(0, 10, 200), y = Float64Array.from(t, (_, i) => 1 + (i % 7) * 1e-4);
    const r = S.bls(t, y, [1, 2], [5]);
    ok(r.power.every((v) => v === 0) && r.best.index === -1, "durations at or above the period are skipped");
    ok(Number.isNaN(r.duration[0]) && Number.isNaN(r.depth[0]) && Number.isNaN(r.epoch[0]) && Number.isNaN(r.best.period), "a period with no box reports NaN fields");
    ok(Number.isNaN(r.best.sde) && r.best.power === 0, "no box anywhere gives power 0 and sde NaN");
    const m = S.bls(t, y, [1, 2], [0.1], { minPoints: 200 });
    ok(m.best.index === -1, "minPoints above every box's count leaves no candidate");
    ok(S.bls(t, y, [1, 2], [0.1], { minPoints: 1 }).best.index >= 0, "minPoints 1 admits single-point boxes");
    const flat = S.bls(t, new Float64Array(200).fill(1), [1, 2], [0.1]);
    ok(flat.power.every((v) => v === 0) && flat.best.index === -1, "a constant light curve has zero power");
});
__kitS.section("missing t", () => {
    const t = [0, 1, 2, 3, 4, 5], y = [1, 1, 1, 1, 1, 0.5];
    throws(() => S.bls(), TypeError, /t must be an array/, "missing t");
    throws(() => S.bls(t, 5, [1], [0.1]), TypeError, /y must be an array/, "y not an array");
    throws(() => S.bls(t, y, 1, [0.1]), TypeError, /periods must be an array/, "periods not an array");
    throws(() => S.bls(t, y, [1], 0.1), TypeError, /durations must be an array/, "durations not an array");
    throws(() => S.bls(t, y, [1], [0.1], 3), TypeError, /opts must be an object/, "opts not an object");
    throws(() => S.bls(t, y, [1], [0.1], { nbins: 3 }), TypeError, /unknown option 'nbins'/, "unknown option");
    throws(() => S.bls(t, y, [1], [0.1], { bins: "4" }), TypeError, /bins must be a number/, "bins not a number");
    throws(() => S.bls(t, y.slice(1), [1], [0.1]), RangeError, /lengths differ/, "t and y lengths differ");
    throws(() => S.bls([0, 1, 2, 3], [1, 1, 1, 1], [1], [0.1]), RangeError, /at least max\(2, minPoints\)/, "too few points for minPoints 5");
    throws(() => S.bls([0], [1], [1], [0.1], { minPoints: 1 }), RangeError, /at least max\(2, minPoints\)/, "one point");
    throws(() => S.bls([0, NaN, 2, 3, 4, 5], y, [1], [0.1]), RangeError, /must be finite/, "NaN time");
    throws(() => S.bls(t, [1, 1, Infinity, 1, 1, 1], [1], [0.1]), RangeError, /must be finite/, "infinite flux");
    throws(() => S.bls(t, y, [], [0.1]), RangeError, /non-empty/, "no periods");
    throws(() => S.bls(t, y, [1], []), RangeError, /non-empty/, "no durations");
    throws(() => S.bls(t, y, [1, 0], [0.1]), RangeError, /periods must be finite and > 0/, "zero period");
    throws(() => S.bls(t, y, [1, NaN], [0.1]), RangeError, /periods must be finite and > 0/, "NaN period");
    throws(() => S.bls(t, y, [1], [0]), RangeError, /durations must be finite and > 0/, "zero duration");
    throws(() => S.bls(t, y, [1], [0.1, -0.2]), RangeError, /durations must be finite and > 0/, "negative duration");
    throws(() => S.bls(t, y, [1], [0.1], { bins: 1 }), RangeError, /bins must be an integer in \[2, 1048576\]/, "bins below 2");
    throws(() => S.bls(t, y, [1], [0.1], { bins: 2.5 }), RangeError, /bins must be an integer/, "fractional bins");
    throws(() => S.bls(t, y, [1], [0.1], { bins: 1048577 }), RangeError, /bins must be an integer/, "bins too large");
    throws(() => S.bls(t, y, [1], [0.1], { minPoints: 0 }), RangeError, /minPoints must be a positive integer/, "minPoints 0");
    ok(S.bls(t, y, [1], [0.1], { bins: 2, minPoints: 1 }).power.length === 1, "bins 2 is allowed");
    const tt = Float64Array.from(t), yy = Float64Array.from(y);
    S.bls(tt, yy, [2], [0.5], { minPoints: 1 });
    ok(tt.every((v, i) => v === t[i]) && yy.every((v, i) => v === y[i]), "inputs are not modified");
});
__kitS.end();

print(`signal.bls: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(`${fail} failures`);
