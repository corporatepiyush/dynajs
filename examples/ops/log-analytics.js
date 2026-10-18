// Log analytics — a day of access and application logs parsed, mined into templates, rolled up per minute, scanned
// for anomalies and narrowed to a root cause, with every sketch checked against an exact answer.
//
// WHAT IT IS
//   What an on-call engineer does with 200 000 log lines and a page at 14:07:
//
//   parse       access lines by one regex; application lines are mined into templates (numbers, hex ids, IPs and
//               durations become <*>), interned to ids, with a RoaringBitmap of line numbers per template
//   rollups     per-minute request counts, 5xx counts and a TDigest of latency per minute and per route;
//               distinct client IPs in a HyperLogLog; error signatures counted by one MultiMatcher pass
//   detection   5xx rate per minute against a rolling median / MAD baseline held in a RingBuffer; anomalous
//               minutes are merged into incident windows by a RangeSet
//   root cause  for each upstream's 5xx lines and each template, lift = P(x | incident) / P(x | normal), computed as bitmap
//               intersection cardinalities; the planted cause (one upstream, one template) must rank first
//   archive     the day's lines zstd-compressed in 10-minute blocks with a SortedMap time index, so a query for
//               the incident window decompresses only the blocks it overlaps
//
// MODULES  dyna:structures (Interner, RoaringBitmap, TDigest, HyperLogLog, RingBuffer, RangeSet, SortedMap, TopK,
//          Multiset) · dyna:matcher (MultiMatcher) · dyna:compress (zstd) · dyna:random · dyna:cli ·
//          String/Array/Date/Number extensions
//
// RUN      dynajs examples/ops/log-analytics.js

import {
    Interner,
    RoaringBitmap,
    TDigest,
    HyperLogLog,
    RingBuffer,
    RangeSet,
    SortedMap,
    TopK,
    Multiset,
} from "dyna:structures";
import { MultiMatcher } from "dyna:matcher";
import { zstd, unzstd } from "dyna:compress";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1407);
const DAY = Date.UTC(2026, 9, 24);
const INCIDENT = [14 * 60 + 0, 14 * 60 + 20];

// =============================================================================
// Generate a day: access lines plus application lines, with an incident
// =============================================================================
const ROUTES = ["/api/cart", "/api/search", "/api/checkout", "/api/user", "/static/app.js", "/api/recommend"];
const UPSTREAMS = ["cart-1", "cart-2", "search-1", "search-2", "pay-1", "pay-2"];
const APP_TEMPLATES = [
    (r) => `cache miss for key session:${r.nextU53().toString(16).slice(0, 8)} after ${r.nextBounded(90)}ms`,
    (r) => `user ${r.nextBounded(90000)} added sku ${r.nextBounded(5000)} qty ${1 + r.nextBounded(4)}`,
    (r) => `gc pause ${(r.nextFloat() * 20).toFixed(1)}ms heap ${200 + r.nextBounded(800)}MB`,
    (r) =>
        `request ${r.nextU53().toString(16).slice(0, 12)} served by ${UPSTREAMS[r.nextBounded(6)]} in ${r.nextBounded(300)}ms`,
];
const CAUSE_TEMPLATE = (r) =>
    `connection pool exhausted on pay-2 waited ${2000 + r.nextBounded(3000)}ms for slot ${r.nextBounded(64)}`;
const lines = [],
    truth = { requests: 0, s5xx: 0, ips: new Set(), latency: [] };
for (let minute = 0; minute < 1440; minute++) {
    const inIncident = minute >= INCIDENT[0] && minute < INCIDENT[1];
    const load = Math.round(90 + 60 * Math.sin(((minute - 360) / 1440) * 2 * Math.PI));
    for (let i = 0; i < load; i++) {
        const ts = DAY + minute * 60_000 + rng.nextBounded(60_000);
        const route = ROUTES[rng.nextBounded(ROUTES.length)];
        let upstream = UPSTREAMS[rng.nextBounded(UPSTREAMS.length)];
        if (route === "/api/checkout") upstream = rng.nextFloat() < 0.5 ? "pay-1" : "pay-2";
        const broken = inIncident && upstream === "pay-2";
        const status = broken ? (rng.nextFloat() < 0.7 ? 503 : 200) : rng.nextFloat() < 0.004 ? 500 : 200;
        const ms = broken ? 2000 + rng.nextBounded(3000) : Math.round(rng.exponential(1 / 40)) + 2;
        const ip = `203.0.${rng.nextBounded(40)}.${rng.nextBounded(250)}`;
        lines.push({
            ts,
            text: `${ip} - - [${new Date(ts).toISOString()}] "GET ${route} HTTP/1.1" ${status} ${200 + rng.nextBounded(5000)} rt=${(ms / 1000).toFixed(3)} upstream=${upstream}`,
        });
        truth.requests++;
        if (status >= 500) truth.s5xx++;
        truth.ips.add(ip);
        truth.latency.push(ms);
        if (rng.nextFloat() < 0.5)
            lines.push({
                ts,
                text: `${new Date(ts).toISOString()} INFO app ${APP_TEMPLATES[rng.nextBounded(4)](rng)}`,
            });
        if (broken && rng.nextFloat() < 0.6)
            lines.push({ ts, text: `${new Date(ts).toISOString()} ERROR app ${CAUSE_TEMPLATE(rng)}` });
    }
}
lines.sort((a, b) => a.ts - b.ts);

// =============================================================================
// Parse and roll up
// =============================================================================
const ACCESS = /^(\S+) - - \[([^\]]+)\] "(\w+) (\S+) [^"]+" (\d{3}) (\d+) rt=([\d.]+) upstream=(\S+)$/;
const templateOf = (msg) => msg.replace(/\b[0-9a-f]{8,}\b/g, "<*>").replace(/\d+(\.\d+)?(ms|MB)?/g, "<*>");
const templates = new Interner(),
    byTemplate = [],
    topTemplates = new TopK(32);
const perMinute = Array.from({ length: 1440 }, () => ({ n: 0, e5: 0, digest: new TDigest(50) }));
const perRoute = new Map(),
    ips = new HyperLogLog(14),
    upstreamLines = new Map(),
    errorLines = new RoaringBitmap();
const signatures = new MultiMatcher(['" 503 ', '" 500 ', "exhausted", "timeout", "OutOfMemory"]),
    sigCounts = new Multiset();
const t0 = performance.now();
for (let i = 0; i < lines.length; i++) {
    const { ts, text } = lines[i];
    const minute = Math.floor((ts - DAY) / 60_000);
    for (const m of signatures.allIn(text)) sigCounts.add(["503", "500", "exhausted", "timeout", "OOM"][m.index]);
    const a = ACCESS.exec(text);
    if (a) {
        const ms = Number(a[7]) * 1000,
            status = Number(a[5]);
        const pm = perMinute[minute];
        pm.n++;
        if (status >= 500) {
            pm.e5++;
            errorLines.add(i);
        }
        pm.digest.add(ms);
        (perRoute.get(a[4]) ?? perRoute.set(a[4], new TDigest(100)).get(a[4])).add(ms);
        ips.add(a[1]);
        (upstreamLines.get(a[8]) ?? upstreamLines.set(a[8], new RoaringBitmap()).get(a[8])).add(i);
        continue;
    }
    const msg = text.slice(text.indexOf(" app ") + 5);
    const id = templates.intern(templateOf(msg));
    (byTemplate[id] ??= new RoaringBitmap()).add(i);
    topTemplates.add(String(id));
}
const parseMs = performance.now() - t0;
const totalReq = perMinute.map((m) => m.n).sum(),
    total5xx = perMinute.map((m) => m.e5).sum();
results["every access line parsed"] = totalReq === truth.requests && total5xx === truth.s5xx;
// Four application shapes plus the failure; "served by cart-1" and "served by pay-2" differ in a word, so that
// shape splits by upstream family (cart/search/pay): seven templates in all.
results["application lines collapse to seven templates"] =
    byTemplate.length === 7 && templates.has("request <*> served by pay-<*> in <*>");
results["distinct IPs within 2% (HyperLogLog)"] = Math.abs(ips.count() - truth.ips.size) / truth.ips.size < 0.02;
{
    const all = new TDigest(200);
    for (const m of perMinute) all.merge(m.digest);
    const sorted = Float64Array.from(truth.latency).sort();
    const exact99 = sorted[Math.floor(0.99 * (sorted.length - 1))];
    results["merged per-minute digests give the day's p99 within 3%"] =
        Math.abs(all.quantile(0.99) - exact99) / exact99 < 0.03;
}
results["signature scan counts every 503"] =
    sigCounts.count("503") === lines.filter((l) => ACCESS.exec(l.text)?.[5] === "503").length;

// =============================================================================
// Detection: robust baseline over the trailing hour
// =============================================================================
const anomalies = new RangeSet(),
    window = new RingBuffer(60);
const scored = [];
for (let m = 0; m < 1440; m++) {
    const rate = perMinute[m].e5 / Math.max(1, perMinute[m].n);
    const hist = window.toArray();
    if (hist.length >= 30) {
        const med = hist.median();
        const mad = hist.map((x) => Math.abs(x - med)).median() || 0.002;
        const z = (rate - med) / (1.4826 * mad);
        scored.push([m, z]);
        if (z > 8 && perMinute[m].e5 >= 5) anomalies.add(m, m + 1);
    }
    if (!anomalies.contains(m)) window.push(rate);
}
const windows = anomalies.ranges();
results["exactly one incident window found"] = windows.length === 1;
results["incident window matches the planted 14:00-14:20 within a minute"] =
    windows.length === 1 && Math.abs(windows[0][0] - INCIDENT[0]) <= 1 && Math.abs(windows[0][1] - INCIDENT[1]) <= 1;
const hhmm = (m) => `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;

// =============================================================================
// Root cause: lift of each upstream and template inside the incident
// =============================================================================
const [w0, w1] = windows[0] ?? INCIDENT;
const incidentLines = new RoaringBitmap(),
    normalLines = new RoaringBitmap();
{
    const firstIn = lines.findIndex((l) => l.ts >= DAY + w0 * 60_000);
    const firstAfter = lines.findIndex((l) => l.ts >= DAY + w1 * 60_000);
    incidentLines.addRange(firstIn, firstAfter);
    normalLines.addRange(0, lines.length).andNot(incidentLines);
}
const nIn = incidentLines.toArray().length,
    nOut = normalLines.toArray().length;
const lift = (bm) => (bm.andCardinality(incidentLines) + 1) / nIn / ((bm.andCardinality(normalLines) + 1) / nOut);
// An upstream's request share does not move in an incident; its share of the ERRORS does.
const upstreamLift = [...upstreamLines]
    .map(([u, bm]) => [u, lift(bm.clone().and(errorLines))])
    .sort((a, b) => b[1] - a[1]);
const templateLift = byTemplate.map((bm, id) => [templates.get(id), lift(bm)]).sort((a, b) => b[1] - a[1]);
results["the failing upstream ranks first by lift"] = upstreamLift[0][0] === "pay-2";
results["the pool-exhaustion template ranks first by lift"] = templateLift[0][0].startsWith(
    "connection pool exhausted on pay-<*>",
);
results["the culprit's error lift is 10x the runner-up's"] = upstreamLift[0][1] > 10 * upstreamLift[1][1];
print(
    Table(
        [
            ["incident", windows.map(([a, b]) => `${hhmm(a)}-${hhmm(b)}`).join(", ")],
            ["top upstream", `${upstreamLift[0][0]} (lift ${upstreamLift[0][1].toFixed(1)})`],
            ["top template", `${templateLift[0][0].truncate(60)} (lift ${templateLift[0][1].toFixed(1)})`],
            ["checkout p99 (ms)", perRoute.get("/api/checkout").quantile(0.99).toFixed(0)],
            ["lines / parse time", `${lines.length.format()} in ${parseMs.toFixed(0)} ms`],
        ],
        { head: ["finding", "value"] },
    ),
);

// =============================================================================
// Archive: 10-minute zstd blocks with a time index
// =============================================================================
{
    const index = new SortedMap(),
        enc = new TextEncoder(),
        dec = new TextDecoder();
    let rawBytes = 0,
        packedBytes = 0;
    for (let start = 0; start < lines.length;) {
        const block = Math.floor((lines[start].ts - DAY) / 600_000);
        let end = start;
        while (end < lines.length && Math.floor((lines[end].ts - DAY) / 600_000) === block) end++;
        const text = lines
            .slice(start, end)
            .map((l) => l.text)
            .join("\n");
        const raw = enc.encode(text),
            packed = zstd(raw);
        rawBytes += raw.length;
        packedBytes += packed.length;
        index.set(block, packed);
        start = end;
    }
    const wanted = index.rangeQuery(Math.floor(w0 / 10), Math.floor((w1 - 1) / 10));
    const restored = wanted.flatMap(([, b]) => dec.decode(unzstd(b)).split("\n"));
    const exhausted = restored.filter((l) => l.includes("pool exhausted")).length;
    results["incident query opens only the overlapping blocks"] = wanted.length === 2 && index.keys().length === 144;
    results["restored blocks contain every pool-exhaustion line"] =
        exhausted === byTemplate[templates.lookup(templateLift[0][0])].toArray().length;
    results["logs compress at least 4x"] = rawBytes / packedBytes > 4;
    print(
        `archive: ${rawBytes.bytes()} -> ${packedBytes.bytes()} (${(rawBytes / packedBytes).toFixed(1)}x), incident query read ${wanted.length} of ${index.keys().length} blocks`,
    );
}

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
