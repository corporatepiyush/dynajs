// covers: dyna:hash.SHA3_224
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_sha3_224");
import { SHA3_224, SHA3_256, SHA3_384, SHA3_512, Keccak256, SHAKE128, SHAKE256,
         SHA3_224Hex, SHA3_256Hex, SHA3_384Hex, SHA3_512Hex, Keccak256Hex,
         SHAKE128Hex, SHAKE256Hex } from "dyna:hash";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + "\n    got  " + a + "\n    want " + b);
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");

__kitS.section("SHA3-224 is 28 bytes", () => {
eq(SHA3_224("").length, 28, "SHA3-224 is 28 bytes");
});
__kitS.end();

if (fails) {
    print("test_sha3: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_sha3 failed");
}
print("test_sha3: " + n + " assertions, 0 failures");
