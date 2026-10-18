// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_2.js: the crypto sections.
import * as crypto from "dyna:crypto";
import { Hasher } from "dyna:hash";

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
{
    const tok = crypto.JWTSign({ a: 1 }, "undefined", { alg: "HS256" });
    for (const [label, bad] of [["undefined", undefined], ["null", null], ["number", 5], ["object", {}], ["boolean", true]]) {
        eq(errName(() => crypto.JWTVerify(tok, bad, { algorithms: ["HS256"] })), "TypeError", "JWTVerify refuses a " + label + " key");
        eq(errName(() => crypto.JWTSign({ a: 1 }, bad, { alg: "HS256" })), "TypeError", "JWTSign refuses a " + label + " key");
        eq(errName(() => crypto.HMACHex("sha256", bad, "x")), "TypeError", "HMACHex refuses a " + label + " key");
        eq(errName(() => new crypto.Hmac("sha256", bad)), "TypeError", "Hmac refuses a " + label + " key");
        eq(errName(() => crypto.TOTPGenerate(bad)), "TypeError", "TOTPGenerate refuses a " + label + " secret");
        eq(errName(() => crypto.TimingSafeEqual(bad, "undefined")), "TypeError", "TimingSafeEqual refuses a " + label);
    }
    eq(JSON.stringify(crypto.JWTVerify(tok, "undefined", { algorithms: ["HS256"] })), '{"a":1}', "a real string key still verifies (control)");
    eq(crypto.HMACHex("sha256", new Uint8Array([1, 2, 3]), "x").length, 64, "a byte-view key still works (control)");
    eq(errName(() => crypto.JWTVerify(crypto.JWTSign({ a: 1 }, "k", { alg: "HS256" }), "", { algorithms: ["HS256"] })), "TypeError", "JWTVerify refuses an empty HMAC key");
    eq(crypto.SHA256Hex({ toString() { return "q"; } }), crypto.SHA256Hex("q"), "DATA arguments keep their string coercion (control)");
}
// ---- D3-04 / D3-09 -----------------------------------------------------------
{
    const kp = crypto.RSA.generate ? crypto.RSA.generate(2048) : null;
    if (kp) {
        const forged = crypto.JWTSign({ admin: true }, kp.publicKey, { alg: "HS256" });
        eq(errName(() => crypto.JWTVerify(forged, kp.publicKey, { algorithms: ["RS256", "HS256"] })), "TypeError", "an HS256 token keyed with the RSA public PEM is refused");
        const good = crypto.JWTSign({ u: 1 }, kp.privateKey, { alg: "RS256" });
        eq(JSON.stringify(crypto.JWTVerify(good, kp.publicKey, { algorithms: ["RS256", "HS256"] })), '{"u":1}', "a real RS256 token verifies under the mixed allowlist (control)");
    } else {
        print("  note: RSA.generate missing, D3-04 case skipped");
    }
    const t = crypto.JWTSign({ a: 1 }, "secret-key", { alg: "HS256" });
    const parts = t.split(".");
    const sig = parts[2];
    const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const last = B64.indexOf(sig[sig.length - 1]);
    const twin = sig.slice(0, -1) + B64[last ^ 1];
    eq(sig.length % 4, 3, "an HS256 signature leaves two unused bits (precondition of the malleability case)");
    eq(errName(() => crypto.JWTVerify(parts[0] + "." + parts[1] + "." + twin, "secret-key", { algorithms: ["HS256"] })), "TypeError", "a signature with flipped unused bits is refused");
    eq(errName(() => crypto.JWTVerify(t + "=", "secret-key", { algorithms: ["HS256"] })), "TypeError", "a padded signature is refused");
    eq(JSON.stringify(crypto.JWTVerify(t, "secret-key", { algorithms: ["HS256"] })), '{"a":1}', "the canonical token verifies (control)");

    const b64u = (s) => { const bytes = new TextEncoder().encode(s); let bin = ""; for (const b of bytes) bin += String.fromCharCode(b); return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, ""); };
    const head = b64u(JSON.stringify({ alg: "HS256", typ: "JWT", crit: ["exp2"] }));
    const body = b64u(JSON.stringify({ a: 1 }));
    const mac = crypto.HMAC("sha256", "secret-key", head + "." + body);
    let macBin = ""; for (const b of mac) macBin += String.fromCharCode(b);
    const critTok = head + "." + body + "." + btoa(macBin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
    ok(/crit/.test(errMsg(() => crypto.JWTVerify(critTok, "secret-key", { algorithms: ["HS256"] }))), "a correctly signed token carrying `crit` is refused for that reason");
}
// ---- D3-02 / D3-11 -------------------------------------------------------------
{
    const ed = crypto.Ed25519Generate();
    const pem = crypto.Ed25519PemFromRaw(ed).privateKey;
    const viaObj = crypto.Ed25519PemToRaw({ toString() { return pem + "\n"; } });
    ok(viaObj && typeof viaObj === "object", "Ed25519PemToRaw reads a PEM that only exists as a temporary C string");
    eq(errName(() => crypto.Ed25519PemToRaw(pem + "é")), "none", "a PEM followed by a non-ASCII character parses without touching freed memory");
    const ENC = "-----BEGIN ENCRYPTED PRIVATE KEY-----\nMIGjMF8GCSqGSIb3DQEFDTBSMDEGCSqGSIb3DQEFDDAkBBBvt0gDhtbIhWDbcG7e\nwmW8AgIIADAMBggqhkiG9w0CCQUAMB0GCWCGSAFlAwQBKgQQyvWn9XrCvHSfy1j3\nxgC1AARAU7lO0J/7Wz5N0kRkXh0hYp2aIl2kUlqDlPxxG6h8Ef4Mh0Cb0qHxE9bS\nR3nWZ5ZlVwJ5gq7W6m8xUuUuGwqzZQ==\n-----END ENCRYPTED PRIVATE KEY-----\n";
    const t0 = performance.now();
    eq(errName(() => crypto.Ed25519PemToRaw(ENC)), "TypeError", "an encrypted PEM is refused");
    ok(performance.now() - t0 < 2000, "and refused promptly, with no passphrase prompt");
}
// ---- D3-08 -----------------------------------------------------------------------
{
    const mk = () => { const h = new Hasher("shake128", { length: 300 }); h.update("abc"); return h; };
    const raw = mk().digest();
    let hex = "";
    for (const b of raw) hex += (b < 16 ? "0" : "") + b.toString(16);
    eq(raw.length, 300, "SHAKE128 digest() honours a 300-byte length (control)");
    eq(mk().digestHex(), hex, "digestHex() returns the same 300 bytes as digest()");
}
// ---- D3-13 -----------------------------------------------------------------------
{
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
}
if (failures) {
    print("test_crypto_audit_w2_native_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_crypto_audit_w2_native_2 failed");
}
print("test_crypto_audit_w2_native_2: " + checks + " checks passed");
