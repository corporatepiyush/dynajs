// covers: dyna:crypto.ECDH
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_ecdh_standalone");
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

__kitS.section("P-256 shared secret is 32 bytes", () => {
if (typeof ECDH !== "undefined") {
    const alice = ECDH.generate("P-256");
    const bob   = ECDH.generate("P-256");

    const secretAlice = ECDH.derive(alice.privateKey, bob.publicKey);
    const secretBob   = ECDH.derive(bob.privateKey, alice.publicKey);

    assert(secretAlice instanceof Uint8Array && secretAlice.length === 32, "P-256 shared secret is 32 bytes");
    assert(toHex(secretAlice) === toHex(secretBob), "ECDH key exchange yields symmetric shared secret");

    const eve = ECDH.generate("P-256");
    const secretEve = ECDH.derive(alice.privateKey, eve.publicKey);
    assert(toHex(secretAlice) !== toHex(secretEve), "Different peer key yields distinct shared secret");
}
});
__kitS.end();

print("test_crypto_standalone: all tests passed (" + n + " assertions)");
