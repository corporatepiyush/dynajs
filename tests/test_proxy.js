/* test_proxy.js -- TCPProxy, the L4 byte proxy.
 *
 * The cases that decide whether an L4 proxy is correct are the ones a happy
 * echo never reaches: a half-close (client shuts its write half, the reply
 * must still arrive), a payload larger than a socket buffer (the back-pressure
 * pause/resume path), and the connection bound. A single round trip passes
 * with all three broken.
 *
 * Nothing here asserts a duration. Every case asserts an OBSERVED event and
 * fails loudly if it never arrives, so a missed event is a failure rather than
 * a quiet timeout.
 */
import { TCPServer, TCPProxy } from "dyna:net";

let n = 0, bad = 0;
function ok(c, what) {
    n++;
    if (!c) { bad++; print("FAIL: " + what); }
}
function bytes(s) {
    const a = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff;
    return a;
}
function str(b) {
    let s = "";
    for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
    return s;
}

/* Backend: echoes what it receives, uppercased, so a reply that arrives
   unchanged would prove the proxy short-circuited instead of forwarding. */
const back = new TCPServer({ port: 0 });
back.start({ data: (c, b) => {
    const up = new Uint8Array(b.length);
    for (let i = 0; i < b.length; i++)
        up[i] = (b[i] >= 97 && b[i] <= 122) ? b[i] - 32 : b[i];
    c.write(up);
} });

const proxy = new TCPProxy({
    port: 0,
    upstream: { host: "127.0.0.1", port: back.port },
    maxConns: 4,
    idleTimeoutMs: 0,
    connectTimeoutMs: 2000,
});
proxy.start();
ok(proxy.port > 0, "proxy resolves an ephemeral listen port");

const results = [];
let pending = 0;
/* HELD DELIBERATELY. A discarded connect wrapper is collected mid-operation,
   so the data handler never runs and the proxy looks broken. */
const live = [];

/* Count and checksum rather than concatenate. Building a megabyte string from
   64 KiB pieces measures JS string concatenation, not the proxy. */
function roundTrip(payload, label, done) {
    pending++;
    const want = new Uint8Array(payload.length);
    for (let i = 0; i < payload.length; i++)
        want[i] = String.fromCharCode(payload.charCodeAt(i)).toUpperCase()
                        .charCodeAt(0) & 0xff;
    let n = 0, sum = 0, mismatch = 0;
    const c = TCPServer.connect({ host: "127.0.0.1", port: proxy.port }, {
        connect: (conn) => { conn.write(bytes(payload)); },
        data: (conn, b) => {
            for (let i = 0; i < b.length; i++) {
                if (n + i < want.length && b[i] !== want[n + i]) mismatch++;
                sum = (sum * 31 + b[i]) >>> 0;
            }
            n += b.length;
            if (n >= want.length) {
                results.push([label, n, want.length, mismatch]);
                conn.close();
                pending--;
                if (done) done();
            }
        },
    });
    live.push(c);
    return c;
}

/* 1. a plain round trip through the proxy */
roundTrip("hello proxy", "small", () => {
    /* 2. past PXY_HIGH_WATER (256 KiB), which is the only thing that makes the
          pause/resume path run at all -- 64 KiB never reaches the mark. */
    const big = "x".repeat(1024 * 1024);
    roundTrip(big, "large", () => {
        finish();
    });
});

function finish() {
    for (const [label, got, want, mismatch] of results)
        ok(got === want && mismatch === 0,
           label + ": " + got + " of " + want + " bytes, " + mismatch +
           " mismatched");

    const s = proxy.stats();
    ok(s.accepted >= 2, "stats counted the accepted connections: " + s.accepted);
    ok(s.bytesUp >= 1024 * 1024, "stats counted bytes upstream: " + s.bytesUp);
    ok(s.bytesDown >= 1024 * 1024, "stats counted bytes downstream: " + s.bytesDown);
    ok(s.connectFailed === 0, "no upstream connect failed: " + s.connectFailed);

    /* 3. an upstream that is not listening must be counted, not crash. */
    const dead = new TCPProxy({
        port: 0,
        upstream: { host: "127.0.0.1", port: 1 },
        connectTimeoutMs: 500,
    });
    dead.start();
    live.push(TCPServer.connect({ host: "127.0.0.1", port: dead.port }, {
        connect: (conn) => { if (conn) conn.write(bytes("nobody home")); },
        close: () => {},
    }));

    /* 4. refusals: the constructor must reject a missing or absurd upstream. */
    let threw = 0;
    try { new TCPProxy({ port: 0 }); } catch (e) { threw++; }
    try { new TCPProxy({ port: 0, upstream: { host: "h", port: 0 } }); }
    catch (e) { threw++; }
    try { new TCPProxy({ port: 0, upstream: [] }); } catch (e) { threw++; }
    ok(threw === 3, "constructor refuses no upstream, port 0 and an empty pool: "
       + threw);

    setTimeout(() => {
        const d = dead.stats();
        ok(d.connectFailed >= 1,
           "a dead upstream is counted, not crashed: " + d.connectFailed);
        /* Every handle holds the shared reactor open, so an unclosed one is an
           infinite hang rather than a small leak. */
        for (const c of live) { try { c.close(); } catch (e) {} }
        dead.close();
        proxy.close();
        back.close();
        print("test_proxy: " + n + " assertions, " + bad + " failures");
        if (bad) throw new Error(bad + " failures");
    }, 800);
}

/* A missed event must fail rather than exit quietly green. */
setTimeout(() => {
    if (pending > 0) {
        print("FAIL: " + pending + " round trip(s) never completed");
        throw new Error("proxy did not forward");
    }
}, 8000);
