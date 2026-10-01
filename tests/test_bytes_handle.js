import { Bytes, Text, compare, equal, isValidUtf8, countUtf8 } from "dyna:bytes";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) { assert(a === b, msg + " (got " + a + ", want " + b + ")"); }
function throws(fn, msg) {
    n++;
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    if (caught === null) throw new Error("assertion failed: " + msg + " (expected throw)");
}

{
    const b = new Bytes("hello");
    eq(b.length, 5, "length");
    eq(b.isAscii, true, "pure ASCII");
    eq(b.isValidUtf8, true, "ASCII is valid UTF-8");
    eq(b.toString(), "hello", "toString decodes UTF-8");

    const w = new Bytes("héllo→");
    eq(w.isAscii, false, "non-ASCII detected");
    eq(w.isValidUtf8, true, "well-formed UTF-8");
    eq(w.length, 9, "byte length, not character count");

    const bad = new Bytes(new Uint8Array([0x41, 0xC3, 0x28]));
    eq(bad.isValidUtf8, false, "a truncated sequence is not valid UTF-8");
    eq(bad.isAscii, false, "and it is not ASCII either");

    eq(new Bytes(new Uint8Array([0xC0, 0x80])).isValidUtf8, false, "overlong NUL");
    eq(new Bytes(new Uint8Array([0xED, 0xA0, 0x80])).isValidUtf8, false, "surrogate");
    eq(new Bytes(new Uint8Array([0xF5, 0x80, 0x80, 0x80])).isValidUtf8, false, "above U+10FFFF");

    eq(new Bytes("").length, 0, "the empty buffer is legal");
    eq(new Bytes("").isAscii, true, "and vacuously ASCII");
    throws(() => new Bytes(), "the data argument is required");
    throws(() => new Bytes(new Float64Array(2)),
        "a wider view is refused rather than reinterpreted");
}

{
    const src = new Uint8Array([65, 66, 67]);
    const copied = new Bytes(src);
    eq(copied.isAscii, true, "ASCII at construction");
    src[0] = 0xFF;
    eq(copied.toString().charCodeAt(0), 65, "the handle did not see the write");
    eq(copied.isAscii, true, "so its cached flag is still true");

    const owner = new Bytes("abcdef");
    const mid = owner.slice(1, 4);
    eq(mid.toString(), "bcd", "the slice reads the right window");
    eq(mid.length, 3, "and has the right length");
    mid.fill(88);
    eq(owner.toString(), "aXXXef", "a write through the view reaches the owner");

    eq(owner.slice(-2).toString(), "ef", "a negative start counts from the end");
    eq(owner.slice(0, 99).length, 6, "an end past the buffer clamps");
    eq(owner.slice(4, 1).length, 0, "an inverted range is empty, not negative");
    eq(owner.slice().length, 6, "no arguments is the whole buffer");

    const inner = owner.slice(1, 5).slice(1, 3);
    inner.fill(89);
    assert(owner.toString().includes("YY"), "a nested view still reaches the root owner");
}

{
    let view;
    {
        const big = new Bytes("Z".repeat(4096));
        view = big.slice(0, 8);
    }
    if (typeof gc === "function") { gc(); gc(); }
    eq(view.toString(), "ZZZZZZZZ", "a view keeps its owner's buffer alive");
    eq(view.length, 8, "and stays the right size");

    const owner = new Bytes("0123456789".repeat(100));
    for (let i = 0; i < 5000; i++) {
        const v = owner.slice(i % 900, (i % 900) + 10);
        if (v.length !== 10) throw new Error("view width at i=" + i);
    }
    if (typeof gc === "function") gc();
    eq(owner.length, 1000, "the owner is intact after 5000 views");
}

{
    const z = Bytes.alloc(4);
    eq(z.length, 4, "alloc gives the requested length");
    eq(z.readUint32LE(0), 0, "and it is zeroed");

    eq(Bytes.concat([new Bytes("ab"), new Bytes("cd"), new Bytes("")]).toString(),
       "abcd", "concat joins in order and tolerates empties");
    eq(Bytes.concat([]).length, 0, "concat of nothing is empty");
    eq(Bytes.concat([new Uint8Array([1, 2])]).length, 2, "concat accepts raw views too");

    assert(Bytes.isBytes(new Bytes("x")), "isBytes accepts a handle");
    assert(!Bytes.isBytes("x") && !Bytes.isBytes(new Uint8Array(1)) && !Bytes.isBytes(null),
        "isBytes rejects everything else");
    throws(() => Bytes.alloc(), "alloc needs a length");
    throws(() => Bytes.concat("nope"), "concat needs an array");
}

{
    const b = new Bytes("hello world");
    eq(b.indexOf(new Bytes("o").array), 4, "indexOf a needle view");
    eq(b.lastIndexOf(new Bytes("o").array), 7, "lastIndexOf");
    eq(b.includes(new Bytes("world").array), true, "includes");
    eq(b.count(new Bytes("l").array), 3, "count");
    eq(b.indexOfAny("dw"), 6, "indexOfAny finds the earliest of a set");
    eq(b.compare(new Bytes("hello world").array), 0, "compare equal");
    assert(b.equals(new Bytes("hello world").array), "equals");

    eq(compare(new Bytes("a"), new Bytes("b")), -1, "a free function takes handles");
    assert(equal(new Bytes("q"), new Uint8Array([113])), "handle vs raw view");

    const n4 = Bytes.alloc(8);
    n4.writeUint32LE(0, 0xdeadbeef);
    n4.writeUint32BE(4, 0xdeadbeef);
    eq(n4.readUint32LE(0), 0xdeadbeef, "LE round trip");
    eq(n4.readUint32BE(4), 0xdeadbeef, "BE round trip");
    assert(n4.readUint32LE(4) !== n4.readUint32BE(4), "LE and BE really differ");
    n4.writeDoubleLE(0, 1.5);
    eq(n4.readDoubleLE(0), 1.5, "double round trip");
    throws(() => n4.readUint32LE(99), "an out-of-range offset throws");

    const acc = Object.getOwnPropertyNames(Object.getPrototypeOf(b))
        .filter((k) => /^(read|write)/.test(k));
    eq(acc.length, 38, "every read*/write* accessor is on the handle");
    const wide = Bytes.alloc(24);
    wide.writeInt16LE(0, -2);
    eq(wide.readInt16LE(0), -2, "readInt16LE was missing entirely once");
    wide.writeBigUint64LE(2, 0xdeadbeefcafen);
    eq(wide.readBigUint64LE(2), 0xdeadbeefcafen, "64-bit accessors were missing too");
    wide.writeDoubleBE(10, 1.5);
    eq(wide.readDoubleBE(10), 1.5, "and every big-endian one");
}

{
    const t = new Text("héllo");
    eq(t.value, "héllo", "value is the string");
    eq(t.toString(), "héllo", "toString");
    eq(JSON.stringify({ t }), '{"t":"héllo"}', "toJSON");
    eq(t.countUtf8(), 5, "code points, not bytes");
    eq(t.isValidUtf8, true, "valid (DT-8: getter)");
    eq(t.toBytes().length, 6, "é is two bytes in UTF-8");
    assert(Bytes.isBytes(t.toBytes()), "toBytes yields a Bytes handle");

    eq(t.isWide, false, "Latin-1 is not wide");
    eq(new Text("héllo→").isWide, true, "U+2192 is wide");
    eq(new Text("abc").isWide, false, "ASCII is not wide");

    eq(new Text("").countUtf8(), 0, "the empty text");
    eq(new Text(42).value, "42", "a non-string argument is coerced once");
    throws(() => new Text(), "the argument is required");

    for (const m of ["isValidUtf16", "countUtf8", "countUtf16",
                     "latin1ToUtf8", "utf8ToLatin1", "utf8ToUtf16", "utf16ToUtf8",
                     "toUtf8", "toBytes", "toString", "toJSON"])
        assert(typeof new Text("x")[m] === "function", "Text has " + m);
    {
        const d = Object.getOwnPropertyDescriptor(
            Object.getPrototypeOf(new Text("x")), "isValidUtf8");
        assert(d && typeof d.get === "function", "Text has isValidUtf8 (getter)");
    }

    assert(isValidUtf8("ok") === true, "isValidUtf8 survived the fold");
    eq(countUtf8("héllo"), 5, "countUtf8 survived the fold");
}

{
    const b = new Bytes("abcdefgh");
    let ran = 0;
    const evil = { valueOf() { ran++; return 2; } };
    eq(b.slice(evil, 5).toString(), "cde", "a coercing bound is honoured");
    assert(ran > 0, "the valueOf hook actually fired, so this tested something");

    throws(() => new Bytes({ toString() { return "xx"; } }),
        "a plain object is not byte data");
}

print("test_bytes_handle: all " + n + " assertions passed");
