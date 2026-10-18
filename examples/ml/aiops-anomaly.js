// AIOps — anomaly detection over service telemetry, alert correlation into incidents, root-cause ranking, paging.
//
// WHAT IT IS
//   What sits between a metrics store and the on-call engineer's phone, so one database outage is one page and
//   not forty:
//
//   telemetry    per service, per minute: latency p99, error rate, throughput, saturation — a day of history plus
//                the live window, for a 9-service dependency graph (gateway -> api -> {orders, search} -> db …)
//   detection    per metric: a weekday-hour seasonal baseline, residuals scored by a robust z (median/MAD), and a
//                multivariate check per service — a Gaussian mixture fitted on normal minutes flags joint states
//                that are individually plausible but together unusual
//   logs         raw log lines are mined into templates (numbers, ids, IPs masked) and template counts per minute
//                feed the same detector, so "new error template appears" is a signal too
//   correlation  anomalies within 10 minutes that are connected in the dependency graph become one incident
//   root cause   each anomalous service is ranked by how many anomalous dependants it explains and how early it
//                went bad; a service whose own dependencies are healthy and that is upstream of the most damage wins
//   paging       an incident is posted once to a paging webhook (HMAC-signed); suppressed during a maintenance
//                window defined as a recurrence rule; re-notified only if it grows
//
//   indexes      the dependency closure is computed once as one BitSet per service (BFS over the Graph), so "is X
//                upstream of Y" is a bit test, not a traversal per anomaly; log templates are interned to ids and
//                counted per minute in a Multiset with the noisiest kept by a TopK; maintenance windows live in an
//                IntervalTree; the seasonal baseline gathers its 30 samples into a Float64Array for stats.median
//
// MODULES  dyna:ml (GaussianMixture, StandardScaler) · dyna:structures (Graph, RingBuffer, BitSet, Interner, Multiset,
//          TopK, IntervalTree) · dyna:dataframe · dyna:mathx (stats) · dyna:time (RRule) · dyna:http · dyna:crypto
//          (HMAC) · dyna:matcher · dyna:random
//
// RUN      dynajs examples/ml/aiops-anomaly.js     replays a day of telemetry with a database incident and a maintenance window

import { GaussianMixture, StandardScaler } from "dyna:ml";
import { Graph, RingBuffer, BitSet, Interner, Multiset, TopK, IntervalTree } from "dyna:structures";
import { DataFrame } from "dyna:dataframe";
import { stats } from "dyna:mathx";
import { RRule } from "dyna:time";
import { App } from "dyna:http";
import { HMACHex, TimingSafeEqual } from "dyna:crypto";
import { MultiMatcher } from "dyna:matcher";
import { Random } from "dyna:random";

const rng = new Random(8);

// =============================================================================
// Topology
// =============================================================================
const SERVICES = ["gateway", "api", "orders", "search", "payments", "inventory", "db", "cache", "es"];
const DEPENDS = {
    gateway: ["api"],
    api: ["orders", "search", "payments"],
    orders: ["db", "inventory", "cache"],
    search: ["es", "cache"],
    payments: ["db"],
    inventory: ["db"],
    db: [],
    cache: [],
    es: [],
};
const graph = new Graph({ directed: true });
const idx = new Map(SERVICES.map((s) => [s, graph.addNode()]));
for (const [s, deps] of Object.entries(DEPENDS)) for (const d of deps) graph.addEdge(idx.get(s), idx.get(d));
const closure = SERVICES.map((s) => {
    const b = new BitSet(SERVICES.length);
    for (const n of graph.bfs(idx.get(s))) b.set(n);
    return b;
});
const dependsOn = (from, to) => closure[idx.get(from)].get(idx.get(to)); // from -> ... -> to
const dependants = (svc) => SERVICES.filter((s) => s !== svc && dependsOn(s, svc));

// =============================================================================
// Telemetry simulation: a day of history and a live day with incidents
// =============================================================================
const MIN = 60_000,
    T0 = Date.UTC(2026, 3, 13),
    HIST = 7 * 1440,
    LIVE = 1440;
const METRICS = ["p99", "errors", "rps", "saturation"];
const baseRps = Object.fromEntries(SERVICES.map((s, i) => [s, 200 + i * 40]));
const incidents = [
    { kind: "db", start: HIST + 600, end: HIST + 650 }, // db saturation cascades upwards
    { kind: "es", start: HIST + 900, end: HIST + 915 }, // search index slowdown, contained
    { kind: "deploy", start: HIST + 1300, end: HIST + 1330 }, // during the maintenance window
];
const telemetry = Object.fromEntries(
    SERVICES.map((s) => [s, Object.fromEntries(METRICS.map((m) => [m, new Float64Array(HIST + LIVE)]))]),
);
const logs = [];
function impact(svc, t) {
    let f = { lat: 1, err: 0, sat: 0 };
    for (const inc of incidents) {
        if (t < inc.start || t >= inc.end) continue;
        if (inc.kind === "db" && dependsOn(svc, "db"))
            f = svc === "db" ? { lat: 4, err: 0.02, sat: 0.45 } : { lat: 2.5, err: 0.06, sat: 0.1 };
        if (inc.kind === "es" && (svc === "es" || svc === "search"))
            f = { lat: 3, err: 0.01, sat: svc === "es" ? 0.3 : 0.05 };
        if (inc.kind === "deploy" && svc === "api") f = { lat: 2, err: 0.08, sat: 0.2 };
    }
    return f;
}
for (let t = 0; t < HIST + LIVE; t++) {
    const hour = ((t / 60) | 0) % 24,
        diurnal = 0.55 + 0.45 * Math.sin((Math.PI * (hour - 6)) / 12) ** 2;
    for (const s of SERVICES) {
        const f = impact(s, t),
            rps = baseRps[s] * diurnal * (1 + rng.normal(0, 0.05));
        const T = telemetry[s];
        T.rps[t] = rps;
        T.p99[t] = (40 + 30 * diurnal) * f.lat * (1 + rng.normal(0, 0.08));
        T.errors[t] = Math.max(0, 0.002 + f.err + rng.normal(0, 0.001));
        T.saturation[t] = Math.min(1, 0.3 + 0.3 * diurnal + f.sat + rng.normal(0, 0.03));
        if (t >= HIST) {
            for (let k = 0, n = rng.poisson(2); k < n; k++)
                logs.push([t, s, `request ${rng.nextBounded(1e6)} served in ${(T.p99[t] / 3).toFixed(0)}ms`]);
            if (f.err > 0.03 && rng.nextFloat() < 0.7)
                logs.push([
                    t,
                    s,
                    `upstream timeout after 3000ms calling db-${rng.nextBounded(3)}.internal:5432 req=${rng.nextBounded(1e9).toString(16)}`,
                ]);
            if (s === "db" && f.sat > 0.3)
                logs.push([
                    t,
                    s,
                    `too many connections (max 200) from 10.0.${rng.nextBounded(4)}.${rng.nextBounded(250)}`,
                ]);
        }
    }
}

// =============================================================================
// Log template mining
// =============================================================================
const MASKS = [
    [/\b\d+\.\d+\.\d+\.\d+\b/g, "<ip>"],
    [/\b[0-9a-f]{6,}\b/g, "<hex>"],
    [/\b\d+(ms|s)?\b/g, "<n>$1"],
    [/db-<n>\.internal/g, "<host>"],
];
const template = (line) => MASKS.reduce((s, [re, r]) => s.replace(re, r), line);
const templates = new Interner();
const perMinute = new Multiset(); // "templateId@minute" -> count
const noisiest = new TopK(5);
for (const [t, s, line] of logs) {
    const id = templates.intern(`${s}: ${template(line)}`);
    perMinute.add(`${id}@${t}`);
    noisiest.add(String(id));
}
const ERROR_WORDS = new MultiMatcher(["timeout", "too many", "refused", "panic", "fatal"]);
const templateCount = templates.size;
const errorTemplates = templates
    .getMany(Uint32Array.from({ length: templateCount }, (_, i) => i))
    .filter((k) => ERROR_WORDS.test(k));
const minutesOf = (key) => {
    const id = templates.lookup(key),
        out = [];
    for (const [k, n] of perMinute.entrySet()) {
        const [tid, t] = k.split("@");
        if (Number(tid) === id) out.push([Number(t), n]);
    }
    return out;
};

// =============================================================================
// Detection
// =============================================================================
const anomalies = []; // { t, svc, metric, z }
const baseBuf = new Float64Array(30),
    devBuf = new Float64Array(30);
function seasonalBaseline(series, t) {
    // same minute-of-day over past days
    let n = 0;
    for (let d = 1; d <= 6; d++) {
        const u = t - d * 1440;
        if (u >= 2) for (let k = -2; k <= 2; k++) baseBuf[n++] = series[u + k];
    }
    return baseBuf.subarray(0, n);
}
const ROBUST = 1.4826;
for (const s of SERVICES)
    for (const m of METRICS) {
        const series = telemetry[s][m];
        const recent = new RingBuffer(5);
        for (let t = HIST; t < HIST + LIVE; t++) {
            const base = seasonalBaseline(series, t),
                med = stats.median(base);
            for (let k = 0; k < base.length; k++) devBuf[k] = Math.abs(base[k] - med);
            const mad = stats.median(devBuf.subarray(0, base.length)) * ROBUST || 1e-9;
            const z = (series[t] - med) / mad;
            recent.push(m === "rps" ? 0 : z); // throughput dips are not paged
            const window = recent.toArray();
            if (window.length === 5 && window.filter((v) => v > 6).length >= 3)
                anomalies.push({ t, svc: s, metric: m, z });
        }
    }
const multivariate = {};
for (const s of SERVICES) {
    const rowsOf = (from, to) =>
        Array.from({ length: to - from }, (_, k) => METRICS.map((m) => telemetry[s][m][from + k]));
    const scaler = await new StandardScaler().fit(rowsOf(HIST - 2 * 1440, HIST));
    const gmm = await new GaussianMixture(3, { seed: 1, maxIter: 200 }).fit(
        await scaler.transform(rowsOf(HIST - 2 * 1440, HIST)),
    );
    const live = await scaler.transform(rowsOf(HIST, HIST + LIVE));
    const proba = await gmm.predictProba(live);
    const lp = live.map((x, i) => {
        // log density under the mixture
        let dens = 0;
        for (let k = 0; k < gmm.weights.length; k++) {
            let q = 0,
                det = 0;
            for (let j = 0; j < x.length; j++) {
                const v = gmm.variances[k][j];
                q += (x[j] - gmm.means[k][j]) ** 2 / v;
                det += Math.log(v);
            }
            dens += gmm.weights[k] * Math.exp(-0.5 * q - 0.5 * det);
        }
        return Math.log(dens + 1e-300);
    });
    const cut = stats.quantile(lp.slice(0, 300), 0.001) - 8;
    multivariate[s] = lp.map((v, i) => [HIST + i, v]).filter(([, v]) => v < cut).length;
    lp.forEach((v, i) => {
        if (v < cut && !anomalies.some((a) => a.svc === s && Math.abs(a.t - (HIST + i)) <= 2))
            anomalies.push({ t: HIST + i, svc: s, metric: "joint", z: -v });
    });
    void proba;
    scaler.close();
    gmm.close();
}
for (const key of errorTemplates) {
    const svc = key.split(":")[0];
    for (const [t, n] of minutesOf(key)) if (n >= 2) anomalies.push({ t, svc, metric: "log", z: n, template: key });
}
anomalies.sort((a, b) => a.t - b.t);

// =============================================================================
// Correlation and root cause
// =============================================================================
const WINDOW = 10;
const groups = [];
for (const a of anomalies) {
    const g = groups.find(
        (x) =>
            a.t - x.last <= WINDOW &&
            [...x.services].some((s) => s === a.svc || dependsOn(s, a.svc) || dependsOn(a.svc, s)),
    );
    if (g) {
        g.items.push(a);
        g.services.add(a.svc);
        g.last = a.t;
    } else groups.push({ first: a.t, last: a.t, items: [a], services: new Set([a.svc]) });
}
function rootCause(g) {
    const first = new Map();
    for (const a of g.items) if (!first.has(a.svc)) first.set(a.svc, a.t);
    return [...g.services]
        .map((s) => {
            const explains = dependants(s).filter((d) => g.services.has(d)).length;
            const sickDeps = [...g.services].count((d) => d !== s && dependsOn(s, d));
            return { svc: s, score: explains * 2 - sickDeps * 3 - (first.get(s) - g.first) * 0.1 };
        })
        .sort((a, b) => b.score - a.score);
}
const incidentsFound = groups.filter((g) => g.items.length >= 5).map((g) => ({ ...g, ranking: rootCause(g) }));

// =============================================================================
// Paging with maintenance windows
// =============================================================================
const maintenance = new RRule({ freq: "DAILY", dtstart: new Date(T0 + (21 * 60 + 30) * MIN) });
const windows = maintenance
    .between(new Date(T0), new Date(T0 + (HIST + LIVE + 1440) * MIN), true)
    .map((d) => d.getTime());
const windowTree = new IntervalTree();
for (const w of windows) windowTree.insert(w, w + 45 * MIN - 1, new Date(w).toISOString());
const inMaintenance = (t) => windowTree.at(T0 + t * MIN).length > 0;
const secret = "whsec-aiops-demo";
const pages = [];
const pager = new App({ port: 0 });
pager.post("/v2/enqueue", (req) => {
    const sig = req.headers["x-signature"] || "";
    const want = HMACHex("sha256", secret, req.body);
    if (sig.length !== want.length || !TimingSafeEqual(new TextEncoder().encode(sig), new TextEncoder().encode(want)))
        return { status: 401, body: "bad signature" };
    pages.push(JSON.parse(req.body));
    return { status: 202, body: JSON.stringify({ status: "success" }), contentType: "application/json" };
});
pager.start();
const notified = new Map();
let suppressed = 0;
for (const inc of incidentsFound) {
    const key = `${inc.ranking[0].svc}@${inc.first}`;
    if (inMaintenance(inc.first)) {
        suppressed++;
        continue;
    }
    if (notified.has(key) && notified.get(key) >= inc.services.size) continue;
    const body = JSON.stringify({
        routing_key: "svc-platform",
        event_action: "trigger",
        dedup_key: key,
        payload: {
            summary: `${inc.ranking[0].svc} degraded; ${inc.services.size} services affected`,
            severity: inc.services.size > 3 ? "critical" : "error",
            source: inc.ranking[0].svc,
            timestamp: new Date(T0 + inc.first * MIN).toISOString(),
            custom_details: {
                suspects: inc.ranking.slice(0, 3).map((r) => r.svc),
                signals: inc.items.length,
                templates: [...new Set(inc.items.filter((a) => a.template).map((a) => a.template))].slice(0, 3),
            },
        },
    });
    const r = await fetch(`http://127.0.0.1:${pager.port}/v2/enqueue`, {
        method: "POST",
        body,
        headers: { "x-signature": HMACHex("sha256", secret, body) },
    });
    await r.text();
    notified.set(key, inc.services.size);
}
const forged = await fetch(`http://127.0.0.1:${pager.port}/v2/enqueue`, {
    method: "POST",
    body: "{}",
    headers: { "x-signature": "00" },
});
await forged.text();
pager.close();

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const hit = (inc) => incidentsFound.find((g) => g.first >= inc.start - 3 && g.first <= inc.start + 10);
const dbInc = hit(incidents[0]),
    esInc = hit(incidents[1]),
    depInc = hit(incidents[2]);
results[`db incident attributed to db (${dbInc?.ranking.slice(0, 3).map((r) => r.svc)})`] =
    dbInc && dbInc.ranking[0].svc === "db";
results["the db incident groups its cascading dependants"] = dbInc && dbInc.services.size >= 4;
results["the search incident stays separate and points at es/search"] =
    esInc && ["es", "search"].includes(esInc.ranking[0].svc) && !esInc.services.has("db");
results["an incident in the maintenance window is suppressed"] =
    depInc &&
    suppressed >= 1 &&
    !pages.some((p) => p.payload.timestamp === new Date(T0 + depInc.first * MIN).toISOString());
results[`${pages.length} pages for ${anomalies.length} anomalous signals`] =
    pages.length === incidentsFound.length - suppressed && pages.length <= 4;
results["the pager refuses unsigned events"] = forged.status === 401;
results["the BitSet closure agrees with a fresh BFS for every pair"] = SERVICES.all((a) =>
    SERVICES.all((b) => dependsOn(a, b) === graph.bfs(idx.get(a)).includes(idx.get(b))),
);
results["the interval tree agrees with a linear scan of the maintenance windows"] = [
    0,
    600,
    HIST + 1290,
    HIST + 1300,
    HIST + 1335,
    HIST + 1350,
].all((t) => inMaintenance(t) === windows.some((w) => T0 + t * MIN >= w && T0 + t * MIN - w < 45 * MIN));
results["the noisiest template is the ordinary request log"] = templates
    .get(Number(noisiest.list()[0][0]))
    .includes("served in");
results["log lines collapse into templates"] = errorTemplates.some((t) =>
    t.includes("upstream timeout after <n>ms calling <host>:<n> req=<hex>"),
);
const quietMinutes = anomalies.filter((a) => !incidents.some((i) => a.t >= i.start - 2 && a.t < i.end + 10)).length;
results[`few false-positive signals outside incidents (${quietMinutes})`] = quietMinutes < 15;
const df = new DataFrame({
    z: Float64Array.from(anomalies.filter((a) => a.metric !== "log").map((a) => Math.min(a.z, 1e3))),
});

print("aiops-anomaly");
print(
    `  telemetry  ${SERVICES.length} services x ${METRICS.length} metrics x ${HIST + LIVE} minutes, ${logs.length} log lines -> ${templateCount} templates`,
);
print(
    `  templates  ${noisiest
        .list()
        .take(3)
        .map(([id, n]) => `${n}x ${templates.get(Number(id)).truncate(60)}`)
        .join(" | ")}`,
);
print(
    `  detection  ${anomalies.length} anomalous signals (median |z| ${df.MEDIAN("z").toFixed(1)}), multivariate ${Object.entries(
        multivariate,
    )
        .filter(([, n]) => n)
        .map(([s, n]) => `${s}:${n}`)
        .join(" ")}`,
);
for (const inc of incidentsFound) {
    const at = new Date(T0 + inc.first * MIN).toISOString().slice(11, 16);
    print(
        `  incident   ${at} ${inc.services.size} services, ${inc.items.length} signals, root cause ${inc.ranking[0].svc} (then ${inc.ranking
            .slice(1, 3)
            .map((r) => r.svc)
            .join(", ")})${inMaintenance(inc.first) ? " [maintenance]" : ""}`,
    );
}
print(`  paging     ${pages.length} sent, ${suppressed} suppressed by the maintenance window`);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
