/* test_hash_split.js -- the dyna:hash / dyna:crypto split and what is new in
 * each half (W4).
 *
 * The split is by WHAT THE OPERATION IS FOR, not by algorithm family: a
 * one-way reduction with no secret is `dyna:hash`, anything that depends on a
 * secret or on constant-time execution is `dyna:crypto`. This file asserts the
 * split is real (each module exports its half and not the other's) and then
 * tests the four operations that only exist because of it.
 *
 * Every new primitive is checked against a PUBLISHED vector, never against
 * itself: RFC 5869 for HKDF, RFC 6070 for PBKDF2, the xxHash spec for xxhash.
 * A KDF that round-trips with itself is a KDF that agrees with its own bugs,
 * and the failure is silent -- you get a perfectly good-looking key that no
 * other implementation will derive.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_hash_split.js */

import * as hash from "dyna:hash";
import * as crypto from "dyna:crypto";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function throws(fn, msg) {
    n++;
    try { fn(); } catch (e) { return; }
    throw new Error("assertion failed: " + msg + " did not throw");
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));

/* ------------------------------------------------------------- 1. the split */
/* These strings are EXPORT NAMES, not algorithm arguments -- the ones inside
 * new Hasher("sha256") stay lowercase because the lookup is strcmp. */
for (const name of ["MD5", "MD5Hex", "SHA1", "SHA256", "SHA256Hex", "SHA512",
                    "CRC32", "CRC32C", "XXHash64", "XXHash32", "Hasher"]) {
    assert(name in hash, "dyna:hash exports " + name);
    assert(!(name in crypto), "dyna:crypto does NOT export " + name +
           " -- an unkeyed reduction is not a crypto operation");
}
for (const name of ["HMAC", "HMACHex", "Hmac", "TimingSafeEqual", "HKDF",
                    "PBKDF2", "RandomBytes"]) {
    assert(name in crypto, "dyna:crypto exports " + name);
    assert(!(name in hash), "dyna:hash does NOT export " + name +
           " -- it depends on a secret");
}

/* --------------------------------------------------------------- 2. xxhash
 * Published vectors from the xxHash specification. XXHash64 returns a HEX
 * STRING, deliberately: a JS number carries 53 exact bits and the value has 64,
 * so returning a number would silently collide in ways the algorithm does not. */
eq(typeof hash.XXHash64(""), "string", "XXHash64 returns a string");
eq(hash.XXHash64(""), "ef46db3751d8e999", "XXH64('') seed 0");
eq(hash.XXHash64("a"), "d24ec4f1a98c6e5b", "XXH64('a') seed 0");
eq(hash.XXHash64("abc"), "44bc2cf5ad770999", "XXH64('abc') seed 0");
eq(hash.XXHash32("abc"), 0x32d153ff, "XXH32('abc') seed 0");
eq(hash.XXHash32(""), 0x02cc5d05, "XXH32('') seed 0");
eq(hash.XXHash32("a"), 0x550d7456, "XXH32('a') seed 0");
assert(hash.XXHash64("abc", 1) !== hash.XXHash64("abc", 0), "the seed matters");
eq(hash.XXHash64(new TextEncoder().encode("abc")), hash.XXHash64("abc"),
   "a string and its UTF-8 bytes hash identically");

/* ------------------------------------------------------------ 3. timingSafe */
{
    const a = bytes("00112233445566778899aabbccddeeff");
    const b = bytes("00112233445566778899aabbccddeeff");
    const c = bytes("00112233445566778899aabbccddee00");   /* differs at the END */
    const d = bytes("ff112233445566778899aabbccddeeff");   /* differs at the START */
    assert(crypto.TimingSafeEqual(a, b), "equal buffers compare equal");
    assert(!crypto.TimingSafeEqual(a, c), "a trailing difference is caught");
    assert(!crypto.TimingSafeEqual(a, d), "a leading difference is caught");
    assert(!crypto.TimingSafeEqual(a, a.slice(0, 8)),
           "different lengths are unequal (a MAC's length is public)");
    assert(crypto.TimingSafeEqual("abc", "abc"), "strings work too");
    assert(crypto.TimingSafeEqual(new Uint8Array(0), new Uint8Array(0)),
           "two empty inputs are equal");
    /* The property that matters cannot be asserted from JS -- it is that the
     * loop has no early exit, which is checked by reading the generated asm
     * (STDLIB_OOP_PLAN, W4). What CAN be asserted is that the answer does not
     * depend on where the difference is. */
    for (let i = 0; i < a.length; i++) {
        const m = new Uint8Array(a);
        m[i] ^= 0xff;
        assert(!crypto.TimingSafeEqual(a, m),
               "a difference at byte " + i + " is caught");
    }
}

/* --------------------------------------------------------- 4. HKDF, RFC 5869 */
{
    /* Test Case 1: SHA-256, 22-byte IKM, 13-byte salt, 10-byte info */
    const okm = crypto.HKDF({
        hash: "sha256",
        key: new Uint8Array(22).fill(0x0b),
        salt: bytes("000102030405060708090a0b0c"),
        info: bytes("f0f1f2f3f4f5f6f7f8f9"),
        length: 42,
    });
    eq(hex(okm),
       "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf" +
       "34007208d5b887185865",
       "RFC 5869 test case 1");

    /* Test Case 2: the long one -- 80-byte inputs, 82 bytes out, so the expand
     * loop runs three times and the T(n-1) chaining is actually exercised */
    const ikm2 = new Uint8Array(80).map((_, i) => i);
    const salt2 = new Uint8Array(80).map((_, i) => 0x60 + i);
    const info2 = new Uint8Array(80).map((_, i) => 0xb0 + i);
    const okm2 = crypto.HKDF({ hash: "sha256", key: ikm2, salt: salt2,
                               info: info2, length: 82 });
    eq(hex(okm2),
       "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c" +
       "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71" +
       "cc30c58179ec3e87c14c01d5c1f3434f1d87",
       "RFC 5869 test case 2 (multi-block expand)");

    /* Test Case 3: no salt, no info -- the zero-salt branch */
    const okm3 = crypto.HKDF({ hash: "sha256", key: new Uint8Array(22).fill(0x0b),
                               length: 42 });
    eq(hex(okm3),
       "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d" +
       "9d201395faa4b61a96c8",
       "RFC 5869 test case 3 (no salt, no info)");

    /* Test Case 4: SHA-1 */
    const okm4 = crypto.HKDF({ hash: "sha1", key: new Uint8Array(11).fill(0x0b),
                               salt: bytes("000102030405060708090a0b0c"),
                               info: bytes("f0f1f2f3f4f5f6f7f8f9"), length: 42 });
    eq(hex(okm4),
       "085a01ea1b10f36933068b56efa5ad81a4f14b822f5b091568a9cdd4f155fda2" +
       "c22e422478d305f3f896",
       "RFC 5869 test case 4 (SHA-1)");

    assert(hash.SHA256Hex !== undefined, "SHA256 still available for comparison");
    throws(() => crypto.HKDF({ hash: "sha256", length: 42 }), "HKDF without a key");
    throws(() => crypto.HKDF({ hash: "nope", key: "k" }), "HKDF with an unknown hash");
    throws(() => crypto.HKDF({ hash: "sha256", key: "k", length: 0 }), "HKDF length 0");
    /* 255 * 32 = 8160 is the most SHA-256 HKDF can produce */
    eq(crypto.HKDF({ hash: "sha256", key: "k", length: 8160 }).length, 8160,
       "HKDF at the construction's maximum");
    throws(() => crypto.HKDF({ hash: "sha256", key: "k", length: 8161 }),
           "HKDF past the construction's maximum");
}

/* ------------------------------------------------------- 5. PBKDF2, RFC 6070 */
{
    const cases = [
        ["password", "salt", 1, 20, "0c60c80f961f0e71f3a9b524af6012062fe037a6"],
        ["password", "salt", 2, 20, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"],
        ["password", "salt", 4096, 20, "4b007901b765489abead49d926f721d065a429c1"],
        ["passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096,
         25, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038"],
    ];
    for (const [pw, salt, iterations, length, want] of cases) {
        const dk = crypto.PBKDF2({ hash: "sha1", password: pw, salt, iterations, length });
        eq(hex(dk), want, "RFC 6070 c=" + iterations + " dkLen=" + length);
    }
    /* a NUL in the password and salt, RFC 6070's last vector */
    eq(hex(crypto.PBKDF2({ hash: "sha1",
                           password: new Uint8Array([0x70, 0x61, 0x73, 0x73, 0, 0x77, 0x6f, 0x72, 0x64]),
                           salt: new Uint8Array([0x73, 0x61, 0, 0x6c, 0x74]),
                           iterations: 4096, length: 16 })),
       "56fa6aa75548099dcc37d7f03425e0c3",
       "RFC 6070 embedded NUL (a string API would truncate here)");
    /* SHA-256, RFC 7914 section 11 */
    eq(hex(crypto.PBKDF2({ hash: "sha256", password: "passwd", salt: "salt",
                           iterations: 1, length: 64 })),
       "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc" +
       "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
       "RFC 7914 PBKDF2-HMAC-SHA256");
    throws(() => crypto.PBKDF2({ hash: "sha1", password: "p", salt: "s",
                                 iterations: 0, length: 20 }), "0 iterations");
    throws(() => crypto.PBKDF2({ hash: "sha1", salt: "s", iterations: 1,
                                 length: 20 }), "PBKDF2 without a password");
}

/* --------------------------------------------------------- 6. RandomBytes */
{
    eq(crypto.RandomBytes(0).length, 0, "zero bytes is allowed");
    eq(crypto.RandomBytes(32).length, 32, "the requested length");
    eq(crypto.RandomBytes().length, 32, "a default length");
    const seen = new Set();
    for (let i = 0; i < 64; i++) seen.add(hex(crypto.RandomBytes(16)));
    eq(seen.size, 64, "64 draws of 16 bytes are all distinct");
    /* a crude but real check that it is not returning a constant or a counter */
    const big = crypto.RandomBytes(4096);
    const counts = new Array(256).fill(0);
    for (const b of big) counts[b]++;
    const max = Math.max(...counts);
    assert(max < 60, "no byte value dominates 4096 draws (max " + max + ")");
    throws(() => crypto.RandomBytes(-1), "a negative count");
}

/* ------------------------------------------------------------ 7. class Hmac */
{
    const KEY = "key";
    const MSG = "The quick brown fox jumps over the lazy dog";
    const WANT = "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8";
    const m = new crypto.Hmac("sha256", KEY);
    eq(m.signHex(MSG), WANT, "Hmac matches the published vector");
    eq(m.signHex(MSG), WANT, "and again -- signing resets the context");
    eq(m.signHex(MSG), crypto.HMACHex("sha256", KEY, MSG),
       "Hmac equals the free function");
    eq(m.algorithm, "sha256", "algorithm getter");
    eq(m.digestSize, 32, "digestSize getter");
    eq(hex(m.sign(MSG)), WANT, "sign() returns bytes");

    /* streaming: the same MAC from pieces */
    m.update("The quick brown ").update("fox jumps over ").update("the lazy dog");
    eq(m.digestHex(), WANT, "streamed update()s equal the one-shot");
    eq(m.signHex(MSG), WANT, "and the object is reusable afterwards");

    /* verify is the reason the class exists: signHex(m) === tag compares two
     * strings with an early exit and leaks how much of the MAC was guessed */
    assert(m.verify(MSG, WANT), "verify accepts the right hex tag");
    assert(m.verify(MSG, bytes(WANT)), "verify accepts the right byte tag");
    assert(!m.verify(MSG, "0".repeat(64)), "verify rejects a wrong tag");
    assert(!m.verify(MSG, WANT.slice(0, 62)), "verify rejects a short tag");
    assert(!m.verify(MSG, "zz" + WANT.slice(2)), "verify rejects a non-hex tag");
    assert(!m.verify("other", WANT), "verify rejects the wrong message");

    /* every algorithm the digests offer */
    for (const algo of ["md5", "sha1", "sha224", "sha256", "sha384", "sha512"]) {
        const h = new crypto.Hmac(algo, KEY);
        eq(h.signHex(MSG), crypto.HMACHex(algo, KEY, MSG), "Hmac/" + algo);
        h.close();
    }
    throws(() => new crypto.Hmac("nope", KEY), "an unknown algorithm");
    throws(() => new crypto.Hmac("sha256"), "a missing key");
    throws(() => crypto.Hmac("sha256", KEY), "calling without new");

    /* close() zeroes the key schedule, and the object is then unusable */
    const c = new crypto.Hmac("sha256", KEY);
    c.signHex("x");
    c.close();
    c.close();
    throws(() => c.signHex("x"), "sign after close");
    throws(() => c.verify("x", WANT), "verify after close");
}

/* reentrancy: the key schedule is mutable scratch, and the argument IS coerced
 * here (ToString for a non-buffer), so unlike Compressor the attack is real and
 * has to be run. The hook is toString, not valueOf -- dyn_crypto_data falls
 * through to JS_ToCStringLen, which never reaches valueOf (CLAUDE.md section 8). */
{
    const m = new crypto.Hmac("sha256", "key");
    const WANT_INNER = m.signHex("inner");
    let innerResult = null, valueOfRan = false;
    const attack = {
        toString() { innerResult = m.signHex("inner"); return "outer"; },
        valueOf() { valueOfRan = true; return "wrong-hook"; },
    };
    const outer = m.signHex(attack);
    eq(valueOfRan, false, "valueOf is NOT the hook here -- a test written " +
       "against it would pass having exercised nothing");
    eq(innerResult, WANT_INNER, "the reentrant call completed correctly");
    eq(outer, m.signHex("outer"),
       "and the outer call is unaffected: the argument is fully coerced " +
       "before the key schedule is touched, so the order is well-defined");

    /* close() during coercion must be a clean throw, never a use-after-free */
    const c = new crypto.Hmac("sha256", "key");
    throws(() => c.signHex({ toString() { c.close(); return "data"; } }),
           "close() during coercion");
    const c2 = new crypto.Hmac("sha256", "key");
    throws(() => c2.verify("m", { toString() { c2.close(); return "tag"; } }),
           "close() during the tag's coercion");
}

console.log("test_hash_split.js: " + n + " assertions passed");
