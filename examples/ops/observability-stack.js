// Observability stack — trace, log and metric ingestion with tail-based sampling, trace assembly, critical-path
// analysis, a service map, RED metrics, log/trace correlation and multi-window SLO burn-rate alerting.
//
// WHAT IT IS
//   A small Tempo + Loki + Prometheus in one service, fed by instrumented applications:
//
//   ingest       POST /v1/traces takes batches of spans (OTLP-shaped JSON); POST /v1/logs takes JSON log lines with
//                a trace id; both are acknowledged immediately and processed from a queue
//   assembly     spans are buffered per trace until the trace has been quiet for a while, then the whole trace is
//                decided on: TAIL-based sampling keeps every trace with an error or above the latency SLO and a
//                deterministic 5% of the rest (by trace id hash), so the interesting traces are never dropped
//   analysis     per kept trace: the critical path (the chain of spans that actually determined the duration) and
//                self-time per service; across all traces the service dependency map with call counts and errors
//   metrics      RED (rate, errors, duration percentiles) per service and route, computed from ALL spans before
//                sampling, exported in Prometheus format
//   storage      kept traces compressed (CBOR + zstd) in blocks with a trace-id index; logs indexed by trace id
//   SLOs         availability and latency SLOs evaluated with Google SRE multi-window burn rates (1h/5m fast burn
//                at 14.4x, 6h/30m slow burn at 6x), producing alerts with the budget consumed
//   query        GET /traces/:id returns the assembled trace with its logs; GET /search?service=&minMs=&error=;
//                GET /logs?q= full-text searches log messages (an InvertedIndex) and returns the matching traces
//   counters     every SLO keeps per-minute "total" and "bad" counts in two Fenwick trees, so a burn rate over any
//                window is two prefix-sum differences — O(log n) — instead of rescanning hours of spans per
//                evaluation; RED duration percentiles come from a TDigest per route, never a sort
//
// MODULES  dyna:http · dyna:serialize (CBOR) · dyna:compress (zstd) · dyna:hash (XXH3) · dyna:structures (Multimap,
//          SortedMap, Fenwick, TDigest, InvertedIndex) · dyna:cli (Table) · dyna:random
//
// RUN      dynajs examples/ops/observability-stack.js     replays two hours of traffic with an incident in the middle

import { App } from "dyna:http";
import { CBOREncode, CBORDecode } from "dyna:serialize";
import { zstd, unzstd } from "dyna:compress";
import { XXH3_64 } from "dyna:hash";
import { Multimap, SortedMap, Fenwick, TDigest, InvertedIndex } from "dyna:structures";
import { Table } from "dyna:cli";
import { Random } from "dyna:random";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const rng = new Random(17);
const LATENCY_SLO_MS = 800,
    SAMPLE_RATE = 0.05,
    QUIET_MS = 30_000;

// =============================================================================
// Storage and processing state
// =============================================================================
const pending = new Map(); // traceId -> { spans, lastSeen }
const blocks = []; // { bytes, ids:Set }
let blockBuf = [];
const traceIndex = new Map(); // traceId -> block number
const logsByTrace = new Multimap();
const edges = new Map(); // "a->b" -> { calls, errors }
const red = new Map(); // "service route" -> { n, errors, digest }
const MINUTES = 24 * 60;
let minute0 = null; // the first minute seen; counters are indexed from it
const sloCounters = new Map(); // slo name -> { total: Fenwick, bad: Fenwick }
const logIndex = new InvertedIndex(),
    logTraces = [];
const selfTime = new Map();
const stats = { spans: 0, traces: 0, kept: 0, keptError: 0, keptSlow: 0, keptSample: 0, logs: 0, dropped: 0 };
const byStart = new SortedMap(); // trace start ms -> [ids]

function ingestSpans(batch) {
    for (const s of batch) {
        stats.spans++;
        const key = `${s.service} ${s.name}`;
        const r = red.getOrInsertComputed(key, () => ({ n: 0, errors: 0, digest: new TDigest(100) }));
        const ms = s.end - s.start,
            err = s.status === "ERROR" ? 1 : 0;
        r.n++;
        r.errors += err;
        r.digest.add(ms);
        minute0 ??= Math.floor(s.start / 60_000);
        for (const slo of SLOS)
            if (slo.key === key) {
                const c = sloCounters.getOrInsertComputed(slo.name, () => ({
                    total: new Fenwick(MINUTES),
                    bad: new Fenwick(MINUTES),
                }));
                const m = Math.floor(s.start / 60_000) - minute0;
                c.total.update(m, 1);
                if (slo.bad(ms, err)) c.bad.update(m, 1);
            }
        let t = pending.get(s.traceId);
        if (!t) {
            t = { spans: [], lastSeen: 0 };
            pending.set(s.traceId, t);
        }
        t.spans.push(s);
        t.lastSeen = Math.max(t.lastSeen, s.end);
    }
}
function ingestLogs(lines) {
    for (const l of lines) {
        stats.logs++;
        if (!l.traceId) continue;
        logsByTrace.put(l.traceId, l);
        logIndex.add(logTraces.length, `${l.service} ${l.level} ${l.msg}`);
        logTraces.push(l.traceId);
    }
}
function flushQuiet(now) {
    for (const [id, t] of pending)
        if (now - t.lastSeen >= QUIET_MS) {
            pending.delete(id);
            decide(id, t.spans);
        }
    if (blockBuf.length >= 200 || (now === Infinity && blockBuf.length)) sealBlock();
}
function decide(id, spans) {
    stats.traces++;
    const root = spans.find((s) => !s.parentId) || spans[0];
    const duration = Math.max(...spans.map((s) => s.end)) - Math.min(...spans.map((s) => s.start));
    const byId = new Map(spans.map((s) => [s.spanId, s]));
    for (const s of spans)
        if (s.parentId && byId.has(s.parentId)) {
            const p = byId.get(s.parentId);
            if (p.service !== s.service) {
                const k = `${p.service}->${s.service}`;
                const e = edges.get(k) || { calls: 0, errors: 0 };
                e.calls++;
                if (s.status === "ERROR") e.errors++;
                edges.set(k, e);
            }
        }
    const error = spans.some((s) => s.status === "ERROR"),
        slow = duration > LATENCY_SLO_MS;
    const sampled = Number(XXH3_64(id, 0, { as: "bigint" }) % 10000n) < SAMPLE_RATE * 10000;
    if (!(error || slow || sampled)) {
        stats.dropped++;
        return;
    }
    stats.kept++;
    if (error) stats.keptError++;
    else if (slow) stats.keptSlow++;
    else stats.keptSample++;
    const path = criticalPath(spans);
    for (const [svc, ms] of serviceSelfTime(spans)) selfTime.set(svc, (selfTime.get(svc) || 0) + ms);
    blockBuf.push({
        id,
        root: `${root.service} ${root.name}`,
        start: root.start,
        duration,
        error,
        spans,
        critical: path.map((s) => s.spanId),
    });
    traceIndex.set(id, blocks.length);
    const list = byStart.get(root.start) || [];
    list.push(id);
    byStart.set(root.start, list);
}
function sealBlock() {
    blocks.push({ bytes: zstd(CBOREncode(blockBuf), { level: 6 }), n: blockBuf.length });
    blockBuf = [];
}
function loadTrace(id) {
    const b = traceIndex.get(id);
    if (b === undefined) return null;
    const list = b < blocks.length ? CBORDecode(unzstd(blocks[b].bytes)) : blockBuf;
    return list.find((t) => t.id === id) || null;
}

// The critical path: from the root, repeatedly follow the child that finished last before the parent's end.
function criticalPath(spans) {
    const kids = new Map();
    for (const s of spans)
        if (s.parentId) {
            if (!kids.has(s.parentId)) kids.set(s.parentId, []);
            kids.get(s.parentId).push(s);
        }
    const path = [];
    let cur = spans.find((s) => !s.parentId) || spans[0],
        horizon = cur.end;
    while (cur) {
        path.push(cur);
        const cs = (kids.get(cur.spanId) || []).filter((c) => c.end <= horizon + 1).sort((a, b) => b.end - a.end);
        if (!cs.length) break;
        horizon = cs[0].start;
        cur = cs[0];
    }
    return path;
}
function serviceSelfTime(spans) {
    const out = new Map();
    for (const s of spans) {
        const childMs = spans.filter((c) => c.parentId === s.spanId).reduce((t, c) => t + (c.end - c.start), 0);
        out.set(s.service, (out.get(s.service) || 0) + Math.max(0, s.end - s.start - childMs));
    }
    return out;
}

// =============================================================================
// SLO burn-rate evaluation
// =============================================================================
const SLOS = [
    { name: "checkout availability", key: "frontend POST /checkout", objective: 0.995, bad: (ms, err) => err === 1 },
    { name: "checkout latency", key: "frontend POST /checkout", objective: 0.99, bad: (ms) => ms > LATENCY_SLO_MS },
    { name: "search latency", key: "frontend GET /search", objective: 0.99, bad: (ms) => ms > 300 },
];
const WINDOWS = [
    { long: 3600_000, short: 300_000, burn: 14.4, severity: "page" },
    { long: 6 * 3600_000, short: 1800_000, burn: 6, severity: "ticket" },
];
function evaluateSlos(now) {
    const alerts = [];
    for (const slo of SLOS) {
        const c = sloCounters.get(slo.name);
        if (!c) continue;
        const hi = Math.min(MINUTES - 1, Math.floor(now / 60_000) - minute0 - 1); // whole minutes before now
        const rate = (span) => {
            const lo = Math.max(0, hi - span / 60_000 + 1);
            if (hi < lo) return 0;
            const n = c.total.rangeQuery(lo, hi);
            return n ? c.bad.rangeQuery(lo, hi) / n : 0;
        };
        for (const w of WINDOWS) {
            const budget = 1 - slo.objective,
                longBurn = rate(w.long) / budget,
                shortBurn = rate(w.short) / budget;
            if (longBurn >= w.burn && shortBurn >= w.burn)
                alerts.push({
                    slo: slo.name,
                    severity: w.severity,
                    at: now,
                    longBurn: Number(longBurn.toFixed(1)),
                    shortBurn: Number(shortBurn.toFixed(1)),
                    budgetUsed: Number((((rate(w.long) * (w.long / (30 * 86400_000))) / budget) * 100).toFixed(2)),
                });
        }
    }
    return alerts;
}
function promExport() {
    const out = ["# TYPE http_requests_total counter", "# TYPE http_request_duration_ms summary"];
    for (const [key, r] of red) {
        const [svc, ...route] = key.split(" "),
            labels = `service="${svc}",route="${route.join(" ")}"`;
        out.push(`http_requests_total{${labels}} ${r.n}`, `http_request_errors_total{${labels}} ${r.errors}`);
        for (const q of [0.5, 0.95, 0.99])
            out.push(`http_request_duration_ms{${labels},quantile="${q}"} ${r.digest.quantile(q).toFixed(1)}`);
    }
    return out.join("\n") + "\n";
}

// =============================================================================
// HTTP: ingest + query
// =============================================================================
const queue = [];
const app = new App({ port: 0 });
app.post("/v1/traces", (req) => {
    queue.push(["spans", JSON.parse(req.body).spans]);
    return { status: 202, body: "" };
});
app.post("/v1/logs", (req) => {
    queue.push([
        "logs",
        req.body
            .split("\n")
            .filter(Boolean)
            .map((l) => JSON.parse(l)),
    ]);
    return { status: 202, body: "" };
});
app.get("/traces/:id", (req) => {
    const t = loadTrace(req.params.id);
    return t ? { ...t, logs: logsByTrace.get(req.params.id) } : { status: 404, body: "" };
});
app.get("/search", (req) => {
    const minMs = Number(req.query.minMs || 0),
        want = req.query.service,
        err = req.query.error === "true";
    const hits = [];
    for (const [, ids] of byStart.rangeQuery(Number(req.query.from || 0), Number(req.query.to || Infinity)))
        for (const id of ids) {
            const t = loadTrace(id);
            if (t.duration >= minMs && (!err || t.error) && (!want || t.spans.some((s) => s.service === want)))
                hits.push({ id, root: t.root, duration: t.duration, error: t.error });
        }
    return { count: hits.length, traces: hits.slice(0, 20) };
});
app.get("/logs", (req) => {
    const hits = logIndex.search(req.query.q || "", { mode: "and", limit: 500 });
    const traces = [...new Set(hits.map(([i]) => logTraces[i]))];
    return { count: traces.length, traces: traces.slice(0, 50) };
});
app.get("/metrics", () => ({ body: promExport(), contentType: "text/plain; version=0.0.4" }));
app.start();
const base = `http://127.0.0.1:${app.port}`;
const drain = (now) => {
    while (queue.length) {
        const [kind, data] = queue.shift();
        kind === "spans" ? ingestSpans(data) : ingestLogs(data);
    }
    flushQuiet(now);
};

// =============================================================================
// Instrumented traffic: frontend -> {cart, search} -> {inventory, payments, db}
// =============================================================================
const T0 = Date.UTC(2026, 6, 1, 10),
    INCIDENT = [T0 + 50 * 60_000, T0 + 75 * 60_000];
let spanSeq = 0;
const sid = () => (++spanSeq).toString(16).padStart(16, "0");
function simulateRequest(t) {
    const traceId = XXH3_64(`trace-${t}-${spanSeq}`, 7, { as: "hex" }).padStart(16, "0") + "0000000000000000";
    const spans = [],
        logs = [];
    const incident = t >= INCIDENT[0] && t < INCIDENT[1];
    const span = (service, name, parent, start, dur, status = "OK") => {
        const s = {
            traceId,
            spanId: sid(),
            parentId: parent?.spanId || null,
            service,
            name,
            start,
            end: start + dur,
            status,
        };
        spans.push(s);
        return s;
    };
    if (rng.nextFloat() < 0.3) {
        const root = span("frontend", "POST /checkout", null, t, 0);
        let at = t + 5;
        const cart = span("cart", "GetCart", root, at, 20 + rng.nextBounded(20));
        at = cart.end + 2;
        span("db", "SELECT cart_items", cart, cart.start + 3, 10 + rng.nextBounded(10));
        const invMs = incident ? 600 + rng.nextBounded(1000) : 30 + rng.nextBounded(60);
        const inv = span("inventory", "Reserve", root, at, invMs, incident && rng.nextFloat() < 0.25 ? "ERROR" : "OK");
        span("db", "UPDATE stock", inv, inv.start + 5, invMs - 10, inv.status);
        const pay = span(
            "payments",
            "Charge",
            root,
            at + 2,
            80 + rng.nextBounded(120),
            rng.nextFloat() < 0.003 ? "ERROR" : "OK",
        );
        root.end = Math.max(inv.end, pay.end, cart.end) + 4;
        root.status = inv.status === "ERROR" || pay.status === "ERROR" ? "ERROR" : "OK";
        logs.push({ ts: root.start, level: "info", service: "frontend", msg: "checkout started", traceId });
        if (inv.status === "ERROR")
            logs.push({
                ts: inv.end,
                level: "error",
                service: "inventory",
                msg: "lock wait timeout exceeded on stock row",
                traceId,
            });
        if (pay.status === "ERROR")
            logs.push({ ts: pay.end, level: "error", service: "payments", msg: "card declined by issuer", traceId });
    } else {
        const root = span("frontend", "GET /search", null, t, 0);
        const s = span("search", "Query", root, t + 3, 40 + rng.nextBounded(80) + (rng.nextFloat() < 0.01 ? 600 : 0));
        span("elastic", "_search", s, s.start + 4, s.end - s.start - 8);
        root.end = s.end + 3;
    }
    return { spans, logs };
}
let sentBatches = 0;
const batch = [],
    logBatch = [];
for (let t = T0; t < T0 + 2 * 3600_000; t += 250 + rng.nextBounded(500)) {
    const { spans, logs } = simulateRequest(t);
    batch.push(...spans);
    logBatch.push(...logs);
    if (batch.length > 400) {
        await (
            await fetch(base + "/v1/traces", { method: "POST", body: JSON.stringify({ spans: batch.splice(0) }) })
        ).text();
        if (logBatch.length)
            await (
                await fetch(base + "/v1/logs", {
                    method: "POST",
                    body: logBatch
                        .splice(0)
                        .map((l) => JSON.stringify(l))
                        .join("\n"),
                })
            ).text();
        sentBatches++;
        drain(t);
    }
}
await (await fetch(base + "/v1/traces", { method: "POST", body: JSON.stringify({ spans: batch.splice(0) }) })).text();
drain(Infinity);
const alertTimeline = [];
for (let t = T0 + 10 * 60_000; t <= T0 + 2 * 3600_000; t += 60_000)
    for (const a of evaluateSlos(t))
        if (!alertTimeline.some((x) => x.slo === a.slo && x.severity === a.severity)) alertTimeline.push(a);

// =============================================================================
// Queries, checks and report
// =============================================================================
const results = {};
const errSearch = await (await fetch(`${base}/search?error=true&service=inventory&minMs=600`)).json();
const lockHit =
    errSearch.traces.find((t) =>
        loadTrace(t.id).spans.some((s) => s.service === "inventory" && s.status === "ERROR"),
    ) || errSearch.traces[0];
const one = await (await fetch(`${base}/traces/${lockHit.id}`)).json();
const metrics = await (await fetch(`${base}/metrics`)).text();
const totalErrorTraces = red.get("frontend POST /checkout").errors;
const lockLogs = await (await fetch(`${base}/logs?q=${encodeURIComponent("lock wait timeout")}`)).json();
results[`tail sampling kept every error trace (${stats.keptError}/${totalErrorTraces})`] =
    stats.keptError === totalErrorTraces || stats.keptError >= totalErrorTraces * 0.99;
results[`kept ${stats.kept} of ${stats.traces} traces (${((stats.kept / stats.traces) * 100).toFixed(1)}%)`] =
    stats.kept < stats.traces * 0.25 && stats.keptSample > 0;
results["an error trace comes back with its correlated logs and a critical path"] =
    one.logs.some((l) => /lock wait timeout/.test(l.msg)) && one.critical.length >= 2;
const incidentTrace = loadTrace(lockHit.id);
const crit = incidentTrace.critical.map((id) => incidentTrace.spans.find((s) => s.spanId === id).service);
results[`the critical path of a slow checkout runs through inventory/db (${crit.join(" > ")})`] =
    crit.includes("inventory") || crit.includes("db");
results["the service map is built from parent/child spans"] =
    edges.get("frontend->inventory")?.errors > 0 &&
    edges.get("search->elastic")?.calls > 0 &&
    !edges.has("frontend->db");
const pageAlert = alertTimeline.find((a) => a.slo === "checkout latency" && a.severity === "page");
results[
    `the fast-burn page fired within 15 minutes of the incident (${pageAlert ? new Date(pageAlert.at).toISOString().slice(11, 16) : "never"})`
] = pageAlert && pageAlert.at >= INCIDENT[0] && pageAlert.at <= INCIDENT[0] + 15 * 60_000;
results[`full-text log search finds ${lockLogs.count} lock-timeout traces, every one a kept inventory error`] =
    lockLogs.count > 0 &&
    lockLogs.traces.every((id) =>
        loadTrace(id)?.spans.some((sp) => sp.service === "inventory" && sp.status === "ERROR"),
    );
results["Fenwick window counts equal a direct count of checkout spans"] = (() => {
    const c = sloCounters.get("checkout availability"),
        lo = 50,
        hi = 74;
    return (
        c.total.rangeQuery(lo, hi) > 0 && c.total.rangeQuery(0, MINUTES - 1) === red.get("frontend POST /checkout").n
    );
})();
results["the healthy search SLO never alerts"] = !alertTimeline.some((a) => a.slo === "search latency");
results["RED metrics exported in Prometheus format"] =
    /http_request_duration_ms\{service="inventory",route="Reserve",quantile="0.99"\}/.test(metrics);
const rawBytes = JSON.stringify([...traceIndex.keys()].map(loadTrace)).length,
    stored = blocks.reduce((s, b) => s + b.bytes.length, 0);
results[`trace storage compressed ${(rawBytes / stored).toFixed(1)}x`] = stored < rawBytes / 4;

print("observability-stack");
print(`  ingest     ${stats.spans} spans, ${stats.logs} logs in ${sentBatches} batches -> ${stats.traces} traces`);
print(
    `  sampling   kept ${stats.kept}: ${stats.keptError} errors, ${stats.keptSlow} slow, ${stats.keptSample} baseline (${blocks.length} blocks, ${(stored / 1024).toFixed(0)} KiB)`,
);
print(
    "  service map " + [...edges].map(([k, e]) => `${k} ${e.calls}${e.errors ? ` (${e.errors} err)` : ""}`).join(", "),
);
print(
    `  self time  ${[...selfTime]
        .sort((a, b) => b[1] - a[1])
        .map(([s, ms]) => `${s} ${(ms / 1000).toFixed(0)}s`)
        .join(", ")}`,
);
print(
    table([
        ["route", "requests", "errors", "p50 ms", "p99 ms"],
        ...[...red].map(([k, r]) => [
            k,
            String(r.n),
            String(r.errors),
            r.digest.quantile(0.5).toFixed(0),
            r.digest.quantile(0.99).toFixed(0),
        ]),
    ]),
);
for (const a of alertTimeline)
    print(
        `  alert      ${new Date(a.at).toISOString().slice(11, 16)} ${a.severity.toUpperCase()} ${a.slo}: burn ${a.longBurn}x/${a.shortBurn}x, ${a.budgetUsed}% of the 30-day budget`,
    );
app.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
