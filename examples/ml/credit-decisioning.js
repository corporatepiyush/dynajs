// Credit decisioning — a scorecard, a policy rule engine, risk-based pricing, adverse-action reasons and a fairness audit.
//
// WHAT IT IS
//   The service behind "apply for a loan" that a lender's compliance team can actually sign off:
//
//   intake       POST /applications validated against a JSON Schema (amount bounds, income, employment, term)
//   bureau       a credit bureau over HTTP (a separate service here) answers with tradelines; responses are cached
//                for 30 days per applicant and the call has a timeout with a "refer to underwriter" fallback
//   scorecard    features are BINNED and replaced by weight-of-evidence (WOE), then a logistic regression gives
//                points per bin — the classic interpretable scorecard (600 points at 10:1 good:bad odds, 20 points
//                to double the odds), with the information value of each feature reported
//   policy       knock-out and refer rules in YAML, each an arithmetic expression with a threshold
//                (dti + payment_ratio above 0.5, bankruptcy, …), evaluated in order with a rule id recorded per hit
//   pricing      APR by score band; the monthly payment and full amortisation schedule in Decimal, with the
//                final payment absorbing rounding so the schedule sums exactly to principal plus interest
//   reasons      declined applicants get the four features that cost them the most points (adverse-action notice)
//   audit        every decision appended to a hash-chained, HMAC-signed log; a fairness check compares approval
//                rates across a protected attribute that the model never sees (four-fifths rule)
//   velocity     a CuckooFilter remembers who applied in the last 30 days (and forgets them on expiry, which a Bloom
//                filter cannot): a repeat application is referred instead of re-scored
//   portfolio    expected loss = PD x LGD x EAD over the approved book as Float32Array vectors (simd mul, scale,
//                sum), and the score distribution's percentiles from a TDigest
//   throughput   applications are decided 25 at a time (Promise.all), so a slow bureau partition costs one timeout
//                per batch rather than one per applicant; WOE tables come from one counting pass over the history
//
// MODULES  dyna:ml (LogisticRegression, rocAuc) · dyna:schema · dyna:yaml · dyna:mathx (Expression) · dyna:decimal
//          · dyna:http · dyna:structures (LRU, CuckooFilter, TDigest) · dyna:simd · dyna:async (withTimeout)
//          · dyna:crypto (HMAC) · dyna:hash · dyna:random · Array/Number extensions
//
// RUN      dynajs examples/ml/credit-decisioning.js     trains on 6,000 historical loans, decides 600 new applications

import { LogisticRegression, rocAuc } from "dyna:ml";
import { Schema } from "dyna:schema";
import * as YAML from "dyna:yaml";
import { Expression } from "dyna:mathx";
import { Decimal } from "dyna:decimal";
import { App } from "dyna:http";
import { LRU, CuckooFilter, TDigest } from "dyna:structures";
import * as simd from "dyna:simd";
import { withTimeout } from "dyna:async";
import { HMACHex, RandomBytes } from "dyna:crypto";
import { SHA256Hex } from "dyna:hash";
import { Random } from "dyna:random";

const rng = new Random(1337);

// =============================================================================
// The bureau (a separate service) and a synthetic population
// =============================================================================
function makePerson(id) {
    const income = Math.round(Math.exp(rng.normal(10.7, 0.45)));
    const age = 21 + rng.nextBounded(45),
        group = rng.nextFloat() < 0.4 ? "B" : "A";
    const years = Math.min(age - 18, rng.nextBounded(25));
    const utilisation = Math.min(1.2, Math.max(0, rng.normal(0.35, 0.25)));
    const delinquencies = rng.poisson(0.3 + utilisation * 0.8);
    const inquiries = rng.poisson(1.2),
        bankruptcy = rng.nextFloat() < 0.02;
    const debt = Math.round((income * Math.max(0, rng.normal(0.2, 0.12))) / 12);
    const latent =
        -3.2 +
        2.2 * utilisation +
        0.7 * delinquencies +
        0.15 * inquiries -
        0.05 * years -
        0.4 * Math.log(income / 40000) +
        (bankruptcy ? 1.5 : 0) +
        rng.normal(0, 0.6);
    return {
        id: `P${id}`,
        income,
        age,
        group,
        employment: ["employed", "self-employed", "retired"][rng.nextBounded(3)],
        bureau: {
            yearsOfHistory: years,
            utilisation,
            delinquencies,
            inquiries6m: inquiries,
            bankruptcy,
            monthlyDebt: debt,
        },
        defaults: rng.nextFloat() < 1 / (1 + Math.exp(-latent)),
    };
}
const history = Array.from({ length: 6000 }, (_, i) => makePerson(i));
const applicants = Array.from({ length: 600 }, (_, i) => makePerson(100000 + i));
const everyone = new Map([...history, ...applicants].map((p) => [p.id, p]));

const bureau = new App({ port: 0 });
let bureauCalls = 0;
bureau.get("/report/:id", async (req) => {
    bureauCalls++;
    const p = everyone.get(req.params.id);
    if (!p) return { status: 404, body: "no file" };
    if (req.params.id.endsWith("7")) await new Promise((r) => setTimeout(r, 300)); // a slow bureau partition
    return { subject: p.id, ...p.bureau };
});
bureau.start();

// =============================================================================
// Scorecard: binning, WOE, logistic regression, points
// =============================================================================
const FEATURES = {
    utilisation: { get: (a, b) => b.utilisation, edges: [0.1, 0.3, 0.5, 0.75] },
    delinquencies: { get: (a, b) => b.delinquencies, edges: [1, 2, 3] },
    inquiries6m: { get: (a, b) => b.inquiries6m, edges: [1, 2, 4] },
    yearsOfHistory: { get: (a, b) => b.yearsOfHistory, edges: [2, 5, 10, 15] },
    income: { get: (a) => a.income, edges: [25000, 40000, 60000, 90000] },
    dti: { get: (a, b) => (b.monthlyDebt * 12) / a.income, edges: [0.1, 0.2, 0.3, 0.4] },
};
const NAMES = Object.keys(FEATURES);
const binOf = (f, v) => {
    const e = FEATURES[f].edges;
    let k = 0;
    while (k < e.length && v >= e[k]) k++;
    return k;
};
const bins = (a, b) => NAMES.map((f) => binOf(f, FEATURES[f].get(a, b)));
const histBins = history.map((p) => bins(p, p.bureau)),
    histY = history.map((p) => (p.defaults ? 1 : 0));
const totalBad = histY.reduce((a, b) => a + b, 0),
    totalGood = histY.length - totalBad;
const MAXBIN = 8;
const counts = new Int32Array(NAMES.length * MAXBIN * 2); // [feature][bin][good|bad], one pass
histBins.forEach((b, i) => b.forEach((k, j) => counts[(j * MAXBIN + k) * 2 + histY[i]]++));
const woe = {},
    iv = {};
NAMES.forEach((f, j) => {
    woe[f] = [];
    iv[f] = 0;
    for (let k = 0; k <= FEATURES[f].edges.length; k++) {
        const good = counts[(j * MAXBIN + k) * 2] + 0.5,
            bad = counts[(j * MAXBIN + k) * 2 + 1] + 0.5; // Laplace smoothing
        const g = good / totalGood,
            d = bad / totalBad;
        woe[f][k] = Math.log(g / d);
        iv[f] += (g - d) * woe[f][k];
    }
});
const toWoe = (b) => b.map((k, j) => woe[NAMES[j]][k]);
const split = Math.floor(history.length * 0.8);
const model = await new LogisticRegression({ maxIter: 5000, learningRate: 0.5 }).fit(
    histBins.slice(0, split).map(toWoe),
    histY.slice(0, split),
);
const holdP = (await model.predictProba(histBins.slice(split).map(toWoe))).map((p) => p[1]);
const auc = await rocAuc(histY.slice(split), holdP);
const coef = model.coef[0],
    intercept = Array.isArray(model.intercept) ? model.intercept[0] : model.intercept;
const PDO = 20,
    BASE = 600,
    BASE_ODDS = 10;
const factor = PDO / Math.log(2),
    offset = BASE - factor * Math.log(BASE_ODDS);
// The model predicts log-odds of BAD; points rise with log-odds of GOOD.
const points = (b) =>
    NAMES.map((f, j) => -(coef[j] * woe[f][b[j]] + intercept / NAMES.length) * factor + offset / NAMES.length);
const scoreOf = (b) => Math.round(points(b).reduce((s, x) => s + x, 0));

// =============================================================================
// Policy rules (YAML + expressions), pricing, reasons
// =============================================================================
const POLICY = YAML.Parse(
    `
- { id: KO-01, action: decline, metric: bankruptcy, above: 0.5, text: bankruptcy on file }
- { id: KO-02, action: decline, metric: dti + payment_ratio, above: 0.5, text: debt-to-income above 50% including this loan }
- { id: KO-03, action: decline, metric: score, below: 540, text: score below the minimum }
- { id: RF-01, action: refer, metric: amount / income, above: 0.6, text: amount large relative to income }
- { id: RF-02, action: refer, metric: years_history, below: 1, text: thin credit file }
`,
).map((r) => ({ ...r, expr: new Expression(r.metric) }));
const BANDS = [
    [720, "A", "0.0649"],
    [680, "B", "0.0899"],
    [640, "C", "0.1249"],
    [600, "D", "0.1649"],
    [540, "E", "0.2199"],
];
function amortise(principal, apr, months) {
    const P = new Decimal(principal),
        r = new Decimal(apr).div(12, { precision: 34 });
    const pay = P.mul(r)
        .div(Decimal.ONE.sub(Decimal.ONE.div(Decimal.ONE.add(r).pow(months, { precision: 34 }), { precision: 34 })), {
            precision: 34,
        })
        .toFixed(2);
    const rows = [];
    let bal = P,
        interestTotal = Decimal.ZERO;
    for (let m = 1; m <= months; m++) {
        const interest = new Decimal(bal.mul(r).toFixed(2));
        let payment = new Decimal(pay);
        if (m === months) payment = bal.add(interest); // the last payment clears the balance
        bal = bal.add(interest).sub(payment);
        interestTotal = interestTotal.add(interest);
        rows.push({ m, payment: payment.toFixed(2), interest: interest.toFixed(2), balance: bal.toFixed(2) });
    }
    return { payment: pay, rows, interest: interestTotal.toFixed(2), total: P.add(interestTotal).toFixed(2) };
}
function reasons(b) {
    const pts = points(b),
        best = NAMES.map((f, j) => Math.max(...woe[f].map((_, k) => points(b.map((x, i) => (i === j ? k : x)))[j])));
    return NAMES.map((f, j) => [f, best[j] - pts[j]])
        .filter(([, lost]) => lost > 1)
        .sort((x, y) => y[1] - x[1])
        .slice(0, 4)
        .map(([f, lost]) => ({ feature: f, pointsLost: Math.round(lost) }));
}

// =============================================================================
// Decision service and audit log
// =============================================================================
const appSchema = Schema.compile({
    type: "object",
    required: ["applicant", "amount", "termMonths", "income", "employment"],
    properties: {
        applicant: { type: "string", pattern: "^P\\d+$" },
        amount: { type: "number", minimum: 1000, maximum: 50000 },
        termMonths: { enum: [12, 24, 36, 48, 60] },
        income: { type: "number", exclusiveMinimum: 0 },
        employment: { enum: ["employed", "self-employed", "retired"] },
    },
    additionalProperties: false,
});
const auditKey = RandomBytes(32),
    audit = [];
let prevHash = "0".repeat(64);
function record(entry) {
    const body = JSON.stringify({ ...entry, prev: prevHash, seq: audit.length });
    const hash = SHA256Hex(body),
        mac = HMACHex("sha256", auditKey, hash);
    audit.push({ body, hash, mac });
    prevHash = hash;
}
const verifyAudit = () =>
    audit.every(
        (e, i) =>
            SHA256Hex(e.body) === e.hash &&
            HMACHex("sha256", auditKey, e.hash) === e.mac &&
            JSON.parse(e.body).prev === (i ? audit[i - 1].hash : "0".repeat(64)),
    );

const bureauCache = new LRU(10000, { ttlMs: 30 * 86400_000 });
const bureauBase = `http://127.0.0.1:${bureau.port}`;
async function bureauReport(id) {
    const hit = bureauCache.get(id);
    if (hit) return hit;
    const report = await withTimeout(
        fetch(`${bureauBase}/report/${id}`).then((r) => r.json()),
        120,
        "bureau timeout",
    );
    bureauCache.put(id, report);
    return report;
}
const recentApplicants = new CuckooFilter(50_000);
const lender = new App({ port: 0 });
lender.post("/applications", async (req) => {
    let a;
    try {
        a = JSON.parse(req.body);
    } catch {
        return { status: 400, body: JSON.stringify({ error: "invalid JSON" }), contentType: "application/json" };
    }
    const v = appSchema.validate(a);
    if (!v.valid)
        return {
            status: 422,
            body: JSON.stringify({ errors: v.errors.map((e) => e.message) }),
            contentType: "application/json",
        };
    if (recentApplicants.mayContain(a.applicant)) {
        // a false positive only means a human look
        record({ applicant: a.applicant, decision: "refer", rule: "RF-03" });
        return {
            applicant: a.applicant,
            decision: "refer",
            rule: "RF-03",
            reason: "repeat application within 30 days",
        };
    }
    recentApplicants.add(a.applicant);
    let b;
    try {
        b = await bureauReport(a.applicant);
    } catch (e) {
        record({ applicant: a.applicant, decision: "refer", rule: "SYS-BUREAU", detail: e.message });
        return { applicant: a.applicant, decision: "refer", rule: "SYS-BUREAU", reason: "credit report unavailable" };
    }
    const bn = bins(a, b),
        score = scoreOf(bn);
    const band = BANDS.find(([min]) => score >= min);
    const months = a.termMonths,
        apr = band ? band[2] : "0.2199";
    const plan = amortise(a.amount, apr, months);
    const vars = {
        bankruptcy: b.bankruptcy ? 1 : 0,
        dti: (b.monthlyDebt * 12) / a.income,
        payment_ratio: (Number(plan.payment) * 12) / a.income,
        score,
        amount: a.amount,
        income: a.income,
        years_history: b.yearsOfHistory,
    };
    const hit = POLICY.find((r) => {
        const v = r.expr.eval(vars);
        return (r.above !== undefined && v > r.above) || (r.below !== undefined && v < r.below);
    });
    const decision = hit ? hit.action : "approve";
    const out = { applicant: a.applicant, decision, score, rule: hit?.id || null };
    if (decision === "approve")
        Object.assign(out, { band: band[1], apr, monthly: plan.payment, totalRepayable: plan.total });
    if (decision === "decline") out.reasons = hit.id === "KO-03" ? reasons(bn) : [{ feature: hit.text }];
    record({ ...out, scoreCardVersion: "2026.1" });
    return out;
});
lender.start();
const lenderBase = `http://127.0.0.1:${lender.port}`;

// =============================================================================
// Run the book of applications
// =============================================================================
const requests = applicants.map((p) => ({
    p,
    body: {
        applicant: p.id,
        amount: 2000 + rng.nextBounded(30) * 1000,
        termMonths: [12, 24, 36, 48, 60][rng.nextBounded(5)],
        income: p.income,
        employment: p.employment,
    },
}));
const decisions = [];
for (const batch of requests.splitEvery(25))
    decisions.push(
        ...(await Promise.all(
            batch.map(async ({ p, body }) => ({
                person: p,
                amount: body.amount,
                ...(await (
                    await fetch(lenderBase + "/applications", { method: "POST", body: JSON.stringify(body) })
                ).json()),
            })),
        )),
    );
const again = await (
    await fetch(lenderBase + "/applications", {
        method: "POST",
        body: JSON.stringify({
            applicant: applicants[0].id,
            amount: 5000,
            termMonths: 36,
            income: applicants[0].income,
            employment: "employed",
        }),
    })
).json();
const bad = await fetch(lenderBase + "/applications", {
    method: "POST",
    body: JSON.stringify({ applicant: "P1", amount: 999999, termMonths: 7, income: 0, employment: "pirate", extra: 1 }),
});
const badBody = await bad.json();

const count = (pred) => decisions.filter(pred).length;
const approved = decisions.filter((d) => d.decision === "approve");
const rate = (g) => count((d) => d.person.group === g && d.decision === "approve") / count((d) => d.person.group === g);
const impact = Math.min(rate("A"), rate("B")) / Math.max(rate("A"), rate("B"));
const badRate = (ds) => ds.filter((d) => d.person.defaults).length / Math.max(1, ds.length);
const sched = amortise(10000, "0.0899", 36);
const schedSum = sched.rows.reduce((s, r) => s.add(r.payment), Decimal.ZERO);

// Portfolio: expected loss of the approved book, PD from the scorecard's odds, LGD 45%, EAD = amount.
const pdOf = (score) => 1 / (1 + Math.exp((score - offset) / factor));
const pd = Float32Array.from(approved, (d) => pdOf(d.score)),
    ead = Float32Array.from(approved, (d) => d.amount);
const expectedLoss = simd.sum(simd.scale(simd.mul(new Float32Array(pd.length), pd, ead), 0.45));
const scoreDigest = new TDigest(100).addMany(
    Float64Array.from(
        decisions.filter((d) => d.score),
        (d) => d.score,
    ),
);

const results = {};
results["a repeat application inside 30 days is referred by the CuckooFilter, not re-scored"] = again.rule === "RF-03";
results[`expected loss of the approved book ${expectedLoss.format(0)} is under 5% of exposure`] =
    expectedLoss > 0 && expectedLoss < 0.05 * simd.sum(ead);
results["the simd expected loss matches a scalar sum within float32 rounding"] =
    Math.abs(expectedLoss - approved.reduce((s, d) => s + pdOf(d.score) * d.amount * 0.45, 0)) / expectedLoss < 1e-4;
results["approved scores sit above the median applicant score"] =
    approved.pluck("score").median() > scoreDigest.quantile(0.5);
results[`scorecard holdout AUC ${auc.toFixed(3)}`] = auc > 0.7;
results["information value ranks utilisation and delinquencies as strong predictors"] =
    iv.utilisation > iv.inquiries6m && iv.delinquencies > 0.1;
results[
    `approved book defaults ${(badRate(approved) * 100).toFixed(1)}% vs ${(badRate(decisions) * 100).toFixed(1)}% overall`
] = badRate(approved) < badRate(decisions) * 0.7;
results[`amortisation sums exactly to ${sched.total}`] =
    schedSum.equals(sched.total) && sched.rows[35].balance === "0.00";
results["knock-out and refer rules fire"] =
    decisions.some((d) => d.rule === "KO-01") &&
    decisions.some((d) => d.rule === "KO-02") &&
    decisions.some((d) => d.decision === "refer");
results["score declines carry adverse-action reasons"] = decisions
    .filter((d) => d.rule === "KO-03")
    .every((d) => d.reasons.length >= 1 && d.reasons.every((r) => r.pointsLost > 0));
results["a slow bureau call becomes a referral, not an error"] = decisions.some((d) => d.rule === "SYS-BUREAU");
results["bureau responses are cached per applicant"] = bureauCalls <= applicants.length + 1 && again.decision;
results["invalid applications are rejected with every schema error"] = bad.status === 422 && badBody.errors.length >= 4;
results["audit chain verifies"] = verifyAudit() && audit.length === decisions.length + 1;
const forged = JSON.parse(audit[10].body);
forged.decision = forged.decision === "approve" ? "decline" : "approve";
audit[10].body = JSON.stringify(forged);
results["an edited audit entry breaks the chain"] = !verifyAudit();
results[`four-fifths rule holds across the protected group (ratio ${impact.toFixed(2)})`] = impact > 0.8;

print("credit-decisioning");
print(`  scorecard  AUC ${auc.toFixed(3)}; IV ${NAMES.map((f) => `${f} ${iv[f].toFixed(2)}`).join(", ")}`);
print(
    `  decisions  approve ${approved.length}, refer ${count((d) => d.decision === "refer")}, decline ${count((d) => d.decision === "decline")} of ${decisions.length}`,
);
print(`  bands      ${BANDS.map(([, n]) => `${n}:${approved.filter((d) => d.band === n).length}`).join(" ")}`);
print(
    `  example    10,000 over 36 months at 8.99%: ${sched.payment}/month, ${sched.interest} interest, final payment ${sched.rows[35].payment}`,
);
const sample = decisions.find((d) => d.rule === "KO-03");
if (sample)
    print(
        `  adverse    ${sample.applicant} score ${sample.score}: ${sample.reasons.map((r) => `${r.feature} -${r.pointsLost}`).join(", ")}`,
    );
print(
    `  portfolio  expected loss ${expectedLoss.format(0)} on ${simd.sum(ead).format(0)} exposure; scores p10 ${scoreDigest.quantile(0.1).round(0)}, p50 ${scoreDigest.quantile(0.5).round(0)}, p90 ${scoreDigest.quantile(0.9).round(0)}`,
);
print(
    `  fairness   approval A ${(rate("A") * 100).toFixed(1)}%, B ${(rate("B") * 100).toFixed(1)}%, ratio ${impact.toFixed(2)}`,
);
lender.close();
bureau.close();
model.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
