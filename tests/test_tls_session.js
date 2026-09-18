/* test_tls_session.js -- client session resumption (audit E12-02).
 *
 * Five sequential TLS connections to the SAME server, same options. #1 is a
 * full handshake; #2..#5 MUST resume. Timing alone is host noise, so the
 * proof is the WIRE: a plaintext relay sits between client and server and
 * counts the server->client bytes per handshake. A full TLS 1.3 server
 * flight carries the certificate chain and a signature; a resumed one does
 * not -- the ratio is stable for a given certificate.
 *
 * Needs CONFIG_TLS=y. Run: make CONFIG_NATIVE_MODULES=y CONFIG_TLS=y test-tls-session
 */
import { TCPServer } from "dyna:net";
import { RSA, X509 } from "dyna:crypto";
import { makeTempDir, writeFile, removeAll, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

/* One round-trip; resolves {state} where "OK <payload>" is an echoed echo. */
function roundTrip(port, tlsOpts, payload) {
    return new Promise((resolve) => {
        let settled = false;
        const done = (v) => { if (!settled) { settled = true; resolve(v); } };
        const t = setTimeout(() => done("TIMEOUT"), 8000);
        const cli = TCPServer.connect({ host: "127.0.0.1", port, tls: tlsOpts }, {
            connect(c, err) {
                if (err) { clearTimeout(t); done("ERR " + err); cli.close(); return; }
                c.write(payload);
            },
            data(c, b) {
                clearTimeout(t);
                done("OK " + new TextDecoder().decode(b));
                cli.close();
            },
            close() {
                clearTimeout(t);
                if (!settled) { done("CLOSED"); cli.close(); }
            },
        });
    });
}

async function main() {
    const T = makeTempDir("tlssession");
    const P = (n) => T + "/" + n;

    const rsa = RSA.generate(2048);
    writeFile(new Path(P("srv.key")), rsa.privateKey);
    writeFile(new Path(P("srv.pem")), X509.generateSelfSigned(
        { key: rsa.privateKey, subject: "localhost", days: 30 }));

    const srv = new TCPServer({ port: 0, tls: { cert: P("srv.pem"), key: P("srv.key") } });
    srv.start({ data(c, b) { c.write(b); } });

    /* The byte-counting relay: client -> fwd -> srv, and back. Writes go to
     * the HANDLER-ARG conn (the wrapper TCPServer.connect() returns has no
     * working write, and dyna swallows the throw); the return value is kept,
     * because a discarded wrapper is collected mid-connect; and the upstream
     * is torn down with the downstream so each run owns exactly one pair. */
    const fwd = new TCPServer({ port: 0 });
    let downstream = null, upstream = null, upConn = null, upReady = false, srvBytes = 0;
    const pending = [];
    fwd.start({
        connect(c) {
            downstream = c;
            upConn = null; upReady = false;
            upstream = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
                connect(u, err) {
                    if (err) { c.close(); return; }
                    upConn = u;
                    upReady = true;
                    while (pending.length) upConn.write(pending.shift());
                },
                data(u, b) { srvBytes += b.length; if (downstream) downstream.write(b); },
            });
        },
        data(c, b) {
            if (upReady) upConn.write(b);
            else pending.push(b);
        },
        close() {
            if (upConn) upConn.close();
            upConn = null; upReady = false; pending.length = 0;
        },
    });

    print("--- 5 sequential handshakes to one server through the relay ---");
    {
        const opts = { servername: "localhost", ca: P("srv.pem") };
        const times = [], sizes = [];
        let runs = 0;
        for (let i = 0; i < 5; i++) {
            srvBytes = 0;
            const t0 = performance.now();
            /* A connect whose readiness event went missing on macOS stalls as
               a TIMEOUT (adapter-level, pre-existing; repo audit notes). A
               repeated attempt through the same relay is served normally. */
            let r;
            for (let att = 0; att < 4; att++) {
                r = await roundTrip(fwd.port, opts, "s" + i);
                if (r.state !== "TIMEOUT") break;
                await new Promise((res) => setTimeout(res, 150));
            }
            times.push(performance.now() - t0);
            if (!/^OK s/.test(String(r))) {
                ok(false, "run " + i + " failed: " + r);
                break;
            }
            runs++;
            await new Promise((res) => setTimeout(res, 120));  /* let tickets land */
            sizes.push(srvBytes);
        }
        ok(runs === 5, "5 sequential TLS handshakes completed");
        if (runs === 5) {
            print("  ..    handshake ms: " + times.map((t) => t.toFixed(2)).join(" "));
            print("  ..    server->client bytes: " + sizes.join(" "));
            ok(sizes[0] > 1000 && sizes[0] > 1.5 * sizes[4],
               "run 1 carried a FULL server flight (cert chain); the resumed " +
               "flights carry none (" + sizes[0] + "B vs " + sizes[4] + "B)");
            let resumed = true;
            for (let i = 1; i < 5; i++) if (sizes[i] >= sizes[0] * 0.75) resumed = false;
            ok(resumed, "handshakes 2..5 are RESUMPTIONS (no certificate on the wire)");
            const avg = (a) => a.reduce((x, y) => x + y, 0) / a.length;
            print("  ..    full=" + times[0].toFixed(2) + "ms resumed-avg=" +
                  avg(times.slice(1)).toFixed(2) + "ms");
        }
    }

    srv.close();
    fwd.close();
    removeAll(new Path(T));
    print("test_tls_session: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_tls_session: " + fail + " failures");
}

main().catch((e) => {
    print("test_tls_session: FATAL " + (e && e.message ? e.message : e));
    print("test_tls_session: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    throw e;
});
