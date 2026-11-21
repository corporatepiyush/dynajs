// covers: dyna:wireguard.WireGuard
// test_wireguard_wire_guard_tunnel.js -- two dyna:wireguard devices in one process over 127.0.0.1 UDP (S13e, 2026-11-27):
// A knows B's endpoint, B learns A's from the first authenticated packet. TCP dial/listen through the tunnel with
// WHATWG streams (2 MiB echoed, a slow reader pushes back through the TCP window, FIN both ways, 16 concurrent
// connections), UDP echo, a refused port, a dial with no route, close() from inside a listen handler and inside a
// data callback, pending promises rejected by close(), the pool doing the checksums (checksumsVerified > 0).
import { suite } from "../lib/kit.js";
import { WireGuard } from "dyna:wireguard";

const t = suite("test_wireguard_wire_guard_tunnel");
let A, B, KA, KB;
const open = [];

async function pair(extraA, extraB) {
    const ka = await WireGuard.generateKeyPair(), kb = await WireGuard.generateKeyPair();
    const b = await WireGuard.create({ privateKey: kb.privateKey, address: ["10.77.0.2/24"], peers: [{ publicKey: ka.publicKey, allowedIPs: ["10.77.0.1/32"] }], ...extraB });
    const a = await WireGuard.create({ privateKey: ka.privateKey, address: ["10.77.0.1/24"], peers: [{ publicKey: kb.publicKey, allowedIPs: ["10.77.0.0/24"], endpoint: "127.0.0.1:" + b.listenPort }], ...extraA });
    open.push(a, b);
    return [a, b, ka, kb];
}

function pattern(n, seed) {
    const u = new Uint8Array(n);
    let x = seed >>> 0 || 1;
    for (let i = 0; i < n; i++) { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; u[i] = x & 255; }
    return u;
}

async function readAll(rs, limit) {
    const r = rs.getReader(), parts = [];
    let n = 0;
    for (;;) {
        const { value, done } = await r.read();
        if (done) break;
        parts.push(value);
        n += value.length;
        if (limit && n >= limit) break;
    }
    r.releaseLock();
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
}

function same(x, y) {
    if (x.length !== y.length) return false;
    for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false;
    return true;
}

function echoServer(w, port) {
    return w.listen(port, (c) => { c.readable.pipeTo(c.writable).catch(() => {}); });
}

t.before(async () => { [A, B, KA, KB] = await pair(); });
t.after(async () => { for (const w of open) w.close(); });

t.test("TCP echo of 2 MiB through the tunnel, FIN both ways", async (a) => {
    const l = echoServer(B, 7);
    a.ok(l.port === 7);
    const c = await A.dial("10.77.0.2", 7);
    a.eq(c.remoteAddress, "10.77.0.2");
    a.eq(c.remotePort, 7);
    a.eq(c.localAddress, "10.77.0.1");
    a.ok(c.localPort >= 1024, "ephemeral local port");
    const data = pattern(2 << 20, 7);
    const wr = c.writable.getWriter();
    const reading = readAll(c.readable);
    for (let off = 0; off < data.length; off += 65536) await wr.write(data.subarray(off, off + 65536));
    await wr.close();
    const got = await reading;
    a.eq(got.length, data.length, "byte count");
    a.ok(same(got, data), "bytes identical");
    l.close();
    const s = B.stats();
    a.ok(s.checksumsVerified > 100, "inner checksums verified on the pool: " + s.checksumsVerified);
    a.ok(A.stats().handshakesCompleted + s.handshakesCompleted >= 1 && A.stats().handshakesSent >= 1, "handshake");
    a.ok(s.poolJobs > s.inlineJobs, "bulk crypto on the pool: " + s.poolJobs + " jobs vs " + s.inlineJobs + " lone small packets inline");
}, { timeoutMs: 30000 });

t.test("slow reader: backpressure through the TCP window, nothing lost", async (a) => {
    const N = 1 << 20;
    let served;
    const done = new Promise((res) => { served = res; });
    const l = B.listen(8, async (c) => {
        const wr = c.writable.getWriter();
        const data = pattern(N, 8);
        for (let off = 0; off < N; off += 16384) await wr.write(data.subarray(off, off + 16384));
        await wr.close();
        served();
    });
    const c = await A.dial("10.77.0.2", 8);
    const r = c.readable.getReader();
    const first = await r.read();
    await new Promise((res) => setTimeout(res, 300));
    const parts = [first.value];
    let n = first.value.length;
    for (;;) {
        const { value, done: d } = await r.read();
        if (d) break;
        parts.push(value);
        n += value.length;
    }
    await done;
    const got = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { got.set(p, o); o += p.length; }
    a.ok(same(got, pattern(N, 8)), "1 MiB after a 300 ms stall, identical");
    l.close();
}, { timeoutMs: 30000 });

t.test("16 concurrent connections", async (a) => {
    const l = echoServer(B, 9);
    const one = async (i) => {
        const c = await A.dial("10.77.0.2", 9);
        const d = pattern(65536 + i * 977, 100 + i);
        const reading = readAll(c.readable);
        const wr = c.writable.getWriter();
        await wr.write(d);
        await wr.close();
        return same(await reading, d);
    };
    const res = await Promise.all(Array.from({ length: 16 }, (_, i) => one(i)));
    a.ok(res.every(Boolean), "every stream echoed intact");
    l.close();
}, { timeoutMs: 30000 });

t.test("UDP echo, both directions", async (a) => {
    const ub = B.udp(53, (d, host, port) => ub.send(d, host, port));
    let got;
    const p = new Promise((res) => { got = res; });
    const ua = A.udp(0, (d, host, port) => got({ d, host, port }));
    a.ok(ua.port > 0, "ephemeral port");
    a.eq(ua.send("hello", "10.77.0.2", 53), true);
    const r = await p;
    a.eq(new TextDecoder().decode(r.d), "hello");
    a.eq(r.host, "10.77.0.2");
    a.eq(r.port, 53);
    a.throws(() => ua.send(new Uint8Array(65508), "10.77.0.2", 53), RangeError);
    a.throws(() => ua.send("x", "nope", 53), TypeError);
    a.throws(() => ua.send("x", "fd00::2", 53), { type: TypeError, message: /no IPv6 address configured/ });
    a.throws(() => B.udp(53, () => {}), { type: TypeError, message: /port 53 is in use/ });
    ua.close();
    ub.close();
    a.throws(() => ua.send("x", "10.77.0.2", 53), { type: TypeError, message: /socket closed/ });
}, { timeoutMs: 10000 });

t.test("refused port, no route, bad arguments", async (a) => {
    await a.rejects(A.dial("10.77.0.2", 9999), { type: Error, message: "WireGuard: connect: connection refused" });
    await a.rejects(A.dial("10.99.0.9", 80), { type: Error, message: /^WireGuard: connect: no route to host/ });
    a.throws(() => A.dial("host.test", 80), { type: TypeError, message: /not an IP literal/ });
    a.throws(() => A.dial("10.77.0.2", 0), TypeError);
    a.throws(() => A.listen(80), { type: TypeError, message: /handler must be a function/ });
    const l = B.listen(10, () => {});
    a.throws(() => B.listen(10, () => {}), { type: TypeError, message: /port 10 is in use/ });
    l.close();
    l.close();
}, { timeoutMs: 20000 });

t.test("conn.close() and readable cancel reset the connection", async (a) => {
    let sv;
    const got = new Promise((res) => { sv = res; });
    const l = B.listen(11, (c) => { readAll(c.readable).then(() => sv("eof"), (e) => sv(e.message)); });
    const c = await A.dial("10.77.0.2", 11);
    c.close();
    a.match(await got, /connection reset|eof/);
    await a.rejects(c.writable.getWriter().write(new Uint8Array(1)));
    l.close();
}, { timeoutMs: 10000 });

t.test("close() inside a listen handler and inside a data callback", async (a) => {
    const [a2, b2] = await pair();
    b2.listen(12, () => { b2.close(); });
    const c = await a2.dial("10.77.0.2", 12).catch((e) => e);
    for (let i = 0; i < 100 && !b2.closed; i++) await new Promise((res) => setTimeout(res, 10));
    a.ok(b2.closed, "closed from inside the handler");
    if (!(c instanceof Error)) c.close();
    const [a3, b3] = await pair();
    let fired = false;
    b3.listen(13, (sc) => {
        const r = sc.readable.getReader();
        r.read().then(() => { fired = true; b3.close(); });
    });
    const c3 = await a3.dial("10.77.0.2", 13);
    await c3.writable.getWriter().write("x");
    for (let i = 0; i < 100 && !b3.closed; i++) await new Promise((res) => setTimeout(res, 10));
    a.ok(fired && b3.closed, "closed from inside a read continuation");
    a3.close();
    a2.close();
}, { timeoutMs: 20000 });

t.test("close() rejects pending promises", async (a) => {
    const [a4] = await pair();
    const p = a4.dial("10.77.0.2", 14);
    a4.close();
    await a.rejects(p, { type: Error, message: /WireGuard: (connect: )?closed/ });
    a.throws(() => a4.dial("10.77.0.2", 14), { type: TypeError, message: "WireGuard: closed" });
}, { timeoutMs: 10000 });

await t.run();
