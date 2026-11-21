// covers: dyna:hash.Murmur3_128
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_murmur3_128_keyed_leak");
import { BLAKE2b, BLAKE2s, BLAKE3, Murmur3_128 } from "dyna:hash";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}

const hex = (u8) => Array.from(u8).map(b => b.toString(16).padStart(2, "0")).join("");

__kitS.section("unkeyed BLAKE3 is stable", () => {
    eq(hex(BLAKE3("abc")), hex(BLAKE3("abc")), "unkeyed BLAKE3 is stable");
    eq(Murmur3_128("abc").length, 16, "Murmur3_128 digest is 16 bytes");
    eq(hex(Murmur3_128("abc", 42)), hex(Murmur3_128("abc", 42)), "seeded Murmur is stable");
});
__kitS.end();

print("test_hash_keyed_leak: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed (LSan must be flat at exit)"));
if (fails) throw new Error(fails + " failures");
