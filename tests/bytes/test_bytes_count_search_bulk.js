// covers: dyna:bytes.count
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_count_search_bulk");
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

__kitS.section("count default unchanged", () => {
    eq(HAY.count(108), 5, "count default unchanged");
    eq(HAY.count(108, 3), 4, "count fromIndex 3 (first 'l' at 2 excluded)");
    eq(HAY.count(108, 14), 2, "count window tail");
    eq(HAY.count(108, 100), 0, "count past end");
    eq(HAY.count(108, -5), 5, "count negative clamps to 0");
    eq(HAY.count(u8()), 25, "count empty needle default length+1 (unchanged)");
    eq(HAY.count(u8(), 7), 18, "count empty needle counts remaining window");
    eq(HAY.count(u8(104, 101), 1), 1, "count multi-byte needle window");
    eq(count(u8(1, 2, 1, 2, 1), 1, 1), 2, "free count window");
});
__kitS.end();
print("test_bytes_search_bulk: all tests passed (" + n + " assertions)");
