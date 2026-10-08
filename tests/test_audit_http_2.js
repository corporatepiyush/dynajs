import "./httpc.js";
import { App, TCPServer, HTTPClient } from "dyna:net";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const bytes = (s) => { const a = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff; return a; };
const str = (b) => { let s = ""; for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]); return s; };

async function raw(app, reqText) {
    let reply = "";
    const cl = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
        connect: (c, e) => { if (!e) c.write(bytes(reqText)); },
        data: (c, b) => { reply += str(b); },
    });
    await wait(400);
    cl.close();
    return reply;
}

// 1. N1-04: app.close() inside middleware must not crash and must not
//    dispatch into the freed route table. The reply may be empty (the app
//    tears the socket down as it closes) -- the assertion is "no crash".
{
    const app = new App({ port: 0 });
    app.use(() => { app.close(); });
    app.get("/q", () => "bye");
    app.start();
    await raw(app, "GET /q HTTP/1.1\r\nHost: x\r\n\r\n");
    app.dispose();
    ok(true, "app.close() in middleware: no crash");
}

// 2. route registration inside middleware must not crash (realloc of the
//    route table while a request is being dispatched).
{
    const app = new App({ port: 0 });
    let first = true;
    app.use(() => { if (first) { first = false; app.get("/late", () => "late"); } });
    app.get("/q", () => "bye");
    app.start();
    const r1 = await raw(app, "GET /q HTTP/1.1\r\nHost: x\r\n\r\n");
    const r2 = await raw(app, "GET /q HTTP/1.1\r\nHost: x\r\n\r\n");
    ok(r1.includes("200") || r1.includes("bye"), "first dispatch survives a route registration in middleware");
    ok(r2.includes("200") || r2.includes("bye"), "second dispatch survives the realloc'd route table");
    app.close();
}

// 3. app.close() inside a handler must not crash; the connection is torn
//    down with the app so no reply is required.
{
    const app = new App({ port: 0 });
    app.get("/q", () => { app.close(); return "bye"; });
    app.start();
    await raw(app, "GET /q HTTP/1.1\r\nHost: x\r\n\r\n");
    await wait(200);
    app.dispose();
    ok(true, "app.close() in handler: no crash");
}

// 4. app.close() inside an RPC method must not crash.
{
    const app = new App({ port: 0 });
    app.rpc("/r", { stop: () => { app.close(); return 1; } });
    app.start();
    const body = JSON.stringify({ jsonrpc: "2.0", method: "stop", id: 1 });
    await raw(app, "POST /r HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: " + body.length + "\r\n\r\n" + body);
    await wait(200);
    app.dispose();
    ok(true, "app.close() in rpc method: no crash");
}

// 5. sanity: after all that, a fresh App still serves and dispatches normally
//    (middleware response short-circuit still works; middleware runs for
//    matched routes only, so /block is registered too).
{
    const app = new App({ port: 0 });
    app.use((req) => { if (req.path === "/block") return { response: { status: 403, body: "denied" } }; });
    app.get("/q", () => "bye");
    app.get("/block", () => "should not run");
    app.start();
    const c = new HTTPClient();
    eq((await c.getAsync("http://127.0.0.1:" + app.port + "/q")).body, "bye", "dispatch still works after the fixes");
    const blocked = await c.getAsync("http://127.0.0.1:" + app.port + "/block");
    eq(blocked.status, 403, "middleware response short-circuit still works");
    c.close();
    app.close();
}

if (fails) {
    print("test_audit_http_2: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_audit_http_2 failed");
}
print("test_audit_http_2: " + n + " assertions, 0 failures");
