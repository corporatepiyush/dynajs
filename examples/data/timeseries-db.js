// Time-series database — line-protocol ingest, compressed chunks, a bitmap tag index, a query language, sketch rollups
// and retention.
//
// WHAT IT IS
//   A compact metrics store of the Prometheus/Influx kind, over HTTP:
//
//   ingest       POST /write takes line protocol: `cpu,host=web1,dc=fra usage=0.42,steal=0.01 1772524800000`
//   storage      each series buffers points and seals them into chunks of 128: timestamps as delta-of-delta
//                varints, values as raw float64 compressed with zstd; chunk metadata (min/max time) in a SortedMap
//   index        series keys are interned to dense ids; every `tag=value` owns a RoaringBitmap of series ids, so a
//                selector is bitmap algebra: `=` intersects, `!=` subtracts, `=~a|b` unions the matching values first;
//                metric names live in a Trie for autocomplete; series cardinality is tracked by a HyperLogLog
//   queries      GET /query?q=avg(cpu.usage{dc=fra})[30m:5m] — aggregate (avg/min/max/sum/p95/rate) over matching
//                series, a range and a step; every step's samples are one Float64Array reduced by dyna:simd; derived
//                expressions like `mem.used / mem.total * 100` evaluate per step through a compiled Expression;
//                `topk(3, avg(cpu.usage))[1h:1h]` ranks series in a ZSet
//   rollups      sealed chunks also feed 5-minute rollups (min, max, sum, count and a TDigest), so a p95 over a long
//                range merges a few dozen digests instead of decompressing every chunk
//   retention    raw chunks older than the retention window are dropped; covered time is tracked as a RangeSet
//   introspection /metrics reports the database's own counters in Prometheus format
//
// MODULES  dyna:http · dyna:encoding (varints) · dyna:compress (zstd) · dyna:structures (SortedMap, Trie, RangeSet,
//          Interner, RoaringBitmap, TDigest, HyperLogLog, ZSet) · dyna:simd · dyna:mathx (Expression) · dyna:net
//          (Metrics) · dyna:random · Array/Number extensions
//
// RUN      dynajs examples/data/timeseries-db.js --port 8086     serve
//          dynajs examples/data/timeseries-db.js                 ingests six hours of metrics for 12 hosts and queries them

import { App } from "dyna:http";
import { appendUvarint, uvarintAt, PutVarint, Varint } from "dyna:encoding";
import { zstd, unzstd } from "dyna:compress";
import { SortedMap, Trie, RangeSet, Interner, RoaringBitmap, TDigest, HyperLogLog, ZSet } from "dyna:structures";
import * as simd from "dyna:simd";
import { Expression } from "dyna:mathx";
import { Metrics } from "dyna:net";
import { Random } from "dyna:random";
import { Command } from "dyna:cli";

const { options } = new Command("timeseries-db")
    .option("-p, --port <n>", "port", { type: "number" })
    .parse(scriptArgs.slice(1));
const selfTest = options.port === undefined;
const CHUNK = 128;
const ROLLUP = 5 * 60_000;

// =============================================================================
// Chunk codec
// =============================================================================
function encodeTimes(ts) {
    const out = new Uint8Array(ts.length * 10 + 10);
    let off = appendUvarint(out, ts.length, 0);
    let prev = 0;
    let prevDelta = 0;
    for (const t of ts) {
        const delta = t - prev;
        const v = PutVarint(delta - prevDelta); // zigzag: small +/- jitter stays one byte
        out.set(v, off);
        off += v.length;
        prev = t;
        prevDelta = delta;
    }
    return out.slice(0, off);
}
const uvarintLength = (n) => {
    let len = 1;
    while (n >= 128) {
        n = Math.floor(n / 128);
        len++;
    }
    return len;
};
function decodeTimes(buf) {
    const n = Number(uvarintAt(buf, 0));
    let off = uvarintLength(n);
    const ts = new Float64Array(n);
    let prev = 0;
    let prevDelta = 0;
    for (let i = 0; i < n; i++) {
        const [dod, used] = Varint(buf.subarray(off));
        off += used;
        prevDelta += Number(dod);
        prev += prevDelta;
        ts[i] = prev;
    }
    return ts;
}
const sealChunk = (ts, vs) => {
    const t = encodeTimes(ts);
    const v = zstd(new Uint8Array(Float64Array.from(vs).buffer), { level: 3 });
    return { min: ts[0], max: ts.last(), n: ts.length, times: t, values: v, bytes: t.length + v.length };
};
const openChunk = (c) => ({ ts: decodeTimes(c.times), vs: new Float64Array(unzstd(c.values).buffer) });

// =============================================================================
// Storage and index
// =============================================================================
class Series {
    constructor(id, metric, tags) {
        this.id = id;
        this.metric = metric;
        this.tags = tags;
        this.chunks = new SortedMap();
        this.ts = [];
        this.vs = [];
        this.rollups = new SortedMap();
    }
    append(t, v) {
        if (this.ts.length && t <= this.ts.last()) {
            db.stats.outOfOrder++;
            return;
        }
        this.ts.push(t);
        this.vs.push(v);
        if (this.ts.length === CHUNK) this.seal();
    }
    seal() {
        if (!this.ts.length) return;
        const c = sealChunk(this.ts, this.vs);
        this.chunks.set(c.min, c);
        for (let i = 0; i < this.ts.length; i++) {
            const b = Math.floor(this.ts[i] / ROLLUP) * ROLLUP;
            let r = this.rollups.get(b);
            if (!r)
                this.rollups.set(
                    b,
                    (r = { min: Infinity, max: -Infinity, sum: 0, count: 0, digest: new TDigest(100) }),
                );
            const v = this.vs[i];
            r.min = Math.min(r.min, v);
            r.max = Math.max(r.max, v);
            r.sum += v;
            r.count++;
            r.digest.add(v);
        }
        db.stats.rawBytes += this.ts.length * 16;
        db.stats.storedBytes += c.bytes;
        db.stats.chunks++;
        this.ts = [];
        this.vs = [];
    }
    points(from, to) {
        const ts = [];
        const vs = [];
        for (const [, c] of this.chunks.rangeQuery(-Infinity, to)) {
            if (c.max < from) continue;
            const { ts: t, vs: v } = openChunk(c);
            for (let i = 0; i < t.length; i++)
                if (t[i] >= from && t[i] < to) {
                    ts.push(t[i]);
                    vs.push(v[i]);
                }
        }
        for (let i = 0; i < this.ts.length; i++)
            if (this.ts[i] >= from && this.ts[i] < to) {
                ts.push(this.ts[i]);
                vs.push(this.vs[i]);
            }
        return { ts, vs };
    }
}
const db = {
    ids: new Interner(),
    series: [],
    postings: new Map(), // "k=v" -> RoaringBitmap of series ids
    tagValues: new Map(), // k -> Set of values (for =~ selectors)
    metrics: new Trie(),
    cardinality: new HyperLogLog(14),
    covered: new RangeSet(),
    stats: { points: 0, lines: 0, rejected: 0, outOfOrder: 0, chunks: 0, rawBytes: 0, storedBytes: 0, dropped: 0 },
    posting(k, v) {
        return this.postings.getOrInsertComputed(`${k}=${v}`, () => new RoaringBitmap());
    },
    get(metric, tags) {
        const key =
            metric +
            "{" +
            Object.keys(tags)
                .sort()
                .map((k) => `${k}=${tags[k]}`)
                .join(",") +
            "}";
        const id = this.ids.intern(key);
        let s = this.series[id];
        if (!s) {
            s = this.series[id] = new Series(id, metric, tags);
            this.cardinality.add(key);
            this.metrics.insert(metric);
            this.posting("__name__", metric).add(id);
            for (const [k, v] of Object.entries(tags)) {
                this.posting(k, v).add(id);
                this.tagValues.getOrInsertComputed(k, () => new Set()).add(v);
            }
        }
        return s;
    },
    select(metric, matchers) {
        const ids = this.posting("__name__", metric).clone();
        for (const { k, op, v } of matchers) {
            if (op === "=") ids.and(this.posting(k, v));
            else if (op === "!=") ids.andNot(this.posting(k, v));
            else {
                const re = new RegExp(`^(?:${v})$`);
                const any = new RoaringBitmap();
                for (const value of this.tagValues.get(k) || []) if (re.test(value)) any.or(this.posting(k, value));
                ids.and(any);
            }
        }
        return [...ids.toArray()].map((id) => this.series[id]);
    },
    retain(cutoff) {
        for (const s of this.series)
            for (const k of s.chunks.keys())
                if (s.chunks.get(k).max < cutoff) {
                    s.chunks.delete(k);
                    this.stats.dropped++;
                }
        this.covered.remove(-Infinity, cutoff);
    },
};

// Line protocol: measurement,tag=v,... field=value,... timestamp_ms
const LINE = /^([a-zA-Z_][\w.]*)((?:,[\w.]+=[^,\s]+)*) ([^\s]+) (\d{13})$/;
function ingest(body) {
    let accepted = 0;
    for (const line of body.lines()) {
        if (line.isBlank() || line.startsWith("#")) continue;
        db.stats.lines++;
        const m = LINE.exec(line.trim());
        if (!m) {
            db.stats.rejected++;
            continue;
        }
        const tags = m[2]
            .split(",")
            .filter(Boolean)
            .map((kv) => kv.split("="))
            .fromPairs();
        const t = Number(m[4]);
        for (const kv of m[3].split(",")) {
            const [field, raw] = kv.split("=");
            const v = Number(raw);
            if (!Number.isFinite(v)) {
                db.stats.rejected++;
                continue;
            }
            db.get(`${m[1]}.${field}`, tags).append(t, v);
            db.stats.points++;
            accepted++;
        }
        db.covered.add(t, t + 1);
    }
    return accepted;
}

// =============================================================================
// Query language: agg(metric{k=v,k!=v,k=~re})[range:step], expressions of such terms, and topk(n, term)
// =============================================================================
const reduce = {
    avg: (vs) => simd.f64Mean(vs),
    min: (vs) => simd.f64Min(vs),
    max: (vs) => simd.f64Max(vs),
    sum: (vs) => simd.f64Sum(vs),
};
function rateOf({ ts, vs }) {
    if (ts.length < 2) return null;
    let inc = 0;
    for (let i = 1; i < vs.length; i++) inc += vs[i] >= vs[i - 1] ? vs[i] - vs[i - 1] : vs[i]; // counter resets
    return (inc / (ts.last() - ts[0])) * 1000;
}
function p95Of(series, from, to) {
    // Whole rollup buckets merge their digests; ragged edges fall back to raw points.
    const d = new TDigest(100);
    for (const s of series) {
        let lo = Math.ceil(from / ROLLUP) * ROLLUP;
        const hi = Math.floor(to / ROLLUP) * ROLLUP;
        if (s.ts.length && s.ts[0] < hi) lo = hi; // buffered points are not rolled up yet
        for (const [, r] of s.rollups.rangeQuery(lo, hi - 1)) d.merge(r.digest);
        for (const [a, b] of [
            [from, lo],
            [Math.max(hi, lo), to],
        ])
            if (a < b) d.addMany(Float64Array.from(s.points(a, b).vs));
    }
    return d.count ? d.quantile(0.95) : null;
}
function evalTerm(agg, metric, matchers, from, to, step) {
    const series = db.select(metric, matchers);
    const out = [];
    for (let t = from; t < to; t += step) {
        if (agg === "p95") {
            out.push([t, p95Of(series, t, t + step)]);
            continue;
        }
        const parts = series.map((s) => s.points(t, t + step));
        if (agg === "rate") {
            const rates = parts.map(rateOf).compact();
            out.push([t, rates.length ? rates.sum() : null]);
            continue;
        }
        const vs = Float64Array.from(parts.flatMap((p) => p.vs));
        out.push([t, vs.length ? reduce[agg](vs) : null]);
    }
    return { series: series.length, ids: series.pluck("id"), points: out };
}
const SELECTOR = /^\s*([\w.]+)\s*(=~|!=|=)\s*(.+?)\s*$/;
const TERM = /(avg|min|max|sum|p95|rate)\(([\w.]+)(?:\{([^}]*)\})?\)/g;
function parseMatchers(sel) {
    return (sel || "")
        .split(",")
        .filter(Boolean)
        .map((part) => {
            const m = SELECTOR.exec(part);
            if (!m) throw new Error(`bad selector "${part}"`);
            return { k: m[1], op: m[2], v: m[3] };
        });
}
function query(q, now) {
    const range = /\[(\d+)([mh]):(\d+)([mh])\]\s*$/.exec(q);
    if (!range) throw new Error("expected a [range:step] suffix, e.g. [1h:5m]");
    const unit = (n, u) => Number(n) * (u === "h" ? 3600_000 : 60_000);
    const span = unit(range[1], range[2]);
    const step = unit(range[3], range[4]);
    const to = Math.floor(now / step) * step;
    const from = to - span;
    let body = q.slice(0, range.index).trim();
    const topk = /^topk\((\d+),\s*(.*)\)$/.exec(body);
    if (topk) body = topk[2];
    const terms = [];
    const exprText = body.replace(TERM, (_, agg, metric, sel) => {
        terms.push(evalTerm(agg, metric, parseMatchers(sel), from, to, step));
        return `t${terms.length - 1}`;
    });
    if (!terms.length) throw new Error("no aggregate term in the query");
    if (topk) {
        // rank each series by its own aggregate over the whole range
        const [, agg, metric, sel] = new RegExp(TERM.source).exec(body);
        const rank = new ZSet();
        for (const s of db.select(metric, parseMatchers(sel))) {
            const v = evalTerm(
                agg,
                metric,
                [...Object.entries(s.tags).map(([k, v]) => ({ k, op: "=", v }))],
                from,
                to,
                to - from,
            ).points[0][1];
            if (v !== null) rank.add(db.ids.get(s.id), v);
        }
        return { query: q, from, to, top: rank.range(0, Number(topk[1]) - 1, { reverse: true }) };
    }
    const expr = new Expression(exprText);
    const points = terms[0].points.map(([t], i) => {
        const vals = terms.map((tm) => tm.points[i][1]);
        return [t, vals.includes(null) ? null : expr.eval(terms.map((_, k) => `t${k}`).zipObj(vals))];
    });
    return { query: q, from, to, step, series: terms.pluck("series"), points };
}

// =============================================================================
// HTTP
// =============================================================================
const app = new App({ port: options.port || 0 });
app.post("/write", (req) => ({ status: 204, body: "", headers: { "x-points-accepted": String(ingest(req.body)) } }));
app.get("/query", (req) => {
    try {
        return query(req.query.q || "", Number(req.query.now) || Date.now());
    } catch (e) {
        return { status: 400, body: JSON.stringify({ error: e.message }), contentType: "application/json" };
    }
});
app.get("/metrics/suggest", (req) => db.metrics.keysWithPrefix(req.query.prefix || "").sort());
app.get("/metrics", () => {
    for (const [k, v] of Object.entries(db.stats)) Metrics.gauge(`tsdb_${k.underscore()}`, v);
    Metrics.gauge("tsdb_series", db.series.length);
    Metrics.gauge("tsdb_series_estimate", Math.round(db.cardinality.count()));
    return { body: Metrics.scrape(), contentType: "text/plain" };
});
app.start();

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}

if (!selfTest) print(`timeseries-db on :${app.port}`);
else {
    const results = {};
    const base = `http://127.0.0.1:${app.port}`;
    const rng = new Random(3);
    const start = Date.UTC(2026, 2, 4, 6);
    const hosts = (12).times((i) => `web${i + 1}`);
    let requests = 0;
    const counter = new Map();
    for (let t = start; t < start + 6 * 3600_000; t += 60_000) {
        // one sample per host per minute, 6 hours
        const lines = [];
        for (const [i, h] of hosts.entries()) {
            const dc = i < 6 ? "fra" : "iad";
            const usage =
                0.3 +
                0.2 * Math.sin((t - start) / 3600_000 + i) +
                rng.normal(0, 0.02) +
                (dc === "iad" && t > start + 4 * 3600_000 ? 0.25 : 0);
            const jitter = rng.nextBounded(400);
            counter.set(h, (counter.get(h) || 0) + 600 + rng.nextBounded(200));
            lines.push(`cpu,host=${h},dc=${dc} usage=${usage.toFixed(4)},steal=0.01 ${t + jitter}`);
            lines.push(
                `mem,host=${h},dc=${dc} used=${(8 + i * 0.5 + rng.normal(0, 0.1)).toFixed(3)},total=32 ${t + jitter}`,
            );
            lines.push(`http,host=${h},dc=${dc} requests=${counter.get(h)} ${t + jitter}`);
        }
        if (t === start + 3600_000) lines.push("broken line without timestamp", "cpu,host=x usage=NaNx 1772600000000");
        await (await fetch(base + "/write", { method: "POST", body: lines.join("\n") })).text();
        requests++;
    }
    for (const s of db.series) s.seal();
    const now = start + 6 * 3600_000;
    const q = async (text) => (await fetch(`${base}/query?now=${now}&q=${encodeURIComponent(text)}`)).json();

    const fra = await q("avg(cpu.usage{dc=fra})[6h:1h]");
    const iad = await q("avg(cpu.usage{dc=iad})[6h:1h]");
    results["tag matching selects six hosts per data centre"] =
        fra.series[0] === 6 && iad.series[0] === 6 && fra.points.length === 6;
    results["iad's late load spike shows up in its last hours only"] =
        iad.points[5][1] - fra.points[5][1] > 0.15 && Math.abs(iad.points[1][1] - fra.points[1][1]) < 0.15;
    const neg = await q("avg(cpu.usage{dc!=fra})[6h:1h]");
    const re = await q("avg(cpu.usage{host=~web1|web2|web12})[6h:1h]");
    results["!= subtracts a posting bitmap (iad only) and =~ unions three hosts"] =
        neg.series[0] === 6 &&
        neg.points.every(([, v], i) => Math.abs(v - iad.points[i][1]) < 1e-12) &&
        re.series[0] === 3;
    const memPct = await q("avg(mem.used{host=web3}) / avg(mem.total{host=web3}) * 100 [1h:15m]");
    results[`derived expression mem.used/mem.total*100 ≈ 28% (${memPct.points.map(([, v]) => v.round(1))})`] =
        memPct.points.every(([, v]) => Math.abs(v - 28.125) < 1.5);
    const rate = await q("rate(http.requests{dc=fra})[1h:10m]");
    results[`request rate per second across six hosts (${rate.points.map(([, v]) => v.round(1))})`] = rate.points.every(
        ([, v]) => v > 6 * 9 && v < 6 * 14,
    );
    const p95 = await q("p95(cpu.usage)[2h:1h]");
    results["an unfiltered query spans every cpu.usage series"] = p95.series[0] === 12;
    const rawP95 = (() => {
        const all = db
            .select("cpu.usage", [])
            .flatMap((s) => s.points(now - 3600_000, now).vs)
            .sort((a, b) => a - b);
        return all[Math.floor(0.95 * (all.length - 1))];
    })();
    results[
        `p95 from merged rollup digests (${p95.points[1][1].round(4)}) matches the raw order statistic (${rawP95.round(4)})`
    ] = Math.abs(p95.points[1][1] - rawP95) < 0.01;
    const top = await q("topk(3, avg(cpu.usage))[2h:2h]");
    results[`topk ranks iad hosts first (${top.top.pluck(0).join(" ")})`] =
        top.top.length === 3 && top.top.all(([k]) => k.includes("dc=iad"));
    results["a query without a range is a 400"] = (await fetch(`${base}/query?q=avg(cpu.usage)`)).status === 400;
    results["a malformed selector is a 400"] =
        (await fetch(`${base}/query?q=${encodeURIComponent("avg(cpu.usage{dc})[1h:1h]")}`)).status === 400;
    results[`ingest accepted ${db.stats.points} points, rejected ${db.stats.rejected}`] =
        db.stats.rejected === 2 && db.stats.points === 12 * 360 * 5;
    const ratio = db.stats.rawBytes / db.stats.storedBytes;
    results[`chunk compression ${ratio.round(1)}x`] = ratio > 2;
    const sug = await (await fetch(`${base}/metrics/suggest?prefix=mem`)).json();
    results["metric-name autocomplete"] = sug.join() === "mem.total,mem.used";

    // Rollups agree with raw data; retention drops old raw chunks and shrinks covered time.
    const s3 = db.series[db.ids.lookup("cpu.usage{dc=fra,host=web3}")];
    const [b0, r0] = s3.rollups.rangeQuery(start + 3600_000, start + 3600_000)[0] || [];
    const raw = s3.points(b0, b0 + ROLLUP);
    results["a 5-minute rollup matches its raw points"] =
        r0 && r0.count === raw.vs.length && Math.abs(r0.sum - raw.vs.sum()) < 1e-9;
    db.retain(start + 3 * 3600_000);
    results[`retention dropped ${db.stats.dropped} chunks`] =
        db.stats.dropped > 0 && db.covered.ranges()[0][0] >= start + 3 * 3600_000;
    const metrics = await (await fetch(`${base}/metrics`)).text();
    results["the database reports its own series count, and HyperLogLog agrees within 2%"] =
        /tsdb_series 60\b/.test(metrics) && Math.abs(db.cardinality.count() - 60) / 60 < 0.02;
    app.close();
    check(results);

    print("timeseries-db");
    print(
        `  ingest      ${db.stats.points} points in ${requests} writes, ${db.series.length} series, ${db.stats.chunks} chunks`,
    );
    print(
        `  storage     ${db.stats.rawBytes.bytes()} raw -> ${db.stats.storedBytes.bytes()} stored (${ratio.round(1)}x)`,
    );
    print(
        `  fra vs iad  ${fra.points.map(([, v]) => v.toFixed(2)).join(" ")}  |  ${iad.points.map(([, v]) => v.toFixed(2)).join(" ")}`,
    );
    print(`  web3 mem%   ${memPct.points.map(([, v]) => v.toFixed(1)).join(" ")}`);
    print(`  topk cpu    ${top.top.map(([k, v]) => `${k.replace(/^cpu\.usage/, "")} ${v.round(3)}`).join(" | ")}`);
}
