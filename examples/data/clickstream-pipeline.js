// Clickstream pipeline — compressed event streams to sessions, funnels, sketches, rollups and a queryable archive.
//
// WHAT IT IS
//   Product analytics on raw event logs too big to load at once:
//
//   ingest      hourly gzip NDJSON files streamed line by line (constant memory), schema-checked per event;
//               malformed lines are counted and skipped with their file and line number
//   sessions    events per user ordered by time and cut into sessions at 30 minutes of inactivity
//   funnel      view -> add_to_cart -> checkout -> purchase, counted per session in order, with drop-off rates
//   sketches    distinct users with HyperLogLog, top pages with a count-min sketch and a Space-Saving TopK,
//               compared against exact counts (Interner ids, a native Multiset) to show the error you buy
//   cohorts     users per funnel stage as RoaringBitmaps over interned user ids: "added to cart but never
//               bought" is one native set expression, not a JS loop over sets
//   spenders    a ZSet leaderboard of customers by revenue and a t-digest of session lengths (p50/p95/p99)
//   rollups     hourly DataFrame rollups (events, revenue) with GROUP_BY over an integer hour column
//   archive     rollups and sessions stored as CBOR + zstd blocks with a min/max time index, so a query for one
//               time range opens only the blocks that overlap it
//
// MODULES  dyna:stream (fromFile, inflate, lines/ndjson) · dyna:compress · dyna:schema · dyna:structures (HyperLogLog,
//          CountMinSketch, TopK, Multiset, Interner, RoaringBitmap, ZSet, TDigest, SortedMap) · dyna:simd (f64Min,
//          argmax) · dyna:dataframe · dyna:serialize (CBOR) · dyna:random · dyna:file · dyna:time · Array/Number extensions
//
// RUN      dynajs examples/data/clickstream-pipeline.js events/*.ndjson.gz     analyse real files
//          dynajs examples/data/clickstream-pipeline.js                        generates a day of traffic

import { fromFile, toFile, inflate, deflate, ndjson } from "dyna:stream";
import { zstd, unzstd } from "dyna:compress";
import { Schema } from "dyna:schema";
import {
    HyperLogLog,
    CountMinSketch,
    SortedMap,
    TopK,
    Multiset,
    Interner,
    RoaringBitmap,
    ZSet,
    TDigest,
} from "dyna:structures";
import { f64Min, argmax } from "dyna:simd";
import { DataFrame } from "dyna:dataframe";
import { CBOREncode, CBORDecode } from "dyna:serialize";
import { Random } from "dyna:random";
import { Path, makeTempDir, writeFile, readBytes, removeAll } from "dyna:file";
import { formatRFC3339 } from "dyna:time";

const selfTest = scriptArgs.length < 2;
const GAP = 30 * 60_000;
const STEPS = ["view", "add_to_cart", "checkout", "purchase"];
const work = makeTempDir("clicks");
const files = selfTest ? await generateDay(work) : scriptArgs.slice(1).map((f) => new Path(f));

// =============================================================================
// Ingest: stream, validate, accumulate
// =============================================================================
const eventSchema = Schema.compile({
    type: "object",
    required: ["ts", "user", "type", "page"],
    properties: {
        ts: { type: "integer", minimum: 1_600_000_000_000 },
        user: { type: "string", minLength: 1 },
        type: { enum: ["view", "add_to_cart", "checkout", "purchase", "search"] },
        page: { type: "string" },
        revenue: { type: "number", minimum: 0 },
    },
});
const byUser = new Map(); // user -> [ [ts, type, page, revenue] ]
const hll = new HyperLogLog(14),
    cms = new CountMinSketch(2048, 5),
    hotPages = new TopK(12);
const userIds = new Interner(),
    exactPages = new Multiset(),
    spenders = new ZSet();
const cohort = Object.fromEntries(["view", "search", ...STEPS.slice(1)].map((t) => [t, new RoaringBitmap()]));
let events = 0,
    malformed = 0;
const problems = [];
for (const file of files) {
    let line = 0;
    const it = ndjson(inflate(fromFile(file), { codec: "gzip" }));
    while (true) {
        let step;
        line++;
        try {
            step = await it.next();
        } catch (e) {
            malformed++;
            if (problems.length < 5) problems.push(`${file.basename}:${line}: ${e.message}`);
            continue;
        }
        if (step.done) break;
        const e = step.value;
        if (!eventSchema.validate(e).valid) {
            malformed++;
            if (problems.length < 5) problems.push(`${file.basename}:${line}: schema`);
            continue;
        }
        events++;
        if (!byUser.has(e.user)) byUser.set(e.user, []);
        byUser.get(e.user).push([e.ts, e.type, e.page, e.revenue || 0]);
        hll.add(e.user);
        cohort[e.type].add(userIds.intern(e.user));
        if (e.revenue) spenders.incrBy(e.user, e.revenue);
        if (e.type === "view") {
            cms.add(e.page);
            hotPages.add(e.page);
            exactPages.add(e.page);
        }
    }
}

// =============================================================================
// Sessions and funnel
// =============================================================================
const funnel = [0, 0, 0, 0];
const sessions = { start: [], end: [], events: [], revenue: [] };
const sessionMinutes = new TDigest(200);
for (const list of byUser.values()) {
    list.sort((a, b) => a[0] - b[0]);
    let s = null;
    const close = () => {
        if (!s) return;
        sessions.start.push(s.start);
        sessions.end.push(s.end);
        sessions.events.push(s.n);
        sessions.revenue.push(s.revenue);
        sessionMinutes.add((s.end - s.start) / 60_000);
        let reached = 0;
        for (const t of s.types)
            if (t === STEPS[reached]) {
                funnel[reached]++;
                if (++reached === STEPS.length) break;
            }
    };
    for (const [ts, type, , revenue] of list) {
        if (!s || ts - s.end > GAP) {
            close();
            s = { start: ts, end: ts, n: 0, revenue: 0, types: [] };
        }
        s.end = ts;
        s.n++;
        s.revenue += revenue;
        s.types.push(type);
    }
    close();
}

// =============================================================================
// Rollups and the archive
// =============================================================================
const allTs = [],
    allRev = [];
for (const list of byUser.values())
    for (const [ts, , , rev] of list) {
        allTs.push(ts);
        allRev.push(rev);
    }
const order = allTs.map((_, i) => i).sort((a, b) => allTs[a] - allTs[b]);
const day0 = Math.floor(f64Min(Float64Array.from(allTs)) / 86400_000) * 86400_000;
const frame = new DataFrame({
    hour: Int32Array.from(order, (i) => Math.floor((allTs[i] - day0) / 3600_000)),
    revenue: Float64Array.from(order, (i) => allRev[i]),
});
const byHourCount = frame.GROUP_BY_COUNT("hour"),
    byHourRevenue = frame.GROUP_BY_SUM("hour", "revenue");
const hours = {
    ts: byHourCount.keys.map((h) => day0 + h * 3600_000),
    one: Array.from(byHourCount.values),
    revenue: Array.from(byHourRevenue.values),
};

const blocks = new SortedMap(); // block start -> { path, min, max, n }
const BLOCK = 6 * 3600_000;
const groups = new Map();
for (let k = 0; k < sessions.start.length; k++) {
    const b = Math.floor(sessions.start[k] / BLOCK) * BLOCK;
    if (!groups.has(b)) groups.set(b, []);
    groups.get(b).push([sessions.start[k], sessions.end[k], sessions.events[k], sessions.revenue[k]]);
}
for (const [b, rows] of groups) {
    const path = work.join(`sessions-${b}.cbor.zst`);
    writeFile(path, zstd(CBOREncode({ block: b, rows }), { level: 9 }));
    blocks.set(b, {
        path: path.toString(),
        min: Math.min(...rows.map((r) => r[0])),
        max: Math.max(...rows.map((r) => r[0])),
        n: rows.length,
    });
}
function querySessions(fromMs, toMs) {
    let opened = 0;
    const out = [];
    for (const [b, meta] of blocks.rangeQuery(Math.floor(fromMs / BLOCK) * BLOCK - BLOCK, toMs)) {
        if (meta.max < fromMs || meta.min > toMs) continue; // the index rules the block out
        opened++;
        for (const r of CBORDecode(unzstd(readBytes(new Path(meta.path)))).rows)
            if (r[0] >= fromMs && r[0] < toMs) out.push(r);
    }
    return { rows: out, opened };
}

// =============================================================================
// Report
// =============================================================================
const topExact = exactPages
    .entrySet()
    .sortWith([(a, b) => b[1] - a[1], (a, b) => a[0].compareBytes(b[0])])
    .take(5);
const hllErr = Math.abs(hll.count() - userIds.size) / userIds.size;
const abandoned = cohort.add_to_cart.clone().andNot(cohort.purchase);
const viewsTotal = exactPages.totalSize;
print("clickstream-pipeline");
print(
    `  ingest     ${events.format()} events from ${files.length} files, ${malformed} malformed (${problems[0] || "none"})`,
);
print(
    `  sessions   ${sessions.start.length.format()}, mean ${sessions.events.mean().toFixed(1)} events, length p50/p95/p99 ${sessionMinutes
        .quantiles([0.5, 0.95, 0.99])
        .map((m) => m.toFixed(1))
        .join("/")} min`,
);
print(
    `  funnel     ${STEPS.map((s, i) => `${s} ${funnel[i]}${i ? ` (${((funnel[i] / funnel[i - 1]) * 100).toFixed(1)}%)` : ""}`).join(" -> ")}`,
);
print(
    `  users      HLL ${Math.round(hll.count())} vs exact ${userIds.size} (${(hllErr * 100).toFixed(2)}% error, ${hll.registers} registers)`,
);
print(`  top pages  ${topExact.map(([p, n]) => `${p} ${n}/~${cms.count(p)}/topk ${hotPages.count(p)}`).join(", ")}`);
print(
    `  cohorts    ${cohort.view.size} viewers -> ${cohort.purchase.size} buyers; ${abandoned.size} carted but never bought (${(cohort.purchase.jaccard(cohort.checkout) * 100).toFixed(1)}% buyer/checkout overlap)`,
);
print(
    `  spenders   ${spenders
        .range(0, 2, { reverse: true })
        .map(([u, r]) => `${u} ${r.toFixed(2)}`)
        .join(", ")}`,
);
const peak = argmax(Float32Array.from(hours.one));
print(
    `  peak hour  ${formatRFC3339(hours.ts[peak] / 1000).slice(11, 16)} UTC with ${hours.one[peak]} events, revenue ${hours.revenue.sum().toFixed(2)}`,
);

if (selfTest) {
    const results = {};
    results[`seven malformed lines detected (${malformed})`] = malformed === 7;
    results["HyperLogLog within 2% of the exact distinct count"] = hllErr < 0.02;
    results["count-min never under-counts"] = topExact.every(([p, n]) => cms.count(p) >= n);
    results["TopK keeps every page above N/k and brackets its exact count"] = exactPages
        .entrySet()
        .every(
            ([p, n]) =>
                n <= viewsTotal / hotPages.k ||
                (hotPages.has(p) && hotPages.count(p) >= n && hotPages.count(p) - hotPages.error(p) <= n),
        );
    results["cart abandonment: in-place andNot equals the cardinality-only form"] =
        abandoned.size === cohort.add_to_cart.andNotCardinality(cohort.purchase);
    results["every buyer reached checkout"] = cohort.purchase.isSubsetOf(cohort.checkout);
    results["the spender leaderboard holds all revenue"] =
        spenders.size === cohort.purchase.size &&
        Math.abs([...spenders].map(([, r]) => r).sum() - hours.revenue.sum()) < 1e-6;
    results["the funnel narrows"] =
        funnel[0] >= funnel[1] && funnel[1] >= funnel[2] && funnel[2] >= funnel[3] && funnel[3] > 0;
    const noon = Date.UTC(2026, 2, 3, 12),
        q = querySessions(noon, noon + 3600_000);
    const brute = sessions.start.filter((s) => s >= noon && s < noon + 3600_000).length;
    results[`range query found ${q.rows.length}/${brute} sessions opening ${q.opened} of ${blocks.size} blocks`] =
        q.rows.length === brute && q.opened <= 2;
    results["rollup revenue equals session revenue"] = Math.abs(frame.SUM("revenue") - sessions.revenue.sum()) < 1e-6;
    removeAll(work);
    check(results);
}

// ---- a day of traffic in 24 hourly files ------------------------------------------
async function generateDay(dir) {
    const rng = new Random(7);
    const pages = Array.from({ length: 40 }, (_, i) => `/p/${i}`);
    const weight = pages.map((_, i) => 1 / (i + 1)); // Zipf-like page popularity
    const total = weight.sum();
    const pickPage = () => {
        let r = rng.nextFloat() * total;
        for (let i = 0; i < pages.length; i++) if ((r -= weight[i]) < 0) return pages[i];
        return pages[0];
    };
    const day0 = Date.UTC(2026, 2, 3);
    const perHour = Array.from({ length: 24 }, () => []);
    for (let u = 0; u < 3000; u++) {
        const user = `u${u}`;
        let t = day0 + rng.nextBounded(20 * 3600_000);
        for (let s = 0, ns = 1 + rng.poisson(0.8); s < ns; s++) {
            let stage = 0;
            for (let k = 0, n = 2 + rng.poisson(4); k < n; k++) {
                t += 5_000 + rng.nextBounded(240_000);
                if (t >= day0 + 24 * 3600_000) break;
                let type = "view";
                if (stage < 3 && rng.nextFloat() < [0.25, 0.5, 0.6][stage]) type = STEPS[++stage];
                const e = { ts: t, user, type, page: type === "view" ? pickPage() : `/${type}` };
                if (type === "purchase") e.revenue = Math.round(rng.exponential(1 / 60) * 100) / 100;
                perHour[Math.floor((t - day0) / 3600_000)].push(JSON.stringify(e));
            }
            t += GAP + rng.nextBounded(3 * 3600_000);
        }
    }
    perHour[3].splice(
        10,
        0,
        "{not json",
        '{"ts":1,"user":"x","type":"view","page":"/"}',
        '{"ts":1772500000000,"user":"","type":"view","page":"/"}',
    );
    perHour[9].push('{"ts":1772500000000,"user":"u1","type":"refund","page":"/"}', "[]", "garbage", '{"user":"u2"}');
    const out = [];
    for (let h = 0; h < 24; h++) {
        const path = dir.join(`events-${String(h).padStart(2, "0")}.ndjson.gz`);
        const sink = deflate(toFile(path), { codec: "gzip" });
        await sink.write(new TextEncoder().encode(perHour[h].join("\n") + "\n"));
        await sink.finish();
        out.push(path);
    }
    return out;
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
