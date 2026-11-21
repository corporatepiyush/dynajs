// covers: dyna:hash.CRC32
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_crc32");
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

__kitS.section("The quick brown fox jumps over the lazy dog", () => {
    assert(CRC32("123456789") === 0xCBF43926, "CRC32('123456789')");
    assert(CRC32("") === 0, "CRC32('')");
    assert(CRC32("The quick brown fox jumps over the lazy dog") === 0x414FA339, "CRC32(fox)");
    assert(CRC32C("123456789") === 0xE3069283, "CRC32C('123456789')");
    assert(CRC32("abc") === CRC32(bytes(0x61, 0x62, 0x63)), "CRC32: string == bytes");
    assert(CRC32("123456789") > 0, "CRC32: unsigned");
});
__kitS.end();

globalThis.console && console.log("test_crypto: all " + n + " tests passed");
