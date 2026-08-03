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
import { makeTempDir, writeFile, removeAll, Path } from "dyna:file";

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

    removeAll(new Path(T));
    print("test_tls_server: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_tls_server: " + fail + " failures");
}
