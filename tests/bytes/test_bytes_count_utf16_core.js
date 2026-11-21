// covers: dyna:bytes.countUtf16
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_count_utf16_core");
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

__kitS.section("countUtf16: odd byte length throws", () => {
    assertThrows(() => countUtf16(u8(0x41, 0x00, 0x42)),
        "countUtf16: odd byte length throws", RangeError);
    assert(countUtf16(u8(0x41, 0x00, 0x42, 0x00)) === 2,
        "countUtf16: an even length still counts");
});
__kitS.end();
print("test_bytes: all tests passed (" + n + " assertions)");
