// covers: dyna:crypto.Bcrypt
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_bcrypt_standalone");
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
async function assertRejects(fn, msg, ErrType) {
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

await __kitS.section("bcrypt empty password verifies", async () => {
    assert(await Bcrypt.verify("", "$2a$06$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s.") === true,
        "bcrypt empty password verifies");
    assert(await Bcrypt.verify("wrong", "$2a$06$DCq7YPn5Rq63x1Lad4cll.TV4S6ytwfsfvkgY8jIucDrjc8deX1s.") === false,
        "bcrypt wrong password returns false");

    const gen = await Bcrypt.hash("secret_password", 4);
    assert(gen.startsWith("$2b$04$"), "bcrypt hash prefix $2b$04$");
    assert(gen.length === 60, "bcrypt hash length exactly 60");
    assert(await Bcrypt.verify("secret_password", gen) === true, "bcrypt verify generated hash");
    assert(await Bcrypt.verify("wrong_password", gen) === false, "bcrypt verify reject mismatch");

    await assertRejects(() => Bcrypt.hash("a".repeat(73), 4), "bcrypt password > 72 bytes throws", RangeError);
    await assertRejects(() => Bcrypt.hash("pass", 3), "bcrypt cost < 4 throws", RangeError);
    await assertRejects(() => Bcrypt.hash("pass", 32), "bcrypt cost > 31 throws", RangeError);
});
__kitS.end();

print("test_crypto_standalone: all tests passed (" + n + " assertions)");
