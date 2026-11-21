// covers: dyna:crypto.Hmac
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_hmac_class");
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

__kitS.section("Hmac.reset:", () => {
    const ok = (c, m) => { n++; if (!c) throw new Error("Hmac.reset: " + m); };
    const hex = (u8) => Array.from(u8).map(b => b.toString(16).padStart(2, "0")).join("");

    const h = new Hmac("sha256", "key");
    h.update("a");
    h.reset();
    h.update("b");
    const fresh = new Hmac("sha256", "key");
    fresh.update("b");
    ok(hex(h.digest()) === hex(fresh.digest()), "reset discards the update() prefix");

    for (const algo of ["sha1", "sha256", "sha512"]) {
        for (const key of ["k", "k".repeat(200)]) {
            const a = new Hmac(algo, key);
            a.update("prefix-that-must-die").update("more");
            a.reset();
            a.update("msg");
            const b = new Hmac(algo, key);
            b.update("msg");
            ok(hex(a.digest()) === hex(b.digest()), algo + " key" + key.length + ": reset == fresh");
        }
    }

    const c = new Hmac("sha256", "key");
    c.update("x");
    const d1 = hex(c.digest());
    c.update("y");
    ok(d1 === hex(new Hmac("sha256", "key").update("x").digest()), "digest() answers the absorbed message");
    ok(hex(c.digest()) === hex(new Hmac("sha256", "key").update("y").digest()),
       "post-digest message starts clean (final resets)");

    const e = new Hmac("sha256", "key");
    e.update("abandoned");
    e.reset();
    ok(hex(e.sign("msg")) === hex(new Hmac("sha256", "key").update("msg").digest()),
       "sign() after reset ignores nothing");

    const f = new Hmac("sha256", "key");
    f.update("junk").reset();
    ok(f.verify("msg", new Hmac("sha256", "key").sign("msg")) === true,
       "verify after reset");

    const g = new Hmac("sha1", "key");
    g.close();
    let threw = "";
    try { g.reset(); } catch (err) { threw = err.constructor.name; }
    ok(threw === "TypeError", "reset on closed -> TypeError, got " + threw);

    const h2 = new Hmac("sha256", "key");
    ok(h2.reset() === h2, "reset returns this");
});
__kitS.end();

globalThis.console && console.log("test_crypto: all " + n + " tests passed");
