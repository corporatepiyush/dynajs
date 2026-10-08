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

    const p = c.post(base + "/", "x".repeat(4096), { "Content-Type": "text/plain" });
    assert(p.status === 200, "POST with body still served");
    assert(p.body === "hello world\n", "POST body response");
} finally {
    c.close();
    server.close();
}
print("test_http_async: all tests passed");
