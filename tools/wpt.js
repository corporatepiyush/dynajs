// tools/wpt.js -- run web-platform-tests *.any.js files under dynajs (owner rule 12: spec conformance first).
//
//   ./dynajs --std tools/wpt.js [--wpt DIR] [--dirs streams,encoding,...] [--jobs N] [--filter SUBSTR]
//            [--expect tests/wpt-expectations.tsv] [--write-expect FILE] [--json FILE] [--bin ./dynajs]
//
// WPT lives outside the tree (git-ignored sparse clone): --wpt DIR or WPT_DIR, default third_party/wpt
// under the main checkout. Each test variant runs in its own child process (crash and hang isolation):
//   <test>.any.html[?variant]         the "window" global, run as testharness's shell environment
//   <test>.any.worker.html[?variant]  the "dedicatedworker" global, run inside an os.Worker of the child
// The child evaluates, in one task: a host shim (self, location, GLOBAL), resources/testharness.js, a report
// hook (add_result_callback / add_completion_callback in place of testharnessreport.js), every `// META: script=`
// include, then the test. Results stream back as "@@WPT <json>" lines, so a child killed by its timeout
// (`// META: timeout=long` = 120 s, else 30 s) still reports the subtests that finished.
// fetch() of a quoted relative path (a fixture such as resources/urltestdata.json) is served from disk by the child.
// Skipped, with the reason in the table and the expectations: fetch/ and websockets/ and anything that talks
// to wptserve (needs-server), tests that use a browser DOM (needs-dom), idlharness (fetches /interfaces from
// the server), and globals other than window/dedicatedworker (sharedworker, serviceworker, shadowrealm).
// /common/subset-tests.js and subset-tests-by-key.js are left out of the needs-dom scan: their only DOM code runs
// under ?split= / ?get-keys (meta generators no META variant uses), so url-constructor, url-setters,
// api-invalid-label and textdecoder-fatal-single-byte run instead of being skipped.
// --expect compares against the baseline: a subtest whose expected status was PASS and is now anything else,
// or a test/subtest that is now missing, is a regression (exit 1); new passes only print.
import * as std from "std";
import * as os from "os";
import * as sys from "dyna:sys";
import * as file from "dyna:file";

const HERE = "tools/wpt.js";
const ST = ["PASS", "FAIL", "TIMEOUT", "NOTRUN", "PRECONDITION_FAILED"];
const HST = ["OK", "ERROR", "TIMEOUT", "PRECONDITION_FAILED"];

function readText(p) { const t = std.loadFile(p); if (t === null) throw new Error("wpt: cannot read " + p); return t; }
function dirname(p) { const i = p.lastIndexOf("/"); return i < 0 ? "." : p.slice(0, i); }
function normalize(p) {
    const out = [];
    for (const s of p.split("/")) {
        if (s === "..") out.pop();
        else if (s !== "." && s !== "") out.push(s);
    }
    return (p.startsWith("/") ? "/" : "") + out.join("/");
}

function meta(text) {
    const m = { global: null, scripts: [], variants: [], long: false, title: null };
    for (const line of text.split("\n")) {
        const r = /^\/\/ META: *(\w+)=(.*)$/.exec(line.trim());
        if (!r) { if (line.trim() && !line.startsWith("//")) break; continue; }
        const [, k, v] = r;
        if (k === "global") m.global = v.split(",").map((s) => s.trim()).filter(Boolean);
        else if (k === "script") m.scripts.push(v.trim());
        else if (k === "variant") m.variants.push(v.trim());
        else if (k === "timeout") m.long = v.trim() === "long";
        else if (k === "title") m.title = v.trim();
    }
    return m;
}

function includePath(wpt, testAbs, inc) {
    return inc.startsWith("/") ? normalize(wpt + inc) : normalize(dirname(testAbs) + "/" + inc);
}

// ---- child side ---------------------------------------------------------------------------------------------

function emit(sink, kind, data) { sink("@@WPT " + JSON.stringify({ kind, ...data })); }

function runHarness(wpt, rel, variant, globalName, sink) {
    const abs = wpt + "/" + rel;
    const text = readText(abs);
    const m = meta(text);
    const g = globalThis;
    g.self = g;
    const href = "http://web-platform.test:8000/" + rel.replace(/\.js$/, globalName === "window" ? ".html" : ".worker.html") + variant;
    g.location = { href, search: variant, pathname: "/" + rel, protocol: "http:", host: "web-platform.test:8000",
        hostname: "web-platform.test", port: "8000", origin: "http://web-platform.test:8000", hash: "", toString() { return href; } };
    g.GLOBAL = { isWindow: () => globalName === "window", isWorker: () => globalName !== "window", isShadowRealm: () => false };
    const realFetch = g.fetch;
    g.fetch = function (input, init) {
        const u = typeof input === "string" ? input : null;
        if (u === null || /^[a-z][a-z0-9+.-]*:/i.test(u) || u.startsWith("//")) return realFetch.call(this, input, init);
        const path = u.split(/[?#]/)[0];
        const disk = path.startsWith("/") ? normalize(wpt + path) : normalize(dirname(abs) + "/" + path);
        let bytes = null;
        try { bytes = file.readFile(new file.Path(disk), { bytes: true }); } catch {}
        if (bytes === null) return Promise.resolve(new Response("", { status: 404 }));
        const type = /\.json$/.test(path) ? "application/json" : /\.js$/.test(path) ? "text/javascript"
            : /\.html?$/.test(path) ? "text/html" : /\.txt$/.test(path) ? "text/plain" : "application/octet-stream";
        return Promise.resolve(new Response(bytes, { status: 200, headers: { "content-type": type } }));
    };
    if (globalName !== "window") g.importScripts =(...ps) => { for (const p of ps) std.evalScript(readText(includePath(wpt, abs, p))); };
    let done = false;
    std.evalScript(readText(wpt + "/resources/testharness.js"));
    g.add_result_callback((t) => emit(sink, "result", { name: t.name, status: ST[t.status] || String(t.status), message: t.message || "" }));
    g.add_completion_callback((tests, st) => {
        if (done) return;
        done = true;
        emit(sink, "done", { status: HST[st.status] || String(st.status), message: st.message || "", n: tests.length });
    });
    try {
        for (const inc of m.scripts) std.evalScript(readText(includePath(wpt, abs, inc)));
        std.evalScript(text);
    } catch (e) {
        emit(sink, "error", { message: String(e && e.stack || e) });
        if (!done) { done = true; emit(sink, "done", { status: "ERROR", message: "uncaught: " + String(e && e.message || e), n: -1 }); }
    }
    return () => done;
}

async function childMain(args) {
    const [wpt, rel, variant, globalName] = args;
    if (globalName === "window") {
        const isDone = runHarness(wpt, rel, variant, "window", print);
        await waitDone(isDone);
        std.exit(0);
    }
    const w = new os.Worker(HERE);
    w.onmessage = (ev) => {
        const d = ev.data;
        if (d.line) print(d.line);
        if (d.exit) std.exit(0);
    };
    w.postMessage({ wpt, rel, variant, globalName });
}

async function waitDone(isDone) {
    while (!isDone()) await new Promise((r) => setTimeout(r, 5));
}

function workerMain() {
    const parent = os.Worker.parent;
    parent.onmessage = async (ev) => {
        const { wpt, rel, variant, globalName } = ev.data;
        const sink = (line) => parent.postMessage({ line });
        const isDone = runHarness(wpt, rel, variant, globalName, sink);
        await waitDone(isDone);
        parent.postMessage({ exit: true });
    };
}

// ---- parent side --------------------------------------------------------------------------------------------

function listAny(wpt, dir) {
    const out = [];
    const walk = (d) => {
        const [names, err] = os.readdir(wpt + "/" + d);
        if (err) return;
        for (const n of names.sort()) {
            if (n === "." || n === "..") continue;
            const rel = d + "/" + n;
            const [st] = os.stat(wpt + "/" + rel);
            if (st && (st.mode & os.S_IFMT) === os.S_IFDIR) walk(rel);
            else if (n.endsWith(".any.js")) out.push(rel);
        }
    };
    walk(dir);
    return out;
}

const SERVER_RE = /get_host_info|\.py\b|RESOURCES_DIR|\/fetch\/api\/resources|new WebSocket|EventSource|XMLHttpRequest|wptserve|\{\{host\}\}|\.sub\.js/;
const SUBSET_RE = /^\/common\/subset-tests(?:-by-key)?\.js$/;
const DOM_RE = /\bdocument\.|\bwindow\.(?:document|open)|createElement\(|\bnew (?:Image|MessageChannel|BroadcastChannel)\b|navigator\.serviceWorker/;

// A test whose every fetch() argument is a quoted relative path (a fixture beside it, e.g. resources/urltestdata.json)
// runs: the child serves those from disk. Any other fetch() (a variable, a URL, a .py handler) needs wptserve.
function localFetchOnly(text) {
    return [...text.matchAll(/\bfetch\(([^)]*)\)/g)].every((m) => /^(["'])[^"':]+\1$/.test(m[1].trim()) && !/\.py\b|^["']\/\//.test(m[1].trim()));
}

function plan(wpt, dirs, filter) {
    const runs = [];
    for (const dir of dirs) {
        for (const rel of listAny(wpt, dir)) {
            if (filter && !rel.includes(filter)) continue;
            const abs = wpt + "/" + rel;
            const text = readText(abs);
            const m = meta(text);
            const incText = m.scripts.filter((s) => !SUBSET_RE.test(s)).map((s) => std.loadFile(includePath(wpt, abs, s)) || "").join("\n");
            const globals = m.global || ["window", "dedicatedworker"];
            const want = [];
            for (const g of globals) {
                if (g === "window" || g === "jsshell") want.push("window");
                else if (g === "worker" || g === "dedicatedworker") want.push("worker");
            }
            const variants = m.variants.length ? m.variants : [""];
            let skip = null;
            if (/^websockets\//.test(rel) || (/^fetch\//.test(rel) && !/^fetch\/api\/(headers|request|response|body|abort)\//.test(rel))) skip = "needs-server";
            else if (/^xhr\//.test(rel) && !/^xhr\/formdata\//.test(rel)) skip = "needs-server";
            else if (/\bidl_test\(|idlharness/.test(text + m.scripts.join(" "))) skip = "needs-server (idlharness fetches /interfaces)";
            else if (SERVER_RE.test(text) || !localFetchOnly(text)) skip = "needs-server";
            else if (DOM_RE.test(text) || DOM_RE.test(incText)) skip = "needs-dom";
            if (!want.length) skip = skip || "unsupported global (" + globals.join(",") + ")";
            for (const g of [...new Set(want.length ? want : ["window"])])
                for (const v of variants)
                    runs.push({ rel, variant: v, global: g, long: m.long, skip,
                        id: rel.replace(/\.js$/, g === "window" ? ".html" : ".worker.html") + v });
        }
    }
    return runs;
}

async function drain(pipe) {
    const parts = [], buf = new Uint8Array(65536);
    for (;;) { const n = await pipe.read(buf); if (n <= 0) break; parts.push(buf.slice(0, n)); }
    const all = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    parts.reduce((o, p) => (all.set(p, o), o + p.length), 0);
    return new TextDecoder().decode(all);
}

async function runOne(bin, wpt, r) {
    const t0 = Date.now();
    const p = new sys.Spawn(bin, ["--std", "--no-unhandled-rejection", HERE, "--child", wpt, r.rel, r.variant, r.global],
        { stdin: "ignore", timeoutMs: r.long ? 120000 : 30000 });
    const [out, err] = await Promise.all([drain(p.stdout), drain(p.stderr)]);
    const res = await p.wait();
    p.close();
    const subs = [];
    let harness = null, uncaught = "";
    for (const line of out.split("\n")) {
        if (!line.startsWith("@@WPT ")) continue;
        let d;
        try { d = JSON.parse(line.slice(6)); } catch (e) { continue; }
        if (d.kind === "result") subs.push({ name: d.name, status: d.status, message: d.message });
        else if (d.kind === "done") harness = { status: d.status, message: d.message };
        else if (d.kind === "error") uncaught = d.message;
    }
    if (!harness) harness = res.timedOut ? { status: "TIMEOUT", message: "runner timeout" }
        : { status: "CRASH", message: `exit ${res.code ?? ""} signal ${res.signal ?? ""}: ${(err || out).trim().split("\n").slice(-3).join(" | ")}` };
    if (uncaught && harness.status === "OK") harness = { status: "ERROR", message: uncaught };
    return { ...r, harness, subs, ms: Date.now() - t0 };
}

async function pool(items, n, fn) {
    const out = new Array(items.length);
    let next = 0;
    await Promise.all(Array.from({ length: Math.min(n, items.length) }, async () => {
        for (;;) { const i = next++; if (i >= items.length) return; out[i] = await fn(items[i]); }
    }));
    return out;
}

function rows(results) {
    const out = [];
    for (const r of results) {
        if (r.skip) { out.push([r.id, "", "SKIP:" + r.skip]); continue; }
        out.push([r.id, "", r.harness.status]);
        const seen = new Map();
        for (const s of r.subs) {
            const k = seen.has(s.name) ? s.name + " #" + (seen.set(s.name, seen.get(s.name) + 1).get(s.name)) : (seen.set(s.name, 1), s.name);
            out.push([r.id, k.replace(/[\t\n\r]/g, " "), s.status]);
        }
    }
    return out;
}

function table(results, dirs) {
    const by = new Map(dirs.map((d) => [d, { files: 0, pass: 0, fail: 0, skip: 0, err: 0 }]));
    for (const r of results) {
        const d = by.get(r.rel.split("/")[0]) || by.set(r.rel.split("/")[0], { files: 0, pass: 0, fail: 0, skip: 0, err: 0 }).get(r.rel.split("/")[0]);
        d.files++;
        if (r.skip) { d.skip++; continue; }
        if (r.harness.status !== "OK") d.err++;
        for (const s of r.subs) s.status === "PASS" ? d.pass++ : d.fail++;
    }
    print(["dir", "runs", "skipped", "harness-err", "subtests", "pass", "fail", "pass%"].join("\t"));
    for (const [d, s] of by) {
        const n = s.pass + s.fail;
        print([d, s.files, s.skip, s.err, n, s.pass, s.fail, n ? (100 * s.pass / n).toFixed(1) : "-"].join("\t"));
    }
}

function compare(cur, file) {
    const exp = new Map();
    for (const line of readText(file).split("\n")) {
        if (!line || line.startsWith("#")) continue;
        const [t, s, st] = line.split("\t");
        exp.set(t + "\t" + s, st);
    }
    const now = new Map(cur.map(([t, s, st]) => [t + "\t" + s, st]));
    const reg = [], gained = [];
    for (const [k, st] of exp) {
        const n = now.get(k);
        if ((st === "PASS" || st === "OK") && n !== st) reg.push(`${k.replace("\t", " :: ")}: expected ${st}, got ${n ?? "MISSING"}`);
        else if (st !== "PASS" && st !== "OK" && n === "PASS") gained.push(`${k.replace("\t", " :: ")}: ${st} -> PASS`);
    }
    for (const [k, st] of now) if (!exp.has(k) && st === "PASS") gained.push(`${k.replace("\t", " :: ")}: new, PASS`);
    return { reg, gained };
}

async function main(argv) {
    const opt = { jobs: 8, dirs: "streams,encoding,url,compression,FileAPI,xhr,fetch,websockets", bin: "./dynajs" };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a.startsWith("--") && i + 1 < argv.length) opt[a.slice(2).replace(/-(\w)/g, (_, c) => c.toUpperCase())] = argv[++i];
    }
    const wpt = opt.wpt || std.getenv("WPT_DIR") || "/Users/piyush/ai/dynascript/third_party/wpt";
    if (std.loadFile(wpt + "/resources/testharness.js") === null) {
        print(`wpt: no WPT checkout at ${wpt} (pass --wpt DIR or WPT_DIR)`);
        std.exit(2);
    }
    const dirs = opt.dirs.split(",");
    const t0 = Date.now();
    const runs = plan(wpt, dirs, opt.filter);
    const results = await pool(runs, +opt.jobs, (r) => (r.skip ? Promise.resolve({ ...r, harness: null, subs: [] }) : runOne(opt.bin, wpt, r)));
    const cur = rows(results);
    table(results, dirs);
    print(`wpt: ${runs.length} runs (${runs.filter((r) => r.skip).length} skipped), ${((Date.now() - t0) / 1000).toFixed(1)} s wall, jobs ${opt.jobs}`);
    if (opt.json) std.writeFile ? std.writeFile(opt.json, JSON.stringify(results)) : writeAll(opt.json, JSON.stringify(results));
    if (opt.writeExpect) writeAll(opt.writeExpect, "# test\tsubtest\tstatus -- tools/wpt.js baseline (WPT " + (opt.wptRev || "6c8a4923") + ")\n" + cur.map((r) => r.join("\t")).join("\n") + "\n");
    if (opt.expect) {
        const { reg, gained } = compare(cur, opt.expect);
        for (const g of gained) print("NEWLY PASSING: " + g);
        for (const r of reg) print("REGRESSION: " + r);
        print(`wpt: ${reg.length} regressions, ${gained.length} newly passing`);
        if (reg.length) std.exit(1);
    }
}

function writeAll(path, text) {
    const f = std.open(path, "w");
    if (!f) throw new Error("wpt: cannot write " + path);
    f.puts(text);
    f.close();
}

if (os.Worker.parent) workerMain();
else if (scriptArgs[1] === "--child") await childMain(scriptArgs.slice(2));
else await main(scriptArgs.slice(1));
