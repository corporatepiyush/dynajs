// timeout: 60
// covers: c:src/modules/net-tcp/net-tcp-connection.c
// TCPConn pause()/resume() and the WHATWG adapter conn.streams(). pause() cancels the connection's receive (no data event, no bytes read
// from the kernel, so the peer's sends back up); resume() re-arms it and, on TLS, first delivers plaintext already decrypted. streams()
// routes data/drain/EOF/close of ONE connection into a byte ReadableStream (enqueue, then pause once desiredSize <= 0; a pull resumes)
// and a WritableStream (writes split to min(writableHighWaterMark, the connection's highWaterMark) and wait for drain past it; close()
// waits for the queue to empty, then half-closes with shutdown(SHUT_WR)). With streams attached a peer FIN closes only the readable,
// so an echo can finish writing; the connection closes once both sides are done. A paused connection is exempt from idleTimeoutMs.
// Callbacks nest (a data handler or stream tap calls conn.close(), whose close handler calls server.close()): the server's in-callback
// mark is a depth, and teardown waits for the outermost frame (it used to run inside the nested frame and the outer frame then wrote
// to the freed server: a heap use-after-free under ASan, reachable without streams too).
import { suite } from "../lib/kit.js";
import { TCPServer } from "dyna:net";
import * as crypto from "dyna:crypto";
import { makeTempDir, writeFile, Path, removeAll } from "dyna:file";

const t = suite("tcp_streams");
const URING = await import("dyna:uring").then(() => true, () => false);
const PAUSE_SKIP = "pause() is a no-op on the io_uring backend (dynajs.d.ts, TCPConnection.pause): no read backpressure there";
const T = { timeoutMs: 15000 };
const tick = (ms = 0) => new Promise((r) => setTimeout(r, ms));
function pattern(n, seed = 0) { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (i * 131 + seed + (i >> 11)) & 255; return u; }
function checkPattern(a, parts, n, seed = 0) {
    let k = 0, bad = -1;
    for (const p of parts) { for (let i = 0; i < p.length; i++, k++) if (bad < 0 && p[i] !== ((k * 131 + seed + (k >> 11)) & 255)) bad = k; }
    a.eq(k, n, "byte count");
    a.eq(bad, -1, "first corrupt byte offset");
}
function pair(srvOpts = {}, srvHandlers = {}, cliOpts = {}) {
    return new Promise((resolve, reject) => {
        const srv = new TCPServer({ port: 0, ...srvOpts });
        let sc, cc;
        const done = () => { if (sc && cc) resolve({ srv, sc, cc }); };
        srv.start({ ...srvHandlers, connect(c) { sc = c; if (srvHandlers.connect) srvHandlers.connect(c); done(); } });
        const { onClose, ...copts } = cliOpts;
        const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port, ...copts }, {
            connect(c, err) { if (err) { reject(new Error(err)); return; } cc = c; done(); },
            close: onClose,
        });
        void cli;
    });
}
async function drain(rs, parts, slow) {
    let n = 0;
    for await (const c of rs) { parts.push(c); n += c.length; if (slow) await tick(slow); }
    return n;
}

t.test("pause() stops data events and resume() delivers what the peer sent meanwhile; both idempotent", async (a) => {
    if (URING) return a.skip(PAUSE_SKIP);
    let got = 0, events = 0;
    const { srv, sc, cc } = await pair({}, { data(c, b) { got += b.length; events++; } });
    a.eq(sc.paused, false);
    sc.pause(); sc.pause();
    a.eq(sc.paused, true);
    cc.write(pattern(100000));
    await tick(150);
    a.eq(events, 0, "no data event while paused");
    sc.resume(); sc.resume();
    a.eq(sc.paused, false);
    for (let i = 0; i < 100 && got < 100000; i++) await tick(10);
    a.eq(got, 100000, "everything arrives after resume");
    sc.close();
    sc.pause(); sc.resume();
    a.eq(sc.paused, false, "a closed connection reports not paused");
    srv.close();
}, T);

t.test("streams(): a 2 MiB echo through pipeTo, half-close ends it, bytes intact", async (a) => {
    let echoDone;
    const echoed = new Promise((r) => { echoDone = r; });
    const { srv, cc } = await pair({}, {
        connect(c) { const { readable, writable } = c.streams({ allowHalfOpen: true }); readable.pipeTo(writable).then(() => echoDone("ok"), (e) => echoDone(e.message)); },
    });
    const { readable, writable } = cc.streams();
    const N = 2 << 20, src = pattern(N);
    const w = writable.getWriter();
    const wp = (async () => { for (let o = 0; o < N; o += 50000) await w.write(src.subarray(o, Math.min(N, o + 50000))); await w.close(); })();
    const parts = [];
    await drain(readable, parts);
    await wp;
    checkPattern(a, parts, N);
    a.eq(await echoed, "ok", "server side pipe finished cleanly after the client's FIN");
    srv.close();
}, T);

t.test("backpressure: a reader that stops makes the connection pause and the remote writer wait", async (a) => {
    if (URING) return a.skip(PAUSE_SKIP);
    let sw, swritten = 0;
    const { srv, cc } = await pair({}, {
        connect(c) {
            const { writable } = c.streams();
            sw = writable.getWriter();
            (async () => { const chunk = pattern(65536); for (let i = 0; i < 512; i++) { await sw.write(chunk); swritten += chunk.length; } await sw.close(); })().catch(() => {});
        },
    });
    const { readable } = cc.streams({ readableHighWaterMark: 65536 });
    const r = readable.getReader();
    const first = await r.read();
    a.ok(first.value.length > 0, "first chunk");
    await tick(300);
    a.eq(cc.paused, true, "the reading connection paused itself");
    const stuck = swritten;
    a.ok(stuck < 32 << 20, "the writer is held back (wrote " + stuck + " of 33554432)");
    a.ok(sw.desiredSize <= 0, "the remote writer reports backpressure");
    let n = first.value.length;
    for (;;) { const x = await r.read(); if (x.done) break; n += x.value.length; }
    a.eq(n, 512 * 65536, "all bytes arrive once reading resumes");
    a.ok(swritten > stuck, "the writer progressed after the reader resumed");
    srv.close();
}, T);

t.test("writes larger than the connection's highWaterMark are split, never refused", async (a) => {
    const parts = [];
    let doneR;
    const done = new Promise((r) => { doneR = r; });
    const { srv, cc } = await pair({}, { connect(c) { drain(c.streams().readable, parts).then(doneR); } }, { highWaterMark: 4096 });
    const { writable } = cc.streams({ writableHighWaterMark: 1 << 20 });
    const w = writable.getWriter();
    const N = 1 << 20;
    await w.write(pattern(N, 5));
    await w.close();
    await done;
    checkPattern(a, parts, N, 5);
    srv.close();
}, T);

t.test("data goes to the streams, not the data handler; with allowHalfOpen a peer FIN ends only the readable; close fires once both are done", async (a) => {
    let dataEvents = 0, closes = 0;
    const { srv, sc, cc } = await pair({}, { data() { dataEvents++; }, close() { closes++; } });
    const { readable, writable } = sc.streams({ allowHalfOpen: true });
    a.throws(() => sc.streams(), /already called/);
    cc.write("hello");
    const r = readable.getReader();
    const x = await r.read();
    a.eq(new TextDecoder().decode(x.value), "hello");
    a.eq(dataEvents, 0, "the data handler is bypassed");
    cc.close();
    const end = await r.read();
    a.eq(end.done, true, "peer close ends the readable");
    await tick(50);
    a.eq(closes, 0, "the connection stays open for writing after the peer's FIN");
    await writable.close();
    for (let i = 0; i < 50 && closes === 0; i++) await tick(10);
    a.eq(closes, 1, "the server close handler still fires");
    a.throws(() => sc.streams(), /closed/);
    srv.close();
}, T);

t.test("without allowHalfOpen a peer FIN closes the connection once the queue drains; later writes fail", async (a) => {
    let closes = 0;
    const { srv, sc, cc } = await pair({}, { close() { closes++; } });
    const { readable, writable } = sc.streams();
    const r = readable.getReader();
    cc.close();
    a.eq((await r.read()).done, true, "the readable closes");
    for (let i = 0; i < 50 && closes === 0; i++) await tick(10);
    a.eq(closes, 1, "the connection closed by itself");
    await a.rejects(writable.getWriter().write("late"), /connection closed/);
    srv.close();
}, T);

t.test("writing to a peer that closed fails the writable and closes the connection", async (a) => {
    let closes = 0;
    const { srv, sc, cc } = await pair({}, { close() { closes++; } });
    const { readable, writable } = sc.streams({ allowHalfOpen: true });
    const w = writable.getWriter();
    const r = readable.getReader();
    const rp = r.read();
    cc.close();
    a.eq((await rp).done, true, "the readable closes");
    let err;
    for (let i = 0; i < 50 && !err; i++) { try { await w.write(pattern(1000)); } catch (e) { err = e; } await tick(10); }
    a.match(err && err.message, /connection closed/, "a write fails once the peer has reset");
    for (let i = 0; i < 50 && closes === 0; i++) await tick(10);
    a.eq(closes, 1, "the failed send closed the connection");
    srv.close();
}, T);

t.test("cancel the readable and close the writable: the connection closes", async (a) => {
    let peerClosed = false;
    const { srv, sc } = await pair({}, {}, { onClose() { peerClosed = true; } });
    const { readable, writable } = sc.streams();
    await readable.cancel();
    const w = writable.getWriter();
    await w.write("bye");
    await w.close();
    for (let i = 0; i < 100 && !peerClosed; i++) await tick(10);
    a.ok(peerClosed, "the client saw the close");
    srv.close();
}, T);

t.test("a paused connection is exempt from idleTimeoutMs", async (a) => {
    if (URING) return a.skip(PAUSE_SKIP);
    let closes = 0;
    const { srv, sc, cc } = await pair({ idleTimeoutMs: 150 }, { close() { closes++; } });
    const { readable } = sc.streams({ readableHighWaterMark: 1 });
    cc.write(pattern(200000, 9));
    await tick(600);
    a.eq(sc.paused, true, "paused by the full queue");
    a.eq(closes, 0, "not idle-closed while paused");
    const r = readable.getReader();
    const parts = [];
    let n = 0;
    while (n < 200000) { const x = await r.read(); parts.push(x.value); n += x.value.length; }
    checkPattern(a, parts, 200000, 9);
    srv.close();
}, T);

t.test("nested close: conn.close() in a data handler or a stream tap, then server.close() in the close handler", async (a) => {
    for (const mode of ["handler", "streams"]) {
        let closed;
        const done = new Promise((r) => { closed = r; });
        const srv = new TCPServer({ port: 0 });
        srv.start({
            connect(c) { if (mode === "streams") { const { readable, writable } = c.streams({ allowHalfOpen: true }); writable.close(); readable.getReader().read(); } },
            data(c) { c.close(); },
            close() { srv.close(); closed(true); },
        });
        TCPServer.connect({ host: "127.0.0.1", port: srv.port }, { connect(c) { if (mode === "streams") c.close(); else c.write("x"); } });
        a.eq(await done, true, mode + ": the close handler ran");
        await tick(20);
        a.eq(srv.closed, true, mode + ": the server tore down after the outer callback returned");
    }
}, T);

t.test("TLS: pause() inside a data event stops the record loop until resume()", async (a) => {
    if (URING) return a.skip(PAUSE_SKIP);
    const dir = makeTempDir("tcpstr");
    const k = crypto.RSA.generate(2048);
    writeFile(new Path(dir, "k.pem"), k.privateKey);
    writeFile(new Path(dir, "c.pem"), crypto.X509.generateSelfSigned({ key: k.privateKey, subject: "localhost", days: 2 }));
    let events = 0, bytes = 0, sc;
    const srv = new TCPServer({ port: 0, tls: { cert: dir + "/c.pem", key: dir + "/k.pem" } });
    srv.start({ connect(c) { sc = c; }, data(c, b) { events++; bytes += b.length; if (events === 1) c.pause(); } });
    const N = 200000;
    await new Promise((resolve, reject) => {
        TCPServer.connect({ host: "localhost", port: srv.port, tls: { ca: dir + "/c.pem" } }, {
            connect(c, err) { if (err) { reject(new Error(err)); return; } c.write(pattern(N)); resolve(); },
        });
    });
    for (let i = 0; i < 200 && events === 0; i++) await tick(5);
    await tick(200);
    a.eq(events, 1, "no further data event while paused (the record loop stopped)");
    sc.resume();
    for (let i = 0; i < 200 && bytes < N; i++) await tick(10);
    a.eq(bytes, N, "the rest arrives after resume");
    srv.close();
    removeAll(dir);
}, T);

t.test("TLS: backpressure pauses and resume drains decrypted plaintext first", async (a) => {
    if (URING) return a.skip(PAUSE_SKIP);
    const dir = makeTempDir("tcpstr");
    const k = crypto.RSA.generate(2048);
    writeFile(new Path(dir, "k.pem"), k.privateKey);
    writeFile(new Path(dir, "c.pem"), crypto.X509.generateSelfSigned({ key: k.privateKey, subject: "localhost", days: 2 }));
    const N = 2 << 20;
    let srvBytes;
    const got = new Promise((res) => { srvBytes = res; });
    const srv = new TCPServer({ port: 0, tls: { cert: dir + "/c.pem", key: dir + "/k.pem" } });
    srv.start({
        connect(c) {
            const { readable } = c.streams({ readableHighWaterMark: 1024 });
            const parts = [];
            (async () => { const r = readable.getReader(); let n = 0; await tick(200); a.eq(c.paused, true, "the TLS connection paused"); for (;;) { const x = await r.read(); if (x.done) break; parts.push(x.value); n += x.value.length; await tick(0); } srvBytes(parts); })();
        },
    });
    await new Promise((resolve, reject) => {
        TCPServer.connect({ host: "localhost", port: srv.port, tls: { ca: dir + "/c.pem" } }, {
            async connect(c, err) {
                if (err) { reject(new Error(err)); return; }
                const w = c.streams().writable.getWriter();
                const src = pattern(N, 3);
                for (let o = 0; o < N; o += 16000) await w.write(src.subarray(o, Math.min(N, o + 16000)));
                await w.close();
                resolve();
            },
        });
    });
    checkPattern(a, await got, N, 3);
    srv.close();
    removeAll(dir);
}, T);

await t.run();
