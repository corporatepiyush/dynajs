// covers: dyna:encoding.HexDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_hex_decode_bytes");
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

__kitS.section("toHex: empty buffer -> empty string", () => {
    assert(toHex(u8()) === "", "toHex: empty buffer -> empty string");
    assert(toHex(u8(0xde, 0xad, 0xbe, 0xef)) === "deadbeef", "toHex: lowercase, no separators");
    assert(eqArr(fromHex(""), []), "fromHex: empty string -> empty buffer");
    assert(eqArr(fromHex("deadbeef"), [0xde, 0xad, 0xbe, 0xef]), "fromHex: lowercase round trip");
    assert(eqArr(fromHex("DEADBEEF"), [0xde, 0xad, 0xbe, 0xef]), "fromHex: uppercase accepted");
    assert(fromHex("deadbeef") instanceof Uint8Array, "fromHex: returns a Uint8Array");

    for (let i = 0; i < 256; i += 17) {
        const buf = u8(i);
        assert(eqArr(fromHex(toHex(buf)), [i]), "hex round trip byte " + i);
    }

    assertThrows(() => fromHex("abc"), "fromHex: odd length throws", SyntaxError);
    assertThrows(() => fromHex("zz"), "fromHex: invalid hex digit throws", SyntaxError);
});
__kitS.end();
print("test_bytes: all tests passed (" + n + " assertions)");
