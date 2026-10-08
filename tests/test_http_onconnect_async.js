// flags: --std
// timeout: 120
import "./httpc.js";
import { HTTPServer, HTTPClient } from "dyna:http";
import { TCPServer } from "dyna:net";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}

const srv = new HTTPServer({ port: 0, routes: { "/ok": "fine" } });
srv.start();
const base = `http://127.0.0.1:${srv.port}/ok`;

async function main() {
    {
        const c = new HTTPClient();
        let saw = "";
        c.onConnect = (ip) => { saw = ip; return false; };
        let refused = false;
        try { await c.getAsync(base); } catch (e) { refused = /connect/i.test(String(e)); }
        ok(refused, "getAsync is refused by a false onConnect");
        ok(saw === "127.0.0.1", "getAsync hook received the resolved IP (got " + saw + ")");
        c.close();
    }
    {
        const c = new HTTPClient();
        let calls = 0;
        c.onConnect = () => { calls++; return true; };
        const r = await c.postAsync(base, "x");
        ok(r.status === 200 && calls === 1, "postAsync proceeds when allowed (calls=" + calls + ")");
        c.close();
    }
    {
        const c = new HTTPClient();
        c.onConnect = () => { throw new Error("policy"); };
        let refused = false;
        try { await c.requestAsync("GET", base); } catch (e) { refused = /connect/i.test(String(e)); }
        ok(refused, "requestAsync refuses when the hook throws");
        c.close();
    }
    {
        const c = new HTTPClient();
        c.onConnect = () => false;
        let refused = false;
        try { c.getStream(base); } catch (e) { refused = /connect/i.test(String(e)); }
        ok(refused, "getStream is refused by onConnect");
        c.close();
    }
    {
        let msg = "";
        let refused = false;
        try { await fetch(base, { onConnect: () => false }); }
        catch (e) { msg = String(e); refused = /connection failed/i.test(msg); }
        ok(refused, "fetch() honours init.onConnect (msg=" + msg.slice(0, 60) + ")");
    }
    {
        const r = await fetch(base, { onConnect: () => true });
        ok(r.status === 200 && (await r.text()) === "fine", "fetch() proceeds when allowed");
    }
    {
        const hang = new TCPServer({ port: 0 });
        hang.start({ connect() {}, data() {} });
        const t0 = Date.now();
        let timedOut = false;
        try { await fetch(`http://127.0.0.1:${hang.port}/slow`, { timeout: 200 }); }
        catch (e) { timedOut = true; }
        const dt = Date.now() - t0;
        ok(timedOut && dt < 5000, `fetch timeout is honoured (dt=${dt}ms, default would be ~15s)`);
        const c = new HTTPClient();
        c.setTimeout(200);
        const t1 = Date.now();
        let cTimedOut = false;
        try { c.get(`http://127.0.0.1:${hang.port}/slow`); } catch (e) { cTimedOut = true; }
        const dt2 = Date.now() - t1;
        ok(cTimedOut && dt2 < 5000, `HTTPClient setTimeout still bounds a slow call (dt=${dt2}ms)`);
        c.close();
        hang.close();
    }
    {
        const c = new HTTPClient();
        c.onConnect = () => { c.close(); return true; };
        let resolved = false;
        try { const r = await c.getAsync(base); resolved = !!(r && r.status === 200); }
        catch (e) { resolved = false; }
        ok(resolved, "getAsync survives c.close() inside onConnect (no crash, promise settles)");
    }
    {
        const c = new HTTPClient();
        c.onConnect = () => { c.close(); return true; };
        let rejected = false;
        try { const s = c.getStream(base); try { s.close(); } catch (e) {} }
        catch (e) { rejected = /closed/i.test(String(e)); }
        ok(rejected, "getStream rejects cleanly when onConnect closes the client");
    }
    {
        const c = new HTTPClient();
        c.onConnect = () => true;
        const r = await c.getAsync(base);
        ok(r.status === 200, "the same client shape still works when the hook does not close");
        c.close();
    }

    srv.close();
    console.log("test_http_onconnect_async: " + (n - failed) + " passed, " + failed + " failed");
    if (failed)
        throw new Error("test_http_onconnect_async: " + failed + " failures");
}
main().catch((e) => {
    throw new Error("test_http_onconnect_async: harness error " + e);
});
