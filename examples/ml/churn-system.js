// Churn system — feature store, training with validation, a versioned model registry, online scoring and monitoring.
//
// WHAT IT IS
//   The whole loop a subscription business runs to keep customers, not just a model fit:
//
//   feature store  raw usage events and invoices in SQLite; point-in-time features computed per customer AS OF a
//                  snapshot date (so training never sees the future): tenure, logins in 7/30 days, trend, seats
//                  used, support tickets, failed payments, plan, discount
//   training       label = cancelled within 60 days of the snapshot; time-based split (train on earlier snapshots,
//                  validate on the latest); gradient-boosted trees vs a logistic baseline, picked by ROC AUC
//   calibration    the boosted scores are binned and mapped to observed rates so "0.3" really means 30%
//   registry       each model saved with its metrics, feature list and a BLAKE3 content hash; promotion only if it
//                  beats the champion; serving loads by version and verifies the hash
//   serving        POST /score scores a batch of customers from the feature store; GET /explain/:id lists the
//                  features that moved this customer's score most (one-at-a-time perturbation)
//   campaign       expected saved revenue per customer = p(churn) * uplift * 12 * MRR - offer cost (Money); the
//                  budget goes to the best expected value, not the highest risk
//                  — every candidate's value goes into one max IndexedHeap with pushMany and the offers are popped in
//                  order until the budget is spent
//   monitoring     population stability index per feature between training and this week's scoring population;
//                  bin edges are quantiles of the sorted training column and both populations are counted in one
//                  merge pass over natively sorted Float64Arrays (no per-value scan of the edges)
//   survival       Kaplan-Meier retention curves per plan from signup to cancellation (or censoring)
//   segments       KMeans on the standardised scoring population; each segment's mean risk and engagement
//
// MODULES  dyna:ml (XGBClassifier, LogisticRegression, StandardScaler, KMeans, rocAuc, logLoss) · dyna:net (SQLite)
//          · dyna:http · dyna:hash (BLAKE3) · dyna:decimal (Money) · dyna:dataframe · dyna:time · dyna:file
//          · dyna:random · dyna:structures (IndexedHeap) · dyna:simd (column means) · Array/Number extensions
//
// RUN      dynajs examples/ml/churn-system.js     simulates 4,000 customers over 14 months and runs the loop

import { XGBClassifier, LogisticRegression, StandardScaler, KMeans, rocAuc, logLoss } from "dyna:ml";
import { IndexedHeap } from "dyna:structures";
import * as simd from "dyna:simd";
import { SQLite } from "dyna:net";
import { App } from "dyna:http";
import { BLAKE3Hex } from "dyna:hash";
import { Money } from "dyna:decimal";
import { DataFrame } from "dyna:dataframe";
import { PlainDate } from "dyna:time";
import { Path, makeTempDir, makeDir, writeFile, readFile, readBytes, removeAll } from "dyna:file";
import { Random } from "dyna:random";

const work = makeTempDir("churn");
const rng = new Random(404);
const db = new SQLite(work.join("warehouse.db"));
db.exec(
    "CREATE TABLE customers (id INTEGER PRIMARY KEY, signup INTEGER, plan TEXT, seats INTEGER, mrr_cents INTEGER, discount REAL)",
);
db.exec("CREATE TABLE logins (customer INTEGER, day INTEGER, users INTEGER)");
db.exec("CREATE TABLE tickets (customer INTEGER, day INTEGER, severity INTEGER)");
db.exec("CREATE TABLE invoices (customer INTEGER, day INTEGER, failed INTEGER)");
db.exec("CREATE TABLE cancellations (customer INTEGER PRIMARY KEY, day INTEGER)");
db.exec("CREATE INDEX logins_c ON logins(customer, day)");
db.exec("CREATE INDEX tickets_c ON tickets(customer, day)");
db.exec("CREATE INDEX invoices_c ON invoices(customer, day)");

// =============================================================================
// Simulated business: engagement decays before people leave
// =============================================================================
const DAYS = 420,
    PLANS = { starter: 2900, team: 9900, business: 29900 };
db.exec("BEGIN");
for (let c = 0; c < 4000; c++) {
    const plan = ["starter", "team", "business"][rng.nextBounded(3)];
    const seats =
        plan === "starter"
            ? 1 + rng.nextBounded(3)
            : plan === "team"
              ? 3 + rng.nextBounded(15)
              : 10 + rng.nextBounded(60);
    const signup = rng.nextBounded(300),
        discount = rng.nextFloat() < 0.2 ? 0.2 : 0;
    const risk =
        0.25 +
        (plan === "starter" ? 0.35 : 0) -
        Math.min(seats, 30) * 0.008 +
        (discount ? 0.15 : 0) +
        rng.normal(0, 0.2);
    const willChurn = rng.nextFloat() < Math.max(0.03, Math.min(0.9, risk));
    const churnDay = willChurn ? signup + 60 + rng.nextBounded(DAYS - signup - 30) : Infinity;
    db.exec("INSERT INTO customers VALUES (?,?,?,?,?,?)", [
        c,
        signup,
        plan,
        seats,
        Math.round(PLANS[plan] * (1 - discount)),
        discount,
    ]);
    for (let d = signup; d < Math.min(DAYS, churnDay); d++) {
        const fade = churnDay === Infinity ? 1 : Math.max(0.05, Math.min(1, (churnDay - d) / 75));
        if (rng.nextFloat() < 0.75 * fade)
            db.exec("INSERT INTO logins VALUES (?,?,?)", [
                c,
                d,
                1 + rng.nextBounded(Math.max(1, Math.round(seats * fade))),
            ]);
        if (rng.nextFloat() < (fade < 0.6 ? 0.025 : 0.004))
            db.exec("INSERT INTO tickets VALUES (?,?,?)", [c, d, 1 + rng.nextBounded(3)]);
        if ((d - signup) % 30 === 0)
            db.exec("INSERT INTO invoices VALUES (?,?,?)", [
                c,
                d,
                rng.nextFloat() < (fade < 0.5 ? 0.25 : 0.03) ? 1 : 0,
            ]);
    }
    if (churnDay < DAYS) db.exec("INSERT INTO cancellations VALUES (?,?)", [c, churnDay]);
}
db.exec("COMMIT");

// =============================================================================
// Point-in-time features
// =============================================================================
const FEATURES = [
    "tenure",
    "logins7",
    "logins30",
    "trend",
    "seatUse",
    "tickets90",
    "sev90",
    "failed90",
    "plan",
    "seats",
    "discount",
];
function featuresAsOf(day, ids) {
    const where = ids ? `AND c.id IN (${ids.map(Number).join(",")})` : "";
    const rows = db.query(
        `
        SELECT c.id, c.signup, c.plan, c.seats, c.discount, c.mrr_cents,
          (SELECT COUNT(*) FROM logins l WHERE l.customer = c.id AND l.day >= ?1 - 7 AND l.day < ?1) AS l7,
          (SELECT COUNT(*) FROM logins l WHERE l.customer = c.id AND l.day >= ?1 - 30 AND l.day < ?1) AS l30,
          (SELECT COUNT(*) FROM logins l WHERE l.customer = c.id AND l.day >= ?1 - 60 AND l.day < ?1 - 30) AS lprev,
          (SELECT COALESCE(AVG(users), 0) FROM logins l WHERE l.customer = c.id AND l.day >= ?1 - 30 AND l.day < ?1) AS users30,
          (SELECT COUNT(*) FROM tickets t WHERE t.customer = c.id AND t.day >= ?1 - 90 AND t.day < ?1) AS t90,
          (SELECT COALESCE(MAX(severity), 0) FROM tickets t WHERE t.customer = c.id AND t.day >= ?1 - 90 AND t.day < ?1) AS s90,
          (SELECT COALESCE(SUM(failed), 0) FROM invoices i WHERE i.customer = c.id AND i.day >= ?1 - 90 AND i.day < ?1) AS f90,
          (SELECT day FROM cancellations x WHERE x.customer = c.id) AS cancelled
        FROM customers c
        WHERE c.signup <= ?1 - 30 AND (NOT EXISTS (SELECT 1 FROM cancellations x WHERE x.customer = c.id AND x.day < ?1)) ${where}
        ORDER BY c.id`,
        [day],
    );
    return rows.map((r) => ({
        id: r.id,
        mrr: r.mrr_cents,
        cancelled: r.cancelled,
        x: [
            day - r.signup,
            r.l7,
            r.l30,
            r.l30 - r.lprev,
            r.users30 / r.seats,
            r.t90,
            r.s90,
            r.f90,
            ["starter", "team", "business"].indexOf(r.plan),
            r.seats,
            r.discount,
        ],
    }));
}
const label = (row, day) => (row.cancelled !== null && row.cancelled >= day && row.cancelled < day + 60 ? 1 : 0);

const snapshots = [150, 210, 270, 330]; // monthly-ish training snapshots
const sets = snapshots.map((d) => featuresAsOf(d).map((r) => ({ ...r, y: label(r, d) })));
const trainRows = sets.slice(0, 3).flat(),
    validRows = sets[3];
const Xtr = trainRows.map((r) => r.x),
    ytr = trainRows.map((r) => r.y),
    Xva = validRows.map((r) => r.x),
    yva = validRows.map((r) => r.y);

// =============================================================================
// Training, calibration, registry
// =============================================================================
const scaler = await new StandardScaler().fit(Xtr);
const baseline = await new LogisticRegression({ maxIter: 3000, learningRate: 0.3 }).fit(
    await scaler.transform(Xtr),
    ytr,
);
const baseP = (await baseline.predictProba(await scaler.transform(Xva))).map((p) => p[1]);
const xgb = await new XGBClassifier({ nEstimators: 200, maxDepth: 4, learningRate: 0.08, subsample: 0.8, seed: 7 }).fit(
    Xtr,
    ytr,
);
const xgbRaw = (await xgb.predictProba(Xva)).map((p) => p[1]);
const aucBase = await rocAuc(yva, baseP),
    aucXgb = await rocAuc(yva, xgbRaw);

function fitCalibration(scores, ys, bins = 10) {
    // histogram binning on equal-count bins
    const order = scores.map((s, i) => [s, ys[i]]).sort((a, b) => a[0] - b[0]);
    const edges = [],
        rates = [];
    for (let b = 0; b < bins; b++) {
        const slice = order.slice(Math.floor((b * order.length) / bins), Math.floor(((b + 1) * order.length) / bins));
        edges.push(slice[slice.length - 1][0]);
        rates.push((slice.reduce((s, [, y]) => s + y, 0) + 1) / (slice.length + 2));
    }
    return { edges, rates };
}
const calibrate = (cal, s) => cal.rates[Math.min(cal.edges.findIndex((e) => s <= e) >>> 0, cal.rates.length - 1)];
const calib = fitCalibration(xgbRaw, yva);
const xgbCal = xgbRaw.map((s) => calibrate(calib, s));
const reliability = (ps, ys) => {
    const df = new DataFrame({ p: Float64Array.from(ps), y: Float64Array.from(ys) });
    return [0.1, 0.3, 0.5]
        .map((lo) => {
            const sel = df.FILTER(df.BETWEEN("p", lo, lo + 0.2));
            return sel.COUNT("p") ? [sel.MEAN("p"), sel.MEAN("y"), sel.COUNT("p")] : null;
        })
        .filter(Boolean);
};

const registryDir = work.join("registry");
const registry = { champion: null, versions: [] };
function register(name, model, metrics) {
    const bytes = model.serialize(),
        hash = BLAKE3Hex(bytes);
    const version = registry.versions.length + 1,
        path = registryDir.join(`${name}-v${version}.bin`);
    writeFile(path, bytes);
    const entry = {
        version,
        name,
        hash,
        metrics,
        features: FEATURES,
        calibration: name === "xgb" ? calib : null,
        path: path.toString(),
    };
    registry.versions.push(entry);
    const champ = registry.versions.find((v) => v.version === registry.champion);
    if (!champ || metrics.auc > champ.metrics.auc + 0.002) registry.champion = version; // promote only on a real AUC gain
    writeFile(registryDir.join("registry.json"), JSON.stringify(registry, null, 1));
    return entry;
}
makeDir(registryDir, { recursive: true });
register("logreg", baseline, { auc: aucBase, logLoss: await logLoss(yva, baseP) });
register("xgb", xgb, { auc: aucXgb, logLoss: await logLoss(yva, xgbCal) });
function loadChampion() {
    const reg = JSON.parse(readFile(registryDir.join("registry.json")));
    const entry = reg.versions.find((v) => v.version === reg.champion);
    const bytes = readBytes(new Path(entry.path));
    if (BLAKE3Hex(bytes) !== entry.hash) throw new Error(`model v${entry.version} failed its integrity check`);
    return {
        entry,
        model: entry.name === "xgb" ? XGBClassifier.deserialize(bytes) : LogisticRegression.deserialize(bytes),
    };
}

// =============================================================================
// Serving
// =============================================================================
const TODAY = 400;
const served = loadChampion();
const columnMeans = FEATURES.map((_, k) => simd.mean(Float32Array.from(trainRows, (r) => r.x[k]))); // the "typical customer", once
const scoreRows = async (rows) => {
    const X = rows.map((r) => r.x),
        raw = await served.model.predictProba(served.entry.name === "xgb" ? X : await scaler.transform(X));
    return raw.map((p) => (served.entry.calibration ? calibrate(served.entry.calibration, p[1]) : p[1]));
};
const app = new App({ port: 0 });
app.post("/score", async (req) => {
    const ids = JSON.parse(req.body).customers;
    const rows = featuresAsOf(TODAY, ids);
    const ps = await scoreRows(rows);
    return {
        model: `${served.entry.name}-v${served.entry.version}`,
        scores: rows.map((r, i) => ({ id: r.id, churn: Number(ps[i].toFixed(4)) })),
    };
});
app.get("/explain/:id", async (req) => {
    const [row] = featuresAsOf(TODAY, [req.params.id]);
    if (!row) return { status: 404, body: "unknown or inactive customer" };
    const prep = async (X) => (served.entry.name === "xgb" ? X : await scaler.transform(X));
    const base = (await served.model.predictProba(await prep([row.x])))[0][1];
    const variants = FEATURES.map((_, k) => row.x.map((v, j) => (j === k ? columnMeans[k] : v)));
    const moved = (await served.model.predictProba(await prep(variants))).map((p, k) => [FEATURES[k], base - p[1]]);
    return {
        id: row.id,
        raw: base,
        drivers: moved
            .sort((a, b) => Math.abs(b[1]) - Math.abs(a[1]))
            .slice(0, 4)
            .map(([f, d]) => ({ feature: f, effect: Number(d.toFixed(4)) })),
    };
});
app.start();
const base = `http://127.0.0.1:${app.port}`;
const population = featuresAsOf(TODAY);
const scored = [];
for (let i = 0; i < population.length; i += 500) {
    const res = await (
        await fetch(base + "/score", {
            method: "POST",
            body: JSON.stringify({ customers: population.slice(i, i + 500).map((r) => r.id) }),
        })
    ).json();
    scored.push(...res.scores);
}
const riskiest = scored.reduce((a, b) => (b.churn > a.churn ? b : a));
const explain = await (await fetch(`${base}/explain/${riskiest.id}`)).json();
const missing = await fetch(`${base}/explain/999999`);
await missing.text();
app.close();

// =============================================================================
// Campaign and monitoring
// =============================================================================
const OFFER = Money.fromString("40.00", "EUR"),
    UPLIFT = 0.3,
    BUDGET = Money.fromString("6000.00", "EUR");
const mrrOf = new Map(population.map((r) => [r.id, r.mrr]));
const evCents = Float64Array.from(scored, (s) => Math.round(s.churn * UPLIFT * 12 * mrrOf.get(s.id)) - OFFER.amount());
const queue = new IndexedHeap({ max: true }).pushMany(Uint32Array.from(scored.keys()), evCents);
const maxOffers = Math.floor(BUDGET.amount() / OFFER.amount());
const picked = [...queue.popUntil(1, maxOffers)].map((i) => ({
    id: scored[i].id,
    p: scored[i].churn,
    value: new Money(evCents[i], "EUR"),
}));
const spent = new Money(OFFER.amount() * picked.length, "EUR");
const expected = picked.reduce((m, e) => m.add(e.value), new Money(0, "EUR"));
const byRisk = [...scored].sort((a, b) => b.churn - a.churn).slice(0, picked.length);
const riskOnlyValue = byRisk.reduce(
    (m, s) => m.add(new Money(Math.round(s.churn * UPLIFT * 12 * mrrOf.get(s.id)), "EUR").sub(OFFER)),
    new Money(0, "EUR"),
);

function psi(train, now, bins = 10) {
    const a = Float64Array.from(train).sort(),
        b = Float64Array.from(now).sort();
    const edges = Float64Array.from({ length: bins - 1 }, (_, k) => a[Math.floor(((k + 1) * a.length) / bins)]);
    const hist = (xs) => {
        // one merge pass: xs and edges are both sorted
        const h = new Float64Array(bins);
        let k = 0;
        for (const x of xs) {
            while (k < bins - 1 && x >= edges[k]) k++;
            h[k]++;
        }
        return h.map((c) => Math.max(c / xs.length, 1e-4));
    };
    const ha = hist(a),
        hb = hist(b);
    return ha.reduce((s, p, k) => s + (hb[k] - p) * Math.log(hb[k] / p), 0);
}
const drift = FEATURES.map((f, k) => [
    f,
    psi(
        trainRows.map((r) => r.x[k]),
        population.map((r) => r.x[k]),
    ),
]);

// Kaplan-Meier retention per plan: duration from signup to cancellation, censored at the end of the data.
function kaplanMeier(rows) {
    const sorted = rows.sortBy((r) => r.t);
    let atRisk = sorted.length,
        surv = 1;
    const curve = [];
    for (let i = 0; i < sorted.length;) {
        let deaths = 0,
            leaving = 0;
        const t = sorted[i].t;
        while (i < sorted.length && sorted[i].t === t) {
            deaths += sorted[i].event;
            leaving++;
            i++;
        }
        if (deaths) surv *= 1 - deaths / atRisk;
        curve.push([t, surv]);
        atRisk -= leaving;
    }
    return (day) => (curve.findLast(([t]) => t <= day) || [0, 1])[1];
}
const lifetimes = db
    .query(
        "SELECT c.id, c.plan, c.signup, x.day AS gone FROM customers c LEFT JOIN cancellations x ON x.customer = c.id",
    )
    .map((r) => ({ plan: r.plan, t: (r.gone ?? DAYS) - r.signup, event: r.gone === null ? 0 : 1 }));
const survival = Object.fromEntries(
    Object.entries(lifetimes.groupBy((r) => r.plan)).map(([plan, rows]) => [plan, kaplanMeier(rows)]),
);

// Segments of today's population.
const popX = await scaler.transform(population.map((r) => r.x));
const km = await new KMeans(4, 11).fit(popX);
const seg = await km.predict(popX);
const churnOf = new Map(scored.map((s) => [s.id, s.churn]));
const segments = Object.entries(
    population.map((r, i) => ({ seg: seg[i], p: churnOf.get(r.id), logins: r.x[2] })).groupBy((r) => r.seg),
)
    .map(([k, rows]) => ({
        seg: Number(k),
        n: rows.length,
        risk: rows.pluck("p").mean(),
        logins30: rows.pluck("logins").mean(),
    }))
    .sortBy((s) => -s.risk);

// =============================================================================
// Report and checks
// =============================================================================
const results = {};
results[`boosted trees beat the baseline (AUC ${aucXgb.toFixed(3)} vs ${aucBase.toFixed(3)})`] =
    aucXgb > 0.8 && aucXgb > aucBase;
results["the registry promotes the better model and serving loads it"] =
    registry.champion === 2 && served.entry.name === "xgb";
const rel = reliability(xgbCal, yva);
results[
    `calibrated scores track observed rates (${rel.map(([p, o]) => `${p.toFixed(2)}->${o.toFixed(2)}`).join(", ")})`
] = rel.every(([p, obs, n]) => n < 30 || Math.abs(p - obs) < 0.08);
results["no snapshot contains a customer already gone (point-in-time correctness)"] = snapshots.every((d, i) =>
    sets[i].every((r) => r.cancelled === null || r.cancelled >= d),
);
results["serving scores everyone and explains one"] =
    scored.length === population.length && explain.drivers.length === 4 && missing.status === 404;
results[
    `a top-two driver for the riskiest customer is behavioural (${explain.drivers.slice(0, 2).map((d) => d.feature)})`
] = explain.drivers
    .slice(0, 2)
    .some((d) => ["logins30", "logins7", "trend", "seatUse", "failed90", "tickets90", "sev90"].includes(d.feature));
results["the heap pops offers in non-increasing expected value"] = picked.every(
    (e, i) => i === 0 || picked[i - 1].value.cmp(e.value) >= 0,
);
results["Kaplan-Meier: starter customers are retained less than business customers at 180 days"] =
    survival.starter(180) < survival.business(180) && survival.business(180) <= 1;
results["the riskiest segment is the least engaged"] = segments[0].logins30 < segments.last().logins30;
results[`value targeting (${expected.format()}) beats risk-only targeting (${riskOnlyValue.format()})`] =
    spent.cmp(BUDGET) <= 0 && expected.cmp(riskOnlyValue) >= 0;
const modelPath = new Path(registry.versions[1].path),
    tampered = readBytes(modelPath);
tampered[tampered.length - 1] ^= 1;
writeFile(modelPath, tampered);
let refused = false;
try {
    loadChampion();
} catch (e) {
    refused = /integrity/.test(e.message);
}
results["a tampered model file is refused at load"] = refused;

print("churn-system");
print(
    `  data       ${db.query("SELECT COUNT(*) AS n FROM logins")[0].n} logins, ${db.query("SELECT COUNT(*) AS n FROM cancellations")[0].n} cancellations; ${trainRows.length} training rows from ${snapshots.length - 1} snapshots`,
);
print(
    `  models     logreg AUC ${aucBase.toFixed(3)} | xgb AUC ${aucXgb.toFixed(3)} -> champion v${registry.champion} (${served.entry.hash.slice(0, 12)})`,
);
print(
    `  scoring    ${scored.length} active customers as of day ${TODAY}; riskiest #${riskiest.id} p=${riskiest.churn} driven by ${explain.drivers.map((d) => `${d.feature} ${d.effect > 0 ? "+" : ""}${d.effect}`).join(", ")}`,
);
print(
    `  campaign   ${picked.length} offers, ${spent.format()} spent, expected ${expected.format()} saved (risk-only would save ${riskOnlyValue.format()})`,
);
print(
    `  survival   at 180 days: ${Object.entries(survival)
        .map(([p, f]) => `${p} ${(f(180) * 100).round(1)}%`)
        .join(", ")}`,
);
print(
    `  segments   ${segments.map((s) => `#${s.seg} n=${s.n} risk ${s.risk.round(3)} logins30 ${s.logins30.round(1)}`).join(" | ")}`,
);
print(
    `  drift PSI  ${drift
        .sort((a, b) => b[1] - a[1])
        .slice(0, 4)
        .map(([f, v]) => `${f} ${v.toFixed(3)}`)
        .join(", ")}`,
);
for (const m of [baseline, xgb, scaler, served.model, km]) m.close();
db.close();
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
