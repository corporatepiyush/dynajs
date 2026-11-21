// timeout: 60
// covers: c:src/modules/net-tcp/net-tcp-connection.c
// v10b-26a: TCPConn.end() on TLS is a half-close (shutdown(SHUT_WR) once nothing is queued), the same as plain TCP. It used to
// close() the socket. A close() while the receive buffer holds unread bytes (here: plaintext the paused client has not read; under
// load: a TLS 1.3 session ticket that arrived after the last read) makes the kernel send RST instead of FIN, and the RST discards
// the ciphertext still in this side's send buffer and the peer's unread receive buffer, so the peer saw ECONNRESET and lost the
// tail of a stream whose writer.close() had resolved (test_tcp_streams "TLS: backpressure ..." failed ~4% of runs under load).
// The client below pauses with unread bytes (the server's 1000-byte write) and ends the moment its queue drains, while the server
// reads one chunk per millisecond, so the client's kernel send buffer still holds ciphertext: RST and a short count with the old
// close(), every byte then EOF with the half-close.
import { suite } from "../lib/kit.js";
import { TCPServer } from "dyna:net";
import * as crypto from "dyna:crypto";
import { makeTempDir, writeFile, Path, removeAll } from "dyna:file";

const t = suite("tcp_tls_end");
const T = { timeoutMs: 15000 };
const tick = (ms = 0) => new Promise((r) => setTimeout(r, ms));
function pattern(n, seed) { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (i * 131 + seed + (i >> 11)) & 255; return u; }

t.test("TLS end() with unread bytes in the receive buffer half-closes: the paused peer reads every byte, then EOF", async (a) => {
    const dir = makeTempDir("tlsend");
    const k = crypto.RSA.generate(2048);
    writeFile(new Path(dir, "k.pem"), k.privateKey);
    writeFile(new Path(dir, "c.pem"), crypto.X509.generateSelfSigned({ key: k.privateKey, subject: "localhost", days: 2 }));
    const N = 1 << 20;
    const src = pattern(N, 5);
    const got = [];
    let srvConn, ended, closedWith;
    const again = () => srvConn.resume();
    const srvDone = new Promise((res) => { closedWith = res; });
    const cliEnded = new Promise((res) => { ended = res; });
    const srv = new TCPServer({ port: 0, tls: { cert: dir + "/c.pem", key: dir + "/k.pem" } });
    srv.start({
        connect(c) { srvConn = c; c.write(new Uint8Array(1000).fill(9)); },
        data(c, b) { got.push(new Uint8Array(b)); c.pause(); setTimeout(again, 1); },
        close() { closedWith(); },
    });
    let endErr = null, cliConn;
    const cli = TCPServer.connect({ host: "localhost", port: srv.port, tls: { ca: dir + "/c.pem" } }, {
        connect(c, err) {
            if (err) { endErr = String(err); ended(); return; }
            cliConn = c;
            c.pause();
            c.write(src);
            if (c.bufferedAmount === 0) { c.end(); ended(); }
        },
        drain(c) { if (c.bufferedAmount === 0) { try { c.end(); } catch (e) { endErr = String(e); } ended(); } },
    });
    await cliEnded;
    a.eq(endErr, null, "client connected and ended");
    let guard;
    const closed = await Promise.race([srvDone.then(() => true), new Promise((r) => { guard = setTimeout(() => r(false), 5000); })]);
    clearTimeout(guard);
    a.eq(closed, true, "the server saw the client's end (EOF) and closed");
    let n = 0, bad = 0;
    for (const p of got) for (let i = 0; i < p.length; i++, n++) if (p[i] !== src[n]) bad++;
    a.eq(n, N, "the paused server read every byte the client wrote before end()");
    a.eq(bad, 0, "byte pattern");
    cliConn.close();
    srv.close();
    removeAll(dir);
}, T);

await t.run();
