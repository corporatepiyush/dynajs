// covers: dyna:hash.SHA512Hex
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_sha512_hex");
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
    assert(MD5Hex("") === "d41d8cd98f00b204e9800998ecf8427e", "MD5('')");
    assert(MD5Hex("abc") === "900150983cd24fb0d6963f7d28e17f72", "MD5('abc')");
    assert(MD5Hex("The quick brown fox jumps over the lazy dog") ===
        "9e107d9d372bb6826bd81d3542a419d6", "MD5(fox)");

    assert(SHA1Hex("") === "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA1('')");
    assert(SHA1Hex("abc") === "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA1('abc')");
    assert(SHA1Hex("The quick brown fox jumps over the lazy dog") ===
        "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12", "SHA1(fox)");

    assert(SHA224Hex("") ===
        "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f", "SHA224('')");
    assert(SHA224Hex("abc") ===
        "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7", "SHA224('abc')");

    assert(SHA256Hex("") ===
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA256('')");
    assert(SHA256Hex("abc") ===
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256('abc')");
    assert(SHA256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ===
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA256(2-block)");

    assert(SHA384Hex("") ===
        "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b",
        "SHA384('')");
    assert(SHA384Hex("abc") ===
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
        "SHA384('abc')");

    assert(SHA512Hex("") ===
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
        "SHA512('')");
    assert(SHA512Hex("abc") ===
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        "SHA512('abc')");
    assert(SHA512Hex("abc").length === 128, "SHA512Hex: 64-byte digest is 128 hex chars");
});
__kitS.end();

globalThis.console && console.log("test_crypto: all " + n + " tests passed");
