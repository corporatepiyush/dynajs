// flags: --std
// covers: dyna:http.App
// Part of the former tests/http/test_http_hardening.js, split so the parts run in parallel.
// peer-proof: drop probe out:User-Agent: curl/ -- the probe's curl calls reach the App directly in the dynajs run; only the Python run's proxy sees them
// peer-proof: drop probe out:^GET /(ws|sse)flood -- how much of a 24 MB flood a stalled peer receives before the 4 MB outbound cap ends it is timing
// peer-proof: drop probe out:"id": 11\} -- survives_abort retries a request that lost the race with the abort churn, so how many tries it takes is timing
// peer-proof: drop probe out:^POST /up -- the App names every upload (/up, /uptxt) up_<pid>_<n>, so its answer differs per process in id and Content-Length
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against HEAD with upstream.py taking its port file as an argument, so the recording proxy sits in front of it, and probe.js's 0.4 s settle before cap_after_close replaced by the admission wait probe.js uses
// slots: 2
import { spawnHelper, cleanupOnExit } from "../lib/helper.js";
import { App, HTTPClient, HTTPServer, HTTPServerAsync } from "dyna:http";
import { pid } from "dyna:sys";
import * as std from "std";
import * as os from "os";
import { suite } from "../lib/kit.js";

const t = suite("test_http_hardening_part1");
let A = null, why = null;
const ok = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; if (why) a.skip(why); return fn(a); }, o);
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_hard.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;

const startBg = (cmd, tag) => {
    sh(`rm -f ${T}/${tag}.pid`); spawnHelper(`${cmd} >${T}/${tag}.log 2>&1`, { pidFile: `${T}/${tag}.pid` });
};
const waitPort = (tag) => {
    for (let i = 0; i < 200; i++) {
        const p = parseInt((cat(`${T}/${tag}.port`) || "").trim(), 10) || 0;
        if (p > 0) return p;
        sh("sleep 0.05");
    }
    return 0;
};
const reapFixtures = () => {
    sh(`for f in ${T}/*.pid; do [ -f "$f" ] || continue; ` +
       `p=$(cat "$f" 2>/dev/null); case "$p" in ''|*[!0-9]*) continue;; esac; ` +
       `case "$(ps -p $p -o command= 2>/dev/null)" in *"${T}/"*) ` +
       `kill $p 2>/dev/null; sleep 0.2; kill -9 $p 2>/dev/null;; esac; done`);
};
cleanupOnExit(`rm -rf ${T}`);

let R = {}, appPid = 0, probeBad = null;
t.before(() => {
    if (why) return;
    sh(`rm -rf ${T}; mkdir -p ${T}`);
        sh(`mkdir -p ${T}/www ${T}/updir ${T}/mtr`);
        sh(`printf USER-METRICS > ${T}/mtr/index.html`);
        sh(`printf 'PREFIX-OK' > ${T}/www/oo.html`);
        write(`${T}/upstream.js`, `import { listen, latin1 } from ${KIT};
const paths = [];
listen(async (c) => {
    const r = await c.readUntil("\\r\\n\\r\\n");
    const d = latin1(r.bytes);
    const p = d ? d.split(" ")[1] : "/";
    paths.push(p);
    if (p.startsWith("/hugehead")) {
        await c.write("HTTP/1.1 200 OK\\r\\n" + ("X-Pad: " + "h".repeat(900) + "\\r\\n").repeat(120) + "Content-Length: 3\\r\\n\\r\\nbig");
    } else if (p.startsWith("/earlyclose")) {
        c.abort();
        return;
    } else {
        const body = "seen=" + paths.join("|");
        await c.write("HTTP/1.1 200 OK\\r\\nContent-Length: " + body.length + "\\r\\n\\r\\n" + body);
    }
    c.close();
}, { portFile: scriptArgs[1] });
`);
        write(`${T}/app.js`, `import { App } from "dyna:http";
import { Path } from "dyna:file";
import { pid } from "dyna:sys";
import * as os from "os";
import * as std from "std";
const f = std.open("${T}/app.pid", "w"); f.puts(String(pid())); f.close();
const UP = parseInt(scriptArgs[1], 10);   /* the upstream's ephemeral port */
const app = new App({ port: 0, idleTimeoutMs: 8000, maxConns: 2,
                   metrics: true });
app.static("/f", new Path("${T}/www"));
app.proxy("/api", { host: "127.0.0.1", port: UP });
app.ws("/ws", { open: () => {}, message: () => {}, close: () => {} });
app.rpc("/rpc", {
  add: (a, b) => a + b,
  quoteThrow: () => { throw new Error('a"b\\\\c\\nd'); },
  doubleSettle: () => ({ then: (res, rej) => { res("first"); res("second"); } }),
  never: () => new Promise(() => {}),
});
app.upload("/up", { dir: new Path("${T}/updir"), maxFileSize: 1 << 22 },
           (p, m) => m.size);
app.upload("/uptxt", { dir: new Path("${T}/updir"), maxFileSize: 65536,
                       allow: ["text/plain"] }, (p, m) => m.size);
/* A user route at the built-ins' path wins: registration beats convention */
app.static("/metrics", new Path("${T}/mtr"));
/* ④ outbound backpressure: each open floods 6 MB at a peer that never
 * reads -- the 4 MB outbound cap must end the connection, not grow the
 * aio queue without bound. */
const MB = new Uint8Array(1048576);
/* 24 MB: large enough that kernel socket buffers cannot absorb the flood on
 * any platform (Linux loopback autotunes into the MB range), so the 4 MB
 * outbound userspace cap is what ends these connections. */
const rss = () => { const [r, w] = os.pipe(); const p = os.exec(["ps", "-o", "rss=", "-p", String(pid())], { block: false, usePath: true, stdout: w }); os.close(w); const f = std.fdopen(r, "r"); const v = parseInt(f.readAsString().trim(), 10) * 1024; f.close(); os.waitpid(p, 0); return v; };
const flood = (tag, conn, send) => { const base = rss(); let topQ = 0, topRss = 0, prev = -1, open = true; for (let i = 0; i < 24; i++) { send(); const q = conn.bufferedAmount; if (q < prev) open = false; if (open) topQ = Math.max(topQ, q); prev = q; topRss = Math.max(topRss, rss() - base); } const f = std.open("${T}/flood." + tag, "w"); f.puts(topQ + " " + topRss + " " + conn.bufferedAmount); f.close(); };
app.ws("/wsflood", { open: (ws) => {
  flood("ws", ws, () => ws.send(MB));
}, message: () => {}, close: () => {} });
app.sse("/sseflood", { open: (sse) => {
  flood("sse", sse, () => sse.send("F".repeat(1048576)));
}, close: () => {} });
/* route registrations while an upload is in flight: the old code held a
 * pointer into the realloc'd route array across event-loop turns (UAF) */
let n = 0;
const timer = setInterval(() => {
  app.upload("/up_gen" + (n++), { dir: new Path("${T}/updir") }, () => 0);
}, 30);
setTimeout(() => clearInterval(timer), 60000);
app.start();
const g = std.open("${T}/app.port", "w"); g.puts(String(app.port)); g.close();
`);
        write(`${T}/probe.js`, `import { connect, cat, latin1, find } from ${KIT};
import * as std from "std";
import * as os from "os";
const H = "127.0.0.1", P = Number(scriptArgs[1]);
const R = [];
const rec = (tag, kv) => R.push(Object.assign({ tag }, kv));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const pj = (v) => Array.isArray(v) ? "[" + v.map(pj).join(", ") + "]" : v && typeof v === "object" ? "{" + Object.entries(v).map(([k, x]) => JSON.stringify(k) + ": " + pj(x)).join(", ") + "}" : JSON.stringify(v);
const KEY = "MDEyMzQ1Njc4OWFiY2RlZg==";
const has = (u8, s) => find(u8, s, 0) >= 0;
const count = (s, n) => s.split(n).length - 1;
async function dial(tries) {
    for (let i = 0; i < tries; i++) { try { return await connect(P, H, { connectTimeoutMs: 2000 }); } catch { await sleep(100); } }
    return null;
}
async function readFor(c, ms) {
    const r = await c.read({ timeoutMs: ms });
    return r.timedOut ? { to: true, b: null } : { to: false, b: r.bytes };
}
async function hit(payload, want = 1, hard = 5.0) {
    const c = await dial(30);
    if (!c) return { n: 0, code: null, body: "", raw: "", err: "no connect" };
    const senderr = (await c.write(payload)) ? null : "send failed";
    let out = new Uint8Array(0);
    const t0 = Date.now();
    for (;;) {
        const left = hard * 1000 - (Date.now() - t0);
        if (left <= 0) break;
        const r = await readFor(c, out.length ? Math.min(350, left) : left);
        if (r.to || !r.b) break;
        out = cat(out, r.b);
    }
    c.abort();
    const s = latin1(out), i = s.indexOf("\\r\\n\\r\\n");
    return { n: count(s, "HTTP/1.1"), code: s.startsWith("HTTP") ? s.slice(9, 12) : null, body: i >= 0 ? s.slice(i + 4) : "", raw: s, senderr };
}
function sh(args) {
    const [r, w] = os.pipe();
    const nul = os.open("/dev/null", os.O_WRONLY);
    const pid = os.exec(args, { block: false, usePath: true, stdout: w, stderr: nul });
    os.close(w); os.close(nul);
    const f = std.fdopen(r, "r");
    const out = f.readAsString(); f.close();
    os.waitpid(pid, 0);
    return out;
}
const curl = (p) => sh(["curl", "-s", "--max-time", "4", "--path-as-is", "http://127.0.0.1:" + P + p]);
const curlRaw = (args) => sh(["curl", "-s", "--max-time", "4", ...args]);
const rpcReq = (body, extra = "") => "POST /rpc HTTP/1.1\\r\\nHost: x\\r\\n" + extra + "Content-Length: " + body.length + "\\r\\n\\r\\n" + body;
const post = (body, extra = "") => hit(rpcReq(body, extra));
const parse = (s) => { try { return JSON.parse(s); } catch { return null; } };

for (let i = 0; i < 50; i++) { if (curl("/f/oo.html").includes("PREFIX-OK")) break; await sleep(100); }
rec("static_ok", { body: curl("/f/oo.html") });
rec("static_boundary", { body: curl("/foo.html") });
let r;
for (let i = 0; i < 30; i++) { r = curl("/api/x"); if (r.includes("seen=")) break; await sleep(100); }
rec("proxy_ok", { body: r });
rec("proxy_boundary", { body: curl("/apiv2/x") });

async function wsProbe(tag, { upgrade = "websocket", conn = "Upgrade", ver = "13", key = null, method = "GET" } = {}) {
    const k = key !== null ? key : KEY;
    const q = await hit(method + " /ws HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: " + upgrade + "\\r\\nConnection: " + conn + "\\r\\nSec-WebSocket-Key: " + k + "\\r\\nSec-WebSocket-Version: " + ver + "\\r\\n\\r\\n");
    rec(tag, { code: q.code, upgraded: q.code === "101" });
}
await wsProbe("ws_valid");
await wsProbe("ws_h2c", { upgrade: "h2c" });
await wsProbe("ws_noversion", { ver: "" });
await wsProbe("ws_version8", { ver: "8" });
await wsProbe("ws_conn_plain", { conn: "keep-alive" });
await wsProbe("ws_shortkey", { key: "dGhlIHNhbXBsZSBub25jZQ===" });
await wsProbe("ws_post", { method: "POST" });
const ACC = "BACScCJPNqyz+UBoqMH89VmURoA=";
r = await hit("GET /ws HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\nSec-WebSocket-Key: " + KEY + "\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n");
rec("ws_accept_correct", { accept: r.raw.includes(ACC) });

r = await post(pj({ method: "quoteThrow", id: 1 }));
let parsed = parse(r.body);
rec("rpc_escape", { ok: parsed !== null && parsed.id === 1, msg: ((parsed || {}).error || {}).message || "" });
const longId = "i".repeat(300);
r = await post(pj({ method: "quoteThrow", id: longId }));
parsed = parse(r.body);
rec("rpc_longid_msg", { ok: parsed !== null, id: parsed ? parsed.id : null });
r = await post(pj({ method: "nope", id: longId }));
parsed = parse(r.body);
rec("rpc_longid_404", { ok: parsed !== null, id: parsed ? parsed.id : null });

r = await hit(rpcReq(pj({ method: "doubleSettle", id: 7 })), 2, 4.0);
rec("double_settle", { n: r.n, code: r.code, body: r.body.slice(0, 80) });
const r2 = await post(pj({ method: "add", params: [1, 2], id: 2 }));
rec("alive_after", { code: r2.code });
r = await hit("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 2000000\\r\\n\\r\\n");
rec("app_413", { code: r.code });

async function admitted(c) {
    if (!(await c.write("GET /healthz HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n"))) return false;
    const q = await readFor(c, 2000);
    return !q.to && !!q.b && latin1(q.b).startsWith("HTTP/1.1");
}
async function holdTwo() {
    for (let i = 0; i < 30; i++) {
        const held = [];
        let good = true;
        for (let j = 0; j < 2 && good; j++) {
            const c = await dial(1);
            if (!c) { good = false; break; }
            held.push(c);
            if (!(await admitted(c))) good = false;
        }
        if (good) return held;
        for (const c of held) c.abort();
        await sleep(100);
    }
    return [];
}
let socks = [];
try {
    socks = await holdTwo();
    const third = await dial(1);
    let refused = false;
    if (!third) refused = true;
    else {
        await third.write("GET /rpc HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n");
        const q = await readFor(third, 2000);
        refused = q.to || !q.b;
        third.abort();
    }
    if (socks[0]) socks[0].abort();
    let code = null;
    for (let i = 0; i < 20 && code !== "200"; i++) {
        code = (await post(pj({ method: "add", params: [2, 3], id: 3 }))).code;
        if (code !== "200") await sleep(100);
    }
    rec("maxconns", { refused, after_free: code });
} finally {
    for (const c of socks) c.abort();
}

for (const c of await holdTwo()) c.abort();
let capRefused = 0, capRounds = 0;
for (let i = 0; i < 60; i++) {
    const trio = [];
    try {
        const p0 = await dial(1); if (!p0) continue; trio.push(p0);
        if (!(await admitted(p0))) continue;
        p0.abort();
        const a0 = await dial(1), b0 = await dial(1);
        if (a0) trio.push(a0);
        if (b0) trio.push(b0);
        if (!a0 || !b0) continue;
        capRounds++;
        if (!((await admitted(a0)) && (await admitted(b0)))) capRefused++;
    } finally {
        for (const c of trio) c.abort();
        await sleep(30);
    }
}
rec("cap_after_close", { rounds: capRounds, refused: capRefused });

try {
    const s = await connect(P, H, { connectTimeoutMs: 3000 });
    const total = 1 << 21;
    await s.write("POST /up HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: application/octet-stream\\r\\nContent-Length: " + total + "\\r\\n\\r\\n");
    const ch = new Uint8Array(65536).fill(0x5a);
    for (let off = 0; off < total; off += 65536) { await s.write(ch); await sleep(30); }
    const h = await s.readUntil("\\r\\n\\r\\n", { timeoutMs: 6000 });
    let extra = "";
    if (h.found) {
        const head = latin1(h.bytes);
        const cl = parseInt(head.split("Content-Length: ")[1], 10);
        const b = await s.readBytes(cl, { timeoutMs: 6000 });
        extra = latin1(b.bytes);
    }
    s.abort();
    rec("upload_race", { body: extra });
} catch (e) {
    rec("upload_race", { body: "ERR:" + String(e.message).slice(0, 60) });
}

r = await hit("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\ntRaNsFeR-eNcOdInG: chunked\\r\\n\\r\\n");
rec("te_mixedcase", { code: r.code });
r = await hit("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nTransfer-Encoding : chunked\\r\\nContent-Length: 0\\r\\n\\r\\nGET /rpc HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n", 2);
rec("te_ows_unknown", { n: r.n, code: r.code });
let body = pj({ method: "add", params: [1, 1], id: 9 });
r = await hit("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: " + body.length + "\\r\\nContent-Length: " + body.length + "\\r\\n\\r\\n" + body);
rec("dup_cl_equal", { code: r.code });
for (const [tag, cl] of [["cl_negative", "-1"], ["cl_plus", "+5"], ["cl_hex", "0x5"]]) {
    r = await hit("POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: " + cl + "\\r\\n\\r\\n");
    rec(tag, { code: r.code });
}
r = await hit("GET /f/oo.html HTTP/1.1\\r\\nHost : x\\r\\n\\r\\n");
rec("ows_colon", { code: r.code });
try {
    const s = await connect(P, H, { connectTimeoutMs: 2000 });
    await s.write("GET /f/oo.html HTTP/1.1\\nHost: x\\n\\n");
    const q = await readFor(s, 3000);
    const got = q.b ? latin1(q.b) : "";
    s.abort();
    rec("bare_lf", { silent: got === "" || got.slice(0, 4) !== "HTTP" });
} catch (e) {
    rec("bare_lf", { silent: null, err: String(e.message).slice(0, 40) });
}
let many = "";
for (let i = 0; i < 200; i++) many += "X-H" + i + ": v\\r\\n";
r = await hit("GET /f/oo.html HTTP/1.1\\r\\nHost: x\\r\\n" + many + "\\r\\n");
rec("many_headers", { code: r.code });
r = await hit("GET /f/" + "q".repeat(3000) + " HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n");
rec("long_reqline", { code: r.code });
const one = "POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: " + body.length + "\\r\\n\\r\\n" + body;
const closer = "POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\nContent-Length: " + body.length + "\\r\\n\\r\\n" + body;
await hit(one + closer + one);
r = await hit(one.repeat(50), 50, 8.0);
rec("pipeline50", { n: r.n });
for (let i = 0; i < 2; i++) { const c = await dial(1); if (c) c.abort(); }
let code = null;
for (let i = 0; i < 4; i++) {
    r = await post(pj({ method: "add", params: [4, 4], id: 11 }));
    if (r.code === "200") { code = "200"; break; }
    await sleep(100);
}
rec("survives_abort", { code });
const full = "GET /f/oo.html HTTP/1.1\\r\\nHost: x\\r\\nUser-Agent: drip\\r\\n\\r\\n";
try {
    const s = await connect(P, H, { connectTimeoutMs: 2000 });
    for (let i = 0; i < full.length; i++) { await s.write(full[i]); await sleep(2); }
    const h = await s.readUntil("PREFIX-OK", { timeoutMs: 4000 });
    s.abort();
    rec("split_bytes", { ok: h.found });
} catch (e) {
    rec("split_bytes", { ok: false, err: String(e.message).slice(0, 40) });
}
r = curl("/f/%2e%2e/%2e%2e/etc/passwd");
rec("pct_not_decoded", { safe: !r.includes("root:") });

async function wsSession() {
    const s = await connect(P, H, { connectTimeoutMs: 2000 });
    await s.write("GET /ws HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\nSec-WebSocket-Key: " + KEY + "\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n");
    const h = await s.readUntil("\\r\\n\\r\\n");
    if (!h.found) throw new Error("hs closed");
    if (!latin1(h.bytes).split("\\r\\n")[0].includes("101")) throw new Error("no upgrade");
    return s;
}
function maskFrame(op, payload, masked = true) {
    const p = new TextEncoder().encode(payload), n = p.length, m = [0x31, 0x32, 0x33, 0x34];
    const hdr = [0x80 | op, (masked ? 0x80 : 0) | n, ...(masked ? m : [])];
    const out = new Uint8Array(hdr.length + n);
    out.set(hdr);
    for (let i = 0; i < n; i++) out[hdr.length + i] = masked ? p[i] ^ m[i & 3] : p[i];
    return out;
}
try {
    const s = await wsSession();
    await s.write(maskFrame(0x3, "unknown-opcode"));
    await s.write(maskFrame(0x9, "ping"));
    const q = await readFor(s, 2000);
    rec("ws_reserved_op", { alive: !!q.b && q.b.includes(0x8a) });
    s.abort();
} catch (e) {
    rec("ws_reserved_op", { alive: null, err: String(e.message).slice(0, 40) });
}
try {
    const s = await wsSession();
    await s.write(maskFrame(0x1, "unmasked", false));
    let got = 0;
    for (;;) { const q = await readFor(s, 2000); if (q.to || !q.b) break; got += q.b.length; }
    s.abort();
    rec("ws_unmasked_refused", { closed: got === 0 });
} catch (e) {
    rec("ws_unmasked_refused", { closed: null, err: String(e.message).slice(0, 40) });
}

r = await hit("POST /uptxt HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: application/octet-stream\\r\\nContent-Length: 10\\r\\n\\r\\nAAAAAAAAAA");
rec("upload_type_disallowed", { code: r.code });
const bigup = "T".repeat(70000);
r = await hit("POST /uptxt HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: text/plain\\r\\nContent-Length: " + bigup.length + "\\r\\n\\r\\n" + bigup);
rec("upload_over_cap", { code: r.code });
const okUp = "T".repeat(100);
r = await hit("POST /uptxt HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: text/plain\\r\\nContent-Length: " + okUp.length + "\\r\\n\\r\\n" + okUp);
rec("upload_allowed_ok", { code: r.code, okbody: r.body.includes('{"ok":true') });
rec("proxy_hugehead", { refused: curl("/api/hugehead").includes("upstream headers too large") });
rec("proxy_earlyclose", { refused: curl("/api/earlyclose").includes("error") });

r = await post(pj([{ method: "add", params: [1, 2], id: "a" }, { method: "nope", id: "b" }, { method: "add", params: [3, 3] }]));
parsed = parse(r.body);
rec("rpc_batch_mixed", { ok: Array.isArray(parsed) && parsed.length === 2, ids: Array.isArray(parsed) ? parsed.map((e) => e.id === undefined ? null : e.id) : null });
r = await post(pj({ method: "add", params: [2, 2] }));
rec("rpc_notification_silent", { n: r.n, code: r.code });

async function hitEof(payload, hard = 4.0) {
    const s = await dial(30);
    if (!s) return { code: null, eof: null };
    await s.write(payload);
    let out = "", eof = false;
    for (;;) {
        const q = await readFor(s, hard * 1000);
        if (q.to) break;
        if (!q.b) { eof = true; break; }
        out += latin1(q.b);
        if (out.includes("}") || out.includes("\\r\\n\\r\\n")) {
            for (;;) { const e = await readFor(s, hard * 1000); if (e.to) break; if (!e.b) { eof = true; break; } out += latin1(e.b); }
            break;
        }
    }
    s.abort();
    return { code: out.startsWith("HTTP") ? out.slice(9, 12) : null, eof, full: out.includes("PREFIX-OK") };
}
r = await hitEof("GET /f/oo.html HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n");
rec("app_close_token", { code: r.code, eof: r.eof, full: r.full });
r = await hitEof("GET /f/oo.html HTTP/1.0\\r\\nHost: x\\r\\n\\r\\n");
rec("app_http10_default_close", { code: r.code, eof: r.eof, full: r.full });
try {
    const s = await connect(P, H, { connectTimeoutMs: 2000 });
    await s.write("GET /f/oo.html HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n");
    let kept = true;
    const h = await s.readUntil("\\r\\n\\r\\n", { timeoutMs: 1500 });
    if (h.eof) kept = false;
    else if (h.found) { const q = await readFor(s, 1500); if (!q.to && !q.b) kept = false; }
    s.abort();
    rec("app_keepalive_control", { kept });
} catch (e) {
    rec("app_keepalive_control", { kept: null, err: String(e.message).slice(0, 40) });
}
r = await hit(one + closer + one, 3, 4.0);
rec("pipeline_after_close", { n: r.n });
const rpcbody = pj({ method: "add", params: [5, 5], id: 31 });
r = await hit("POST /up HTTP/1.1\\r\\nHost: x\\r\\nContent-Type: text/plain\\r\\nContent-Length: 0\\r\\n\\r\\n" + rpcReq(rpcbody), 2, 4.0);
rec("upload_pipelined", { n: r.n, up: r.raw.includes('"ok":true'), rpc: r.raw.includes('"result":10'), tail: r.raw.slice(-120) });
const U = "http://127.0.0.1:" + P + "/f/oo.html";
let hd = curlRaw(["-si", "-I", "-H", "Range: bytes=2-5", U]);
rec("static_head_range", { ok: hd.split("\\n")[0].includes("206") && hd.includes("Content-Length: 4") && !hd.includes("PREFIX-OK") });
const mv = curl("/metrics");
rec("metrics_override", { ok: mv.includes("USER-METRICS") && !mv.includes("dyn_http_") });
hd = curlRaw(["-I", U]);
rec("static_head", { ok: hd.split("\\r\\n")[0].includes("200") && hd.includes("Content-Length: 9") && !hd.endsWith("PREFIX-OK") });
rec("static_range_first4", { body: curlRaw(["-si", "-H", "Range: bytes=0-3", U]) });
rec("static_range_suffix", { body: curlRaw(["-si", "-H", "Range: bytes=-4", U]) });
rec("static_range_unsat", { code: curlRaw(["-si", "-H", "Range: bytes=100-", U]).slice(0, 40) });
rec("static_range_multi_full", { body: curlRaw(["-si", "-H", "Range: bytes=0-2,5-8", U]) });

const STALL_MS = 4000;
async function floodProbe(path, isWs) {
    try {
        const s = await connect(P, H, { connectTimeoutMs: 2000 });
        await s.write(isWs ? "GET " + path + " HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\nSec-WebSocket-Key: " + KEY + "\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n"
            : "GET " + path + " HTTP/1.1\\r\\nHost: h\\r\\nAccept: text/event-stream\\r\\n\\r\\n");
        const t0 = Date.now();
        let got = 0, eof = false;
        const q = await readFor(s, 10000);
        if (q.b) got += q.b.length;
        s.conn.pause();
        await new Promise((res) => setTimeout(res, STALL_MS));
        s.conn.resume();
        while (!q.to && Date.now() - t0 < 8000) {
            const d = await readFor(s, 10000);
            if (d.to) break;
            if (!d.b) { eof = true; break; }
            got += d.b.length;
        }
        s.abort();
        return { eof, waited: Math.round((Date.now() - t0) / 100) / 10, bytes: got };
    } catch (e) {
        return { eof: null, err: String(e.message).slice(0, 50) };
    }
}
rec("ws_flood_bounded", await floodProbe("/wsflood", true));
rec("sse_flood_bounded", await floodProbe("/sseflood", false));
print(JSON.stringify(R));
`);
        write(`${T}/assert.js`, `
/* parsed by the parent below */
`);

        startBg(`${EXE} --std ${T}/upstream.js ${T}/up.port`, "up");
        const UP = waitPort("up");
        startBg(`${EXE} --std ${T}/app.js ${UP}`, "appsh");
        appPid = 0;
        for (let i = 0; i < 100 && !(appPid > 0); i++) {
            appPid = parseInt(cat(`${T}/app.pid`).trim(), 10) || 0;
            if (!(appPid > 0)) sh("sleep 0.05");
        }
        const AP = waitPort("app");

        sh(`${EXE} --std ${T}/probe.js ${AP} > ${T}/probe.json 2>${T}/probe.err`);
        R = {};
        try {
            for (const r of JSON.parse(cat(`${T}/probe.json`))) R[r.tag] = r;
        } catch (e) {
            probeBad = cat(`${T}/probe.err`).slice(0, 100);
        }
});
t.after(() => {
    if (why) return;
    if (appPid > 0) sh(`kill ${appPid} 2>/dev/null`);
    reapFixtures();
    sh(`rm -rf ${T}`);
});

test("fixture: the App child started and every probe recorded", () => {
    ok(appPid > 0, "App child started");
    if (probeBad !== null)
        ok(false, "probe.js produced parseable JSON (" +
           cat(`${T}/probe.err`).slice(0, 100) + ")");
    const need = ["static_ok", "static_boundary", "proxy_ok", "proxy_boundary",
                  "ws_valid", "ws_h2c", "ws_noversion", "ws_version8",
                  "ws_conn_plain", "ws_shortkey", "ws_post", "ws_accept_correct",
                  "rpc_escape", "rpc_longid_msg", "rpc_longid_404",
                  "double_settle", "alive_after", "app_413",
                  "maxconns", "cap_after_close", "upload_race",
                  "te_mixedcase", "te_ows_unknown", "dup_cl_equal",
                  "cl_negative", "cl_plus", "cl_hex", "ows_colon", "bare_lf",
                  "many_headers", "long_reqline", "pipeline_after_close",
                  "pipeline50", "survives_abort", "split_bytes",
                  "pct_not_decoded", "ws_reserved_op", "ws_unmasked_refused",
                  "upload_type_disallowed", "upload_over_cap",
                  "upload_allowed_ok", "proxy_hugehead", "proxy_earlyclose",
                  "rpc_batch_mixed", "rpc_notification_silent",
                  "app_close_token", "app_http10_default_close",
                  "app_keepalive_control",
                  "static_head", "static_range_first4", "static_range_suffix",
                  "static_range_unsat", "static_range_multi_full",
                  "ws_flood_bounded", "sse_flood_bounded",
                  "upload_pipelined", "static_head_range",
                  "metrics_override"];
    const missing = need.filter(t => !R[t]);
    ok(missing.length === 0, "every probe recorded (missing means the probe died, not that a defence failed)"
       + (missing.length ? " -- missing: " + missing.join(",") + " stderr: " +
          cat(`${T}/probe.err`).slice(0, 120) : ""));
});

test("static and proxy prefixes match only at a segment boundary", () => {
    ok(R.static_ok && R.static_ok.body === "PREFIX-OK",
       "static control: /f/oo.html serves (got " + JSON.stringify(R.static_ok && R.static_ok.body) + ")");
    ok(R.static_boundary && R.static_boundary.body.indexOf("PREFIX-OK") < 0,
       "S6: /foo.html does NOT match the /f prefix (got " +
       JSON.stringify(R.static_boundary && R.static_ok && R.static_boundary.body.slice(0, 40)) + ")");

    ok(R.proxy_ok && R.proxy_ok.body.indexOf("seen=/x") >= 0,
       "proxy control: /api/x forwarded as /x (got " + JSON.stringify(R.proxy_ok && R.proxy_ok.body) + ")");
    ok(R.proxy_boundary && R.proxy_boundary.body.indexOf("seen=") < 0,
       "S6: /apiv2/x is NOT forwarded to the upstream (got " +
       JSON.stringify(R.proxy_boundary && R.proxy_boundary.body.slice(0, 40)) + ")");
});

test("WebSocket: a correct handshake upgrades and malformed handshakes are refused", () => {
    ok(R.ws_valid && R.ws_valid.upgraded === true,
       "S4 control: a correct RFC 6455 handshake still upgrades");
    ok(R.ws_accept_correct && R.ws_accept_correct.accept === true,
       "S4 control: Sec-WebSocket-Accept is the RFC value");
    for (const t of ["ws_h2c", "ws_noversion", "ws_version8", "ws_conn_plain",
                     "ws_shortkey", "ws_post"]) {
        ok(R[t] && R[t].code === "400" && !R[t].upgraded,
           "S4: " + t + " refused with 400, no upgrade (got " +
           JSON.stringify(R[t] && R[t].code) + ")");
    }
});

test("JSON-RPC: thrown messages are escaped and over-long ids are answered with a null id", () => {
    ok(R.rpc_escape && R.rpc_escape.ok === true,
       "S11: an exception containing quote/backslash/newline yields parseable JSON");
    ok(R.rpc_escape && R.rpc_escape.msg === 'Error: a"b\\c\nd',
       "S11: the message round-trips exactly through the JSON escape (got '" +
       (R.rpc_escape ? R.rpc_escape.msg.slice(0, 40) : "") + "')");
    ok(R.rpc_longid_msg && R.rpc_longid_msg.ok === true &&
       R.rpc_longid_msg.id === null,
       "S11: a 300-char id + 500-char throw composes valid JSON, id echoed null (got " +
       JSON.stringify(R.rpc_longid_msg && R.rpc_longid_msg.id) + ")");
    ok(R.rpc_longid_404 && R.rpc_longid_404.ok === true &&
       R.rpc_longid_404.id === null,
       "S11: a 300-char id on method-not-found stays valid JSON (got " +
       JSON.stringify(R.rpc_longid_404 && R.rpc_longid_404.id) + ")");
});

test("a thenable that settles twice yields one response and the server keeps serving", () => {
    ok(R.double_settle && R.double_settle.n === 1,
       "S10: a thenable resolving twice yields exactly ONE response (n=" +
       (R.double_settle && R.double_settle.n) + ")");
    ok(R.double_settle && R.double_settle.code === "200" &&
       R.double_settle.body.indexOf('"first"') >= 0,
       "S10: the first resolution wins (got '" +
       (R.double_settle ? R.double_settle.body.slice(0, 60) : "") + "')");
    ok(R.alive_after && R.alive_after.code === "200",
       "S10: the server serves a normal request afterwards (no UAF)");
});

test("a declared 2MB body on a non-upload route is a 413", () => {
    ok(R.app_413 && R.app_413.code === "413",
       "S16: a declared 2MB body on a non-upload route is a 413 (got " +
       JSON.stringify(R.app_413 && R.app_413.code) + ")");
});

test("maxConns refuses the extra connection and a freed slot admits the next", () => {
    ok(R.maxconns && R.maxconns.refused === true,
       "S15: the third concurrent connection is refused (accept-then-close)");
    ok(R.maxconns && R.maxconns.after_free === "200",
       "S15: closing a slot admits a new connection (got " +
       JSON.stringify(R.maxconns && R.maxconns.after_free) + ")");
    ok(R.cap_after_close && R.cap_after_close.rounds >= 50 && R.cap_after_close.refused === 0,
       "S15b: a slot freed by a peer close admits the next connection (" +
       JSON.stringify(R.cap_after_close) + ")");
});

test("an upload survives concurrent route registrations", () => {
    ok(R.upload_race && R.upload_race.body.indexOf('{"ok":true,"id":"up_') === 0,
       "S9: an upload survives concurrent route registrations (got '" +
       (R.upload_race ? R.upload_race.body.slice(0, 60) : "") + "')");
});

test("request framing: Transfer-Encoding, Content-Length and field syntax are strict", () => {
    ok(R.te_mixedcase && R.te_mixedcase.code === "501",
       "framing: mixed-case Transfer-Encoding is still recognised -> 501 (got " +
       JSON.stringify(R.te_mixedcase && R.te_mixedcase.code) + ")");
    ok(R.te_ows_unknown && R.te_ows_unknown.n === 1,
       "framing: 'Transfer-Encoding :' (OWS colon) is an unknown header -- " +
       "framed by CL, exactly ONE response, no smuggle (n=" +
       (R.te_ows_unknown && R.te_ows_unknown.n) + ")");
    ok(R.dup_cl_equal && R.dup_cl_equal.code === "400",
       "framing: duplicate IDENTICAL Content-Length refused with 400 " +
       "(stricter than the RFC minimum, pinned here) (got " +
       JSON.stringify(R.dup_cl_equal && R.dup_cl_equal.code) + ")");
    for (const t of ["cl_negative", "cl_plus", "cl_hex"]) {
        ok(R[t] && R[t].code === "400",
           "framing: Content-Length " + t.slice(3) + " form is a 400 (got " +
           JSON.stringify(R[t] && R[t].code) + ")");
    }
    ok(R.ows_colon && R.ows_colon.code === "400",
       "framing: 'Host : x' (OWS before colon) is a malformed field line -> 400");
    ok(R.bare_lf && R.bare_lf.silent === true,
       "framing: bare-LF endings never parse -- bounded silence, no crash");
});

test("many headers, long request lines, deep pipelines, abort churn and dripped bytes are handled", () => {
    ok(R.many_headers && R.many_headers.code === "200",
       "best-case: a 200-header request under the size cap serves");
    ok(R.long_reqline && R.long_reqline.code === "404",
       "worst-case: a 3000-char request line truncates and answers 404, bounded (got " +
       JSON.stringify(R.long_reqline && R.long_reqline.code) + ")");

    ok(R.pipeline50 && R.pipeline50.n === 50,
       "best-case: 50 pipelined requests give 50 in-order responses (n=" +
       (R.pipeline50 && R.pipeline50.n) + ")");
    ok(R.survives_abort && R.survives_abort.code === "200",
       "worst-case: connect+abort churn leaves the server serving");
    ok(R.split_bytes && R.split_bytes.ok === true,
       "best-case: a request dripped one byte at a time reassembles correctly " +
       "(hdr_scan_from resume logic)");
});

test("percent-encoded dot segments are not decoded into a traversal", () => {
    ok(R.pct_not_decoded && R.pct_not_decoded.safe === true,
       "security: %2e%2e is NOT decoded -- no traversal through the static route");
});

test("ws: a reserved opcode is ignored and an unmasked client frame fails the connection", () => {
    ok(R.ws_reserved_op && R.ws_reserved_op.alive === true,
       "ws: reserved opcode 0x3 is ignored; ping afterwards still pongs");
    ok(R.ws_unmasked_refused && R.ws_unmasked_refused.closed === true,
       "ws: an UNMASKED client frame fails the connection (RFC 6455 5.1)");
});

test("upload: disallowed types are 415, over-cap bodies 413, allowed uploads succeed", () => {
    ok(R.upload_type_disallowed && R.upload_type_disallowed.code === "415",
       "upload: a content type outside allow[] is a 415 (got " +
       JSON.stringify(R.upload_type_disallowed && R.upload_type_disallowed.code) + ")");
    ok(R.upload_over_cap && R.upload_over_cap.code === "413",
       "upload: declared body over maxFileSize is a 413 before bytes matter (got " +
       JSON.stringify(R.upload_over_cap && R.upload_over_cap.code) + ")");
    ok(R.upload_allowed_ok && R.upload_allowed_ok.code === "200" &&
       R.upload_allowed_ok.okbody === true,
       "upload control: an allowed type under the cap completes");
});

test("proxy: a huge upstream head and an early upstream close are clean refusals", () => {
    ok(R.proxy_hugehead && R.proxy_hugehead.refused === true,
       "worst-case: a ~110KB upstream header head exceeds the 8KB " +
       "re-serialisation cap -> bounded 502 'upstream headers too large'");
    ok(R.proxy_earlyclose && R.proxy_earlyclose.refused === true,
       "worst-case: an upstream that closes before any head is a clean 502");
});

test("rpc: a mixed batch answers each request", () => {
    ok(R.rpc_batch_mixed && R.rpc_batch_mixed.ok === true &&
       R.rpc_batch_mixed.ids &&
       R.rpc_batch_mixed.ids[0] === "a" && R.rpc_batch_mixed.ids[1] === "b",
       "rpc: a mixed batch yields result(a), error(b) and NOTHING for the notification (got " +
       JSON.stringify(R.rpc_batch_mixed && R.rpc_batch_mixed.ids) + ")");
});

test("static HEAD and Range requests return exact partial lengths and failure forms", () => {
    ok(R.static_head && R.static_head.ok === true,
       "static HEAD: 200 with the true Content-Length and no body (got " +
       JSON.stringify(R.static_head) + ")");
    ok(R.static_range_first4 && R.static_range_first4.body.indexOf("206") >= 0 &&
       R.static_range_first4.body.endsWith("PREF"),
       "static Range bytes=0-3 -> 206 Partial Content, first 4 bytes (got '" +
       (R.static_range_first4 ? R.static_range_first4.body.slice(-8) : "") + "')");
    ok(R.static_range_suffix && R.static_range_suffix.body.endsWith("X-OK") &&
       R.static_range_suffix.body.indexOf("206") >= 0,
       "static suffix Range -4 -> last 4 bytes 'X-OK' (got '" +
       (R.static_range_suffix ? R.static_range_suffix.body.slice(-8) : "") + "')");
    ok(R.static_range_unsat && R.static_range_unsat.code.indexOf("416") >= 0,
       "static Range past EOF -> 416 with bytes */9 failure form (got '" +
       (R.static_range_unsat ? R.static_range_unsat.code.slice(0, 30) : "") + "')");
    ok(R.static_range_multi_full &&
       R.static_range_multi_full.body.indexOf("200") >= 0 &&
       R.static_range_multi_full.body.endsWith("PREFIX-OK"),
       "static multi-range falls back to a full 200 (got '" +
       (R.static_range_multi_full ? R.static_range_multi_full.body.slice(0, 30) : "") + "')");
});

// The App queues at most 4 MB per connection (DYN_HTTP_OUTBOUND_MAX) and closes past it, so a stalled peer of the 24 MB flood
// sees EOF after the queue plus what loopback socket buffers held: well under 8 MB. Without the cap it reads all 24 MB.
// The App records the peak bufferedAmount while the connection is open, which the cap holds at or under 4 MB plus the
// refused frame, and its final value: per WHATWG each send after the cap's close adds its 1 MB. RSS is a loose second bound.
const FLOOD_SEEN_MAX = 8 << 20, FLOOD_QUEUE_MAX = (4 << 20) + (1 << 20) + 64, FLOOD_RSS_MAX = 24 << 20;
test("WebSocket and SSE floods at a stalled peer are bounded", () => {
    ok(R.ws_flood_bounded && R.ws_flood_bounded.eof === true &&
       R.ws_flood_bounded.waited < 9 && R.ws_flood_bounded.bytes < FLOOD_SEEN_MAX,
       "backpressure: 6 MB ws flood at a non-reading peer ends the connection (" +
       JSON.stringify(R.ws_flood_bounded) + ")");
    ok(R.sse_flood_bounded && R.sse_flood_bounded.eof === true &&
       R.sse_flood_bounded.waited < 9 && R.sse_flood_bounded.bytes < FLOOD_SEEN_MAX,
       "backpressure: same bound on the sse push path (" +
       JSON.stringify(R.sse_flood_bounded) + ")");
    const [qWs, rWs, endWs] = cat(`${T}/flood.ws`).split(" ").map(Number), [qSse, rSse, endSse] = cat(`${T}/flood.sse`).split(" ").map(Number);
    const mb = (v) => v > 0 && v % (1 << 20) === 0;
    ok(qWs > 0 && qWs <= FLOOD_QUEUE_MAX && qSse > 0 && qSse <= FLOOD_QUEUE_MAX && mb(endWs) && mb(endSse),
       "backpressure: bufferedAmount peaks at or under the 4 MB cap plus one frame while open, then counts whole 1 MB sends after the cap's close (ws " + qWs + "/" + endWs + ", sse " + qSse + "/" + endSse + ")");
    ok(rWs < FLOOD_RSS_MAX && rSse < FLOOD_RSS_MAX,
       "backpressure: resident memory grows by less than the 24 MB flood (ws " + rWs + ", sse " + rSse + " bytes)");
});

test("pipelined uploads are both answered", () => {
    ok(R.upload_pipelined && R.upload_pipelined.n === 2 &&
       R.upload_pipelined.up === true &&
       R.upload_pipelined.rpc === true,
       "P1: a request pipelined behind a synchronous upload is dispatched " +
       "without waiting for another byte (n=" +
       (R.upload_pipelined && R.upload_pipelined.n) + ", up=" +
       (R.upload_pipelined && R.upload_pipelined.up) + ", rpc=" +
       (R.upload_pipelined && R.upload_pipelined.rpc) + ")");
});

test("static HEAD with Range sends partial headers and no body", () => {
    ok(R.static_head_range && R.static_head_range.ok === true,
       "static HEAD+Range: 206 headers with the PARTIAL length and no body (got " +
       JSON.stringify(R.static_head_range) + ")");
});

test("a user route at /metrics beats the built-in", () => {
    ok(R.metrics_override && R.metrics_override.ok === true,
       "routing: a user route at /metrics beats the built-in -- " +
       "registration beats convention (got " + JSON.stringify(R.metrics_override) + ")");
});

test("a JSON-RPC notification gets no response", () => {
    ok(R.rpc_notification_silent && R.rpc_notification_silent.n === 0,
       "rpc: a single notification (no id) gets NO response, per JSON-RPC 2.0 " +
       "(n=" + (R.rpc_notification_silent && R.rpc_notification_silent.n) + ")");
});

test("Connection: close and HTTP/1.0 deliver the full body then EOF; keep-alive stays open", () => {
    ok(R.app_close_token && R.app_close_token.code === "200" &&
       R.app_close_token.eof === true && R.app_close_token.full === true,
       "close: Connection: close delivers the FULL body, then EOF (got " +
       JSON.stringify(R.app_close_token) + ")");
    ok(R.app_http10_default_close.full === true,
       "close: HTTP/1.0 default-close also delivers the full body");
    ok(R.app_http10_default_close && R.app_http10_default_close.code === "200" &&
       R.app_http10_default_close.eof === true,
       "close: HTTP/1.0 with no Connection header defaults to close (got " +
       JSON.stringify(R.app_http10_default_close) + ")");
    ok(R.app_keepalive_control && R.app_keepalive_control.kept === true,
       "close control: an HTTP/1.1 keep-alive request stays open");
    ok(R.pipeline_after_close && R.pipeline_after_close.n === 2,
       "close: a Connection: close request ends the pipelined batch at itself " +
       "-- the request behind it is NOT answered (n=" +
       (R.pipeline_after_close && R.pipeline_after_close.n) + ")");
});

await t.run();
