// flags: --std
// tools/coverage-map.js -- MEASURE which source files each test suite executes, and pick the suites a change affects.
//
//   ./dynajs --std tools/coverage-map.js build  [--out DIR]          coverage engine DIR/dynajs (never ./dynajs)
//   ./dynajs --std tools/coverage-map.js run    [--out DIR] [-j N] [SUITE...]   run suites, one profile each
//   ./dynajs --std tools/coverage-map.js map    [--out DIR]          DIR/map.tsv: src file -> suite, one pair a line
//   ./dynajs --std tools/coverage-map.js all    [--out DIR] [-j N]   build + run (every suite) + map
//   ./dynajs --std tools/coverage-map.js select [--out DIR] [--why] [--touched] [--fallback] FILE...   suites for changed files
//   ./dynajs --std tools/coverage-map.js fresh  [--out DIR] [--days N]   exit 0 and say why when the map can drive select
//   ./dynajs --std tools/coverage-map.js stats  [--out DIR]          one-file selection sizes vs the module fallback
//   ./dynajs --std tools/coverage-map.js selftest                   the whole pipeline on a two-file C fixture
//   (covfiles ROOT < export.json: the worker's filter, files with an executed line, relative to ROOT)
//
// DIR defaults to .obj/cov (git-ignored). Use with the manual runner:
//   .agent-work/manual-run/run2.sh $(./dynajs --std tools/coverage-map.js select src/libregexp.c src/dyna-csv.c)
//
// build: needs a release build first (./build.sh release). It reuses that build's recorded compiler, flags and
// rules (.obj/release/flags.env, rules.mk, objs.list), adds clang source coverage (-fprofile-instr-generate
// -fcoverage-mapping) and compiles every object into DIR, so ./dynajs and .obj/release are never touched.
//
// run: every suite (tests/suites.sh) runs once under the coverage engine, with the suite's own `// flags:` and
// `// timeout:` header (bound x3: instrumented code is slower) and LLVM_PROFILE_FILE=DIR/raw/<suite>/%p.profraw,
// so child processes count toward their suite. Suites run from DIR/root, a mirror of the repo top level whose
// ./dynajs is the coverage engine, so a suite that spawns ./dynajs measures its children too. Each suite's
// profiles are merged and reduced to the list of source files with at least one executed line (DIR/cov/).
// A suite that fails under the coverage engine still contributes what it executed; DIR/status.tsv lists them.
// An empty --std script is measured the same way (the startup baseline): a pair is marked x when the suite runs
// more lines of the file than startup does, s when it only runs what every process runs at startup.
//
// select: the union of (a) the measured map (the suites that run the file beyond startup; every suite that runs the
// file at all when none does, or always with --touched, for a change on a startup path) and (b) the hand-written floor, the `// covers:` headers that name the
// file (c:src/x.c). A file the map has never seen (new file, header only seen through callers) falls back to the
// module level: the suites under tests/<module>/ plus every suite whose covers header names dyna:<module>, and
// with no module, every suite (printed to stderr so a script sees it). --why prints "suite<TAB>reason".
// --fallback ignores the map: the covers floor plus the module level for every file (the selection without a map).
//
// fresh: the map can drive select when it exists and is at most --days old (default 14). Edits to existing files keep
// it fresh: their measured suites still run them and the covers floor is read live. A source file added since the
// build has no map row, so select gives that file alone the module fallback; a removed file only leaves dead rows.
// (A whole-map fallback on any added file selected ~2250 suites for one module change.) A map written before
// srcset= existed is fresh only on the exact source tree (src=). run2.sh asks this before select.
// The fingerprints and the age are those of the source tree the coverage engine was built from (DIR/build.meta).
import * as std from "std";
import * as os from "os";
import * as sys from "dyna:sys";
import { mapCompile, selftest as rulesSelftest } from "./js/rules-mk.js";

const ROOT = sys.cwd();
const say = (s) => print("coverage-map: " + s);
const die = (s) => { std.err.puts("coverage-map: " + s + "\n"); std.exit(2); };

function run(cmd, args, opts) {
    const r = sys.Exec(cmd, args, { maxBuffer: 256 << 20, ...(opts || {}) });
    return { code: r.code, out: String(r.stdout), err: String(r.stderr) };
}
function must(cmd, args, opts) {
    const r = run(cmd, args, opts);
    if (r.code !== 0) throw new Error(cmd + " " + args.join(" ") + " -> " + r.code + ": " + (r.err || r.out).slice(-1500));
    return r.out;
}
const sh = (script, opts) => run("/bin/bash", ["-c", script], opts);
const exists = (p) => os.stat(p)[1] === 0;
const isDir = (p) => { const [st, e] = os.stat(p); return e === 0 && (st.mode & os.S_IFMT) === os.S_IFDIR; };
function read(p) { const s = std.loadFile(p); return s === null ? null : s; }
function write(p, s) { const f = std.open(p, "w"); if (!f) throw new Error("cannot write " + p); f.puts(s); f.close(); }
function mkdirp(p) { must("/bin/mkdir", ["-p", p]); }
function listDir(p) { const [names, e] = os.readdir(p); return e ? [] : names.filter((n) => n !== "." && n !== ".."); }
const now = () => Date.now();
const secs = (t0) => ((Date.now() - t0) / 1000).toFixed(1) + "s";
const q = (s) => "'" + String(s).replace(/'/g, "'\\''") + "'";

function llvmTool(name, cc) {
    const c = sys.Which(cc || "clang");
    if (c) {
        const dir = must("/bin/bash", ["-c", "cd \"$(dirname \"$(readlink -f " + q(c) + ")\")\" && pwd"]).trim();
        if (exists(dir + "/" + name)) return dir + "/" + name;
    }
    const x = run("/usr/bin/xcrun", ["-f", name]);
    if (x.code === 0 && x.out.trim()) return x.out.trim();
    die(name + " not found next to " + (cc || "clang") + " or via xcrun: the profile tools must come from the compiler's own toolchain");
}

function parseArgs(argv) {
    const o = { out: ".obj/cov", jobs: 0, why: false, touched: false, fallback: false, days: 14, rest: [] };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === "--out") o.out = argv[++i];
        else if (a === "-j") o.jobs = +argv[++i];
        else if (a === "--why") o.why = true;
        else if (a === "--touched") o.touched = true;
        else if (a === "--fallback") o.fallback = true;
        else if (a === "--days") o.days = +argv[++i];
        else if (a.startsWith("-")) die("unknown option " + a);
        else o.rest.push(a);
    }
    if (!o.out) die("--out needs a directory");
    o.outAbs = o.out.startsWith("/") ? o.out : ROOT + "/" + o.out;
    if (!(o.days >= 0)) die("--days needs a number");
    if (!o.jobs) {
        const n = +run("/usr/sbin/sysctl", ["-n", "hw.ncpu"]).out.trim() || 4;
        o.jobs = Math.max(1, n - 2);
    }
    return o;
}

export function readEnvFile(text) {
    const env = {};
    for (const line of text.split("\n")) {
        const m = /^([A-Z_]+)=(.*)$/.exec(line);
        if (m) env[m[1]] = must("/bin/bash", ["-c", "printf %s " + m[2]]);
    }
    return env;
}

export const COV_FLAGS = "-fprofile-instr-generate -fcoverage-mapping";

export function coverageRules(rulesText, outRel) {
    const moved = rulesText.split(".obj/release/").join(outRel + "/").split(".obj/release ").join(outRel + " ");
    return mapCompile(moved, (r) => r.cmd + " " + COV_FLAGS);
}

function build(o) {
    const rel = ".obj/release";
    for (const f of ["flags.env", "rules.mk", "objs.list"]) if (!exists(rel + "/" + f)) die("needs " + rel + "/" + f + ": run ./build.sh release first");
    if (!exists(".obj/repl_src.inc")) die("needs .obj/repl_src.inc: run ./build.sh release first");
    const env = readEnvFile(read(rel + "/flags.env"));
    const outRel = o.out.replace(/\/+$/, "");
    mkdirp(o.outAbs);
    const { text, rules } = coverageRules(read(rel + "/rules.mk"), outRel);
    if (rules < 100) die("rules.mk has only " + rules + " compile rules; the release rule format changed");
    write(o.outAbs + "/rules.mk", text);
    const t0 = now();
    const jobs = String(o.jobs + 2);
    const mk = run("/usr/bin/make", ["-s", "-j" + jobs, "-f", outRel + "/rules.mk", "all"]);
    if (mk.code !== 0) throw new Error("coverage compile failed:\n" + (mk.err + mk.out).slice(-3000));
    const cc = env.CC, cf = env.CF + " " + COV_FLAGS, ldf = env.LDF + " -fprofile-instr-generate";
    const objs = read(rel + "/objs.list").trim().split(/\s+/).map((p) => p.replace(".obj/release/", outRel + "/"));
    must("/bin/bash", ["-c", `${cc} ${ldf} -o ${q(outRel + "/dynajs")} ${q(outRel + "/src/dyna-cli.o")} ${q(outRel + "/src/dyna-repl.o")} ${objs.map(q).join(" ")} ${env.LIBS}`]);
    const sym = run("/usr/bin/nm", [outRel + "/dynajs"]).out;
    if (!sym.includes("__llvm_profile_runtime")) die(outRel + "/dynajs is not coverage-instrumented");
    const probe = run(outRel + "/dynajs", ["-e", 'import("dyna:csv").then(() => print("ok"))'], { env: { LLVM_PROFILE_FILE: "/dev/null", PATH: "/usr/bin:/bin" } });
    if (!probe.out.includes("ok")) die("coverage engine cannot run: " + (probe.err || probe.out).slice(0, 400));
    const mrows = read(".agent-work/merges.tsv");
    write(o.outAbs + "/build.meta", `src=${srcFingerprint()}\tsrcset=${srcSetFingerprint()}\tbuilt=${Math.floor(Date.now() / 1000)}${mrows === null ? "" : "\tmerges=" + mergeRows(mrows)}\n`);
    say(`build: ${rules} objects + repl -> ${outRel}/dynajs in ${secs(t0)}`);
    return (now() - t0) / 1000;
}

export function covFiles(json, root) {
    const d = JSON.parse(json);
    const out = [];
    const pre = root.endsWith("/") ? root : root + "/";
    for (const unit of d.data || []) for (const f of unit.files || []) {
        const s = f.summary && f.summary.lines;
        if (!s || !(s.covered > 0)) continue;
        out.push([f.filename.startsWith(pre) ? f.filename.slice(pre.length) : f.filename, s.covered]);
    }
    return out.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
}

export function suiteHeader(text) {
    const lines = text.split("\n").slice(0, 3);
    let flags = [], timeout = 120;
    for (const l of lines) {
        let m = /^\/\/\s*flags:(.*)$/.exec(l);
        if (m) flags = m[1].trim().split(/\s+/).filter(Boolean);
        m = /^\/\/\s*timeout:\s*(\d+)/.exec(l);
        if (m) timeout = +m[1];
    }
    return { flags, timeout };
}

export const SOLO_RE = /(PostgreSQL|Redis|SQLite|TCPServer|HTTPServer|HTTPServerAsync|DNSServer|test_http_params|test_http_pentest|test_net_pentest|test_http_proxy|test_http_hardening|test_client_tls|test_onconnect_hook)/;

export function suiteId(path) { return path.replace(/\//g, "__"); }

function suiteList() {
    return must("/bin/bash", ["tests/suites.sh"]).trim().split("\n").filter(Boolean);
}

function makeRoot(outAbs) {
    const root = outAbs + "/root";
    mkdirp(root);
    for (const n of listDir(ROOT)) {
        if (n === "dynajs" || n === ".obj" || n === ".git") continue;
        const dst = root + "/" + n;
        if (!exists(dst)) os.symlink(ROOT + "/" + n, dst);
    }
    if (!exists(root + "/dynajs")) os.symlink(outAbs + "/dynajs", root + "/dynajs");
    if (!exists(root + "/.obj")) os.symlink(ROOT + "/.obj", root + "/.obj");
    return root;
}

function workerScript(o, profdata, cov) {
    return `#!/bin/bash
set -u
f=$1; flags=$2; tmo=$3
[ "$flags" = - ] && flags=""
OUT=${q(o.outAbs)}; ROOT=${q(ROOT)}
id=$(printf '%s' "$f" | sed 's|/|__|g')
raw="$OUT/raw/$id"; mkdir -p "$raw" "$OUT/tmp"; rm -f "$raw"/*.profraw "$raw"/m.profdata
t=$(mktemp -d "$OUT/tmp/t.XXXXXX")
t0=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
cd "$OUT/root" && TMPDIR=$t LLVM_PROFILE_FILE="$raw/%p.profraw" perl -e 'alarm shift; exec @ARGV' "$tmo" ./dynajs $flags "$f" </dev/null >"$OUT/log/$id.log" 2>&1; rc=$?
t1=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
find "$t" -depth -delete 2>/dev/null
n=$(ls "$raw" | grep -c profraw)
if [ "$n" -gt 0 ]; then
  ${q(profdata)} merge -sparse -o "$raw/m.profdata" "$raw"/*.profraw 2>>"$OUT/log/$id.log" && \\
  ${q(cov)} export -summary-only -instr-profile "$raw/m.profdata" "$OUT/dynajs" 2>>"$OUT/log/$id.log" | "$ROOT/dynajs" --std "$ROOT/tools/coverage-map.js" covfiles "$ROOT" > "$OUT/cov/$id.tsv.tmp" && mv "$OUT/cov/$id.tsv.tmp" "$OUT/cov/$id.tsv"
  rm -f "$raw"/*.profraw "$raw/m.profdata"
fi
printf '%s\\t%s\\t%s\\t%s\\n' "$f" "$rc" "$(echo "$t1 - $t0" | bc)" "$n" >> "$OUT/status.tsv"
`;
}

function runSuites(o) {
    if (!exists(o.outAbs + "/dynajs")) die("no " + o.out + "/dynajs: run build first");
    const profdata = llvmTool("llvm-profdata"), cov = llvmTool("llvm-cov");
    const base = o.outAbs + "/baseline.js";
    write(base, "// flags: --std\n");
    const suites = [base, ...(o.rest.length ? o.rest : suiteList())];
    mkdirp(o.outAbs + "/raw"); mkdirp(o.outAbs + "/cov"); mkdirp(o.outAbs + "/log");
    makeRoot(o.outAbs);
    write(o.outAbs + "/worker.sh", workerScript(o, profdata, cov));
    const status = o.outAbs + "/status.tsv";
    if (!o.rest.length) write(status, "");
    else {
        const keep = (read(status) || "").split("\n").filter((l) => l && !suites.includes(l.split("\t")[0]) && /^tests\//.test(l));
        write(status, keep.map((l) => l + "\n").join(""));
    }
    const par = [], solo = [];
    for (const f of suites) {
        const text = read(f);
        if (text === null) die("no such suite " + f);
        const h = suiteHeader(text);
        (SOLO_RE.test(text) ? solo : par).push([f, h.flags.join(" ") || "-", String(h.timeout * 3)].join("\t"));
    }
    write(o.outAbs + "/jobs.tsv", par.join("\n") + "\n");
    write(o.outAbs + "/jobs-solo.tsv", solo.join("\n") + "\n");
    say(`run: ${suites.length} suites, ${par.length} on ${o.jobs} workers then ${solo.length} one at a time (servers and ports, as run2.sh)`);
    const t0 = now();
    const wave = (list, p) => sh(`cd ${q(ROOT)} && grep -v '^$' ${q(list)} | tr '\\t\\n' '\\0\\0' | xargs -0 -n3 -P ${p} bash ${q(o.outAbs + "/worker.sh")}`, { timeoutMs: 6 * 3600 * 1000 });
    const r = wave(o.outAbs + "/jobs.tsv", o.jobs);
    if (solo.length) wave(o.outAbs + "/jobs-solo.tsv", 1);
    const st = (read(status) || "").trim().split("\n").filter(Boolean).map((l) => l.split("\t"));
    const failed = st.filter((s) => s[1] !== "0").length, noprof = st.filter((s) => s[3] === "0").length;
    say(`run: ${st.length} suites in ${secs(t0)} (xargs rc ${r.code}); ${failed} exited non-zero under the coverage engine, ${noprof} wrote no profile (listed in ${o.out}/status.tsv)`);
    return (now() - t0) / 1000;
}

export function mapRows(covs, base) {
    const pairs = [];
    const files = new Set(), suites = new Set();
    let beyond = 0;
    for (const [suite, rows] of covs) {
        suites.add(suite);
        for (const [f, n] of rows) {
            if (!f || f.startsWith("/")) continue;
            const x = n > (base.get(f) || 0);
            if (x) beyond++;
            pairs.push(f + "\t" + suite + "\t" + (x ? "x" : "s"));
            files.add(f);
        }
    }
    pairs.sort();
    return { pairs, files, suites, beyond };
}

function readCov(p) {
    return read(p).split("\n").filter(Boolean).map((l) => { const [f, n] = l.split("\t"); return [f, +n]; });
}

function buildMap(o) {
    const covDir = o.outAbs + "/cov";
    const baseId = suiteId(o.outAbs + "/baseline.js") + ".tsv";
    if (!exists(covDir + "/" + baseId)) die("no startup baseline profile (" + covDir + "/" + baseId + "): run `run` again");
    const base = new Map(readCov(covDir + "/" + baseId));
    const covs = [];
    for (const n of listDir(covDir).sort()) {
        if (!n.endsWith(".tsv") || n === baseId || !n.startsWith("tests__")) continue;
        covs.push([n.slice(0, -4).replace(/__/g, "/"), readCov(covDir + "/" + n)]);
    }
    const { pairs, files, suites, beyond } = mapRows(covs, base);
    const head = git(["rev-parse", "HEAD"]).trim();
    const bm = read(o.outAbs + "/build.meta");
    if (bm === null) die("no " + o.out + "/build.meta (the source fingerprints of the measured engine): run build again");
    const bmeta = "\t" + bm.trim();
    write(o.outAbs + "/map.tsv", `# coverage-map v1\thead=${head}\tsrc=${metaField(bmeta, "src")}\tsrcset=${metaField(bmeta, "srcset")}\tbuilt=${metaField(bmeta, "built")}${metaField(bmeta, "merges") === null ? "" : "\tmerges=" + metaField(bmeta, "merges")}\tsuites=${suites.size}\tfiles=${files.size}\tpairs=${pairs.length}\tbeyond_startup=${beyond}\tstartup_files=${base.size}\n` + pairs.join("\n") + "\n");
    say(`map: ${o.out}/map.tsv: ${files.size} source files, ${suites.size} suites, ${pairs.length} pairs (${beyond} beyond what startup executes; startup alone runs ${base.size} files)`);
    return { files: files.size, suites: suites.size, pairs: pairs.length };
}

function git(args) { return must("/usr/bin/git", args); }
function srcFingerprint() {
    return must("/bin/bash", ["-c", "git ls-files src repl.js | git hash-object --stdin-paths | git hash-object --stdin"]).trim().slice(0, 12);
}

function srcSetFingerprint() {
    return must("/bin/bash", ["-c", "git ls-files src repl.js | git hash-object --stdin"]).trim().slice(0, 12);
}

export function mergeRows(tsv) {
    return tsv.split("\n").filter((l) => /^\d{4}-\d\d-\d\d\t/.test(l)).length;
}

export function metaField(meta, k) {
    const m = new RegExp("\\t" + k + "=([^\\t]*)").exec(meta);
    return m ? m[1] : null;
}

export function freshness(meta, nowSec, days, cur) {
    if (!/^# coverage-map v1\t/.test(meta)) return { fresh: false, why: "not a coverage-map v1 header" };
    const built = +metaField(meta, "built"), set = metaField(meta, "srcset"), src = metaField(meta, "src");
    if (set === null) return src === cur.src ? { fresh: true, why: "older map, measured on this exact source tree" } : { fresh: false, why: "older map without srcset=, measured on another source tree (src " + src + ")" };
    const age = (nowSec - built) / 86400;
    if (!(age <= days)) return { fresh: false, why: "built " + age.toFixed(1) + " days ago (limit " + days + ")" };
    if (set !== cur.srcset) return { fresh: true, why: "built " + age.toFixed(1) + " days ago; source files were added or removed since, so files new to the map select by module" };
    return { fresh: true, why: "built " + age.toFixed(1) + " days ago on the same source file set" + (src === cur.src ? ", same contents" : ", contents edited since") };
}

function fresh(o) {
    const t = read(o.outAbs + "/map.tsv");
    if (t === null) { print("stale: no " + o.out + "/map.tsv"); std.exit(1); }
    const r = freshness(t.split("\n")[0], Date.now() / 1000, o.days, { src: srcFingerprint(), srcset: srcSetFingerprint() });
    print((r.fresh ? "fresh: " : "stale: ") + o.out + "/map.tsv " + r.why);
    if (!r.fresh) std.exit(1);
}

export function loadMap(text) {
    const byFile = new Map(), byFileX = new Map();
    let meta = "";
    for (const line of text.split("\n")) {
        if (!line) continue;
        if (line.startsWith("#")) { meta = line; continue; }
        const [f, s, k] = line.split("\t");
        if (!byFile.has(f)) byFile.set(f, new Set());
        byFile.get(f).add(s);
        if (k !== "s") {
            if (!byFileX.has(f)) byFileX.set(f, new Set());
            byFileX.get(f).add(s);
        }
    }
    return { byFile, byFileX, meta };
}

export function coversOf(text) {
    const lines = text.split("\n").slice(0, 3);
    for (const l of lines) {
        const m = /^\/\/\s*covers:\s*(.*)$/.exec(l);
        if (m) {
            const body = m[1].split(" -- ")[0];
            return body.split(/\s+/).filter((w) => w && w !== "integration" && w !== "blackbox" && w !== "example" && w !== "contract");
        }
    }
    return [];
}

export function moduleOf(file) {
    let m = /^src\/modules\/([a-z0-9]+)\//.exec(file);
    if (m) return m[1];
    m = /^src\/dyna-([a-z0-9]+)(?:-[a-z0-9-]+)?\.[ch]$/.exec(file);
    if (m && isDir("tests/" + m[1])) return m[1];
    return null;
}

function coversIndex(suites) {
    const idx = [];
    for (const s of suites) {
        const t = read(s);
        if (t !== null) idx.push([s, coversOf(t)]);
    }
    return idx;
}

export function selectSuites(changed, map, covers, allSuites, touched) {
    const why = new Map();
    const add = (s, r) => { if (!why.has(s)) why.set(s, r); };
    let fellBack = [];
    for (const f of changed) {
        const all = map.byFile.get(f), x = map.byFileX.get(f);
        const measured = touched ? all : x && x.size ? x : all;
        if (measured) for (const s of measured) add(s, (measured === x ? "measured " : "executes ") + f);
        for (const [s, cv] of covers) if (cv.includes("c:" + f)) add(s, "covers c:" + f);
        if (!measured) {
            const mod = moduleOf(f);
            fellBack.push(f);
            if (mod) {
                for (const s of allSuites) if (s.startsWith("tests/" + mod + "/")) add(s, "module tests/" + mod + "/ for " + f);
                for (const [s, cv] of covers) if (cv.some((c) => c === "dyna:" + mod || c.startsWith("dyna:" + mod + "."))) add(s, "module dyna:" + mod + " for " + f);
            } else {
                for (const s of allSuites) add(s, "no map entry and no module for " + f);
            }
        }
    }
    return { why, fellBack };
}

export function moduleFallback(f, covers, allSuites) {
    const mod = moduleOf(f);
    if (!mod) return allSuites.length;
    const set = new Set();
    for (const s of allSuites) if (s.startsWith("tests/" + mod + "/")) set.add(s);
    for (const [s, cv] of covers) if (cv.some((c) => c === "dyna:" + mod || c.startsWith("dyna:" + mod + "."))) set.add(s);
    return set.size;
}

function loadMapFile(o) {
    const t = read(o.outAbs + "/map.tsv");
    if (t === null) die("no " + o.out + "/map.tsv: run `tools/coverage-map.js all` first");
    const map = loadMap(t);
    const fp = /src=(\w+)/.exec(map.meta);
    if (fp && fp[1] !== srcFingerprint()) std.err.puts("coverage-map: note: sources changed since the map was measured (src fingerprint " + fp[1] + "); call paths added since are not in it, the covers floor is read live\n");
    return map;
}

function select(o) {
    if (!o.rest.length) die("select needs changed FILE arguments");
    const map = o.fallback ? loadMap("") : loadMapFile(o);
    const all = suiteList();
    const covers = coversIndex(all);
    const { why, fellBack } = selectSuites(o.rest, map, covers, all, o.touched);
    if (fellBack.length && !o.fallback) std.err.puts("coverage-map: no measured suites for " + fellBack.join(" ") + "; used the module-level fallback\n");
    const out = [...why.keys()].filter((s) => all.includes(s)).sort();
    for (const s of out) print(o.why ? s + "\t" + why.get(s) : s);
}

function stats(o) {
    const map = loadMapFile(o);
    const all = suiteList();
    const covers = coversIndex(all);
    const rows = [];
    for (const f of [...map.byFile.keys()].filter((f) => f.startsWith("src/")).sort()) {
        const { why } = selectSuites([f], map, covers, all);
        rows.push([f, why.size, moduleFallback(f, covers, all), moduleOf(f) || "-"]);
    }
    const pct = (a, p) => { const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };
    const sel = rows.map((r) => r[1]), fb = rows.map((r) => r[2]);
    const modRows = rows.filter((r) => r[3] !== "-");
    say(`stats: ${rows.length} measured src files, ${all.length} suites`);
    print(`  one-file change, map+covers: median ${pct(sel, 0.5)}, p90 ${pct(sel, 0.9)}, max ${Math.max(...sel)} suites`);
    print(`  module-level fallback:       median ${pct(fb, 0.5)}, p90 ${pct(fb, 0.9)} suites (every suite = ${all.length} when the file has no module)`);
    if (modRows.length) print(`  files with a module (${modRows.length}): map+covers median ${pct(modRows.map((r) => r[1]), 0.5)} vs module fallback median ${pct(modRows.map((r) => r[2]), 0.5)}`);
    const show = ["src/libregexp.c", "src/modules/http/http-app-send.c", "src/core/csvx/csvx-col.c", "src/dyna-csv.c", "src/vm/interpreter.inc.c", "src/core/ds/ds-trie.c"];
    for (const f of show) { const r = rows.find((x) => x[0] === f); if (r) print(`  ${f}: ${r[1]} selected vs ${r[2]} module fallback`); }
    write(o.outAbs + "/stats.tsv", "file\tselected\tmodule_fallback\tmodule\n" + rows.map((r) => r.join("\t")).join("\n") + "\n");
}

function selftest() {
    const t0 = now();
    let fails = 0;
    const ok = (c, m) => { if (!c) { fails++; print("FAIL: " + m); } };
    ok(JSON.stringify(suiteHeader("// flags: --std\n// timeout: 300\n// covers: c:src/x.c\n")) === JSON.stringify({ flags: ["--std"], timeout: 300 }), "suiteHeader reads flags and timeout");
    ok(JSON.stringify(coversOf("// flags: --std\n// covers: integration c:src/a.c c:src/b.c -- why\n")) === JSON.stringify(["c:src/a.c", "c:src/b.c"]), "coversOf drops the kind word and the reason");
    ok(JSON.stringify(coversOf("// covers: dyna:csv.parse\n")) === JSON.stringify(["dyna:csv.parse"]), "coversOf keeps a d.ts symbol");
    ok(moduleOf("src/modules/http/http-app-send.c") === "http", "moduleOf a module source");
    ok(moduleOf("src/libregexp.c") === null, "moduleOf a core source is null");
    const cr = coverageRules(".obj/release/src/a.o: src/a.c\n\t@clang -O2 -MMD -MF $@.d -c -o $@ $<\nall: .obj/release/src/a.o\n", ".obj/cov");
    ok(cr.rules === 1 && cr.text.includes(".obj/cov/src/a.o: src/a.c") && cr.text.includes(COV_FLAGS + " -MMD") && !cr.text.includes(".obj/release"), "coverageRules rewrites paths and adds coverage flags");
    const cr2 = coverageRules(".obj/release/src/a.o: src/a.c\n\t@mkdir -p $(@D)\n\t@clang -O2 -I.obj  -MMD -MP -MF $@.d -c -o $@ $<\nall: .obj/release/src/a.o\n", ".obj/cov");
    ok(cr2.rules === 1 && cr2.text.includes("-I.obj " + COV_FLAGS + "  -MMD -MP -MF $@.d -c -o $@ $<"), "coverageRules handles the -MMD -MP rule format");
    const rs = rulesSelftest(exists(".obj/release/rules.mk") ? read(".obj/release/rules.mk") : null);
    for (const b of rs.bad) ok(false, "rules-mk: " + b);
    if (exists(".obj/release/rules.mk")) { const real = coverageRules(read(".obj/release/rules.mk"), ".obj/cov"); ok(real.rules >= 100 && real.text.split(COV_FLAGS).length - 1 === real.rules, "real rules.mk: coverageRules adds the flags to every one of " + real.rules + " compile rules"); }
    else print("note: no .obj/release/rules.mk; the real-file checks were skipped (run ./build.sh release)");
    const map = loadMap("# coverage-map v1\nsrc/a.c\ttests/x/test_a.js\nsrc/a.c\ttests/x/test_b.js\nsrc/b.c\ttests/y/test_c.js\n");
    const all = ["tests/x/test_a.js", "tests/x/test_b.js", "tests/y/test_c.js", "tests/y/test_d.js", "tests/http/test_h.js"];
    const covers = [["tests/y/test_d.js", ["c:src/a.c"]], ["tests/x/test_a.js", ["dyna:x.f"]], ["tests/y/test_c.js", ["dyna:http.App"]]];
    let r = selectSuites(["src/a.c"], map, covers, all);
    ok(JSON.stringify([...r.why.keys()].sort()) === JSON.stringify(["tests/x/test_a.js", "tests/x/test_b.js", "tests/y/test_d.js"]), "measured suites plus the covers floor");
    ok(r.why.get("tests/y/test_d.js") === "covers c:src/a.c" && r.fellBack.length === 0, "the floor names its reason");
    r = selectSuites(["src/modules/http/new.c"], map, covers, all);
    ok(JSON.stringify([...r.why.keys()].sort()) === JSON.stringify(["tests/http/test_h.js", "tests/y/test_c.js"]) && r.fellBack.length === 1, "an unmeasured module file falls back to its module");
    const m2 = loadMap("# v\nsrc/c.c\ttests/x/test_a.js\tx\nsrc/c.c\ttests/x/test_b.js\ts\nsrc/d.c\ttests/x/test_a.js\ts\nsrc/d.c\ttests/x/test_b.js\ts\n");
    r = selectSuites(["src/c.c"], m2, [], all);
    ok(JSON.stringify([...r.why.keys()]) === JSON.stringify(["tests/x/test_a.js"]), "only the suite that runs the file beyond startup");
    r = selectSuites(["src/c.c"], m2, [], all, true);
    ok(r.why.size === 2, "--touched takes every suite that runs the file");
    r = selectSuites(["src/d.c"], m2, [], all);
    ok(r.why.size === 2 && r.why.get("tests/x/test_a.js") === "executes src/d.c", "a startup-only file selects every suite that runs it");
    const mr = mapRows([["tests/x/test_a.js", [["src/a.c", 10], ["src/b.c", 3]]], ["tests/x/test_b.js", [["src/a.c", 4]]]], new Map([["src/a.c", 4], ["src/b.c", 3]]));
    ok(mr.pairs.join("|") === "src/a.c\ttests/x/test_a.js\tx|src/a.c\ttests/x/test_b.js\ts|src/b.c\ttests/x/test_a.js\ts" && mr.beyond === 1, "mapRows marks lines beyond the startup baseline, got " + JSON.stringify(mr.pairs));
    r = selectSuites(["src/unmapped-core.c"], map, covers, all);
    ok(r.why.size === all.length, "an unmeasured non-module file selects every suite");
    r = selectSuites(["src/a.c"], loadMap(""), covers, all);
    ok(r.why.size === all.length && r.fellBack.length === 1, "--fallback: a measured file without a module selects every suite");
    r = selectSuites(["src/modules/http/a.c"], loadMap(""), [["tests/x/test_b.js", ["c:src/modules/http/a.c"]], ...covers], all);
    ok(JSON.stringify([...r.why.keys()].sort()) === JSON.stringify(["tests/http/test_h.js", "tests/x/test_b.js", "tests/y/test_c.js"]), "--fallback: the covers floor plus the module level");
    ok(mergeRows("date\tpatch\n2026-11-28\ta\tgreen\n2026-11-01\tb\tgreen\nfree note\n") === 2 && mergeRows("") === 0, "merges= counts dated merges.tsv rows only");
    const hdr = (extra) => "# coverage-map v1\thead=h\tsrc=S1" + extra + "\tsuites=1";
    const cur = { src: "S2", srcset: "F1" };
    ok(freshness(hdr("\tsrcset=F1\tbuilt=1000"), 1000 + 86400, 14, cur).fresh, "fresh: same file set, contents edited, one day old");
    {
        const fr = freshness(hdr("\tsrcset=F0\tbuilt=1000"), 1000, 14, cur);
        ok(fr.fresh && /select by module/.test(fr.why), "fresh: a source file added or removed (new files fall back one by one), got " + JSON.stringify(fr));
    }
    ok(!freshness(hdr("\tsrcset=F0\tbuilt=1000"), 1000 + 15 * 86400, 14, cur).fresh, "stale: file set changed and older than --days");
    r = selectSuites(["src/a.c", "src/modules/http/new.c"], map, covers, all);
    ok(r.fellBack.join() === "src/modules/http/new.c" && r.why.has("tests/http/test_h.js") && [...r.why.values()].some((v) => /^(?:measured|executes) src\/a\.c/.test(v)), "a file new to the map falls back alone; a mapped file keeps its measured suites, got " + JSON.stringify([...r.why.keys()]));
    ok(!freshness(hdr("\tsrcset=F1\tbuilt=1000"), 1000 + 15 * 86400, 14, cur).fresh, "stale: older than --days");
    ok(!freshness(hdr(""), 0, 14, cur).fresh && freshness(hdr(""), 0, 14, { src: "S1", srcset: "x" }).fresh, "an older map without srcset= is fresh only on the same tree");
    ok(!freshness("# v\tsrc=S2", 0, 14, cur).fresh, "stale: not a v1 header");
    ok(metaField(hdr("\tsrcset=F1"), "srcset") === "F1" && metaField(hdr(""), "srcset") === null, "metaField reads one header field");

    const dir = ROOT + "/.obj/cov-selftest";
    sh("[ -d " + q(dir) + " ] && find " + q(dir) + " -depth -delete");
    mkdirp(dir);
    write(dir + "/a.c", "int fa(int x) { return x * 2; }\n");
    write(dir + "/b.c", "int fb(int x) { return x + 1; }\n");
    write(dir + "/main.c", '#include <stdlib.h>\n#include <string.h>\nint fa(int); int fb(int);\nint main(int c, char** v) { return c > 1 && strcmp(v[1], "a") == 0 ? fa(1) - 2 : fb(1) - 2; }\n');
    must("/bin/bash", ["-c", `cd ${q(dir)} && clang ${COV_FLAGS} -O1 -o prog a.c b.c main.c`]);
    const profdata = llvmTool("llvm-profdata"), cov = llvmTool("llvm-cov");
    const files = {};
    for (const arg of ["a", "b"]) {
        must("/bin/bash", ["-c", `cd ${q(dir)} && LLVM_PROFILE_FILE=${arg}-%p.profraw ./prog ${arg}`]);
        must("/bin/bash", ["-c", `cd ${q(dir)} && ${q(profdata)} merge -sparse -o ${arg}.profdata ${arg}-*.profraw`]);
        const rep = must("/bin/bash", ["-c", `cd ${q(dir)} && ${q(cov)} export -summary-only -instr-profile ${arg}.profdata ./prog`]);
        files[arg] = covFiles(rep, dir).map((x) => x[0]);
    }
    ok(JSON.stringify(files.a) === JSON.stringify(["a.c", "main.c"]), "run 'a' executes a.c and main.c, got " + JSON.stringify(files.a));
    ok(JSON.stringify(files.b) === JSON.stringify(["b.c", "main.c"]), "run 'b' executes b.c and main.c, got " + JSON.stringify(files.b));
    const wtext = workerScript({ outAbs: "/X" }, "/p", "/c");
    ok(wtext.includes("export -summary-only") && wtext.includes("coverage-map.js\" covfiles") && wtext.includes("LLVM_PROFILE_FILE=\"$raw/%p.profraw\""), "the worker uses the same report filter and a per-process profile name");
    sh("[ -d " + q(dir) + " ] && find " + q(dir) + " -depth -delete");
    print(`coverage-map selftest: ${fails ? fails + " failed" : "all passed"} in ${secs(t0)}`);
    if (fails) std.exit(1);
}

function main() {
    const [cmd, ...rest] = scriptArgs.slice(1);
    if (cmd === "selftest") return selftest();
    if (cmd === "covfiles") { for (const [f, n] of covFiles(std.in.readAsString(), rest[0] || ROOT)) print(f + "\t" + n); return; }
    const o = parseArgs(rest);
    if (cmd === "build") build(o);
    else if (cmd === "run") runSuites(o);
    else if (cmd === "map") buildMap(o);
    else if (cmd === "all") { const b = build(o); const r = runSuites(o); const m = buildMap(o); say(`all: build ${b.toFixed(0)}s, corpus ${r.toFixed(0)}s, map ${m.files} files x ${m.suites} suites = ${m.pairs} pairs`); }
    else if (cmd === "select") select(o);
    else if (cmd === "stats") stats(o);
    else if (cmd === "fresh") fresh(o);
    else die("usage: build|run|map|all|select|fresh|stats|selftest (see the header of tools/coverage-map.js)");
}
main();
