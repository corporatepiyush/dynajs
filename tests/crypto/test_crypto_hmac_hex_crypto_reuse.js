// covers: dyna:crypto.HMACHex
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_hmac_hex_crypto_reuse");
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

__kitS.section("update()+digest() still streams", () => {
    for (const algo of ["sha256", "sha512"]) {
        const h = new Hmac(algo, "key");
        h.update("ab");
        eq(h.signHex(""), HMACHex(algo, "key", ""),
           `sign() ignores a pending update() prefix (${algo})`);
        h.update("cd");
        eq(h.signHex("ef"), HMACHex(algo, "key", "ef"),
           `sign() MACs exactly its own message (${algo})`);
        h.update("gh");
        eq(h.verify("ij", HMACHex(algo, "key", "ij")), true,
           `verify() is also a complete MAC (${algo})`);
        h.close();
    }
    const s = new Hmac("sha256", "key");
    s.update("ab").update("cd");
    eq(s.digestHex(), HMACHex("sha256", "key", "abcd"), "update()+digest() still streams");
    s.close();
});
__kitS.end();

console.log("test_crypto_reuse: all " + n + " tests passed");
