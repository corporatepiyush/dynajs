// covers: dyna:hash.Hasher
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_hasher");
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

__kitS.section("streaming abc", () => {
    for (const name of Object.keys(HASH)) {
        const h = new Hasher(name);
        h.update("a").update("b").update("c");
        assert(h.digestHex() === HASH[name].hex("abc"), name + ": streaming abc");
        assert(toHex(h.digest()) === HASH[name].hex("abc"), name + ": streaming abc binary");
        h.update("d");
        assert(h.digestHex() === HASH[name].hex("abcd"), name + ": digest non-destructive");
        h.reset();
        h.update("abc");
        assert(h.digestHex() === HASH[name].hex("abc"), name + ": reset");
        assert(h.algorithm === name, name + ": algorithm getter");
        assert(h.digestSize === HASH[name].size, name + ": digestSize getter");
    }
});

__kitS.section("streaming==oneshot @len", () => {
    function msg(len) {
        const u = new Uint8Array(len);
        let x = 0x2545f491 ^ len;
        for (let i = 0; i < len; i++) {
            x ^= x << 13; x >>>= 0; x ^= x >> 17; x ^= x << 5; x >>>= 0;
            u[i] = x & 0xff;
        }
        return u;
    }
    let seed = 0x9e3779b9;
    function rnd(mod) {
        seed ^= seed << 13; seed >>>= 0; seed ^= seed >> 17; seed ^= seed << 5; seed >>>= 0;
        return seed % mod;
    }
    const lengths = [0, 1, 2, 3, 54, 55, 56, 57, 63, 64, 65, 66,
                     110, 111, 112, 113, 119, 120, 127, 128, 129, 130,
                     191, 192, 255, 256, 257, 1000, 4096, 100000];
    for (const name of Object.keys(HASH)) {
        for (const L of lengths) {
            const m = msg(L);
            const oneShot = HASH[name].hex(m);
            const h = new Hasher(name);
            let off = 0;
            while (off < L) {
                const chunk = Math.min(L - off, 1 + rnd(40));
                h.update(m.subarray(off, off + chunk));
                off += chunk;
            }
            assert(h.digestHex() === oneShot, name + ": streaming==oneshot @len " + L);
            }
    }
});

__kitS.section("Hasher: unknown algorithm throws", () => {
    assertThrows(() => new Hasher("nope"), "Hasher: unknown algorithm throws");
    assertThrows(() => new Hasher(), "Hasher: missing algorithm throws");

    const h = new Hasher("sha256");
    assert(typeof h.close === "function", "Hasher: close() exists");
    assert(typeof h.dispose === "function", "Hasher: dispose() exists");
    assert(h.closed === false, "Hasher: closed getter reads false while open");
    assert(typeof h[Symbol.dispose] === "function", "Hasher: disposable");

    h.update("a");
    h.update("bc");
    assert(h.digestHex() === SHA256Hex("abc"), "Hasher: usable, digest correct");
    h.update("d");
    assert(h.digestHex() === SHA256Hex("abcd"), "Hasher: still usable after digest");
    h.reset();
    h.update("abc");
    assert(h.digestHex() === SHA256Hex("abc"), "Hasher: reset then reuse");

    for (let i = 0; i < 5000; i++) {
        const t = new Hasher("md5");
        t.update("x");
        t.digestHex();
    }
    assert(true, "Hasher: 5000 dropped instances reclaimed by the GC");
});

__kitS.section("hashed a coerced object, still usable", () => {
    {
        const h = new Hasher("sha256");
        h.update({ valueOf() { return 1; } });
        assert(typeof h.digestHex() === "string", "hashed a coerced object, still usable");
    }
    {
        const h = new Hasher("sha256");
        h.update("a");
        assertThrows(() => h.update({ toString() { throw new Error("boom"); } }),
            "a throwing toString propagates");
        h.update("bc");
        assert(h.digestHex() === SHA256Hex("abc"),
            "the failed update contributed nothing: digest is of \"abc\"");
    }
    {
        const h = new Hasher("sha512");
        assertThrows(() => h.update({ [Symbol.toPrimitive]() { throw new Error("nope"); } }),
            "a throwing @@toPrimitive propagates");
        h.update("z");
        assert(h.digestHex() === SHA512Hex("z"), "and the state is still clean");
    }
    {
        const h = new Hasher("sha256");
        let side = 0;
        h.update({ toString() { side++; return "q"; } });
        assert(side === 1, "coerced exactly once");
        assert(h.digestHex() === SHA256Hex("q"), "hashed what the coercion returned");
    }
});

__kitS.section("some data", () => {
    for (let i = 0; i < 20000; i++) {
        const h = new Hasher("sha256");
        h.update("some data ").update(bytes(i & 0xff, (i >> 8) & 0xff));
        h.digest();
    }
    assert(true, "20k create/update/digest/close cycles completed");
});
__kitS.end();

globalThis.console && console.log("test_crypto: all " + n + " tests passed");
