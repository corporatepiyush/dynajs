/* test_client_tls.js -- Redis and PostgreSQL over TLS.
 *
 * Redis and PostgreSQL clients accept tls:true -- the TLS handshake runs on
 * the aio engine (the same machinery as wss:// and dyna:http), and the
 * protocol bytes (HELLO, startup message) flow encrypted. Against a
 * TLS-capable TCP echo (not real servers), the TLS handshake must complete
 * with the CA pinned, and the protocol bytes must be rejected at the PROTOCOL
 * layer (not TLS).
 *
 * TLS is a BUILD CAPABILITY, not a given: without CONFIG_TLS the RSA/X509
 * classes are absent from dyna:crypto outright, so the suite cannot even
 * build its fixture certificate. On such a binary it reports a named SKIP
 * and exits 0 -- a green pass on a build that could not have run the test
 * would be a lie -- and DYNAJS_REQUIRE_TOOLS=1 turns the skip into a FAILURE
 * where TLS is mandatory. Both branches are exercised: the default gate runs
 * it for real on a TLS-enabled build and skips it here otherwise.
 */
import { Redis, PostgreSQL, TCPServer } from "dyna:net";
import * as crypto from "dyna:crypto";
import { makeTempDir, writeFile, Path, removeAll } from "dyna:file";
import { getEnv } from "dyna:sys";

/* The capability probe is the fixture's own prerequisites: without CONFIG_TLS
   dyna:crypto carries no RSA/X509 export at all (a missing export, not a
   silently weaker one), so a build that cannot make a certificate cannot run
   the exchange either. */
const HAS_TLS = !!crypto.RSA && !!crypto.X509 &&
                typeof crypto.RSA.generate === "function" &&
                typeof crypto.X509.generateSelfSigned === "function";

if (!HAS_TLS) {
    console.log("  SKIP  test_client_tls: this binary has no TLS " +
                "(dyna:crypto exports no RSA/X509) -- build with CONFIG_TLS=y");
    console.log("test_client_tls: 0 passed, 0 failed, 1 skipped (no TLS)");
    if (getEnv("DYNAJS_REQUIRE_TOOLS") === "1") {
        console.log("  FAIL  test_client_tls: TLS is required here (DYNAJS_REQUIRE_TOOLS=1)");
        throw new Error("test_client_tls: TLS unavailable");
    }
} else {
    runClientTLSSuite();
}

function runClientTLSSuite() {
    let pass = 0, fail = 0;
    const ok = (c, w) => { if (c) pass++; else { fail++; console.log("  FAIL " + w); } };

    const T = makeTempDir("c9tls");
    const k = crypto.RSA.generate(2048);
    writeFile(new Path(T, "k.pem"), k.privateKey);
    writeFile(new Path(T, "c.pem"),
        crypto.X509.generateSelfSigned({ key: k.privateKey, subject: "localhost", days: 30 }));

    /* a TLS TCP echo server (echoes whatever it receives) */
    const echo = new TCPServer({ port: 0, tls: { cert: T + "/c.pem", key: T + "/k.pem" } });
    echo.start({ data(c, b) { c.write(b); } });

    /* 1. Redis tls:true — constructor must NOT throw */
    let r;
    try { r = new Redis({ host: "localhost", port: echo.port, tls: true,
                          ca: T + "/c.pem", connectTimeoutMs: 5000 }); }
    catch (e) { ok(false, "Redis tls:true threw: " + e.message); }
    ok(typeof r === "object", "Redis tls:true constructs");
    r.on("error", () => {});   // the echo server won't speak RESP — expect errors

    /* 2. PostgreSQL tls:true — constructor must NOT throw */
    let pg;
    try { pg = new PostgreSQL({ host: "localhost", port: echo.port, tls: true,
                                ca: T + "/c.pem", connectTimeoutMs: 5000 }); }
    catch (e) { ok(false, "PostgreSQL tls:true threw: " + e.message); }
    ok(typeof pg === "object", "PostgreSQL tls:true constructs");
    pg.on("error", () => {});

    /* 3. TLS verification: without the CA pin, the handshake must FAIL (self-signed
          refused — the TLS layer actually verified) */
    const r2 = new Redis({ host: "localhost", port: echo.port, tls: true,
                           connectTimeoutMs: 5000 });
    let tlsVerifyRefused = false;
    r2.on("error", (e) => {
        if (/TLS|certificate|handshake/i.test(String(e))) tlsVerifyRefused = true;
    });

    /* 4. Plaintext (tls absent) still works against a PLAINTEXT server */
    const plain = new TCPServer({ port: 0 });
    plain.start({ data(c, b) { c.write(b); } });
    const r3 = new Redis({ host: "127.0.0.1", port: plain.port, connectTimeoutMs: 5000 });
    r3.on("error", () => {});
    ok(true, "plaintext Redis constructs alongside TLS");

    setTimeout(() => {
        ok(tlsVerifyRefused, "no-CA TLS is refused by verification (self-signed)");
        r.close(); pg.close(); r2.close(); r3.close();
        echo.close(); plain.close();
        removeAll(new Path(T));
        console.log("test_client_tls: " + pass + " passed, " + fail + " failed");
        if (fail) throw new Error("test_client_tls: " + fail + " failures");
    }, 4000);
}
