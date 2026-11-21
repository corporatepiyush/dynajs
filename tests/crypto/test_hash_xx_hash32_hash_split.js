// covers: dyna:hash.XXHash32
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_xx_hash32_hash_split");
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

__kitS.section("XXH32('abc') seed 0", () => {
eq(hash.XXHash32("abc"), 0x32d153ff, "XXH32('abc') seed 0");
});

__kitS.section("XXH32('') seed 0", () => {
eq(hash.XXHash32(""), 0x02cc5d05, "XXH32('') seed 0");
});

__kitS.section("XXH32('a') seed 0", () => {
eq(hash.XXHash32("a"), 0x550d7456, "XXH32('a') seed 0");
});
__kitS.end();

console.log("test_hash_split.js: " + n + " assertions passed");
