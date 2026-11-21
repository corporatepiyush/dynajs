// timeout: 120
// covers: dyna:http.HTTPClient
import { sections as __kitSections } from "../lib/kit.js";
import { HTTPServer, HTTPClient } from "dyna:http";
import { TCPServer } from "dyna:net";
const __kitS = __kitSections("test_http_http_client_onconnect_async", { chain: true });
let srv, base;

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}

await __kitS.section("fixture: start the HTTP server", async () => {
srv = new HTTPServer({ port: 0, routes: { "/ok": "fine" } });
srv.start();
base = `http://127.0.0.1:${srv.port}/ok`;
});

async function main() {
    {
let c;
        let saw = "";
        let refused = false;
await __kitS.section("getAsync is refused by a false onConnect", async () => {
        c = new HTTPClient();
        c.onConnect = (ip) => { saw = ip; return false; };
        try { await c.get(base); } catch (e) { refused = /connect/i.test(String(e)); }
        ok(refused, "getAsync is refused by a false onConnect");
        ok(saw === "127.0.0.1", "getAsync hook received the resolved IP (got " + saw + ")");
        c.close();
});
    }
    {
let c, r;
        let calls = 0;
await __kitS.section("postAsync proceeds when allowed", async () => {
        c = new HTTPClient();
        c.onConnect = () => { calls++; return true; };
        r = await c.post(base, "x");
        ok(r.status === 200 && calls === 1, "postAsync proceeds when allowed (calls=" + calls + ")");
        c.close();
});
    }
    {
let c;
        let refused = false;
await __kitS.section("requestAsync refuses when the hook throws", async () => {
        c = new HTTPClient();
        c.onConnect = () => { throw new Error("policy"); };
        try { await c.request("GET", base); } catch (e) { refused = /connect/i.test(String(e)); }
        ok(refused, "requestAsync refuses when the hook throws");
        c.close();
});
    }
    {
let c;
        let refused = false;
await __kitS.section("getStream is refused by onConnect", async () => {
        c = new HTTPClient();
        c.onConnect = () => false;
        try { await c.getStream(base); } catch (e) { refused = /connect/i.test(String(e)); }
        ok(refused, "getStream is refused by onConnect");
        c.close();
});
    }
    {
        let msg = "";
        let refused = false;
await __kitS.section("fetch() honours init.onConnect", async () => {
        try { await fetch(base, { onConnect: () => false }); }
        catch (e) { msg = String(e); refused = /connection failed/i.test(msg); }
        ok(refused, "fetch() honours init.onConnect (msg=" + msg.slice(0, 60) + ")");
});
    }
await __kitS.section("fetch() proceeds when allowed", async () => {
    {
        const r = await fetch(base, { onConnect: () => true });
        ok(r.status === 200 && (await r.text()) === "fine", "fetch() proceeds when allowed");
    }
});
    {
let hang, t0, dt, c, t1, dt2;
        let timedOut = false;
await __kitS.section("fixture: start a TCP server that never answers", async () => {
        hang = new TCPServer({ port: 0 });
        hang.start({ connect() {}, data() {} });
        t0 = Date.now();
        try { await fetch(`http://127.0.0.1:${hang.port}/slow`, { timeout: 200 }); }
        catch (e) { timedOut = true; }
        dt = Date.now() - t0;
});
await __kitS.section("fetch timeout is honoured", async () => {
        // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
        ok(timedOut && dt < 10000, `fetch timeout is honoured (dt=${dt}ms, default would be ~15s)`);
});
        let cTimedOut = false;
await __kitS.section("fixture: create the HTTP client", async () => {
        c = new HTTPClient();
        c.setTimeout(200);
        t1 = Date.now();
        try { await c.get(`http://127.0.0.1:${hang.port}/slow`); } catch (e) { cTimedOut = true; }
        dt2 = Date.now() - t1;
});
await __kitS.section("HTTPClient setTimeout still bounds a slow call", async () => {
        // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
        ok(cTimedOut && dt2 < 10000, `HTTPClient setTimeout still bounds a slow call (dt=${dt2}ms)`);
        c.close();
        hang.close();
});
    }
    {
let c;
        let resolved = false;
await __kitS.section("getAsync survives c.close() inside onConnect (no crash, promise settles)", async () => {
        c = new HTTPClient();
        c.onConnect = () => { c.close(); return true; };
        try { const r = await c.get(base); resolved = !!(r && r.status === 200); }
        catch (e) { resolved = false; }
        ok(resolved, "getAsync survives c.close() inside onConnect (no crash, promise settles)");
});
    }
    {
let c;
        let settled = false;
await __kitS.section("getStream survives c.close() inside onConnect like get() (one reactor path since T1)", async () => {
        c = new HTTPClient();
        c.onConnect = () => { c.close(); return true; };
        try { const s = await c.getStream(base); const b = new Uint8Array(4096); let n = 0, k; while ((k = await s.read(b)) > 0) n += k; s.close(); settled = s.status === 200 && n > 0; }
        catch (e) { settled = false; }
        ok(settled, "getStream survives c.close() inside onConnect and reads its body to the end");
});
    }
    {
let c, r;
await __kitS.section("the same client shape still works when the hook does not close", async () => {
        c = new HTTPClient();
        c.onConnect = () => true;
        r = await c.get(base);
        ok(r.status === 200, "the same client shape still works when the hook does not close");
        c.close();
});
    }

await __kitS.section("teardown: close the HTTP server", async () => {
    srv.close();
    console.log("test_http_onconnect_async: " + (n - failed) + " passed, " + failed + " failed");
    if (failed)
        throw new Error("test_http_onconnect_async: " + failed + " failures");
});
}
await main().catch((e) => {
    throw new Error("test_http_onconnect_async: harness error " + e);
});
__kitS.end();
