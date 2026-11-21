// covers: dyna:bytes.concat
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_concat_core");
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

__kitS.section("concat: three pieces", () => {
    assert(eqArr(concat([u8(1, 2), u8(3), u8(4, 5, 6)]), [1, 2, 3, 4, 5, 6]), "concat: three pieces");
    assert(eqArr(concat([]), []), "concat: empty array -> empty result");
    assert(eqArr(concat([u8(), u8()]), []), "concat: array of empties -> empty result");
    assert(concat([u8(1, 2), u8(3)]) instanceof Uint8Array, "concat: returns a Uint8Array");
    assert(eqArr(concat([u8(1, 2), new Uint8Array([3, 4]).buffer]), [1, 2, 3, 4]), "concat: ArrayBuffer element");
});

__kitS.section("concat(number) throws", () => {
    assertThrows(() => concat(5), "concat(number) throws", TypeError);
    assertThrows(() => concat(undefined), "concat(undefined) throws", TypeError);
    assertThrows(() => concat("ab"), "concat(string) throws", TypeError);
    assert(eqArr(concat([u8(1), u8(2, 3)]), [1, 2, 3]), "concat(array) still works");
});
__kitS.end();
print("test_bytes: all tests passed (" + n + " assertions)");
