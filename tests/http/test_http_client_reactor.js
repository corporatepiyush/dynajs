// flags: --std
// fetch runs on the shared reactor: an in-flight request holds no pool thread,
// many run at once, and the incremental response reader keeps the blocking
// client's verdicts (lengths, truncation, interim responses, limits, timeouts).
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { App, TCPServer, WsClient } from "dyna:net";
import { Exec, cwd } from "dyna:sys";

// race p against a value after ms; the timer is cleared when the race settles
function within(p, ms, v) {
    let timer;
    return Promise.race([p, new Promise((r) => { timer = setTimeout(() => r(v), ms); })]).finally(() => clearTimeout(timer));
}

const t = suite("http_client_reactor");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const bytes = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };

const app = new App({ port: 0 });
app.get("/n/:n", (req) => "x".repeat(+req.params.n));
app.post("/echo", (req) => ({ body: req.body, contentType: "application/octet-stream" }));
app.start();
const B = "http://127.0.0.1:" + app.port;
t.after(() => app.close());

// A raw peer that answers every connection with fixed bytes, then closes or holds.
function rawPeer(reply, hold) {
    const srv = new TCPServer({ port: 0 });
    srv.start({ data(conn) { if (reply) conn.write(bytes(reply)); if (!hold) conn.close(); } });
    return srv;
}
const message = async (p) => { try { await p; return "RESOLVED"; } catch (e) { return String(e && e.message); } };

t.test("parked requests hold no pool thread", async (a) => {
    const held = [];
    const mute = new TCPServer({ port: 0 });
    mute.start({ connect(conn) { held.push(conn); } });
    const parked = [];
    for (let i = 0; i < 24; i++) parked.push(message(fetch("http://127.0.0.1:" + mute.port + "/")));
    for (let w = 0; w < 100 && held.length < 24; w++) await sleep(20);
    a.eq(held.length, 24, "24 requests are waiting on a peer that never answers");
    let looked = false;
    const probe = TCPServer.connect({ host: "localhost", port: app.port }, { connect: (conn) => { looked = true; if (conn) conn.close(); } });
    for (let w = 0; w < 100 && !looked; w++) await sleep(20);
    a.eq(looked, true, "a name lookup still gets a pool thread");
    a.eq(await (await fetch(B + "/n/5")).text(), "xxxxx", "and a new request completes");
    for (const c of held) c.close();
    mute.close();
    probe.close();
    const out = await Promise.all(parked);
    a.eq(out.filter((m) => /failed/.test(m)).length, 24, "every parked request rejects once the peer hangs up");
}, { timeoutMs: 30000 });

t.test("300 requests in flight at once each get their own body", async (a) => {
    const sizes = [0, 1, 100, 8191, 8192, 70000, 300000];
    const jobs = [];
    for (let i = 0; i < 300; i++) { const n = sizes[i % sizes.length]; jobs.push(fetch(B + "/n/" + n).then((r) => r.text()).then((s) => s.length === n)); }
    const ok = await Promise.all(jobs);
    a.eq(ok.filter(Boolean).length, 300);
}, { timeoutMs: 60000 });

t.test("a request body larger than a socket buffer arrives whole", async (a) => {
    // The peer counts body bytes and answers with the count once all have come.
    const want = 3 * 1024 * 1024;
    const sink = new TCPServer({ port: 0 });
    sink.start({
        connect(conn) { conn.seen = 0; conn.head = ""; },
        data(conn, b) {
            if (conn.head !== null) {
                conn.head += String.fromCharCode.apply(null, b.subarray(0, Math.min(b.length, 2048)));
                const end = conn.head.indexOf("\r\n\r\n");
                if (end < 0) return;
                conn.seen = b.length - (end + 4 - (conn.head.length - Math.min(b.length, 2048)));
                conn.head = null;
            } else conn.seen += b.length;
            if (conn.seen >= want) { const body = String(conn.seen); conn.write(bytes("HTTP/1.1 200 OK\r\nContent-Length: " + body.length + "\r\n\r\n" + body)); conn.close(); }
        },
    });
    const r = await fetch("http://127.0.0.1:" + sink.port + "/", { method: "POST", body: new Uint8Array(want) });
    a.eq(await r.text(), String(want));
    sink.close();
}, { timeoutMs: 30000 });

t.test("a body with no declared length is read to the end of the connection", async (a) => {
    const srv = rawPeer("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil-close");
    a.eq(await (await fetch("http://127.0.0.1:" + srv.port + "/")).text(), "until-close");
    srv.close();
});

t.test("a short body names how much arrived", async (a) => {
    const srv = rawPeer("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
    a.match(await message(fetch("http://127.0.0.1:" + srv.port + "/")), /truncated body: connection ended after 3 of 10 declared bytes/);
    srv.close();
});

t.test("interim 1xx responses are skipped, at most eight of them", async (a) => {
    const one = "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n";
    const fin = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    let srv = rawPeer(one + one + fin);
    a.eq(await (await fetch("http://127.0.0.1:" + srv.port + "/")).text(), "ok");
    srv.close();
    srv = rawPeer(one.repeat(9) + fin);
    a.match(await message(fetch("http://127.0.0.1:" + srv.port + "/")), /failed/);
    srv.close();
});

t.test("bytes past the declared length are dropped", async (a) => {
    const srv = rawPeer("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokEXTRA");
    a.eq(await (await fetch("http://127.0.0.1:" + srv.port + "/")).text(), "ok");
    srv.close();
});

t.test("a response head over 64 KiB, and a peer that sends nothing, are refused", async (a) => {
    let srv = rawPeer("HTTP/1.1 200 OK\r\nX-Pad: " + "p".repeat(70000) + "\r\n\r\n");
    a.match(await message(fetch("http://127.0.0.1:" + srv.port + "/")), /failed/);
    srv.close();
    srv = rawPeer("");
    a.match(await message(fetch("http://127.0.0.1:" + srv.port + "/")), /failed/);
    srv.close();
});

t.test("a head that never ends is refused at 64 KiB without waiting for the peer", async (a) => {
    // The peer keeps the connection open, so only the size limit can end this.
    const srv = rawPeer("HTTP/1.1 200 OK\r\nX-Pad: " + "p".repeat(70000), true);
    const first = await within(message(fetch("http://127.0.0.1:" + srv.port + "/")), 4000, "STILL WAITING after 4 s");
    a.match(first, /failed/);
    srv.close();
}, { timeoutMs: 15000 });

t.test("conflicting framing is refused", async (a) => {
    for (const head of ["Content-Length: 2\r\nContent-Length: 3", "Content-Length: 2\r\nTransfer-Encoding: chunked", "Content-Length: x"]) {
        // The body satisfies either declared length, so only the framing check can refuse it.
        const srv = rawPeer("HTTP/1.1 200 OK\r\n" + head + "\r\n\r\nokay");
        a.match(await message(fetch("http://127.0.0.1:" + srv.port + "/")), /failed/, head);
        srv.close();
    }
});

t.test("a peer that stops answering is abandoned at the timeout", async (a) => {
    const srv = rawPeer("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", true);
    const first = await within(message(fetch("http://127.0.0.1:" + srv.port + "/", { timeout: 300 })), 5000, "STILL WAITING after 5 s");
    a.match(first, /failed/, "rejected, not left hanging");
    srv.close();
}, { timeoutMs: 15000 });

t.test("a refused connection and an unknown host each say which", async (a) => {
    a.match(await message(fetch("http://127.0.0.1:1/")), /failed/);
    a.match(await message(fetch("http://no-such-host.invalid/")), /failed/);
}, { timeoutMs: 30000 });

t.test("WebSocket handshakes parked on a silent peer hold no pool thread", async (a) => {
    const held = [];
    const mute = new TCPServer({ port: 0 });
    mute.start({ connect(conn) { held.push(conn); } });
    const closes = [];
    const clients = [];
    for (let i = 0; i < 12; i++) clients.push(new WsClient("ws://127.0.0.1:" + mute.port + "/", { close: (w, code, reason) => closes.push(reason) }));
    for (let w = 0; w < 100 && held.length < 12; w++) await sleep(20);
    a.eq(held.length, 12, "12 handshakes are waiting");
    let looked = false;
    const probe = TCPServer.connect({ host: "localhost", port: app.port }, { connect: (conn) => { looked = true; if (conn) conn.close(); } });
    for (let w = 0; w < 100 && !looked; w++) await sleep(20);
    a.eq(looked, true, "a name lookup still gets a pool thread");
    for (const c of held) c.close();
    for (let w = 0; w < 100 && closes.length < 12; w++) await sleep(20);
    a.eq(closes.length, 12, "every client reports the failed handshake");
    a.match(closes[0], /failed/);
    mute.close();
    probe.close();
}, { timeoutMs: 30000 });

t.test("a large frame sent right behind the 101 reaches the client whole", async (a) => {
    const wsapp = new App({ port: 0 });
    const BIGMSG = "m".repeat(200000);
    wsapp.ws("/w", { open(sock) { sock.send(BIGMSG); sock.send("tail"); } });
    wsapp.start();
    const got = await new Promise((resolve) => {
        const msgs = [];
        const timer = setTimeout(() => resolve(["TIMEOUT", msgs.length]), 8000);
        const c = new WsClient("ws://127.0.0.1:" + wsapp.port + "/w", {
            message(w, data) { msgs.push(data); if (msgs.length === 2) { clearTimeout(timer); w.close(); resolve(msgs); } },
        });
    });
    wsapp.close();
    a.eq(got.length, 2);
    a.eq(got[0].length, 200000);
    a.eq(got[1], "tail");
}, { timeoutMs: 20000 });

t.test("a frame in the same segment as the 101, larger than the handshake buffer, is kept", async (a) => {
    // One write carries the 101 and a 40000-byte binary frame, so the client's
    // first read holds more than its 16 KiB handshake buffer can.
    // SHA-1 and base64 written out here so the suite needs no module beyond dyna:net
    // (a per-module slim build carries nothing else).
    const sha1b64 = (text) => {
        const rotl = (x, n) => ((x << n) | (x >>> (32 - n))) >>> 0;
        const msg = [];
        for (let i = 0; i < text.length; i++) msg.push(text.charCodeAt(i) & 255);
        const bits = msg.length * 8;
        msg.push(0x80);
        while (msg.length % 64 !== 56) msg.push(0);
        for (let i = 7; i >= 0; i--) msg.push(i >= 4 ? 0 : (bits >>> (i * 8)) & 255);
        let h = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
        for (let off = 0; off < msg.length; off += 64) {
            const w = [];
            for (let i = 0; i < 16; i++) w[i] = ((msg[off + 4 * i] << 24) | (msg[off + 4 * i + 1] << 16) | (msg[off + 4 * i + 2] << 8) | msg[off + 4 * i + 3]) >>> 0;
            for (let i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            let [a0, b0, c0, d0, e0] = h;
            for (let i = 0; i < 80; i++) {
                const f = i < 20 ? ((b0 & c0) | (~b0 & d0)) : i < 40 ? (b0 ^ c0 ^ d0) : i < 60 ? ((b0 & c0) | (b0 & d0) | (c0 & d0)) : (b0 ^ c0 ^ d0);
                const k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
                const tmp = (rotl(a0, 5) + (f >>> 0) + e0 + k + w[i]) >>> 0;
                e0 = d0; d0 = c0; c0 = rotl(b0, 30); b0 = a0; a0 = tmp;
            }
            h = [(h[0] + a0) >>> 0, (h[1] + b0) >>> 0, (h[2] + c0) >>> 0, (h[3] + d0) >>> 0, (h[4] + e0) >>> 0];
        }
        const out = [];
        for (const v of h) out.push(v >>> 24, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);
        const A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        let b64 = "";
        for (let i = 0; i < out.length; i += 3) {
            const n = (out[i] << 16) | ((out[i + 1] || 0) << 8) | (out[i + 2] || 0);
            b64 += A[(n >> 18) & 63] + A[(n >> 12) & 63] + (i + 1 < out.length ? A[(n >> 6) & 63] : "=") + (i + 2 < out.length ? A[n & 63] : "=");
        }
        return b64;
    };
    const srv = new TCPServer({ port: 0 });
    srv.start({
        data(conn, b) {
            const head = String.fromCharCode.apply(null, b);
            const key = /sec-websocket-key: (\S+)/i.exec(head)[1];
            const accept = sha1b64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
            const reply = bytes("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
            const payload = new Uint8Array(40000);
            for (let i = 0; i < payload.length; i++) payload[i] = i % 251;
            const out = new Uint8Array(reply.length + 4 + payload.length);
            out.set(reply);
            out.set([0x82, 126, payload.length >> 8, payload.length & 255], reply.length);
            out.set(payload, reply.length + 4);
            conn.write(out);
        },
    });
    const got = await new Promise((resolve) => {
        const timer = setTimeout(() => resolve("TIMEOUT"), 6000);
        const c = new WsClient("ws://127.0.0.1:" + srv.port + "/", {
            message(w, data) { clearTimeout(timer); resolve(data); w.close(); },
            close(w, code, reason) { clearTimeout(timer); resolve("CLOSED " + reason); },
        });
    });
    srv.close();
    a.ok(got instanceof Uint8Array, "a binary message arrived: " + (got instanceof Uint8Array ? got.length + " bytes" : got));
    if (got instanceof Uint8Array) {
        a.eq(got.length, 40000);
        let same = true;
        for (let i = 0; i < got.length; i++) if (got[i] !== i % 251) { same = false; break; }
        a.ok(same, "byte for byte");
    }
}, { timeoutMs: 20000 });

t.test("a script whose only work was a fetch exits on its own", async (a) => {
    // the child talks to this process's App, so the parent keeps serving while it runs
    const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_hcr.${os.getpid()}.js`;
    const f = std.open(T, "w");
    f.puts(`import * as std from "std";\nconst r = await fetch("${B}/n/3"); print("GOT " + (await r.text()));\n`);
    f.close();
    const { Spawn } = await import("dyna:sys");
    const child = new Spawn(cwd() + "/dynajs", ["--std", T]);
    const buf = new Uint8Array(256);
    const n = await child.stdout.read(buf);
    a.eq(String.fromCharCode.apply(null, buf.subarray(0, n)).trim(), "GOT xxx");
    const res = await within(child.wait(), 8000, "HUNG");
    a.ne(res, "HUNG", "the child exited without being killed");
    if (res === "HUNG") child.kill("SIGKILL");
    child.close();
    os.remove(T);
}, { timeoutMs: 20000 });

t.test("two Workers fetching at once both finish", async (a) => {
    const W = `${std.getenv("TMPDIR") || "/tmp"}/dj_hcr_w.${os.getpid()}.js`;
    const f = std.open(W, "w");
    f.puts(`import * as os from "os";\nlet n = 0, settled = 0;\nfor (let i = 0; i < 20; i++) fetch("${B}/n/1000").then((r) => r.text()).then((s) => { if (s.length === 1000) n++; if (++settled === 20) os.Worker.parent.postMessage(n); }, (e) => os.Worker.parent.postMessage(String(e)));\n`);
    f.close();
    const out = [];
    const got = await within(new Promise((resolve) => {
        for (let k = 0; k < 2; k++) { const w = new os.Worker(W); w.onmessage = (e) => { out.push(e.data); w.onmessage = null; if (out.length === 2) resolve(out); }; }
    }), 15000, ["TIMEOUT", out]);
    os.remove(W);
    a.deep(got, [20, 20]);
}, { timeoutMs: 30000 });

await t.run();
