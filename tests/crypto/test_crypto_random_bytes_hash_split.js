// covers: dyna:crypto.RandomBytes
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_random_bytes_hash_split");
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

__kitS.section("zero bytes is allowed", () => {
    eq(crypto.RandomBytes(0).length, 0, "zero bytes is allowed");
    eq(crypto.RandomBytes(32).length, 32, "the requested length");
    eq(crypto.RandomBytes().length, 32, "a default length");
    const seen = new Set();
    for (let i = 0; i < 64; i++) seen.add(hex(crypto.RandomBytes(16)));
    eq(seen.size, 64, "64 draws of 16 bytes are all distinct");
    const big = crypto.RandomBytes(4096);
    const counts = new Array(256).fill(0);
    for (const b of big) counts[b]++;
    const max = Math.max(...counts);
    assert(max < 60, "no byte value dominates 4096 draws (max " + max + ")");
    throws(() => crypto.RandomBytes(-1), "a negative count");
});
__kitS.end();

console.log("test_hash_split.js: " + n + " assertions passed");
