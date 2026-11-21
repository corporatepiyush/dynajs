// covers: dyna:bytes.count
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_count_core");
import { Bytes, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy, isValidUtf8, countUtf8, utf8ToLatin1, countUtf16 } from "dyna:bytes";
import { HexEncode as toHex, HexDecode as fromHex, Base64Encode as toBase64, Base64Decode as fromBase64 } from "dyna:encoding";

const fromUtf8 = (str) => new TextEncoder().encode(str);
const toUtf8 = (v) => new TextDecoder().decode(v);


let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eqArr(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
function assertThrows(fn, msg, ErrType) {
    n++;
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
    if (ErrType && !(err instanceof ErrType))
        throw new Error("assertion failed (wrong error type, got " + err + "): " + msg);
}
function u8(...bytes) { return new Uint8Array(bytes); }

__kitS.section("count: non-overlapping pairs in 1111", () => {
    assert(count(u8(1, 1, 1, 1), u8(1, 1)) === 2, "count: non-overlapping pairs in 1111");
    assert(count(u8(1, 2, 1, 2, 1, 2), u8(1, 2)) === 3, "count: three non-overlapping");
    assert(count(u8(1, 2, 3), u8(9)) === 0, "count: absent byte -> 0");
    assert(count(u8(1, 1, 1), 1) === 3, "count: byte-number needle counts every occurrence");
    assert(count(u8(1, 2, 3), u8()) === 4, "count: empty needle -> length+1 (byte-count convention)");
    assert(count(u8(), u8()) === 1, "count: empty haystack, empty needle -> 1");
});
__kitS.end();
print("test_bytes: all tests passed (" + n + " assertions)");
