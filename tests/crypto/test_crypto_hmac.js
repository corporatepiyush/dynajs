// covers: dyna:crypto.HMAC
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_hmac");
import {
    MD5, MD5Hex, SHA1, SHA1Hex, SHA224, SHA224Hex,
    SHA256, SHA256Hex, SHA384, SHA384Hex, SHA512, SHA512Hex,
    CRC32, CRC32C, Hasher,
} from "dyna:hash";
import { HMAC, HMACHex, PBKDF2, RSA, Argon2id, Hmac, XXH3_64, Ed25519Generate, Ed25519PemToRaw, Ed25519PemFromRaw, X509, ECDSA, AESGCM } from "dyna:crypto";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertThrows(fn, msg) {
    n++;
    let threw = false;
    try { fn(); } catch (e) { threw = true; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
}
function toHex(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i++) s += u8[i].toString(16).padStart(2, "0");
    return s;
}
function bytes(...b) { return new Uint8Array(b); }
function rep(byteVal, count) { return new Uint8Array(count).fill(byteVal); }

const HASH = {
    "md5":    { bin: MD5,    hex: MD5Hex,    size: 16 },
    "sha1":   { bin: SHA1,   hex: SHA1Hex,   size: 20 },
    "sha224": { bin: SHA224, hex: SHA224Hex, size: 28 },
    "sha256": { bin: SHA256, hex: SHA256Hex, size: 32 },
    "sha384": { bin: SHA384, hex: SHA384Hex, size: 48 },
    "sha512": { bin: SHA512, hex: SHA512Hex, size: 64 },
};

__kitS.section("Hi There", () => {
    assert(HMACHex("sha256", rep(0x0b, 20), "Hi There") ===
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
        "HMAC-SHA256 rfc4231 #1");
    assert(HMACHex("sha512", rep(0x0b, 20), "Hi There") ===
        "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854",
        "HMAC-SHA512 rfc4231 #1");

    const jefe = "what do ya want for nothing?";
    assert(HMACHex("sha256", "Jefe", jefe) ===
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
        "HMAC-SHA256 Jefe");
    assert(HMACHex("sha1", "Jefe", jefe) ===
        "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", "HMAC-SHA1 Jefe");
    assert(HMACHex("md5", "Jefe", jefe) ===
        "750c783e6ab0b503eaa86e310a5db738", "HMAC-MD5 Jefe");

    assert(HMACHex("sha256", rep(0xaa, 131),
        "Test Using Larger Than Block-Size Key - Hash Key First") ===
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
        "HMAC-SHA256 rfc4231 #6 (long key)");

    const hb = HMAC("sha256", rep(0x0b, 20), "Hi There");
    assert(hb instanceof Uint8Array && hb.length === 32, "HMAC: binary Uint8Array");
    assert(toHex(hb) === HMACHex("sha256", rep(0x0b, 20), "Hi There"), "HMAC: binary == hex");
    assert(HMACHex("sha256", "key", "data") ===
        HMACHex("sha256", new TextEncoder().encode("key"), new TextEncoder().encode("data")),
        "HMAC: string key/data == bytes");
});
__kitS.end();

globalThis.console && console.log("test_crypto: all " + n + " tests passed");
