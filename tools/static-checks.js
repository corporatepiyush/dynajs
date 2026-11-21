// tools/static-checks.js [--only NAME,...] [--list] -- the static checks, run in parallel, each timed:
// codegraph (report >= 8000 functions, selfcheck, shape selftest, offload net, io_uring view), the defect
// scanner and its selftest, the dod_scan selftest, the no-comments and generated-file checks, every bench_*.js smoke-run
// at a tiny size (bench-smoke, so renamed options cannot rot them silently) with its selftest, and the hardening of
// ./dynajs (Mach-O: PIE, stack canary, fortify, PAC, BTI; ELF: RELRO+BIND_NOW, NX stack, canary, fortify).
// Logs land in .agent-work/static/<check>.log; exit status is the number of failed checks (capped at 1).
// Resumable: .agent-work/static/ledger.tsv (check, PASS|FAIL, key, secs, date) skips a check that passed on the same key
// (tests/lib/tree-hash.sh of src tools tests bench examples README.md dynajs.d.ts build.sh, plus the sha of ./dynajs and the defects baseline ref); --fresh or STATIC_FRESH=1 reruns all.
// CODEGRAPH=<command prefix> swaps the code-graph tool (default: ./dynajs --std tools/codegraph.js).
import * as std from "std";
import * as sys from "dyna:sys";
import * as file from "dyna:file";
import { Command } from "dyna:cli";
import { monotonicNano } from "dyna:time";

const { Path } = file;
const SELF_PATH = new Path(scriptArgs[0]);
const ROOT = (SELF_PATH.isAbsolute ? SELF_PATH : Path.cwd().join(scriptArgs[0])).toString().replace(/\/tools\/static-checks\.js$/, "");
sys.chDir(ROOT);
const LOGS = ".agent-work/static";
file.makeDir(new Path(LOGS), { recursive: true });
const PORT_CG = ["./dynajs", "--std", "tools/codegraph.js"];
const CG = (sys.getEnv("CODEGRAPH") || "").split(/\s+/).filter(Boolean);
if (CG.length === 0) CG.push(...PORT_CG);
const URING = { CODEGRAPH_EXTRA_DEFINES: "CONFIG_IO_URING" };

async function drain(pipe) {
    const parts = [], buf = new Uint8Array(65536);
    for (;;) { const n = await pipe.read(buf); if (n <= 0) break; parts.push(buf.slice(0, n)); }
    const all = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    parts.reduce((o, p) => (all.set(p, o), o + p.length), 0);
    return new TextDecoder().decode(all);
}
async function sh(argv, env) {
    const p = new sys.Spawn(argv[0], argv.slice(1), { stdin: "ignore", ...(env ? { env, extendEnv: true } : {}) });
    const [out, err] = await Promise.all([drain(p.stdout), drain(p.stderr)]);
    const r = await p.wait();
    p.close();
    return { code: r.code ?? -1, out, err, all: out + err };
}
const functions = (text) => { const m = /^ *symbols +function=(\d+)/m.exec(text); return m ? Number(m[1]) : 0; };
const ok = (pass, log) => ({ pass, log });
const cmd = (argv, env) => async () => { const r = await sh(argv, env); return ok(r.code === 0, r.all); };

const CHECKS = [
    { name: "codegraph-report", run: async () => {
        const r = await sh([...CG, "src", "--quiet", "--report"]);
        const n = functions(r.out);
        return ok(r.code === 0 && n >= 8000, `functions=${n} (need >= 8000)\n` + r.all);
    } },
    { name: "codegraph-selfcheck", run: () => cmd([...CG, "src", "--quiet", "--selfcheck"])() },
    { name: "codegraph-shapes", run: () => cmd(["bash", "tests/lib/codegraph_shapes.sh", ...CG])() },
    { name: "codegraph-offload-net", run: async () => {
        const r = await sh([...CG, "src", "--quiet", "--csv", "offload-unguarded-runtime-call"]);
        const rows = r.out.split("\n").slice(1).filter(l => l.trim() !== "");
        return ok(r.code === 0 && rows.length === 0, `unguarded=${rows.length}\n` + rows.join("\n"));
    } },
    { name: "codegraph-uring-view", run: async () => {
        const [d, u, e, s] = await Promise.all([sh([...CG, "src", "--quiet", "--report"]), sh([...CG, "src", "--quiet", "--report"], URING),
            sh([...CG, "src", "--quiet", "--parse-errors"], URING), sh([...CG, "src", "--quiet", "--selfcheck"], URING)]);
        const nd = functions(d.out), nu = functions(u.out);
        const bad = e.out.split("\n").filter(l => /^ *(dyna-aio-uring|dyna-uring|dyna-http)\.c:/.test(l));
        return ok(nu > nd && bad.length === 0 && s.code === 0,
            `default=${nd} uring=${nu} (uring must read more)\n${bad.length ? "parse errors:\n" + bad.join("\n") + "\n" : ""}selfcheck rc=${s.code}\n${s.all}`);
    } },
    { name: "defects-selftest", run: cmd(["./dynajs", "--std", "tools/js/test_defects.js"]) },
    { name: "defects-scan", run: cmd(["./dynajs", "--std", "tools/js/defects.js", ".", "--gate", "D04,D28,D48,D49", "--baseline-ref", "file:.agent-work/defects-baseline.ref.tsv"]) },
    { name: "sweep-selftest", run: cmd(["./dynajs", "tools/sweep.js", "--selftest"]) },
    { name: "dod_scan-selftest", run: cmd(["./dynajs", "--std", "tools/dod_scan.js", "selftest"]) },
    { name: "no-comments", run: cmd(["./dynajs", "--std", "tools/js/check-no-comments.js", "."]) },
    { name: "dts-bundle", run: cmd(["./dynajs", "--std", "tools/dts.js", "check"]) },
    { name: "dts-tsc", run: cmd(["./dynajs", "--std", "tools/dts-tsc.js"]) },
    { name: "error-identifiers", run: cmd(["./dynajs", "--std", "tools/js/check-error-identifiers.js", "./dynajs"]) },
    { name: "error-identifiers-selftest", run: cmd(["./dynajs", "--std", "tools/js/check-error-identifiers.js", "selftest"]) },
    { name: "generated-selftest", run: cmd(["./dynajs", "--std", "tools/js/check-generated.js", "selftest"]) },
    { name: "generated", run: cmd(["./dynajs", "--std", "tools/js/check-generated.js"]) },
    { name: "bench-smoke", run: cmd(["./dynajs", "--std", "tools/bench-smoke.js"]) },
    { name: "bench-smoke-selftest", run: cmd(["./dynajs", "--std", "tools/bench-smoke.js", "selftest"]) },
    { name: "readme-examples", run: cmd(["./dynajs", "--std", "tools/check-readme-examples.js", "README.md"]) },
    { name: "readme-examples-selftest", run: cmd(["./dynajs", "--std", "tools/check-readme-examples.js", "selftest"]) },
    { name: "android-smoke-selftest", run: cmd(["./dynajs", "--std", "tools/android/smoke.js", "selftest"]) },
    { name: "core-purity", run: cmd(["bash", "tools/core-purity.sh"]) },
    { name: "hardening", run: async () => (sys.platform() === "darwin" ? machHardening() : elfHardening()) },
];

async function machHardening() {
    const [h, u, d] = await Promise.all([sh(["otool", "-hv", "./dynajs"]), sh(["nm", "-u", "./dynajs"]), sh(["otool", "-tV", "./dynajs"])]);
    const chk = u.out.split("\n").filter(l => /_chk$/.test(l)).length;
    const pac = (d.out.match(/pacia(sp|z)?\b/g) || []).length;
    const bti = (d.out.match(/\bbti\b/g) || []).length;
    const pie = / PIE/.test(h.out), canary = u.out.includes("___stack_chk_fail");
    return ok(pie && canary && chk >= 1 && pac > 0 && bti > 0, `pie=${pie} canary=${canary} chk=${chk} pac=${pac} bti=${bti}`);
}

async function elfHardening() {
    const [l, d, s] = await Promise.all([sh(["readelf", "-lW", "./dynajs"]), sh(["readelf", "-dW", "./dynajs"]), sh(["readelf", "-sW", "--dyn-syms", "./dynajs"])]);
    const relro = l.out.includes("GNU_RELRO"), now = /BIND_NOW|FLAGS.*NOW/.test(d.out);
    const nx = !l.out.split("\n").some(x => x.includes("GNU_STACK") && x.includes("RWE"));
    const canary = s.out.includes("__stack_chk_fail");
    const fortify = s.out.split("\n").some(x => /__[a-z0-9]+_chk/.test(x) && !x.includes("__stack_chk"));
    return ok(relro && now && nx && canary && fortify, `relro=${relro} bind_now=${now} nx_stack=${nx} canary=${canary} fortify=${fortify}`);
}

const cli = new Command("static-checks").describe("run the static checks in parallel")
    .option("--only <names>", "comma-separated checks to run", { type: "string" })
    .option("--list", "print the check names", { type: "boolean" })
    .option("--fresh", "ignore the ledger and rerun checks that already passed", { type: "boolean" });
const opts = cli.parse(scriptArgs.slice(1)).options;
if (opts.list) { for (const c of CHECKS) print(c.name); std.exit(0); }
const want = opts.only ? new Set(opts.only.split(",")) : null;
if (want) for (const n of want) if (!CHECKS.some(c => c.name === n)) { print(`static-checks: unknown check ${n} (--list)`); std.exit(2); }
const picked = CHECKS.filter(c => !want || want.has(c.name));

const t0 = monotonicNano();
const ts = () => new Date().toTimeString().slice(0, 8);
const DYNA_LOG = std.getenv("DYNA_LOG") || ".agent-work/test.log";
const dlog = (...f) => { const d = new Date(), p = (n, w = 2) => String(n).padStart(w, "0"); try { const fd = std.open(DYNA_LOG, "a"); fd.puts(`${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}\tstatic\t` + f.join("\t") + "\n"); fd.close(); } catch (e) {} };
const LEDGER = `${LOGS}/ledger.tsv`;
const fresh = opts.fresh || !!sys.getEnv("STATIC_FRESH");
const KEY = (await sh(["bash", "-c", "{ bash tests/lib/tree-hash.sh src tools tests bench examples README.md dynajs.d.ts build.sh; shasum ./dynajs .agent-work/defects-baseline.ref.tsv 2>/dev/null; } | shasum | cut -c1-16"])).out.trim();
const passed = new Set(fresh ? [] : (std.loadFile(LEDGER) ?? "").split("\n").map(l => l.split("\t")).filter(r => r[1] === "PASS" && r[2] === KEY).map(r => r[0]));
print(`static-checks: key ${KEY}, ${picked.filter(c => passed.has(c.name)).length} of ${picked.length} already passed on it${fresh ? " (ignored: --fresh)" : ""}`);
const ledger = std.open(LEDGER, "a");
const done = new Map();
const results = [];
const start = (c) => {
    if (done.has(c.name)) return done.get(c.name);
    const dep = c.after && picked.some(p => p.name === c.after) ? start(picked.find(p => p.name === c.after)) : Promise.resolve({ pass: true });
    const p = dep.then(async (d) => {
        const s = monotonicNano();
        let r;
        if (passed.has(c.name)) { print(`${ts()} CACHE ${c.name.padEnd(24)}      -  (passed on key ${KEY})`); dlog("CACHE", c.name); results.push({ name: c.name, pass: true, secs: 0, log: "" }); return ok(true, ""); }
        print(`${ts()} START ${c.name}`); dlog("START", c.name);
        if (!d.pass) r = ok(false, `skipped: ${c.after} failed`);
        else { try { r = await c.run(); } catch (e) { r = ok(false, String(e && e.stack || e)); } }
        const secs = Number(monotonicNano() - s) / 1e9;
        file.writeFile(new Path(`${LOGS}/${c.name}.log`), r.log);
        results.push({ name: c.name, pass: r.pass, secs, log: r.log });
        dlog("END", c.name, r.pass ? "PASS" : "FAIL", secs.toFixed(1) + "s");
        ledger.puts([c.name, r.pass ? "PASS" : "FAIL", KEY, secs.toFixed(1), new Date().toISOString()].join("\t") + "\n"); ledger.flush();
        print(`${ts()} ${r.pass ? "PASS" : "FAIL"}  ${c.name.padEnd(24)} ${secs.toFixed(1).padStart(6)}s`);
        if (!r.pass) for (const l of r.log.trimEnd().split("\n").slice(-8)) print("      " + l);
        return r;
    });
    done.set(c.name, p);
    return p;
};
await Promise.all(picked.map(start));
const failed = results.filter(r => !r.pass).length;
print(`${ts()} static-checks: ${results.length - failed}/${results.length} passed in ${(Number(monotonicNano() - t0) / 1e9).toFixed(1)}s wall; logs in ${LOGS}/`);
std.exit(failed ? 1 : 0);
