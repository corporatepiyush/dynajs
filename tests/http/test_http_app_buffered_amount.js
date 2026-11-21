// flags: --std
// timeout: 120
// covers: dyna:http.App
// WsConn/SseConn.bufferedAmount follows WHATWG WebSocket (and Node ws on the server): open, the bytes queued and unwritten;
// the handshake response counts until written; after close the dropped queue left with the socket, and every later send adds its data's byte length without sending it.
// The App's 4 MB outbound cap closes a connection whose queue would pass it, so a 24 MB flood at a peer that never reads
// climbs to at most 4 MB, drops to 0 at the cap's close (the refused send is not counted), then grows 1 MB per send.
// Rule 18: the getter takes no argument, so hostile valueOf/toString/getters and NUL bytes have nothing to reach, and it
// allocates only its number, so it has no allocation-failure path. Covered: wrong receiver, the prototype and a Proxy
// (TypeError), the edges 0 (fresh, after close()) and the cap, GC while frames are queued, and a one-shot interrupt
// (--timeout-ms) ending a catch-all loop over the getter.
import { App } from "dyna:http";
import { Exec, args } from "dyna:sys";
import { connect } from "../lib/rawpeer.js";
import * as std from "std";
import { suite } from "../lib/kit.js";

const CAP = 4 << 20, MB = new Uint8Array(1 << 20), KEY = "dGhlIHNhbXBsZSBub25jZQ==";
const WS_REQ = (p) => "GET " + p + " HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + KEY + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
const SSE_REQ = (p) => "GET " + p + " HTTP/1.1\r\nHost: h\r\nAccept: text/event-stream\r\n\r\n";

async function withConn(kind, onOpen) {
    let done;
    const got = new Promise((r) => { done = r; });
    const app = new App({ port: 0 });
    const open = (conn) => { try { done({ v: onOpen(conn) }); } catch (e) { done({ e }); } };
    if (kind === "ws") app.ws("/c", { open, message: () => {}, close: () => {} });
    else app.sse("/c", { open, close: () => {} });
    app.start();
    const c = await connect(app.port);
    c.conn.pause();
    await c.write(kind === "ws" ? WS_REQ("/c") : SSE_REQ("/c"));
    const r = await got;
    if (!r.e) try { r.v = await r.v; } catch (e) { r.e = e; }
    c.abort();
    app.close();
    if (r.e) throw r.e;
    return r.v;
}
const sendOf = (kind, conn) => kind === "ws" ? () => conn.send(MB) : () => conn.send("F".repeat(1 << 20));

if (args().includes("--child")) {
    const app = new App({ port: 0 });
    app.ws("/c", { open: (ws) => { ws.send(MB); let n = 0; for (;;) { try { n += ws.bufferedAmount; } catch (e) {} } }, message: () => {}, close: () => {} });
    app.start();
    const c = await connect(app.port);
    c.conn.pause();
    await c.write(WS_REQ("/c"));
} else {
    const t = suite("test_http_app_buffered_amount");
    for (const kind of ["ws", "sse"]) {
        t.test(kind + ": a fresh connection counts only its unwritten handshake response, then drains to 0", async (a) => {
            const [fresh, later] = await withConn(kind, (conn) => { const f = conn.bufferedAmount; return new Promise((res) => setTimeout(() => res([f, conn.bufferedAmount]), 100)); });
            a.ok(fresh > 0 && fresh < 512, "in open() the handshake response is still queued: " + fresh + " bytes");
            a.eq(later, 0, "once the loop wrote it, nothing is queued");
        });
        t.test(kind + ": a flood at a stalled peer climbs to at most the 4 MB cap, then counts each send after the cap's close", async (a) => {
            const r = await withConn(kind, (conn) => {
                const send = sendOf(kind, conn), trace = [];
                for (let i = 0; i < 24; i++) { send(); trace.push(conn.bufferedAmount); }
                return trace;
            });
            const drop = r.findIndex((v, i) => i > 0 && v < r[i - 1]);
            const top = Math.max(...r.slice(0, drop < 0 ? r.length : drop));
            a.ok(top > 0 && top <= CAP, "bufferedAmount while open peaks in (0, " + CAP + "] at " + top);
            a.ok(drop > 0 && r[drop] === 0, "the cap's close drops the count to 0 and the refused send is not counted (trace " + r.join(",") + ")");
            a.ok(drop > 0 && r.slice(drop + 1).every((v, i) => v === (i + 1) * (1 << 20)), "each send after the close adds its 1 MB byte length");
            a.ok(r.every((v) => Number.isSafeInteger(v) && v >= 0), "every read is a non-negative safe integer");
        });
        t.test(kind + ": close() drops the queue with the socket; a later send adds its byte length (WHATWG)", async (a) => {
            const r = await withConn(kind, (conn) => { const send = sendOf(kind, conn); send(); send(); const before = conn.bufferedAmount; conn.close(); const after = conn.bufferedAmount; send(); return [before, after, conn.bufferedAmount]; });
            a.ok(r[0] > 0, "two 1 MB sends at a stalled peer leave bytes queued (" + r[0] + ")");
            a.eq(r[1], 0, "0 right after close()");
            a.eq(r[2], 1 << 20, "a 1 MB send after close() adds 1048576 and is never sent");
        });
        t.test(kind + ": GC while frames are queued keeps the count", async (a) => {
            const r = await withConn(kind, (conn) => { const send = sendOf(kind, conn); send(); send(); const before = conn.bufferedAmount; std.gc(); return [before, conn.bufferedAmount]; });
            a.ok(r[0] > 0 && r[1] === r[0], "the count survives a GC with the queue unchanged (" + r.join(" -> ") + ")");
        });
        t.test(kind + ": the getter refuses a foreign receiver", async (a) => {
            const r = await withConn(kind, (conn) => {
                const proto = Object.getPrototypeOf(conn), get = Object.getOwnPropertyDescriptor(proto, "bufferedAmount").get;
                const kindOf = (f) => { try { f(); return "none"; } catch (e) { return e instanceof TypeError ? "TypeError" : String(e); } };
                return [kindOf(() => get.call({})), kindOf(() => proto.bufferedAmount), kindOf(() => get.call(new Proxy(conn, {}))), kindOf(() => get.call(undefined)), typeof Object.getOwnPropertyDescriptor(proto, "bufferedAmount").set];
            });
            a.eq(r.slice(0, 4).join(","), "TypeError,TypeError,TypeError,TypeError", "plain object, prototype, Proxy and undefined receivers throw TypeError");
            a.eq(r[4], "undefined", "bufferedAmount has no setter");
        });
    }
    t.test("a one-shot interrupt ends a catch-all loop over the getter", (a) => {
        const t0 = Date.now();
        const i = Exec(args()[0], ["--std", "--timeout-ms", "300", "tests/http/test_http_app_buffered_amount.js", "--child"], { timeoutMs: 20000 });
        a.ok(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), "the child ends on the interrupt: rc " + i.code + " after " + (Date.now() - t0) + " ms " + (i.stderr + i.stdout).slice(-160));
    });
    await t.run();
}
