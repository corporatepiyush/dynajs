import { Redis, PostgreSQL, TCPServer } from "dyna:net";
import * as crypto from "dyna:crypto";
import { makeTempDir, writeFile, Path, removeAll } from "dyna:file";

const HAS_TLS = !!crypto.RSA && !!crypto.X509 &&
                typeof crypto.RSA.generate === "function" &&
                typeof crypto.X509.generateSelfSigned === "function";

if (!HAS_TLS) {
    throw new Error("test_client_tls: TLS (dyna:crypto RSA/X509, OpenSSL 3.5) is a hard dependency of every native build, and this binary lacks it");
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

    const echo = new TCPServer({ port: 0, tls: { cert: T + "/c.pem", key: T + "/k.pem" } });
    echo.start({ data(c, b) { c.write(b); } });

    let r;
    try { r = new Redis({ host: "localhost", port: echo.port, tls: true,
                          ca: T + "/c.pem", connectTimeoutMs: 5000 }); }
    catch (e) { ok(false, "Redis tls:true threw: " + e.message); }
    ok(typeof r === "object", "Redis tls:true constructs");
    r.on("error", () => {});

    let pg;
    try { pg = new PostgreSQL({ host: "localhost", port: echo.port, tls: true,
                                ca: T + "/c.pem", connectTimeoutMs: 5000 }); }
    catch (e) { ok(false, "PostgreSQL tls:true threw: " + e.message); }
    ok(typeof pg === "object", "PostgreSQL tls:true constructs");
    pg.on("error", () => {});

    const r2 = new Redis({ host: "localhost", port: echo.port, tls: true,
                           connectTimeoutMs: 5000 });
    let tlsVerifyRefused = false;
    r2.on("error", (e) => {
        if (/TLS|certificate|handshake/i.test(String(e))) tlsVerifyRefused = true;
    });

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
