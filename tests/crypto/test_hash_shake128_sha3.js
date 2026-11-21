// covers: dyna:hash.SHAKE128
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_shake128_sha3");
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

__kitS.section("SHAKE128 takes a length", () => {
eq(SHAKE128("", 16).length, 16, "SHAKE128 takes a length");
});

__kitS.section("a zero-length squeeze", () => {
throws(() => SHAKE128("", 0), "a zero-length squeeze");
});

__kitS.section("a negative one", () => {
throws(() => SHAKE128("", -1), "a negative one");
});

__kitS.section("and an unreasonable one", () => {
throws(() => SHAKE128("", 1 << 21), "and an unreasonable one");
});
__kitS.end();

if (fails) {
    print("test_sha3: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_sha3 failed");
}
print("test_sha3: " + n + " assertions, 0 failures");
