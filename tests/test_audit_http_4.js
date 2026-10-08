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
    await wait(350);
    cl.close();
    return reply;
}

// N1-09 + N1-10 in one app: an RPC endpoint and an /admin route behind a
// middleware guard.
const app = new App({ port: 0 });
let mwPaths = [];
app.rpc("/r", { add: (a, b) => a + b });
app.use((req) => {
    mwPaths.push(req.path);
    if (req.path.startsWith("/admin")) return { response: { status: 403, body: "denied" } };
});
app.get("/admin/x", () => "SECRET");
app.start();

const rpcCall = async (method) => {
    const body = JSON.stringify({ jsonrpc: "2.0", method, params: [1, 2], id: 1 });
    return raw(app, "POST /r HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: " + body.length + "\r\n\r\n" + body);
};

// 1. prototype members are NOT callable as RPC methods.
for (const m of ["constructor", "toString", "valueOf", "hasOwnProperty", "__proto__", "__defineGetter__"]) {
    const reply = await rpcCall(m);
    ok(reply.includes("-32601"), "rpc method '" + m + "' is not found (got " + JSON.stringify(reply.split("\r\n\r\n")[1] || "") + ")");
}

// 2. real methods still work.
const okReply = await rpcCall("add");
ok(okReply.includes('"result":3'), "rpc add(1,2) still returns 3");

// 3. N1-10: the router collapses '//' but the middleware used to see the raw
//    path -- both must agree now.
mwPaths = [];
const r1 = await raw(app, "GET /admin/x HTTP/1.1\r\nHost: x\r\n\r\n");
ok(r1.includes("denied"), "GET /admin/x is guarded");
const r2 = await raw(app, "GET //admin/x HTTP/1.1\r\nHost: x\r\n\r\n");
ok(r2.includes("denied"), "GET //admin/x is guarded too (was SECRET pre-fix)");
ok(r2.includes("SECRET") === false, "no secret leak through double slashes");
ok(mwPaths[mwPaths.length - 2] === "/admin/x" && mwPaths[mwPaths.length - 1] === "/admin/x",
    "middleware sees the normalized path (got " + JSON.stringify(mwPaths.slice(-2)) + ")");

// 4. mid-path double slashes also normalize and still match.
mwPaths = [];
const r3 = await raw(app, "GET /admin//x HTTP/1.1\r\nHost: x\r\n\r\n");
ok(r3.includes("denied"), "GET /admin//x is guarded");
ok(mwPaths[mwPaths.length - 1] === "/admin/x", "mid-path slashes normalized for middleware");

// 5. ordinary paths are untouched.
{
    const c = new HTTPClient();
    app.get("/plain", () => "ok");
    const res = await c.getAsync("http://127.0.0.1:" + app.port + "/plain");
    eq(res.body, "ok", "plain single-slash routing unaffected");
    c.close();
}

app.close();
if (fails) {
    print("test_audit_http_4: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_audit_http_4 failed");
}
print("test_audit_http_4: " + n + " assertions, 0 failures");
