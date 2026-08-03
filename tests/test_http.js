/* test_http.js — dyna:net (in-repo HTTP/1.1 client + threaded server).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_http.js
 * Prints "test_http: all tests passed" on success; throws on failure. */

import { HTTPClient, HTTPServer } from "dyna:net";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

/* --- bring up a threaded server on an ephemeral port --- */
const server = new HTTPServer({
    port: 0,
    workers: 4,
    routes: {
        "/": "hello world",
        "/json": { status: 200, contentType: "application/json", body: '{"a":1}' },
        "/created": { status: 201, contentType: "text/plain", body: "made" },
        "/empty": { status: 204, contentType: "text/plain", body: "" },
        /* Longer than the 512-byte line buffer the client used to build its
           request in: snprintf truncated it into a well-formed GET for the
           WRONG resource, and reported that only through a return value
           nobody read. 600 chars puts the request line well past it. */
        ["/" + "a".repeat(600)]: "long path ok",
    },
});
server.start();
const port = server.port;
assert(typeof port === "number" && port > 0, "ephemeral port resolved (" + port + ")");
const base = "http://127.0.0.1:" + port;

try {
    /* --- GET a plain-string route --- */
    {
        const c = new HTTPClient();
        try {
            const r = c.get(base + "/");
            assert(r.status === 200, "GET / status 200 (" + r.status + ")");
            assert(r.ok === true, "GET / ok");
            assert(r.body === "hello world", "GET / body (" + r.body + ")");
            const ct = r.headers["Content-Type"] || r.headers["content-type"];
            assert(ct === "text/plain", "GET / content-type (" + ct + ")");
            assert(typeof r.statusText === "string", "statusText is a string");
        } finally { c.close(); }
    }

    /* --- a path that does not fit a fixed request-line buffer --- */
    {
        const c = new HTTPClient();
        try {
            const p = "/" + "a".repeat(600);
            const r = c.get(base + p);
            assert(r.status === 200,
                   "long path is not truncated (status " + r.status + ")");
            assert(r.body === "long path ok", "long path routed correctly");
        } finally { c.close(); }
    }

    /* --- GET a JSON route --- */
    {
        const c = new HTTPClient();
        try {
            const r = c.get(base + "/json");
            assert(r.status === 200, "GET /json status");
            assert(r.body === '{"a":1}', "GET /json body");
            const ct = r.headers["Content-Type"] || r.headers["content-type"];
            assert(ct === "application/json", "GET /json content-type (" + ct + ")");
        } finally { c.close(); }
    }

    /* --- route status honoured (201) --- */
    {
        const c = new HTTPClient();
        try {
            const r = c.get(base + "/created");
            assert(r.status === 201, "GET /created status 201 (" + r.status + ")");
            assert(r.ok === true, "201 is ok");
            assert(r.body === "made", "GET /created body");
        } finally { c.close(); }
    }

    /* --- 404 for an unknown path --- */
    {
        const c = new HTTPClient();
        try {
            const r = c.get(base + "/nope");
            assert(r.status === 404, "unknown path -> 404 (" + r.status + ")");
            assert(r.ok === false, "404 not ok");
        } finally { c.close(); }
    }

    /* --- POST reaches a route (server matches on path) --- */
    {
        const c = new HTTPClient();
        try {
            const r = c.post(base + "/", '{"ping":1}',
                             { "Content-Type": "application/json" });
            assert(r.status === 200, "POST / status 200");
            assert(r.body === "hello world", "POST / body");
        } finally { c.close(); }
    }

    /* --- request() with an explicit method --- */
    {
        const c = new HTTPClient();
        try {
            const r = c.request("PUT", base + "/json");
            assert(r.status === 200, "PUT /json status");
            assert(r.body === '{"a":1}', "PUT /json body");
        } finally { c.close(); }
    }

    /* --- clean structured error for a refused connection --- */
    {
        const c = new HTTPClient();
        c.setTimeout(1000);
        let threw = false, dynajsError = -1;
        try {
            c.get("http://127.0.0.1:9/"); // discard port: connection refused
        } catch (e) {
            threw = true;
            dynajsError = e.dynajsError;
            assert(e instanceof Error, "network failure is an Error");
        } finally { c.close(); }
        assert(threw, "refused connection throws");
        assert(typeof dynajsError === "number", "error carries numeric .dynajsError");
    }

    /* --- bad URL throws --- */
    {
        const c = new HTTPClient();
        let threw = false;
        try { c.get("ftp://127.0.0.1/"); } catch { threw = true; }
        assert(threw, "unsupported scheme throws");
        c.close();
    }

    /* --- client closed-resource semantics --- */
    {
        const c = new HTTPClient();
        assert(c.closed === false, "client open initially");
        c.close();
        assert(c.closed === true, "client closed after close()");
        let threw = false;
        try { c.get(base + "/"); } catch { threw = true; }
        assert(threw, "client use-after-close throws");
        c.close(); // idempotent
    }

    /* --- reentrant-close attack: a coercion that close()s `this` must not
           UAF; the resolve-after-coerce rejects the closed client. --- */
    {
        const c = new HTTPClient();
        let threw = false;
        try {
            c.get({ toString() { c.close(); return base + "/"; } });
        } catch { threw = true; }
        assert(threw, "get() coerce-then-close is caught (no UAF)");
        c.close();
    }
    {
        const c = new HTTPClient();
        let threw = false;
        try {
            c.setTimeout({ valueOf() { c.close(); return 1000; } });
        } catch { threw = true; }
        assert(threw, "setTimeout() coerce-then-close is caught (no UAF)");
        c.close();
    }
    {
        const c = new HTTPClient();
        let threw = false;
        try {
            c.request("GET", { toString() { c.close(); return base + "/"; } });
        } catch { threw = true; }
        assert(threw, "request() coerce-then-close is caught (no UAF)");
        c.close();
    }
} finally {
    server.close(); // stop() (joins all threads) then frees the route table
}

/* --- server closed-resource + idempotent close --- */
assert(server.closed === true, "server closed after close()");
{
    let threw = false;
    try { server.port; } catch { threw = true; }
    assert(threw, "server .port after close throws");
}
server.close();   // idempotent
{
    let threw = false;
    try { server.stop(); } catch { threw = true; } // resolves native -> rejects closed
    assert(threw, "server.stop() after close throws (consistent use-after-close)");
}

/* --- a second server can bind and serve after the first is gone --- */
{
    const s2 = new HTTPServer({ routes: { "/ping": "pong" } });
    s2.start();
    const c = new HTTPClient();
    try {
        const r = c.get("http://127.0.0.1:" + s2.port + "/ping");
        assert(r.body === "pong", "second server serves (" + r.body + ")");
    } finally { c.close(); s2.close(); }
}

print("test_http: all tests passed (" + n + " assertions)");
