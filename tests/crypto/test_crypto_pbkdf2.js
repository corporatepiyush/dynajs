// covers: dyna:crypto.PBKDF2
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_pbkdf2");
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
async function assertThrows(fn, msg) {
    n++;
    let threw = false;
    try { await fn(); } catch (e) { threw = true; }
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

await __kitS.section("RFC 6070 c", async () => {
    const cases = [
        [1,     "0c60c80f961f0e71f3a9b524af6012062fe037a6"],
        [2,     "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"],
        [4096,  "4b007901b765489abead49d926f721d065a429c1"],
    ];
    for (const [iters, want] of cases) {
        const dk = await PBKDF2({ hash: "sha1", password: "password", salt: "salt",
                            iterations: iters, length: 20 });
        assert(toHex(dk) === want, "RFC 6070 c=" + iters);
    }
    await assertThrows(() => PBKDF2({ hash: "sha1", password: "password", salt: "salt",
                                iterations: 16777219, length: 20 }),
        "RFC 6070 c=16777219 is refused by the 2^24-compression DoS bound");
});
__kitS.end();

globalThis.console && console.log("test_crypto: all " + n + " tests passed");
