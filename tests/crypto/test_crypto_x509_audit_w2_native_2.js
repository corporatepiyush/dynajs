// timeout: 300
// covers: dyna:crypto.X509
// Split from tests/test_audit_w2_native_2.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_x509_audit_w2_native_2");
import * as crypto from "dyna:crypto";
import { Hasher } from "dyna:hash";
import { cpuNow } from "../lib/cpuclock.js";

let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got).slice(0, 80) + ", want " + String(want).slice(0, 80) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
function errMsg(fn) {
    try { fn(); return ""; } catch (e) { return String(e.message); }
}
// ---- D3-03 -----------------------------------------------------------------

__kitS.section("two self-signed certificates get different serial numbers", () => {
    const kp = crypto.Ed25519Generate();
    const keyPem = crypto.Ed25519PemFromRaw(kp).privateKey;
    const a = crypto.X509.parse(crypto.X509.generateSelfSigned({ key: keyPem, subject: "w2.test" }));
    const b = crypto.X509.parse(crypto.X509.generateSelfSigned({ key: keyPem, subject: "w2.test" }));
    ok(a.serialNumber !== b.serialNumber, "two self-signed certificates get different serial numbers");
    ok(/^[0-9A-F]{32}$/.test(a.serialNumber), "the serial is a 16-byte value (got " + a.serialNumber + ")");
    eq(errName(() => crypto.X509.generateSelfSigned({ key: keyPem, sans: ["a.test,DNS:evil.test"] })), "TypeError", "a SAN entry carrying a comma is refused");
    eq(errName(() => crypto.X509.generateSelfSigned({ key: keyPem, sans: ["a.test\nDNS:evil.test"] })), "TypeError", "a SAN entry carrying a line break is refused");
    const okc = crypto.X509.parse(crypto.X509.generateSelfSigned({ key: keyPem, sans: ["a.test", "b.test"] }));
    ok(JSON.stringify(okc).includes("b.test") && !JSON.stringify(okc).includes("evil"), "ordinary SAN lists still work (control)");
});
__kitS.end();
if (failures) {
    print("test_crypto_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_crypto_audit_w2_native_2 failed");
}
print("test_crypto_audit_w2_native_2: " + checks + " checks passed");
