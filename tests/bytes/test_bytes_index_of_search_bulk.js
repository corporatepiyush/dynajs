// covers: dyna:bytes.indexOf
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_index_of_search_bulk");
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

__kitS.section("indexOf default unchanged", () => {
    eq(HAY.indexOf(u8(104, 101)), 0, "indexOf default unchanged");
    eq(HAY.indexOf(u8(104, 101), 1), 13, "indexOf fromIndex skips first");
    eq(HAY.indexOf(u8(104, 101), 13), 13, "indexOf fromIndex == match index");
    eq(HAY.indexOf(u8(104, 101), 14), -1, "indexOf fromIndex past last match");
    eq(HAY.indexOf(108, 14), 15, "indexOf byte-value needle with fromIndex");
    eq(HAY.indexOf(u8(104, 101), -3), 0, "indexOf negative fromIndex clamps to 0");
    eq(HAY.indexOf(u8(104, 101), 1e9), -1, "indexOf huge fromIndex");
    eq(HAY.indexOf(u8(104, 101), 1e9), -1, "indexOf huge fromIndex idempotent");
    eq(HAY.indexOf(u8(104, 101), 1.9), 13, "indexOf fractional fromIndex truncates (ToInteger)");
    eq(HAY.indexOf(u8(104, 101), 24), -1, "indexOf fromIndex == length");
    eq(HAY.indexOf(u8(104, 101), 25), -1, "indexOf fromIndex > length");
    eq(HAY.indexOf(u8(250), 5), -1, "absent needle with fromIndex");
    eq(HAY.indexOf(u8()), 0, "indexOf empty needle default 0 (unchanged)");
    eq(HAY.indexOf(u8(), 7), 7, "indexOf empty needle matches at fromIndex");
    eq(HAY.indexOf(u8(), 100), 24, "indexOf empty needle clamps to length");
    eq(indexOf(HAY.array, 108, 14), 15, "free indexOf fromIndex");
    eq(indexOf(u8(1, 2, 3, 2), 2, 2), 3, "free indexOf window");

    const hits = [];
    let pos = 0;
    while ((pos = HAY.indexOf(108, pos)) !== -1) { hits.push(pos); pos += 1; }
    eq(hits.join(","), "2,3,9,15,16", "successive-match loop via fromIndex");
    const two = [];
    pos = 0;
    while ((pos = HAY.indexOf(u8(108, 108), pos)) !== -1) { two.push(pos); pos += 2; }
    eq(two.join(","), "2,15", "multi-byte successive loop advances by plen");
});
__kitS.end();
print("test_bytes_search_bulk: all tests passed (" + n + " assertions)");
