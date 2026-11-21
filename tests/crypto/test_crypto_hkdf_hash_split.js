// covers: dyna:crypto.HKDF
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_hkdf_hash_split");
import * as hash from "dyna:hash";
import * as crypto from "dyna:crypto";

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
function throws(fn, msg) {
    n++;
    try { fn(); } catch (e) { return; }
    throw new Error("assertion failed: " + msg + " did not throw");
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));

__kitS.section("RFC 5869 test case 1", () => {
    const okm = crypto.HKDF({
        hash: "sha256",
        key: new Uint8Array(22).fill(0x0b),
        salt: bytes("000102030405060708090a0b0c"),
        info: bytes("f0f1f2f3f4f5f6f7f8f9"),
        length: 42,
    });
    eq(hex(okm),
       "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf" +
       "34007208d5b887185865",
       "RFC 5869 test case 1");

    const ikm2 = new Uint8Array(80).map((_, i) => i);
    const salt2 = new Uint8Array(80).map((_, i) => 0x60 + i);
    const info2 = new Uint8Array(80).map((_, i) => 0xb0 + i);
    const okm2 = crypto.HKDF({ hash: "sha256", key: ikm2, salt: salt2,
                               info: info2, length: 82 });
    eq(hex(okm2),
       "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c" +
       "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71" +
       "cc30c58179ec3e87c14c01d5c1f3434f1d87",
       "RFC 5869 test case 2 (multi-block expand)");

    const okm3 = crypto.HKDF({ hash: "sha256", key: new Uint8Array(22).fill(0x0b),
                               length: 42 });
    eq(hex(okm3),
       "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d" +
       "9d201395faa4b61a96c8",
       "RFC 5869 test case 3 (no salt, no info)");

    const okm4 = crypto.HKDF({ hash: "sha1", key: new Uint8Array(11).fill(0x0b),
                               salt: bytes("000102030405060708090a0b0c"),
                               info: bytes("f0f1f2f3f4f5f6f7f8f9"), length: 42 });
    eq(hex(okm4),
       "085a01ea1b10f36933068b56efa5ad81a4f14b822f5b091568a9cdd4f155fda2" +
       "c22e422478d305f3f896",
       "RFC 5869 test case 4 (SHA-1)");

    assert(hash.SHA256Hex !== undefined, "SHA256 still available for comparison");
    throws(() => crypto.HKDF({ hash: "sha256", length: 42 }), "HKDF without a key");
    throws(() => crypto.HKDF({ hash: "nope", key: "k" }), "HKDF with an unknown hash");
    throws(() => crypto.HKDF({ hash: "sha256", key: "k", length: 0 }), "HKDF length 0");
    eq(crypto.HKDF({ hash: "sha256", key: "k", length: 8160 }).length, 8160,
       "HKDF at the construction's maximum");
    throws(() => crypto.HKDF({ hash: "sha256", key: "k", length: 8161 }),
           "HKDF past the construction's maximum");
});
__kitS.end();

console.log("test_hash_split.js: " + n + " assertions passed");
