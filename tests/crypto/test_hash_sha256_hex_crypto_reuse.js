// covers: dyna:hash.SHA256Hex
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_sha256_hex_crypto_reuse");
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

__kitS.section("digestHex() is idempotent (state survives finalization)", () => {
    const h = new Hasher("sha256");
    h.update("ab");
    const d1 = h.digestHex();
    const d2 = h.digestHex();
    eq(d2, d1, "digestHex() is idempotent (state survives finalization)");
    eq(d1, SHA256Hex("ab"), "partial digest matches the one-shot");
    h.update("c");
    eq(h.digestHex(), SHA256Hex("abc"),
       "the hasher keeps absorbing after a digest");
});

__kitS.section("reentrant digest() sees the prefix", () => {
    const h = new Hasher("sha256");
    h.update("A");
    let inner = null;
    h.update({ toString() { inner = h.digestHex(); return "B"; } });
    eq(inner, SHA256Hex("A"), "reentrant digest() sees the prefix");
    eq(h.digestHex(), SHA256Hex("AB"), "outer update lands after the inner digest");
});
__kitS.end();

console.log("test_crypto_reuse: all " + n + " tests passed");
