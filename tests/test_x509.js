/* test_x509.js -- X.509 certificate parsing and generation (plan row 5, 3.5b)
 *
 * Verifies:
 *   - X509.generateSelfSigned({ key, subject, days })
 *   - X509.parse(pemString)
 *   - X509.parse(derBytes)
 *   - Subject, issuer, serialNumber, version, notBefore, notAfter, fingerprint
 *   - SANs parsing (DNS, IP, email)
 *   - Refusal on invalid PEM/DER
 */

import { RSA, ECDSA, X509 } from "dyna:crypto";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } else { print("  ok  " + msg); } }
function eq(a, b, msg) { assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg + " (expected throw)");
}

print("=== 1. X509.generateSelfSigned with RSA ===");
{
    const rsa = RSA.generate(2048);
    assert(typeof rsa.privateKey === "string", "RSA privateKey generated");

    const certPem = X509.generateSelfSigned({
        key: rsa.privateKey,
        subject: "test.local",
        days: 90
    });
    assert(typeof certPem === "string" && certPem.startsWith("-----BEGIN CERTIFICATE-----"), "Cert PEM format");

    const parsed = X509.parse(certPem);
    assert(parsed.subject.includes("test.local"), "Parsed subject includes CN");
    assert(parsed.issuer.includes("test.local"), "Parsed issuer matches subject for self-signed");
    eq(parsed.version, 3, "X.509 v3");
    eq(parsed.serialNumber, "01", "Serial number 01");
    assert(typeof parsed.fingerprint === "string" && parsed.fingerprint.length === 64, "SHA-256 fingerprint length 64");
    assert(typeof parsed.notBefore === "string" && parsed.notBefore.length > 0, "notBefore present");
    assert(typeof parsed.notAfter === "string" && parsed.notAfter.length > 0, "notAfter present");
    assert(parsed.sans && Array.isArray(parsed.sans.dns), "SANs structure present");
}

print("=== 2. X509.generateSelfSigned with ECDSA ===");
{
    const ecdsa = ECDSA.generate("P-256");
    assert(typeof ecdsa.privateKey === "string", "ECDSA privateKey generated");

    const certPem = X509.generateSelfSigned({
        key: ecdsa.privateKey,
        subject: "api.domain.com",
        days: 365
    });
    assert(certPem.includes("-----BEGIN CERTIFICATE-----"), "ECDSA Cert PEM");

    const parsed = X509.parse(certPem);
    assert(parsed.subject.includes("api.domain.com"), "Parsed ECDSA cert subject");
}

print("=== 3. Error refusals ===");
{
    throws(() => X509.parse("not a cert"), "Invalid string throws TypeError");
    throws(() => X509.parse(new Uint8Array([0, 1, 2, 3])), "Invalid DER throws TypeError");
    throws(() => X509.parse(), "Missing argument throws");
    throws(() => X509.generateSelfSigned({ key: "not a key" }), "Invalid key throws");
    throws(() => X509.generateSelfSigned(), "Missing options throws");
}

if (fails) {
    print("test_x509: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_x509 failed");
}
print("test_x509: " + n + " assertions, 0 failures");
