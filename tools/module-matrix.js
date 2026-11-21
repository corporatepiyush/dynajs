// flags: --std
// tools/module-matrix.js -- build the engine with the core plus exactly one module, for every module of tools/modules.tsv,
// and turn the link errors into the true cross-module C dependencies (S8 B1).
//
//   ./dynajs --std tools/module-matrix.js [--out DIR] [-j N] [--deps] [--only a,b] [--ext]
//
// Needs a release build first (./build.sh release): it reuses that build's recorded compiler, flags and per-file
// compile commands (.obj/release/flags.env, rules.mk) and never touches ./dynajs or .obj. DIR (default .obj-matrix)
// holds everything: an object cache keyed by the hash of each file's preprocessed text (a file whose text does not
// change between configurations compiles once), one directory per configuration with its engine, link log and report.
//
// Configurations: "core" (no dyna:* module, no OpenSSL/SQLite/zstd/brotli, no CONFIG_OPENSSL/TLS/SQLITE/ZSTD/BROTLI), then
// "core+M" for every module M (its sources, its CONFIG_NATIVE_MODULE_<M> gate, or CONFIG_<M> for kind "feature" such as
// tls, and the feature define and libraries of the pkg-config names it lists; OpenSSL links through the pkg column: crypto and tls each list it, and it defines CONFIG_OPENSSL), then
// "all" (every module; must link like ./dynajs). With --deps each "core+M" also gets the closure of M's manifest deps (and their sources, gates and libraries): the build B2 will produce.
// -Werror is dropped so every configuration gets to the link step; warnings are counted (they would fail -Werror).
//
// Each configuration's undefined symbols are attributed by the defining source file (from the objects of "all"):
// a module's file -> that module is a C dependency; an unknown symbol -> a library by prefix (OpenSSL, SQLite, zstd,
// brotli). The referencing file says where the dependency lives: a core file that needs a module is a leak of the core.
// Each linked engine imports its modules ("dyna:x" from the manifest exports) to prove it starts, then probes the core web
// globals (URLSearchParams, FormData multipart), fetch without http (TypeError naming "http"), https/wss without tls
// (TypeError naming "tls"), https with tls (no such TypeError) and the OpenSSL-backed crypto export with crypto+tls.
// Reachability from main (object symbol graph) lists the core-pool files a module pulls in beyond the core build and the
// core-pool files the core build never reaches (kits only modules use: candidates to move out of the always-built core).
//
// Output: DIR/report.txt (human), DIR/deps.tsv (module, needs modules, needs libs, leaking files, link ms, compile ms).
// Manifest format (tools/modules.tsv, tab separated, header row): module kind exports src kits pkg deps tests description. A kits entry is a file, a dir/ prefix, a glob (* within one path segment, {a,b} alternatives) or a !glob that excludes.
// kind "ext" (ui, sdl, raylib, uring: opt-in through build.sh flags) is skipped unless --ext; kind "feature" (tls) has no
// dyna:* module of its own and turns on CONFIG_<NAME> for the code other modules gate on it. A src entry "file@m" is
// built only when module m is selected too. A file of one module listed in another module's kits is built for either.
import * as std from "std";
import { compileRules as parseRules, selftest as rulesSelftest } from "./js/rules-mk.js";
import * as os from "os";
import * as sys from "dyna:sys";

const ROOT = sys.cwd();
const say = (s) => print("module-matrix: " + s);
const die = (s) => { std.err.puts("module-matrix: " + s + "\n"); std.exit(2); };
function run(cmd, args, opts) {
    const r = sys.Exec(cmd, args, { maxBuffer: 512 << 20, ...(opts || {}) });
    return { code: r.code, out: String(r.stdout), err: String(r.stderr) };
}
const sh = (script) => run("/bin/bash", ["-c", script]);
function must(script) { const r = sh(script); if (r.code !== 0) throw new Error(script.slice(0, 200) + " -> " + r.code + ": " + (r.err || r.out).slice(-2000)); return r.out; }
const exists = (p) => os.stat(p)[1] === 0;
const read = (p) => std.loadFile(p);
function write(p, s) { const f = std.open(p, "w"); if (!f) throw new Error("cannot write " + p); f.puts(s); f.close(); }
const q = (s) => "'" + String(s).replace(/'/g, "'\\''") + "'";
const ms = (t0) => Date.now() - t0;

function parseArgs(argv) {
    const o = { out: ".obj-matrix", jobs: 0, deps: false, only: null, ext: false, base: [], tag: "" };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === "--out") o.out = argv[++i];
        else if (a === "-j") o.jobs = +argv[++i];
        else if (a === "--deps") o.deps = true;
        else if (a === "--ext") o.ext = true;
        else if (a === "--only") o.only = argv[++i].split(",");
        else if (a === "--tag") o.tag = argv[++i].replace(/[^a-z0-9_-]/gi, "");
        else if (a === "--base") o.base = argv[++i].split(",").filter(Boolean);
        else die("unknown argument " + a);
    }
    if (!o.out || o.out === ".obj" || o.out.startsWith(".obj/")) die("--out must be a scratch directory outside .obj");
    o.outAbs = o.out.startsWith("/") ? o.out : ROOT + "/" + o.out;
    if (!o.jobs) o.jobs = +sh("sysctl -n hw.ncpu 2>/dev/null || nproc").out.trim() || 4;
    return o;
}

export function readManifest(text) {
    const rows = text.split("\n").filter((l) => l.trim() && !l.startsWith("#"));
    const head = rows.shift().split("\t");
    const want = ["module", "kind", "exports", "src", "kits", "pkg", "deps", "tests", "description"];
    if (head.join(",") !== want.join(",")) throw new Error("tools/modules.tsv header must be: " + want.join(" "));
    const mods = new Map();
    for (const r of rows) {
        const c = r.split("\t");
        if (c.length !== want.length) throw new Error("tools/modules.tsv: " + c[0] + ": " + c.length + " columns, want " + want.length);
        const words = (s) => s.split(" ").filter(Boolean);
        mods.set(c[0], { name: c[0], kind: c[1], exports: words(c[2]), src: words(c[3]), kits: words(c[4]), pkg: words(c[5]), deps: words(c[6]), tests: words(c[7]), description: c[8] });
    }
    for (const m of mods.values()) for (const d of m.deps) if (!mods.has(d)) throw new Error("tools/modules.tsv: " + m.name + " depends on unknown module " + d);
    return mods;
}

function readEnvFile(text) {
    const env = {};
    for (const line of text.split("\n")) {
        const m = /^([A-Z_]+)=(.*)$/.exec(line);
        if (m) env[m[1]] = must("printf %s " + m[2]);
    }
    return env;
}

function compileRules(text) {
    return parseRules(text).map((r) => ({ src: r.src, cmd: r.cmd }));
}

const GATES = /(^| )-DCONFIG_(NATIVE_MODULE_[A-Z0-9_]+|OPENSSL|TLS|SQLITE|ZSTD|BROTLI)(?= |$)/g;
const stripCmd = (c) => c.replace(GATES, "").replace(/(^| )-Werror(?= |$)/g, "");

function libGroups(libs) {
    const toks = libs.split(/\s+/).filter(Boolean);
    const g = { base: [], openssl: [], sqlite3: [], libzstd: [], brotli: [] };
    let pendL = [];
    const own = (l) => /^-l(ssl|crypto)$/.test(l) ? "openssl" : l === "-lsqlite3" ? "sqlite3" : l === "-lzstd" ? "libzstd" : /^-lbrotli/.test(l) ? "brotli" : "base";
    for (const t of toks) {
        if (t.startsWith("-L")) { pendL.push(t); continue; }
        const k = own(t);
        if (pendL.length) { g[k].push(...pendL); pendL = []; }
        g[k].push(t);
    }
    return g;
}
const PKG = {
    openssl: { def: "-DCONFIG_OPENSSL", lib: "openssl" },
    sqlite3: { def: "-DCONFIG_SQLITE", lib: "sqlite3" },
    libzstd: { def: "-DCONFIG_ZSTD", lib: "libzstd" },
    libbrotlienc: { def: os.platform === "darwin" ? "" : "-DCONFIG_BROTLI", lib: "brotli" },
    libbrotlidec: { def: os.platform === "darwin" ? "" : "-DCONFIG_BROTLI", lib: "brotli" },
};
const SMOKE = `let ok = 0;
for (const x of n) { const m = await import(x); if (Object.keys(m).length) ok++; else throw new Error(x + " has no exports"); }
const probes = [], has = (m) => sel.includes(m);
const fail = (m) => { throw new Error("probe " + m); };
const rej = async (p) => { try { await p; return null; } catch (e) { return e; } };
const names = (e) => e instanceof TypeError ? e.message : String(e);
if (typeof URLSearchParams !== "function" || typeof FormData !== "function" || typeof fetch !== "function") fail("core web globals missing");
if (new URLSearchParams("a=1&a=2").getAll("a").length !== 2) fail("core URLSearchParams");
if (!/^multipart\\/form-data; boundary=/.test(new Response(new FormData()).headers.get("content-type"))) fail("core FormData multipart");
probes.push("core-web-globals");
if (typeof URL !== "function" || new URL("../b?q=1", "http://h/a/c").href !== "http://h/b?q=1" || !URL.canParse("x:y")) fail("core URL global");
if (has("url") && (await import("dyna:url")).URL !== URL) fail("dyna:url URL is not the core URL global");
probes.push("core-url");
if (has("net")) {
    const nm = await import("dyna:net");
    for (const k of ["Redis", "PostgreSQL", "SQLite", "HTTPClient", "App", "fetch", "DNSResolver", "TCPProxy", "Prefix", "parseAddr", "Metrics"]) if (k in nm) fail("dyna:net still exports " + k);
    if (typeof nm.TCPServer !== "function" || typeof nm.RateLimiter !== "function") fail("dyna:net lost TCPServer/RateLimiter");
    probes.push("net-sockets-only");
}
if (has("netip")) { const m = await import("dyna:netip"); if (m.canonical("::0:1") !== "::1" || typeof m.Prefix !== "function") fail("dyna:netip"); probes.push("netip"); }
if (has("dns")) { if (typeof (await import("dyna:dns")).DNSResolver !== "function") fail("dyna:dns DNSResolver"); probes.push("dns"); }
if (has("proxy")) { if (typeof (await import("dyna:proxy")).TCPProxy !== "function") fail("dyna:proxy TCPProxy"); probes.push("proxy"); }
if (has("metrics")) { const { Metrics } = await import("dyna:metrics"); Metrics.counter("mx_probe_total", 1); if (!/mx_probe_total/.test(Metrics.scrape())) fail("dyna:metrics scrape"); probes.push("metrics"); }
if (has("http")) {
    const h = await import("dyna:http");
    if (h.fetch !== fetch || typeof h.App !== "function") fail("dyna:http fetch is not the global");
    if (typeof WebSocket !== "function" || WebSocket !== h.WsClient) fail("WebSocket global is not dyna:http WsClient");
    probes.push("http");
} else if (typeof WebSocket !== "undefined") fail("WebSocket global without http");
if (has("sqlite")) { if (typeof (await import("dyna:sqlite")).SQLite !== "function") fail("dyna:sqlite SQLite"); probes.push("sqlite"); }
if (has("postgres")) { if (typeof (await import("dyna:postgres")).PostgreSQL !== "function") fail("dyna:postgres PostgreSQL"); probes.push("postgres"); }
if (has("redis")) { if (typeof (await import("dyna:redis")).Redis !== "function") fail("dyna:redis Redis"); probes.push("redis"); }
if (!has("http")) {
    const e = await rej(fetch("http://127.0.0.1:9/"));
    if (!(e instanceof TypeError) || !/"http" module/.test(e.message)) fail("fetch without http: " + names(e));
    probes.push("fetch-names-http");
} else if (!has("tls")) {
    const e = await rej(fetch("https://127.0.0.1:9/"));
    if (!(e instanceof TypeError) || !/"tls" module/.test(e.message)) fail("https fetch without tls: " + names(e));
    let w = null;
    try { new WebSocket("wss://127.0.0.1:9/", {}); } catch (x) { w = x; }
    if (!(w instanceof TypeError) || !/"tls" module/.test(w.message)) fail("wss without tls: " + names(w));
    const { HTTPClient } = await import("dyna:http");
    for (const [what, call] of [["HTTPClient.get", (c) => c.get("https://127.0.0.1:9/")], ["HTTPClient.getStream", (c) => c.getStream("https://127.0.0.1:9/")]]) {
        let x = null;
        const c = new HTTPClient();
        try { await call(c); } catch (y) { x = y; } finally { c.close(); }
        if (!(x instanceof TypeError) || !/"tls" module/.test(x.message)) fail(what + " https without tls: " + names(x));
    }
    probes.push("https-names-tls", "wss-names-tls", "client-get-names-tls", "client-stream-names-tls");
} else {
    const e = await rej(fetch("https://127.0.0.1:9/"));
    if (!e || /"tls" module/.test(e.message)) fail("https fetch with tls: " + names(e));
    probes.push("https-has-tls");
}
if (has("crypto")) {
    const c = await import("dyna:crypto");
    if (typeof c.AESGCM !== "function") fail("crypto lacks the OpenSSL-backed AESGCM");
    probes.push("crypto-openssl");
}
print("smoke " + ok + "/" + n.length + " probes " + probes.join(","));
`;
function libOfSymbol(s) {
    s = s.replace(/^_/, "");
    if (/^(SSL|EVP|OPENSSL|OSSL|X509|BIO|ERR|PEM|RAND|BN|EC|ECDSA|RSA|DH|HMAC|CRYPTO|ASN1|PKCS|d2i|i2d|TLS|DTLS|OBJ|CMS|SRTP)_/.test(s) || /^(SSL|DTLS|TLS)_?[a-z]/.test(s)) return "openssl";
    if (/^sqlite3_/.test(s)) return "sqlite3";
    if (/^ZSTD/.test(s)) return "libzstd";
    if (/^Brotli/.test(s)) return "brotli";
    if (/^compression_/.test(s)) return "libcompression";
    return null;
}

function ownerOf(mods, src) {
    for (const m of mods.values()) for (const e of m.src) {
        const p = e.split("@")[0];
        if (p.endsWith("/") ? src.startsWith(p) : src === p) return m.name;
    }
    return null;
}

function kitIn(list, src) {
    let hit = false;
    for (const e of list) {
        const neg = e.startsWith("!"), p = neg ? e.slice(1) : e;
        const m = p.endsWith("/") ? src.startsWith(p) : /[*{]/.test(p) ? new RegExp("^" + p.replace(/[.+?^$()|[\]\\]/g, "\\$&").replace(/\*/g, "[^/]*").replace(/\{([^}]*)\}/g, (_, a) => "(?:" + a.split(",").join("|") + ")") + "$").test(src) : src === p;
        if (m) { if (neg) return false; hit = true; }
    }
    return hit;
}

function kitOwners(mods, src) {
    const r = [];
    for (const m of mods.values()) if (kitIn(m.kits, src)) r.push(m.name);
    return r;
}

function closure(mods, names) {
    const seen = new Set(), st = [...names];
    while (st.length) { const n = st.pop(); if (seen.has(n)) continue; seen.add(n); for (const d of mods.get(n).deps) st.push(d); }
    return seen;
}

function main() {
    if (scriptArgs[1] === "selftest") {
        const real = exists(".obj/release/rules.mk") ? read(".obj/release/rules.mk") : null;
        const r = rulesSelftest(real);
        const n = real === null ? 0 : compileRules(real).length;
        if (real !== null && n < 100) r.bad.push("compileRules found only " + n + " rules in the real rules.mk");
        for (const b of r.bad) print("FAIL: " + b);
        print("module-matrix selftest: " + (r.checks + 1) + " checks, " + r.bad.length + " failed; real rules.mk " + (real === null ? "absent (skipped)" : n + " compile rules"));
        std.exit(r.bad.length ? 1 : 0);
    }
    const o = parseArgs(scriptArgs.slice(1));
    const t0 = Date.now();
    const rel = ".obj/release";
    for (const f of ["flags.env", "rules.mk"]) if (!exists(rel + "/" + f)) die("needs " + rel + "/" + f + ": run ./build.sh release first");
    if (!exists(".obj/repl_src.inc")) die("needs .obj/repl_src.inc: run ./build.sh release first");
    const mods = readManifest(read("tools/modules.tsv"));
    const env = readEnvFile(read(rel + "/flags.env"));
    const rules = compileRules(read(rel + "/rules.mk"));
    if (rules.length < 100) die("rules.mk has only " + rules.length + " compile rules; the release rule format changed");
    const lg = libGroups(env.LIBS);
    const O = o.outAbs;
    must("mkdir -p " + q(O + "/cache") + " " + q(O + "/cmd") + " " + q(O + "/cfg"));
    const srcs = [];
    rules.forEach((r, i) => { srcs.push(r.src); write(O + "/cmd/" + i, stripCmd(r.cmd)); write(O + "/cmd/" + i + ".src", r.src); });

    const units = [...mods.values()].filter((m) => (o.ext || m.kind !== "ext") && (!o.only || o.only.includes(m.name)));
    const skipped = [...mods.values()].filter((m) => m.kind === "ext" && !o.ext).map((m) => m.name);
    const allMods = [...mods.values()].filter((m) => o.ext || m.kind !== "ext").map((m) => m.name);
    for (const b of o.base) if (!mods.has(b)) die("--base: unknown module " + b);
    const base = o.base.length ? closure(mods, o.base) : new Set();
    const configs = [{ name: "core", sel: [...base] }];
    for (const u of units) configs.push({ name: u.name, sel: [...new Set([...base, ...(o.deps ? closure(mods, [u.name]) : [u.name])])], unit: u.name });
    if (!o.only) configs.push({ name: "all", sel: allMods });

    const jobs = [];
    for (const c of configs) {
        const sel = new Set(c.sel);
        const pkgs = new Set();
        for (const n of sel) for (const p of mods.get(n).pkg) pkgs.add(p);
        const defs = [...sel].map((n) => (mods.get(n).kind === "feature" ? "-DCONFIG_" : "-DCONFIG_NATIVE_MODULE_") + n.toUpperCase());
        const libs = [...lg.base];
        const libSet = new Set();
        for (const p of pkgs) { const k = PKG[p]; if (!k) continue; if (k.def && !defs.includes(k.def)) defs.push(k.def); libSet.add(k.lib); }
        for (const k of libSet) libs.push(...lg[k]);
        c.files = [];
        srcs.forEach((s, i) => {
            const own = ownerOf(mods, s);
            if (own && !sel.has(own) && !kitOwners(mods, s).some((x) => sel.has(x))) return;
            if (!own) { const ko = kitOwners(mods, s); if (ko.length && !ko.some((x) => sel.has(x))) return; }
            if (own) {
                const e = mods.get(own).src.find((x) => { const p = x.split("@")[0]; return p.endsWith("/") ? s.startsWith(p) : s === p; });
                const need = e.split("@")[1];
                if (need && !sel.has(need)) return;
            }
            c.files.push(i);
        });
        c.pkgs = [...pkgs];
        c.libs = libs;
        c.dir = O + "/cfg" + (o.tag ? "-" + o.tag : "") + "/" + c.name;
        must("rm -rf " + q(c.dir) + " && mkdir -p " + q(c.dir + "/k"));
        write(c.dir + "/defs", defs.join(" "));
        for (const i of c.files) jobs.push(c.name + " " + i);
    }
    write(O + "/job.sh", [
        "#!/usr/bin/env bash",
        "O=" + q(O) + "; cfg=$1; i=$2",
        "cmd=\"$(cat \"$O/cmd/$i\") $(cat \"$O/cfg" + (o.tag ? "-" + o.tag : "") + "/$cfg/defs\")\"; src=$(cat \"$O/cmd/$i.src\")",
        "pre=$(eval \"$cmd -E $src\" 2>\"$O/cfg" + (o.tag ? "-" + o.tag : "") + "/$cfg/k/$i.eerr\"); rc=$?",
        "if [ $rc != 0 ]; then echo \"ERR-$cfg-$i\" > \"$O/cfg" + (o.tag ? "-" + o.tag : "") + "/$cfg/k/$i\"; exit 0; fi",
        "key=$(printf '%s' \"$pre\" | shasum | cut -c1-24); rm -f \"$O/cfg" + (o.tag ? "-" + o.tag : "") + "/$cfg/k/$i.eerr\"",
        "echo \"$key\" > \"$O/cfg" + (o.tag ? "-" + o.tag : "") + "/$cfg/k/$i\"",
        "[ -f \"$O/cache/$key.o\" ] && exit 0",
        "if eval \"$cmd -c -o $O/cache/$key.o.$$ $src\" 2>\"$O/cache/$key.log.$$\"; then mv \"$O/cache/$key.o.$$\" \"$O/cache/$key.o\"; mv \"$O/cache/$key.log.$$\" \"$O/cache/$key.log\"; else mv \"$O/cache/$key.log.$$\" \"$O/cache/$key.err\"; rm -f \"$O/cache/$key.o.$$\"; fi",
    ].join("\n") + "\n");
    write(O + "/jobs.txt", jobs.join("\n") + "\n");
    const cachedBefore = +sh("ls " + q(O + "/cache") + " | grep -c '\\.o$'").out.trim() || 0;
    say(configs.length + " configurations, " + jobs.length + " compile jobs, -j " + o.jobs + ", " + cachedBefore + " objects already cached");
    const tc = Date.now();
    must("xargs -P " + o.jobs + " -n 2 bash " + q(O + "/job.sh") + " < " + q(O + "/jobs.txt"));
    const compileMs = ms(tc);
    const cachedAfter = +sh("ls " + q(O + "/cache") + " | grep -c '\\.o$'").out.trim() || 0;
    say("compile phase " + (compileMs / 1000).toFixed(1) + "s, " + (cachedAfter - cachedBefore) + " objects compiled");

    for (const c of configs) {
        c.keys = c.files.map((i) => ({ i, src: srcs[i], key: (read(c.dir + "/k/" + i) || "").trim() }));
        c.compileErr = [];
        c.warnings = 0;
        for (const k of c.keys) {
            if (k.key.startsWith("ERR-") || !exists(O + "/cache/" + k.key + ".o")) {
                const e = read(c.dir + "/k/" + k.i + ".eerr") || read(O + "/cache/" + k.key + ".err") || "";
                c.compileErr.push({ src: k.src, msg: (e.split("\n").find((l) => / error: /.test(l)) || e.slice(0, 200)).trim() });
            } else {
                const lg2 = read(O + "/cache/" + k.key + ".log") || "";
                c.warnings += (lg2.match(/ warning: /g) || []).length;
            }
        }
    }

    const all = configs.find((c) => c.name === "all") || null;
    const symOwner = new Map();
    const nmCache = new Map();
    const nmKeys = (keys) => {
        const need = [...new Set(keys)].filter((k) => !nmCache.has(k) && exists(O + "/cache/" + k + ".o"));
        for (let s = 0; s < need.length; s += 200) {
            const part = need.slice(s, s + 200);
            const r = run("/usr/bin/nm", ["-A", ...part.map((k) => O + "/cache/" + k + ".o")]);
            for (const k of part) nmCache.set(k, { def: [], und: [] });
            for (const line of r.out.split("\n")) {
                const m = /\/cache\/([0-9a-f]+)\.o:\s+(?:[0-9a-f]+\s+)?([A-Za-z])\s+(\S+)$/.exec(line);
                if (!m) continue;
                const e = nmCache.get(m[1]);
                if (m[2] === "U") e.und.push(m[3]);
                else if (m[2] === m[2].toUpperCase()) e.def.push(m[3]);
            }
        }
    };
    const ownerConfigs = all ? [all] : configs;
    for (const c of ownerConfigs) {
        nmKeys(c.keys.map((k) => k.key));
        for (const k of c.keys) { const e = nmCache.get(k.key); if (e) for (const s of e.def) if (!symOwner.has(s)) symOwner.set(s, k.src); }
    }
    if (!all) say("no \"all\" configuration (--only): symbols are attributed from the selected configurations only");

    const linkJobs = [];
    for (const c of configs) {
        const objs = c.keys.filter((k) => exists(O + "/cache/" + k.key + ".o")).map((k) => O + "/cache/" + k.key + ".o");
        write(c.dir + "/objs.list", objs.join("\n") + "\n");
        write(c.dir + "/link.sh", "#!/usr/bin/env bash\ncd " + q(ROOT) + "\ns=$(perl -MTime::HiRes=time -e 'printf \"%d\", time*1000')\n" +
            env.CC + " " + env.LDF + " -o " + q(c.dir + "/dynajs") + " $(cat " + q(c.dir + "/objs.list") + ") " + c.libs.join(" ") + " > " + q(c.dir + "/link.log") + " 2>&1\necho $? > " + q(c.dir + "/link.rc") +
            "\necho $(( $(perl -MTime::HiRes=time -e 'printf \"%d\", time*1000') - s )) > " + q(c.dir + "/link.ms") + "\n");
        linkJobs.push(c.dir + "/link.sh");
    }
    write(O + "/links.txt", linkJobs.join("\n") + "\n");
    const tl = Date.now();
    must("xargs -P " + Math.max(1, Math.min(6, o.jobs >> 1)) + " -n 1 bash < " + q(O + "/links.txt"));
    say("link phase " + (ms(tl) / 1000).toFixed(1) + "s");

    const headers = must("find src -name '*.h'").split("\n").filter(Boolean).map((p) => ({ p, lines: read(p).split("\n") }));
    const gateOf = (ident) => gateScan(ident, true) || gateScan(ident, false);
    const gateScan = (ident, declOnly) => {
        const re = new RegExp("\\b" + ident + "\\b");
        const decl = new RegExp("(typedef\\b.*\\b" + ident + "\\s*;|^\\s*(typedef\\s+)?struct\\s+" + ident + "\\b|^[A-Za-z_][\\w \\*]*\\b" + ident + "\\s*\\(|^extern\\b.*\\b" + ident + "\\b|^\\}\\s*" + ident + "\\s*;)");
        for (const h of headers) {
            const stack = [];
            for (let n = 0; n < h.lines.length; n++) {
                const l = h.lines[n];
                if (/^#\s*if/.test(l)) stack.push(l);
                else if (/^#\s*endif/.test(l)) stack.pop();
                else if (re.test(l) && !/^#/.test(l) && (!declOnly || decl.test(l))) {
                    const g = stack.flatMap((x) => [...x.matchAll(/CONFIG_NATIVE_MODULE_([A-Z0-9_]+)/g)].map((y) => y[1].toLowerCase()));
                    return { at: h.p + ":" + (n + 1), mods: g.filter((x) => mods.has(x)) };
                }
            }
        }
        return null;
    };
    const keyToSrc = new Map();
    for (const c of configs) for (const k of c.keys) keyToSrc.set(k.key, k.src);
    const rows = [];
    const lines = [];
    for (const c of configs) {
        c.linkRc = +(read(c.dir + "/link.rc") || "1").trim();
        c.linkMs = +(read(c.dir + "/link.ms") || "0").trim();
        const log = read(c.dir + "/link.log") || "";
        const und = [];
        const re = /^\s+"([^"]+)", referenced from:\n((?:\s+\S.* in \S+\n?)+)/gm;
        let m;
        while ((m = re.exec(log))) {
            const refs = [...m[2].matchAll(/ in (?:.*\/)?([0-9a-f]+)\.o/g)].map((x) => keyToSrc.get(x[1]) || x[1]);
            und.push({ sym: m[1], refs: [...new Set(refs)] });
        }
        for (const x of log.matchAll(/^(\S+?\.o):.*undefined reference to `([^']+)'/gm)) {
            const k = /([0-9a-f]+)\.o$/.exec(x[1]);
            und.push({ sym: x[2], refs: [k ? keyToSrc.get(k[1]) || x[1] : x[1]] });
        }
        const needMods = new Map(), needLibs = new Map(), leaks = [], unknown = [];
        for (const u of und) {
            const own = symOwner.get(u.sym);
            const om = own ? ownerOf(mods, own) : null;
            const lib = own ? null : libOfSymbol(u.sym);
            for (const r of u.refs) {
                const rm = ownerOf(mods, r) || "core";
                if (om) {
                    if (!needMods.has(om)) needMods.set(om, new Set());
                    needMods.get(om).add(r + " -> " + u.sym.replace(/^_/, "") + " (" + own + ")");
                    if (rm === "core") leaks.push(r + " -> " + u.sym.replace(/^_/, "") + " [" + om + "]");
                } else if (lib) {
                    if (!needLibs.has(lib)) needLibs.set(lib, new Set());
                    needLibs.get(lib).add(r);
                    if (rm === "core") leaks.push(r + " -> " + u.sym.replace(/^_/, "") + " [lib " + lib + "]");
                } else if (own && kitOwners(mods, own).length) {
                    const ko = kitOwners(mods, own);
                    for (const x of ko) { if (!needMods.has(x)) needMods.set(x, new Set()); needMods.get(x).add(r + " -> " + u.sym.replace(/^_/, "") + " (kit " + own + ", listed by " + ko.join(" ") + ")"); }
                    if (rm === "core") leaks.push(r + " -> " + u.sym.replace(/^_/, "") + " [kit " + own + "]");
                } else if (own) {
                    leaks.push(r + " -> " + u.sym.replace(/^_/, "") + " [core file " + own + " not linked?]");
                } else unknown.push(r + " -> " + u.sym);
            }
        }
        for (const e of c.compileErr) {
            const m2 = /(?:undeclared identifier|unknown type name|undeclared function|incomplete type) '([A-Za-z_][A-Za-z0-9_]*)'/.exec(e.msg);
            const g = m2 && gateOf(m2[1]);
            if (g && !g.mods.length) {
                const hb = g.at.split(":")[0].split("/").pop();
                const dir = e.src.slice(0, e.src.lastIndexOf("/") + 1);
                for (const h of headers.filter((x) => x.p.startsWith(dir))) {
                    const stack = [];
                    h.lines.forEach((l, n) => {
                        if (/^#\s*if/.test(l)) stack.push(l);
                        else if (/^#\s*endif/.test(l)) stack.pop();
                        else if (new RegExp("^#\\s*include\\s+\"" + hb.replace(/\./g, "\\.") + "\"").test(l)) {
                            const gm = stack.flatMap((x) => [...x.matchAll(/CONFIG_NATIVE_MODULE_([A-Z0-9_]+)/g)].map((y) => y[1].toLowerCase())).filter((x) => mods.has(x));
                            if (gm.length) { g.mods.push(...gm); g.at = g.at + " via the gated include at " + h.p + ":" + (n + 1); }
                        }
                    });
                }
            }
            if (!g || !g.mods.length) { unknown.push(e.src + " -> compile: " + (m2 ? m2[1] + (g ? " declared ungated at " + g.at : " (no declaration found)") : e.msg.slice(0, 120))); continue; }
            for (const gm of g.mods) {
                if (c.sel.includes(gm)) continue;
                if (!needMods.has(gm)) needMods.set(gm, new Set());
                needMods.get(gm).add(e.src + " -> " + m2[1] + " (declared under the " + gm + " gate at " + g.at + ")");
                if ((ownerOf(mods, e.src) || "core") === "core") leaks.push(e.src + " -> " + m2[1] + " [" + gm + " gate]");
            }
        }
        let smoke = "not linked";
        if (c.linkRc === 0) {
            const names = c.sel.flatMap((n) => mods.get(n).exports);
            const js = c.dir + "/smoke.js";
            write(js, "const n = " + JSON.stringify(names) + ", sel = " + JSON.stringify(c.sel) + ";\n" + SMOKE);
            const r = run("/bin/bash", ["-c", "tools/bounded-run.sh 60 60 module-matrix -- " + q(c.dir + "/dynajs") + " --std " + q(js)]);
            smoke = r.code === 0 ? r.out.trim() : "FAIL rc=" + r.code + " " + (r.err || r.out).trim().split("\n")[0].slice(0, 160);
        }
        c.und = und; c.needMods = needMods; c.needLibs = needLibs; c.leaks = leaks; c.unknown = unknown; c.smoke = smoke;
        const coreNeed = configs[0].needMods ? new Set(configs[0].needMods.keys()) : new Set();
        c.beyond = [...needMods.entries()].filter(([x, set]) => x !== c.unit && (c === configs[0] || [...set].some((e) => (ownerOf(mods, e.split(" -> ")[0]) || "core") !== "core"))).map(([x]) => x).sort();
        const dash = (a) => a.length ? a.join(" ") : "-";
        rows.push([c.name, dash(c.beyond), dash([...needMods.keys()].sort()), dash([...needLibs.keys()].sort()), dash([...new Set(leaks.map((l) => l.split(" -> ")[0]))]),
            c.compileErr.length, c.warnings, c.linkRc, und.length, c.linkMs, smoke].join("\t"));
    }

    const reach = (c) => {
        nmKeys(c.keys.map((k) => k.key));
        const def = new Map();
        for (const k of c.keys) { const e = nmCache.get(k.key); if (e) for (const s of e.def) if (!def.has(s)) def.set(s, k.key); }
        const mainKey = def.get("_main") || def.get("main");
        const seen = new Set();
        const st = mainKey ? [mainKey] : [];
        while (st.length) {
            const k = st.pop();
            if (seen.has(k)) continue;
            seen.add(k);
            const e = nmCache.get(k);
            if (e) for (const s of e.und) { const d = def.get(s); if (d && !seen.has(d)) st.push(d); }
        }
        return new Set(c.keys.filter((k) => seen.has(k.key)).map((k) => k.src));
    };
    const core = configs[0];
    const coreReach = core.linkRc === 0 ? reach(core) : null;
    const corePool = core.keys.map((k) => k.src);
    for (const c of configs) if (c !== core && c.linkRc === 0 && coreReach) {
        const r = reach(c);
        c.pulls = corePool.filter((s) => r.has(s) && !coreReach.has(s));
    }

    lines.push("module-matrix " + new Date().toISOString() + " on " + os.platform + ", -j " + o.jobs + (o.deps ? ", with manifest deps closure" : ", core + exactly one module"));
    lines.push("compile phase " + (compileMs / 1000).toFixed(1) + "s (" + (cachedAfter - cachedBefore) + " objects compiled, " + cachedAfter + " cached), total " + (ms(t0) / 1000).toFixed(1) + "s");
    if (base.size) lines.push("baseline added to every configuration (--base): " + [...base].join(" "));
    if (skipped.length) lines.push("not built (opt-in extensions, --ext to include): " + skipped.join(" "));
    lines.push("");
    for (const c of configs) {
        lines.push("== " + c.name + (c.unit ? "" : "") + "  modules: " + (c.sel.join(" ") || "-") + "  pkgs: " + (c.pkgs.join(" ") || "-"));
        lines.push("   files " + c.files.length + ", compile errors " + c.compileErr.length + ", warnings " + c.warnings + ", link rc " + c.linkRc + " in " + c.linkMs + "ms, undefined " + c.und.length + ", " + c.smoke);
        for (const e of c.compileErr.slice(0, 8)) lines.push("   compile error: " + e.src + ": " + e.msg.slice(0, 200));
        for (const [mm, set] of c.needMods) { lines.push("   needs module " + mm + ":"); for (const x of [...set].slice(0, 6)) lines.push("      " + x); if (set.size > 6) lines.push("      ... " + (set.size - 6) + " more"); }
        for (const [l, set] of c.needLibs) lines.push("   needs lib " + l + ": " + [...set].slice(0, 6).join(" ") + (set.size > 6 ? " ... " + (set.size - 6) + " more" : ""));
        for (const x of [...new Set(c.leaks)].slice(0, 12)) lines.push("   core leak: " + x);
        for (const x of c.unknown.slice(0, 6)) lines.push("   unattributed: " + x);
        if (c.pulls && c.pulls.length) lines.push("   pulls core-pool files the core build never reaches: " + c.pulls.join(" "));
    }
    if (coreReach) {
        const dead = corePool.filter((s) => !coreReach.has(s));
        lines.push("");
        lines.push("core-pool files unreachable from main in the core build (" + dead.length + "): " + dead.join(" "));
    }
    const nat = read("src/dyna-nat.c").split("\n");
    const regDeps = [];
    for (const l of nat) {
        const ms2 = [...l.matchAll(/defined\((CONFIG_[A-Z0-9_]+)\)/g)].map((x) => x[1]);
        if (/^#\s*if/.test(l) && ms2.length > 1) {
            const [first, ...rest] = ms2.map((x) => x.startsWith("CONFIG_NATIVE_MODULE_") ? x.slice(21).toLowerCase() : x === "CONFIG_TLS" ? "tls" : x === "CONFIG_OPENSSL" ? "openssl" : x);
            regDeps.push(first + " is registered only with " + rest.join(" and ") + " (src/dyna-nat.c: " + l.trim() + ")");
        }
    }
    lines.push("");
    lines.push("registration gates (a module that silently is not registered without the other):");
    for (const r of regDeps) lines.push("   " + r);
    const T = o.tag ? "-" + o.tag : "";
    write(O + "/report" + T + ".txt", lines.join("\n") + "\n");
    write(O + "/deps" + T + ".tsv", "config\tneeds_beyond_core\tneeds_modules\tneeds_libs\tleaking_core_files\tcompile_errors\twarnings\tlink_rc\tundefined\tlink_ms\tsmoke\n" + rows.join("\n") + "\n");
    print(lines.join("\n"));
    say("report " + O + "/report" + (o.tag ? "-" + o.tag : "") + ".txt, deps" + (o.tag ? "-" + o.tag : "") + ".tsv; total " + (ms(t0) / 1000).toFixed(1) + "s");
}

main();
