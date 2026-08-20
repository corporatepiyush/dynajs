/* test_http.js — dyna:net (in-repo HTTP/1.1 client + threaded server).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_http.js
 * Prints "test_http: all tests passed" on success; throws on failure. */

import { HTTPClient, HTTPServer } from "dyna:net";
import * as std from "std";
import { mkdir, open, read, close, kill, O_RDONLY } from "os";
import { cwd } from "dyna:sys";

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

    /* --- a byte VIEW body is sent raw, framed by its real length --- */
    {
        /* The sync client blocks the loop thread, so its peer must be a real
           process (a spawned TCPServer would starve on the loop it shares).
           A background dynajs captures the exact wire bytes to a file; the
           sync request then posts a byte view and the capture is asserted. */
        const dir = "/tmp/sync_wire_" + Date.now() + "_" + Math.floor(Math.random() * 1e9);
        const peer = dir + "/peer.js";
        const portFile = dir + "/port";
        const pidFile = dir + "/pid";
        const capFile = dir + "/cap.bin";
        const debugFile = dir + "/debug.txt";
        mkdir(dir, 0o755);
        const f = std.open(peer, "w");
        f.puts(`import { TCPServer } from "dyna:net";
import * as std from "std";
import { pid } from "dyna:sys";
const srv = new TCPServer({ port: 0 });
let acc = [];
srv.start({
    data: (c, bytes) => {
        acc = acc.concat(Array.from(bytes));
        let end = -1;
        let i = 0;
        while (i < acc.length - 3) {
            if (acc[i] === 13 && acc[i+1] === 10 && acc[i+2] === 13 && acc[i+3] === 10) {
                end = i + 4; break;
            }
            i++;
        }
        if (end < 0) return;
        let head = "";
        for (let j = 0; j < end; j++) head += String.fromCharCode(acc[j]);
        const m = /Content-Length:\\s*(\\d+)/i.exec(head);
        if (!m) return;
        const need = end + parseInt(m[1], 10);
        if (acc.length < need) return;
        const body = acc.slice(end, need);
        const ab = new Uint8Array(body).buffer;
        const f = std.open(${JSON.stringify(capFile)}, "w");
        const wr = f.write(ab, 0, ab.byteLength);
        const ferr = f.error();
        f.close();
        const fd = std.open(${JSON.stringify(debugFile)}, "a");
        fd.puts("wr=" + wr + " err=" + ferr + " bl=" + ab.byteLength + "\\n");
        fd.close();
        c.write("HTTP/1.1 200 OK\\r\\nContent-Length: 0\\r\\nConnection: close\\r\\n\\r\\n");
        c.close();
    },
});
const f = std.open(${JSON.stringify(portFile)}, "w");
f.puts(String(srv.port)); f.close();
const pf = std.open(${JSON.stringify(pidFile)}, "w");
pf.puts(String(pid())); pf.close();
setInterval(() => {}, 1000);
`);
        f.close();
        std.gc();
        const p = std.popen("cd " + dir + " && " + cwd() + "/dynajs peer.js > " + dir + "/peer.log 2>&1 &", "r");
        if (p) p.close();
        let port = null;
        for (let t = 0; t < 200 && port === null; t++) {
            try {
                const pf = std.open(portFile, "r");
                const s = pf.getline(); pf.close();
                if (s) port = parseInt(s.trim(), 10);
            } catch (e) { }
            if (port === null) { const d = new Date(Date.now() + 25); while (new Date() < d) {} }
        }
        assert(port !== null && port > 0, "wire-capture peer is up (" + port + ")");
        const c = new HTTPClient();
        try {
            const r = c.request("POST", "http://127.0.0.1:" + port + "/raw",
                                new Uint8Array([0x00, 0x80, 0x41, 0xff]));
            assert(r.status === 200, "sync byte-view body reaches a route (" + r.status + ")");
        } finally { c.close(); }
        let cap = null;
        for (let t = 0; t < 200 && cap === null; t++) {
            try {
                const fd = open(capFile, O_RDONLY);
                const buf = new ArrayBuffer(65536);
                const n = read(fd, buf, 0, 65536);
                close(fd);
                if (n > 0) cap = new Uint8Array(buf, 0, n);
            } catch (e) { }
            if (cap === null) { const d = new Date(Date.now() + 25); while (new Date() < d) {} }
        }
        assert(cap !== null, "wire capture was written");
        const all = Array.from(cap);
        assert(JSON.stringify(all) === JSON.stringify([0, 128, 65, 255]),
               "sync byte body is raw on the wire, never comma-joined (" + JSON.stringify(all) + ")");
        let killed = false;
        try {
            const pf = std.open(pidFile, "r");
            const s = pf.getline(); pf.close();
            if (s) { kill(parseInt(s.trim(), 10), 15); killed = true; }
        } catch (e) { }
        assert(killed, "wire-capture peer was reaped");
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
