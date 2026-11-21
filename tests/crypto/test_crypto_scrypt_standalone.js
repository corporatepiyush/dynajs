// covers: dyna:crypto.Scrypt
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_scrypt_standalone");
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
async function assertThrows(fn, msg, ErrType) {
    n++;
    let threw = false, err = null;
    try { await fn(); } catch (e) { threw = true; err = e; }
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

await __kitS.section("scrypt RFC 7914 vector 1 matches", async () => {
if (typeof Scrypt === "function") {
    const v1 = await Scrypt("", "", { N: 16, r: 1, p: 1, keyLen: 64 });
    assert(toHex(v1) === "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906",
        "scrypt RFC 7914 vector 1 matches");

    const v2 = await Scrypt("password", "NaCl", { N: 1024, r: 8, p: 16, keyLen: 64 });
    assert(toHex(v2) === "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640",
        "scrypt RFC 7914 vector 2 matches");

    await assertThrows(() => Scrypt("p", "s", { N: 3 }), "N not power of 2 throws", RangeError);
    await assertThrows(() => Scrypt("p", "s", { r: 0 }), "r < 1 throws", RangeError);
}
});
__kitS.end();

print("test_crypto_standalone: all tests passed (" + n + " assertions)");
