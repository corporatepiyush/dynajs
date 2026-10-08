// timeout: 300
// tests/test_audit_w3_native_6.js -- audit wave 3, batch 6.
// Each section names its ORACLE and CONTROL; [red] fails on the pre-fix binary.
//   N1-13 a buffered HTTP client exchange has an overall deadline
//   N1-15 WebSocket routes can inspect the upgrade request (Origin check)
// build-note: needs the native modules
import "./httpc.js";
import { HTTPClient } from "dyna:http";
import { TCPServer } from "dyna:net";

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- N1-13 --------------------------------------------------------------
// ORACLE: ordering against the peer. The mock promises 100000 body bytes and
// then sends ONE byte every 50 ms: each read succeeds well inside the 150 ms
// per-read timeout, so only an overall bound (20 x timeout = 3 s, stated in
// the d.ts) can end the exchange. "Settled while the server was still
// dripping" is the property; the 9 s guard only turns a hang into a verdict.
// CONTROL: the same client gets a complete response from a prompt server.
{
    let dripped = 0, conns = [];
    const srv = new TCPServer({ port: 0 });
    srv.start({
        data: (conn, bytes) => {
            const text = new TextDecoder().decode(bytes);
            if (/GET \/fast/.test(text)) { conn.write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"); return; }
            conn.write("HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n");
            const t = setInterval(() => { try { conn.write("x"); dripped++; } catch (e) { clearInterval(t); } }, 50);
            conns.push(t);
        },
    });
    const c = new HTTPClient();
    c.setTimeout(150);
    const fast = await c.getAsync("http://127.0.0.1:" + srv.port + "/fast");
    ok(fast.status === 200, "control: a prompt response is delivered (status " + fast.status + ")");
    let outcome = "pending", at = -1;
    const req = c.getAsync("http://127.0.0.1:" + srv.port + "/slow").then(() => { outcome = "resolved"; at = dripped; }, (e) => { outcome = "rejected: " + String(e.message || e).slice(0, 60); at = dripped; });
    await Promise.race([req, sleep(9000)]);
    const after = dripped;
    ok(/^rejected/.test(outcome), "[red] N1-13: a slow-drip response is cut off by the overall deadline (" + outcome + ")");
    ok(at >= 20 && at < 100000, "[red] N1-13: it ended while the server was still sending, after many successful reads (" + at + " bytes dripped)");
    await sleep(200);
    ok(dripped >= after, "fixture: the server kept dripping throughout");
    for (const t of conns) clearInterval(t);
    try { c.close(); } catch (e) {}
    srv.close();
}

// ---- N1-15 --------------------------------------------------------------
// ORACLE: RFC 6455 section 10.2 -- a server that is not meant for arbitrary
// web pages "SHOULD verify the Origin field" and may answer with an HTTP
// error; and section 4.2.2: the 101 response is what opens the socket.
// The handshake bytes are written by hand so the Origin is ours to choose.
// CONTROLS: the allowed origin still upgrades (101) and its open handler
// runs; a route with no upgrade hook accepts as before.
{
    const { App } = await import("dyna:http");
    const seen = [];
    let opened = 0;
    const app = new App({ port: 0 });
    app.ws("/guarded", {
        upgrade: (req) => { seen.push(req.origin + "|" + req.cookie + "|" + req.protocol); return req.origin === "https://good.example"; },
        open: () => { opened++; },
    });
    app.ws("/thrower", { upgrade: () => { throw new Error("nope"); }, open: () => { opened += 100; } });
    app.ws("/open", { open: () => { opened += 10; } });
    app.start();
    function handshake(path, origin) {
        return new Promise((resolve) => {
            let buf = "", done = false;
            const fin = () => { if (!done) { done = true; try { cli.close(); } catch (e) {} resolve(buf.split("\r\n")[0]); } };
            const cli = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
                connect: (conn) => {
                    if (!conn) return fin();
                    conn.write("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
                        (origin ? "Origin: " + origin + "\r\n" : "") + "Cookie: sid=abc\r\nSec-WebSocket-Protocol: chat\r\n\r\n");
                },
                data: (conn, bytes) => { buf += new TextDecoder().decode(bytes); if (buf.includes("\r\n\r\n")) fin(); },
                close: fin,
            });
            setTimeout(fin, 3000);
        });
    }
    const good = await handshake("/guarded", "https://good.example");
    const evil = await handshake("/guarded", "https://evil.example");
    const none = await handshake("/guarded", null);
    const thrown = await handshake("/thrower", "https://good.example");
    const plain = await handshake("/open", "https://evil.example");
    await sleep(100);
    ok(/ 101 /.test(good), "control: the allowed origin is upgraded (" + good + ")");
    ok(/ 403 /.test(evil), "[red] N1-15: a foreign Origin is answered 403 before any upgrade (" + evil + ")");
    ok(/ 403 /.test(none), "[red] N1-15: a missing Origin reaches the hook as null and is refused here (" + none + ")");
    ok(/ 403 /.test(thrown), "[red] N1-15: a throwing hook refuses the upgrade (" + thrown + ")");
    ok(/ 101 /.test(plain), "control: a route without the hook accepts as before (" + plain + ")");
    ok(seen[0] === "https://good.example|sid=abc|chat" && seen[2] === "null|sid=abc|chat", "[red] N1-15: the hook sees Origin, Cookie and the subprotocol (" + seen.join(" ; ") + ")");
    ok(opened === 11, "[red] N1-15: open ran only for the two accepted sockets (" + opened + ")");
    app.close();
}

print("test_audit_w3_native_6: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_6: " + failures + " failures");
