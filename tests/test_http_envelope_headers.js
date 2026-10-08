// App route envelopes carry response headers, exact and dynamic routes win
// over a prefix mount, a refused upload stays readable, and fetch exposes
// redirect: "follow" | "manual" | "error".
import { suite } from "./kit.js";
import { App, TCPServer } from "dyna:net";
import { makeTempDir, writeFile, removeAll } from "dyna:file";

const t = suite("http_envelope_headers");
const root = makeTempDir("envroot");
const updir = makeTempDir("envup");
writeFile(root.join("index.txt"), "static-file");
writeFile(root.join("_status"), "static-status");
writeFile(root.join("postonly"), "static-postonly");

const app = new App({ port: 0 });
app.static("/", root, {});
app.get("/_status", () => ({ ok: true }));
app.post("/postonly", () => "posted");
app.rpc("/rpc", { ping: () => "pong" });
app.upload("/up", { dir: updir, maxFileSize: 1024 * 1024, allow: ["application/octet-stream"] }, () => {});
app.get("/redir", () => ({ status: 302, headers: { Location: "/landed" } }));
app.get("/landed", () => "landed");
app.get("/cookies", () => ({ body: "ok", headers: { "Set-Cookie": ["a=1; Path=/", "b=2; HttpOnly"], "Cache-Control": "no-store" } }));
app.get("/big", () => ({ body: "ok", headers: { "X-Big": "v".repeat(4000) } }));
app.get("/nobody", () => ({ status: 204, headers: { "X-Trace": "t1" } }));
app.get("/json", () => ({ status: 201, body: { a: 1 }, headers: { "X-Kind": "json" } }));
app.get("/bytes", () => ({ body: new Uint8Array([1, 2, 3]), headers: { "X-Kind": "bytes" } }));
const BAD = {
    length: { status: 200, headers: { "Content-Length": "5" } },
    te: { status: 200, headers: { "transfer-encoding": "chunked" } },
    conn: { status: 200, headers: { Connection: "close" } },
    enc: { status: 200, headers: { "Content-Encoding": "gzip" } },
    ctype: { status: 200, headers: { "Content-Type": "text/html" } },
    crlf: { status: 200, headers: { "X-A": "a\r\nInjected: 1" } },
    nul: { status: 200, headers: { "X-A": "a\u0000b" } },
    name: { status: 200, headers: { "Bad Name": "x" } },
    colon: { status: 200, headers: { "X:Y": "x" } },
    number: { status: 200, headers: { "X-N": 5 } },
    nested: { status: 200, headers: { "X-N": ["a", 5] } },
    array: { status: 200, headers: ["a", "b"] },
    string: { status: 200, headers: "X-A: 1" },
    toobig: { status: 200, headers: { "X-Big": "v".repeat(9000) } },
    unknown: { status: 200, bogus: 1 },
    bodyplus: { body: "x", title: "y" },
};
for (const k of Object.keys(BAD)) app.get("/bad/" + k, () => BAD[k]);
app.start();
const B = "http://127.0.0.1:" + app.port;
t.after(() => { app.close(); removeAll(root); removeAll(updir); });

// settleMs: hang up that long after the first reply bytes instead of waiting
// for the server to close (a lingering server waits for the client).
function raw(request, settleMs) {
    return new Promise((resolve, reject) => {
        let got = "", settle = null;
        const timer = setTimeout(() => { h.close(); reject(new Error("raw: no reply; got " + JSON.stringify(got))); }, 4000);
        const h = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
            connect: (conn, err) => { if (err) { clearTimeout(timer); reject(new Error(err)); } else conn.write(request); },
            data: (conn, b) => {
                got += String.fromCharCode.apply(null, b);
                if (settleMs && !settle) settle = setTimeout(() => { clearTimeout(timer); h.close(); resolve(got); }, settleMs);
            },
            close: () => { clearTimeout(timer); clearTimeout(settle); resolve(got); },
        });
    });
}
const get = (path) => raw("GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

t.test("a 302 envelope sends Location", async (a) => {
    const r = await fetch(B + "/redir", { redirect: "manual" });
    a.eq(r.status, 302);
    a.eq(r.headers.get("location"), "/landed");
});

t.test("an array value becomes one header line per element", async (a) => {
    const text = await get("/cookies");
    a.match(text, /^HTTP\/1\.1 200 /);
    a.eq(text.split("\r\n").filter((l) => /^set-cookie:/i.test(l)).length, 2);
    a.ok(text.includes("\r\nSet-Cookie: a=1; Path=/\r\n"));
    a.ok(text.includes("\r\nSet-Cookie: b=2; HttpOnly\r\n"));
    a.ok(text.includes("\r\nCache-Control: no-store\r\n"));
    a.ok(text.endsWith("\r\n\r\nok"));
});

t.test("a header larger than the fixed head buffer is sent whole", async (a) => {
    const r = await fetch(B + "/big");
    a.eq(r.status, 200);
    a.eq(r.headers.get("x-big").length, 4000);
    a.eq(await r.text(), "ok");
});

t.test("headers ride on bodyless, JSON and byte bodies", async (a) => {
    const r1 = await fetch(B + "/nobody");
    a.eq(r1.status, 204);
    a.eq(r1.headers.get("x-trace"), "t1");
    const r2 = await fetch(B + "/json");
    a.eq(r2.status, 201);
    a.eq(r2.headers.get("x-kind"), "json");
    a.deep(await r2.json(), { a: 1 });
    const r3 = await fetch(B + "/bytes");
    a.eq(r3.headers.get("x-kind"), "bytes");
    a.deep(Array.from(await r3.bytes()), [1, 2, 3]);
});

const WHY = {
    length: /belong to the server/, te: /belong to the server/, conn: /belong to the server/, enc: /belong to the server/,
    ctype: /contentType/, crlf: /control characters/, nul: /control characters/, name: /HTTP tokens/, colon: /HTTP tokens/,
    number: /strings or arrays of strings/, nested: /strings or arrays of strings/, array: /must be an object/,
    string: /must be an object/, toobig: /too large/, unknown: /unknown key/, bodyplus: /unknown key/,
};
for (const k of Object.keys(BAD)) {
    t.test("refused envelope: " + k, async (a) => {
        const text = await get("/bad/" + k);
        a.match(text, /^HTTP\/1\.1 500 /);
        a.match(text.slice(text.indexOf("\r\n\r\n")), WHY[k], "the 500 names its reason");
        a.ok(!/\r\nInjected: 1/.test(text), "no injected header line");
    });
}

t.test("a dynamic route wins over static('/') registered first", async (a) => {
    const r = await fetch(B + "/_status");
    a.deep(await r.json(), { ok: true });
});

t.test("the static mount still serves what no route claims", async (a) => {
    a.eq(await (await fetch(B + "/index.txt")).text(), "static-file");
    a.eq((await fetch(B + "/missing.txt")).status, 404);
});

t.test("a method the dynamic route does not take falls through to the mount", async (a) => {
    a.eq(await (await fetch(B + "/postonly")).text(), "static-postonly");
    a.eq(await (await fetch(B + "/postonly", { method: "POST", body: "x" })).text(), "posted");
});

t.test("exact rpc and upload routes win over the mount", async (a) => {
    const r = await fetch(B + "/rpc", { method: "POST", headers: { "content-type": "application/json" },
        body: JSON.stringify({ jsonrpc: "2.0", method: "ping", id: 1 }) });
    a.eq((await r.json()).result, "pong");
    const u = await fetch(B + "/up", { method: "POST", headers: { "content-type": "application/octet-stream" }, body: new Uint8Array(1000) });
    a.eq(u.status, 200);
});

for (const n of [1024 * 1024 + 1, 3 * 1024 * 1024, 24 * 1024 * 1024]) {
    t.test("an upload of " + n + " bytes over the cap reads back as 413", async (a) => {
        const r = await fetch(B + "/up", { method: "POST", headers: { "content-type": "application/octet-stream" }, body: new Uint8Array(n) });
        a.eq(r.status, 413);
        a.eq(r.headers.get("connection"), "close");
        a.deep(await r.json(), { error: "too large" });
    }, { timeoutMs: 20000 });
}

t.test("a refused media type with a large body reads back as 415", async (a) => {
    const r = await fetch(B + "/up", { method: "POST", headers: { "content-type": "text/weird" }, body: new Uint8Array(900 * 1024) });
    a.eq(r.status, 415);
});

t.test("the server keeps serving after refusals", async (a) => {
    a.eq(await (await fetch(B + "/landed")).text(), "landed");
});

t.test("a lingering connection never runs a smuggled second request", async (a) => {
    const text = await raw("POST /up HTTP/1.1\r\nHost: x\r\nContent-Type: application/octet-stream\r\nContent-Length: 2000000\r\n\r\n"
        + "GET /landed HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    a.eq((text.match(/HTTP\/1\.1 /g) || []).length, 1, "exactly one response");
    a.match(text, /^HTTP\/1\.1 413 /);
    a.ok(!text.includes("landed"));
}, { timeoutMs: 15000 });

t.test("fetch redirect: follow is the default", async (a) => {
    const r = await fetch(B + "/redir");
    a.eq(r.status, 200);
    a.eq(await r.text(), "landed");
    a.eq(new Request(B + "/redir").redirect, "follow");
});

t.test("fetch redirect: manual returns the 3xx itself", async (a) => {
    const r = await fetch(new Request(B + "/redir", { redirect: "manual" }));
    a.eq(r.status, 302);
});

t.test("fetch redirect: error rejects", async (a) => {
    await a.rejects(fetch(B + "/redir", { redirect: "error" }), { type: TypeError, message: /refused by redirect/ });
    a.eq((await fetch(B + "/landed", { redirect: "error" })).status, 200, "a non-redirect is unaffected");
});

t.test("fetch redirect: an unknown mode is refused", async (a) => {
    a.throws(() => new Request(B + "/redir", { redirect: "sideways" }), TypeError);
    await a.rejects(async () => fetch(B + "/redir", { redirect: "sideways" }), TypeError);
});

// With the idle timeout off, a refused upload whose body never comes is still
// dropped by the linger deadline (5 s): the close handler is the evidence.
t.test("a lingering refusal is closed on a deadline even with idleTimeoutMs 0", async (a) => {
    const quiet = new App({ port: 0, idleTimeoutMs: 0 });
    quiet.upload("/up", { dir: updir, maxFileSize: 1024, allow: ["application/octet-stream"] }, () => {});
    quiet.start();
    const got = await new Promise((resolve) => {
        let text = "";
        const timer = setTimeout(() => { h.close(); resolve("STILL OPEN after 9 s: " + text.slice(0, 40)); }, 9000);
        const h = TCPServer.connect({ host: "127.0.0.1", port: quiet.port }, {
            connect: (conn) => conn.write("POST /up HTTP/1.1\r\nHost: x\r\nContent-Type: application/octet-stream\r\nContent-Length: 2000000\r\n\r\n"),
            data: (conn, b) => { text += String.fromCharCode.apply(null, b); },
            close: () => { clearTimeout(timer); resolve(text); },
        });
    });
    quiet.close();
    a.match(got, /^HTTP\/1\.1 413 /, "refused, then closed by the server");
}, { timeoutMs: 15000 });

await t.run();
