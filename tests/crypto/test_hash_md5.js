// covers: dyna:hash.MD5
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_md5");
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

__kitS.section("binary is Uint8Array", () => {
    for (const name of Object.keys(HASH)) {
        const h = HASH[name];
        const d = h.bin("abc");
        assert(d instanceof Uint8Array, name + ": binary is Uint8Array");
        assert(d.length === h.size, name + ": digest size");
        assert(toHex(d) === h.hex("abc"), name + ": binary == hex");
    }
});

import * as Aliases from "dyna:crypto";

import * as HashNS from "dyna:hash";

__kitS.section("P14:", () => {
    const data = new Uint8Array(513);
    let s = 0x2545F491 >>> 0;
    for (let i = 0; i < data.length; i++) {
        s = ((s * 1103515245) + 12345) >>> 0;
        data[i] = s >>> 24;
    }
    const names = [
        "MD5", "MD5Hex", "SHA1", "SHA1Hex", "SHA224", "SHA224Hex",
        "SHA256", "SHA256Hex", "SHA384", "SHA384Hex", "SHA512", "SHA512Hex",
        "CRC32", "CRC32C", "XXHash32", "XXHash64",
        "SHA3_224", "SHA3_224Hex", "SHA3_256", "SHA3_256Hex",
        "SHA3_384", "SHA3_384Hex", "SHA3_512", "SHA3_512Hex",
        "Keccak256", "Keccak256Hex",
        "SHAKE128", "SHAKE128Hex", "SHAKE256", "SHAKE256Hex",
        "BLAKE3", "BLAKE3Hex", "BLAKE2b", "BLAKE2bHex", "BLAKE2s", "BLAKE2sHex",
        "Murmur3_128", "Murmur3_128Hex",
    ];
    const lenFamily = new Set([
        "SHAKE128", "SHAKE128Hex", "SHAKE256", "SHAKE256Hex",
        "BLAKE3", "BLAKE3Hex",
    ]);
    for (const nm of names) {
        const viaCrypto = Aliases[nm], viaHash = HashNS[nm];
        assert(typeof viaCrypto === "function" && typeof viaHash === "function",
            "P14: " + nm + " exported by both modules");
        const args = lenFamily.has(nm) ? [data, 32] : [data];
        const ca = viaCrypto(...args), ch = viaHash(...args);
        if (typeof ca === "string") {
            assert(ca === ch, "P14: " + nm + " alias == dyna:hash (hex)");
        } else if (typeof ca === "number") {
            assert(ca === ch, "P14: " + nm + " alias == dyna:hash (number)");
        } else {
            assert(ca instanceof Uint8Array && ch instanceof Uint8Array &&
                ca.length === ch.length &&
                ca.every((b, i) => b === ch[i]),
                "P14: " + nm + " alias == dyna:hash (bytes)");
        }
    }
});
__kitS.end();

globalThis.console && console.log("test_crypto: all " + n + " tests passed");
