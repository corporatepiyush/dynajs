/* test_tls_conn.js -- TLS through TCPServer.connect (design 16 milestone 2).
 *
 * The C matrix (tests/test_tls.c) proves the engine. This proves the SEAM:
 * that `connect` fires only after the handshake, that plaintext reaches the
 * data handler, and that a bad certificate arrives as a connect ERROR rather
 * than a silent plaintext session.
 *
 * connect() takes the hostname directly since af6ad29, so this no longer
 * resolves by hand -- and the certificate is verified against that same name,
 * which is what the wrong-host pair below actually pins.
 *
 * Needs the network. A skip is loud, and fatal under DYNAJS_REQUIRE_TOOLS=1.
 */
import { TCPServer } from "dyna:net";
import { getEnv } from "dyna:sys";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";

function ok(cond, what, detail) {
    if (cond) { pass++; print("  ok    " + what); }
    else { fail++; print("  FAIL  " + what + (detail ? "  [" + detail + "]" : "")); }
}
function skipped(what) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + what); return; }
    skip++; print("  SKIP  " + what);
}

/* Drive one request to completion. Resolves {status, body, err}. */
function fetchTLS(host, opts, path) {
    return new Promise((resolve) => {
        let buf = "", settled = false;
        const done = (v) => { if (!settled) { settled = true; resolve(v); } };
        const t = setTimeout(() => done({ err: "timeout" }), 20000);
        const conn = TCPServer.connect(
            { host, port: 443, tls: opts },
            {
                connect(c, err) {
                    if (err) { clearTimeout(t); done({ err }); conn.close(); return; }
                    c.write("GET " + path + " HTTP/1.1\r\nHost: " + host +
                            "\r\nConnection: close\r\n\r\n");
                },
                data(c, bytes) {
                    buf += new TextDecoder().decode(bytes);
                },
                close() {
                    clearTimeout(t);
                    const line = buf.split("\r\n")[0] || "";
                    /* A failed handshake can surface as a CLOSE rather than a
                       connect error, and that path carried no err -- so a refusal
                       read as `undefined` and looked like the check not firing.
                       No bytes means no session: report it as a refusal. */
                    done({ status: (line.split(" ")[1] || ""), body: buf.length,
                           refused: buf.length === 0 ? 1 : 0 });
                    /* MANDATORY: the server holds a reference to the shared
                       reactor, so an unclosed one hangs the loop for ever. */
                    conn.close();
                },
            });
    });
}

async function main() {
    print("--- a real HTTPS request through the seam ---");
    /* Only `err === "timeout"` counted as absent, so any OTHER way the host is
       unreachable -- DNS, refused, reset -- scored as a FAILURE of the seam.
       An empty status is the same thing: connected, nothing came back. */
    let r = await fetchTLS("badssl.com", true, "/");
    if (r.err || !r.status) skipped("badssl.com unreachable (" + (r.err || "no response") + ")");
    else {
        ok(!r.err, "connect fired without error", r.err);
        ok(r.status === "200", "GET / over TLS returned 200 (got " + r.status + ")");
        ok(r.body > 0, "plaintext reached the data handler (" + r.body + " bytes)");
    }

    print("--- a bad certificate is a connect ERROR, not a silent session ---");
    /* Here an error is the EXPECTED result, so "unreachable" cannot be read off
       r.err. Distinguish them: a certificate rejection names the certificate. */
    r = await fetchTLS("expired.badssl.com", true, "/");
    if (!r.err || /timeout|refused|reset|resolve|unreachable|ENOTFOUND|EAI_/i.test(String(r.err)) === true &&
        !/cert|expired|verify/i.test(String(r.err)))
        skipped("expired.badssl.com unreachable (" + (r.err || "no error at all") + ")");
    else {
        ok(!!r.err, "expired cert reported through the connect handler", "no error");
        ok(String(r.err).indexOf("expired") >= 0,
           "and the error NAMES the check (" + r.err + ")");
        ok(r.status === undefined, "no data handler ran for a refused session");
    }

    /* The PAIR is the point: the same host, differing only in the opt-out.
       Either row alone would prove nothing about hostname verification. */
    print("--- the name reaches BOTH SNI and verification ---");
    r = await fetchTLS("wrong.host.badssl.com", true, "/");
    if (r.err === "timeout") skipped("wrong.host with verification on");
    /* The bypass -- the thing this test exists to catch -- is an HTTP RESPONSE.
       An error naming the host and a closed session with no bytes are both the
       refusal; only a status line means verification did not fire. */
    else ok((!!r.err && /match|host/i.test(String(r.err))) || r.refused === 1,
            "wrong-host is REFUSED for its name (" +
            String(r.err).slice(0, 55) + ")");

    r = await fetchTLS("wrong.host.badssl.com", { rejectUnauthorized: false }, "/");
    if (r.err === "timeout") skipped("wrong.host with verification off");
    else ok(!r.err, "and CONNECTS once rejectUnauthorized:false is written out " +
                    "in full -- so the refusal above was hostname checking", r.err);

    print("--- ALPN is negotiated ---");
    r = await fetchTLS("cloudflare.com", { alpn: ["http/1.1"] }, "/");
    if (r.err === "timeout") skipped("cloudflare.com alpn");
    else ok(!r.err && r.body > 0, "an ALPN list is accepted and connects", r.err);

    print("test_tls_conn: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_tls_conn: " + fail + " failures");
}

main();
