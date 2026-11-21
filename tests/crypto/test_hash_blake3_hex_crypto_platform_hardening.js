// timeout: 120
// covers: dyna:hash.BLAKE3Hex
// Split from tests/test_platform_hardening.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_blake3_hex_crypto_platform_hardening");
import { BLAKE3Hex } from "dyna:hash";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function throwsRange(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof RangeError; }
    assert(t, msg);
}
function throwsType(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof TypeError; }
    assert(t, msg);
}

__kitS.section("a NUL-bearing context separates domains (COMPAT-180)", () => {
    const a = BLAKE3Hex("data", { context: "a\u0000b" });
    const b = BLAKE3Hex("data", { context: "a" });
    assert(a !== b, "a NUL-bearing context separates domains (COMPAT-180)");
});
__kitS.end();
print("test_crypto_platform_hardening: all tests passed (" + n + " assertions)");
