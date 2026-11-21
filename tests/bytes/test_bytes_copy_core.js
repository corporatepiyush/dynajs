// covers: dyna:bytes.copy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_copy_core");
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

__kitS.section("copy: returns bytes-copied count", () => {
    const dst1 = new Uint8Array(5);
    const written = copy(dst1, u8(9, 8, 7));
    assert(written === 3, "copy: returns bytes-copied count");
    assert(eqArr(dst1, [9, 8, 7, 0, 0]), "copy: default offsets/len");

    const dst2 = new Uint8Array(5);
    copy(dst2, u8(1, 2, 3, 4), 2);
    assert(eqArr(dst2, [0, 0, 1, 2, 3]), "copy: dstOff clips to dst capacity (len defaults to min avail)");

    const dst3 = new Uint8Array(3);
    copy(dst3, u8(1, 2, 3, 4, 5), 0, 2);
    assert(eqArr(dst3, [3, 4, 5]), "copy: srcOff selects a slice of src");

    const dst4 = new Uint8Array(4);
    const w4 = copy(dst4, u8(1, 2, 3, 4, 5), 1, 1, 2);
    assert(w4 === 2 && eqArr(dst4, [0, 2, 3, 0]), "copy: explicit len");

    const overlap1 = u8(1, 2, 3, 4, 5);
    copy(overlap1, overlap1, 1, 0, 4);
    assert(eqArr(overlap1, [1, 1, 2, 3, 4]), "copy: overlap shift-right (dst after src)");

    const overlap2 = u8(1, 2, 3, 4, 5);
    copy(overlap2, overlap2, 0, 1, 4);
    assert(eqArr(overlap2, [2, 3, 4, 5, 5]), "copy: overlap shift-left (dst before src)");

    assert(copy(new Uint8Array(0), u8(1, 2, 3)) === 0, "copy: empty dst -> 0 bytes copied");
    assert(copy(new Uint8Array(3), new Uint8Array(0)) === 0, "copy: empty src -> 0 bytes copied");

    assertThrows(() => copy(new Uint8Array(2), u8(1, 2, 3), 0, 0, 3),
        "copy: explicit len exceeding src throws", RangeError);
    assertThrows(() => copy(new Uint8Array(2), u8(1, 2, 3), 5),
        "copy: dstOff beyond dst.length throws", RangeError);
});

__kitS.section("copy: reentrant detach via dstOff valueOf", () => {
    function detachTrap(ab) {
        return { valueOf() { ab.transfer(); return 0; } };
    }

    {
        const ab = new ArrayBuffer(8);
        const view = new Uint8Array(ab);
        const src = new Uint8Array(4);
        assertThrows(() => copy(view, src, detachTrap(ab)),
            "copy: reentrant detach via dstOff valueOf");
    }
    {
        const ab = new ArrayBuffer(8);
        const dst = new Uint8Array(4);
        const srcView = new Uint8Array(ab);
        assertThrows(() => copy(dst, srcView, 0, detachTrap(ab)),
            "copy: reentrant detach via srcOff valueOf (src buffer)");
    }
    {
        const dst = new Uint8Array(4);
        assert(copy(dst, new Uint8Array([1, 2, 3, 4])) === 4 && dst[3] === 4, "sanity: a non-reentrant copy still works");
    }
});
__kitS.end();
print("test_bytes: all tests passed (" + n + " assertions)");
