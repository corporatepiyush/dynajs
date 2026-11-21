// covers: dyna:crypto.TimingSafeEqual
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_timing_safe_equal_hash_split");
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

__kitS.section("equal buffers compare equal", () => {
    const a = bytes("00112233445566778899aabbccddeeff");
    const b = bytes("00112233445566778899aabbccddeeff");
    const c = bytes("00112233445566778899aabbccddee00");
    const d = bytes("ff112233445566778899aabbccddeeff");
    assert(crypto.TimingSafeEqual(a, b), "equal buffers compare equal");
    assert(!crypto.TimingSafeEqual(a, c), "a trailing difference is caught");
    assert(!crypto.TimingSafeEqual(a, d), "a leading difference is caught");
    assert(!crypto.TimingSafeEqual(a, a.slice(0, 8)),
           "different lengths are unequal (a MAC's length is public)");
    assert(crypto.TimingSafeEqual("abc", "abc"), "strings work too");
    assert(crypto.TimingSafeEqual(new Uint8Array(0), new Uint8Array(0)),
           "two empty inputs are equal");
    for (let i = 0; i < a.length; i++) {
        const m = new Uint8Array(a);
        m[i] ^= 0xff;
        assert(!crypto.TimingSafeEqual(a, m),
               "a difference at byte " + i + " is caught");
    }
});
__kitS.end();

console.log("test_hash_split.js: " + n + " assertions passed");
