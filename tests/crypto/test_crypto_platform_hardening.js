// timeout: 120
// flags: --std
// Split from tests/test_platform_hardening.js: the crypto sections.
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
{
    const a = BLAKE3Hex("data", { context: "a\u0000b" });
    const b = BLAKE3Hex("data", { context: "a" });
    assert(a !== b, "a NUL-bearing context separates domains (COMPAT-180)");
}
print("test_crypto_platform_hardening: all tests passed (" + n + " assertions)");
