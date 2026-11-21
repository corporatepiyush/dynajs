// covers: dyna:hash.SHA1
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_sha1_crypto_reuse");
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

__kitS.section("RFC 6455 accept: SHA1 of key||GUID", () => {
    const key = "dGhlIHNhbXBsZSBub25jZQ==";
    const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    eq(SHA1Hex(key + GUID), "b37a4f2cc0624f1690f64606cf385945b2bec4ea",
       "RFC 6455 accept: SHA1 of key||GUID");
    eq(Base64Encode(SHA1(key + GUID)), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
       "RFC 6455 accept: the base64 the client compares against");
});
__kitS.end();

console.log("test_crypto_reuse: all " + n + " tests passed");
