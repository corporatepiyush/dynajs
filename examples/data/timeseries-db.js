// Time-series database — line-protocol ingest, compressed chunks, a bitmap tag index, a query language, sketch rollups
// and retention.
//
// WHAT IT IS
//   A compact metrics store of the Prometheus/Influx kind, over HTTP:
//
//   ingest       POST /write takes line protocol: `cpu,host=web1,dc=fra usage=0.42,steal=0.01 1772524800000`
//   ingest order points may arrive up to 10 minutes late (inserted in order) and retried writes replace rather than
//                duplicate; anything older than newest - 10 min is refused. Only points already outside that window
//                are sealed, so an acceptable late point always finds the open buffer
//   storage      each series seals chunks of 128: timestamps as delta-of-delta varints, values per chunk in whichever
//                codec is smaller — Gorilla XOR bit-packing (constants cost one bit) or zstd over raw float64;
//                chunk metadata (min/max time) in a SortedMap; decoded chunks are LRU-cached (sealed chunks never change)
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
//   oracles      the self-test delays, drops and retries 5% of its lines; a plain Map-per-series model applies the
//                acceptance contract to what was delivered, every series must equal it point for point, and 300
//                random queries (aggregate x metric x selector x range x step) must equal a plain evaluation of it
//
// MODULES  dyna:http · dyna:encoding (varints) · dyna:compress (zstd) · dyna:structures (SortedMap, Trie, RangeSet,
//          Interner, RoaringBitmap, TDigest, HyperLogLog, ZSet, LRU) · dyna:simd · dyna:mathx (Expression) · dyna:net
//          (Metrics) · dyna:random · Array/Number extensions
//
// RUN      dynajs examples/data/timeseries-db.js --port 8086     serve
//          dynajs examples/data/timeseries-db.js                 ingests six hours of metrics for 12 hosts and queries them

import { App } from "dyna:http";
import { appendUvarint, PutVarint } from "dyna:encoding";
import { zstd, unzstd } from "dyna:compress";
import { SortedMap, Trie, RangeSet, Interner, RoaringBitmap, TDigest, HyperLogLog, ZSet, LRU } from "dyna:structures";
import * as simd from "dyna:simd";
import { Expression } from "dyna:mathx";
import { Metrics } from "dyna:metrics";
import { Random } from "dyna:random";
import { Command } from "dyna:cli";

const { options } = new Command("timeseries-db")
    .option("-p, --port <n>", "port", { type: "number" })
    .parse(scriptArgs.slice(1));
const selfTest = options.port === undefined;
const CHUNK = 128;
const ROLLUP = 5 * 60_000;
const OOO_WINDOW = 10 * 60_000; // a point up to 10 minutes older than its series' newest is still accepted

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
// The varints are decoded inline: the first delta is a whole epoch timestamp (41 bits), so the arithmetic is in
// doubles (exact below 2^53), not 32-bit shifts. Varint(buf.subarray(off)) per value cost 0.5 us: a view and a pair.
function decodeTimes(buf) {
    let off = 0;
    const uvarint = () => {
        let u = 0,
            scale = 1,
            b;
        do {
            b = buf[off++];
            u += (b & 127) * scale;
            scale *= 128;
        } while (b & 128);
        return u;
    };
    const n = uvarint(),
        ts = new Float64Array(n);
    let prev = 0,
        prevDelta = 0;
    for (let i = 0; i < n; i++) {
        const u = uvarint();
        prevDelta += u % 2 ? -(u + 1) / 2 : u / 2; // zigzag
        prev += prevDelta;
        ts[i] = prev;
    }
    return ts;
}
// Gorilla XOR (Pelkonen et al., VLDB 2015 §4.1.2): each value is XORed with the previous one; an identical value is
// one 0 bit; otherwise the meaningful bits are written inside the previous value's leading/trailing-zero window when
// they fit ('10'), else with a new 5-bit leading count and 6-bit length ('11'). Doubles are handled as two uint32
// halves, so no BigInt is involved.
class BitWriter {
    constructor(cap) {
        this.buf = new Uint8Array(cap);
        this.bit = 0;
    }
    write(v, n) {
        // the low n bits of v (n <= 32), most significant first, up to 8 bits per step
        while (n > 0) {
            if (this.bit >> 3 >= this.buf.length) {
                const b = new Uint8Array(this.buf.length * 2);
                b.set(this.buf);
                this.buf = b;
            }
            const off = this.bit & 7,
                take = Math.min(8 - off, n),
                bits = Math.floor(v / 2 ** (n - take)) & ((1 << take) - 1);
            this.buf[this.bit >> 3] |= bits << (8 - off - take);
            this.bit += take;
            n -= take;
        }
    }
    write64(hi, lo, n) {
        if (n > 32) {
            this.write(hi, n - 32);
            this.write(lo, 32);
        } else this.write(lo, n);
    }
    bytes() {
        return this.buf.slice(0, (this.bit + 7) >> 3);
    }
}
class BitReader {
    constructor(buf) {
        this.buf = buf;
        this.bit = 0;
    }
    read(n) {
        // up to 8 bits per step; v stays below 2^32, so the arithmetic is exact in a double
        let v = 0;
        while (n > 0) {
            const off = this.bit & 7,
                take = Math.min(8 - off, n);
            v = v * (1 << take) + ((this.buf[this.bit >> 3] >> (8 - off - take)) & ((1 << take) - 1));
            this.bit += take;
            n -= take;
        }
        return v;
    }
}
const ctz32 = (x) => 31 - Math.clz32(x & -x);
const F64 = new Float64Array(1),
    U32 = new Uint32Array(F64.buffer),
    [LO, HI] = new Uint8Array(new Uint16Array([1]).buffer)[0] === 1 ? [0, 1] : [1, 0];
function gorillaEncode(vs) {
    const w = new BitWriter(vs.length * 2 + 16);
    F64[0] = vs[0];
    let ph = U32[HI],
        pl = U32[LO],
        lead = -1,
        trail = 0;
    w.write(ph, 32);
    w.write(pl, 32);
    for (let i = 1; i < vs.length; i++) {
        F64[0] = vs[i];
        const h = U32[HI],
            l = U32[LO],
            xh = (h ^ ph) >>> 0,
            xl = (l ^ pl) >>> 0;
        ph = h;
        pl = l;
        if (!xh && !xl) {
            w.write(0, 1);
            continue;
        }
        const lz = Math.min(31, xh ? Math.clz32(xh) : 32 + Math.clz32(xl)),
            tz = xl ? ctz32(xl) : 32 + ctz32(xh);
        let L, T;
        if (lead >= 0 && lz >= lead && tz >= trail) {
            w.write(0b10, 2);
            L = lead;
            T = trail;
        } else {
            w.write(0b11, 2);
            lead = L = lz;
            trail = T = tz;
            w.write(L, 5);
            w.write((64 - L - T) & 63, 6); // a 64-bit window is written as 0
        }
        // xor >> T, then its low (64 - L - T) bits
        const sh = T >= 32 ? 0 : T === 0 ? xh : (xh >>> T) >>> 0,
            sl = T >= 32 ? xh >>> (T - 32) : T === 0 ? xl : ((xl >>> T) | (xh << (32 - T))) >>> 0;
        w.write64(sh, sl, 64 - L - T);
    }
    return w.bytes();
}
function gorillaDecode(buf, n) {
    const r = new BitReader(buf),
        out = new Float64Array(n);
    let ph = r.read(32),
        pl = r.read(32),
        L = 0,
        T = 0;
    U32[HI] = ph;
    U32[LO] = pl;
    out[0] = F64[0];
    for (let i = 1; i < n; i++) {
        if (r.read(1)) {
            if (r.read(1)) {
                L = r.read(5);
                T = 64 - L - (r.read(6) || 64);
            }
            const m = 64 - L - T,
                sh = m > 32 ? r.read(m - 32) : 0,
                sl = r.read(Math.min(m, 32));
            const xh = T >= 32 ? (sl << (T - 32)) >>> 0 : T === 0 ? sh : ((sh << T) | (sl >>> (32 - T))) >>> 0,
                xl = T >= 32 ? 0 : (sl << T) >>> 0;
            ph = (ph ^ xh) >>> 0;
            pl = (pl ^ xl) >>> 0;
        }
        U32[HI] = ph;
        U32[LO] = pl;
        out[i] = F64[0];
    }
    return out;
}
// The value codec is chosen per chunk: Gorilla wins on constants, counters and slowly moving gauges; zstd over raw
// doubles wins on noisy decimals. Both are lossless, so the choice is only about bytes.
const codecUse = { gorilla: 0, zstd: 0 };
const sealChunk = (ts, vs) => {
    const t = encodeTimes(ts);
    const g = gorillaEncode(vs),
        z = zstd(new Uint8Array(Float64Array.from(vs).buffer), { level: 3 });
    const codec = g.length <= z.length ? "gorilla" : "zstd",
        v = codec === "gorilla" ? g : z;
    codecUse[codec]++;
    return { min: ts[0], max: ts.last(), n: ts.length, codec, times: t, values: v, bytes: t.length + v.length };
};
const decodeChunk = (c) => ({
    ts: decodeTimes(c.times),
    vs: c.codec === "gorilla" ? gorillaDecode(c.values, c.n) : new Float64Array(unzstd(c.values).buffer),
});
// Sealed chunks never change, so decoded ones are cached (Prometheus keeps a chunk cache for the same reason): a
// dashboard's queries overlap, and the tail chunks are read by nearly every query ending at "now".
let chunkIds = 0;
const chunkCache = new LRU(512);
const openChunk = (c) => {
    const key = String((c.id ??= ++chunkIds));
    let d = chunkCache.get(key);
    if (!d) chunkCache.set(key, (d = decodeChunk(c)));
    return d;
};

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
        this.newest = -Infinity;
    }
    // Contract: a point is accepted iff t >= newest - OOO_WINDOW, where newest is the latest timestamp the series
    // had received before it; a repeated timestamp replaces the value (a retried write is idempotent). The open
    // buffer stays sorted, and only points older than newest - OOO_WINDOW are ever sealed, so an acceptable late
    // point always lands in the buffer.
    append(t, v) {
        if (t < this.newest - OOO_WINDOW) {
            db.stats.tooLate++;
            return "refused";
        }
        const ts = this.ts;
        let lo = 0,
            hi = ts.length;
        if (hi && t > ts[hi - 1])
            lo = hi; // in-order: the common case, no search
        else
            while (lo < hi) {
                const mid = (lo + hi) >> 1;
                if (ts[mid] < t) lo = mid + 1;
                else hi = mid;
            }
        if (ts[lo] === t) {
            this.vs[lo] = v;
            db.stats.overwritten++;
            return "replaced";
        }
        if (lo < ts.length) db.stats.outOfOrder++;
        ts.splice(lo, 0, t);
        this.vs.splice(lo, 0, v);
        if (t > this.newest) this.newest = t;
        if (ts.length > CHUNK && ts[CHUNK - 1] < this.newest - OOO_WINDOW) this.seal(CHUNK);
        return "stored";
    }
    seal(count = this.ts.length) {
        if (!count) return;
        const ts = this.ts.slice(0, count),
            vs = this.vs.slice(0, count);
        const c = sealChunk(ts, vs);
        this.chunks.set(c.min, c);
        for (let i = 0; i < ts.length; i++) {
            const b = Math.floor(ts[i] / ROLLUP) * ROLLUP;
            let r = this.rollups.get(b);
            if (!r)
                this.rollups.set(
                    b,
                    (r = { min: Infinity, max: -Infinity, sum: 0, count: 0, digest: new TDigest(100) }),
                );
            const v = vs[i];
            r.min = Math.min(r.min, v);
            r.max = Math.max(r.max, v);
            r.sum += v;
            r.count++;
            r.digest.add(v);
        }
        db.stats.rawBytes += ts.length * 16;
        db.stats.storedBytes += c.bytes;
        db.stats.chunks++;
        this.ts.splice(0, count);
        this.vs.splice(0, count);
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
    stats: {
        points: 0,
        lines: 0,
        rejected: 0,
        outOfOrder: 0,
        tooLate: 0,
        overwritten: 0,
        chunks: 0,
        rawBytes: 0,
        storedBytes: 0,
        dropped: 0,
    },
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
            const r = db.get(`${m[1]}.${field}`, tags).append(t, v);
            if (r === "stored") db.stats.points++;
            if (r !== "refused") accepted++;
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
    // each series is read (chunks decoded) once for the whole range, then cut into steps by a cursor
    const whole = agg === "p95" ? [] : series.map((s) => s.points(from, to)),
        cursor = whole.map(() => 0);
    for (let t = from; t < to; t += step) {
        if (agg === "p95") {
            out.push([t, p95Of(series, t, t + step)]);
            continue;
        }
        const parts = whole.map((p, k) => {
            const a = cursor[k];
            let i = a;
            while (i < p.ts.length && p.ts[i] < t + step) i++;
            cursor[k] = i;
            return { ts: p.ts.slice(a, i), vs: p.vs.slice(a, i) };
        });
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
    // The network misbehaves: 4% of lines arrive 1-8 minutes late (inside the window), 0.5% two hours late (outside
    // it), and 1% are retried a minute later. The model below applies the acceptance CONTRACT to what was delivered,
    // in delivery order, with a plain Map per series: it shares no code with the database.
    const delayed = []; // [deliverAt, line]
    const model = { series: new Map(), newest: new Map(), accepted: 0, tooLate: 0, overwritten: 0 };
    const deliver = (lines) => {
        for (const line of lines) {
            const m = /^(\w+),(\S+) (\S+) (\d+)$/.exec(line);
            if (!m || !line.includes("=") || /NaN/.test(line)) continue;
            const t = Number(m[4]),
                tags = m[2].split(",").sort().join(",");
            for (const kv of m[3].split(",")) {
                const [field, raw] = kv.split("=");
                const key = `${m[1]}.${field}{${tags}}`;
                const newest = model.newest.get(key) ?? -Infinity;
                if (t < newest - OOO_WINDOW) {
                    model.tooLate++;
                    continue;
                }
                const pts = model.series.getOrInsertComputed(key, () => new Map());
                if (pts.has(t)) model.overwritten++;
                else model.accepted++;
                pts.set(t, Number(raw));
                model.newest.set(key, Math.max(newest, t));
            }
        }
    };
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
        const now = [];
        for (const line of lines) {
            const u = rng.nextFloat();
            if (u < 0.04) delayed.push([t + 60_000 * (1 + rng.nextBounded(8)), line]);
            else if (u < 0.045) delayed.push([t + 120 * 60_000, line]);
            else {
                now.push(line);
                if (u > 0.99) delayed.push([t + 60_000, line]); // a retry of a write that had in fact succeeded
            }
        }
        for (let k = delayed.length - 1; k >= 0; k--)
            if (delayed[k][0] <= t) {
                now.push(delayed[k][1]);
                delayed.splice(k, 1);
            }
        deliver(now);
        await (await fetch(base + "/write", { method: "POST", body: now.join("\n") })).text();
        requests++;
    }
    const undelivered = delayed.length;
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
    results[
        `ingest accepted ${db.stats.points} points (${db.stats.outOfOrder} late inserts, ${db.stats.overwritten} retries replaced), refused ${db.stats.tooLate} past the window, rejected ${db.stats.rejected} malformed`
    ] =
        db.stats.rejected === 2 &&
        db.stats.points === model.accepted &&
        db.stats.tooLate === model.tooLate &&
        db.stats.overwritten === model.overwritten &&
        db.stats.outOfOrder > 500 &&
        model.tooLate > 50 &&
        model.overwritten > 100;
    // Every series holds exactly the model's points, in time order, value for value (both codecs, every chunk).
    let contentOk = db.series.length === model.series.size;
    for (const s of db.series) {
        const want = [...(model.series.get(db.ids.get(s.id)) ?? new Map())].sort((a, b) => a[0] - b[0]);
        const got = s.points(-Infinity, Infinity);
        if (got.ts.length !== want.length || want.some(([t, v], i) => got.ts[i] !== t || !Object.is(got.vs[i], v)))
            contentOk = false;
    }
    results[`all ${db.series.length} series equal the delivery model point for point`] = contentOk;
    results[`value codec chosen per chunk: gorilla ${codecUse.gorilla}, zstd ${codecUse.zstd}`] =
        codecUse.gorilla > 0 && codecUse.zstd > 0;
    const ratio = db.stats.rawBytes / db.stats.storedBytes;
    results[`chunk compression ${ratio.round(1)}x`] = ratio > 2;
    const sug = await (await fetch(`${base}/metrics/suggest?prefix=mem`)).json();
    results["metric-name autocomplete"] = sug.join() === "mem.total,mem.used";

    // Query differential: 300 random queries against a plain evaluation over the delivery model (scan every
    // series key, filter tags with string compares, reduce left to right). Sums and means differ from the SIMD
    // reductions only by reassociation, so they get a relative bound; min, max and series counts must be exact.
    const sortedCache = new Map();
    const sortedModel = (key) =>
        sortedCache.getOrInsertComputed(key, () => [...model.series.get(key)].sort((a, b) => a[0] - b[0]));
    const plain = (agg, metric, matchers, spanMin, stepMin) => {
        const step = stepMin * 60_000,
            to = Math.floor(now / step) * step,
            from = to - spanMin * 60_000;
        const keys = [...model.series.keys()].filter((key) => {
            if (!key.startsWith(metric + "{")) return false;
            const tags = Object.fromEntries(
                key
                    .slice(metric.length + 1, -1)
                    .split(",")
                    .map((kv) => kv.split("=")),
            );
            return matchers.every(({ k, op, v }) =>
                op === "=" ? tags[k] === v : op === "!=" ? tags[k] !== v : v.split("|").includes(tags[k]),
            );
        });
        const out = [],
            nSteps = (to - from) / step;
        // bucket[step][key] = that key's points in the step, time-ordered
        const bucket = Array.from({ length: nSteps }, () => keys.map(() => []));
        keys.forEach((key, k) => {
            for (const [pt, v] of sortedModel(key))
                if (pt >= from && pt < to) bucket[Math.floor((pt - from) / step)][k].push([pt, v]);
        });
        for (let b = 0; b < nSteps; b++) {
            const per = bucket[b];
            if (agg === "rate") {
                let total = null;
                for (const pts of per) {
                    if (pts.length < 2) continue;
                    let inc = 0;
                    for (let i = 1; i < pts.length; i++)
                        inc += pts[i][1] >= pts[i - 1][1] ? pts[i][1] - pts[i - 1][1] : pts[i][1];
                    total = (total ?? 0) + (inc / (pts.at(-1)[0] - pts[0][0])) * 1000;
                }
                out.push(total);
                continue;
            }
            const vs = per.flat().map(([, v]) => v);
            if (!vs.length) out.push(null);
            else if (agg === "min") out.push(Math.min(...vs));
            else if (agg === "max") out.push(Math.max(...vs));
            else {
                let sum = 0;
                for (const v of vs) sum += v;
                out.push(agg === "sum" ? sum : sum / vs.length);
            }
        }
        return { series: keys.length, points: out };
    };
    const METRICS = ["cpu.usage", "cpu.steal", "mem.used", "mem.total", "http.requests"],
        AGGS = ["avg", "min", "max", "sum", "rate"],
        STEPS = [5, 10, 15, 30, 60];
    let diffWorst = 0,
        diffBad = 0,
        diffEmptyMatch = 0;
    for (let i = 0; i < 300; i++) {
        const agg = AGGS[rng.nextBounded(5)],
            metric = METRICS[rng.nextBounded(5)],
            stepMin = STEPS[rng.nextBounded(5)],
            spanMin = stepMin * (1 + rng.nextBounded(Math.floor(360 / stepMin)));
        const matchers = [];
        const kind = rng.nextBounded(4);
        if (kind === 1) matchers.push({ k: "dc", op: "=", v: rng.nextBounded(2) ? "fra" : "iad" });
        if (kind === 2) matchers.push({ k: "dc", op: "!=", v: "fra" });
        if (kind >= 2) {
            const pick = [...new Set((1 + rng.nextBounded(3)).times(() => hosts[rng.nextBounded(12)]))];
            matchers.push({ k: "host", op: "=~", v: pick.join("|") });
        }
        const sel = matchers.map(({ k, op, v }) => `${k}${op}${v}`).join(",");
        const got = query(`${agg}(${metric}${sel ? `{${sel}}` : ""})[${spanMin}m:${stepMin}m]`, now);
        const want = plain(agg, metric, matchers, spanMin, stepMin);
        if (!want.series) diffEmptyMatch++;
        let ok = got.series[0] === want.series && got.points.length === want.points.length;
        for (let k = 0; ok && k < want.points.length; k++) {
            const g = got.points[k][1],
                w = want.points[k];
            if (g === null || w === null) ok = g === w;
            else {
                const err =
                    agg === "min" || agg === "max" ? (g === w ? 0 : 1) : Math.abs(g - w) / Math.max(1, Math.abs(w));
                diffWorst = Math.max(diffWorst, err);
                ok = err < 1e-9;
            }
        }
        if (!ok) diffBad++;
    }
    results[
        `300 random queries equal a plain evaluation of the model (worst relative error ${diffWorst.toExponential(1)})`
    ] = diffBad === 0 && diffEmptyMatch < 60;

    // Rollups agree with raw data; retention drops old raw chunks and shrinks covered time.
    const s3 = db.series[db.ids.lookup("cpu.usage{dc=fra,host=web3}")];
    const [b0, r0] = s3.rollups.rangeQuery(start + 3600_000, start + 3600_000)[0] || [];
    const raw = s3.points(b0, b0 + ROLLUP);
    results["a 5-minute rollup matches its raw points"] =
        r0 && r0.count === raw.vs.length && Math.abs(r0.sum - raw.vs.sum()) < 1e-9;
    db.retain(start + 3 * 3600_000);
    results[`retention dropped ${db.stats.dropped} chunks`] =
        db.stats.dropped > 0 && db.covered.ranges()[0][0] >= start + 3 * 3600_000;
    // Codec edges the workload never produces: values differing only in the low mantissa bits (XOR with 32+ leading
    // zeros, past the 5-bit leading count), a full 64-bit window, signed zeros, infinities, NaN payloads, subnormals.
    const edge = new Float64Array([
        1000.5,
        1000.5 + 2 ** -40,
        1000.5 + 2 ** -41,
        1000.5,
        -0,
        0,
        -0,
        Infinity,
        -Infinity,
        NaN,
        5e-324,
        -5e-324,
        Number.MAX_VALUE,
        1,
        1 + Number.EPSILON,
        1 + 2 * Number.EPSILON,
        2 ** 52 + 1,
        1,
        1,
        1,
        -1,
    ]);
    const nanPayload = new Float64Array(1);
    new Uint32Array(nanPayload.buffer)[LO] = 0xdead;
    new Uint32Array(nanPayload.buffer)[HI] = 0x7ff80000;
    const edges = Float64Array.from([...edge, nanPayload[0], 3, nanPayload[0]]);
    const back = gorillaDecode(gorillaEncode(edges), edges.length);
    results["gorilla round-trips codec edges bit for bit (signed zero, NaN payload, subnormal, low-bit deltas)"] =
        new Uint8Array(back.buffer).every((b, i) => b === new Uint8Array(edges.buffer)[i]);
    // The window boundary, exactly: newest - 10 min is accepted, one millisecond older is refused.
    const probe = db.get("probe.x", { host: "edge" }),
        T = start + 7 * 3600_000;
    const answers = [probe.append(T, 1), probe.append(T - OOO_WINDOW, 2), probe.append(T - OOO_WINDOW - 1, 3)];
    results["the out-of-order window is inclusive at newest - 10 min and exclusive beyond"] =
        answers.join() === "stored,stored,refused";
    const metrics = await (await fetch(`${base}/metrics`)).text();
    results["the database reports its own series count, and HyperLogLog agrees within 2%"] =
        /tsdb_series 61\b/.test(metrics) && Math.abs(db.cardinality.count() - 61) / 61 < 0.02;
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
