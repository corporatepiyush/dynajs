// Metrics pipeline — a StatsD/DogStatsD UDP and Graphite TCP ingest tier, flush-interval aggregation with
// percentiles, cardinality protection with HyperLogLog, rollups, alert rules as expressions, and Prometheus /
// JSON export — the statsd + carbon + alertmanager shape.
//
// WHAT IT IS
//   ingest       UDP datagrams carrying many newline-separated StatsD lines (`api.requests:1|c|@0.1|#route:/pay`),
//                counters scaled by their sample rate, gauges (absolute and +/- deltas), timers, histograms and
//                sets; a Graphite plaintext listener on TCP (`path value timestamp`); malformed lines are counted
//                per reason, never fatal
//   aggregation  every flush interval: counter sums and per-second rates, gauge last values, timer count / mean /
//                p50 / p90 / p99 / max, set cardinality — per metric AND tag set
//   cardinality  each metric name tracks its distinct tag sets in a HyperLogLog; a name exceeding its budget
//                (a user id leaked into a tag) has new series dropped and is reported, so one bad deploy cannot
//                take the store down
//   storage      flushed points are kept at 10 s resolution for the last hour and rolled up to 1 min (sum, min,
//                max, count) beyond that
//   alerts       rules written as expressions over the latest aggregates, with `for:` durations, producing
//                pending -> firing -> resolved transitions and a grouped notification
//   export       GET /metrics (Prometheus text format) and GET /render?target=…&from=… (Graphite-style JSON)
//   sketches     a timer is a TDigest per flush, fed with the sample rate as the WEIGHT (a 10% sample counts ten
//                times, without pushing ten copies); flushed digests are kept, so GET /quantile merges any window
//                into a true p99 over that window — not an average of per-flush p99s; a set is a HyperLogLog, so a
//                set of user ids costs the same 4 KiB whether it holds ten users or ten million
//
// MODULES  dyna:net (UDPSocket, TCPServer) · dyna:structures (HyperLogLog, RingBuffer, TDigest) · dyna:mathx (Expression)
//          · dyna:http (App) · dyna:random · dyna:cli (Table) · dyna:async (sleep)
//
// RUN      dynajs examples/net/metrics-pipeline.js     replays 30 simulated minutes of traffic from 40 hosts

import { UDPSocket, TCPServer } from "dyna:net";
import { HyperLogLog, RingBuffer, TDigest } from "dyna:structures";
import { Expression } from "dyna:mathx";
import { App } from "dyna:http";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { sleep } from "dyna:async";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const rng = new Random(8125);
const FLUSH_S = 10,
    CARDINALITY_BUDGET = 200;

// =============================================================================
// Ingest and aggregation state
// =============================================================================
const bucket = { counters: new Map(), gauges: new Map(), timers: new Map(), sets: new Map() };
const gaugeLast = new Map(); // gauges persist across flushes
const seriesHll = new Map(),
    seriesSeen = new Map(),
    dropped = new Map();
const errors = new Map();
const err = (reason) => errors.set(reason, (errors.get(reason) || 0) + 1);
const stats = { lines: 0, datagrams: 0, graphite: 0 };
function seriesKey(name, tags) {
    const t = Object.keys(tags)
        .sort()
        .map((k) => `${k}=${tags[k]}`)
        .join(",");
    const key = t ? `${name}{${t}}` : name;
    if (!seriesSeen.has(name)) {
        seriesSeen.set(name, new Set());
        seriesHll.set(name, new HyperLogLog(12));
    }
    const seen = seriesSeen.get(name);
    if (!seen.has(key)) {
        seriesHll.get(name).add(key);
        if (seen.size >= CARDINALITY_BUDGET) {
            dropped.set(name, (dropped.get(name) || 0) + 1);
            return null;
        }
        seen.add(key);
    }
    return key;
}
function ingestLine(line) {
    stats.lines++;
    const m = /^([A-Za-z_][\w.\-]*):([^|]+)\|(c|g|ms|h|s)(?:\|@([\d.]+))?(?:\|#(.+))?$/.exec(line.trim());
    if (!m) return err(line.includes(":") ? "bad type or format" : "missing value");
    const [, name, raw, type, rate, tagStr] = m;
    const tags = {};
    for (const t of (tagStr || "").split(",").filter(Boolean)) {
        const i = t.indexOf(":");
        if (i < 1) return err("bad tag");
        tags[t.slice(0, i)] = t.slice(i + 1);
    }
    const key = seriesKey(name, tags);
    if (!key) return;
    const sample = rate ? Number(rate) : 1;
    if (!(sample > 0 && sample <= 1)) return err("bad sample rate");
    if (type === "s") {
        bucket.sets.getOrInsertComputed(key, () => new HyperLogLog(12)).add(raw);
        return;
    }
    const v = Number(raw);
    if (!Number.isFinite(v)) return err("not a number");
    if (type === "c") bucket.counters.set(key, (bucket.counters.get(key) || 0) + v / sample);
    else if (type === "g") gaugeLast.set(key, /^[+-]/.test(raw) ? (gaugeLast.get(key) || 0) + v : v);
    else bucket.timers.getOrInsertComputed(key, () => new TDigest(100)).add(v, 1 / sample);
}

// =============================================================================
// Storage: raw 10 s points for an hour, 1 min rollups beyond
// =============================================================================
const raw = new Map(),
    rollup = new Map();
function storePoint(key, ts, value) {
    if (!raw.has(key)) raw.set(key, new RingBuffer(360));
    raw.get(key).push([ts, value]);
    const minute = Math.floor(ts / 60) * 60;
    if (!rollup.has(key)) rollup.set(key, new Map());
    const r = rollup.get(key).get(minute) || { sum: 0, min: Infinity, max: -Infinity, count: 0 };
    r.sum += value;
    r.min = Math.min(r.min, value);
    r.max = Math.max(r.max, value);
    r.count++;
    rollup.get(key).set(minute, r);
}
const latest = new Map();
const digests = new Map(); // timer key -> RingBuffer of [ts, TDigest]
function flush(ts) {
    for (const [k, v] of bucket.counters) {
        latest.set(`${k}.count`, v);
        latest.set(`${k}.rate`, v / FLUSH_S);
        storePoint(`${k}.rate`, ts, v / FLUSH_S);
    }
    for (const [k, v] of gaugeLast) {
        latest.set(k, v);
        storePoint(k, ts, v);
    }
    for (const [k, d] of bucket.timers) {
        const [p50, p90, p99] = d.quantiles([0.5, 0.9, 0.99]);
        const agg = { count: Math.round(d.count), mean: d.mean, p50, p90, p99, max: d.max };
        for (const [s, x] of Object.entries(agg)) {
            latest.set(`${k}.${s}`, x);
            storePoint(`${k}.${s}`, ts, x);
        }
        digests.getOrInsertComputed(k, () => new RingBuffer(360)).push([ts, d]);
    }
    for (const [k, hll] of bucket.sets) {
        const n = Math.round(hll.count());
        latest.set(`${k}.unique`, n);
        storePoint(`${k}.unique`, ts, n);
    }
    bucket.counters.clear();
    bucket.timers.clear();
    bucket.sets.clear();
}

// =============================================================================
// Alert rules
// =============================================================================
const RULES = [
    {
        name: "HighErrorRate",
        expr: "errors / max(requests, 1)",
        vars: { errors: "api.errors{route=/pay}.rate", requests: "api.requests{route=/pay}.rate" },
        op: ">",
        threshold: 0.05,
        forFlushes: 3,
        severity: "page",
    },
    {
        name: "SlowCheckout",
        expr: "p99",
        vars: { p99: "checkout.latency{region=eu}.p99" },
        op: ">",
        threshold: 800,
        forFlushes: 2,
        severity: "page",
    },
    {
        name: "QueueBacklog",
        expr: "depth",
        vars: { depth: "jobs.queue_depth" },
        op: ">",
        threshold: 5000,
        forFlushes: 2,
        severity: "ticket",
    },
].map((r) => ({
    ...r,
    compiled: new Expression(r.expr.replace(/max\(([^,]+), 1\)/, "($1 + 1e-9)")),
    state: "inactive",
    streak: 0,
    history: [],
}));
function evaluate(ts) {
    const notices = [];
    for (const r of RULES) {
        const vars = Object.fromEntries(Object.entries(r.vars).map(([k, key]) => [k, latest.get(key) ?? 0]));
        const v = r.compiled.eval(vars),
            breach = r.op === ">" ? v > r.threshold : v < r.threshold;
        r.streak = breach ? r.streak + 1 : 0;
        const next = breach
            ? r.streak >= r.forFlushes
                ? "firing"
                : "pending"
            : r.state === "firing"
              ? "resolved"
              : "inactive";
        if (next !== r.state) {
            r.history.push([ts, next, Number(v.toFixed(3))]);
            if (next === "firing" || next === "resolved")
                notices.push({ rule: r.name, state: next, value: v, severity: r.severity });
        }
        r.state = next === "resolved" ? "inactive" : next;
    }
    return notices;
}

// =============================================================================
// Network listeners and the export API
// =============================================================================
const udp = new UDPSocket({ port: 0, host: "127.0.0.1" });
udp.start({
    message: (data) => {
        stats.datagrams++;
        for (const l of new TextDecoder().decode(data).split("\n")) if (l) ingestLine(l);
    },
});
const graphite = new TCPServer({ port: 0 });
graphite.start({
    data: (conn, bytes) => {
        conn.buf = (conn.buf || "") + new TextDecoder().decode(bytes);
        const lines = conn.buf.split("\n");
        conn.buf = lines.pop();
        for (const l of lines) {
            const p = l.trim().split(/\s+/);
            if (p.length !== 3 || !Number.isFinite(Number(p[1]))) {
                err("bad graphite line");
                continue;
            }
            stats.graphite++;
            const key = seriesKey(p[0], {});
            if (key) gaugeLast.set(key, Number(p[1]));
        }
    },
});
const api = new App({ port: 0 });
api.get("/metrics", () => {
    const lines = [];
    for (const [k, v] of latest) {
        const m = /^([^{]+)(?:\{([^}]*)\})?(?:\.(\w+))?$/.exec(k);
        const name = `${m[1]}${m[3] ? "_" + m[3] : ""}`.replace(/[^a-zA-Z0-9_]/g, "_");
        const labels = (m[2] || "")
            .split(",")
            .filter(Boolean)
            .map((kv) => {
                const [a, b] = kv.split("=");
                return `${a}="${b.replace(/"/g, '\\"')}"`;
            })
            .join(",");
        lines.push(`${name}${labels ? `{${labels}}` : ""} ${Number.isInteger(v) ? v : v.toFixed(4)}`);
    }
    return { body: lines.sort().join("\n") + "\n", contentType: "text/plain; version=0.0.4" };
});
api.get("/render", (req) => {
    const target = req.query.target,
        from = Number(req.query.from || 0);
    const points = (raw.get(target)?.toArray() || []).filter(([ts]) => ts >= from).map(([ts, v]) => [v, ts]);
    const minutes = [...(rollup.get(target) || [])]
        .filter(([ts]) => ts >= from)
        .map(([ts, r]) => [r.sum / r.count, ts]);
    return [{ target, datapoints: req.query.rollup === "1m" ? minutes : points }];
});
api.get("/quantile", (req) => {
    const from = Number(req.query.from || 0),
        to = Number(req.query.to || Infinity),
        q = Number(req.query.q || 0.99);
    const merged = new TDigest(100);
    let flushes = 0;
    for (const [ts, d] of digests.get(req.query.metric)?.toArray() || [])
        if (ts >= from && ts < to) {
            merged.merge(d);
            flushes++;
        }
    return { metric: req.query.metric, q, flushes, value: merged.count ? merged.quantile(q) : null };
});
api.start();

// =============================================================================
// 30 minutes of traffic from 40 hosts, compressed into a few seconds
// =============================================================================
const sendBatch = (lines) => udp.send(new TextEncoder().encode(lines.join("\n")), "127.0.0.1", udp.port);
const T0 = Math.floor(Date.UTC(2026, 6, 1, 12) / 1000);
const alerts = [];
const truthCounts = { requests: 0 };
let graphiteConn, graphiteClient;
await new Promise((res) => {
    graphiteClient = TCPServer.connect(
        { host: "127.0.0.1", port: graphite.port },
        {
            connect: (c) => {
                graphiteConn = c;
                res();
            },
        },
    );
});
for (let f = 0; f < 180; f++) {
    const ts = T0 + f * FLUSH_S,
        incident = f >= 60 && f < 75,
        slow = f >= 100 && f < 110;
    for (let host = 0; host < 40; host++) {
        const lines = [];
        for (let k = 0; k < 6; k++) {
            const sampled = rng.nextFloat() < 0.5; // half the hosts sample at 10%
            truthCounts.requests += 10;
            if (!sampled) lines.push("api.requests:10|c|#route:/pay");
            else if (rng.nextFloat() < 0.1) lines.push("api.requests:10|c|@0.1|#route:/pay");
            if (rng.nextFloat() < (incident ? 0.5 : 0.02)) lines.push(`api.errors:${incident ? 2 : 1}|c|#route:/pay`);
            lines.push(`checkout.latency:${Math.round(rng.exponential(1 / (slow ? 500 : 120)) + 30)}|ms|#region:eu`);
        }
        lines.push(`users.active:${`u${rng.nextBounded(3000)}`}|s`);
        if (host === 0) lines.push(`jobs.queue_depth:${f < 120 ? 100 + f * 10 : 6000 + f}|g`);
        if (host < 10 && f > 120)
            for (let u = 0; u < 5; u++)
                lines.push(`api.latency_by_user:${rng.nextBounded(100)}|ms|#user:${rng.nextBounded(100000)}`); // a bad deploy leaks user ids into a tag
        if (host === 2 && f % 20 === 0)
            lines.push("garbage line", "api.requests:x|c", "api.requests:1|c|@7", "api.requests:1|q");
        sendBatch(lines);
    }
    graphiteConn.write(`infra.disk.free.db1 ${80 - f * 0.1} ${ts}\ninfra.disk.free.db2 bad ${ts}\n`);
    await sleep(2);
    flush(ts);
    for (const n of evaluate(ts)) alerts.push({ ts, ...n });
}
graphiteConn.close();
graphiteClient.close();

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const metrics = await (await fetch(`http://127.0.0.1:${api.port}/metrics`)).text();
const render = await (
    await fetch(
        `http://127.0.0.1:${api.port}/render?target=${encodeURIComponent("checkout.latency{region=eu}.p99")}&from=${T0 + 900}`,
    )
).json();
const minute = await (
    await fetch(
        `http://127.0.0.1:${api.port}/render?target=${encodeURIComponent("checkout.latency{region=eu}.p99")}&rollup=1m`,
    )
).json();
const totalRate = [...raw.get("api.requests{route=/pay}.rate").toArray()].reduce((s, [, v]) => s + v * FLUSH_S, 0);
results[
    `sample-rate scaling recovers the true request count within 5% (${totalRate.toFixed(0)} vs ${truthCounts.requests})`
] = Math.abs(totalRate / truthCounts.requests - 1) < 0.05;
const fired = (name) => alerts.filter((a) => a.rule === name);
results["the error-rate alert fires after its for: duration and resolves"] =
    fired("HighErrorRate")
        .map((a) => a.state)
        .join() === "firing,resolved" && fired("HighErrorRate")[0].ts >= T0 + 62 * FLUSH_S;
results["latency and backlog alerts fire"] =
    fired("SlowCheckout")[0]?.state === "firing" && fired("QueueBacklog")[0]?.severity === "ticket";
results[
    `the user-id tag explosion is capped at ${CARDINALITY_BUDGET} series; HLL estimates ${Math.round(seriesHll.get("api.latency_by_user").count())} attempted`
] =
    dropped.get("api.latency_by_user") > 0 &&
    seriesSeen.get("api.latency_by_user").size === CARDINALITY_BUDGET &&
    seriesHll.get("api.latency_by_user").count() > CARDINALITY_BUDGET * 3;
results["malformed StatsD and Graphite lines are counted by reason"] =
    errors.get("bad type or format") >= 9 &&
    errors.get("bad sample rate") >= 9 &&
    errors.get("bad graphite line") >= 100;
results["Prometheus export includes tagged timers and Graphite gauges"] =
    /checkout_latency_p99\{region="eu"\} \d/.test(metrics) && /infra_disk_free_db1 \d/.test(metrics);
results["raw 10 s points and 1 minute rollups are both queryable"] =
    render[0].datapoints.length === 90 && minute[0].datapoints.length === 30;
const unique = latest.get("users.active.unique");
const qWindow = async (from, to) =>
    (
        await fetch(
            `http://127.0.0.1:${api.port}/quantile?metric=${encodeURIComponent("checkout.latency{region=eu}")}&q=0.99&from=${from}&to=${to}`,
        )
    ).json();
const slowWin = await qWindow(T0 + 100 * FLUSH_S, T0 + 110 * FLUSH_S),
    calmWin = await qWindow(T0, T0 + 50 * FLUSH_S);
results[
    `merged-digest p99 over the slow window (${slowWin.value.round(0)} ms) breaches 800 ms; a calm window (${calmWin.value.round(0)} ms) does not`
] = slowWin.flushes === 10 && slowWin.value > 800 && calmWin.value < 800;
const lastDigest = digests.get("checkout.latency{region=eu}").toArray().last()[1];
results["a timer digest's weighted count equals the sampled-up event count"] =
    Math.round(lastDigest.count) === latest.get("checkout.latency{region=eu}.count");
results[`set cardinality per flush (${unique} distinct users in the last interval)`] = unique > 30 && unique <= 40;

print("metrics-pipeline");
print(
    `  ingest     ${stats.datagrams} UDP datagrams, ${stats.lines} StatsD lines, ${stats.graphite} Graphite points; ${[...seriesSeen.values()].reduce((s, x) => s + x.size, 0)} series`,
);
print(
    `  rejected   ${[...errors].map(([k, v]) => `${k} ${v}`).join(", ")}; cardinality drops ${[...dropped].map(([k, v]) => `${k} ${v}`).join(", ")}`,
);
print(
    table([
        ["time", "rule", "state", "value"],
        ...alerts.map((a) => [new Date(a.ts * 1000).toISOString().slice(11, 19), a.rule, a.state, a.value.toFixed(3)]),
    ]),
);
print(
    `  windows    checkout p99 slow window ${slowWin.value.round(0)} ms over ${slowWin.flushes} flushes, calm window ${calmWin.value.round(0)} ms over ${calmWin.flushes}`,
);
print(
    `  /metrics   ${metrics.split("\n").length - 1} lines, e.g. ${metrics.split("\n").find((l) => l.startsWith("checkout_latency_p99"))}`,
);
udp.close();
graphite.close();
api.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
