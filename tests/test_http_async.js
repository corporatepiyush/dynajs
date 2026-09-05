/* test_http_async.js — exercise HTTPServerAsync (the single-thread reactor,
 * Model A) end-to-end using the in-process HTTPClient. Backend-agnostic: proves
 * kqueue (macOS), epoll (Linux), or io_uring (Linux, CONFIG_IO_URING) all serve
 * correctly. The io_uring path in particular fails here if the ring cannot be
 * created (e.g. seccomp-blocked) or the poll/complete state machine is wrong. */
import { HTTPServerAsync, HTTPClient } from "dyna:net";

function assert(cond, msg) { if (!cond) throw new Error("assertion failed: " + msg); }

const server = new HTTPServerAsync({ port: 0, routes: {
    "/":     "hello world\n",
    "/json": { status: 200, contentType: "application/json", body: '{"a":1}' },
}});
server.start();
const base = "http://127.0.0.1:" + server.port;
const c = new HTTPClient();
try {
    /* many sequential requests to shake out per-request reactor bugs */
    for (let i = 0; i < 500; i++) {
        const r = c.get(base + "/");
        assert(r.status === 200, "GET / status 200");
        assert(r.body === "hello world\n", "GET / body");
    }
    const j = c.get(base + "/json");
    assert(j.status === 200, "GET /json status");
    assert(j.headers["Content-Type"] === "application/json", "json content-type");
    assert(j.body === '{"a":1}', "GET /json body");

    const nf = c.get(base + "/nope");
    assert(nf.status === 404, "unknown route -> 404");

    /* a POST with a body must be framed/consumed even though routes ignore it */
    const p = c.post(base + "/", "x".repeat(4096), { "Content-Type": "text/plain" });
    assert(p.status === 200, "POST with body still served");
    assert(p.body === "hello world\n", "POST body response");
} finally {
    c.close();
    server.close();
}
print("test_http_async: all tests passed");
