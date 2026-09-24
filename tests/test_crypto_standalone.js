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
    /*default form is the PHC string; the raw KAT is {encoded:false} */
    assert(typeof Argon2id.hash("password", salt, {
        iterations: 3, memory: 65536, parallelism: 4, hashLen: 32
    }) === "string", "argon2id default form is the PHC string");
    const tag = Argon2id.hash("password", salt, {
        iterations: 3, memory: 65536, parallelism: 4, hashLen: 32, encoded: false
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

/* DER ECDSA-Sig-Value (RFC 3279 / X.690): SEQUENCE { INTEGER r, INTEGER s }.
   Assert the STRUCTURE, never a length constant: the DER length of a P-256
   signature legitimately ranges over 8..72 bytes (r and s are minimal
   two's-complement INTEGERs of 1..33 bytes), so any fixed lower bound
   eventually rejects a real signature. Returns null when valid, or a reason
   when not. `qBytes` is the curve's field size in bytes. */
function derEcdsaSigCheck(sig, qBytes) {
    if (!(sig instanceof Uint8Array)) return "not a Uint8Array";
    if (sig.length < 2) return "too short for any TLV";
    if (sig[0] !== 0x30) return "not a DER SEQUENCE";
    let off = 1, len = sig[off++];
    if (len & 0x80) {
        const nlen = len & 0x7f;
        if (nlen === 0 || nlen > 2) return "bad long-form length size";
        if (off + nlen > sig.length) return "length octets past the end";
        if (sig[off] === 0x00) return "non-minimal length encoding";
        len = 0;
        for (let k = 0; k < nlen; k++) len = (len << 8) | sig[off++];
        if (nlen === 1 && len < 0x80) return "long form used for a short length";
        if (nlen === 2 && len <= 0xff) return "long form padded to two octets";
    }
    if (off + len !== sig.length) return "SEQUENCE length does not cover the buffer";
    for (const name of ["r", "s"]) {
        if (off + 2 > sig.length) return "missing INTEGER " + name;
        if (sig[off] !== 0x02) return name + " is not a DER INTEGER";
        const ilen = sig[off + 1];
        if (ilen & 0x80) return name + " has a long-form length";
        off += 2;
        if (ilen === 0) return name + " is empty";
        if (off + ilen > sig.length) return name + " runs past the SEQUENCE";
        const first = sig[off];
        if (first & 0x80) return name + " is negative (high bit set)";
        if (ilen >= 2 && first === 0x00 && !(sig[off + 1] & 0x80))
            return name + " has a redundant leading zero";
        if (ilen > qBytes + 1) return name + " is wider than the field";
        if (ilen === qBytes + 1 && first !== 0x00) return name + " overflows the field";
        off += ilen;
    }
    if (off !== sig.length) return "trailing bytes after the INTEGER pair";
    return null;
}

if (typeof ECDSA !== "undefined") {
    /* P-256 */
    const ec256 = ECDSA.generate("P-256");
    const msg = "ECDSA message test";

    /* Raw JWS R||S format (64 bytes) */
    const sigRaw = ECDSA.sign("SHA256", ec256.privateKey, msg);
    assert(sigRaw instanceof Uint8Array && sigRaw.length === 64, "ECDSA P-256 raw signature is 64 bytes");
    assert(ECDSA.verify("SHA256", ec256.publicKey, msg, sigRaw) === true, "ECDSA P-256 raw signature verifies");
    assert(ECDSA.verify("SHA256", ec256.publicKey, "tampered", sigRaw) === false, "ECDSA P-256 raw verify rejects tampered");

    /* DER format: the structure property, not a length constant */
    const sigDer = ECDSA.sign("SHA256", ec256.privateKey, msg, { format: "der" });
    assert(sigDer instanceof Uint8Array, "ECDSA DER signature is bytes");
    const badDer = derEcdsaSigCheck(sigDer, 32);
    assert(badDer === null, "ECDSA DER signature has valid TLV structure (got: " + badDer + ")");
    assert(ECDSA.verify("SHA256", ec256.publicKey, msg, sigDer, { format: "der" }) === true, "ECDSA P-256 DER signature verifies");

    /* Structural boundary matrix over hand-built DER: every length shape a
       real signer emits is accepted; every malformed shape is caught. A
       length-constant assert fails on the 68/69-byte members of this matrix
       even though they are exactly what minimal INTEGERs produce. */
    const derParts = (rLead, rExtra, sLead, sExtra) => {
        const r = [rLead].concat(new Array(rExtra).fill(0x91));
        const s = [sLead].concat(new Array(sExtra).fill(0xA2));
        const body = [0x02, r.length].concat(r, [0x02, s.length], s);
        return new Uint8Array([0x30, body.length].concat(body));
    };
    const shapes = [
        [derParts(0x01, 31, 0x01, 31), null, "70: both INTEGERs 32 bytes"],
        [derParts(0x00, 32, 0x00, 32), null, "72: both padded to 33"],
        [derParts(0x00, 32, 0x01, 31), null, "71: only r padded"],
        [derParts(0x01, 31, 0x01, 30), null, "69: s is a 31-byte magnitude"],
        [derParts(0x01, 30, 0x01, 30), null, "68: both magnitudes 31 bytes"],
        [derParts(0x01, 0, 0x01, 0), null, "minimal: both INTEGERs one byte"],
        [new Uint8Array([0x30, 6, 0x02, 1, 0x01, 0x02, 1, 0x01]), null,
         "8: minimal short-form frame"],
        [new Uint8Array([0x31, 6, 0x02, 1, 0x01, 0x02, 1, 0x01]), "SEQUENCE",
         "wrong outer tag"],
        [new Uint8Array([0x30, 6, 0x02, 1, 0x01, 0x02, 1, 0x80]), "negative",
         "s has the sign bit set"],
        [new Uint8Array([0x30, 7, 0x02, 2, 0x00, 0x01, 0x02, 1, 0x01]), "redundant",
         "r has a redundant leading zero"],
        [new Uint8Array([0x30, 5, 0x02, 1, 0x01, 0x02, 2]), "past",
         "s runs past the SEQUENCE"],
        [new Uint8Array([0x30, 4, 0x02, 1, 0x01, 0x02, 1, 0x01]), "cover",
         "SEQUENCE length short of the buffer"],
        [new Uint8Array([0x30, 0x81, 6, 0x02, 1, 0x01, 0x02, 1, 0x01]), "long form",
         "non-minimal long-form length"],
        [new Uint8Array([0x30, 7, 0x02, 0x81, 1, 0x01, 0x02, 1, 0x01]), "long-form",
         "long-form INTEGER length"],
    ];
    for (const [buf, wantBad, why] of shapes) {
        const got = derEcdsaSigCheck(buf, 32);
        if (wantBad === null)
            assert(got === null, "valid DER shape accepted: " + why + " (got: " + got + ")");
        else
            assert(got !== null && got.indexOf(wantBad) >= 0,
                   "invalid DER shape refused for " + wantBad + ": " + why + " (got: " + got + ")");
    }

    /* Statistical run: N fresh signatures, every one checked structurally and
       verified. The DER length distribution spans 68..72 for P-256 whenever
       r/s have leading zero bytes in their magnitudes, which is exactly the
       shape a length constant gets wrong. Override with
       DYNAJS_DER_SIG_RUNS for a longer soak. */
    const runs = (function () {
        const env = (typeof std !== "undefined" && std.getenv)
            ? std.getenv("DYNAJS_DER_SIG_RUNS") : null;
        const n = env ? parseInt(env, 10) : 5000;
        return (n > 0 && n < 1000000) ? n : 5000;
    })();
    let minDer = 1 << 30, maxDer = 0, shortSeen = 0;
    for (let i = 0; i < runs; i++) {
        const s = ECDSA.sign("SHA256", ec256.privateKey, msg + i, { format: "der" });
        const bad = derEcdsaSigCheck(s, 32);
        assert(bad === null, "sig #" + i + " structure (got: " + bad + ")");
        assert(ECDSA.verify("SHA256", ec256.publicKey, msg + i, s, { format: "der" }) === true,
               "sig #" + i + " verifies");
        if (s.length < minDer) minDer = s.length;
        if (s.length > maxDer) maxDer = s.length;
        if (s.length < 70) shortSeen++;
    }
    assert(minDer >= 8 && maxDer <= 72,
           "DER length distribution within the structural bounds [" + minDer + ".." + maxDer + "]");
    if (shortSeen > 0)
        print("  note: " + shortSeen + "/" + runs + " DER signatures shorter than 70 bytes (legal minimal INTEGERs)");
    else
        print("  note: no sub-70-byte DER in " + runs + " signatures (p~" +
              (Math.pow(0.9985, runs)).toFixed(4) + " under the ~0.15% rate)");

    /* P-384 */
    const ec384 = ECDSA.generate("P-384");
    const sig384Raw = ECDSA.sign("SHA384", ec384.privateKey, msg);
    assert(sig384Raw instanceof Uint8Array && sig384Raw.length === 96, "ECDSA P-384 raw signature is 96 bytes");
    assert(ECDSA.verify("SHA384", ec384.publicKey, msg, sig384Raw) === true, "ECDSA P-384 raw signature verifies");
    const sig384Der = ECDSA.sign("SHA384", ec384.privateKey, msg, { format: "der" });
    const bad384 = derEcdsaSigCheck(sig384Der, 48);
    assert(bad384 === null, "ECDSA P-384 DER signature has valid TLV structure (got: " + bad384 + ")");
    assert(ECDSA.verify("SHA384", ec384.publicKey, msg, sig384Der, { format: "der" }) === true,
           "ECDSA P-384 DER signature verifies");
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
