// covers: dyna:hash.XXHash64
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_xx_hash64_hash_split");
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

__kitS.section("XXHash64 returns a string", () => {
eq(typeof hash.XXHash64(""), "string", "XXHash64 returns a string");
});

__kitS.section("XXH64('') seed 0", () => {
eq(hash.XXHash64(""), "ef46db3751d8e999", "XXH64('') seed 0");
});

__kitS.section("XXH64('a') seed 0", () => {
eq(hash.XXHash64("a"), "d24ec4f1a98c6e5b", "XXH64('a') seed 0");
});

__kitS.section("XXH64('abc') seed 0", () => {
eq(hash.XXHash64("abc"), "44bc2cf5ad770999", "XXH64('abc') seed 0");
});

__kitS.section("the seed matters", () => {
assert(hash.XXHash64("abc", 1) !== hash.XXHash64("abc", 0), "the seed matters");
});

__kitS.section("a string and its UTF-8 bytes hash identically", () => {
eq(hash.XXHash64(new TextEncoder().encode("abc")), hash.XXHash64("abc"),
   "a string and its UTF-8 bytes hash identically");
});
__kitS.end();

console.log("test_hash_split.js: " + n + " assertions passed");
