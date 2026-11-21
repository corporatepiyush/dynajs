// covers: dyna:bytes.indexOf
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_index_of_core");
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

__kitS.section("indexOf: byte-number needle, first match", () => {
    const hay = u8(1, 2, 3, 2, 3, 4);
    assert(indexOf(hay, 3) === 2, "indexOf: byte-number needle, first match");
    assert(lastIndexOf(hay, 3) === 4, "lastIndexOf: byte-number needle, last match");
    assert(indexOf(hay, 9) === -1, "indexOf: byte not present");
    assert(lastIndexOf(hay, 9) === -1, "lastIndexOf: byte not present");

    assert(indexOf(hay, u8(2, 3)) === 1, "indexOf: Uint8Array needle, first match");
    assert(lastIndexOf(hay, u8(2, 3)) === 3, "lastIndexOf: Uint8Array needle, last match");
    assert(indexOf(hay, u8(9, 9)) === -1, "indexOf: Uint8Array needle absent");
    assert(lastIndexOf(hay, u8(9, 9)) === -1, "lastIndexOf: Uint8Array needle absent");

    assert(indexOf(hay, u8()) === 0, "indexOf: empty needle -> 0");
    assert(lastIndexOf(hay, u8()) === hay.length, "lastIndexOf: empty needle -> length");
    assert(indexOf(u8(), u8(1)) === -1, "indexOf: empty haystack, non-empty needle");
    assert(indexOf(u8(), u8()) === 0, "indexOf: empty haystack, empty needle -> 0");

    assert(indexOf(hay, u8(1, 2, 3, 2, 3, 4, 5)) === -1, "indexOf: needle longer than haystack -> -1");

    assert(contains(hay, 4) === true, "contains: byte-number present");
    assert(contains(hay, 99) === false, "contains: byte-number absent");
    assert(contains(hay, u8(3, 2)) === true, "contains: Uint8Array present");
    assert(contains(hay, u8(3, 9)) === false, "contains: Uint8Array absent");
    assert(contains(hay, u8()) === true, "contains: empty needle always true");

    const big = u8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    const sub = big.subarray(3, 7);
    assert(indexOf(sub, 5) === 2, "indexOf: works on a subarray view");
    assert(sub[0] === 3, "subarray view resolves its own byteOffset");
});
__kitS.end();
print("test_bytes: all tests passed (" + n + " assertions)");
