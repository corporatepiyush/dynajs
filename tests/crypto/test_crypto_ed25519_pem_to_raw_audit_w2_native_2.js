// timeout: 300
// covers: dyna:crypto.Ed25519PemToRaw
// Split from tests/test_audit_w2_native_2.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_ed25519_pem_to_raw_audit_w2_native_2");
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

__kitS.section("Ed25519PemToRaw reads a PEM that only exists as a temporary C string", () => {
    const ed = crypto.Ed25519Generate();
    const pem = crypto.Ed25519PemFromRaw(ed).privateKey;
    const viaObj = crypto.Ed25519PemToRaw({ toString() { return pem + "\n"; } });
    ok(viaObj && typeof viaObj === "object", "Ed25519PemToRaw reads a PEM that only exists as a temporary C string");
    eq(errName(() => crypto.Ed25519PemToRaw(pem + "é")), "none", "a PEM followed by a non-ASCII character parses without touching freed memory");
    const ENC = "-----BEGIN ENCRYPTED PRIVATE KEY-----\nMIGjMF8GCSqGSIb3DQEFDTBSMDEGCSqGSIb3DQEFDDAkBBBvt0gDhtbIhWDbcG7e\nwmW8AgIIADAMBggqhkiG9w0CCQUAMB0GCWCGSAFlAwQBKgQQyvWn9XrCvHSfy1j3\nxgC1AARAU7lO0J/7Wz5N0kRkXh0hYp2aIl2kUlqDlPxxG6h8Ef4Mh0Cb0qHxE9bS\nR3nWZ5ZlVwJ5gq7W6m8xUuUuGwqzZQ==\n-----END ENCRYPTED PRIVATE KEY-----\n";
    const t0 = cpuNow();
    eq(errName(() => crypto.Ed25519PemToRaw(ENC)), "TypeError", "an encrypted PEM is refused");
    ok(cpuNow() - t0 < 2000, "and refused promptly, with no passphrase prompt");
});
__kitS.end();
if (failures) {
    print("test_crypto_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_crypto_audit_w2_native_2 failed");
}
print("test_crypto_audit_w2_native_2: " + checks + " checks passed");
