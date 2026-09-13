/* bench_tls_ctx.js -- the numbers for audit E12-04 and E12-02.
 *
 * E12-04: a fetch with a CUSTOM CA used to build a throwaway ctx per connect,
 *   paying a full trust-store load every time. Here the "custom CA" is the
 *   vendored Mozilla bundle (tests/corpus/ca-bundle.pem, ~150 roots) plus the
 *   server's self-signed cert -- the same shape as a pinned corporate bundle,
 *   and big enough that the parse cost is visible over handshake noise.
 *   cold  = first connect with that CA (ctx build + handshake)
 *   warm  = median of the following connects (ctx cached: handshake only)
 *   Run this file BEFORE and AFTER the keyed cache: the before-warm number
 *   stays at the cold level (nothing is cached), the after-warm number drops
 *   to the handshake floor.
 *
 * E12-02: N=5 sequential connects to the SAME TLS server, per-connect
 *   handshake time. Before client resumption all five are full handshakes;
 *   after, #1 is full and #2..#5 resume (TLS 1.3 tickets / 1.2 session ids).
 *   The 40-rep series gives the stable resumed-vs-full average.
 *
 * Run: ./dynajs bench/bench_tls_ctx.js
 */
import { TCPServer } from "dyna:net";
import { RSA, X509 } from "dyna:crypto";
import { makeTempDir, writeFile, removeAll, Path, readFile } from "dyna:file";

const now = () => performance.now();

/* One round-trip against the TLS echo server; resolves when the echo lands. */
function roundTrip(port, tlsOpts, payload) {
    return new Promise((resolve) => {
        const t0 = now();
        let settled = false;
        const done = (v, okv) => { if (!settled) { settled = true; resolve({ v, okv, ms: now() - t0 }); } };
        const to = setTimeout(() => done("TIMEOUT", false), 8000);
        const cli = TCPServer.connect({ host: "127.0.0.1", port, tls: tlsOpts }, {
            connect(c, err) {
                if (err) { clearTimeout(to); done("ERR " + err, false); cli.close(); return; }
                c.write(payload);
            },
            data(c, b) {
                clearTimeout(to);
                const s = new TextDecoder().decode(b);
                done(s, s === payload);
                cli.close();
            },
            close() { clearTimeout(to); done("CLOSED", false); },
        });
    });
}

async function series(srv, opts, n, payload) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const r = await roundTrip(srv.port, opts, payload + i);
        if (!r.okv) throw new Error("series iteration " + i + " failed: " + r.v);
        out.push(r.ms);
    }
    return out;
}

const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };

async function main() {
    const T = makeTempDir("tlsbench");
    const P = (n) => T + "/" + n;

    /* server fixture: RSA-2048 self-signed (the cert whose parse/sign cost a
       resumption skips is exactly what makes the E12-02 number). */
    const rsa = RSA.generate(2048);
    writeFile(new Path(P("srv.key")), rsa.privateKey);
    writeFile(new Path(P("srv.pem")), X509.generateSelfSigned(
        { key: rsa.privateKey, subject: "localhost", days: 30 }));

    /* the custom CA: vendored Mozilla bundle + the server cert, concatenated.
       load_verify_locations parses ALL of it -- that parse is the cost the
       keyed cache removes. */
    const bundle = readFile(new Path("tests/corpus/ca-bundle.pem"));
    const srvCert = readFile(new Path(P("srv.pem")));
    writeFile(new Path(P("ca-all.pem")), bundle + srvCert);

    const srv = new TCPServer({ port: 0, tls: { cert: P("srv.pem"), key: P("srv.key") } });
    srv.start({ data(c, b) { c.write(b); } });

    print("bench_tls_ctx: server on port " + srv.port +
          ", custom-CA bundle " + (bundle.length / 1024).toFixed(0) + " KiB");

    /* --- E12-02: N=5 sequential handshakes to the same server ----------
       FIRST, while the session store is cold: #1 is the full handshake,
       #2..#5 resume. (Run this series before anything else in the process,
       or earlier series' sessions make even #1 resumed.) */
    {
        const opts = { servername: "localhost", ca: P("srv.pem") };
        const t5 = await series(srv, opts, 5, "s");
        print("BENCH tls-session n5=" + t5.map((x) => x.toFixed(2)).join(" ") +
              "ms  (ms per handshake; #1 full, #2..#5 resumed)");
        /* stable averages: 41 connects, #1 dropped. A DIFFERENT alpn list is
           a different cache entry, so its #1 cannot inherit the n5 series'
           session -- a genuinely full handshake to compare against. */
        const t40 = await series(srv, { servername: "localhost",
                                        ca: P("srv.pem"), alpn: ["http/1.1"] },
                                 41, "s");
        const full = t40[0];
        const rest = t40.slice(1);
        print("BENCH tls-session full=" + full.toFixed(2) +
              "ms resumed-avg=" + (rest.reduce((a, b) => a + b, 0) / rest.length).toFixed(2) +
              "ms resumed-median=" + median(rest).toFixed(2) + "ms n=" + rest.length);
    }

    /* --- E12-04: custom-CA cold vs warm -------------------------------- */
    {
        const opts = { servername: "localhost", ca: P("ca-all.pem") };
        const t = await series(srv, opts, 30, "k");
        print("BENCH tls-ctx custom-ca cold=" + t[0].toFixed(2) +
              "ms warm-median=" + median(t.slice(1)).toFixed(2) +
              "ms min=" + Math.min(...t).toFixed(2) + "ms n=" + t.length +
              "  (warm includes session reuse; ctx-only number = pre-session build)");
        /* CONTROL: the DEFAULT client (cached before and after both changes)
           must not move. rejectUnauthorized:false skips sessions by design
           (no verification, no store lookup), so this is the pure handshake
           floor at every point in the series. */
        const bad = { servername: "localhost", rejectUnauthorized: false };
        const c = await series(srv, bad, 30, "k");
        print("BENCH tls-ctx control(insecure) cold=" + c[0].toFixed(2) +
              "ms warm-median=" + median(c.slice(1)).toFixed(2) + "ms");
    }

    srv.close();
    removeAll(new Path(T));
    print("bench_tls_ctx: done");
}

main().catch((e) => {
    print("bench_tls_ctx: FATAL " + (e && e.message ? e.message : e));
    throw e;
});
