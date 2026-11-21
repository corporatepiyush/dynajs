// covers: dyna:crypto.Argon2id
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_argon2id");
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

await __kitS.section("Argon2id: 64 MiB hash works", async () => {
    const salt = new TextEncoder().encode("somesalt");
    const tag = await Argon2id.hash("password", salt,
        { iterations: 1, memory: 65536, parallelism: 1, hashLen: 32, encoded: false });
    assert(tag instanceof Uint8Array && tag.length === 32, "Argon2id: 64 MiB hash works");
    assert(typeof await Argon2id.hash("password", salt,
        { iterations: 1, memory: 65536, parallelism: 1, hashLen: 32 }) === "string",
        "Argon2id: default form is the PHC string");
    assert(await Argon2id.verify("password", salt, tag,
        { iterations: 1, memory: 65536, parallelism: 1, hashLen: 32 }) === true,
        "Argon2id: verify shares the parser's accept-set");
    await assertThrows(() => Argon2id.hash("p", new Uint8Array(16),
        { memory: 1048577, iterations: 1 }), "Argon2id: memory above 1 GiB throws");
    await assertThrows(() => Argon2id.verify("p", new Uint8Array(16), new Uint8Array(32),
        { memory: 1048577, iterations: 1 }), "Argon2id.verify: same 1 GiB cap");
});
__kitS.end();

globalThis.console && console.log("test_crypto: all " + n + " tests passed");
