// flags: --std
// covers: c:src/modules/http/http-client-transport.c
// F10 (2026-11-16) gave the blocking HTTPClient one kept connection per client. T1 (2026-11-26) removed
// the blocking client: get/post/request/getStream now run on the reactor and share fetch()'s connection
// pool, so these cases pin the pooled contract: sequential requests reuse one connection, a reused
// connection that died before any response byte is retried once on a fresh one only for replayable
// requests (GET/HEAD/OPTIONS or an Idempotency-Key), a POST lost on a dropped pooled connection is
// rejected (RFC 9110 9.2.2) and the host is remembered so the next non-replayable request opens its
// own, connections are pooled per origin, the client owns the Connection field, and disconnect()
// cancels in-flight exchanges and closes the idle connections this client pooled. fetch() shares the transport: its cases pin the
// same retry (RFC 9112 9.3.1) for an origin that closes as the request is written, or 50 ms after its last answer.
// Each case runs its client in a child process against a raw peer that counts accepted connections,
// so the counts are not shared with this process's pool.
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { TCPServer } from "dyna:net";
import { Spawn, args } from "dyna:sys";

const t = suite("http_client_pooled_keepalive");
const bytes = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };
const OK = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

function peer(answer) {
    const srv = new TCPServer({ port: 0 });
    const st = { srv, accepted: 0, requests: 0, heads: [], perConn: [] };
    srv.start({
        connect(conn) { st.accepted++; conn.buf = ""; conn.n = 0; conn.id = st.accepted; },
        data(conn, b) {
            conn.buf += String.fromCharCode.apply(null, b);
            for (;;) {
                const end = conn.buf.indexOf("\r\n\r\n");
                if (end < 0) return;
                const head = conn.buf.slice(0, end + 4);
                const m = /content-length: (\d+)/i.exec(head);
                const total = end + 4 + (m ? +m[1] : 0);
                if (conn.buf.length < total) return;
                conn.buf = conn.buf.slice(total);
                conn.n++; st.requests++; st.heads.push(head);
                st.perConn.push(conn.id + ":" + conn.n + ":" + head.slice(0, head.indexOf(" ")));
                const out = answer(conn.n, conn, head);
                if (out === null) { conn.close(); return; }
                if (typeof out === "string") conn.write(bytes(out));
                else { conn.write(bytes(out.data)); if (out.close) conn.close(); }
            }
        },
    });
    st.url = "http://127.0.0.1:" + srv.port + "/";
    return st;
}

let seq = 0;
async function child(body) {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_hsk.${os.getpid()}.${seq++}.js`;
    const f = std.open(T, "w");
    f.puts(`import * as os from "os";\nimport * as std from "std";\nimport { HTTPClient } from "dyna:http";\nconst out = [];\nconst c = new HTTPClient();\nc.setTimeout(5000);\nconst run = async (fn) => { try { out.push(await fn()); } catch (e) { out.push("ERR " + e.message); } };\n${body}\nstd.out.puts(JSON.stringify(out) + "\\n");\nstd.out.flush();\n`);
    f.close();
    const ch = new Spawn(args()[0], ["--std", T]);
    const buf = new Uint8Array(65536);
    let s = "";
    for (;;) {
        const n = await ch.stdout.read(buf);
        if (!n) break;
        s += String.fromCharCode.apply(null, buf.subarray(0, n));
        if (s.endsWith("\n")) break;
    }
    await ch.wait();
    ch.close();
    os.remove(T);
    try { return JSON.parse(s); } catch (e) { return ["BAD OUTPUT " + s]; }
}

t.test("sequential GETs share one connection", async (a) => {
    const p = peer(() => OK);
    const r = await child(`for (let i = 0; i < 20; i++) await run(async () => (await c.get("${p.url}" + i)).body);`);
    a.eq(r.join(""), "ok".repeat(20));
    a.eq(p.accepted, 1, "20 requests, one connection");
    a.match(p.heads[0], /\r\nConnection: keep-alive\r\n/i);
    p.srv.close();
});

t.test("chunked and bodyless responses keep the connection too", async (a) => {
    const p = peer((n, c, head) => head.startsWith("HEAD")
        ? "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"
        : n % 2 ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n" : "HTTP/1.1 204 No Content\r\n\r\n");
    const r = await child(`await run(async () => (await c.get("${p.url}")).body); await run(async () => (await c.get("${p.url}")).status); await run(async () => (await c.request("HEAD", "${p.url}")).status); await run(async () => (await c.get("${p.url}")).status);`);
    a.eq(JSON.stringify(r), JSON.stringify(["abc", 204, 200, 204]));
    a.eq(p.accepted, 1, "one connection for chunked, 204 and HEAD");
    p.srv.close();
});

t.test("a server that says Connection: close gets a new connection per request", async (a) => {
    const p = peer(() => ({ data: "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok", close: true }));
    const r = await child(`for (let i = 0; i < 5; i++) await run(async () => (await c.get("${p.url}")).body);`);
    a.eq(r.join(""), "ok".repeat(5));
    a.eq(p.accepted, 5);
    p.srv.close();
});

t.test("a body framed by close is never reused", async (a) => {
    const p = peer(() => ({ data: "HTTP/1.1 200 OK\r\n\r\nuntil-eof", close: true }));
    const r = await child(`for (let i = 0; i < 3; i++) await run(async () => (await c.get("${p.url}")).body);`);
    a.eq(r.join(","), "until-eof,until-eof,until-eof");
    a.eq(p.accepted, 3);
    p.srv.close();
});

t.test("an HTTP/1.0 response is not reused", async (a) => {
    const p = peer(() => "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const r = await child(`for (let i = 0; i < 3; i++) await run(async () => (await c.get("${p.url}")).body);`);
    a.eq(r.join(""), "okokok");
    a.eq(p.accepted, 3);
    p.srv.close();
});

t.test("a kept connection the server dropped on the next request is retried once on a fresh one", async (a) => {
    const p = peer((n) => n === 1 ? OK : null);
    const r = await child(`for (let i = 0; i < 4; i++) await run(async () => (await c.get("${p.url}")).body);`);
    a.eq(r.join(","), "ok,ok,ok,ok", "every GET answered");
    a.eq(p.accepted, 4, "first on its own; each reuse dropped, retried fresh");
    p.srv.close();
});

t.test("a kept connection the server closed while idle is not used", async (a) => {
    const q = peer((n, conn) => { os.setTimeout(() => conn.close(), 30); return OK; });
    const r = await child(`let calls = 0; c.onConnect = () => { calls++; return true; };\nfor (let i = 0; i < 3; i++) { await run(async () => (await c.get("${q.url}")).body); os.sleep(200); }\nout.push(calls);`);
    a.eq(r.join(","), "ok,ok,ok,3", "three answers and one connection attempt each");
    a.eq(q.accepted, 3, "the closed connection is noticed before sending");
    a.eq(q.requests, 3, "and no request was written into it");
    q.srv.close();
});

t.test("bytes the server pushes onto an idle kept connection make it unusable", async (a) => {
    const q = peer((n, conn) => { os.setTimeout(() => conn.write(bytes("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nstale")), 30); return OK; });
    const r = await child(`for (let i = 0; i < 2; i++) { await run(async () => (await c.get("${q.url}")).body); os.sleep(200); }`);
    a.eq(r.join(","), "ok,ok", "the unsolicited response is never read as the answer to the next request");
    a.eq(q.accepted, 2);
    q.srv.close();
});

t.test("a POST that a dropped pooled connection loses is not replayed, and the host is then remembered", async (a) => {
    const p = peer((n) => n === 1 ? OK : null);
    const r = await child(`await run(async () => (await c.get("${p.url}")).body); await run(async () => (await c.post("${p.url}", "x")).body); await run(async () => (await c.post("${p.url}", "y")).body); await run(async () => (await c.get("${p.url}")).body);`);
    a.eq(r[0], "ok");
    a.ok(/^ERR .*connection closed before any response/.test(r[1]), "RFC 9110 9.2.2: the POST lost on a dropped pooled connection is rejected, not replayed: " + r[1]);
    a.eq(r[2], "ok", "the next POST to a host that dropped a pooled connection opens its own");
    a.ok(p.perConn.includes("2:1:POST"), "the second POST opened connection 2: " + p.perConn.join(" "));
    a.eq(r[3], "ok");
    p.srv.close();
});

t.test("an Idempotency-Key POST may reuse and retry", async (a) => {
    const p = peer(() => OK);
    const r = await child(`await run(async () => (await c.get("${p.url}")).body); await run(async () => (await c.post("${p.url}", "x", { "Idempotency-Key": "k1" })).body);`);
    a.eq(r.join(","), "ok,ok");
    a.eq(p.accepted, 1);
    p.srv.close();
});

const F = "const { fetch } = await import(\"dyna:http\");\n";
t.test("fetch: a GET the origin drops as it is written on a reused connection is retried once on a fresh one (RFC 9112 9.3.1)", async (a) => {
    const p = peer((n) => n === 1 ? OK : null);
    const r = await child(F + `for (let i = 0; i < 3; i++) await run(async () => (await fetch("${p.url}")).text());`);
    a.eq(r.join(","), "ok,ok,ok", "every GET answered");
    a.eq(p.accepted, 3, "each reused connection dropped on the request, each GET retried on its own");
    a.eq(p.perConn.filter((x) => x.endsWith(":GET")).length, 5, "the dropped GETs reached the origin once before their retry: " + p.perConn.join(" "));
    p.srv.close();
});

t.test("fetch: a POST lost on a reused connection is rejected, not replayed", async (a) => {
    const p = peer((n) => n === 1 ? OK : null);
    const r = await child(F + `await run(async () => (await fetch("${p.url}")).text()); await run(async () => (await fetch("${p.url}", { method: "POST", body: "x" })).text());`);
    a.eq(r[0], "ok");
    a.ok(/^ERR /.test(String(r[1])), "the POST rejects: " + r[1]);
    a.eq(p.perConn.filter((x) => x.endsWith(":POST")).length, 1, "the POST reached the origin exactly once: " + p.perConn.join(" "));
    p.srv.close();
});

t.test("fetch: a GET racing the origin's close 50 ms after its last answer succeeds", async (a) => {
    const p = peer((n, conn) => { if (n !== 1) return { data: "" }; os.setTimeout(() => conn.close(), 50); return OK; });
    const r = await child(F + `for (let i = 0; i < 2; i++) await run(async () => (await fetch("${p.url}")).text());`);
    a.eq(r.join(","), "ok,ok", "the second GET, written into the closing connection, is retried");
    a.eq(p.accepted, 2);
    p.srv.close();
});

t.test("pooled connections are kept per origin: switching hosts keeps both", async (a) => {
    const p = peer(() => OK), q = peer(() => OK);
    const r = await child(`for (const u of ["${p.url}", "${p.url}", "${q.url}", "${q.url}", "${p.url}"]) await run(async () => (await c.get(u)).body);`);
    a.eq(r.join(""), "ok".repeat(5));
    a.eq(p.accepted, 1, "first host: its pooled connection survives the switch");
    a.eq(q.accepted, 1);
    p.srv.close(); q.srv.close();
});

t.test("a caller's Connection header is dropped: the client owns it", async (a) => {
    const p = peer(() => OK);
    const r = await child(`await run(async () => (await c.get("${p.url}", { Connection: "close" })).body); await run(async () => (await c.get("${p.url}", "Connection: close")).body); await run(async () => (await c.get("${p.url}", { "X-A": "1", connection: "upgrade" })).body);`);
    a.eq(r.join(""), "okokok");
    a.eq(p.accepted, 1);
    for (const h of p.heads) a.eq((h.match(/\r\nconnection:/gi) || []).length, 1, "exactly one Connection field: " + JSON.stringify(h));
    a.match(p.heads[2], /\r\nX-A: 1\r\n/);
    p.srv.close();
});

t.test("disconnect() drops the kept connection", async (a) => {
    const p = peer(() => OK);
    const r = await child(`await run(async () => (await c.get("${p.url}")).body); await run(async () => (await c.get("${p.url}")).body); c.disconnect(); await run(async () => (await c.get("${p.url}")).body);`);
    a.eq(r.join(""), "okokok");
    a.eq(p.accepted, 2);
    p.srv.close();
});

t.test("onConnect still vets every request on a reused connection", async (a) => {
    const p = peer(() => OK);
    const r = await child(`let calls = 0; c.onConnect = (ip) => { calls++; return calls < 3; };\nfor (let i = 0; i < 3; i++) await run(async () => (await c.get("${p.url}")).body);\nout.push(calls);`);
    a.eq(r[0], "ok");
    a.eq(r[1], "ok");
    a.match(String(r[2]), /^ERR .*connect/i, "the third is refused although its connection is kept");
    a.eq(r[3], 3);
    a.eq(p.accepted, 1);
    a.eq(p.requests, 2, "the refused request never reached the wire");
    p.srv.close();
});

t.test("an error response with a framed body keeps the connection", async (a) => {
    const p = peer((n) => n === 1 ? "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\nnope" : OK);
    const r = await child(`await run(async () => (await c.get("${p.url}")).status); await run(async () => (await c.get("${p.url}")).body);`);
    a.eq(JSON.stringify(r), JSON.stringify([404, "ok"]));
    a.eq(p.accepted, 1);
    p.srv.close();
});

await t.run();
