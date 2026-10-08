// 16 · Access log analytics — streams a (possibly gzipped) log and reports traffic, errors and latency percentiles.
//
// WHAT IT SHOWS
//   - dyna:stream lines(): constant memory regardless of file size
//   - parsing into typed columns once, then answering many questions with dyna:dataframe
//   - percentiles, top-N paths, error rates per path, and an hourly traffic histogram
//
// RUN      dynajs examples/apps/16-access-log-analytics.js [access.log | access.log.zst]
//          Line format:  ISO-timestamp method path status latency-ms bytes
//          Without an argument it analyzes a generated sample.

import { lines, fromFile, inflate, deflate, toFile } from "dyna:stream";
import { DataFrame } from "dyna:dataframe";
import { Path, makeTempDir, removeAll } from "dyna:file";

const selfTest = scriptArgs.length < 2;
const work = selfTest ? makeTempDir("logs") : null;
const logPath = selfTest ? work.join("access.log.zst") : new Path(scriptArgs[1]);

if (selfTest) {
    // 20,000 synthetic requests, written through a compressing sink so the
    // sample exercises the same decompress-while-reading path as production.
    const sink = deflate(toFile(logPath), { codec: "zstd" });
    const paths = ["/", "/api/users", "/api/orders", "/api/search", "/static/app.js"];
    const enc = new TextEncoder();
    let chunk = "";
    for (let i = 0; i < 20000; i++) {
        const path = paths[i % paths.length];
        const slow = path === "/api/search";
        const status = i % 97 === 0 ? 500 : i % 41 === 0 ? 404 : 200;
        const ms = (slow ? 180 : 12) + (i * 7919) % (slow ? 400 : 30);
        const hour = String(Math.floor(i / 1000) % 24).padStart(2, "0");
        chunk += `2026-03-01T${hour}:00:00Z GET ${path} ${status} ${ms} ${500 + (i % 900)}\n`;
        if (chunk.length > 32768) { await sink.write(enc.encode(chunk)); chunk = ""; }
    }
    await sink.write(enc.encode(chunk));
    await sink.finish();                              // finalizes the stream and closes the file
}

// ---- ingest: one pass, growing typed columns --------------------------------
// Open the right source for the file: compressed logs are inflated as they are read.
const raw = fromFile(logPath);
const source = String(logPath).endsWith(".zst") ? inflate(raw, { codec: "zstd" }) : raw;

const pathCol = [], hourCol = [], statusCol = [], msCol = [], bytesCol = [];
let malformed = 0;
for await (const line of lines(source)) {
    const f = line.split(" ");
    // Logs contain garbage: truncated lines, scanner noise. Count and move on.
    if (f.length !== 6 || !(Number(f[3]) >= 100)) { malformed++; continue; }
    hourCol.push(Number(f[0].slice(11, 13)));
    pathCol.push(f[2].split("?")[0]);               // group by path, not by query string
    statusCol.push(Number(f[3]));
    msCol.push(Number(f[4]));
    bytesCol.push(Number(f[5]));
}

const df = new DataFrame({
    path: pathCol,
    hour: Int32Array.from(hourCol),
    status: Int32Array.from(statusCol),
    ms: Float64Array.from(msCol),
    bytes: Float64Array.from(bytesCol),
});

// ---- questions -------------------------------------------------------------
const errors = df.GT("status", 499);                              // mask of 5xx rows
const [p50, p95, p99] = df.QUANTILES("ms", [0.5, 0.95, 0.99]);
const hits = df.GROUP_BY_COUNT("path");
const errorsByPath = df.GROUP_BY_COUNT("path", errors);
const latencyByPath = df.GROUP_BY_MEAN("path", "ms");
const perHour = df.GROUP_BY_COUNT("hour");

const pathReport = hits.keys.map((path, i) => {
    const e = errorsByPath.keys.indexOf(path);
    return {
        path, hits: hits.values[i],
        errorRate: (e < 0 ? 0 : errorsByPath.values[e]) / hits.values[i],
        meanMs: latencyByPath.values[latencyByPath.keys.indexOf(path)],
    };
}).sort((a, b) => b.meanMs - a.meanMs);

console.log(`${df.ROWS} requests, ${malformed} malformed lines, ${(df.SUM("bytes") / 1e6).toFixed(1)} MB sent`);
console.log(`latency p50=${p50.toFixed(0)}ms p95=${p95.toFixed(0)}ms p99=${p99.toFixed(0)}ms`);
for (const r of pathReport)
    console.log(`  ${r.path.padEnd(16)} ${String(r.hits).padStart(6)} hits  ${r.meanMs.toFixed(0).padStart(4)}ms  ${(r.errorRate * 100).toFixed(2)}% 5xx`);

// ---- self-test -------------------------------------------------------------
if (selfTest) {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    check(df.ROWS === 20000 && malformed === 0, "every generated line was parsed");
    check(pathReport[0].path === "/api/search", "the slow endpoint ranks first by latency");
    check(p50 < p95 && p95 <= p99, "percentiles are ordered");
    check(perHour.keys.length === 20 && perHour.values.every((v) => v === 1000), "traffic buckets by hour");
    const total5xx = errors.reduce((a, b) => a + b, 0);
    check(total5xx === Math.ceil(20000 / 97), "5xx count matches the generator: " + total5xx);
    console.log("self-test passed");
    removeAll(work);
}
