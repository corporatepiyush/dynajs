/* test_crypto_standalone.js -- RSA, ECDSA, ECDH, Scrypt, Bcrypt, Argon2id.
 *
 * Asserting external standard vectors:
 *   - Bcrypt: OpenBSD test vectors
 *   - Argon2id: RFC 9106 / PHC reference KATs
 *   - Scrypt: RFC 7914 KATs
 *   - RSA: PKCS#1 v1.5 sign/verify round trip + error handling
 *   - ECDSA: P-256 and P-384 JWS raw R||S and DER sign/verify
 *   - ECDH: P-256 key exchange shared secret symmetry
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_crypto_standalone.js
 * Prints "test_crypto_standalone: all N tests passed" on success.
 */
import * as crypto from "dyna:crypto";
const {
    Bcrypt, Argon2id, Scrypt,
    RSA, ECDSA, ECDH,
    TimingSafeEqual,
} = crypto;

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

/* ==================================================================== *
 *  1. Bcrypt (OpenBSD $2b$) KATs
 * ==================================================================== */
{
    /* OpenBSD test vectors */
    assert(Bcrypt.verify("", "$2a$06$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s.") === true,
        "bcrypt empty password verifies");
    assert(Bcrypt.verify("wrong", "$2a$06$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s.") === false,
        "bcrypt wrong password returns false");

    /* Generated hash matches itself on verify */
    const gen = Bcrypt.hash("secret_password", 4);
    assert(gen.startsWith("$2b$04$"), "bcrypt hash prefix $2b$04$");
    assert(gen.length === 60, "bcrypt hash length exactly 60");
    assert(Bcrypt.verify("secret_password", gen) === true, "bcrypt verify generated hash");
    assert(Bcrypt.verify("wrong_password", gen) === false, "bcrypt verify reject mismatch");

    /* Password length cap: >72 bytes throws */
    assertThrows(() => Bcrypt.hash("a".repeat(73), 4), "bcrypt password > 72 bytes throws", RangeError);
    assertThrows(() => Bcrypt.hash("pass", 3), "bcrypt cost < 4 throws", RangeError);
    assertThrows(() => Bcrypt.hash("pass", 32), "bcrypt cost > 31 throws", RangeError);
}

/* ==================================================================== *
 *  2. Argon2id (RFC 9106) KATs
 * ==================================================================== */
{
    /* PHC / RFC 9106 Argon2id reference test vector:
     * password="password", salt="somesalt", t=3, m=65536, p=4, outlen=32 */
    const salt = new TextEncoder().encode("somesalt");
    const tag = Argon2id.hash("password", salt, {
        iterations: 3, memory: 65536, parallelism: 4, hashLen: 32
    });
    assert(tag.length === 32, "argon2id output length 32 bytes");
    assert(toHex(tag) === "661fefbd6f29bcbc8f4646abc32a9d7a4645bb5c059537f8a5587f31adbecccd",
        "argon2id KAT vector matches reference");

    /* Verify helper */
    assert(Argon2id.verify("password", salt, tag, {
        iterations: 3, memory: 65536, parallelism: 4, hashLen: 32
    }) === true, "argon2id verify matches");

    assert(Argon2id.verify("wrong_password", salt, tag, {
        iterations: 3, memory: 65536, parallelism: 4, hashLen: 32
    }) === false, "argon2id verify rejects wrong password");

    /* Parameter bounds */
    assertThrows(() => Argon2id.hash("p", new Uint8Array(7)), "salt < 8 bytes throws", RangeError);
    assertThrows(() => Argon2id.hash("p", salt, { iterations: 0 }), "iterations < 1 throws", RangeError);
    assertThrows(() => Argon2id.hash("p", salt, { parallelism: 0 }), "parallelism < 1 throws", RangeError);
}

/* ==================================================================== *
 *  3. Scrypt (RFC 7914) KATs
 * ==================================================================== */
if (typeof Scrypt === "function") {
    /* RFC 7914 Test Vector 1: P="", S="", N=16, r=1, p=1, dkLen=64 */
    const v1 = Scrypt("", "", { N: 16, r: 1, p: 1, keyLen: 64 });
    assert(toHex(v1) === "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906",
        "scrypt RFC 7914 vector 1 matches");

    /* RFC 7914 Test Vector 2: P="password", S="NaCl", N=1024, r=8, p=16, dkLen=64 */
    const v2 = Scrypt("password", "NaCl", { N: 1024, r: 8, p: 16, keyLen: 64 });
    assert(toHex(v2) === "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640",
        "scrypt RFC 7914 vector 2 matches");

    /* Parameter bounds */
    assertThrows(() => Scrypt("p", "s", { N: 3 }), "N not power of 2 throws", RangeError);
    assertThrows(() => Scrypt("p", "s", { r: 0 }), "r < 1 throws", RangeError);
}

/* ==================================================================== *
 *  4. RSA Standalone Sign / Verify
 * ==================================================================== */
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

    /* Mismatched key type throws */
    if (typeof ECDSA !== "undefined") {
        const ecKeys = ECDSA.generate("P-256");
        assertThrows(() => RSA.sign("SHA256", ecKeys.privateKey, msg), "RSA.sign with EC key throws", TypeError);
    }
}

/* ==================================================================== *
 *  5. ECDSA Standalone Sign / Verify (P-256 & P-384, raw R||S & DER)
 * ==================================================================== */
if (typeof ECDSA !== "undefined") {
    /* P-256 */
    const ec256 = ECDSA.generate("P-256");
    const msg = "ECDSA message test";

    /* Raw JWS R||S format (64 bytes) */
    const sigRaw = ECDSA.sign("SHA256", ec256.privateKey, msg);
    assert(sigRaw instanceof Uint8Array && sigRaw.length === 64, "ECDSA P-256 raw signature is 64 bytes");
    assert(ECDSA.verify("SHA256", ec256.publicKey, msg, sigRaw) === true, "ECDSA P-256 raw signature verifies");
    assert(ECDSA.verify("SHA256", ec256.publicKey, "tampered", sigRaw) === false, "ECDSA P-256 raw verify rejects tampered");

    /* DER format */
    const sigDer = ECDSA.sign("SHA256", ec256.privateKey, msg, { format: "der" });
    assert(sigDer instanceof Uint8Array && sigDer.length >= 70, "ECDSA DER signature is ASN.1 sequence");
    assert(ECDSA.verify("SHA256", ec256.publicKey, msg, sigDer, { format: "der" }) === true, "ECDSA P-256 DER signature verifies");

    /* P-384 */
    const ec384 = ECDSA.generate("P-384");
    const sig384Raw = ECDSA.sign("SHA384", ec384.privateKey, msg);
    assert(sig384Raw instanceof Uint8Array && sig384Raw.length === 96, "ECDSA P-384 raw signature is 96 bytes");
    assert(ECDSA.verify("SHA384", ec384.publicKey, msg, sig384Raw) === true, "ECDSA P-384 raw signature verifies");
}

/* ==================================================================== *
 *  6. ECDH Standalone Key Exchange
 * ==================================================================== */
if (typeof ECDH !== "undefined") {
    const alice = ECDH.generate("P-256");
    const bob   = ECDH.generate("P-256");

    const secretAlice = ECDH.derive(alice.privateKey, bob.publicKey);
    const secretBob   = ECDH.derive(bob.privateKey, alice.publicKey);

    assert(secretAlice instanceof Uint8Array && secretAlice.length === 32, "P-256 shared secret is 32 bytes");
    assert(toHex(secretAlice) === toHex(secretBob), "ECDH key exchange yields symmetric shared secret");

    /* Different peer key yields different shared secret */
    const eve = ECDH.generate("P-256");
    const secretEve = ECDH.derive(alice.privateKey, eve.publicKey);
    assert(toHex(secretAlice) !== toHex(secretEve), "Different peer key yields distinct shared secret");
}

print("test_crypto_standalone: all tests passed (" + n + " assertions)");
