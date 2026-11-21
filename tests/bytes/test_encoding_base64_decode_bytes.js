// covers: dyna:encoding.Base64Decode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base64_decode_bytes");
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

__kitS.section("toBase64: empty buffer -> empty string", () => {
    assert(toBase64(u8()) === "", "toBase64: empty buffer -> empty string");
    assert(toBase64(u8(0x68, 0x69)) === "aGk=", "toBase64: known vector 'hi'");
    assert(eqArr(fromBase64(""), []), "fromBase64: empty string -> empty buffer");
    assert(eqArr(fromBase64("aGk="), [0x68, 0x69]), "fromBase64: known vector round trip");
    assert(fromBase64("aGk=") instanceof Uint8Array, "fromBase64: returns a Uint8Array");

    for (const len of [0, 1, 2, 3, 4, 5, 16, 100]) {
        const bytes = [];
        for (let i = 0; i < len; i++) bytes.push((i * 37 + 11) & 0xFF);
        const buf = new Uint8Array(bytes);
        assert(eqArr(fromBase64(toBase64(buf)), bytes), "base64 round trip len=" + len);
    }

    assertThrows(() => fromBase64("a"), "fromBase64: length not multiple of 4 throws", SyntaxError);
    assertThrows(() => fromBase64("****"), "fromBase64: invalid characters throw", SyntaxError);
});
__kitS.end();
print("test_bytes: all tests passed (" + n + " assertions)");
