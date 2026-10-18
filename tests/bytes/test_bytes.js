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
    assert(sub[0] === 3, "subarray view resolves its own byteOffset");
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
}

{
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
        assert(new Bytes(new Uint8Array(bytes)).toString() === want,
            "Bytes.toString lossy: " + what);
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
