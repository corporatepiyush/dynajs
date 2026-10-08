// Module probe: for a spread of dyna:* operations at realistic sizes, three
// numbers that decide where optimisation effort should go --
//   ns/op and MB/s   CPU cost of the operation itself
//   block ms         how long ONE call holds the event loop (the concurrency cost)
//   KiB/op           process memory growth per call (native + JS), second half vs first
// Lines are machine readable: "probe <module> <op> <size> <ns/op> <MB/s> <block_ms> <KiB/op>".
// A row whose block ms exceeds the budget is tagged BLOCKS: run it on an
// os.Worker when the loop must keep serving (dyna:ml already computes each call
// on a thread of its own).
// Run: ./dynajs --std tests/probes/probe_modules.js [module-filter]
import * as std from "std";
import { rusage } from "dyna:sys";

const only = scriptArgs[1] || "";
const BLOCK_BUDGET_MS = 5;
const now = () => performance.now();
const rows = [];
let skipped = 0;

async function mod(n) { try { return await import("dyna:" + n); } catch (e) { return null; } }

function measure(module, op, size, bytes, fn) {
    if (only && !module.includes(only)) return;
    let one;
    try {
        const t = now(); fn(); one = now() - t;         // first call: also the block time of one call
    } catch (e) { skipped++; print(`probe-skip ${module} ${op} ${size}: ${String(e && e.message).slice(0, 70)}`); return; }
    let reps = Math.max(2, Math.min(20000, Math.floor(120 / Math.max(one, 0.001))));
    reps -= reps % 2;
    std.gc();
    const m0 = rusage().maxrss;
    let t0 = now(), best = Infinity;
    for (let i = 0; i < reps / 2; i++) { const a = now(); fn(); const d = now() - a; if (d < best) best = d; }
    const m1 = rusage().maxrss;
    for (let i = 0; i < reps / 2; i++) { const a = now(); fn(); const d = now() - a; if (d < best) best = d; }
    const total = now() - t0;
    std.gc();
    const m2 = rusage().maxrss;
    const nsop = total * 1e6 / reps;
    const mbs = bytes ? (bytes / 1e6) / (total / 1000 / reps) : 0;
    const kib = Math.max(0, (m2 - m1)) / 1024 / (reps / 2);
    rows.push({ module, op, size, nsop, mbs, block: best, kib });
}

const KB = 1024, MB = 1024 * 1024;
function bytesOf(n, seed = 7) { const u = new Uint8Array(n); let s = seed; for (let i = 0; i < n; i++) { s = (s * 1103515245 + 12345) & 0x7fffffff; u[i] = (s >> 16) & 0xff; } return u; }
function textOf(n) { const w = ["alpha", "beta", "gamma", "delta", "lorem", "ipsum", "dolor", "sit", "amet", "42", "3.14"]; let s = ""; let i = 0; while (s.length < n) s += w[i++ % w.length] + (i % 9 ? " " : "\n"); return s.slice(0, n); }

const hash = await mod("hash"), crypto = await mod("crypto"), compress = await mod("compress"), encoding = await mod("encoding");
const csv = await mod("csv"), yaml = await mod("yaml"), xml = await mod("xml"), html = await mod("html"), serialize = await mod("serialize");
const mathx = await mod("mathx"), simd = await mod("simd"), matcher = await mod("matcher"), url = await mod("url"), uuid = await mod("uuid");
const validate = await mod("validate"), decimal = await mod("decimal"), semver = await mod("semver"), time = await mod("time");
const structures = await mod("structures"), dataframe = await mod("dataframe"), bytesm = await mod("bytes"), config = await mod("config");
const random = await mod("random"), schema = await mod("schema"), file = await mod("file"), jsonm = await mod("json");

for (const n of [64, 64 * KB, 16 * MB]) {
    const b = bytesOf(n), tag = n >= MB ? (n / MB) + "MiB" : n >= KB ? (n / KB) + "KiB" : n + "B";
    if (hash) {
        measure("hash", "SHA256", tag, n, () => hash.SHA256(b));
        measure("hash", "BLAKE3", tag, n, () => hash.BLAKE3(b));
        measure("hash", "XXH3_64", tag, n, () => hash.XXH3_64(b));
        measure("hash", "CRC32C", tag, n, () => hash.CRC32C(b));
    }
    if (encoding) {
        measure("encoding", "Base64Encode", tag, n, () => encoding.Base64Encode(b));
        measure("encoding", "HexEncode", tag, n, () => encoding.HexEncode(b));
    }
}
for (const n of [4 * KB, 1 * MB, 8 * MB]) {
    const t = new TextEncoder().encode(textOf(n)), tag = n >= MB ? (n / MB) + "MiB" : (n / KB) + "KiB";
    if (compress) {
        const gz = compress.gzip(t), zs = compress.zstd(t);
        measure("compress", "gzip", tag, n, () => compress.gzip(t));
        measure("compress", "gunzip", tag, n, () => compress.gunzip(gz));
        measure("compress", "zstd", tag, n, () => compress.zstd(t));
        measure("compress", "unzstd", tag, n, () => compress.unzstd(zs));
        measure("compress", "brotli", tag, n, () => compress.brotli(t));
        measure("compress", "snappy", tag, n, () => compress.snappy(t));
        measure("compress", "lz4Compress", tag, n, () => compress.lz4Compress(t));
    }
}
if (crypto) {
    const salt = bytesOf(16);
    measure("crypto", "Bcrypt.hash", "cost10", 0, () => crypto.Bcrypt.hash("correct horse", 10));
    measure("crypto", "Bcrypt.hash", "cost12", 0, () => crypto.Bcrypt.hash("correct horse", 12));
    measure("crypto", "Argon2id.hash", "64MiB,t3", 0, () => crypto.Argon2id.hash("pw", salt));
    measure("crypto", "PBKDF2", "600k", 0, () => crypto.PBKDF2({ password: "pw", salt, iterations: 600000, length: 32 }));
    if (crypto.Scrypt) measure("crypto", "Scrypt", "N=32768", 0, () => crypto.Scrypt("pw", salt, { N: 32768 }));
    measure("crypto", "HMAC", "64KiB", 64 * KB, () => crypto.HMAC("sha256", "key", bytesOf(64 * KB)));
    measure("crypto", "Ed25519Sign", "64B", 0, (() => { const k = crypto.Ed25519Generate(); const m = bytesOf(64); return () => crypto.Ed25519Sign(k.privateKey || k.secretKey || k.priv, m); })());
    measure("crypto", "RandomBytes", "4KiB", 4 * KB, () => crypto.RandomBytes(4096));
}
for (const rowsN of [200, 20000, 200000]) {
    const recs = []; for (let i = 0; i < rowsN; i++) recs.push({ id: i, name: "user" + i, score: i * 1.5, tags: ["a", "b"], ok: i % 2 === 0 });
    const js = JSON.stringify(recs), tag = rowsN + "rows";
    measure("json(core)", "JSON.parse", tag, js.length, () => JSON.parse(js));
    measure("json(core)", "JSON.stringify", tag, js.length, () => JSON.stringify(recs));
    if (serialize) {
        const mp = serialize.MsgPackEncode(recs), cb = serialize.CBOREncode(recs);
        measure("serialize", "MsgPackEncode", tag, mp.length, () => serialize.MsgPackEncode(recs));
        measure("serialize", "MsgPackDecode", tag, mp.length, () => serialize.MsgPackDecode(mp));
        measure("serialize", "CBOREncode", tag, cb.length, () => serialize.CBOREncode(recs));
        measure("serialize", "structuredClone", tag, js.length, () => serialize.structuredClone(recs));
    }
    if (csv) {
        let text = "id,name,score,ok\n"; for (let i = 0; i < rowsN; i++) text += `${i},user${i},${i * 1.5},${i % 2 === 0}\n`;
        measure("csv", "parse", tag, text.length, () => csv.parse(text));
    }
    if (yaml && rowsN <= 20000) {
        let y = ""; for (let i = 0; i < rowsN; i++) y += `- id: ${i}\n  name: user${i}\n  score: ${i * 1.5}\n`;
        measure("yaml", "Parse", tag, y.length, () => yaml.Parse(y));
    }
    if (xml && rowsN <= 20000) {
        let x = "<r>"; for (let i = 0; i < rowsN; i++) x += `<u id="${i}"><n>user${i}</n><s>${i * 1.5}</s></u>`; x += "</r>";
        measure("xml", "XMLParse", tag, x.length, () => xml.XMLParse(x));
    }
    if (html && rowsN <= 20000) {
        let h = "<html><body><ul>"; for (let i = 0; i < rowsN; i++) h += `<li class="u"><a href="/u/${i}">user${i}</a></li>`; h += "</ul></body></html>";
        measure("html", "HTMLParse", tag, h.length, () => html.HTMLParse(h));
    }
    if (dataframe) {
        const id = new Int32Array(rowsN), sc = new Float64Array(rowsN), grp = [];
        for (let i = 0; i < rowsN; i++) { id[i] = (i * 7919) % rowsN; sc[i] = Math.sin(i) * 100; grp.push("g" + (i % 50)); }
        let df;
        measure("dataframe", "new DataFrame", tag, rowsN * 16, () => { df = new dataframe.DataFrame({ id, sc, grp }); });
        if (df) {
            measure("dataframe", "SORT", tag, rowsN * 8, () => df.SORT("sc"));
            measure("dataframe", "ARG_SORT", tag, rowsN * 8, () => df.ARG_SORT("sc"));
            measure("dataframe", "MEDIAN", tag, rowsN * 8, () => df.MEDIAN("sc"));
            measure("dataframe", "QUANTILE", tag, rowsN * 8, () => df.QUANTILE("sc", 0.9));
            measure("dataframe", "GROUP_BY_SUM", tag, rowsN * 8, () => df.GROUP_BY_SUM("grp", "sc"));
            measure("dataframe", "GROUP_BY_MEAN", tag, rowsN * 8, () => df.GROUP_BY_MEAN("grp", "sc"));
            measure("dataframe", "SUM", tag, rowsN * 8, () => df.SUM("sc"));
            measure("dataframe", "UNIQUE", tag, rowsN * 4, () => df.UNIQUE("id"));
            measure("dataframe", "VALUE_COUNTS", tag, rowsN * 8, () => df.VALUE_COUNTS("grp"));
            measure("dataframe", "CUM_SUM", tag, rowsN * 8, () => df.CUM_SUM("sc"));
            measure("dataframe", "ROLLING_MEAN", tag, rowsN * 8, () => df.ROLLING_MEAN("sc", 16));
        }
    }
    if (structures) {
        measure("structures", "SortedMap set", tag, 0, () => { const m = new structures.SortedMap(); for (let i = 0; i < rowsN; i++) m.set((i * 7919) % rowsN, i); });
        measure("structures", "Heap push+pop", tag, 0, () => { const h = new structures.Heap(); for (let i = 0; i < rowsN; i++) h.push((i * 7919) % rowsN); for (let i = 0; i < rowsN; i++) h.pop(); });
        measure("structures", "LRU set+get", tag, 0, () => { const l = new structures.LRU(1024); for (let i = 0; i < rowsN; i++) { l.set("k" + (i % 4096), i); l.get("k" + ((i * 7) % 4096)); } });
    }
}
for (const n of [1024, 1 * MB]) {
    const a = new Float32Array(n), b = new Float32Array(n), d = new Float64Array(n), tag = n >= MB ? (n / MB) + "M" : n + "";
    for (let i = 0; i < n; i++) { a[i] = Math.sin(i); b[i] = Math.cos(i); d[i] = a[i]; }
    if (simd) {
        measure("simd", "dot", tag + " f32", n * 4, () => simd.dot(a, b));
        measure("simd", "sum", tag + " f32", n * 4, () => simd.sum(a));
        measure("simd", "relu", tag + " f32", n * 4, () => simd.relu(a));
        measure("simd", "f64Sum", tag + " f64", n * 8, () => simd.f64Sum(d));
    }
    if (mathx) {
        measure("mathx", "stats.mean", tag + " f64", n * 8, () => mathx.stats.mean(d));
        measure("mathx", "stats.median", tag + " f64", n * 8, () => mathx.stats.median(d));
        measure("mathx", "cumsum", tag + " f64", n * 8, () => mathx.cumsum(d));
    }
}
{
    const text = textOf(1 * MB);
    if (matcher) {
        const m = new matcher.Matcher("dolor sit amet 42");
        measure("matcher", "Matcher.countIn", "1MiB", MB, () => m.countIn(text));
        measure("matcher", "Levenshtein", "200ch", 0, () => matcher.Levenshtein(text.slice(0, 200), text.slice(100, 300)));
        measure("matcher", "DiffLines", "64KiB", 64 * KB, () => matcher.DiffLines(text.slice(0, 64 * KB), text.slice(100, 64 * KB + 100)));
    }
    measure("regexp(core)", "match all words", "1MiB", MB, () => { let c = 0; const re = /\b[a-z]+\b/g; while (re.exec(text)) c++; return c; });
    measure("string(core)", "split+join", "1MiB", MB, () => text.split(" ").join("-"));
    if (bytesm) {
        const u = new TextEncoder().encode(text);
        measure("bytes", "isValidUtf8", "1MiB", MB, () => bytesm.isValidUtf8(u));
        measure("bytes", "indexOf (miss)", "1MiB", MB, () => bytesm.indexOf(u, new TextEncoder().encode("zzzzqqqq")));
    }
    if (html) measure("html", "MarkdownToHTML", "64KiB", 64 * KB, () => html.MarkdownToHTML(text.slice(0, 64 * KB)));
}
if (url) {
    measure("url", "new URL", "1", 0, () => new url.URL("https://user:pw@example.com:8443/a/b/c?x=1&y=2#frag"));
    measure("url", "domainToASCII", "1", 0, () => url.domainToASCII("bücher.example"));
}
if (uuid) { measure("uuid", "v4", "1", 0, () => uuid.v4()); measure("uuid", "v7", "1", 0, () => uuid.v7()); }
if (validate) { measure("validate", "IsEmail", "1", 0, () => validate.IsEmail("someone@example.com")); measure("validate", "IsJSON", "4KiB", 4 * KB, (() => { const j = JSON.stringify({ a: "x".repeat(4000) }); return () => validate.IsJSON(j); })()); }
if (decimal) { const a = new decimal.Decimal("12345.6789"), b = new decimal.Decimal("0.000123"); measure("decimal", "mul", "1", 0, () => a.mul(b)); measure("decimal", "div", "1", 0, () => a.div(b)); measure("decimal", "sqrt", "1", 0, () => a.sqrt()); }
if (semver) measure("semver", "satisfies", "1", 0, () => semver.satisfies("1.2.3-beta.4", ">=1.2.0 <2.0.0 || ^3.1"));
if (time) { measure("time", "formatRFC3339", "1", 0, () => time.formatRFC3339(1700000000000)); measure("time", "parseRFC3339", "1", 0, () => time.parseRFC3339("2023-11-14T22:13:20.123Z")); }
if (random) { const r = new random.Random(42); measure("random", "nextFloat", "1", 0, () => r.nextFloat()); measure("random", "shuffle", "100k", 0, (() => { const a = Array.from({ length: 100000 }, (_, i) => i); return () => r.shuffle(a); })()); }
if (config) { let t = ""; for (let i = 0; i < 2000; i++) t += `[s${i}]\nkey = "v${i}"\nnum = ${i}\n`; measure("config", "TOML.parse", "2000 tables", t.length, () => config.TOML.parse(t)); }
if (file) {
    const dir = file.makeTempDir("probe");
    const p = new file.Path(String(dir) + "/blob.bin"), big = bytesOf(32 * MB);
    measure("file", "writeFile", "32MiB", 32 * MB, () => file.writeFile(p, big));
    measure("file", "readBytes", "32MiB", 32 * MB, () => file.readBytes(p));
    file.removeAll(dir);
}

const f = (x, w, d = 1) => (x >= 1000 ? x.toFixed(0) : x.toFixed(d)).padStart(w);
print(`${"module".padEnd(13)} ${"operation".padEnd(18)} ${"size".padEnd(12)} ${"ns/op".padStart(13)} ${"MB/s".padStart(9)} ${"block ms".padStart(9)} ${"KiB/op".padStart(8)}`);
for (const r of rows) {
    print(`${r.module.padEnd(13)} ${r.op.padEnd(18)} ${r.size.padEnd(12)} ${f(r.nsop, 13, 0)} ${r.mbs ? f(r.mbs, 9) : "-".padStart(9)} ${f(r.block, 9, 3)} ${f(r.kib, 8)}${r.block > BLOCK_BUDGET_MS ? "  BLOCKS" : ""}`);
    print(`probe ${r.module} ${r.op.replace(/ /g, "_")} ${r.size.replace(/ /g, "_")} ${r.nsop.toFixed(0)} ${r.mbs.toFixed(1)} ${r.block.toFixed(3)} ${r.kib.toFixed(1)}`);
}
const blockers = rows.filter((r) => r.block > BLOCK_BUDGET_MS).sort((a, b) => b.block - a.block);
print(`\n${rows.length} operations probed, ${skipped} skipped (see probe-skip lines); ${blockers.length} hold the loop over ${BLOCK_BUDGET_MS} ms per call:`);
for (const r of blockers.slice(0, 25)) print(`  ${r.block.toFixed(1).padStart(8)} ms  ${r.module}.${r.op} @ ${r.size}`);
