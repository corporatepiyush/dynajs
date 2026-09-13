/* test_ws_client.js -- WsClient, the WebSocket CLIENT (RFC 6455 side).
 *
 * The server side had a full frame-path test (test_http_ws.js); this is
 * the client, which the review listed as gap G2. The servers run in CHILD
 * PROCESSES: the handshake is OFFLOADED to the io pool, but the child
 * process keeps the tests honest about the wire -- an in-process App could
 * answer without exercising a real connect. After the 101, every frame
 * moves on the shared reactor.
 *
 * Semantics pinned here: argument and scheme errors THROW from the
 * constructor; connection and handshake failures surface as
 * close(1006, reason), because there is no error event. The accept-key
 * forgery is the load-bearing case: the client verifies the
 * Sec-WebSocket-Accept against ITS OWN key, so a server that never saw
 * the request cannot answer one -- an identifier match is not enough.
 */
import { WsClient, App, TCPServer } from "dyna:net";
import * as os from "os";
import * as std from "std";
import { Path, makeTempDir, removeAll } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const threw = (fn, re, m) => {
    try { fn(); fail++; print("  FAIL: " + m + ": did not throw"); }
    catch (e) { ok(re.test(e.message), m + " -- " + e.message); }
};
const wait = (ms) => new Promise((res) => setTimeout(res, ms));
/* Await `pred()` polling every 20ms up to `ms`; throw on timeout. */
async function until(pred, m, ms = 8000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
        if (pred()) return;
        await wait(20);
    }
    throw new Error(m + ": never happened");
}

const T = String(makeTempDir("wsc"));
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };

/* Spawn a dynajs child. The script writes its pid and port to PIDFILE /
   PORTFILE after start() and runs until killed -- by PID, not pkill
   pattern, so a same-named unrelated process cannot be hit. */
async function spawnChild(scriptBody) {
    const name = "child" + Math.floor(Math.random() * 1e9);
    const js = T + "/" + name + ".js";
    const pf = T + "/" + name + ".port";
    const pidf = T + "/" + name + ".pid";
    write(js, scriptBody.split("PORTFILE").join(JSON.stringify(pf))
                .split("PIDFILE").join(JSON.stringify(pidf)));
    sh(`./dynajs ${js} >${T}/${name}.log 2>&1 &`);
    let port = 0;
    for (let i = 0; i < 200 && !port; i++) {
        await wait(50);
        port = parseInt(cat(pf).trim() || "0", 10);
    }
    ok(port > 0, "the child came up");
    const pid = parseInt(cat(pidf).trim() || "0", 10);
    ok(pid > 0, "the child reported its pid");
    return { port, kill: () => { if (pid) sh(`kill ${pid} 2>/dev/null`); } };
}

/* -------------------------------------------------- the echo server child */
const echoSrv = await spawnChild(`import { App } from "dyna:net";
import { pid } from "dyna:sys";
import * as std from "std";
const app = new App({ port: 0, idleTimeoutMs: 60000 });
app.ws("/ws", {
    open: (c) => {},
    message: (c, d, bin) => c.send(d, bin),
    close: (c, code, reason) => {
        const f = std.open("${T}/srv.close", "w"); f.puts(code + " " + reason); f.close();
    },
});
app.start();
const f = std.open(PORTFILE, "w"); f.puts(String(app.port)); f.close();
const p = std.open(PIDFILE, "w"); p.puts(String(pid())); p.close();
`);

/* ---- the happy path: open, text echo, binary echo, big frame ---- */
{
    const got = { open: null, messages: [], closeCode: null };
    const ws = new WsClient("ws://127.0.0.1:" + echoSrv.port + "/ws", {
        open: (self) => { got.open = self; },
        message: (self, data, bin) => { got.messages.push([data, bin]); },
        close: (self, code) => { got.closeCode = code; },
    });
    /* the handshake is async: `open` fires on the reactor, and anything
       sent before it is not yet deliverable */
    await until(() => got.open !== null, "open fired");
    ws.send("ping");
    ws.send(new Uint8Array([1, 2, 3, 4]).buffer);
    const big = new Uint8Array(70000);
    for (let i = 0; i < big.length; i++) big[i] = i & 0xff;
    ws.send(big.buffer);

    await until(() => got.messages.length === 3, "three echoes came back");
    eq(got.messages[0][0], "ping", "the text echo is the text");
    eq(got.messages[0][1], false, "flagged as text");
    eq(got.messages[1][1], true, "the ArrayBuffer echo is flagged binary");
    ok(got.messages[1][0] instanceof ArrayBuffer, "and arrives as an ArrayBuffer");
    const u = new Uint8Array(got.messages[1][0]);
    eq(u.length, 4, "the binary payload length");
    eq(u[0] + u[1] + u[2] + u[3], 10, "the binary payload bytes");
    const ub = new Uint8Array(got.messages[2][0]);
    eq(ub.length, 70000, "a 70000-byte frame (the 64-bit length path) round trips");
    let bad = 0;
    for (let i = 0; i < ub.length; i++) if (ub[i] !== (i & 0xff)) bad++;
    eq(bad, 0, "byte for byte");

    /* client-initiated close: the close handler fires, and the SERVER-side
       handler records it (the file write races the frame, so poll it) */
    ws.close();
    await until(() => got.closeCode !== null, "close() fires the client close handler");
    ok(got.closeCode === 1000, "with the normal closure code (" + got.closeCode + ")");
    let srvSaw = "";
    try { await until(() => { srvSaw = cat(T + "/srv.close").trim(); return srvSaw !== ""; },
                      "the server-side close handler ran"); }
    catch (e) { ok(false, e.message); }
    ok(parseInt(srvSaw.split(" ")[0], 10) >= 1000, "server-side saw a normal closure");
}

/* ---- the LOOP STAYS LIVE during the handshake: a timer must fire while
        the connect is in flight (the blocking predecessor failed this) ---- */
{
    const s = new TCPServer({ port: 0 });
    s.start({
        connect: () => {},   /* accept and never answer the upgrade */
        data: () => {},
        close: () => {},
    });
    const got = { timer: false, closed: null };
    const ws = new WsClient("ws://127.0.0.1:" + s.port + "/ws", {
        open: () => {},
        message: () => {},
        close: (self, code, reason) => { got.closed = [code, reason]; },
    });
    setTimeout(() => { got.timer = true; }, 50);
    ws.close();
    await until(() => got.closed !== null, "the closed handshake settles");
    /* The timer fires at 50ms. A BLOCKING handshake would hold the loop
       for its whole 15s timeout, so the 1s bound here IS the assertion. */
    try {
        await until(() => got.timer === true,
                    "a timer fired while the handshake was pending", 1000);
        ok(true, "a timer fired while the handshake was pending (the loop was not blocked)");
    } catch (e) {
        ok(false, e.message);
    }
    s.close();
}

/* ---- scheme and reachability refusals: SYNCHRONOUS vs close(1006) ---- */
{
    threw(() => new WsClient("wss://127.0.0.1:1/ws", {}), /wss/, "wss:// throws rather than silently downgrading");
    threw(() => new WsClient("http://127.0.0.1:1/ws", {}), /ws:\/\//, "a non-ws scheme is refused");
    threw(() => new WsClient(), /WsClient/, "no arguments is a usage error");

    /* a port nothing listens on: the constructor returns, close(1006)
       carries the failure */
    const s = new TCPServer({ port: 0 });
    const deadPort = s.port;
    s.close();
    const got = { closed: null };
    const ws = new WsClient("ws://127.0.0.1:" + deadPort + "/ws", {
        open: () => {},
        message: () => {},
        close: (self, code, reason) => { got.closed = [code, reason]; },
    });
    await until(() => got.closed !== null, "a refused connection settles as close");
    eq(got.closed[0], 1006, "with the abnormal-closure code");
    ok(/connection failed|refused/.test(got.closed[1]),
       "and a reason naming the failure (" + got.closed[1] + ")");
}

/* ---- the accept-key forgery: a child peer answering with a WRONG
        Sec-WebSocket-Accept -- the shape of an off-path hijack where the
        identifier matches but the peer never saw our request. ---- */
{
    const fake = await spawnChild(`import { TCPServer } from "dyna:net";
import { pid } from "dyna:sys";
import * as std from "std";
const s = new TCPServer({ port: 0 });
s.start({
    connect: () => {},
    data: (c, b) => {
        const req = new TextDecoder().decode(b);
        if (req.indexOf("Sec-WebSocket-Key") < 0) return;
        c.write("HTTP/1.1 101 Switching Protocols\\r\\nUpgrade: websocket\\r\\n"
                + "Connection: Upgrade\\r\\nSec-WebSocket-Accept: "
                + "AAAAAAAAAAAAAAAAAAAAAAAAAAA=\\r\\n\\r\\n");
    },
    close: () => {},
});
const f = std.open(PORTFILE, "w"); f.puts(String(s.port)); f.close();
const p = std.open(PIDFILE, "w"); p.puts(String(pid())); p.close();
`);
    const got = { closed: null };
    const ws = new WsClient("ws://127.0.0.1:" + fake.port + "/ws", {
        open: () => {},
        message: () => {},
        close: (self, code, reason) => { got.closed = [code, reason]; },
    });
    await until(() => got.closed !== null, "the forgery settles as close");
    eq(got.closed[0], 1006, "with the abnormal-closure code");
    ok(/Sec-WebSocket-Accept/.test(got.closed[1]),
       "a wrong Sec-WebSocket-Accept is refused, not trusted (" + got.closed[1] + ")");
    fake.kill();
}

/* ---- a non-101 answer is refused, naming the status ---- */
{
    const fake = await spawnChild(`import { TCPServer } from "dyna:net";
import { pid } from "dyna:sys";
import * as std from "std";
const s = new TCPServer({ port: 0 });
s.start({
    connect: () => {},
    data: (c, b) => { c.write("HTTP/1.1 403 Forbidden\\r\\nContent-Length: 0\\r\\n\\r\\n"); },
    close: () => {},
});
const f = std.open(PORTFILE, "w"); f.puts(String(s.port)); f.close();
const p = std.open(PIDFILE, "w"); p.puts(String(pid())); p.close();
`);
    const got = { closed: null };
    const ws = new WsClient("ws://127.0.0.1:" + fake.port + "/ws", {
        open: () => {},
        message: () => {},
        close: (self, code, reason) => { got.closed = [code, reason]; },
    });
    await until(() => got.closed !== null, "the non-101 settles as close");
    ok(/status 403/.test(got.closed[1]),
       "a non-101 response is refused with its status named (" + got.closed[1] + ")");
    fake.kill();
}

/* ---- close() before the handshake completes: the in-flight job must
        settle into a freed husk without use-after-free ---- */
{
    const s = new TCPServer({ port: 0 });
    s.start({
        connect: () => {},   /* accept and stall: the upgrade never answers */
        data: () => {},
        close: () => {},
    });
    const got = { closed: null };
    const ws = new WsClient("ws://127.0.0.1:" + s.port + "/ws", {
        open: () => {},
        message: () => {},
        close: (self, code, reason) => { got.closed = [code, reason]; },
    });
    ws.close();
    await until(() => got.closed !== null, "close during the handshake settles");
    eq(got.closed[0], 1000, "with the caller's code");
    s.close();
}

echoSrv.kill();
removeAll(new Path(T));
print("test_ws_client: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
std.exit(0);
