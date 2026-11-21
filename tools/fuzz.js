// tools/fuzz.js [--secs N] [--only a,b] [--jobs J] [--build] [--fresh] -- runs the libFuzzer targets in .obj-fuzz/bin,
// J at a time (default: CPU count), each with its own corpus .agent-work/fuzz/corpus/<t> and log .agent-work/fuzz/logs/<t>.log.
// Resumable: .agent-work/fuzz/ledger.tsv (date, target, secs, rc, cov, ft, corp, execs, exec/s, rss, crash, why, key) skips a target
// whose last row is clean for the same key (sha of the target binary + budget); --fresh or FUZZ_FRESH=1 reruns all.
// Rebuilds (./build.sh fuzz) when tests/lib/tree-hash.sh of src differs from .obj-fuzz/tree.stamp, so stale binaries never count as cached.
import * as std from "std";
import * as sys from "dyna:sys";
import { Exec } from "dyna:sys";

const args = scriptArgs.slice(1);
const opt = (k, d) => {
    const i = args.indexOf(k);
    return i >= 0 && i + 1 < args.length ? args[i + 1] : d;
};
const secs = +(opt("--secs", std.getenv("FUZZ_SECS") ?? "20"));
const only = opt("--only", "").split(",").filter(Boolean);
const fresh = args.includes("--fresh") || !!std.getenv("FUZZ_FRESH");
const bin = ".obj-fuzz/bin";
const work = ".agent-work/fuzz";
const sh = (c) => Exec("bash", ["-c", c], { maxBuffer: 256 << 20 });
const ncpu = +String(sh("sysctl -n hw.ncpu 2>/dev/null || nproc").stdout).trim() || 4;
const jobs = Math.max(1, +(opt("--jobs", std.getenv("FUZZ_JOBS") ?? String(ncpu))));
const ts = () => new Date().toTimeString().slice(0, 8);
const DYNA_LOG = std.getenv("DYNA_LOG") || ".agent-work/test.log";
const dlog = (...f) => { const d = new Date(), p = (n, w = 2) => String(n).padStart(w, "0"); try { const fd = std.open(DYNA_LOG, "a"); fd.puts(`${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}\tfuzz\t` + f.join("\t") + "\n"); fd.close(); } catch (e) {} };
const LIMITS = { default: { rss: 2048, malloc: 64 }, fuzz_dataframe: { rss: 2048, malloc: 256 }, fuzz_parsers: { rss: 2048, malloc: 256 } };

const srcKey = String(sh("bash tests/lib/tree-hash.sh src build.sh").stdout).trim();
const stamp = std.loadFile(".obj-fuzz/tree.stamp")?.trim();
if (stamp !== srcKey) print(`fuzz: sources changed since the last fuzz build (${stamp ?? "none"} -> ${srcKey}): rebuilding`);
if (args.includes("--build") || stamp !== srcKey || sh(`ls ${bin}`).code !== 0) {
    const t = Date.now();
    const b = sh("set -o pipefail; ./build.sh fuzz 2>&1 | grep -v '^  CC'");
    print(String(b.stdout).trim().split("\n").filter((l) => l.startsWith("build:")).join("\n"), `(${((Date.now() - t) / 1000).toFixed(0)}s)`);
    if (b.code === 0) { const fd = std.open(".obj-fuzz/tree.stamp", "w"); fd.puts(srcKey + "\n"); fd.close(); }
}

const names = String(sh(`ls ${bin} 2>/dev/null`).stdout).trim().split("\n").filter((n) => n && (!only.length || only.some((o) => n.includes(o))));
sh(`mkdir -p ${work}/corpus ${work}/artifacts ${work}/logs`);
const LEDGER = `${work}/ledger.tsv`;
const keyOf = (n) => String(sh(`shasum ${bin}/${n} | cut -c1-16`).stdout).trim() + `/${secs}`;
const last = new Map();
for (const l of (std.loadFile(LEDGER) ?? "").split("\n")) { const r = l.split("\t"); if (r.length >= 13) last.set(r[1], r); }
const todo = [], rows = [];
for (const n of names) {
    const key = keyOf(n), r = last.get(n);
    if (!fresh && r && r[12] === key && r[10] === "0") print(`${ts()} ${n.padEnd(26)} CACHE clean on key ${key} (--fresh reruns)`);
    else todo.push({ n, key });
}
print(`fuzz: ${names.length} targets, ${names.length - todo.length} cached, ${todo.length} to run, ${jobs} at a time, ${secs}s budget each`);

async function drain(pipe) {
    const buf = new Uint8Array(65536);
    for (;;) { const n = await pipe.read(buf); if (n <= 0) break; }
}
async function runOne({ n, key }) {
    const short = n.replace(/^fuzz_/, "");
    const corpus = `${work}/corpus/${n}`;
    const seeds = sh(`ls -d src/fuzz/corpus_${short} 2>/dev/null`).stdout.trim();
    const dict = sh(`ls src/fuzz/${n}.dict 2>/dev/null || ls src/fuzz/fuzz.dict 2>/dev/null`).stdout.trim().split("\n")[0];
    sh(`mkdir -p ${corpus}`);
    const log = `${work}/logs/${n}.log`;
    const lim = { ...LIMITS.default, ...(LIMITS[n] ?? {}) };
    const cmd = [`${bin}/${n}`, `-max_total_time=${secs}`, "-timeout=10", `-rss_limit_mb=${lim.rss}`, `-malloc_limit_mb=${lim.malloc}`, "-print_final_stats=1",
        `-artifact_prefix=${work}/artifacts/${n}-`, ...(dict ? [`-dict=${dict}`] : []), corpus, ...(seeds ? [seeds] : [])];
    print(`${ts()} START ${n}`); dlog("START", n);
    const t = Date.now();
    const p = new sys.Spawn("bash", ["-c", `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ${cmd.join(" ")} > ${log} 2>&1`], { stdin: "ignore" });
    await Promise.all([drain(p.stdout), drain(p.stderr)]);
    const rc = (await p.wait()).code ?? -1;
    p.close();
    const txt = std.loadFile(log) ?? "";
    const lastLine = txt.split("\n").filter((l) => /^#\d+/.test(l)).pop() ?? "";
    const num = (re, s = lastLine) => +((s.match(re) ?? [])[1] ?? 0);
    const crash = rc !== 0 || /==ERROR:|deadly signal|runtime error:|SUMMARY: /.test(txt);
    const why = crash ? ((txt.match(/(ERROR: [^\n]{0,90}|runtime error: [^\n]{0,90}|deadly signal|timeout after \d+ seconds|out-of-memory[^\n]{0,40})/) ?? ["exit " + rc])[1]) : "";
    const row = { n, secs: ((Date.now() - t) / 1000).toFixed(0), rc, cov: num(/cov: (\d+)/), ft: num(/ft: (\d+)/), corp: num(/corp: (\d+)/),
        execs: num(/stat::number_of_executed_units: (\d+)/, txt), eps: num(/exec\/s: (\d+)/), rss: num(/stat::peak_rss_mb: (\d+)/, txt), crash, why };
    rows.push(row);
    dlog("END", n, row.crash ? "CRASH " + row.why : "PASS", row.secs + "s", `cov ${row.cov} exec/s ${row.eps}`);
    print(`${ts()} ${n.padEnd(26)} ${row.crash ? "CRASH" : "ok   "} cov ${String(row.cov).padStart(6)} ft ${String(row.ft).padStart(6)} corp ${String(row.corp).padStart(5)} exec/s ${String(row.eps).padStart(7)} rss ${String(row.rss).padStart(5)}MB ${row.secs}s ${row.why}`);
    const fd = std.open(LEDGER, "a");
    fd.puts([new Date().toISOString(), n, secs, rc, row.cov, row.ft, row.corp, row.execs, row.eps, row.rss, row.crash ? 1 : 0, row.why.replace(/\t/g, " "), key].join("\t") + "\n");
    fd.close();
}

const T0 = Date.now();
let next = 0;
await Promise.all(Array.from({ length: Math.min(jobs, todo.length) }, async () => { while (next < todo.length) await runOne(todo[next++]); }));
const bad = rows.filter((r) => r.crash);
print(`${ts()} fuzz: ${rows.length} run, ${names.length - todo.length} cached, ${bad.length} crashed, ${secs}s budget each, ${jobs} jobs, ${((Date.now() - T0) / 1000).toFixed(0)}s wall; logs ${work}/logs, reproducers ${work}/artifacts`);
std.exit(bad.length ? 1 : 0);
