// covers: dyna:crypto.RSA
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_rsa_standalone");
import {
    Bcrypt, Argon2id, Scrypt,
    RSA, ECDSA, ECDH,
    TimingSafeEqual,
} from "dyna:crypto";

let n = 0;
function assert(c, msg) {
    n++;
    if (!c) throw new Error("assertion failed: " + msg);
}
function assertThrows(fn, msg, ErrType) {
    n++;
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
    if (ErrType && !(err instanceof ErrType))
        throw new Error("assertion failed (wrong error type, got " + err + "): " + msg);
}
function toHex(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i++) s += u8[i].toString(16).padStart(2, "0");
    return s;
}
function u8(...bytes) { return new Uint8Array(bytes); }

__kitS.section("BEGIN PRIVATE KEY", () => {
if (typeof RSA !== "undefined") {
    const keys = RSA.generate(2048);
    assert(typeof keys.privateKey === "string" && keys.privateKey.indexOf("BEGIN PRIVATE KEY") >= 0,
        "RSA privateKey is PEM");
    assert(typeof keys.publicKey === "string" && keys.publicKey.indexOf("BEGIN PUBLIC KEY") >= 0,
        "RSA publicKey is PEM");

    const msg = "Hello RSA standalone signature test!";
    const sig = RSA.sign("SHA256", keys.privateKey, msg);
    assert(sig instanceof Uint8Array && sig.length === 256, "RSA 2048 signature is 256 bytes");

    assert(RSA.verify("SHA256", keys.publicKey, msg, sig) === true, "RSA signature verifies with correct public key");
    assert(RSA.verify("SHA256", keys.publicKey, "Tampered message", sig) === false, "RSA verify rejects tampered message");

    if (typeof ECDSA !== "undefined") {
        const ecKeys = ECDSA.generate("P-256");
        assertThrows(() => RSA.sign("SHA256", ecKeys.privateKey, msg), "RSA.sign with EC key throws", TypeError);
    }
}
});
__kitS.end();

print("test_crypto_standalone: all tests passed (" + n + " assertions)");
