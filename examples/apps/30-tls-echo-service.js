// 30 · TLS service with a private CA — generates a certificate, serves over TLS, and pins it on the client.
//
// WHAT IT SHOWS
//   - dyna:crypto ECDSA.generate + X509.generateSelfSigned: a certificate with proper subjectAltNames
//   - dyna:net TCPServer with TLS, and a client that trusts ONLY the pinned certificate
//   - verification failing closed: an unpinned client and a wrong hostname are both refused
//   - reading certificate details to log what is being served and when it expires
//
// RUN      dynajs examples/apps/30-tls-echo-service.js
// DEPLOY   Replace the generated pair with files from your CA; everything else stays the same.

import { TCPServer } from "dyna:net";
import { ECDSA, X509 } from "dyna:crypto";
import { makeTempDir, writeFile, removeAll } from "dyna:file";

// ---- certificate -----------------------------------------------------------
// The names a client may connect by go in `sans`. Hostname verification reads
// ONLY subjectAltName; a certificate with just a common name fails every modern client.
const key = ECDSA.generate("P-256");
const cert = X509.generateSelfSigned({
    key: key.privateKey,
    subject: "echo.internal",                 // the common name; SANs carry the real identities
    days: 30,
    sans: ["echo.internal", "localhost", "127.0.0.1"],
});
const info = X509.parse(cert);
console.log("serving certificate:", JSON.stringify({ subject: info.subject, notAfter: info.notAfter }));

// The TLS options take file paths, as they would in production.
const dir = makeTempDir("tls");
const certPath = dir.join("server.crt"), keyPath = dir.join("server.key");
writeFile(certPath, cert);
writeFile(keyPath, key.privateKey);

// ---- server: upper-cases each line it receives ------------------------------
const server = new TCPServer({ port: 0, tls: { cert: String(certPath), key: String(keyPath) } });
server.start({
    data: (conn, bytes) => conn.write(new TextDecoder().decode(bytes).toUpperCase()),
});
console.log("TLS echo on port", server.port);

// ---- client helper ---------------------------------------------------------
// Resolves with the reply, or rejects with the handshake error.
function request(text, tls, timeoutMs = 2000) {
    return new Promise((resolve, reject) => {
        let settled = false;
        const finish = (fn, value) => { if (!settled) { settled = true; client.close(); fn(value); } };
        const client = TCPServer.connect({ host: "127.0.0.1", port: server.port, tls, connectTimeoutMs: timeoutMs }, {
            connect: (conn, err) => (err ? finish(reject, new Error(err)) : conn.write(text)),
            data: (_conn, bytes) => finish(resolve, new TextDecoder().decode(bytes)),
            close: () => finish(reject, new Error("connection closed")),
        });
        setTimeout(() => finish(reject, new Error("timeout")), timeoutMs);
    });
}

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const failure = (p) => p.then(() => "", (e) => e.message || "failed");

// Pinned: trust this one certificate, verify the name "localhost".
const pinned = { ca: String(certPath), servername: "localhost" };
check(await request("hello over tls\n", pinned) === "HELLO OVER TLS\n", "a pinned client talks to the server");

// The IP literal is in the SANs too.
check(await request("by ip\n", { ca: String(certPath), servername: "127.0.0.1" }) === "BY IP\n", "an IP SAN verifies");

// No pin: the certificate is not signed by any system CA, so it is refused.
check((await failure(request("x\n", { servername: "localhost" }))) !== "", "an unpinned client refuses the self-signed certificate");

// Right certificate, wrong name: the SAN list does not contain it.
check((await failure(request("x\n", { ca: String(certPath), servername: "billing.internal" }))) !== "",
      "a name outside the SANs is refused");

// A different (equally self-signed) CA does not validate this server.
const otherKey = ECDSA.generate("P-256");
const otherPath = dir.join("other.crt");
writeFile(otherPath, X509.generateSelfSigned({ key: otherKey.privateKey, subject: "localhost", sans: ["localhost"] }));
check((await failure(request("x\n", { ca: String(otherPath), servername: "localhost" }))) !== "",
      "pinning a different certificate refuses this server");

console.log("self-test passed: pinned success; unpinned, wrong-name and wrong-CA all refused");
server.close();
removeAll(dir);
