// covers: contract dyna:hash.* dyna:crypto.* -- which names each module exports: secret-free aliases in both, keyed primitives only in dyna:crypto
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_split_surface");
import * as hash from "dyna:hash";
import * as crypto from "dyna:crypto";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

__kitS.section("dyna:hash exports", () => {
for (const name of ["MD5", "MD5Hex", "SHA1", "SHA256", "SHA256Hex", "SHA512",
                    "CRC32", "CRC32C", "XXHash64", "XXHash32"]) {
    assert(name in hash, "dyna:hash exports " + name);
    assert(name in crypto, "P14: dyna:crypto re-exports " + name +
           " (alias over the same implementation)");
}
assert(!("Hasher" in crypto), "the streaming Hasher class stays dyna:hash-only");
for (const name of ["HMAC", "HMACHex", "Hmac", "TimingSafeEqual", "HKDF",
                    "PBKDF2", "RandomBytes"]) {
    assert(name in crypto, "dyna:crypto exports " + name);
    assert(!(name in hash), "dyna:hash does NOT export " + name +
           " -- it depends on a secret");
}
});
__kitS.end();

console.log("test_hash_split_surface.js: " + n + " assertions passed");
