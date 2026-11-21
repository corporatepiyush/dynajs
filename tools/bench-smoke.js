// flags: --std
// tools/bench-smoke.js [selftest] [FILE...] -- every tests/**/bench_*.js runs for a short budget and must not throw.
// Benches sit in no suite table, so an API rename rots them silently: bench_ml_trees.js passed the old option
// nTrees (renamed nEstimators) and bench_idna.js fed domainToASCII a name it rejects, both unnoticed for weeks
// (F-bench-1, F-bench-2). Each bench runs with --std plus its `// flags:` header for BENCH_SMOKE_MS (default 3000);
// a bench FAILS when it exits on its own with a nonzero status after printing an uncaught exception with its stack,
// or dies of a signal. Still running at the budget is a pass (it got past setup and its first measured calls
// without throwing); a deliberate refusal (exit 2 under host load, no stack) is a pass. Logs land in
// .agent-work/bench-smoke/<path>.log. BENCH_SMOKE_P sets the parallelism (default 8), BENCH_SMOKE_BIN the binary.
//
//   ./dynajs --std tools/bench-smoke.js selftest   # planted benches: the two throwers fail, the three near misses pass
//   ./dynajs --std tools/bench-smoke.js            # every tracked + untracked-unignored tests/**/bench_*.js
import * as std from "std";
import * as sys from "dyna:sys";
import * as file from "dyna:file";
import { monotonicNano } from "dyna:time";

const { Path } = file;
const SELF = new Path(scriptArgs[0]);
const ROOT = (SELF.isAbsolute ? SELF : Path.cwd().join(scriptArgs[0])).toString().replace(/\/tools\/bench-smoke\.js$/, "");
sys.chDir(ROOT);
const LOGS = ".agent-work/bench-smoke";
const BUDGET = Number(sys.getEnv("BENCH_SMOKE_MS") || 3000);
const PAR = Number(sys.getEnv("BENCH_SMOKE_P") || 8);
const BIN = sys.getEnv("BENCH_SMOKE_BIN") || "./dynajs";
const STACK = /^(?:Possibly unhandled promise rejection: )?[A-Za-z]*Error\b[^\n]*\n\s+at /m;

async function drain(pipe) {
    const parts = [], buf = new Uint8Array(65536);
    for (;;) { const n = await pipe.read(buf); if (n <= 0) break; parts.push(buf.slice(0, n)); }
    const all = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    parts.reduce((o, p) => (all.set(p, o), o + p.length), 0);
    return new TextDecoder().decode(all);
}

function headerFlags(p) {
    const head = (std.loadFile(p) || "").split("\n").slice(0, 3);
    for (const line of head) {
        const m = /^\/\/\s*flags:(.*)$/.exec(line);
        if (m) return m[1].trim().split(/\s+/).filter((f) => f && f !== "--std");
    }
    return [];
}

function verdict(r, all) {
    if (r.timedOut) return { fail: false, why: "running at the budget" };
    if (r.code === null) return { fail: true, why: "killed by " + r.signal };
    if (r.code !== 0 && STACK.test(all)) return { fail: true, why: "threw: " + (STACK.exec(all)[0].split("\n")[0]) };
    return { fail: false, why: "exit " + r.code };
}

async function runOne(p, logDir) {
    const t0 = monotonicNano();
    const c = new sys.Spawn(BIN, ["--std", ...headerFlags(p), p], { stdin: "ignore", timeoutMs: BUDGET });
    const [out, err] = await Promise.all([drain(c.stdout), drain(c.stderr)]);
    const r = await c.wait();
    c.close();
    const all = out + err;
    const v = verdict(r, all);
    const ms = Number(monotonicNano() - t0) / 1e6;
    const log = logDir + "/" + p.replace(/\//g, "_") + ".log";
    file.writeFile(new Path(log), `${p}\ncode=${r.code} signal=${r.signal} timedOut=${r.timedOut} ms=${ms.toFixed(0)}\n${v.why}\n----\n${all}`);
    return { p, ms, ...v };
}

async function runAll(list, logDir) {
    file.makeDir(new Path(logDir), { recursive: true });
    const results = [];
    let next = 0;
    const worker = async () => { while (next < list.length) { const p = list[next++]; results.push(await runOne(p, logDir)); } };
    await Promise.all(Array.from({ length: Math.min(PAR, list.length) }, worker));
    return results.sort((a, b) => (a.p < b.p ? -1 : 1));
}

async function listBenches() {
    const g = new sys.Spawn("git", ["ls-files", "--cached", "--others", "--exclude-standard", "tests"], { stdin: "ignore" });
    const out = await drain(g.stdout);
    await g.wait();
    g.close();
    return out.split("\n").filter((p) => /(^|\/)bench_[^/]*\.js$/.test(p) && file.exists(new Path(p)));
}

async function selftest() {
    const dir = ROOT + "/" + LOGS + "/selftest";
    if (file.exists(new Path(dir))) file.removeAll(new Path(dir));
    file.makeDir(new Path(dir), { recursive: true });
    const plants = {
        "bench_fire_throw.js": { fail: true, src: 'const o = { nTrees: 1 };\nif (o.nEstimators === undefined) throw new TypeError("unknown option \\"nTrees\\"");\n' },
        "bench_fire_reject.js": { fail: true, src: 'async function run() { throw new RangeError("domainToASCII: validation error"); }\nawait run();\n' },
        "bench_miss_slow.js": { fail: false, src: 'print("TypeError: printed as data, not thrown");\nlet x = 0;\nfor (;;) x++;\n' },
        "bench_miss_refuse.js": { fail: false, src: 'import * as std from "std";\nprint("host load 99 >= 12: not measuring");\nstd.exit(2);\n' },
        "bench_miss_clean.js": { fail: false, src: 'print("#DATA\\tok\\t1.0");\n' },
    };
    for (const [n, pl] of Object.entries(plants)) file.writeFile(new Path(dir + "/" + n), pl.src);
    const res = await runAll(Object.keys(plants).map((n) => dir + "/" + n), dir + "/logs");
    let bad = 0;
    for (const r of res) {
        const want = plants[r.p.slice(dir.length + 1)].fail;
        if (r.fail !== want) { bad++; print(`selftest: ${r.p.slice(dir.length + 1)} ${want ? "did NOT fail" : "failed on a near miss"} (${r.why})`); }
    }
    file.removeAll(new Path(dir));
    print(bad ? `bench-smoke selftest: ${bad} problem(s) in ${res.length} plants` : `bench-smoke selftest: ${res.length} plants, the 2 throwers fail and the 3 near misses pass`);
    return bad === 0;
}

const args = scriptArgs.slice(1);
if (args[0] === "selftest") {
    std.exit((await selftest()) ? 0 : 1);
}
const t0 = monotonicNano();
const list = args.length ? args : await listBenches();
const res = await runAll(list, LOGS);
const failed = res.filter((r) => r.fail);
for (const r of res) print(`${r.fail ? "FAIL" : "ok  "} ${(r.ms / 1000).toFixed(1).padStart(5)}s ${r.p}  ${r.why}`);
print(`bench-smoke: ${res.length} benches, ${failed.length} failed, budget ${BUDGET} ms, P=${PAR}, wall ${(Number(monotonicNano() - t0) / 1e9).toFixed(1)}s`);
std.exit(failed.length ? 1 : 0);
