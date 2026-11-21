// covers: dyna:bytes.isValidUtf8
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_is_valid_utf8_core");
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

__kitS.section("isValidUtf8: Uint16Array is not a byte view", () => {
    assertThrows(() => isValidUtf8(new Uint16Array([0x41, 0x00])),
        "isValidUtf8: Uint16Array is not a byte view", TypeError);
    assertThrows(() => countUtf8(new Uint16Array([0x41, 0x00])),
        "countUtf8: Uint16Array is not a byte view", TypeError);
    assertThrows(() => countUtf16(new Uint16Array([0x41])),
        "countUtf16: Uint16Array is not a byte view", TypeError);
    assertThrows(() => utf8ToLatin1(new Float64Array(2)),
        "utf8ToLatin1: Float64Array is not a byte view", TypeError);

    {
        const ab = new ArrayBuffer(8);
        const v = new Uint8Array(ab);
        const dv = new DataView(ab);
        ab.transfer();
        assertThrows(() => isValidUtf8(v), "isValidUtf8: detached TypedArray throws", TypeError);
        assertThrows(() => countUtf16(v), "countUtf16: detached TypedArray throws", TypeError);
        assertThrows(() => isValidUtf8(dv), "isValidUtf8: detached DataView throws", TypeError);
        assertThrows(() => isValidUtf8(ab), "isValidUtf8: detached ArrayBuffer throws", TypeError);
    }
});
__kitS.end();
print("test_bytes: all tests passed (" + n + " assertions)");
