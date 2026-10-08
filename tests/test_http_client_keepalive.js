// flags: --std
// The reactor HTTP client keeps connections alive: a finished exchange whose
// framing proves where the response ended parks its connection for the next
// request to the same host, and nothing about that changes what a caller sees.
import * as os from "os";
import * as std from "std";
import { suite } from "./kit.js";
import { App, TCPServer } from "dyna:net";
import { Spawn, cwd } from "dyna:sys";

const t = suite("http_client_keepalive");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const bytes = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };
const message = async (p) => { try { await p; return "RESOLVED"; } catch (e) { return String(e && e.message); } };
const OK = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

// A raw peer. `answer(n, conn, request)` is called for the n-th request (from 1)
// seen on a connection and returns the bytes to send, or null to hang up.
// It records how many connections were accepted and how many are open.
function peer(answer) {
    const srv = new TCPServer({ port: 0 });
    const st = { srv, accepted: 0, open: 0, requests: 0, url: null, heads: [] };
    srv.start({
        connect(conn) { st.accepted++; st.open++; conn.buf = ""; conn.n = 0; },
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
                const out = answer(conn.n, conn, head);
                if (out === null) { conn.close(); return; }
                if (typeof out === "string") conn.write(bytes(out));
            }
        },
        close() { st.open--; },
    });
    st.url = "http://127.0.0.1:" + srv.port + "/";
    return st;
}

t.test("sequential requests share one connection", async (a) => {
    const p = peer(() => OK);
    for (let i = 0; i < 20; i++) a.eq(await (await fetch(p.url)).text(), "ok");
    a.eq(p.accepted, 1, "20 requests, one connection");
    a.match(p.heads[0], /\r\nConnection: keep-alive\r\n/i);
    p.srv.close();
});

t.test("concurrent requests reuse what the previous wave left", async (a) => {
    const p = peer(() => OK);
    for (let wave = 0; wave < 5; wave++) await Promise.all(Array.from({ length: 16 }, () => fetch(p.url).then((r) => r.text())));
    a.eq(p.requests, 80);
    a.ok(p.accepted <= 16, "80 requests over " + p.accepted + " connections (at most the 16 in flight at once)");
    p.srv.close();
});

t.test("a chunked body ends at its last chunk and the connection is reused", async (a) => {
    const chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n5;ext=1\r\ndefgh\r\n0\r\nX-Trailer: 1\r\n\r\n";
    const p = peer(() => chunked);
    a.eq(await (await fetch(p.url)).text(), "abcdefgh");
    a.eq(await (await fetch(p.url)).text(), "abcdefgh");
    a.eq(p.accepted, 1);
    p.srv.close();
});

t.test("a chunked body delivered one byte at a time is assembled the same", async (a) => {
    const whole = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nwxyz\r\nA\r\n0123456789\r\n0\r\n\r\n";
    const p = peer((n, conn) => { (async () => { for (const ch of whole) { conn.write(bytes(ch)); await sleep(1); } })(); return undefined; });
    a.eq(await (await fetch(p.url)).text(), "wxyz0123456789");
    a.eq(await (await fetch(p.url)).text(), "wxyz0123456789");
    a.eq(p.accepted, 1, "and the connection was reusable afterwards");
    p.srv.close();
}, { timeoutMs: 20000 });

t.test("a peer that says close, speaks HTTP/1.0, or sends surplus bytes is not reused", async (a) => {
    for (const [why, reply] of [
        ["Connection: close", "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok"],
        ["a close token in a list", "HTTP/1.1 200 OK\r\nConnection: Upgrade, Close\r\nContent-Length: 2\r\n\r\nok"],
        ["HTTP/1.0", "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok"],
        ["surplus bytes", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokJUNK"],
    ]) {
        const p = peer(() => reply);
        a.eq(await (await fetch(p.url)).text(), "ok", why);
        a.eq(await (await fetch(p.url)).text(), "ok", why);
        a.eq(p.accepted, 2, why + ": a second connection was opened");
        p.srv.close();
    }
});

t.test("a body with no framing still reads to the end of the connection", async (a) => {
    const p = peer((n, conn) => { conn.write(bytes("HTTP/1.1 200 OK\r\n\r\nuntil-close")); return null; });
    a.eq(await (await fetch(p.url)).text(), "until-close");
    a.eq(await (await fetch(p.url)).text(), "until-close");
    a.eq(p.accepted, 2);
    p.srv.close();
});

t.test("a reused connection the peer has dropped is retried once on a fresh one", async (a) => {
    // Each connection answers its first request and hangs up on its second.
    const p = peer((n) => (n === 1 ? OK : null));
    a.eq(await (await fetch(p.url)).text(), "ok");
    a.eq(await (await fetch(p.url)).text(), "ok", "the second request was replayed");
    a.eq(p.accepted, 2);
    a.eq(p.requests, 3, "first, the dropped replay, and its retry");
    p.srv.close();
});

t.test("a reused connection that dies part-way through a response is not replayed", async (a) => {
    const p = peer((n, conn) => { if (n === 1) return OK; conn.write(bytes("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc")); return null; });
    a.eq(await (await fetch(p.url)).text(), "ok");
    a.match(await message(fetch(p.url)), /truncated body: connection ended after 3 of 10 declared bytes/);
    a.eq(p.requests, 2, "the peer saw the second request exactly once");
    p.srv.close();
});

t.test("a POST always opens its own connection, and leaves it for a later GET", async (a) => {
    const p = peer(() => OK);
    await (await fetch(p.url)).text();
    await (await fetch(p.url, { method: "POST", body: "x" })).text();
    a.eq(p.accepted, 2, "the POST did not ride the pooled connection");
    await (await fetch(p.url, { method: "POST", body: "y" })).text();
    a.eq(p.accepted, 3);
    await (await fetch(p.url)).text();
    await (await fetch(p.url)).text();
    a.eq(p.accepted, 3, "GETs reuse what is pooled");
    p.srv.close();
});

t.test("a POST is never replayed", async (a) => {
    const p = peer((n, conn, head) => (/^POST/.test(head) ? null : OK));
    await (await fetch(p.url)).text();
    a.match(await message(fetch(p.url, { method: "POST", body: "x" })), /failed/);
    a.eq(p.requests, 2, "one GET and exactly one POST reached the peer");
    p.srv.close();
});

t.test("malformed chunk framing falls back to reading until the peer closes", async (a) => {
    const p = peer((n, conn) => { conn.write(bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nabc\r\n0\r\n\r\n")); return null; });
    a.match(await message(fetch(p.url)), /failed/, "the decoder refuses it once the stream ends");
    p.srv.close();
});

t.test("an unterminated chunked body on an open connection ends at the timeout", async (a) => {
    const p = peer(() => "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n");
    const first = await Promise.race([message(fetch(p.url, { timeout: 300 })), sleep(5000).then(() => "STILL WAITING after 5 s")]);
    a.match(first, /failed/);
    p.srv.close();
}, { timeoutMs: 15000 });

t.test("HEAD and 204 leave the connection reusable", async (a) => {
    const p = peer((n, conn, head) => (/^HEAD/.test(head) ? "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n" : "HTTP/1.1 204 No Content\r\n\r\n"));
    a.eq((await fetch(p.url, { method: "HEAD" })).status, 200);
    a.eq((await fetch(p.url)).status, 204);
    a.eq((await fetch(p.url, { method: "HEAD" })).status, 200);
    a.eq(p.accepted, 1);
    p.srv.close();
});

t.test("onConnect sees the peer address on a reused connection too, and can refuse it", async (a) => {
    const p = peer(() => OK);
    const seen = [];
    for (let i = 0; i < 3; i++) await (await fetch(p.url, { onConnect: (ip) => { seen.push(ip); return true; } })).text();
    a.deep(seen, ["127.0.0.1", "127.0.0.1", "127.0.0.1"]);
    a.eq(p.accepted, 1);
    a.match(await message(fetch(p.url, { onConnect: () => false })), /failed/);
    a.eq(p.requests, 3, "the refused request was never sent");
    p.srv.close();
});

t.test("idle connections are bounded and expire", async (a) => {
    const p = peer(() => OK);
    await Promise.all(Array.from({ length: 300 }, () => fetch(p.url).then((r) => r.text())));
    await sleep(300);
    a.ok(p.open <= 256, p.open + " connections left open after 300 concurrent requests (cap 256)");
    a.ok(p.open >= 1, "some are kept");
    await sleep(5000);
    a.eq(p.open, 0, "all idle connections were closed after the 4 s idle limit");
    p.srv.close();
}, { timeoutMs: 30000 });

t.test("replies of every size from an App arrive intact over kept-alive connections", async (a) => {
    const app = new App({ port: 0 });
    app.get("/n/:n", (req) => "x".repeat(+req.params.n));
    app.start();
    const B = "http://127.0.0.1:" + app.port;
    for (const n of [0, 1, 8192, 70000, 300000, 5]) a.eq((await (await fetch(B + "/n/" + n)).text()).length, n);
    app.close();
    a.ok(true);
});

t.test("a script exits at once with a connection still pooled", async (a) => {
    const p = peer(() => OK);
    const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_hka.${os.getpid()}.js`;
    const f = std.open(T, "w");
    f.puts(`import * as std from "std";\nfor (let i = 0; i < 3; i++) await (await fetch("${p.url}")).text();\nstd.out.puts("DONE\\n");\nstd.out.flush();\n`);
    f.close();
    const child = new Spawn(cwd() + "/dynajs", ["--std", T]);
    const buf = new Uint8Array(64);
    const n = await child.stdout.read(buf);
    a.eq(String.fromCharCode.apply(null, buf.subarray(0, n)).trim(), "DONE");
    const res = await Promise.race([child.wait(), sleep(2000).then(() => "STILL RUNNING 2 s after its last line")]);
    a.ok(typeof res === "object", "exited without waiting out the idle limit: " + JSON.stringify(res));
    if (typeof res !== "object") child.kill("SIGKILL");
    child.close();
    os.remove(T);
    a.eq(p.accepted, 1, "and its three requests shared a connection");
    p.srv.close();
}, { timeoutMs: 20000 });

t.test("a Worker that leaves pooled connections behind exits cleanly", async (a) => {
    const p = peer(() => OK);
    const W = `${std.getenv("TMPDIR") || "/tmp"}/dj_hka_w.${os.getpid()}.js`;
    const f = std.open(W, "w");
    f.puts(`import * as os from "os";\nlet n = 0;\nfor (let i = 0; i < 6; i++) n += (await (await fetch("${p.url}")).text()).length;\nos.Worker.parent.postMessage(n);\n`);
    f.close();
    const got = await new Promise((resolve) => {
        const w = new os.Worker(W);
        w.onmessage = (e) => { w.onmessage = null; resolve(e.data); };
        setTimeout(() => resolve("TIMEOUT"), 10000);
    });
    os.remove(W);
    a.eq(got, 12);
    await sleep(300);
    a.eq(await (await fetch(p.url)).text(), "ok", "the main thread still fetches afterwards");
    p.srv.close();
}, { timeoutMs: 20000 });

await t.run();
