// flags: --std
// covers: dyna:http.HTTPServerAsync
// Part of the former tests/http/test_http_hardening.js, split so the parts run in parallel.
// slots: 2
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against the HEAD Python probes changed only to take the port as
// argv[1] (they embedded it in their source, so the python3 shim could not put a recording proxy in front of them).
// peer-proof: prefix tp -- the slowloris dribbler stops at the server's FIN; the Python one slept 200 ms blind and sent one more byte into it
import { spawnHelper, cleanupOnExit } from "../lib/helper.js";
import { App, HTTPClient, HTTPServer, HTTPServerAsync } from "dyna:http";
import * as std from "std";
import * as os from "os";

import { suite } from "../lib/kit.js";

const t = suite("test_http_hardening_part2");
let A = null, why = null;
const ok = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; if (why) a.skip(why); return fn(a); }, o);
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

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
t.before(() => { if (!why) sh(`rm -rf ${T}; mkdir -p ${T}`); });
t.after(() => { if (!why) { reapFixtures(); sh(`rm -rf ${T}`); } });

test("HTTPServerAsync: over-cap headers get 431, over-cap bodies 413, and the close token is matched exactly", async () => {
    {
        const s = new HTTPServerAsync({ port: 0, routes: { "/": "m\n" } });
        s.start();
        const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
        write(`${T}/modela.js`, `import { connect, latin1 } from ${KIT};
const H = "127.0.0.1", P = Number(scriptArgs[1]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const R = [];
const rec = (tag, kv) => R.push(Object.assign({ tag }, kv));
async function open() {
    for (let i = 0; i < 40; i++) { try { return await connect(P, H, { connectTimeoutMs: 2000 }); } catch { await sleep(100); } }
    return null;
}
async function hit(payload, hard = 6.0) {
    const c = await open();
    if (!c) return { code: null, conn: "err", nresp: 0, closed: null };
    await c.write(payload);
    const r = await c.readAll({ timeoutMs: hard * 1000 });
    c.abort();
    const out = latin1(r.bytes), i = out.indexOf("\\r\\n\\r\\n");
    return { code: out.startsWith("HTTP") ? out.slice(9, 12) : null,
        conn: out.includes("Connection: keep-alive") ? "keep-alive" : out.includes("Connection: close") ? "close" : "?",
        nresp: out.split("HTTP/1.1").length - 1, body: i >= 0 ? out.slice(i + 4) : "" };
}
let r;
for (let i = 0; i < 50; i++) { r = await hit("GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n"); if (r.code === "200") break; await sleep(100); }
rec("control", { code: r.code });
r = await hit("GET / HTTP/1.1\\r\\nHost: x\\r\\nX-Pad: " + "A".repeat(1024 * 1024 + 8000));
rec("modela_431", { code: r.code });
r = await hit("POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 2000000\\r\\n\\r\\n");
rec("modela_413", { code: r.code });
r = await hit("GET / HTTP/1.0\\r\\nHost: x\\r\\nX-Pad: HTTP/1.1\\r\\n\\r\\n");
rec("version_anchor", { conn: r.conn });
r = await hit("GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: xclose\\r\\n\\r\\nGET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n", 4.0);
rec("token_substr", { nresp: r.nresp, conn: r.conn });
r = await hit("GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n");
rec("token_real_close", { conn: r.conn });
print(JSON.stringify(R));
`);
        sh(`${EXE} --std ${T}/modela.js ${s.port} > ${T}/modela.json 2>${T}/modela.err`);
        let M = {};
        try { for (const r of JSON.parse(cat(`${T}/modela.json`))) M[r.tag] = r; }
        catch (e) { ok(false, "modela probe failed: " + cat(`${T}/modela.err`).slice(0, 80)); }
        ok(M.control && M.control.code === "200", "Model A control serves");
        ok(M.modela_431 && M.modela_431.code === "431",
           "S16: an over-cap INCOMPLETE header block gets a 431, not silence (got " +
           JSON.stringify(M.modela_431 && M.modela_431.code) + ")");
        ok(M.modela_413 && M.modela_413.code === "413",
           "S16: a request whose body passes the cap is a 413 (got " +
           JSON.stringify(M.modela_413 && M.modela_413.code) + ")");
        ok(M.version_anchor && M.version_anchor.conn === "close",
           "S8: HTTP/1.0 with a header value ending 'HTTP/1.1' still closes (got " +
           JSON.stringify(M.version_anchor && M.version_anchor.conn) + ")");
        ok(M.token_substr && M.token_substr.nresp === 2 &&
           M.token_substr.conn === "keep-alive",
           "S7: Connection: xclose is NOT the close token -- conn stays up (got " +
           JSON.stringify(M.token_substr) + ")");
        ok(M.token_real_close && M.token_real_close.conn === "close",
           "S7 control: Connection: close still closes");
        s.close();
    }
});

test("HTTPServer thread pool: header and body caps, bad Content-Length, the slowloris deadline and the close token", async () => {
    {
        const s = new HTTPServer({ port: 0, requestTimeoutMs: 1500,
                                   routes: { "/": "tp\n" } });
        s.start();
        const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
        write(`${T}/tp.js`, `import { connect, latin1 } from ${KIT};
const H = "127.0.0.1", P = Number(scriptArgs[1]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const R = [];
const rec = (tag, kv) => R.push(Object.assign({ tag }, kv));
async function open() {
    for (let i = 0; i < 40; i++) { try { return await connect(P, H, { connectTimeoutMs: 2000 }); } catch { await sleep(100); } }
    return null;
}
async function hit(payload, hard = 6.0) {
    const c = await open();
    if (!c) return { code: null, conn: "err", nresp: 0, closed: null };
    await c.write(payload);
    const r = await c.readAll({ timeoutMs: hard * 1000 });
    c.abort();
    const out = latin1(r.bytes), i = out.indexOf("\\r\\n\\r\\n");
    return { code: out.startsWith("HTTP") ? out.slice(9, 12) : null,
        conn: out.includes("Connection: keep-alive") ? "keep-alive" : out.includes("Connection: close") ? "close" : "?",
        nresp: out.split("HTTP/1.1").length - 1, body: i >= 0 ? out.slice(i + 4) : "" };
}
let r;
for (let i = 0; i < 50; i++) { r = await hit("GET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n"); if (r.code === "200") break; await sleep(100); }
rec("control", { code: r.code, body: r.body === "tp\\n" });
r = await hit("GET /big HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n");
rec("big", { code: r.code });
r = await hit("GET / HTTP/1.1\\r\\nHost: x\\r\\nX-Pad: " + "A".repeat(20000) + "\\r\\n\\r\\n");
rec("tp_431", { code: r.code });
r = await hit("POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 20000\\r\\n\\r\\n" + "B".repeat(20000));
rec("tp_413", { code: r.code });
r = await hit("POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: abc\\r\\n\\r\\n");
rec("tp_400", { code: r.code });
r = await hit("POST / HTTP/1.1\\r\\nHost: x\\r\\nContent-Length:\\r\\n\\r\\n");
rec("tp_400_empty", { code: r.code });
const sk = await open();
if (!sk) rec("slowloris", { closed: null, err: "no connection" });
else {
    const t0 = Date.now();
    await sk.write("GET / HTTP/1.1\\r\\nX-Slow: ");
    let closed = false;
    while (Date.now() - t0 < 8000) {
        if (!(await sk.write("x"))) { closed = true; break; }
        const d = await sk.read({ timeoutMs: 2000 });
        if (!d.timedOut && !d.bytes) { closed = true; break; }
        const e = await sk.read({ timeoutMs: 200 });
        if (!e.timedOut && !e.bytes) { closed = true; break; }
    }
    rec("slowloris", { closed, waited: Math.round((Date.now() - t0) / 100) / 10 });
    sk.abort();
}
r = await hit("GET / HTTP/1.0\\r\\nHost: x\\r\\nX-Pad: HTTP/1.1\\r\\n\\r\\n");
rec("tp_version_anchor", { conn: r.conn });
r = await hit("GET / HTTP/1.1\\r\\nHost: x\\r\\nConnection: xclose\\r\\n\\r\\nGET / HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n", 4.0);
rec("tp_token_substr", { nresp: r.nresp, conn: r.conn });
print(JSON.stringify(R));
`);
        sh(`${EXE} --std ${T}/tp.js ${s.port} > ${T}/tp.json 2>${T}/tp.err`);
        let P5 = {};
        try { for (const r of JSON.parse(cat(`${T}/tp.json`))) P5[r.tag] = r; }
        catch (e) { ok(false, "tp probe failed: " + cat(`${T}/tp.err`).slice(0, 80)); }
        ok(P5.control && P5.control.code === "200" && P5.control.body === true,
           "thread-pool control serves (writev body intact)");
        ok(P5.tp_431 && P5.tp_431.code === "431",
           "S16: thread-pool 20KB header block is a 431 (got " +
           JSON.stringify(P5.tp_431 && P5.tp_431.code) + ")");
        ok(P5.tp_413 && P5.tp_413.code === "413",
           "S16: thread-pool body past the 16KB frame buffer is a 413 (got " +
           JSON.stringify(P5.tp_413 && P5.tp_413.code) + ")");
        ok(P5.tp_400 && P5.tp_400.code === "400",
           "S16: 'Content-Length: abc' is a 400 (got " +
           JSON.stringify(P5.tp_400 && P5.tp_400.code) + ")");
        ok(P5.tp_400_empty && P5.tp_400_empty.code === "400",
           "S16: an empty Content-Length is a 400 (got " +
           JSON.stringify(P5.tp_400_empty && P5.tp_400_empty.code) + ")");
        ok(P5.slowloris && P5.slowloris.closed === true &&
           P5.slowloris.waited < 6,
           "S13: the dribbling peer is cut by the absolute deadline (" +
           (P5.slowloris ? P5.slowloris.waited : "?") + "s, requestTimeoutMs=1500)");
        ok(P5.tp_version_anchor && P5.tp_version_anchor.conn === "close",
           "S8: thread-pool HTTP/1.0 + fake header version still closes (got " +
           JSON.stringify(P5.tp_version_anchor && P5.tp_version_anchor.conn) + ")");
        ok(P5.tp_token_substr && P5.tp_token_substr.nresp === 2,
           "S7: thread-pool 'xclose' does not close -- pipelined request answered");
        s.close();
    }
});


await t.run();
