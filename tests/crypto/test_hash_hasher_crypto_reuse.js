// covers: dyna:hash.Hasher
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_hasher_crypto_reuse");
import { SHA256Hex, MD5Hex, SHA1, SHA1Hex, Hasher } from "dyna:hash";
import { Hmac, HMACHex } from "dyna:crypto";
import { Base64Encode } from "dyna:encoding";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}

const ALGOS = ["md5", "sha1", "sha224", "sha256", "sha384", "sha512"];

__kitS.section("CONTAMINANT", () => {
    const MSGS = ["", "a", "abc", "x".repeat(55), "x".repeat(56), "x".repeat(64),
                  "y".repeat(111), "y".repeat(112), "y".repeat(128),
                  "z".repeat(1000)];

    for (const algo of ALGOS) {
        const reused = new Hasher(algo);
        for (const m of MSGS) {
            const fresh = new Hasher(algo);
            const want = fresh.update(m).digestHex();
            reused.reset();
            const got = reused.update(m).digestHex();
            eq(got, want, `reuse ${algo} len=${m.length}`);
        }
        reused.update("CONTAMINANT");
        reused.reset();
        eq(reused.update("abc").digestHex(),
           new Hasher(algo).update("abc").digestHex(),
           `reset() discards a partial message (${algo})`);
    }
});

__kitS.section("valueOf is NOT the coercion hook here (ToString wins)", () => {
    const h = new Hasher("sha256");
    h.update("A");
    let fired = false;
    h.update({ valueOf() { fired = true; h.update("B"); return "C"; } });
    assert(!fired, "valueOf is NOT the coercion hook here (ToString wins)");
    eq(h.digestHex(), SHA256Hex("A[object Object]"),
       "a valueOf-only object coerces via Object.prototype.toString");
});

__kitS.section("reentrant update() during coercion: inner bytes absorbed first", () => {
    const h = new Hasher("sha256");
    h.update("A");
    h.update({ toString() { h.update("B"); return "C"; } });
    eq(h.digestHex(), SHA256Hex("ABC"),
       "reentrant update() during coercion: inner bytes absorbed first");
});

__kitS.section("reentrant reset() during coercion: prefix discarded, no corruption", () => {
    const h = new Hasher("md5");
    h.update("DISCARDED");
    h.update({ toString() { h.reset(); return "abc"; } });
    eq(h.digestHex(), MD5Hex("abc"),
       "reentrant reset() during coercion: prefix discarded, no corruption");
});

__kitS.section("8-deep reentrancy absorbs innermost-first with no lost or duplicated byte", () => {
    const h = new Hasher("sha256");
    let depth = 0;
    const nest = {
        toString() {
            const me = String.fromCharCode(65 + depth);
            if (depth < 8) { depth++; h.update(nest); }
            return me;
        },
    };
    h.update(nest);
    eq(h.digestHex(), SHA256Hex("IHGFEDCBA"),
       "8-deep reentrancy absorbs innermost-first with no lost or duplicated byte");
});

__kitS.section("assertion failed: unknown algorithm must throw TypeError", () => {
    n++;
    let threw = false;
    try { new Hasher("sha3-256"); } catch (e) { threw = e instanceof TypeError; }
    if (!threw) throw new Error("assertion failed: unknown algorithm must throw TypeError");
});

__kitS.section("block L90", () => {
    for (const algo of ALGOS) {
        const h = new Hasher(algo);
        eq(h.algorithm, algo, `algorithm getter (${algo})`);
        eq(h.digest().length, h.digestSize, `digestSize matches digest() (${algo})`);
    }
});
__kitS.end();

console.log("test_crypto_reuse: all " + n + " tests passed");
