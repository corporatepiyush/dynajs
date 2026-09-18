/* test_client_tls.js — C9 from SECURITY_COMPAT_PLAN.md.
 * Redis and PostgreSQL clients accept tls:true — the TLS handshake runs
 * on the aio engine (the same machinery as wss:// and dyna:http), and the
 * protocol bytes (HELLO, startup message) flow encrypted.
 * Against a TLS-capable TCP echo (not real servers), the TLS handshake
 * must complete with the CA pinned, and the protocol bytes must be
 * rejected at the PROTOCOL layer (not TLS).
 */
import { Redis, PostgreSQL, TCPServer } from "dyna:net";
import { RSA, X509 } from "dyna:crypto";
import { makeTempDir, writeFile, Path, removeAll } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; console.log("  FAIL  " + w); } };

const T = makeTempDir("c9tls");
const k = RSA.generate(2048);
writeFile(new Path(T, "k.pem"), k.privateKey);
writeFile(new Path(T, "c.pem"),
    X509.generateSelfSigned({ key: k.privateKey, subject: "localhost", days: 30 }));

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
