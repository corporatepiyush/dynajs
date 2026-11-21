// tools/check-apps.js [DIR] [path/to/dynajs] -- run every example program (DIR/<area>/<name>.js, DIR defaults to
// examples, the binary to ./dynajs) and report which fail. The port of tools/check-apps.sh, which was the
// examples leg of the old check-api target of build.sh; run it as `./dynajs --std tools/check-apps.js` (or .obj-san/dynajs as the
// binary under test: `./dynajs --std tools/check-apps.js examples .obj-san/dynajs`).
//
// Each example is a complete program that drives itself when run without arguments (a server starts on port 0,
// calls itself and shuts down; a tool builds sample input in a temp directory and checks its own report), so
// "it exits 0" is the contract. The top level of DIR holds the embedding fixtures; only the area directories hold
// examples. A file whose first line contains "apps:skip" needs an external service and is counted as skipped, never
// silently dropped. A "// flags: ..." line within the first three lines passes those interpreter flags (e.g. --std
// for os.Worker). "apps:solo" in the first three lines: the example measures its own timing and runs alone after the
// pool drains. "apps:needs <module> ...": it uses an opt-in module (dyna:ui, dyna:sdl, dyna:raylib); a binary built
// without it counts the example as skipped and names the module.
// Every run has a wall-clock bound (CHECK_WALL_LIMIT s, default 30: SIGTERM, SIGKILL 2 s later) and a CPU-time bound
// (CHECK_CPU_LIMIT s, default 30, via ulimit -t); hitting either is reported as TIMEOUT, distinct from FAIL, and a
// crash names its signal. CHECK_APPS_PAR examples run at once (default: the CPU thread count); the report is printed
// in file order, so it reads the same whatever finished first. Logs: .obj/apps-check/<area>_<name>.out.
// Exit: 0 all pass, 1 any FAIL/TIMEOUT, 2 usage error or no examples found.
import * as std from "std";
import * as sys from "dyna:sys";
import * as file from "dyna:file";
import { monotonicNano } from "dyna:time";

const { Path } = file;
const DIR = (scriptArgs[1] || "examples").replace(/\/+$/, "");
let BIN = scriptArgs[2] || "./dynajs";
const WALL = Number(sys.getEnv("CHECK_WALL_LIMIT") || 30);
const CPU = Number(sys.getEnv("CHECK_CPU_LIMIT") || 30);
const parEnv = sys.getEnv("CHECK_APPS_PAR");
const PAR = parEnv ? Number(parEnv) : sys.cpuInfo().threads || 4;
const OUT = ".obj/apps-check";

function die(msg) { std.err.puts(msg + "\n"); std.exit(2); }
if (!Number.isInteger(PAR) || PAR < 1) die(`CHECK_APPS_PAR must be a positive integer (got '${parEnv}')`);
if (!file.exists(new Path(DIR)) || !file.stat(new Path(DIR)).isDir) die(`no such directory: ${DIR}`);
if (!file.exists(new Path(BIN))) die(`not executable: ${BIN}`);
if (!BIN.startsWith("/")) BIN = Path.cwd().join(BIN).toString();
file.makeDir(new Path(OUT), { recursive: true });

async function drain(pipe) {
    const parts = [], buf = new Uint8Array(65536);
    for (;;) { const n = await pipe.read(buf); if (n <= 0) break; parts.push(buf.slice(0, n)); }
    const all = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    parts.reduce((o, p) => (all.set(p, o), o + p.length), 0);
    return new TextDecoder().decode(all);
}
async function run(argv, timeoutMs) {
    const p = new sys.Spawn(argv[0], argv.slice(1), { stdin: "ignore", ...(timeoutMs ? { timeoutMs } : {}) });
    const [out, err] = await Promise.all([drain(p.stdout), drain(p.stderr)]);
    const r = await p.wait();
    p.close();
    return { ...r, all: out + err };
}

const head3 = (f) => (std.loadFile(f) || "").split("\n").slice(0, 3);
const examples = file.readDir(new Path(DIR)).filter((d) => d.isDir).map((d) => d.name).sort()
    .flatMap((area) => file.readDir(new Path(`${DIR}/${area}`)).filter((e) => e.isFile && e.name.endsWith(".js"))
        .map((e) => e.name).sort().map((n) => ({ f: `${DIR}/${area}/${n}`, name: `${area}/${n.slice(0, -3)}` })));
if (examples.length === 0) die(`no examples found under ${DIR}/*/`);

const moduleCache = new Map();
async function hasModule(m) {
    if (!moduleCache.has(m)) moduleCache.set(m, run([BIN, "-e", `import('${m}').catch(() => { throw new Error('no ${m}'); })`], 30000).then((r) => r.code === 0));
    return moduleCache.get(m);
}

async function runApp(ex) {
    const h = head3(ex.f);
    const flags = (h.map((l) => /^\/\/ flags: (.*)$/.exec(l)).find(Boolean) || [, ""])[1].split(/\s+/).filter(Boolean);
    const t0 = monotonicNano();
    const r = await run(["bash", "-c", `ulimit -t ${CPU} 2>/dev/null; exec "$0" "$@"`, BIN, ...flags, ex.f], WALL * 1000);
    ex.secs = Number(monotonicNano() - t0) / 1e9;
    ex.log = `${OUT}/${ex.name.replace(/\//g, "_")}.out`;
    writeLog(ex.log, r.all);
    if (r.timedOut) ex.verdict = "TIMEOUT", ex.why = `killed after ${WALL}s wall`;
    else if (r.signal === "SIGXCPU" || r.signal === "SIGKILL") ex.verdict = "TIMEOUT", ex.why = `${r.signal}: over the ${CPU}s CPU bound`;
    else if (r.signal) ex.verdict = "FAIL", ex.why = `killed by ${r.signal}`;
    else if (r.code !== 0) ex.verdict = "FAIL", ex.why = `exit ${r.code}`;
    else ex.verdict = "ok";
    ex.tail = r.all.split("\n").filter((l) => l !== "").slice(-6);
}
function writeLog(path, text) {
    const fh = std.open(path, "w");
    if (!fh) throw new Error(`cannot write ${path}`);
    fh.puts(text);
    fh.close();
}

const T0 = monotonicNano();
const pool = [], solo = [];
for (const ex of examples) {
    const h = head3(ex.f);
    if (h[0].includes("apps:skip")) { ex.verdict = "skip"; continue; }
    const needs = h.map((l) => /apps:needs (.*)$/.exec(l)).filter(Boolean).flatMap((m) => m[1].trim().split(/\s+/));
    const missing = [];
    for (const m of needs) if (!(await hasModule(m))) missing.push(m);
    if (missing.length) { ex.verdict = "skip"; ex.why = `needs ${missing.join(" ")}, not in this build`; continue; }
    (h.some((l) => l.includes("apps:solo")) ? solo : pool).push(ex);
}
let next = 0;
await Promise.all(Array.from({ length: Math.min(PAR, pool.length) }, async () => {
    while (next < pool.length) await runApp(pool[next++]);
}));
for (const ex of solo) await runApp(ex);

let count = 0, fail = 0, tmo = 0, skip = 0;
for (const ex of examples) {
    if (ex.verdict === "skip") { skip++; std.out.puts(`skip    ${ex.name}${ex.why ? ` (${ex.why})` : ""}\n`); continue; }
    count++;
    const t = ex.secs.toFixed(1).padStart(5) + "s";
    if (ex.verdict === "ok") { std.out.puts(`ok      ${t} ${ex.name}\n`); continue; }
    fail++;
    if (ex.verdict === "TIMEOUT") tmo++;
    std.out.puts(`${ex.verdict.padEnd(7)} ${t} ${ex.name} (${ex.why}; log ${ex.log})\n`);
    for (const l of ex.tail) std.out.puts(`         ${l}\n`);
}
std.out.puts(`\n${DIR}: ${count} examples run, ${fail} failed (${tmo} of them timeouts), ${skip} skipped (apps:skip or apps:needs); ` +
    `${(Number(monotonicNano() - T0) / 1e9).toFixed(1)}s wall on ${PAR} workers\n`);
std.exit(fail === 0 ? 0 : 1);
