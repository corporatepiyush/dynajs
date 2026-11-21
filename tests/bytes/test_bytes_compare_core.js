// covers: dyna:bytes.compare
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_compare_core");
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

__kitS.section("compare: less at last byte", () => {
    assert(compare(u8(1, 2), u8(1, 3)) === -1, "compare: less at last byte");
    assert(compare(u8(1, 3), u8(1, 2)) === 1, "compare: greater at last byte");
    assert(compare(u8(1, 2), u8(1, 2)) === 0, "compare: equal");
    assert(compare(u8(1, 2), u8(1, 2, 3)) === -1, "compare: prefix is less");
    assert(compare(u8(1, 2, 3), u8(1, 2)) === 1, "compare: superset is greater");
    assert(compare(u8(), u8()) === 0, "compare: two empty buffers");
    assert(compare(u8(), u8(1)) === -1, "compare: empty vs non-empty");
    assert(compare(new ArrayBuffer(0), new ArrayBuffer(0)) === 0, "compare: ArrayBuffer args");

    assert(equal(u8(1, 2, 3), u8(1, 2, 3)) === true, "equal: same content");
    assert(equal(u8(1, 2, 3), u8(1, 2, 4)) === false, "equal: differ at last byte");
    assert(equal(u8(1, 2), u8(1, 2, 3)) === false, "equal: different length");
    assert(equal(u8(), u8()) === true, "equal: two empty buffers");
});

__kitS.section("empty: compare", () => {
    const e = new Uint8Array(0);
    assert(compare(e, e) === 0, "empty: compare");
    assert(equal(e, e) === true, "empty: equal");
    assert(indexOf(e, 1) === -1, "empty: indexOf byte -> -1");
    assert(lastIndexOf(e, 1) === -1, "empty: lastIndexOf byte -> -1");
    assert(contains(e, 1) === false, "empty: contains byte -> false");
    assert(count(e, 1) === 0, "empty: count byte -> 0");
    assert(eqArr(concat([e]), []), "empty: concat([empty])");
    assert(copy(e, e) === 0, "empty: copy");
    assert(e.fill(1) === e, "empty: fill is a no-op returning the buffer");
    assert(toHex(e) === "", "empty: toHex");
    assert(toBase64(e) === "", "empty: toBase64");
    assert(toUtf8(e) === "", "empty: toUtf8");
    assert(eqArr(fromHex(""), []), "empty: fromHex");
    assert(eqArr(fromBase64(""), []), "empty: fromBase64");
    assert(eqArr(fromUtf8(""), []), "empty: fromUtf8");
});
__kitS.end();
print("test_bytes: all tests passed (" + n + " assertions)");
