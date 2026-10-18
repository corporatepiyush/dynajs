// Bytes caches isAscii / isValidUtf8. Once `array` has handed out the live view, writes through it
// must be visible to both flags (the cache used to answer from before the write). The duplicated
// accessors (fill, readBytes/writeBytes, the fixed-width readers and writers, toUtf8) are gone:
// TypedArray#fill/#set/#slice, DataView and TextEncoder/TextDecoder are the API for those.
import * as B from "dyna:bytes";
const { Bytes } = B;

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
{
    const b = new Bytes(new Uint8Array([65, 66, 67]));
    const a = b.array;
    assert(b.isAscii === true && b.isValidUtf8 === true, "an ASCII copy starts ASCII and valid");
    a[1] = 0xff;
    assert(b.isAscii === false, "a non-ASCII byte written through the live array clears isAscii");
    assert(b.isValidUtf8 === false, "and clears isValidUtf8 (0xff is never valid UTF-8)");
    a[1] = 0x42;
    assert(b.isAscii === true && b.isValidUtf8 === true, "restoring the byte restores both flags");
    a.set([0xc3, 0xa9], 0);
    assert(b.isAscii === false && b.isValidUtf8 === true, "a two-byte UTF-8 sequence is valid but not ASCII");
}
{
    const b = new Bytes("plain");
    assert(b.isAscii === true, "a Bytes whose array was never taken still caches correctly");
}
for (const gone of ["fill", "readBytes", "writeBytes", "readUint8", "readUint32LE", "writeDoubleBE", "readBigInt64LE", "toUtf8"])
    assert(!(gone in Bytes.prototype), "Bytes.prototype." + gone + " is gone (the standard API covers it)");
for (const gone of ["bytesOf", "fill", "toUtf8", "fromUtf8", "readUint8", "writeUint8", "readUint16BE", "writeInt32LE", "readFloatLE", "writeBigUint64BE"])
    assert(!(gone in B), "dyna:bytes no longer exports " + gone);
for (const kept of ["compare", "equal", "indexOf", "lastIndexOf", "contains", "count", "concat", "copy", "isValidUtf8", "isValidUtf16", "countUtf8", "countUtf16", "latin1ToUtf8", "utf8ToLatin1", "utf8ToUtf16", "utf16ToUtf8", "decode", "encode", "encodingExists", "encodings"])
    assert(typeof B[kept] === "function", "dyna:bytes keeps " + kept);
if (fails) { print("test_bytes_p2_batch3: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_bytes_p2_batch3 failed"); }
print("test_bytes_p2_batch3: " + n + " assertions, 0 failures");
