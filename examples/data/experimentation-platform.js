// Experimentation platform — deterministic assignment, event collection and a statistically honest readout.
//
// WHAT IT IS
//   The A/B testing service a product team calls on every page view, and the analysis behind its dashboard:
//
//   config      experiments in YAML: variants with weights, a salt, a targeting rule, and a LAYER — experiments in
//               one layer are mutually exclusive, experiments in different layers overlap independently
//   assignment  GET /assign?user=…&country=… hashes salt+user into 10,000 buckets (XXH3), so a user always gets the
//               same variant with no stored state; targeting is evaluated against the request attributes
//   events      POST /events accepts NDJSON batches (exposure, conversion, revenue, latency) and is idempotent per
//               event id: numeric ids live in a RoaringBitmap, so a duplicate check is one tryAdd() in C
//   readout     per experiment and variant:
//                 - sample-ratio mismatch: chi-square on exposures vs configured weights (a broken assignment or a
//                   logging bug shows up here before anyone looks at a metric)
//                 - conversion: two-proportion z-test with a confidence interval on the absolute lift
//                 - revenue per user: Welch's t-test, with and without CUPED (pre-period spend as a covariate),
//                   reporting the variance reduction
//                 - guardrail: p95 latency per variant must not regress more than 10%
//                 - Holm–Bonferroni correction across the primary metrics
//                 - Bayesian: P(variant converts better) under Beta(1, 1) priors, in closed form (a finite sum of
//                   beta functions), checked against numerical integration and scipy
//   peeking     a dashboard that re-tests every day inflates false positives; 2000 simulated A/A tests looked at
//               50 times show it, and the mixture SPRT (always-valid p-values, Johari et al. 2017) keeps the rate
//               under 5% at any stopping time while still detecting a real lift
//               users are interned to dense ids once; each arm is a RoaringBitmap, so "converters in this arm" is
//               andCardinality() and per-user revenue is a Float64Array indexed by id
//
// MODULES  dyna:http · dyna:yaml · dyna:hash (XXH3) · dyna:mathx (stats: normSf, tSf, chi2Sf, ttestInd, proportionsZTest, pAdjust; erfc, betainc, gammainc) · dyna:dataframe
//          · dyna:json (NDJSON) · dyna:structures (RoaringBitmap, Interner) · dyna:cli (Table) · dyna:random
//          · Array extensions (sum)
//
// RUN      dynajs examples/data/experimentation-platform.js --port 8090     serve
//          dynajs examples/data/experimentation-platform.js                 simulates 40,000 users through three experiments

import { App } from "dyna:http";
import * as YAML from "dyna:yaml";
import { XXH3_64 } from "dyna:hash";
import { erfc, betainc, betaln, gammainc, stats } from "dyna:mathx";
import { DataFrame } from "dyna:dataframe";
import { Ndjson as NDJSON } from "dyna:json";
import { RoaringBitmap, Interner } from "dyna:structures";
import { Table, Command } from "dyna:cli";
import { Random } from "dyna:random";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const { options } = new Command("experiments")
    .option("-p, --port <n>", "port", { type: "number" })
    .parse(scriptArgs.slice(1));
const selfTest = options.port === undefined;

const CONFIG = YAML.Parse(`
layers:
  checkout: { buckets: 10000 }
  discovery: { buckets: 10000 }
experiments:
  - key: checkout-v2
    layer: checkout
    range: [0, 5000]
    salt: c2-2026q1
    variants: { control: 50, treatment: 50 }
    primary: conversion
  - key: pricing-banner
    layer: checkout
    range: [5000, 10000]
    salt: pb-7
    variants: { control: 50, banner: 50 }
    primary: revenue
    target: { country: [DE, AT, CH, FR, NL] }
  - key: search-ranker
    layer: discovery
    range: [0, 10000]
    salt: sr-bm25f
    variants: { control: 34, bm25f: 33, learned: 33 }
    primary: conversion
`);

// =============================================================================
// Assignment
// =============================================================================
const bucket = (salt, user) => Number(XXH3_64(`${salt}:${user}`, 0, { as: "bigint" }) % 10000n);
function assign(user, attrs) {
    const out = {};
    for (const [layer] of Object.entries(CONFIG.layers)) {
        const b = bucket(`layer:${layer}`, user); // which experiment in the layer
        const exp = CONFIG.experiments.find((e) => e.layer === layer && b >= e.range[0] && b < e.range[1]);
        if (!exp) continue;
        if (exp.target && !Object.entries(exp.target).every(([k, allowed]) => allowed.includes(attrs[k]))) continue;
        const v = bucket(exp.salt, user),
            total = Object.values(exp.variants).sum();
        let acc = 0;
        for (const [name, w] of Object.entries(exp.variants)) {
            acc += (w / total) * 10000;
            if (v < acc) {
                out[exp.key] = name;
                break;
            }
        }
    }
    return out;
}

// =============================================================================
// Event collection
// =============================================================================
const events = { exposure: [], conversion: [], revenue: [], latency: [] };
const seenNumeric = new RoaringBitmap(),
    seenOther = new Set();
let duplicates = 0;
function firstTime(id) {
    const m = /^e(\d{1,9})$/.exec(id);
    if (m) return seenNumeric.tryAdd(Number(m[1])); // exact, ~2 bytes per id, one native call
    if (seenOther.has(id)) return false;
    seenOther.add(id);
    return true;
}
function collect(body) {
    let accepted = 0;
    for (const e of NDJSON.parse(body)) {
        if (!firstTime(e.id)) {
            duplicates++;
            continue;
        }
        if (!(e.type in events)) continue;
        events[e.type].push(e);
        accepted++;
    }
    return accepted;
}

// =============================================================================
// Statistics
// =============================================================================
// distribution tails, tests and corrections are native (dyna:mathx stats); the special-function identities they
// replace stay below as an independent cross-check
const normSf = stats.normSf,
    chi2Sf = stats.chi2Sf;
const tSf2 = (t, df) => 2 * stats.tSf(Math.abs(t), df); // two-sided p for Student t
const identity = {
    normSf: (z) => 0.5 * erfc(z / Math.SQRT2),
    tSf2: (t, df) => betainc(df / (df + t * t), df / 2, 0.5),
    chi2Sf: (x, k) => gammainc(x / 2, k / 2, "upper"),
};
function twoProportion(c1, n1, c2, n2) {
    const p1 = c1 / n1,
        p2 = c2 / n2,
        p = (c1 + c2) / (n1 + n2);
    const z = (p2 - p1) / Math.sqrt(p * (1 - p) * (1 / n1 + 1 / n2));
    const se = Math.sqrt((p1 * (1 - p1)) / n1 + (p2 * (1 - p2)) / n2);
    return {
        lift: p2 - p1,
        rel: (p2 - p1) / p1,
        ci: [p2 - p1 - 1.96 * se, p2 - p1 + 1.96 * se],
        p: stats.proportionsZTest(c2, n2, c1, n1).pvalue,
        z,
    };
}
function welch(a, b) {
    const ma = stats.mean(a),
        mb = stats.mean(b),
        va = stats.variance(a),
        vb = stats.variance(b);
    const t = stats.ttestInd(b, a, { equalVar: false });
    return { diff: mb - ma, p: t.pvalue, se: Math.sqrt(va / a.length + vb / b.length), t: t.statistic, df: t.df };
}
function cuped(y, x) {
    // y - theta (x - mean x), theta = cov/var over all units
    const theta = stats.cov(x, y) / stats.variance(x),
        mx = stats.mean(x);
    return y.map((v, i) => v - theta * (x[i] - mx));
}
// P(pB > pA) for Beta(aA, bA) and Beta(aB, bB) posteriors (Evan Miller's sum; aB terms)
function probBetter(aA, bA, aB, bB) {
    let p = 0;
    for (let i = 0; i < aB; i++)
        p += Math.exp(betaln(aA + i, bA + bB) - Math.log(bB + i) - betaln(1 + i, bB) - betaln(aA, bA));
    return p;
}
// Mixture SPRT on the difference of two proportions with a N(0, tau^2) mixing prior: the likelihood ratio after
// n users per arm; reject once it reaches 1/alpha. min(1, 1/ratio) over the looks is an always-valid p-value.
function msprtRatio(cA, cB, n, tau2 = 0.02 ** 2) {
    const pA = cA / n,
        pB = cB / n,
        pool = (cA + cB) / (2 * n),
        V = (2 * pool * (1 - pool)) / n;
    if (!(V > 0)) return 1;
    return Math.sqrt(V / (V + tau2)) * Math.exp((tau2 * (pB - pA) ** 2) / (2 * V * (V + tau2)));
}
const holmCheck = [];
function holm(ps) {
    const order = ps.map((p, i) => [p, i]).sort((a, b) => a[0] - b[0]);
    const adj = new Array(ps.length);
    let running = 0;
    order.forEach(([p, i], k) => {
        running = Math.max(running, Math.min(1, p * (ps.length - k)));
        adj[i] = running;
    });
    return adj;
}

function readout(prePeriod) {
    const readouts = [];
    const ids = new Interner(); // user -> dense id, shared by every experiment
    ids.internMany([...prePeriod.keys()]);
    const converters = new RoaringBitmap(ids.internMany(events.conversion.map((e) => e.user)));
    const revenue = new Float64Array(ids.size + events.revenue.length),
        pre = new Float64Array(revenue.length);
    for (const e of events.revenue) revenue[ids.intern(e.user)] += e.amount;
    for (const [u, x] of prePeriod) pre[ids.intern(u)] = x;
    const latUser = ids.internMany(events.latency.map((e) => e.user));
    for (const exp of CONFIG.experiments) {
        const variants = Object.keys(exp.variants);
        const arm = Object.fromEntries(variants.map((v) => [v, new RoaringBitmap()]));
        const exposed = new RoaringBitmap(),
            variantOf = new Int8Array(revenue.length).fill(-1);
        for (const e of events.exposure) {
            if (e.exp !== exp.key) continue;
            const id = ids.intern(e.user);
            if (exposed.tryAdd(id)) {
                arm[e.variant].add(id);
                variantOf[id] = variants.indexOf(e.variant);
            } // first exposure wins
        }
        const lat = events.latency.filter((e, i) => variantOf[latUser[i]] >= 0);
        const latFrame = new DataFrame({
            v: Float64Array.from(lat, (e) => variantOf[ids.lookup(e.user)]),
            ms: Float64Array.from(lat, (e) => e.ms),
        });

        const total = Object.values(exp.variants).sum(),
            n = exposed.size;
        const chi = variants.reduce((s, v) => {
            const expct = (n * exp.variants[v]) / total;
            return s + (arm[v].size - expct) ** 2 / expct;
        }, 0);
        const srmP = chi2Sf(chi, variants.length - 1);
        const control = variants[0];
        const perUser = (bm, values) => Float64Array.from(bm.toArray(), (id) => values[id]);
        const arms = variants.slice(1).map((v) => {
            const cA = arm[control].andCardinality(converters),
                cB = arm[v].andCardinality(converters);
            const conversion = twoProportion(cA, arm[control].size, cB, arm[v].size);
            const ya = Array.from(perUser(arm[control], revenue)),
                yb = Array.from(perUser(arm[v], revenue));
            const both = arm[control].clone().or(arm[v]).toArray();
            const index = new Map(Array.from(both, (id, i) => [id, i]));
            const adjAll = cuped(
                Array.from(both, (id) => revenue[id]),
                Array.from(both, (id) => pre[id]),
            );
            const adj = [...arm[control].toArray(), ...arm[v].toArray()].map((id) => adjAll[index.get(id)]);
            const raw = welch(ya, yb),
                cu = welch(adj.slice(0, ya.length), adj.slice(ya.length));
            const p95 = (k) => latFrame.FILTER(latFrame.EQ("v", k)).QUANTILE("ms", 0.95);
            const latency = { control: p95(0), variant: p95(variants.indexOf(v)) };
            const cAll = arm[control].size,
                vAll = arm[v].size;
            return {
                variant: v,
                conversion,
                probBetter: probBetter(cA + 1, cAll - cA + 1, cB + 1, vAll - cB + 1),
                // msprtRatio wants equal arms; these differ by a few percent, so the variant's count is rescaled
                alwaysValidP: Math.min(1, 1 / msprtRatio(cA, Math.round((cB * cAll) / vAll), cAll)),
                revenue: raw,
                cuped: cu,
                varianceReduction: 1 - (cu.se / raw.se) ** 2,
                latency,
                guardrailOk: latency.variant <= latency.control * 1.1,
            };
        });
        readouts.push({
            key: exp.key,
            primary: exp.primary,
            n,
            counts: Object.fromEntries(variants.map((v) => [v, arm[v].size])),
            srm: { chi, p: srmP, flagged: srmP < 0.001 },
            arms,
        });
    }
    const primaries = readouts.flatMap((r) =>
        r.arms.map((a) => (r.primary === "conversion" ? a.conversion.p : a.cuped.p)),
    );
    const adj = stats.pAdjust(primaries, "holm");
    holmCheck.push([primaries, adj]);
    let k = 0;
    for (const r of readouts)
        for (const a of r.arms) {
            a.pAdj = adj[k++];
            a.significant = a.pAdj < 0.05 && !r.srm.flagged;
        }
    return readouts;
}

// =============================================================================
// HTTP
// =============================================================================
const app = new App({ port: options.port || 0 });
app.get("/assign", (req) => {
    if (!req.query.user)
        return { status: 400, body: JSON.stringify({ error: "user is required" }), contentType: "application/json" };
    return { user: req.query.user, assignments: assign(req.query.user, req.query) };
});
app.post("/events", (req) => {
    try {
        return { accepted: collect(req.body) };
    } catch (e) {
        return { status: 400, body: JSON.stringify({ error: e.message }), contentType: "application/json" };
    }
});
app.get("/experiments", () =>
    CONFIG.experiments.map(({ key, layer, variants, primary }) => ({ key, layer, variants, primary })),
);
app.start();

if (!selfTest) print(`experimentation platform on :${app.port}`);
else {
    const results = {};
    let peeking;
    const base = `http://127.0.0.1:${app.port}`;
    const rng = new Random(2026);
    const countries = ["DE", "US", "FR", "GB", "NL", "AT", "IN", "CH"];
    const pre = new Map();
    const batch = [];
    let eid = 0,
        assigns = 0;
    const flush = async () => {
        if (!batch.length) return;
        const body = NDJSON.stringify(batch.splice(0));
        await (await fetch(base + "/events", { method: "POST", body })).json();
    };
    for (let u = 0; u < 40000; u++) {
        const user = `user-${u}`,
            country = countries[u % countries.length];
        const spender = rng.exponential(1 / 40); // a user's propensity to spend
        pre.set(user, spender * (0.6 + 0.8 * rng.nextFloat())); // last month's spend correlates
        let a;
        if (u % 40 === 0) {
            a = (await (await fetch(`${base}/assign?user=${user}&country=${country}`)).json()).assignments;
            assigns++;
        } else a = assign(user, { country }); // same function the endpoint runs
        for (const [exp, variant] of Object.entries(a)) {
            if (exp === "pricing-banner" && variant === "banner" && rng.nextFloat() < 0.08) continue; // the banner's logging bug
            batch.push({ id: `e${eid++}`, type: "exposure", user, exp, variant });
        }
        let pConv = Math.min(0.5, (0.1 * spender) / 40); // big spenders buy more often
        if (a["checkout-v2"] === "treatment") pConv *= 1.18;
        const converted = rng.nextFloat() < pConv;
        if (converted) {
            batch.push({ id: `e${eid++}`, type: "conversion", user });
            let amount = spender * (0.7 + 0.6 * rng.nextFloat());
            if (a["pricing-banner"] === "banner") amount *= 1.0; // no real effect on spend
            batch.push({ id: `e${eid++}`, type: "revenue", user, amount: Math.round(amount * 100) / 100 });
        }
        const slow = a["search-ranker"] === "learned" ? 1.35 : 1; // the learned ranker is slower
        batch.push({ id: `e${eid++}`, type: "latency", user, ms: Math.round(rng.exponential(1 / 80) * slow + 20) });
        if (u % 7 === 0) batch.push({ ...batch[batch.length - 1] }); // client retries resend events
        if (batch.length > 2000) await flush();
    }
    await flush();
    const readouts = readout(pre);
    const r = Object.fromEntries(readouts.map((x) => [x.key, x]));

    results["assignment is deterministic"] =
        JSON.stringify(assign("user-77", { country: "DE" })) === JSON.stringify(assign("user-77", { country: "DE" }));
    const both = [...Array(5000).keys()].filter((u) => {
        const a = assign(`user-${u}`, { country: "DE" });
        return a["checkout-v2"] && a["pricing-banner"];
    });
    results["experiments in one layer are mutually exclusive"] = both.length === 0;
    results["targeting excludes countries outside the rule"] =
        !assign("user-5", { country: "US" })["pricing-banner"] || false;
    results[`retried events deduplicated (${duplicates})`] = duplicates > 3000;
    results[`SRM flags the banner's logging bug (p=${r["pricing-banner"].srm.p.toExponential(1)})`] =
        !r["checkout-v2"].srm.flagged && r["pricing-banner"].srm.flagged;
    const cv2 = r["checkout-v2"].arms[0];
    results[
        `checkout-v2 lift detected: ${(cv2.conversion.rel * 100).toFixed(1)}% (p_adj=${cv2.pAdj.toExponential(1)})`
    ] = cv2.significant && cv2.conversion.ci[0] > 0;
    results["the null search experiment is not significant after correction"] = r["search-ranker"].arms.every(
        (a) => !a.significant,
    );
    results["a flagged SRM blocks a significance call"] = r["pricing-banner"].arms[0].significant === false;
    results["CUPED cuts revenue variance by more than 20% everywhere"] = readouts.every((x) =>
        x.arms.every((a) => a.varianceReduction > 0.2),
    );
    const learned = r["search-ranker"].arms.find((a) => a.variant === "learned");
    results["latency guardrail trips for the slow ranker only"] =
        !learned.guardrailOk && r["search-ranker"].arms.find((a) => a.variant === "bm25f").guardrailOk;
    {
        // statistics against scipy 1.18.1 (stats.t.sf, stats.chi2.sf, stats.norm.sf; P(B>A) by integrate.quad of
        // beta.pdf(x, aB, bB) * beta.cdf(x, aA, bA))
        const near = (x, want, rel) => Math.abs(x - want) <= rel * Math.abs(want);
        results["native tails equal the incomplete beta / gamma / erfc identities"] = [
            [tSf2(2.1, 37.3), identity.tSf2(2.1, 37.3)],
            [tSf2(-4.0, 3.2), identity.tSf2(-4.0, 3.2)],
            [chi2Sf(7.8, 2), identity.chi2Sf(7.8, 2)],
            [normSf(1.96), identity.normSf(1.96)],
        ].every(([a, b]) => near(a, b, 1e-12));
        results["native Holm adjustment equals the step-down definition"] = holmCheck.every(([p, adj]) =>
            holm(p).every((v, i) => v === adj[i]),
        );
        results["two-sided t, chi-square and normal tails equal scipy"] =
            near(tSf2(2.1, 37.3), 0.042549519395355936, 1e-10) &&
            near(tSf2(0.5, 9998.7), 0.6170860807536405, 1e-10) &&
            near(tSf2(4.0, 3.2), 0.024817461725749532, 1e-10) &&
            near(chi2Sf(7.8, 2), 0.020241911445804395, 1e-10) &&
            near(chi2Sf(25.0, 1), 5.733031437583878e-7, 1e-8) &&
            near(chi2Sf(3.1, 4), 0.5412323332581949, 1e-10) &&
            near(2 * normSf(1.96), 0.04999579029644087, 1e-10) &&
            near(2 * normSf(3.5), 0.00046525815807105003, 1e-9);
        results["P(B > A) in closed form equals scipy's integral"] =
            near(probBetter(101, 901, 121, 881), 0.9231834389735604, 1e-9) &&
            near(probBetter(10, 40, 14, 36), 0.8297126728126386, 1e-9) &&
            near(probBetter(500, 4500, 560, 4440), 0.9744273193970361, 1e-9);
        // and against a Simpson integration written here: the integral of f_B(x) F_A(x) dx, F_A from betainc
        const simpson = (aA, bA, aB, bB) => {
            const N = 20000,
                h = 1 / N,
                f = (x) =>
                    x <= 0 || x >= 1
                        ? 0
                        : Math.exp((aB - 1) * Math.log(x) + (bB - 1) * Math.log1p(-x) - betaln(aB, bB)) *
                          betainc(x, aA, bA);
            let acc = f(0) + f(1);
            for (let i = 1; i < N; i++) acc += (i % 2 ? 4 : 2) * f(i * h);
            return (acc * h) / 3;
        };
        results["P(B > A) in closed form equals a Simpson integration"] = [
            [30, 270, 41, 259],
            [3, 9, 7, 5],
            [200, 1800, 190, 1810],
        ].every(([a, b, c, d]) => near(probBetter(a, b, c, d), simpson(a, b, c, d), 1e-6));
        const cv = r["checkout-v2"].arms[0];
        results[`P(treatment converts better) for checkout-v2 is ${cv.probBetter.toFixed(4)}`] = cv.probBetter > 0.99;
        results["P(B > A) + P(A > B) = 1"] =
            Math.abs(probBetter(101, 901, 121, 881) + probBetter(121, 881, 101, 901) - 1) < 1e-9;
        results[`the always-valid p for checkout-v2 is significant on its own (${cv.alwaysValidP.toExponential(1)})`] =
            cv.alwaysValidP < 0.05;
    }
    {
        // peeking: A/A tests (p = 10%) looked at after every 200 users per arm, 50 looks. Per-look counts are drawn
        // as rounded normals (np = 20, binomial variance), close enough at this size.
        const LOOKS = 50,
            PER = 200,
            sim = new Random(11),
            draw = (p) => Math.max(0, Math.round(sim.normal(PER * p, Math.sqrt(PER * p * (1 - p)))));
        const run = (pB, sims) => {
            let naive = 0,
                firstLook = 0;
            const ps = [];
            for (let k = 0; k < sims; k++) {
                let cA = 0,
                    cB = 0,
                    naiveHit = false,
                    best = 1,
                    stopped = 0;
                for (let look = 1; look <= LOOKS; look++) {
                    cA += draw(0.1);
                    cB += draw(pB);
                    const n = look * PER;
                    if (!naiveHit && twoProportion(cA, n, cB, n).p < 0.05) naiveHit = true;
                    best = Math.max(best, msprtRatio(cA, cB, n));
                    if (!stopped && 1 / best <= 0.05) stopped = look;
                }
                naive += naiveHit;
                firstLook += stopped;
                ps.push(Math.min(1, 1 / best)); // the always-valid p after the last look
            }
            const rejects = ps.count((p) => p <= 0.05);
            return { naive: naive / sims, ps, msprt: rejects / sims, meanLook: firstLook / Math.max(1, rejects) };
        };
        const aa = run(0.1, 2000),
            ab = run(0.125, 1000);
        peeking = { aa, ab };
        results[`peeking at a naive z-test ${LOOKS} times rejects ${(aa.naive * 100).toFixed(1)}% of A/A tests`] =
            aa.naive > 0.15;
        // Ville's inequality makes the always-valid p super-uniform under the null: P(p <= a) <= a for EVERY a, not
        // only 5% (a single level is too blunt: the method is conservative there, so a weakened ratio still passes)
        const levels = [0.01, 0.02, 0.05, 0.1, 0.2, 0.3, 0.5, 0.7, 0.9];
        const over = levels.filter(
            (a) => aa.ps.count((p) => p <= a) / aa.ps.length > a + 3 * Math.sqrt((a * (1 - a)) / aa.ps.length),
        );
        results[
            `the mixture SPRT's always-valid p is super-uniform on A/A tests at every level (${(aa.msprt * 100).toFixed(1)}% at 5%)`
        ] = over.length === 0;
        results[
            `and still detects a 25% lift in ${(ab.msprt * 100).toFixed(0)}% of tests (mean look ${ab.meanLook.toFixed(1)})`
        ] = ab.msprt > 0.7;
    }
    results["bad requests are 400s"] =
        (await fetch(`${base}/assign`)).status === 400 &&
        (await fetch(base + "/events", { method: "POST", body: "{bad" })).status === 400;

    print("experimentation-platform");
    print(
        `  traffic   40000 users, ${eid} events sent, ${duplicates} duplicates dropped, ${assigns} assignments over HTTP`,
    );
    const rows = [
        [
            "experiment",
            "variant",
            "n",
            "conv lift",
            "p(adj)",
            "P(better)",
            "rev diff (CUPED)",
            "var -",
            "p95 ms",
            "verdict",
        ],
    ];
    for (const x of readouts)
        for (const a of x.arms)
            rows.push([
                x.key,
                a.variant,
                String(x.counts[a.variant]),
                `${(a.conversion.rel * 100).toFixed(1)}%`,
                a.pAdj.toFixed(4),
                a.probBetter.toFixed(3),
                a.cuped.diff.toFixed(2),
                `${(a.varianceReduction * 100).toFixed(0)}%`,
                `${a.latency.variant.toFixed(0)}/${a.latency.control.toFixed(0)}`,
                x.srm.flagged
                    ? "SRM - invalid"
                    : !a.guardrailOk
                      ? "guardrail fail"
                      : a.significant
                        ? "ship"
                        : "no effect",
            ]);
    print(table(rows));
    print(
        `  peeking   A/A, 50 looks: naive z-test ${(peeking.aa.naive * 100).toFixed(1)}% false positives, mSPRT ${(peeking.aa.msprt * 100).toFixed(1)}%; 25% lift found by mSPRT in ${(peeking.ab.msprt * 100).toFixed(0)}% (mean look ${peeking.ab.meanLook.toFixed(1)} of 50)`,
    );
    app.close();
    check(results);
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
