// Bandit experiments — epsilon-greedy, UCB1 and Thompson sampling allocating traffic across variants, and the
// peeking problem in fixed-horizon A/B tests shown by simulation and fixed by a sequential boundary.
//
// WHAT IT IS
//   The two halves of an experimentation platform's decision engine:
//
//   bandits     eight variants with Bernoulli conversion rates; each policy chooses a variant per visitor and learns
//               from the outcome. Thompson sampling draws Beta(1 + wins, 1 + losses) via two Gamma draws
//               (Marsaglia-Tsang, built on Random.normal / nextFloat). Regret is measured against always showing
//               the best variant; epsilon-greedy's grows linearly, Thompson's logarithmically, and UCB1 (which
//               over-explores when the gaps are a point of conversion) must still beat uniform allocation
//   leaderboard posterior means kept in a ZSet, so "current best three" is a range read
//   peeking     2 000 A/A tests (no real difference), each checked ten times as data arrives: stopping at the first
//               |z| > 1.96 rejects far more often than 5%. A Pocock boundary for ten looks (|z| > 2.555) brings the
//               false-positive rate back to about 5%, and still finds a real lift
//   stats       two-proportion z statistics; p-values from mathx.erfc; critical values from mathx.erfinv
//
// MODULES  dyna:random (Random) · dyna:mathx (erfc, erfinv) · dyna:structures (ZSet) · dyna:simd (f64Sum) · dyna:cli
//          · Array/Number extensions
//
// RUN      dynajs examples/ml/bandit-experiments.js

import { Random } from "dyna:random";
import { erfc, erfinv } from "dyna:mathx";
import { ZSet } from "dyna:structures";
import { f64Sum } from "dyna:simd";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1933);
const RATES = [0.04, 0.045, 0.05, 0.038, 0.062, 0.051, 0.044, 0.055];
const BEST = RATES.indexOf(Math.max(...RATES));
const HORIZON = 40_000;

// =============================================================================
// Beta sampling from Gamma draws
// =============================================================================
function gamma(shape) {
    if (shape < 1) return gamma(shape + 1) * rng.nextFloat() ** (1 / shape);
    const d = shape - 1 / 3,
        c = 1 / Math.sqrt(9 * d);
    for (;;) {
        let x, v;
        do {
            x = rng.normal();
            v = 1 + c * x;
        } while (v <= 0);
        v = v ** 3;
        const u = rng.nextFloat();
        if (u < 1 - 0.0331 * x ** 4 || Math.log(u) < 0.5 * x * x + d * (1 - v + Math.log(v))) return d * v;
    }
}
const beta = (a, b) => {
    const x = gamma(a);
    return x / (x + gamma(b));
};
{
    const draws = new Float64Array(20_000);
    for (let i = 0; i < draws.length; i++) draws[i] = beta(3, 7);
    const mean = f64Sum(draws) / draws.length;
    const variance = draws.reduce((s, x) => s + (x - mean) ** 2, 0) / draws.length;
    results["Beta(3,7) sampler has mean 0.3 and variance 0.0191"] =
        Math.abs(mean - 0.3) < 0.005 && Math.abs(variance - 21 / 1100) < 0.001;
}

// =============================================================================
// Policies
// =============================================================================
const policies = {
    "epsilon-greedy 10%": () => {
        const n = new Float64Array(8),
            w = new Float64Array(8);
        return {
            choose: () => (rng.nextFloat() < 0.1 ? rng.nextBounded(8) : argmaxRatio(w, n)),
            update: (a, r) => {
                n[a]++;
                w[a] += r;
            },
            n,
            w,
        };
    },
    UCB1: () => {
        const n = new Float64Array(8),
            w = new Float64Array(8);
        let t = 0;
        return {
            choose: () => {
                t++;
                for (let a = 0; a < 8; a++) if (!n[a]) return a;
                let best = 0,
                    bestV = -Infinity;
                for (let a = 0; a < 8; a++) {
                    const v = w[a] / n[a] + Math.sqrt((2 * Math.log(t)) / n[a]);
                    if (v > bestV) {
                        bestV = v;
                        best = a;
                    }
                }
                return best;
            },
            update: (a, r) => {
                n[a]++;
                w[a] += r;
            },
            n,
            w,
        };
    },
    "Thompson sampling": () => {
        const n = new Float64Array(8),
            w = new Float64Array(8);
        return {
            choose: () => {
                let best = 0,
                    bestV = -1;
                for (let a = 0; a < 8; a++) {
                    const v = beta(1 + w[a], 1 + n[a] - w[a]);
                    if (v > bestV) {
                        bestV = v;
                        best = a;
                    }
                }
                return best;
            },
            update: (a, r) => {
                n[a]++;
                w[a] += r;
            },
            n,
            w,
        };
    },
};
function argmaxRatio(w, n) {
    let best = 0,
        bestV = -1;
    for (let a = 0; a < 8; a++) {
        const v = n[a] ? w[a] / n[a] : 1;
        if (v > bestV) {
            bestV = v;
            best = a;
        }
    }
    return best;
}
const runs = {};
for (const [name, make] of Object.entries(policies)) {
    const p = make();
    let regret = 0;
    const curve = [];
    for (let t = 1; t <= HORIZON; t++) {
        const a = p.choose();
        p.update(a, rng.nextFloat() < RATES[a] ? 1 : 0);
        regret += RATES[BEST] - RATES[a];
        if (t === HORIZON / 2 || t === HORIZON) curve.push(regret);
    }
    const board = new ZSet();
    for (let a = 0; a < 8; a++) board.add(`variant-${a}`, (1 + p.w[a]) / (2 + p.n[a]));
    runs[name] = {
        regret,
        half: curve[0],
        bestShare: p.n[BEST] / HORIZON,
        top3: board.range(0, 2, { reverse: true }).map(([m]) => m),
    };
}
print(
    Table(
        Object.entries(runs).map(([name, r]) => [
            name,
            r.regret.toFixed(1),
            (r.regret / r.half).toFixed(2),
            (100 * r.bestShare).toFixed(1) + "%",
            r.top3.join(", "),
        ]),
        { head: ["policy", "regret", "regret(T)/regret(T/2)", "best-arm share", "posterior top 3"] },
    ),
);
// Epsilon-greedy pays its exploration forever (regret doubles with T); Thompson's regret grows like log T, so each
// doubling adds about the same amount. Projecting both to 4T shows who wins a long-running experiment.
const eps = runs["epsilon-greedy 10%"],
    ts = runs["Thompson sampling"];
const uniformRegret = HORIZON * (RATES[BEST] - RATES.average());
results["epsilon-greedy regret grows linearly"] = eps.regret / eps.half > 1.8;
results["Thompson regret grows sub-linearly"] = ts.regret / ts.half < 1.6;
results["projected to 4T, Thompson beats epsilon-greedy"] = ts.regret + 2 * (ts.regret - ts.half) < 4 * eps.regret;
results["UCB1 beats uniform allocation"] = runs.UCB1.regret < uniformRegret;
results["Thompson puts most traffic on the best variant"] = runs["Thompson sampling"].bestShare > 0.7;
results["the leaderboard's top variant is the true best"] = runs["Thompson sampling"].top3[0] === `variant-${BEST}`;

// =============================================================================
// The peeking problem
// =============================================================================
const zTest = (ca, na, cb, nb) => {
    const p = (ca + cb) / (na + nb),
        se = Math.sqrt(p * (1 - p) * (1 / na + 1 / nb));
    return se ? (cb / nb - ca / na) / se : 0;
};
const twoSidedP = (z) => erfc(Math.abs(z) / Math.SQRT2);
const zCrit = Math.SQRT2 * erfinv(1 - 0.05);
results["erfinv gives the textbook 1.96 critical value"] = Math.abs(zCrit - 1.959964) < 1e-6;
results["erfc p-value at z = 1.96 is 0.05"] = Math.abs(twoSidedP(1.959964) - 0.05) < 1e-6;
function experiment(rateA, rateB, perLook, looks, boundary) {
    let ca = 0,
        cb = 0,
        n = 0;
    for (let k = 1; k <= looks; k++) {
        for (let i = 0; i < perLook; i++) {
            ca += rng.nextFloat() < rateA ? 1 : 0;
            cb += rng.nextFloat() < rateB ? 1 : 0;
        }
        n += perLook;
        if (Math.abs(zTest(ca, n, cb, n)) > boundary) return { rejected: true, at: k };
    }
    return { rejected: false, at: looks };
}
const SIMS = 2000,
    LOOKS = 10,
    PER = 400,
    POCOCK_10 = 2.555;
let peekFp = 0,
    onceFp = 0,
    pocockFp = 0,
    pocockPower = 0;
for (let s = 0; s < SIMS; s++) {
    if (experiment(0.1, 0.1, PER, LOOKS, zCrit).rejected) peekFp++;
    if (experiment(0.1, 0.1, PER * LOOKS, 1, zCrit).rejected) onceFp++;
    if (experiment(0.1, 0.1, PER, LOOKS, POCOCK_10).rejected) pocockFp++;
    if (s < 500 && experiment(0.1, 0.12, PER, LOOKS, POCOCK_10).rejected) pocockPower++;
}
const se = (p) => Math.sqrt((p * (1 - p)) / SIMS);
print(
    Table(
        [
            ["one look at the end", ((100 * onceFp) / SIMS).toFixed(1) + "%"],
            ["peek 10x at 1.96", ((100 * peekFp) / SIMS).toFixed(1) + "%"],
            ["peek 10x, Pocock 2.555", ((100 * pocockFp) / SIMS).toFixed(1) + "%"],
            ["Pocock power for 10% -> 12%", ((100 * pocockPower) / 500).toFixed(1) + "%"],
        ],
        { head: ["A/A design (nominal 5%)", "false-positive rate"] },
    ),
);
results["a single look holds the 5% error rate"] = Math.abs(onceFp / SIMS - 0.05) < 4 * se(0.05);
results["peeking ten times inflates false positives past 15%"] = peekFp / SIMS > 0.15;
results["the Pocock boundary restores about 5%"] = Math.abs(pocockFp / SIMS - 0.05) < 4 * se(0.05);
results["the sequential test still detects a real 20% lift most of the time"] = pocockPower / 500 > 0.6;

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
