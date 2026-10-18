// flags: --std
import { App, TCPServer } from "dyna:net";
import * as std from "std";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
function wait(ms) { return new Promise((res) => setTimeout(res, ms)); }

function rawClient(port) {
    let buf = "";
    let conn = null;
    let cli;
    const connected = new Promise((res) => {
        cli = TCPServer.connect({ host: "127.0.0.1", port }, {
            connect: (c, err) => { conn = c; res(); },
            data: (c, b) => { buf += new TextDecoder().decode(b); },
            close: () => {},
        });
    });
    return {
        client: cli,
        async send(s, tag) {
            await connected;
            try { conn.write(s); }
            catch (e) { throw new Error("send(" + tag + ") failed: " + e.message); }
        },        close() { try { conn ? conn.close() : null; } catch (e) {} },
        waitFor(needle, m, ms = 3000) {
            return new Promise((res, rej) => {
                const t0 = Date.now();
                const poll = () => {
                    if (buf.indexOf(needle) >= 0) { res(buf); return; }
                    if (Date.now() - t0 > ms) { rej(new Error(m + ": never saw " + JSON.stringify(needle) + " in " + JSON.stringify(buf))); return; }
                    setTimeout(poll, 20);
                };
                poll();
            });
        },
        get buf() { return buf; },
    };
}

{
    const events = [];
    const app = new App({ port: 0, idleTimeoutMs: 5000 });
    app.sse("/stream", {
        open: (conn) => {
            events.push(["open"]);
            conn.send("hello");
            conn.send("line1\nline2");
            conn.send("named", "evt");
            conn.send(42);
            conn.send("");
        },
        close: () => { events.push(["close"]); },
    });
    app.start();
    const rc = rawClient(app.port);
    await rc.send("GET /stream HTTP/1.1\r\nHost: x\r\n\r\n", "stream");
    let seen;
    try { seen = await rc.waitFor("data: \n\n", "the frame stream"); }
    catch (e) { ok(false, String(e.message)); }
    if (seen) {
        const head = seen.split("\r\n\r\n")[0];
        ok(head.indexOf("HTTP/1.1 200") >= 0, "the upgrade answers 200");
        ok(head.toLowerCase().indexOf("content-type: text/event-stream") >= 0,
           "with the SSE content type (" + head + ")");
        ok(head.toLowerCase().indexOf("cache-control: no-cache") >= 0,
           "and the no-cache directive");
        const want = "data: hello\n\n"
                   + "data: line1\ndata: line2\n\n"
                   + "event: evt\ndata: named\n\n"
                   + "data: 42\n\n"
                   + "data: \n\n";
        ok(seen.indexOf(want) >= 0,
           "the frame bytes are the SSE grammar, in order (got " + JSON.stringify(seen) + ")");
    }
    {
        let conn = null;
        const app2 = new App({ port: 0, idleTimeoutMs: 5000 });
        app2.sse("/s2", { open: (c) => { conn = c; }, close: () => {} });
        app2.start();
        const rc2 = rawClient(app2.port);
        await rc2.send("GET /s2 HTTP/1.1\r\nHost: x\r\n\r\n", "s2");
        await rc2.waitFor("200 OK", "the second handshake");
        ok(conn !== null, "the open handler receives the SseConn");
        let e1, e2;
        try { conn.send("x", "a\nb"); } catch (e) { e1 = e; }
        try { conn.send("a\rb"); } catch (e) { e2 = e; }
        ok(e1 && /newline/.test(e1.message), "an event name with a newline throws (" + (e1 && e1.message) + ")");
        ok(e2 && /\\r/.test(e2.message), "data with a CR throws (" + (e2 && e2.message) + ")");
        await rc2.send("GET /other HTTP/1.1\r\nHost: x\r\n\r\n", "junk");
        await wait(150);
        ok(rc2.buf.indexOf("HTTP/1.1") === rc2.buf.lastIndexOf("HTTP/1.1"),
           "inbound bytes on an SSE conn are discarded, never answered");
        conn.send("still-alive");
        await rc2.waitFor("data: still-alive", "a stream survives inbound junk");
        conn.close();
        await wait(150);
        rc2.close();
        app2.close();
    }
    rc.close();
    app.close();
}

{
    let closedWith = null;
    const app = new App({ port: 0, idleTimeoutMs: 5000 });
    app.sse("/s3", { open: () => {}, close: (conn) => { closedWith = conn; } });
    app.start();
    const rc = rawClient(app.port);
    await rc.send("GET /s3 HTTP/1.1\r\nHost: x\r\n\r\n", "s3");
    await rc.waitFor("200 OK", "handshake");
    ok(closedWith === null, "no close before the client leaves");
    rc.close();
    await wait(200);
    ok(closedWith !== null, "the client's disconnect reaches the close handler");
    let threwOnClosed = false;
    try { closedWith.send("nope"); } catch (e) { threwOnClosed = true; }
    ok(!threwOnClosed, "send() on a closed SseConn is a silent no-op");
    app.close();
}

{
    let conn = null;
    const app = new App({ port: 0, idleTimeoutMs: 400 });
    app.sse("/s4", { open: (c) => { conn = c; }, close: () => {} });
    app.start();
    const rc = rawClient(app.port);
    await rc.send("GET /s4 HTTP/1.1\r\nHost: x\r\n\r\n", "s4");
    await rc.waitFor("200 OK", "handshake");
    await wait(1400);
    conn.send("after-idle");
    try {
        await rc.waitFor("data: after-idle", "a silent stream survives the sweep", 2000);
        ok(true, "the sweep does not reap a server-driven stream");
    } catch (e) {
        ok(false, String(e.message));
    }
    rc.close();
    app.close();
}

print("test_http_sse: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
std.exit(0);
