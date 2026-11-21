// flags: --std
// covers: c:src/modules/http/http-websocket.c
import * as os from "os";
// slots: 2
// peer-proof: prefix c -- fragflood: the server fails the connection after its fragment cap, and how many more fragments each client gets into the socket before its write fails is timing
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against HEAD with c.py taking the App port as its second argument, so the recording proxy sits in front of it, and a fixed Sec-WebSocket-Key and mask so both runs send the same bytes
import { spawnHelper } from "../lib/helper.js";
import * as std from "std";
import { suite } from "../lib/kit.js";

// The echo App child starts, and the rawpeer client is written, in the kit's
// before hook (never under --list); each case runs the client in one mode.
const t = suite("test_http_ws");
let A = null;
const ok = (c, w) => A.ok(c, w);
const test = (n, fn, o) => t.test(n, (a) => { A = a; if (why) a.skip(why); return fn(a); }, o);
const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_ws.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };

let why = null, made = false;
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
t.before(() => {
    made = true;
    sh(`rm -rf ${T}; mkdir -p ${T}`);
    const srv = std.open(`${T}/srv.js`, "w");
    srv.puts(`import { App } from "dyna:http";
import * as std from "std";
import * as os from "os";
const app = new App({ port: 0 });
app.ws("/ws", { open: () => {}, message: (c, d, bin) => c.send(d, bin), close: () => {} });
app.start();
const f = std.open("${T}/port.tmp", "w"); f.puts(String(app.port)); f.close();
os.rename("${T}/port.tmp", "${T}/port");
`);
    srv.close();
    spawnHelper(`${EXE} --std ${T}/srv.js >${T}/srv.log 2>&1`);
    for (let i = 0; i < 200 && !cat(`${T}/port`).trim(); i++) os.sleep(25);

    const cj = std.open(`${T}/c.js`, "w");
    cj.puts(`import { connect, latin1 } from ${KIT};
import * as std from "std";
const say = (s) => { std.out.puts(s + "\\n"); std.out.flush(); };
const PORT = parseInt(String(std.loadFile("${T}/port") || "").trim(), 10);
const KEY = "AAECAwQFBgcICQoLDA0ODw==";
const MASK = [0x11, 0x22, 0x33, 0x44];
const frame = (op, payload, fin = true) => {
    const n = payload.length;
    let h = String.fromCharCode((fin ? 0x80 : 0) | op);
    if (n < 126) h += String.fromCharCode(0x80 | n);
    else if (n < 65536) h += String.fromCharCode(0x80 | 126, n >> 8, n & 255);
    else { h += String.fromCharCode(0x80 | 127, 0, 0, 0, 0); for (let k = 24; k >= 0; k -= 8) h += String.fromCharCode((n >>> k) & 255); }
    let p = "";
    for (let i = 0; i < n; i++) p += String.fromCharCode(payload.charCodeAt(i) ^ MASK[i & 3]);
    return h + String.fromCharCode(...MASK) + p;
};
const s = await connect(PORT, "127.0.0.1", { connectTimeoutMs: 5000 });
const send = async (b) => { try { await s.write(b); return true; } catch (e) { return false; } };
await send("GET /ws HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\nSec-WebSocket-Key: " + KEY + "\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n");
const hs = await s.readUntil("\\r\\n\\r\\n", { timeoutMs: 5000 });
if (!hs.found) { say("handshake closed"); std.exit(1); }
const rd = async (n, timeoutMs) => {
    const r = await s.readBytes(n, { timeoutMs });
    if (r.timedOut) return { timeout: true };
    if (r.bytes.length < n) return null;
    return { b: latin1(r.bytes) };
};
async function readframe(timeoutMs) {
    const h = await rd(2, timeoutMs);
    if (!h || h.timeout) return h ? { timeout: true } : { op: null };
    const op = h.b.charCodeAt(0) & 0x0f;
    let ln = h.b.charCodeAt(1) & 0x7f;
    if (ln === 126) { const x = await rd(2, timeoutMs); if (!x || x.timeout) return { op: null }; ln = (x.b.charCodeAt(0) << 8) | x.b.charCodeAt(1); }
    else if (ln === 127) { const x = await rd(8, timeoutMs); if (!x || x.timeout) return { op: null }; ln = 0; for (let i = 0; i < 8; i++) ln = ln * 256 + x.b.charCodeAt(i); }
    if (!ln) return { op, d: "" };
    const p = await rd(ln, timeoutMs);
    return { op, d: p && !p.timeout ? p.b : null };
}
async function drain(deadline = 3000) {
    const end = Date.now() + deadline;
    let n = 0;
    while (Date.now() < end) {
        const f = await readframe(500);
        if (f.timeout) return n;
        if (f.op === null) return n;
        n++;
    }
    return n;
}
const mode = scriptArgs[1];
if (mode === "echo") {
    let okall = true;
    for (const n of [1, 7, 8, 9, 63, 64, 65, 1000, 100003]) {
        let payload = "";
        for (let i = 0; i < n; i++) payload += String.fromCharCode((i * 37 + n) & 0xff);
        await send(frame(0x2, payload));
        const f = await readframe(5000);
        if (f.d !== payload) { say("MISMATCH len=" + n); okall = false; break; }
    }
    say(okall ? "ECHO_OK" : "ECHO_BAD");
} else if (mode === "pingflood") {
    const SENT = 500;
    for (let i = 0; i < SENT; i++) if (!(await send(frame(0x9, "p")))) break;
    const pongs = await drain();
    say(pongs < SENT / 2 ? "PING_BOUNDED " + pongs : "PING_UNBOUNDED " + pongs);
} else if (mode === "bigping") {
    await send(frame(0x9, "P".repeat(200000)));
    const pongs = await drain(2000);
    say(pongs === 0 ? "BIGPING_REFUSED" : "BIGPING_ECHOED " + pongs);
} else if (mode === "smallping") {
    await send(frame(0x9, "P".repeat(125)));
    const pongs = await drain(2000);
    say(pongs >= 1 ? "SMALLPING_OK" : "SMALLPING_DROPPED");
} else if (mode === "fragctl") {
    if (await send(frame(0x9, "p", false))) await send(frame(0x2, "after"));
    const n = await drain(2000);
    say(n === 0 ? "FRAGCTL_REFUSED" : "FRAGCTL_ACCEPTED " + n);
} else if (mode === "fragflood") {
    if (await send(frame(0x2, "a", false))) {
        let ok = true;
        for (let i = 0; i < 20000 && ok; i++) ok = await send(frame(0x0, "a", false));
        if (ok) await send(frame(0x0, "a", true));
    }
    const echoes = await drain();
    say(echoes === 0 ? "FRAG_REFUSED" : "FRAG_DELIVERED " + echoes);
}
s.abort();
std.exit(0);
`);
    cj.close();
});
t.after(() => { if (made) sh(`pkill -f "${T}/srv.js" 2>/dev/null; rm -rf ${T}`); });

test("the echo server reports its port", () => {
    ok(cat(`${T}/port`).trim() !== "", "the echo server reported its port (log: " + cat(`${T}/srv.log`).trim() + ")");
});

test("masked payloads echo byte-for-byte", () => {
    sh(`${EXE} --std ${T}/c.js echo > ${T}/echo.out 2>&1; true`);
    const echo = cat(`${T}/echo.out`).trim();
    ok(echo.indexOf("ECHO_OK") >= 0, "masked payloads echo byte-for-byte (got " + echo + ")");

});

test("a control frame over 125 octets fails the connection", () => {
    sh(`${EXE} --std ${T}/c.js bigping > ${T}/bp.out 2>&1; true`);
    const bp = cat(`${T}/bp.out`).trim();
    ok(bp.indexOf("BIGPING_REFUSED") >= 0,
       "a control frame over 125 octets fails the connection rather than being " +
       "echoed back (got " + bp + ")");

});

test("a 125-octet ping is still answered", () => {
    sh(`${EXE} --std ${T}/c.js smallping > ${T}/sp.out 2>&1; true`);
    const sp = cat(`${T}/sp.out`).trim();
    ok(sp.indexOf("SMALLPING_OK") >= 0,
       "and a 125-octet ping is still answered, so the refusal is not blanket " +
       "(got " + sp + ")");

});

test("a fragmented control frame fails the connection", () => {
    sh(`${EXE} --std ${T}/c.js fragctl > ${T}/fc.out 2>&1; true`);
    const fc = cat(`${T}/fc.out`).trim();
    ok(fc.indexOf("FRAGCTL_REFUSED") >= 0,
       "a fragmented control frame fails the connection (got " + fc + ")");

});

test("a ping flood is answered a bounded number of times", () => {
    sh(`${EXE} --std ${T}/c.js pingflood > ${T}/ping.out 2>&1; true`);
    const ping = cat(`${T}/ping.out`).trim();
    ok(ping.indexOf("PING_BOUNDED") >= 0, "ping flood answered a bounded number of times (got " + ping + ")");

});

test("an over-long fragment chain is never delivered", () => {
    sh(`${EXE} --std ${T}/c.js fragflood > ${T}/frag.out 2>&1; true`);
    const frag = cat(`${T}/frag.out`).trim();
    ok(frag.indexOf("FRAG_REFUSED") >= 0, "over-long fragment chain is never delivered (got " + frag + ")");
});

await t.run();
