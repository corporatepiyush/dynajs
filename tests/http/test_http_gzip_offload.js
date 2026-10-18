// Responses of 8 KiB and up are gzipped on the io thread pool while the
// connection is parked: bytes must be identical to the inline path, pipelined
// replies must keep request order, and a client that vanishes mid-compression
// must not disturb the server. Already-compressed media types are not gzipped.
import { suite } from "../lib/kit.js";
import { App, TCPServer } from "dyna:net";
import { gunzip } from "dyna:compress";

const t = suite("http_gzip_offload");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function body(n, tag) {
    let s = "";
    for (let i = 0; s.length < n; i++) s += '{"id":' + i + ',"tag":"' + tag + '","path":"/api/v1/resource/' + i + '"},';
    return s.slice(0, n);
}
const BIG = body(100000, "big"), EDGE = [8191, 8192, 8193].map((n) => body(n, "edge" + n));
const PIX = new Uint8Array(20000).fill(7);

const app = new App({ port: 0, compress: true });
app.get("/small", () => ({ body: "small-reply", contentType: "text/plain" }));
app.get("/big", () => ({ body: BIG, contentType: "application/json" }));
app.get("/bigasync", async () => { await sleep(5); return { body: BIG, contentType: "application/json", headers: { "X-Mark": "async" } }; });
app.get("/n/:i", (req) => ({ body: BIG.slice(0, 20000) + "#" + req.params.i, contentType: "text/plain" }));
EDGE.forEach((b, i) => app.get("/edge" + i, () => ({ body: b, contentType: "application/json" })));
app.get("/png", () => ({ body: PIX, contentType: "image/png" }));
app.get("/svg", () => ({ body: PIX, contentType: "image/svg+xml" }));
app.get("/zip", () => ({ body: PIX, contentType: "application/zip; x=1" }));
app.start();
t.after(() => app.close());

// Sends `request`, collects until `want` complete responses have arrived (or the
// peer closes), and returns them parsed: {status, headers, body: Uint8Array}.
function exchange(request, want) {
    return new Promise((resolve, reject) => {
        let buf = new Uint8Array(0);
        const out = [];
        const timer = setTimeout(() => { h.close(); reject(new Error("exchange: got " + out.length + " of " + want)); }, 8000);
        const done = () => { clearTimeout(timer); h.close(); resolve(out); };
        const parse = () => {
            for (;;) {
                let end = -1;
                for (let i = 0; i + 3 < buf.length; i++)
                    if (buf[i] === 13 && buf[i + 1] === 10 && buf[i + 2] === 13 && buf[i + 3] === 10) { end = i; break; }
                if (end < 0) return;
                const head = String.fromCharCode.apply(null, buf.subarray(0, end)).split("\r\n");
                const headers = {};
                for (const line of head.slice(1)) { const k = line.indexOf(":"); headers[line.slice(0, k).toLowerCase()] = line.slice(k + 1).trim(); }
                const len = +(headers["content-length"] || 0);
                if (buf.length < end + 4 + len) return;
                out.push({ status: +head[0].split(" ")[1], headers, body: buf.slice(end + 4, end + 4 + len) });
                buf = buf.slice(end + 4 + len);
                if (out.length === want) { done(); return; }
            }
        };
        const h = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
            connect: (conn, err) => { if (err) { clearTimeout(timer); reject(new Error(err)); } else conn.write(request); },
            data: (conn, b) => { const nb = new Uint8Array(buf.length + b.length); nb.set(buf); nb.set(b, buf.length); buf = nb; parse(); },
            close: () => { clearTimeout(timer); resolve(out); },
        });
    });
}
const GET = (path, gz) => "GET " + path + " HTTP/1.1\r\nHost: x\r\n" + (gz ? "Accept-Encoding: gzip\r\n" : "") + "\r\n";
const str = (u8) => { let s = ""; for (let i = 0; i < u8.length; i += 8192) s += String.fromCharCode.apply(null, u8.subarray(i, i + 8192)); return s; };

t.test("a large body is gzipped and decodes to the handler's bytes", async (a) => {
    const [r] = await exchange(GET("/big", true), 1);
    a.eq(r.status, 200);
    a.eq(r.headers["content-encoding"], "gzip");
    a.eq(r.headers["vary"], "Accept-Encoding");
    a.ok(r.body.length < BIG.length / 4, "compressed to " + r.body.length);
    a.eq(str(gunzip(r.body)), BIG);
});

t.test("without Accept-Encoding the same route is sent as is", async (a) => {
    const [r] = await exchange(GET("/big", false), 1);
    a.eq(r.headers["content-encoding"], undefined);
    a.eq(str(r.body), BIG);
});

t.test("bodies on both sides of the 8 KiB hand-off decode identically", async (a) => {
    for (let i = 0; i < EDGE.length; i++) {
        const [r] = await exchange(GET("/edge" + i, true), 1);
        a.eq(r.headers["content-encoding"], "gzip", "edge " + EDGE[i].length);
        a.eq(str(gunzip(r.body)), EDGE[i], "edge " + EDGE[i].length);
    }
});

t.test("pipelined replies keep request order across inline and pooled work", async (a) => {
    const order = ["/n/0", "/small", "/n/1", "/small", "/big", "/small", "/n/2", "/n/3", "/small"];
    const rs = await exchange(order.map((p) => GET(p, true)).join(""), order.length);
    a.must(rs.length === order.length, "all " + order.length + " replies arrived, got " + rs.length);
    rs.forEach((r, i) => {
        const text = r.headers["content-encoding"] === "gzip" ? str(gunzip(r.body)) : str(r.body);
        const p = order[i];
        if (p === "/small") a.eq(text, "small-reply", "reply " + i);
        else if (p === "/big") a.eq(text, BIG, "reply " + i);
        else a.ok(text.endsWith("#" + p.slice(3)), "reply " + i + " belongs to " + p + " (ends " + JSON.stringify(text.slice(-4)) + ")");
    });
});

t.test("many connections in flight each get their own body", async (a) => {
    const all = await Promise.all(Array.from({ length: 24 }, (_, i) => exchange(GET("/n/" + (100 + i), true), 1)));
    all.forEach(([r], i) => a.ok(str(gunzip(r.body)).endsWith("#" + (100 + i)), "connection " + i));
});

t.test("an async handler's large reply takes the same path and keeps its headers", async (a) => {
    const [r] = await exchange(GET("/bigasync", true), 1);
    a.eq(r.headers["content-encoding"], "gzip");
    a.eq(r.headers["x-mark"], "async");
    a.eq(str(gunzip(r.body)), BIG);
});

t.test("already-compressed media types are not gzipped; svg is", async (a) => {
    const [png] = await exchange(GET("/png", true), 1);
    a.eq(png.headers["content-encoding"], undefined, "image/png");
    a.eq(png.body.length, PIX.length);
    const [zip] = await exchange(GET("/zip", true), 1);
    a.eq(zip.headers["content-encoding"], undefined, "application/zip with a parameter");
    const [svg] = await exchange(GET("/svg", true), 1);
    a.eq(svg.headers["content-encoding"], "gzip", "image/svg+xml");
    a.eq(gunzip(svg.body).length, PIX.length);
});

t.test("clients that vanish mid-compression leave the server healthy", async (a) => {
    for (let round = 0; round < 40; round++) {
        await new Promise((resolve) => {
            const h = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
                connect: (conn, err) => { if (!err) conn.write(GET("/big", true) + GET("/big", true)); h.close(); resolve(); },
            });
        });
    }
    await sleep(100);
    const [r] = await exchange(GET("/big", true), 1);
    a.eq(str(gunzip(r.body)), BIG, "still serving");
});

// With no pool (--io-threads 0) or a single thread, the same replies are
// produced on the reactor or the lone thread; what a client reads is unchanged.
// (That cpu work never queues behind blocking pool jobs is pinned at the pool
// and reactor layers: tests/core/test_pool.c and tests/net/test_aio_cpu_offload.c.)
const CHILD = [
    'import * as std from "std";',
    'import { App, TCPServer } from "dyna:net";',
    'import { gunzip } from "dyna:compress";',
    'const BIG = "{" + "\\"k\\":\\"value\\",".repeat(9000) + "\\"end\\":1}";',
    'const app = new App({ port: 0, compress: true });',
    'app.get("/big", () => ({ body: BIG, contentType: "application/json" }));',
    'app.start();',
    'let buf = new Uint8Array(0), got = 0;',
    'const h = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {',
    '  connect: (c) => c.write("GET /big HTTP/1.1\\r\\nHost: x\\r\\nAccept-Encoding: gzip\\r\\n\\r\\n".repeat(3)),',
    '  data: (c, b) => { const nb = new Uint8Array(buf.length + b.length); nb.set(buf); nb.set(b, buf.length); buf = nb;',
    '    for (;;) { let end = -1; for (let i = 0; i + 3 < buf.length; i++) if (buf[i] === 13 && buf[i + 1] === 10 && buf[i + 2] === 13 && buf[i + 3] === 10) { end = i + 4; break; }',
    '      if (end < 0) return; const head = String.fromCharCode.apply(null, buf.subarray(0, end)); const len = +/content-length: (\\d+)/i.exec(head)[1];',
    '      if (buf.length < end + len) return; const body = buf.subarray(end, end + len); buf = buf.slice(end + len);',
    '      const text = /content-encoding: gzip/i.test(head) ? gunzip(body) : body; if (text.length === BIG.length) got++;',
    '      if (got === 3) { std.out.puts("GZIP-OK 3\\n"); std.out.flush(); h.close(); app.close(); return; } } },',
    '});',
].join("\n");

for (const n of [0, 1]) {
    t.test("compressed replies are intact with --io-threads " + n, async (a) => {
        const { Exec, cwd } = await import("dyna:sys");
        const { makeTempDir, writeFile, removeAll } = await import("dyna:file");
        const dir = makeTempDir("gzchild");
        writeFile(dir.join("child.js"), CHILD);
        const r = Exec("/bin/sh", ["-c", "'" + cwd() + "/dynajs' --std --io-threads " + n + " '" + dir.join("child.js") + "' 2>&1"], { timeoutMs: 20000, encoding: "utf8" });
        removeAll(dir);
        a.match(String(r.stdout), /GZIP-OK 3/, String(r.stdout).slice(0, 200));
    }, { timeoutMs: 30000 });
}

await t.run();
