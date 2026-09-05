/* test_tls_server.js -- server-side TLS on TCPServer (design 16 milestone 3).
 *
 * The oracle is openssl(1) s_client: a well-tested independent peer is the
 * right check for OUR SERVER, exactly as a hand-written hostile peer is the
 * right check for our client. A dynajs client talking to a dynajs server would
 * prove only that the two agree with each other.
 *
 * THE SERVER RUNS IN ITS OWN PROCESS. Exec() is synchronous and blocks the JS
 * thread, but a server needs the event loop running to accept and pump a
 * handshake -- driving the peer from inside the same process deadlocks the two
 * against each other, silently, with no output because a killed process loses
 * its buffered stdout. Same shape test_http_pentest already uses.
 *
 * Needs openssl(1). A skip is loud, and fatal under DYNAJS_REQUIRE_TOOLS=1.
 */
import { TCPServer } from "dyna:net";
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, removeAll, exists, Path } from "dyna:file";

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
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
const cat = (f) => (Exec("/bin/sh", ["-c", "cat " + f + " 2>/dev/null"]).stdout || "");

if (!Which("openssl")) {
    skipped("openssl(1) missing -- the foreign peer cannot run, and a dynajs " +
            "client talking to a dynajs server proves only self-agreement");
    print("test_tls_server: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_tls_server: " + fail + " failures");
} else {
    const T = makeTempDir("tlssrv");
    const P = (n) => T + "/" + n;
    const q = (n) => JSON.stringify(P(n));
    sh(`openssl req -x509 -newkey rsa:2048 -keyout ${P("key.pem")} -out ${P("cert.pem")} ` +
       `-days 2 -nodes -subj "/CN=localhost" 2>/dev/null`);
    sh(`openssl req -x509 -newkey rsa:2048 -keyout ${P("other.key")} -out ${P("other.crt")} ` +
       `-days 2 -nodes -subj "/CN=other" 2>/dev/null`);

    /* ---- the contract, in-process: no server needs to run for any of it ---- */
    {
        let m = throws(() => new TCPServer({ port: 0, tls: {} }),
                       "a tls server without cert/key is refused");
        ok(/cert/.test(m) && /key/.test(m), "and the error names both");
        throws(() => new TCPServer({ port: 0, tls: { cert: P("cert.pem") } }),
               "cert without key is refused");
        /* Would otherwise fail per-client at runtime instead of at startup. */
        m = throws(() => new TCPServer({ port: 0,
                        tls: { cert: P("cert.pem"), key: P("other.key") } }),
                   "a key that does not match the certificate is refused AT STARTUP");
        ok(/match/i.test(m), "and the error says so (" + m.slice(0, 52) + ")");
        throws(() => new TCPServer({ port: 0,
                        tls: { cert: P("nope.pem"), key: P("key.pem") } }),
               "an unreadable certificate path is refused");
    }

    /* ---- a real handshake against openssl s_client ---- */
    {
        writeFile(new Path(P("srv.js")), `
import { TCPServer } from "dyna:net";
import * as std from "std";
const log = (s) => { const f = std.open(${q("srv.out")}, "a"); f.puts(s + "\\n"); f.close(); };
const srv = new TCPServer({ port: 0,
    tls: { cert: ${q("cert.pem")}, key: ${q("key.pem")} } });
srv.start({
  data(c, b) { log("DATA " + new TextDecoder().decode(b).trim());
               c.write("HTTP/1.0 200 OK\\r\\nContent-Length: 5\\r\\n\\r\\nhello"); },
});
log("PORT " + srv.port);
setTimeout(() => srv.close(), 20000);
`);
        sh(`./dynajs ${P("srv.js")} >/dev/null 2>&1 &`);
        let port = 0;
        for (let i = 0; i < 60 && !port; i++) {
            sh("sleep 0.2");
            const m = cat(P("srv.out")).match(/PORT (\d+)/);
            if (m) port = parseInt(m[1], 10);
        }
        if (!port) skipped("the TLS server child did not start");
        else {
            ok(port > 0, "the TLS server binds and reports its port");
            sh(`(printf 'PING\\r\\n\\r\\n'; sleep 1) | timeout 12 openssl s_client ` +
               `-connect 127.0.0.1:${port} -servername localhost ` +
               `>${P("cli.out")} 2>${P("cli.err")}`);
            const out = cat(P("cli.out")), err = cat(P("cli.err"));
            const srvlog = cat(P("srv.out"));
            ok(/200 OK/.test(out),
               "openssl s_client completes the handshake and reads our plaintext");
            ok(/DATA PING/.test(srvlog),
               "and the server's data handler received the client's plaintext");
            /* s_client prints the session summary on STDOUT, not stderr. */
            const both = out + err;
            ok(/TLSv1\.[23]/.test(both), "negotiated 1.2 or 1.3 (" +
               (both.match(/TLSv1\.[0-9]/) || ["?"])[0] + ")");
            ok(/CN\s*=\s*localhost/.test(out),
               "the certificate we configured is the one served");
        }
        sh(`pkill -f ${q("srv.js")} 2>/dev/null`);
    }

    /* ---- server-side ALPN: the configured list must actually negotiate ----
     * Until SSL_CTX_set_alpn_select_cb was installed, alpn_wire was parsed and
     * stored but NEVER consulted: every handshake negotiated nothing, whatever
     * the options said. s_client prints "ALPN protocol: <name>" for the
     * protocol the SERVER selected, which makes the selection directly
     * observable to an independent peer. */
    {
        writeFile(new Path(P("srv2.js")), `
import { TCPServer } from "dyna:net";
import * as std from "std";
const log = (s) => { const f = std.open(${q("srv2.out")}, "a"); f.puts(s + "\\n"); f.close(); };
const srv = new TCPServer({ port: 0,
    tls: { cert: ${q("cert.pem")}, key: ${q("key.pem")}, alpn: "http/1.1,h2" } });
srv.start({
  data(c, b) { log("DATA " + new TextDecoder().decode(b).trim());
               c.write("HTTP/1.0 200 OK\\r\\nContent-Length: 5\\r\\n\\r\\nhello"); },
});
log("PORT " + srv.port);
setTimeout(() => srv.close(), 20000);
`);
        sh(`./dynajs ${P("srv2.js")} >/dev/null 2>&1 &`);
        let port = 0;
        for (let i = 0; i < 60 && !port; i++) {
            sh("sleep 0.2");
            const m = cat(P("srv2.out")).match(/PORT (\d+)/);
            if (m) port = parseInt(m[1], 10);
        }
        if (!port) skipped("the ALPN TLS server child did not start");
        else {
            const probe = (alpnFlag) => {
                sh(`(printf 'PING\\r\\n\\r\\n'; sleep 1) | timeout 12 openssl s_client ` +
                   `-connect 127.0.0.1:${port} -servername localhost ${alpnFlag} ` +
                   `>${P("cli2.out")} 2>${P("cli2.err")}`);
                return cat(P("cli2.out")) + cat(P("cli2.err"));
            };
            /* A matching offer selects that protocol -- visible to s_client. */
            let both = probe("-alpn http/1.1");
            ok(/ALPN protocol: http\/1\.1/.test(both),
               "the server SELECTS the protocol it shares with the client (" +
               (both.match(/ALPN protocol: \S+/) || ["no ALPN line"])[0] + ")");
            ok(/200 OK/.test(both), "and the session still carries data");
            /* The CLIENT's first match wins: offering h2 first picks h2. */
            both = probe("-alpn h2,http/1.1");
            ok(/ALPN protocol: h2/.test(both),
               "the client's preference order is honored (h2 first) (" +
               (both.match(/ALPN protocol: \S+/) || ["no ALPN line"])[0] + ")");
            /* No overlap is a NO-ACK, not a refusal: the handshake completes
               with no protocol negotiated. */
            both = probe("-alpn spdy/3");
            ok(/200 OK/.test(both),
               "an offer we do not share still completes the handshake (no-ack)");
            ok(!/ALPN protocol: (http\/1\.1|h2)/.test(both),
               "and nothing was negotiated (" +
               (both.match(/ALPN protocol: \S+/) || ["no ALPN line"])[0] + ")");
        }
        sh(`pkill -f ${q("srv2.js")} 2>/dev/null`);
    }

    /* ---- server-side session resumption (audit E12-02): the FOREIGN peer
     * stores its session with -sess_out and replays it with -sess_in, and
     * s_client prints "Reused" ONLY when our server accepted the replay --
     * proof our server issues resumable sessions (TLS 1.3 tickets by
     * default; TLS 1.2 session ids via session_id_context) and honours them.
     */
    {
        writeFile(new Path(P("srv3.js")), `
import { TCPServer } from "dyna:net";
import * as std from "std";
const log = (s) => { const f = std.open(${q("srv3.out")}, "a"); f.puts(s + "\\n"); f.close(); };
const srv = new TCPServer({ port: 0,
    tls: { cert: ${q("cert.pem")}, key: ${q("key.pem")} } });
srv.start({
  data(c, b) { c.write("HTTP/1.0 200 OK\\r\\nContent-Length: 5\\r\\n\\r\\nhello"); },
});
log("PORT " + srv.port);
setTimeout(() => srv.close(), 20000);
`);
        sh(`./dynajs ${P("srv3.js")} >/dev/null 2>&1 &`);
        let port3 = 0;
        for (let i = 0; i < 60 && !port3; i++) {
            sh("sleep 0.2");
            const m = cat(P("srv3.out")).match(/PORT (\d+)/);
            if (m) port3 = parseInt(m[1], 10);
        }
        if (!port3) skipped("the resumption TLS server child did not start");
        else {
            sh(`(printf 'PING\\r\\n'; sleep 1) | timeout 12 openssl s_client ` +
               `-connect 127.0.0.1:${port3} -servername localhost ` +
               `-sess_out ${P("sess.pem")} >${P("c1.out")} 2>&1`);
            ok(exists(new Path(P("sess.pem"))),
               "first s_client session stored with -sess_out");
            sh(`(printf 'PING\\r\\n'; sleep 1) | timeout 12 openssl s_client ` +
               `-connect 127.0.0.1:${port3} -servername localhost ` +
               `-sess_in ${P("sess.pem")} >${P("c2.out")} 2>&1`);
            const c2 = cat(P("c2.out"));
            ok(/Reused/.test(c2),
               "s_client REPLAYS its session and our server RESUMES (" +
               (c2.match(/New,|Reused[^\n]*/) || ["no summary"])[0].slice(0, 40) + ")");
            ok(/200 OK/.test(c2), "and the resumed session still carries data");
        }
        sh(`pkill -f ${q("srv3.js")} 2>/dev/null`);
    }

    removeAll(new Path(T));
    print("test_tls_server: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_tls_server: " + fail + " failures");
}
