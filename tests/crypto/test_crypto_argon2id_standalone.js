// covers: dyna:crypto.Argon2id
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_argon2id_standalone");
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

await __kitS.section("argon2id default form is the PHC string", async () => {
    const salt = new TextEncoder().encode("somesalt");
    assert(typeof await Argon2id.hash("password", salt, {
        iterations: 3, memory: 65536, parallelism: 4, hashLen: 32
    }) === "string", "argon2id default form is the PHC string");
    const tag = await Argon2id.hash("password", salt, {
        iterations: 3, memory: 65536, parallelism: 4, hashLen: 32, encoded: false
    });
    assert(tag.length === 32, "argon2id output length 32 bytes");
    assert(toHex(tag) === "661fefbd6f29bcbc8f4646abc32a9d7a4645bb5c059537f8a5587f31adbecccd",
        "argon2id KAT vector matches reference");

    assert(await Argon2id.verify("password", salt, tag, {
        iterations: 3, memory: 65536, parallelism: 4, hashLen: 32
    }) === true, "argon2id verify matches");

    assert(await Argon2id.verify("wrong_password", salt, tag, {
        iterations: 3, memory: 65536, parallelism: 4, hashLen: 32
    }) === false, "argon2id verify rejects wrong password");

    await assertThrows(() => Argon2id.hash("p", new Uint8Array(7)), "salt < 8 bytes throws", RangeError);
    await assertThrows(() => Argon2id.hash("p", salt, { iterations: 0 }), "iterations < 1 throws", RangeError);
    await assertThrows(() => Argon2id.hash("p", salt, { parallelism: 0 }), "parallelism < 1 throws", RangeError);
});
__kitS.end();

print("test_crypto_standalone: all tests passed (" + n + " assertions)");
