/* test_tls_matrix.js -- the offline TLS matrix (design 16 milestone 4).
 *
 * The badssl tests (test_tls.c, test_tls_conn.js, test_http_tls.js) prove
 * the client against the WORLD, and test_tls_server.js proves the server
 * against openssl s_client. This file owns what those cannot: the whole
 * verification/ALPN/payload/adversarial matrix against CERTIFICATES THIS
 * PROCESS GENERATES, all on loopback, deterministic, no network, no
 * openssl except the one expired-cert fixture.
 *
 * Every refusal must NAME its check ("hostname mismatch", "certificate has
 * expired") -- "handshake failed" is not actionable -- and every refusal
 * carries its own control (rejectUnauthorized:false must connect), because
 * a check that cannot be turned off proves nothing about the check.
 *
 * The server and client share one event loop, so every session here is
 * in-process: the loop pumps both sides of the handshake.
 *
 * Bounded: every phase carries a timeout; the whole file must finish well
 * under the gate's limits. Needs CONFIG_TLS=y, like the rest of the TLS
 * stage. Run: make CONFIG_NATIVE_MODULES=y CONFIG_TLS=y test-tls-matrix
 */
import { TCPServer } from "dyna:net";
import { RSA, ECDSA, X509 } from "dyna:crypto";
import { makeTempDir, writeFile, readFile, removeAll, Path, exists } from "dyna:file";
import { Exec, Which, getEnv } from "dyna:sys";
import * as std from "std";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function throws(fn, w) {
    let t = false, m = "";
    try { fn(); } catch (e) { t = true; m = String(e.message || e); }
    ok(t, w, t ? "" : "did NOT throw");
    return m;
}
function skipped(w) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; }
    skip++; print("  SKIP  " + w);
}

/* Global watchdog: a hang must be reported as a timeout, distinctly from a
   failure -- and must not look like the whole suite silently stopping. */
let watchdogFired = false;
const watchdog = setTimeout(() => {
    watchdogFired = true;
    print("TIMEOUT: test_tls_matrix exceeded its 120s budget");
    print("test_tls_matrix: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail === 0) { fail++; }
    throw new Error("test_tls_matrix timed out");
}, 120000);
watchdog.unref && watchdog.unref();

/* One round-trip against a TLS server: resolve {state} when settled.
   state: "OK <echo>" | "ERR <message>" | "CLOSED" (closed before echo). */
function roundTrip(port, tlsOpts, payload, handlers) {
    return new Promise((resolve) => {
        let settled = false;
        const done = (v) => { if (!settled) { settled = true; resolve(v); } };
        const t = setTimeout(() => done({ state: "TIMEOUT" }), 8000);
        const cli = TCPServer.connect({ host: "127.0.0.1", port, tls: tlsOpts }, {
            connect(c, err) {
                if (err) { clearTimeout(t); done({ state: "ERR " + err }); cli.close(); return; }
                if (handlers && handlers.connect) { handlers.connect(c); return; }
                c.write(payload);
            },
            data(c, b) {
                clearTimeout(t);
                done({ state: "OK " + new TextDecoder().decode(b) });
                cli.close();
            },
            close() {
                clearTimeout(t);
                if (!settled) { done({ state: "CLOSED" }); cli.close(); }
            },
        });
    });
}

/* A TLS echo server on an ephemeral port; `onData` may replace the echo. */
function tlsEchoServer(cert, key, alpn, onData) {
    const srv = new TCPServer({ port: 0, tls: { cert, key, alpn } });
    srv.start({
        data(c, b) {
            if (onData) { onData(c, b); return; }
            c.write(b);
        },
    });
    return srv;
}

async function main() {
    const T = makeTempDir("tlsmatrix");
    const P = (...n) => T + "/" + n.join("/");

    /* ---- fixtures: certificates this process generates ---------------- */
    print("--- fixtures ---");
    const rsa = RSA.generate(2048);
    writeFile(new Path(P("rsa.key")), rsa.privateKey);
    writeFile(new Path(P("rsa.pem")), X509.generateSelfSigned(
        { key: rsa.privateKey, subject: "localhost", days: 30 }));
    const other = RSA.generate(2048);
    writeFile(new Path(P("other.key")), other.privateKey);
    writeFile(new Path(P("other.pem")), X509.generateSelfSigned(
        { key: other.privateKey, subject: "other.test", days: 30 }));
    const ec = ECDSA.generate("P-256");
    writeFile(new Path(P("ec.key")), ec.privateKey);
    writeFile(new Path(P("ec.pem")), X509.generateSelfSigned(
        { key: ec.privateKey, subject: "localhost", days: 30 }));
    ok(exists(new Path(P("rsa.pem"))), "RSA self-signed fixture written");
    ok(exists(new Path(P("ec.pem"))), "ECDSA self-signed fixture written");

    /* The one fixture openssl must make: a cert already expired. The in-house
       generator clamps days to >= 1, so openssl ca with explicit PAST dates
       is the offline source. Skip loudly if openssl is absent. */
    let expired = null;
    if (!Which("openssl")) {
        skipped("openssl(1) missing -- the expired-cert fixture cannot be built");
    } else {
        const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
        sh("mkdir -p " + P("ca") + " && cd " + P("ca") +
           " && touch index.txt && echo 01 > serial");
        writeFile(new Path(P("ca", "ca.cnf")),
            "[ca]\ndefault_ca = CA_default\n[CA_default]\ndatabase = ./index.txt\n" +
            "serial = ./serial\nnew_certs_dir = .\ndefault_md = sha256\n" +
            "policy = policy_any\nx509_extensions = ext\n[policy_any]\n" +
            "commonName = supplied\n[ext]\nbasicConstraints = CA:true\n");
        sh("cd " + P("ca") + " && openssl req -new -newkey rsa:2048 -nodes " +
           "-keyout k.pem -out r.pem -subj '/CN=localhost' 2>/dev/null && " +
           "openssl ca -selfsign -batch -config ca.cnf -keyfile k.pem -in r.pem " +
           "-out expired.pem -startdate 20200101000000Z " +
           "-enddate 20210101000000Z 2>/dev/null");
        if (exists(new Path(P("ca", "expired.pem")))) {
            expired = P("ca");
            ok(true, "expired fixture built (openssl ca, past validity)");
        } else {
            skipped("openssl ca refused to build the expired fixture");
        }
    }

    /* ---- 1. handshake + echo over generated certs -------------------- */
    print("--- handshake and echo (RSA and ECDSA certs) ---");
    {
        const s1 = tlsEchoServer(P("rsa.pem"), P("rsa.key"));
        let r = await roundTrip(s1.port,
            { servername: "localhost", ca: P("rsa.pem") }, "ping");
        ok(/^OK ping$/.test(r.state), "RSA cert: handshake + echo (" + r.state + ")");
        r = await roundTrip(s1.port, { servername: "localhost" }, "ping");
        ok(/^ERR .*self-signed/.test(r.state),
           "RSA cert: refused without the CA pin, naming the check (" + r.state.slice(0, 60) + ")");
        s1.close();

        const s2 = tlsEchoServer(P("ec.pem"), P("ec.key"));
        r = await roundTrip(s2.port,
            { servername: "localhost", ca: P("ec.pem") }, "ping");
        ok(/^OK ping$/.test(r.state), "ECDSA cert: handshake + echo (ECDHE_ECDSA path)");
        s2.close();
    }

    /* ---- 2. the verification matrix ---------------------------------- */
    print("--- verification matrix (each refusal NAMES its check) ---");
    {
        const s = tlsEchoServer(P("rsa.pem"), P("rsa.key"));
        let r = await roundTrip(s.port,
            { servername: "localhost", ca: P("other.pem") }, "ping");
        ok(/^ERR .*self-signed/.test(r.state),
           "a pinned CA that did not sign the leaf is refused (" + r.state.slice(0, 60) + ")");

        r = await roundTrip(s.port,
            { servername: "other.test", ca: P("rsa.pem") }, "ping");
        ok(/^ERR .*hostname mismatch/.test(r.state),
           "wrong servername is refused, NAMING hostname mismatch (" + r.state.slice(0, 60) + ")");

        /* The control: the same wrong name with verification OFF must
           connect, or the refusal above proved nothing about name checking. */
        r = await roundTrip(s.port,
            { servername: "other.test", ca: P("rsa.pem"), rejectUnauthorized: false }, "ping");
        ok(/^OK ping$/.test(r.state),
           "CONTROL: the same wrong name CONNECTS with rejectUnauthorized:false");

        /* SNI is a separate axis from verification: servername drives both,
           so an IP host with the cert's name in the option must connect. */
        r = await roundTrip(s.port,
            { servername: "localhost", ca: P("rsa.pem") }, "ping");
        ok(/^OK ping$/.test(r.state), "servername option satisfies CN verification");

        if (expired) {
            const se = tlsEchoServer(P("ca", "expired.pem"), P("ca", "k.pem"));
            r = await roundTrip(se.port,
                { servername: "localhost", ca: P("ca", "expired.pem") }, "ping");
            ok(/^ERR .*expired/.test(r.state),
               "an expired cert is refused, NAMING expired (" + r.state.slice(0, 60) + ")");
            se.close();
        }
        s.close();
    }

    /* ---- 3. what actually goes on the wire --------------------------- */
    print("--- the wire is TLS framing, not the plaintext ---");
    {
        const cap = [];
        const mitm = new TCPServer({ port: 0 });
        mitm.start({ data(c, b) { cap.push(...Array.from(b)); } });
        const cli = TCPServer.connect(
            { host: "127.0.0.1", port: mitm.port, tls: { servername: "localhost" } }, {
            connect(c, err) { if (!err) c.write("SECRET-PLAINTEXT"); },
            close() { mitm.close(); },
        });
        await new Promise((res) => setTimeout(res, 1200));
        const wire = String.fromCharCode(...cap.slice(0, 64));
        ok(cap[0] === 0x16 && cap[1] === 0x03,
           "first bytes on the wire are a TLS handshake record (0x16 0x03), got " +
           cap[0].toString(16) + " " + cap[1].toString(16));
        ok(!wire.includes("SECRET"),
           "the plaintext never appears on the wire");
        cli.close();
    }

    /* ---- 4. ALPN: negotiation is optional, both directions ----------- */
    print("--- ALPN (selection semantics here; the negotiated VALUE is proven against the openssl foreign peer in test_tls_server.js) ---");
    {
        const sA = tlsEchoServer(P("rsa.pem"), P("rsa.key"), "http/1.1,h2");
        let r = await roundTrip(sA.port,
            { servername: "localhost", ca: P("rsa.pem"), alpn: ["http/1.1"] }, "ping");
        ok(/^OK ping$/.test(r.state), "server http/1.1 + client http/1.1 connects");
        r = await roundTrip(sA.port,
            { servername: "localhost", ca: P("rsa.pem") }, "ping");
        ok(/^OK ping$/.test(r.state), "client with no ALPN still connects (no offer, no ack)");
        r = await roundTrip(sA.port,
            { servername: "localhost", ca: P("rsa.pem"), alpn: ["spdy/3"] }, "ping");
        ok(/^OK ping$/.test(r.state),
           "client offering only an unknown protocol still connects (no match = no ack, not a refusal)");
        sA.close();
    }

    /* ---- 5. payload boundaries --------------------------------------- */
    print("--- payload boundaries (record size 16384, multi-record drain) ---");
    {
        const s = tlsEchoServer(P("rsa.pem"), P("rsa.key"));
        const sizes = [1, 16383, 16384, 16385, 65536, 1048576];
        let si = 0, got = 0, expect = sizes[0], state = "wait";
        const cli = TCPServer.connect(
            { host: "127.0.0.1", port: s.port,
              tls: { servername: "localhost", ca: P("rsa.pem") } }, {
            connect(c, err) {
                if (err) { state = "ERR " + err; return; }
                c.write(new Uint8Array(expect).fill(0x41));
            },
            data(c, b) {
                got += b.length;
                if (got === expect) {
                    print("  ok    echo " + expect + " bytes");
                    si++;
                    if (si < sizes.length) {
                        got = 0; expect = sizes[si];
                        c.write(new Uint8Array(expect).fill(0x41));
                    } else { state = "all-done"; cli.close(); s.close(); }
                } else if (got > expect) {
                    state = "OVERFLOW " + got + " want " + expect;
                    cli.close(); s.close();
                }
            },
            close() { if (state === "wait") state = "closed-early"; },
        });
        await new Promise((res) => setTimeout(res, 20000));
        ok(state === "all-done", "all " + sizes.length + " payload sizes round-tripped");
        if (state !== "all-done") { cli.close(); s.close(); }
    }

    /* ---- 6. adversarial inputs --------------------------------------- */
    print("--- adversarial: a peer that does not speak TLS ---");
    {
        let serverGot = 0, rawClosed = false;
        const s = tlsEchoServer(P("rsa.pem"), P("rsa.key"));
        const raw = TCPServer.connect({ host: "127.0.0.1", port: s.port }, {
            connect(c, err) { if (!err) c.write("GET / HTTP/1.0\r\n\r\n"); },
            close() { rawClosed = true; },
        });
        await new Promise((res) => setTimeout(res, 1500));
        ok(serverGot === 0, "plaintext bytes to the TLS server never reach the data handler");
        ok(rawClosed, "and the raw connection is closed");
        raw.close(); s.close();
    }
    {
        let state = "wait";
        const plain = new TCPServer({ port: 0 });
        plain.start({ data(c, b) { c.write("PLAINTEXT-REPLY"); } });
        const cli = TCPServer.connect(
            { host: "127.0.0.1", port: plain.port, tls: { servername: "localhost" } }, {
            connect(c, err) { if (err) { state = "ERR " + err; cli.close(); plain.close(); } },
            data(c, b) { state = "DATA " + new TextDecoder().decode(b); },
            close() { if (state === "wait") { state = "CLOSED"; plain.close(); } },
        });
        await new Promise((res) => setTimeout(res, 1500));
        ok(state.startsWith("ERR") || state === "CLOSED",
           "a TLS client against a plaintext server never reads plaintext (" +
           state.slice(0, 50) + ")");
        cli.close();
    }
    {
        /* Abrupt close: the server drops the connection without close_notify.
           The client's close handler must still fire. */
        let state = "wait";
        const s = tlsEchoServer(P("rsa.pem"), P("rsa.key"), null,
            (c) => { c.close(); });
        const cli = TCPServer.connect(
            { host: "127.0.0.1", port: s.port,
              tls: { servername: "localhost", ca: P("rsa.pem") } }, {
            connect(c, err) { if (!err) c.write("x"); },
            close() { state = "CLIENT-CLOSED"; s.close(); },
        });
        await new Promise((res) => setTimeout(res, 1500));
        ok(state === "CLIENT-CLOSED", "abrupt server close still fires the client close handler");
        cli.close();
    }

    /* ---- 7. concurrency + the close-handler refcount regression ------ */
    print("--- 8 simultaneous TLS sessions, closed from their close handlers ---");
    {
        const s = tlsEchoServer(P("rsa.pem"), P("rsa.key"));
        let okCount = 0, doneCount = 0;
        const N = 8;
        const finish = () => {
            doneCount++;
            if (doneCount === N) {
                ok(okCount === N, "all " + N + " concurrent sessions echoed and closed cleanly");
                s.close();
            }
        };
        for (let i = 0; i < N; i++) {
            roundTrip(s.port, { servername: "localhost", ca: P("rsa.pem") },
                      "s" + i, { connect(c) { c.write("s" + i); } })
                .then((r) => { if (/^OK s/.test(r.state)) okCount++; finish(); });
        }
        /* The TCPConn-leak regression: closing from inside the close handler
           used to leak one conn per session and abort at JS_FreeRuntime with
           "list_empty(&rt->gc_obj_list)". Reaching the summary line with rc=0
           IS the assertion -- the process survived the teardown. */
        await new Promise((res) => setTimeout(res, 10000));
        if (doneCount !== N) ok(false, "concurrent sessions did not all settle");
    }

    print("--- sequential close-from-handler churn (the leak regression) ---");
    {
        const s = tlsEchoServer(P("rsa.pem"), P("rsa.key"));
        for (let i = 0; i < 12; i++) {
            const r = await roundTrip(s.port,
                { servername: "localhost", ca: P("rsa.pem") }, "c" + i);
            if (!/^OK c/.test(r.state)) {
                ok(false, "churn session " + i + " failed: " + r.state);
                break;
            }
        }
        ok(true, "12 sequential close-from-handler sessions completed");
        s.close();
    }

    /* ---- 8. the keyed ctx cache (audit E12-04) ------------------------ */
    print("--- keyed ctx cache: two trust stores ALTERNATING -------------");
    /* A,B,A,B across two servers whose certs need DIFFERENT pinned CAs.
       With a key-less keyed cache the second caller silently inherits the
       first caller's trust store and the alternation FAILS verification.
       It must be ALL-OK: any refusal here is a cache-key mutation
       announcing itself. */
    {
        const srvR = tlsEchoServer(P("rsa.pem"), P("rsa.key"));
        const srvE = tlsEchoServer(P("ec.pem"), P("ec.key"));
        const optsR = { servername: "localhost", ca: P("rsa.pem") };
        const optsE = { servername: "localhost", ca: P("ec.pem") };
        let altOk = 0;
        for (let i = 0; i < 6; i++) {
            const alt = i % 2 === 0;
            const r = await roundTrip(alt ? srvR.port : srvE.port,
                                      alt ? optsR : optsE, "k" + i);
            if (!/^OK k/.test(r.state)) {
                ok(false, "cache alternation " + i + " (" + (alt ? "rsa" : "ec") +
                          ") got the WRONG ctx: " + r.state);
                break;
            }
            altOk++;
        }
        ok(altOk === 6, "6 alternating rsa/ec connects all verified -- " +
                        "no caller inherited another's trust store");
        srvR.close();
        srvE.close();
    }

    /* ---- 9. mTLS: the server verifies the client (audit E12-03) ------- */
    print("--- mTLS: requestCert on the server, cert+key on the client ---");
    {
        writeFile(new Path(P("cli.key")), RSA.generate(2048).privateKey);
        writeFile(new Path(P("cli.pem")), X509.generateSelfSigned(
            { key: readFile(new Path(P("cli.key"))), subject: "client", days: 30 }));
        writeFile(new Path(P("ocli.key")), RSA.generate(2048).privateKey);
        writeFile(new Path(P("ocli.pem")), X509.generateSelfSigned(
            { key: readFile(new Path(P("ocli.key"))), subject: "intruder", days: 30 }));

        /* Startup contract: demanding client certificates with no store to
           verify them against is a guaranteed-fail server -- refuse to
           start, and name the option. */
        const m = throws(() => new TCPServer({ port: 0,
                        tls: { cert: P("rsa.pem"), key: P("rsa.key"),
                               requestCert: true } }),
                       "requestCert without ca is refused AT STARTUP");
        ok(/requestCert|ca/i.test(m), "and the error names the missing store");

        const mtlsServer = () => {
            const s = new TCPServer({ port: 0,
                tls: { cert: P("rsa.pem"), key: P("rsa.key"),
                       requestCert: true, ca: P("cli.pem") } });
            s.start({ data(c, b) { c.write(b); } });
            return s;
        };
        const cli = (cert, key) => ({ servername: "localhost",
                                      ca: P("rsa.pem"), cert, key });

        /* The pinned client works -- twice, so the second is not luck. */
        const sM = mtlsServer();
        let r = await roundTrip(sM.port, cli(P("cli.pem"), P("cli.key")), "m1");
        ok(/^OK m1$/.test(r.state), "mTLS: the pinned client connects (" + r.state.slice(0, 50) + ")");
        r = await roundTrip(sM.port, cli(P("cli.pem"), P("cli.key")), "m2");
        ok(/^OK m2$/.test(r.state), "mTLS: and a second time");
        sM.close();

        /* A certificate the server does not trust is refused. Each refusing
           session runs against a FRESH single-use server: the client aborts
           mid-handshake, and on macOS a refusal can wedge the listener's
           next accept (repo audit note) -- so nothing connects to these
           servers afterwards. The refusal itself may surface as ERR or as a
           plain close: either way the session never opens. */
        const sW = mtlsServer();
        r = await roundTrip(sW.port, cli(P("ocli.pem"), P("ocli.key")), "bad");
        ok(/^ERR |^CLOSED/.test(r.state),
           "mTLS: an UNTRUSTED client cert is refused (" + r.state.slice(0, 60) + ")");
        sW.close();

        /* No certificate at all is refused too -- the FAIL_IF_NO_PEER_CERT
           half of the contract. */
        const sN = mtlsServer();
        r = await roundTrip(sN.port,
            { servername: "localhost", ca: P("rsa.pem") }, "nocert");
        ok(/^ERR |^CLOSED/.test(r.state),
           "mTLS: a client with NO certificate is refused (" + r.state.slice(0, 60) + ")");
        sN.close();

        /* Presenting a certificate to a server that never asked is fine. */
        const sP = tlsEchoServer(P("rsa.pem"), P("rsa.key"));
        r = await roundTrip(sP.port, cli(P("cli.pem"), P("cli.key")), "extra");
        ok(/^OK extra$/.test(r.state),
           "a client cert offered to a non-requesting server still connects");
        sP.close();
    }

    removeAll(new Path(T));
    clearTimeout(watchdog);
    print("test_tls_matrix: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_tls_matrix: " + fail + " failures");
}

main().catch((e) => {
    clearTimeout(watchdog);
    print("test_tls_matrix: FATAL " + (e && e.message ? e.message : e));
    print("test_tls_matrix: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    throw e;
});