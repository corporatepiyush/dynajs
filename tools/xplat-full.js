// tools/xplat-full.js [--wall SECONDS] [SUITE...] -- run the whole suite AND every example on whatever platform
// this is (the port of tools/xplat-full.sh; tools/xplat-verify.sh runs it inside the amd64 container under
// DYNA_COMPAT_ONLY=1). It first builds the native-module fixtures (tests/ctest/run.sh so), then three legs, each
// reported as ok / FAIL / TIMEOUT / CRASH with its wall time:
//   suites    every suite tests/suites.sh lists (or the SUITE arguments), through tools/run-tests-parallel.sh with
//             DEV_SUITE_TIMEOUT=SECONDS as the per-suite bound (CPU bound: half of it)
//   examples  tools/check-apps.js over examples/<area>/*.js, CHECK_WALL_LIMIT=SECONDS, CHECK_CPU_LIMIT=SECONDS+10
//   embed     the top-level examples/*.js one at a time under the same two bounds; hello_module.js is a module
//             the others import, and test_fib.js / test_point.js need examples/point.so (shared-library builds)
// Two bounds per case, because a spin burns CPU (ulimit -t catches it) and a blocked socket burns none (only the
// wall clock catches it); a timeout is reported distinctly from a failure. SECONDS defaults to 60.
// DYNAJS=<bin> picks the engine (default ./dynajs). Exit 0 only when every leg is ok.
import * as std from "std";
import * as sys from "dyna:sys";
import * as file from "dyna:file";
import { monotonicNano } from "dyna:time";

const { Path } = file;
const argv = scriptArgs.slice(1);
let WALL = 60;
const only = [];
for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--wall" && i + 1 < argv.length) WALL = Number(argv[++i]);
    else if (argv[i].startsWith("--")) { std.err.puts(`xplat-full: unknown option ${argv[i]}\n`); std.exit(2); }
    else only.push(argv[i]);
}
if (!Number.isInteger(WALL) || WALL < 1) { std.err.puts("xplat-full: --wall needs a positive integer\n"); std.exit(2); }
const CPU = WALL + 10;
const BIN = sys.getEnv("DYNAJS") || "./dynajs";
if (!file.exists(new Path(BIN))) { std.err.puts(`xplat-full: FAIL: no ${BIN} -- build first (./build.sh release)\n`); std.exit(1); }

async function drain(pipe) {
    const parts = [], buf = new Uint8Array(65536);
    for (;;) { const n = await pipe.read(buf); if (n <= 0) break; parts.push(buf.slice(0, n)); }
    const all = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    parts.reduce((o, p) => (all.set(p, o), o + p.length), 0);
    return new TextDecoder().decode(all);
}
async function run(cmd, env, timeoutMs) {
    const t0 = monotonicNano();
    const p = new sys.Spawn(cmd[0], cmd.slice(1), { stdin: "ignore", env, extendEnv: true, ...(timeoutMs ? { timeoutMs } : {}) });
    const [out, err] = await Promise.all([drain(p.stdout), drain(p.stderr)]);
    const r = await p.wait();
    p.close();
    return { ...r, all: out + err, secs: Number(monotonicNano() - t0) / 1e9 };
}
const bounded = (args) => ["bash", "-c", `ulimit -t ${CPU} 2>/dev/null; exec "$0" "$@"`, ...args];

const tally = { ok: 0, FAIL: 0, TIMEOUT: 0, CRASH: 0 };
const bad = [];
function report(label, r, tailLines) {
    let v = "ok", why = "";
    if (r.timedOut || r.signal === "SIGXCPU") v = "TIMEOUT", why = r.timedOut ? `>${Math.round(r.secs)}s wall` : `over the ${CPU}s CPU bound`;
    else if (r.signal) v = "CRASH", why = r.signal;
    else if (r.code !== 0) v = "FAIL", why = `exit ${r.code}`;
    tally[v]++;
    std.out.puts(`  ${v.padEnd(7)} ${r.secs.toFixed(1).padStart(7)}s  ${label}${why ? ` (${why})` : ""}\n`);
    if (v !== "ok") {
        bad.push(`${v} ${label}`);
        for (const l of r.all.split("\n").filter((x) => x.trim() !== "").slice(-tailLines)) std.out.puts(`            ${l}\n`);
    }
}

const OS = `${sys.platform()}-${sys.arch()}`;
const T0 = monotonicNano();
std.out.puts(`=== xplat-full on ${OS} (wall ${WALL}s, cpu ${CPU}s per case) ===\n`);

let suites = only;
if (!suites.length) {
    const l = await run(["bash", "tests/suites.sh"]);
    if (l.code !== 0) { std.out.puts("xplat-full: FAIL: tests/suites.sh could not list the suites\n"); std.exit(1); }
    suites = l.all.split("\n").filter((x) => x.endsWith(".js"));
}
std.out.puts("--- native fixtures (tests/ctest/run.sh so: tests/bjson.so, tests/park_sweep.so for this platform) ---\n");
report("fixtures", await run(["bash", "tests/ctest/run.sh", "so"], undefined, WALL * 1000), 10);
std.out.puts(`--- suites (${suites.length}, tools/run-tests-parallel.sh) ---\n`);
report(`suites x${suites.length}`, await run(["bash", "tools/run-tests-parallel.sh", ...suites], { DYNAJS: BIN, DEV_SUITE_TIMEOUT: String(WALL) }), 25);

std.out.puts("--- examples (tools/check-apps.js) ---\n");
report("examples/<area>/*.js", await run([BIN, "--std", "tools/check-apps.js", "examples", BIN],
    { CHECK_WALL_LIMIT: String(WALL), CHECK_CPU_LIMIT: String(CPU) }), 25);

std.out.puts("--- embedding examples (examples/*.js) ---\n");
const hasSo = file.exists(new Path("examples/point.so"));
for (const name of file.readDir(new Path("examples")).filter((e) => e.isFile && e.name.endsWith(".js")).map((e) => e.name).sort()) {
    if (name === "hello_module.js") continue;
    if ((name === "test_fib.js" || name === "test_point.js") && !hasSo) {
        std.out.puts(`  SKIP              examples/${name} (no examples/point.so: not a shared-library build)\n`);
        continue;
    }
    report(`examples/${name}`, await run(bounded([BIN, `examples/${name}`]), undefined, WALL * 1000), 3);
}

std.out.puts(`\n=== ${OS} SUMMARY: ${tally.ok} ok, ${tally.FAIL} failed, ${tally.TIMEOUT} timed out, ${tally.CRASH} crashed; ` +
    `${(Number(monotonicNano() - T0) / 1e9).toFixed(1)}s wall ===\n`);
for (const b of bad) std.out.puts(`  ${b}\n`);
std.exit(bad.length ? 1 : 0);
