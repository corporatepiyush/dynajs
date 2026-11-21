// covers: integration dyna:redis.Redis dyna:postgres.PostgreSQL -- TLS client handshake of both db clients against one TLS echo server
// The TLS and plaintext echo servers start in the kit's before hook (never
// under --list); each case builds its own client and closes it.
import { TCPServer } from "dyna:net";
import { Redis } from "dyna:redis";
import { PostgreSQL } from "dyna:postgres";
import * as crypto from "dyna:crypto";
import { makeTempDir, writeFile, Path, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";

const HAS_TLS = !!crypto.RSA && !!crypto.X509 &&
                typeof crypto.RSA.generate === "function" &&
                typeof crypto.X509.generateSelfSigned === "function";
if (!HAS_TLS)
    throw new Error("test_client_tls: TLS (dyna:crypto RSA/X509, OpenSSL 3.5) is a hard dependency of every native build, and this binary lacks it");

const t = suite("test_client_tls");
let A = null;
const ok = (c, w) => A.ok(c, w);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);

let T = null, echo = null, plain = null;
const clients = [];
t.before(() => {
    T = makeTempDir("c9tls");
    const k = crypto.RSA.generate(2048);
    writeFile(new Path(T, "k.pem"), k.privateKey);
    writeFile(new Path(T, "c.pem"),
        crypto.X509.generateSelfSigned({ key: k.privateKey, subject: "localhost", days: 30 }));
    echo = new TCPServer({ port: 0, tls: { cert: T + "/c.pem", key: T + "/k.pem" } });
    echo.start({ data(c, b) { c.write(b); } });
    plain = new TCPServer({ port: 0 });
    plain.start({ data(c, b) { c.write(b); } });
});
t.after(() => {
    for (const c of clients) { try { c.close(); } catch (e) { } }
    if (echo) echo.close();
    if (plain) plain.close();
    if (T) removeAll(new Path(T));
});

test("Redis tls:true with a CA constructs", () => {
    let r;
    try { r = new Redis({ host: "localhost", port: echo.port, tls: true,
                          ca: T + "/c.pem", connectTimeoutMs: 5000 }); }
    catch (e) { ok(false, "Redis tls:true threw: " + e.message); }
    ok(typeof r === "object", "Redis tls:true constructs");
    if (r) { r.on("error", () => {}); clients.push(r); }
});

test("PostgreSQL tls:true with a CA constructs", () => {
    let pg;
    try { pg = new PostgreSQL({ host: "localhost", port: echo.port, tls: true,
                                ca: T + "/c.pem", connectTimeoutMs: 5000 }); }
    catch (e) { ok(false, "PostgreSQL tls:true threw: " + e.message); }
    ok(typeof pg === "object", "PostgreSQL tls:true constructs");
    if (pg) { pg.on("error", () => {}); clients.push(pg); }
});

test("plaintext Redis constructs alongside TLS", () => {
    const r3 = new Redis({ host: "127.0.0.1", port: plain.port, connectTimeoutMs: 5000 });
    r3.on("error", () => {});
    clients.push(r3);
    ok(true, "plaintext Redis constructs alongside TLS");
});

test("no-CA TLS is refused by verification (self-signed)", async () => {
    const r2 = new Redis({ host: "localhost", port: echo.port, tls: true,
                           connectTimeoutMs: 5000 });
    clients.push(r2);
    let tlsVerifyRefused = false;
    r2.on("error", (e) => {
        if (/TLS|certificate|handshake/i.test(String(e))) tlsVerifyRefused = true;
    });
    for (let i = 0; i < 80 && !tlsVerifyRefused; i++) await new Promise((r) => setTimeout(r, 50));
    ok(tlsVerifyRefused, "no-CA TLS is refused by verification (self-signed)");
});

await t.run();
