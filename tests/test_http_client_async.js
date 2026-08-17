/* test_http_client_async.js -- getAsync/postAsync/requestAsync on HTTPClient.
 *
 * THE POINT OF THIS API IS THE POINT OF THIS FILE: the sync methods block
 * the loop thread for the whole round trip, so in a single-threaded runtime
 * one outbound call freezes every connection the process is serving. The
 * async methods offload the blocking exchange to the io pool and settle a
 * Promise. The load-bearing property is therefore NOT "the value is right"
 * (the sync tests already pin that) -- it is that the LOOP STAYS LIVE while
 * a request is in flight. Every other test in this file would pass against
 * a client that secretly blocks.
 *
 * In-process App + async client works because the exchange runs on a pool
 * worker while the loop thread keeps draining the reactor. Verified by
 * injection: with --io-threads=0 the pool is absent and the job runs
 * inline, which the run below with the flag proves still settles (blocking,
 * but correct) -- the flag, not the API, decides.
 */
import { HTTPClient, App, TCPServer } from "dyna:net";
import * as std from "std";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
function threw(fn, re, m) {
    try { fn(); fail++; print("  FAIL: " + m + ": did not throw"); }
    catch (e) { ok(re ? re.test(e.message) : true, m + " -- " + e.message); }
}
async function rejects(p, re, m) {
    try { await p; fail++; print("  FAIL: " + m + ": resolved instead of rejecting"); }
    catch (e) { ok(re ? re.test(e.message) : true, m + " -- " + e.message); }
}

/* The App under test: an rpc route with sync AND async handlers, so both
   immediate and delayed responses run through the settle path. */
const app = new App({ port: 0, idleTimeoutMs: 5000 });
app.rpc("/rpc", {
    add: ([a, b]) => a + b,
    echo: ([x]) => x,
    slow: () => new Promise((res) => setTimeout(() => res("done"), 300)),
});
app.start();
const base = "http://127.0.0.1:" + app.port;
const rpc = (method, params, id) => JSON.stringify({ jsonrpc: "2.0", method, params, id });

/* ---- the response VALUE matches the sync path ---- */
{
    const c = new HTTPClient();
    try {
        const r = await c.getAsync(base + "/no-such-route");
        eq(r.status, 404, "an unknown path is a 404, not a hang");
        const j = await c.postAsync(base + "/rpc", rpc("add", [2, 3], 1),
                                    { "Content-Type": "application/json" });
        eq(j.status, 200, "postAsync status");
        eq(j.body, '{"jsonrpc":"2.0","result":5,"id":1}', "postAsync rpc result");
        eq(j.ok, true, "postAsync ok flag");
        eq(typeof j.statusText, "string", "statusText is a string");
        const put = await c.requestAsync("PUT", base + "/rpc", rpc("echo", ["x"], 7));
        eq(put.status, 200, "requestAsync with an explicit method");
        ok(put.body.indexOf('"result":"x"') >= 0, "requestAsync body (" + put.body + ")");
        const getrpc = await c.requestAsync("POST", base + "/rpc",
                                            rpc("echo", [1, 2], 2));
        eq(getrpc.body, '{"jsonrpc":"2.0","result":1,"id":2}', "requestAsync post");
    } finally { c.close(); }
}

/* ---- a Promise, not a maybe: the object is thenable and instanceof ---- */
{
    const c = new HTTPClient();
    const p = c.getAsync(base + "/no-such-route");
    ok(p instanceof Promise, "getAsync returns a Promise");
    ok(typeof p.then === "function", "and it is thenable");
    const r = await p;
    eq(r.status, 404, "a 404 is an HTTP status, not a rejection");
    c.close();
}

/* ---- THE LOAD-BEARING CASE: the loop stays live while a request is in
        flight. The handler takes 300ms; a 50ms timer must fire while the
        request is still pending. A blocking client fails this. ---- */
{
    const c = new HTTPClient();
    let pendingAtTimer = null;
    const p = c.postAsync(base + "/rpc", rpc("slow", [], 9),
                          { "Content-Type": "application/json" });
    const timerFired = new Promise((res) => {
        setTimeout(() => { pendingAtTimer = true; res(); }, 50);
    });
    await timerFired;
    ok(pendingAtTimer === true, "a timer fired while the request was pending");
    const r = await p;
    ok(r.body.indexOf('"result":"done"') >= 0, "the delayed rpc resolves (" + r.body + ")");
    c.close();
}

/* ---- N concurrent requests each get THEIR OWN result (no cross-linking
        of settle closures), and the loop serves other work meanwhile ---- */
{
    const c = new HTTPClient();
    const N = 24;
    const ps = [];
    for (let i = 0; i < N; i++)
        ps.push(c.postAsync(base + "/rpc", rpc("echo", [i], i),
                            { "Content-Type": "application/json" }));
    const rs = await Promise.all(ps);
    for (let i = 0; i < N; i++) {
        const want = '"result":' + i + ',"id":' + i;
        ok(rs[i].status === 200 && rs[i].body.indexOf(want) >= 0,
           "request #" + i + " got its own result (" + rs[i].body + ")");
    }
    c.close();
}

/* ---- a rejection is an Error with the structured dynajsError code ---- */
{
    const c = new HTTPClient();
    c.setTimeout(1500);
    let seen;
    try {
        await c.getAsync("http://127.0.0.1:1/");   /* nothing listens on 1 */
    } catch (e) {
        seen = e;
    }
    ok(seen instanceof Error, "a refused connection rejects with an Error");
    ok(typeof seen.dynajsError === "number", "carrying a numeric .dynajsError (" + seen.dynajsError + ")");
    ok(/GET http:\/\/127\.0\.0\.1:1\/ failed/.test(seen.message),
       "the message names the method and url (" + seen.message + ")");
    c.close();
}

/* ---- a bad URL THROWS synchronously, exactly like the sync methods ---- */
{
    const c = new HTTPClient();
    threw(() => c.getAsync("ftp://x/"), /URL/, "getAsync with an unsupported scheme throws");
    threw(() => c.getAsync("not a url"), /URL/, "getAsync with garbage throws");
    threw(() => c.requestAsync("GET"), /requestAsync/, "requestAsync arity is checked");
    c.close();
}

/* ---- a silent peer + a client timeout rejects on the worker ---- */
{
    const srv = new TCPServer({ port: 0 });
    srv.start({ connect: () => {}, data: () => {}, close: () => {} });
    const c = new HTTPClient();
    c.setTimeout(300);
    const t0 = Date.now();
    let seen;
    try { await c.getAsync("http://127.0.0.1:" + srv.port + "/"); }
    catch (e) { seen = e; }
    ok(seen instanceof Error, "a silent peer rejects (" + (seen && seen.message) + ")");
    ok(Date.now() - t0 < 5000, "the timeout bound the wait, not the default");
    c.close();
    srv.close();
}

/* ---- a garbage response rejects; a valid one does not ----
   ("NOT HTTP AT ALL\r\n\r\n" would PARSE: the lenient builder reads any
   leading space as a status separator and reports status 0 -- which is
   worth knowing, but it is not an error. No CRLF at all is.) */
{
    const srv = new TCPServer({ port: 0 });
    srv.start({
        connect: () => {},
        data: (c, b) => {
            c.write("GARBAGE");
            c.close();
        },
        close: () => {},
    });
    const c = new HTTPClient();
    let seen;
    try { await c.getAsync("http://127.0.0.1:" + srv.port + "/"); }
    catch (e) { seen = e; }
    ok(seen instanceof Error, "a malformed response rejects (" + (seen && seen.message) + ")");
    c.close();
    srv.close();
}

/* ---- the request WIRE BYTES: headers, method, body (incl. NULs) are what
        the peer receives -- a proxy or an auth gateway reads these ---- */
{
    const received = [];
    const srv = new TCPServer({ port: 0 });
    srv.start({
        connect: () => {},
        data: (c, b) => {
            received.push(new TextDecoder().decode(b));
            if (received.join("").indexOf("\r\n\r\n") >= 0) {
                c.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                        + "Connection: close\r\n\r\nok");
                c.close();
            }
        },
        close: () => {},
    });
    const c = new HTTPClient();
    const r = await c.postAsync("http://127.0.0.1:" + srv.port + "/x?q=1",
                                "a\0b\0c", { "X-Trace": "t1" });
    eq(r.status, 200, "the peer's answer is parsed");
    eq(r.body, "ok", "and its body");
    const wire = received.join("");
    ok(wire.startsWith("POST /x?q=1 HTTP/1.1\r\n"), "method, path and query on the wire");
    ok(wire.indexOf("X-Trace: t1\r\n") >= 0, "custom headers reach the wire");
    ok(wire.indexOf("\r\n\r\na\0b\0c") >= 0, "a body with embedded NULs is sent whole");
    ok(wire.indexOf("Content-Length: 5\r\n") >= 0, "and its length counts the NULs");
    c.close();
    srv.close();
}

/* ---- use-after-close and coerce-then-close are refused, no UAF ---- */
{
    const c = new HTTPClient();
    c.close();
    threw(() => c.getAsync(base + "/rpc"), /closed/, "getAsync on a closed client throws");
    c.close();
}
{
    const c = new HTTPClient();
    threw(() => c.getAsync({ toString() { c.close(); return base + "/rpc"; } }),
          /closed/, "a coercion that closes the client is caught");
    c.close();
}
{
    const c = new HTTPClient();
    threw(() => c.postAsync(base + "/rpc", { toString() { c.close(); return "x"; } }),
          /closed/, "postAsync coerce-then-close is caught");
    c.close();
}

/* ---- a GET with headers (no body) works, and a POST without one too ---- */
{
    const c = new HTTPClient();
    const j = await c.getAsync(base + "/no-such-route", { "X-Ping": "1" });
    eq(j.status, 404, "getAsync(url, headers) still routes (404 here)");
    const p = await c.postAsync(base + "/rpc", rpc("add", [1, 2], 3));
    eq(p.status, 200, "postAsync without explicit headers works");
    c.close();
}

app.close();
ok(app.closed === true, "the App closed cleanly after all in-flight work");

print("test_http_client_async: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
std.exit(0);
