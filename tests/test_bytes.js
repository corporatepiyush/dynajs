import { Bytes, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy, fill, readUint8, readInt8, readUint16LE, readUint16BE, readInt16LE, readUint32LE, readUint32BE, readInt32LE, readBigUint64LE, readBigUint64BE, readBigInt64LE, readFloatLE, readFloatBE, readDoubleLE, readDoubleBE, writeUint8, writeInt8, writeUint16LE, writeUint16BE, writeInt16LE, writeUint32LE, writeUint32BE, writeInt32LE, writeBigUint64LE, writeBigUint64BE, writeBigInt64LE, writeFloatLE, writeFloatBE, writeDoubleLE, writeDoubleBE, toUtf8, fromUtf8, bytesOf, isValidUtf8, countUtf8, utf8ToLatin1, countUtf16 } from "dyna:bytes";
import { HexEncode as toHex, HexDecode as fromHex, Base64Encode as toBase64, Base64Decode as fromBase64 } from "dyna:encoding";

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

{
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
}

{
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
    assert(readUint8(sub, 0) === 3, "subarray view resolves its own byteOffset");
}

{
    assert(count(u8(1, 1, 1, 1), u8(1, 1)) === 2, "count: non-overlapping pairs in 1111");
    assert(count(u8(1, 2, 1, 2, 1, 2), u8(1, 2)) === 3, "count: three non-overlapping");
    assert(count(u8(1, 2, 3), u8(9)) === 0, "count: absent byte -> 0");
    assert(count(u8(1, 1, 1), 1) === 3, "count: byte-number needle counts every occurrence");
    assert(count(u8(1, 2, 3), u8()) === 4, "count: empty needle -> length+1 (byte-count convention)");
    assert(count(u8(), u8()) === 1, "count: empty haystack, empty needle -> 1");
}

{
    assert(eqArr(concat([u8(1, 2), u8(3), u8(4, 5, 6)]), [1, 2, 3, 4, 5, 6]), "concat: three pieces");
    assert(eqArr(concat([]), []), "concat: empty array -> empty result");
    assert(eqArr(concat([u8(), u8()]), []), "concat: array of empties -> empty result");
    assert(concat([u8(1, 2), u8(3)]) instanceof Uint8Array, "concat: returns a Uint8Array");
    assert(eqArr(concat([u8(1, 2), new Uint8Array([3, 4]).buffer]), [1, 2, 3, 4]), "concat: ArrayBuffer element");
}

{
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
}

{
    const f1 = new Uint8Array(5);
    const r1 = fill(f1, 7);
    assert(r1 === f1 && eqArr(f1, [7, 7, 7, 7, 7]), "fill: whole buffer, returns same buffer");

    const f2 = new Uint8Array(5);
    fill(f2, 9, 1, 4);
    assert(eqArr(f2, [0, 9, 9, 9, 0]), "fill: start/end range");

    const f3 = new Uint8Array(5);
    fill(f3, 0x1FF);
    assert(eqArr(f3, [0xFF, 0xFF, 0xFF, 0xFF, 0xFF]), "fill: value wraps modulo 256");

    const f4 = new Uint8Array(0);
    assert(fill(f4, 5) === f4, "fill: empty buffer is a safe no-op");

    assertThrows(() => fill(new Uint8Array(4), 1, 3, 2), "fill: start > end throws", RangeError);
    assertThrows(() => fill(new Uint8Array(4), 1, 0, 5), "fill: end beyond length throws", RangeError);
}

{
    const b = new Uint8Array(2);
    assert(writeUint8(b, 0, 0) === 1, "writeUint8 returns next offset");
    assert(readUint8(b, 0) === 0, "u8 round trip: 0");
    writeUint8(b, 0, 255);
    assert(readUint8(b, 0) === 255, "u8 round trip: max (255)");
    assert(readInt8(b, 0) === -1, "u8/i8 share bits: 0xFF reads back as -1 signed");
    writeInt8(b, 0, -1);
    assert(readUint8(b, 0) === 255, "i8 -1 reads back as 255 unsigned");
    writeInt8(b, 0, -128);
    assert(readInt8(b, 0) === -128, "i8 round trip: min (-128)");
    writeInt8(b, 0, 127);
    assert(readInt8(b, 0) === 127, "i8 round trip: max (127)");
}

{
    const b = new Uint8Array(2);
    writeUint16LE(b, 0, 0);
    assert(readUint16LE(b, 0) === 0, "u16le round trip: 0");
    const off = writeUint16LE(b, 0, 0xFFFF);
    assert(off === 2, "writeUint16LE returns next offset");
    assert(readUint16LE(b, 0) === 0xFFFF, "u16le round trip: max (65535)");
    assert(readInt16LE(b, 0) === -1, "i16le round trip via same bits: -1");
    writeInt16LE(b, 0, -32768);
    assert(readInt16LE(b, 0) === -32768, "i16le round trip: min (-32768)");
    assert(readUint16LE(b, 0) === 32768, "u16le view of i16 min is 32768");

    writeUint16BE(b, 0, 0x0102);
    assert(readUint16BE(b, 0) === 0x0102, "u16be round trip");
    assert(b[0] === 0x01 && b[1] === 0x02, "u16be byte order: MSB first");
    writeUint16LE(b, 0, 0x0102);
    assert(b[0] === 0x02 && b[1] === 0x01, "u16le byte order: LSB first");
    writeUint16LE(b, 0, 0x1234);
    assert(readUint16BE(b, 0) === 0x3412, "u16: LE write read back as BE is byte-swapped");
}

{
    const b = new Uint8Array(4);
    writeUint32LE(b, 0, 0);
    assert(readUint32LE(b, 0) === 0, "u32le round trip: 0");
    const off = writeUint32LE(b, 0, 0xFFFFFFFF);
    assert(off === 4, "writeUint32LE returns next offset");
    assert(readUint32LE(b, 0) === 0xFFFFFFFF, "u32le round trip: max (4294967295)");
    assert(readInt32LE(b, 0) === -1, "i32le round trip via same bits: -1");
    writeInt32LE(b, 0, -2147483648);
    assert(readInt32LE(b, 0) === -2147483648, "i32le round trip: min");
    assert(readUint32LE(b, 0) === 2147483648, "u32le view of i32 min is 2147483648");
    writeInt32LE(b, 0, 2147483647);
    assert(readInt32LE(b, 0) === 2147483647, "i32le round trip: max");

    writeUint32BE(b, 0, 0x01020304);
    assert(readUint32BE(b, 0) === 0x01020304, "u32be round trip");
    assert(eqArr(b, [0x01, 0x02, 0x03, 0x04]), "u32be byte order: MSB first");
    writeUint32LE(b, 0, 0x01020304);
    assert(eqArr(b, [0x04, 0x03, 0x02, 0x01]), "u32le byte order: LSB first");
    writeUint32LE(b, 0, 0x01020304);
    assert(readUint32BE(b, 0) === 0x04030201, "u32: LE write read back as BE is byte-swapped");
}

{
    const b = new Uint8Array(8);
    writeBigUint64LE(b, 0, 0n);
    assert(readBigUint64LE(b, 0) === 0n, "u64le round trip: 0");
    const off = writeBigUint64LE(b, 0, 0xFFFFFFFFFFFFFFFFn);
    assert(off === 8, "writeBigUint64LE returns next offset");
    assert(readBigUint64LE(b, 0) === 0xFFFFFFFFFFFFFFFFn, "u64le round trip: max (2^64-1)");
    assert(readBigInt64LE(b, 0) === -1n, "i64le round trip via same bits: -1");
    assert(typeof readBigUint64LE(b, 0) === "bigint", "u64 reads return a BigInt");

    writeBigInt64LE(b, 0, -9223372036854775808n);
    assert(readBigInt64LE(b, 0) === -9223372036854775808n, "i64le round trip: min");
    assert(readBigUint64LE(b, 0) === 9223372036854775808n, "u64le view of i64 min is 2^63");
    writeBigInt64LE(b, 0, 9223372036854775807n);
    assert(readBigInt64LE(b, 0) === 9223372036854775807n, "i64le round trip: max");

    writeBigUint64BE(b, 0, 0x0102030405060708n);
    assert(readBigUint64BE(b, 0) === 0x0102030405060708n, "u64be round trip");
    assert(eqArr(b, [1, 2, 3, 4, 5, 6, 7, 8]), "u64be byte order: MSB first");
    writeBigUint64LE(b, 0, 0x0102030405060708n);
    assert(eqArr(b, [8, 7, 6, 5, 4, 3, 2, 1]), "u64le byte order: LSB first");
}

{
    const b = new Uint8Array(4);
    writeFloatLE(b, 0, 0);
    assert(readFloatLE(b, 0) === 0, "f32le round trip: 0");
    writeFloatLE(b, 0, -0);
    assert(Object.is(readFloatLE(b, 0), -0), "f32le round trip: negative zero preserved");
    writeFloatLE(b, 0, 1.5);
    assert(readFloatLE(b, 0) === 1.5, "f32le round trip: 1.5 (exact in binary32)");
    writeFloatLE(b, 0, Math.fround(3.14));
    assert(readFloatLE(b, 0) === Math.fround(3.14), "f32le round trip: fround(3.14)");
    writeFloatLE(b, 0, Infinity);
    assert(readFloatLE(b, 0) === Infinity, "f32le round trip: +Infinity");
    writeFloatLE(b, 0, -Infinity);
    assert(readFloatLE(b, 0) === -Infinity, "f32le round trip: -Infinity");
    writeFloatLE(b, 0, NaN);
    assert(Number.isNaN(readFloatLE(b, 0)), "f32le round trip: NaN");

    writeFloatBE(b, 0, 1.5);
    assert(readFloatBE(b, 0) === 1.5, "f32be round trip: 1.5");
    writeFloatLE(b, 0, 1.5);
    const leBytes = Array.from(b);
    writeFloatBE(b, 0, 1.5);
    const beBytes = Array.from(b);
    assert(!eqArr(leBytes, beBytes), "f32: LE and BE byte layouts differ for the same value");
}

{
    const b = new Uint8Array(8);
    writeDoubleLE(b, 0, 0);
    assert(readDoubleLE(b, 0) === 0, "f64le round trip: 0");
    writeDoubleLE(b, 0, -0);
    assert(Object.is(readDoubleLE(b, 0), -0), "f64le round trip: negative zero preserved");
    writeDoubleLE(b, 0, Math.PI);
    assert(readDoubleLE(b, 0) === Math.PI, "f64le round trip: Math.PI (full double precision)");
    writeDoubleLE(b, 0, Number.MAX_VALUE);
    assert(readDoubleLE(b, 0) === Number.MAX_VALUE, "f64le round trip: MAX_VALUE");
    writeDoubleLE(b, 0, Number.MIN_VALUE);
    assert(readDoubleLE(b, 0) === Number.MIN_VALUE, "f64le round trip: MIN_VALUE (denormal)");
    writeDoubleLE(b, 0, -Infinity);
    assert(readDoubleLE(b, 0) === -Infinity, "f64le round trip: -Infinity");
    writeDoubleLE(b, 0, NaN);
    assert(Number.isNaN(readDoubleLE(b, 0)), "f64le round trip: NaN");

    writeDoubleBE(b, 0, Math.PI);
    assert(readDoubleBE(b, 0) === Math.PI, "f64be round trip: Math.PI");
    const off = writeDoubleBE(b, 0, 1);
    assert(off === 8, "writeDoubleBE returns next offset");
}

{
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
}

{
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
}

{
    assert(toUtf8(u8()) === "", "toUtf8: empty buffer -> empty string");
    assert(toUtf8(fromUtf8("")) === "", "fromUtf8: empty string round trip");
    assert(toUtf8(fromUtf8("hello")) === "hello", "utf8 round trip: ASCII");
    assert(toUtf8(fromUtf8("héllo wörld")) === "héllo wörld", "utf8 round trip: Latin-1 accents");
    assert(toUtf8(fromUtf8("日本語")) === "日本語", "utf8 round trip: CJK (3-byte sequences)");
    assert(toUtf8(fromUtf8("😀🎉")) === "😀🎉", "utf8 round trip: astral / surrogate pairs (4-byte)");

    const encoded = fromUtf8("café");
    assert(encoded instanceof Uint8Array, "fromUtf8: returns a Uint8Array");
    assert(encoded.length === 5, "fromUtf8: 'café' is 5 UTF-8 bytes (é is 2 bytes)");
}

{
    assertThrows(() => readUint8(new Uint8Array(0), 0), "readUint8: empty buffer at 0", RangeError);
    assertThrows(() => readUint32LE(new Uint8Array(2), 0), "readUint32LE: buffer too short", RangeError);
    assertThrows(() => readUint8(new Uint8Array(1), 1), "readUint8: offset one past the end", RangeError);
    assertThrows(() => readBigUint64LE(new Uint8Array(7), 0), "readBigUint64LE: buffer too short", RangeError);
    assertThrows(() => readDoubleLE(new Uint8Array(4), 0), "readDoubleLE: buffer too short", RangeError);
    assertThrows(() => readUint8(new Uint8Array(4), -1), "readUint8: negative offset", RangeError);

    assertThrows(() => writeUint32LE(new Uint8Array(2), 0, 1), "writeUint32LE: buffer too short", RangeError);
    assertThrows(() => writeInt8(new Uint8Array(1), 5, 1), "writeInt8: offset beyond buffer", RangeError);
    assertThrows(() => writeBigInt64LE(new Uint8Array(4), 0, 1n), "writeBigInt64LE: buffer too short", RangeError);

    assertThrows(() => readUint8([1, 2, 3], 0), "readUint8: plain Array is not a byte view", TypeError);
    assertThrows(() => compare("abc", "abd"), "compare: strings are not byte views", TypeError);
}

{
    const ab = new ArrayBuffer(16);
    const dv = new DataView(ab);
    assert(writeUint32LE(dv, 0, 0xdeadbeef) === 4, "DataView: writeUint32LE returns next offset");
    assert(readUint32LE(dv, 0) === 0xdeadbeef, "DataView: readUint32LE round-trip");
    assert(dv.getUint32(0, true) === 0xdeadbeef, "DataView: engine getUint32 agrees");
    dv.setUint16(8, 0x1234, false);
    assert(readUint16BE(dv, 8) === 0x1234, "DataView: reads a value the engine wrote");

    const win = new DataView(ab, 4, 4);
    assert(writeUint32BE(win, 0, 0x01020304) === 4, "DataView window: write at its own 0");
    assert(readUint8(new Uint8Array(ab), 4) === 0x01, "DataView window: byte 0 maps to buffer byte 4");
    assertThrows(() => readUint32LE(win, 1), "DataView window: read past its length", RangeError);

    assert(equal(dv, new Uint8Array(ab)) === true, "DataView: equal against a Uint8Array over the same buffer");
    assert(indexOf(dv, 0xef) === 0, "DataView: indexOf finds the LE low byte");
    assert(toHex(win) === "01020304", "DataView: toHex covers only the window");
}

{
    assertThrows(() => readUint8(new Uint16Array(4), 0), "Uint16Array is not a byte view", TypeError);
    assertThrows(() => compare(new Float64Array(2), new Float64Array(2)),
        "Float64Array is not a byte view", TypeError);

    const f = new Float64Array([1.5, -2.5]);
    const fb = bytesOf(f);
    assert(fb instanceof Uint8Array, "bytesOf: returns a Uint8Array");
    assert(fb.length === 16, "bytesOf(Float64Array[2]): 16 bytes");
    assert(readDoubleLE(fb, 0) === 1.5, "bytesOf: first double readable as bytes");
    assert(readDoubleLE(fb, 8) === -2.5, "bytesOf: second double readable as bytes");

    writeDoubleLE(fb, 0, 42.25);
    assert(f[0] === 42.25, "bytesOf: write through the alias is visible in the source view");
    f[1] = 7;
    assert(readDoubleLE(fb, 8) === 7, "bytesOf: write through the source view is visible in the alias");

    const buf = new ArrayBuffer(32);
    const mid = new Uint16Array(buf, 8, 4);
    const mb = bytesOf(mid);
    assert(mb.length === 8, "bytesOf: length is the view's byteLength, not the buffer's");
    fill(mb, 0xff);
    assert(new Uint8Array(buf)[7] === 0, "bytesOf: byte before the window untouched");
    assert(new Uint8Array(buf)[8] === 0xff, "bytesOf: window start written");
    assert(new Uint8Array(buf)[15] === 0xff, "bytesOf: window end written");
    assert(new Uint8Array(buf)[16] === 0, "bytesOf: byte after the window untouched");

    const dv2 = new DataView(buf, 4, 8);
    assert(bytesOf(dv2).length === 8, "bytesOf(DataView): its own window");
    assert(bytesOf(buf).length === 32, "bytesOf(ArrayBuffer): the whole buffer");
    const u8 = new Uint8Array(buf, 3, 5);
    assert(bytesOf(u8).length === 5, "bytesOf(Uint8Array): same extent");

    assertThrows(() => bytesOf([1, 2, 3]), "bytesOf: plain Array is not a view", TypeError);
    assertThrows(() => bytesOf("abc"), "bytesOf: string is not a view", TypeError);
    {
        const ab2 = new ArrayBuffer(8);
        const v2 = new Uint8Array(ab2);
        const dv3 = new DataView(ab2);
        ab2.transfer();
        assertThrows(() => bytesOf(v2), "bytesOf: detached TypedArray", TypeError);
        assertThrows(() => bytesOf(dv3), "bytesOf: detached DataView", TypeError);
        assertThrows(() => readUint8(dv3, 0), "read through a detached DataView", TypeError);
    }
}

{
    const e = new Uint8Array(0);
    assert(compare(e, e) === 0, "empty: compare");
    assert(equal(e, e) === true, "empty: equal");
    assert(indexOf(e, 1) === -1, "empty: indexOf byte -> -1");
    assert(lastIndexOf(e, 1) === -1, "empty: lastIndexOf byte -> -1");
    assert(contains(e, 1) === false, "empty: contains byte -> false");
    assert(count(e, 1) === 0, "empty: count byte -> 0");
    assert(eqArr(concat([e]), []), "empty: concat([empty])");
    assert(copy(e, e) === 0, "empty: copy");
    assert(fill(e, 1) === e, "empty: fill is a no-op returning the buffer");
    assert(toHex(e) === "", "empty: toHex");
    assert(toBase64(e) === "", "empty: toBase64");
    assert(toUtf8(e) === "", "empty: toUtf8");
    assert(eqArr(fromHex(""), []), "empty: fromHex");
    assert(eqArr(fromBase64(""), []), "empty: fromBase64");
    assert(eqArr(fromUtf8(""), []), "empty: fromUtf8");
}

{
    function detachTrap(ab) {
        return { valueOf() { ab.transfer(); return 0; } };
    }

    {
        const ab = new ArrayBuffer(8);
        const view = new Uint8Array(ab);
        assertThrows(() => readUint32LE(view, detachTrap(ab)),
            "readUint32LE: reentrant detach via offset valueOf");
    }
    {
        const ab = new ArrayBuffer(8);
        const dv = new DataView(ab);
        assertThrows(() => writeUint32LE(dv, detachTrap(ab), 0xdeadbeef),
            "writeUint32LE(DataView): reentrant detach via offset valueOf");
    }
    {
        const ab = new ArrayBuffer(8);
        const view = new Uint8Array(ab);
        assertThrows(() => writeUint32LE(view, detachTrap(ab), 0xdeadbeef),
            "writeUint32LE: reentrant detach via offset valueOf");
    }
    {
        const ab = new ArrayBuffer(8);
        const view = new Uint8Array(ab);
        assertThrows(() => writeUint32LE(view, 0, detachTrap(ab)),
            "writeUint32LE: reentrant detach via value valueOf");
    }
    {
        const ab = new ArrayBuffer(8);
        const view = new Uint8Array(ab);
        assertThrows(() => fill(view, 1, detachTrap(ab)),
            "fill: reentrant detach via start valueOf");
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
        const view = new Uint8Array(8);
        writeUint32LE(view, 0, 0xdeadbeef);
        assert(readUint32LE(view, 0) === 0xdeadbeef, "sanity: non-reentrant call still works");
    }
}

{
    const b = new Bytes("abc");
    assert(b.isAscii === true && b.isValidUtf8 === true, "Bytes: flags true at construction");
    b.fill(0xFF);
    assert(b.isAscii === false, "Bytes: fill(0xFF) invalidates isAscii");
    assert(b.isValidUtf8 === false, "Bytes: fill(0xFF) invalidates isValidUtf8");
    b.fill(65);
    assert(b.isAscii === true && b.isValidUtf8 === true, "Bytes: refill with ASCII recomputes to true");

    const w = Bytes.alloc(8);
    assert(w.isAscii === true, "Bytes: a zeroed buffer is ASCII");
    w.writeUint32LE(0, 0xFFFFFFFF);
    assert(w.isAscii === false && w.isValidUtf8 === false, "Bytes: writeUint32LE invalidates the flags");
    w.writeUint8(0, 65);
    assert(w.isAscii === false, "Bytes: an ASCII write into non-ASCII stays honestly false");

    const t = new Bytes("hello");
    t.array[1] = 0xFF;
    assert(t.isAscii === false, "Bytes: a write through .array invalidates isAscii");
    assert(t.isValidUtf8 === false, "Bytes: a write through .array invalidates isValidUtf8");

    const h = new Bytes("abc");
    fill(h, 0xFF);
    assert(h.isAscii === false, "Bytes: free fill() on a handle invalidates its flags");

    const p = new Bytes("abc");
    const s = p.slice(0, 2);
    assert(s.isAscii === true, "Bytes: an untouched slice of ASCII is ASCII");
    s.array[0] = 0xFF;
    assert(s.isAscii === false, "Bytes: the slice's own flags recompute after a write");
}

{
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
}

{
    let msg = null;
    try { utf8ToLatin1(fromUtf8("ok😀")); }
    catch (e) { msg = String(e); }
    assert(msg !== null && /code point > 0xFF/.test(msg),
        "utf8ToLatin1: a wide code point is its own error (got " + msg + ")");
    msg = null;
    try { utf8ToLatin1(u8(0xC3, 0x28)); }
    catch (e) { msg = String(e); }
    assert(msg !== null && /invalid UTF-8/.test(msg) && !/> 0xFF/.test(msg),
        "utf8ToLatin1: invalid UTF-8 is its own error (got " + msg + ")");
}

{
    assertThrows(() => countUtf16(u8(0x41, 0x00, 0x42)),
        "countUtf16: odd byte length throws", RangeError);
    assert(countUtf16(u8(0x41, 0x00, 0x42, 0x00)) === 2,
        "countUtf16: an even length still counts");
}

print("test_bytes: all tests passed (" + n + " assertions)");

{
    const expect = (bytes, want, what) => {
        assert(toUtf8(new Uint8Array(bytes)) === want,
            "toUtf8 lossy: " + what, JSON.stringify(toUtf8(new Uint8Array(bytes))));
        assert(new Bytes(new Uint8Array(bytes)).toUtf8() === want,
            "Bytes.toUtf8 lossy: " + what);
    };
    expect([0x61, 0xC3, 0x28], "a\uFFFD(", "truncated 2-byte at ASCII");
    expect([0xED, 0xA0, 0x80], "\uFFFD\uFFFD\uFFFD", "WTF-8 surrogate -> 3 x U+FFFD, not a lone surrogate");
    expect([0xC0, 0xAF], "\uFFFD\uFFFD", "overlong 2-byte -> 2 x U+FFFD");
    expect([0xF4, 0x90, 0x80, 0x80], "\uFFFD\uFFFD\uFFFD\uFFFD", "out of range -> 4 x U+FFFD");
    expect([0xE2, 0x82, 0x41], "\uFFFDA", "bad continuation consumed as one subpart");
    expect([0xF0, 0x9F, 0x98, 0x80], "\u{1F600}", "valid 4-byte unchanged");
    assert(toUtf8(new Uint8Array()) === "", "toUtf8 lossy: empty stays empty");
}

{
    const b = new Bytes("hello world");
    assert(b.slice(100).length === 0, "slice(100) past end -> empty");
    assert(b.slice(100, 105).length === 0, "slice(100,105) past end -> empty");
    assert(b.slice(11).length === 0, "slice(len) -> empty");
    assert(b.slice(5).toString() === " world", "slice(5) in range unchanged");
    assert(b.slice(-100).toString() === "hello world", "slice(-100) clamps low");
}

{
    assertThrows(() => concat(5), "concat(number) throws", TypeError);
    assertThrows(() => concat(undefined), "concat(undefined) throws", TypeError);
    assertThrows(() => concat("ab"), "concat(string) throws", TypeError);
    assert(eqArr(concat([u8(1), u8(2, 3)]), [1, 2, 3]), "concat(array) still works");
}

{
    assertThrows(() => Bytes.alloc(2 ** 31), "alloc(2^31) throws the module error", RangeError);
    assertThrows(() => Bytes.alloc(2 ** 32), "alloc(2^32) throws", RangeError);
    assert(Bytes.alloc(0).length === 0, "alloc(0) still works");
}
