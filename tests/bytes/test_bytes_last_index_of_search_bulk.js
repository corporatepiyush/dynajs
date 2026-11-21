// covers: dyna:bytes.lastIndexOf
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_last_index_of_search_bulk");
import {
    Bytes,
    indexOf, lastIndexOf, count,
} from "dyna:bytes";

let n = 0;
function assert(c, msg) {
    n++;
    if (!c) throw new Error("assertion failed: " + msg + " (case " + n + ")");
}
function eq(a, b, msg) { assert(a === b, msg + " (got " + a + ", want " + b + ")"); }
function assertThrows(fn, msg, ErrType) {
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    assert(threw, "expected throw: " + msg);
    if (ErrType) assert(err instanceof ErrType, msg + " (wrong error type: " + err + ")");
}
function u8(...bytes) { return new Uint8Array(bytes); }
function str(s) { return new Bytes(s); }

const HAY = str("hello world, hello bytes");

__kitS.section("lastIndexOf default unchanged", () => {
    eq(HAY.lastIndexOf(u8(104, 101)), 13, "lastIndexOf default unchanged");
    eq(HAY.lastIndexOf(u8(104, 101), 12), 0, "lastIndexOf window excludes later match");
    eq(HAY.lastIndexOf(u8(104, 101), 13), 13, "lastIndexOf window includes match at fromIndex");
    eq(HAY.lastIndexOf(u8(104, 101), 0), 0, "lastIndexOf window shrinks to first byte");
    eq(HAY.lastIndexOf(u8(104, 101), -1), 0, "lastIndexOf negative clamps to 0");
    eq(HAY.lastIndexOf(u8(104, 101), 1e9), 13, "lastIndexOf huge fromIndex clamps to length");
    eq(HAY.lastIndexOf(u8(115)), 23, "lastIndexOf default finds the last 's'");
    eq(HAY.lastIndexOf(u8(115), 4), -1, "lastIndexOf window without any match");
    eq(HAY.lastIndexOf(u8()), 24, "lastIndexOf empty needle default length (unchanged)");
    eq(HAY.lastIndexOf(u8(), 7), 7, "lastIndexOf empty needle matches at fromIndex");
    eq(str("hello").lastIndexOf(u8(108, 108), 1), -1, "straddling match excluded");
    eq(str("hello").lastIndexOf(u8(108, 108), 2), 2, "match starting at edge included");
    eq(lastIndexOf(u8(1, 2, 3, 2), 2, 1), 1, "free lastIndexOf window");
});
__kitS.end();
print("test_bytes_search_bulk: all tests passed (" + n + " assertions)");
