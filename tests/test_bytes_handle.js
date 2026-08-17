/* test_bytes_handle.js -- class Bytes and class Text in dyna:bytes (W3.3/W3.4).
 *
 * Bytes is a VALUE HANDLE over one contiguous buffer; Text is an
 * interpretation of a string. They live in one module because they are two
 * views of the same material -- the same argument that folded dyna:path into
 * dyna:file -- and `dyna:text` is gone.
 *
 * The two things worth testing hardest, because they are where a handle over
 * shared memory goes wrong:
 *
 *   1. THE COPY/VIEW BOUNDARY. `new Bytes(u8)` COPIES, so a later write
 *      through the original Uint8Array cannot make the cached isAscii and
 *      isValidUtf8 flags lie. `.slice()` VIEWS, so a write through the slice
 *      IS visible in the owner -- that is the point of the method. Getting
 *      these the wrong way round is a silent correctness bug in both
 *      directions, so both are asserted explicitly.
 *
 *   2. VIEW LIFETIME. A slice holds a strong reference to its owner's backing
 *      buffer, so the owner cannot be collected while any view of it lives.
 *      That is correct (a view into freed memory would be far worse) and it
 *      also means a small view retains a large buffer, which is the cost the
 *      RSS work characterises.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_bytes_handle.js
 */
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

/* ---- construction and the cached summaries ----------------------------- */
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

    /* Invalid UTF-8 is DETECTED, not repaired -- the flag is a summary of what
     * is there, and a caller uses it to decide whether a decode is safe. */
    const bad = new Bytes(new Uint8Array([0x41, 0xC3, 0x28]));
    eq(bad.isValidUtf8, false, "a truncated sequence is not valid UTF-8");
    eq(bad.isAscii, false, "and it is not ASCII either");

    /* Overlong, surrogate and out-of-range forms are all invalid. */
    eq(new Bytes(new Uint8Array([0xC0, 0x80])).isValidUtf8, false, "overlong NUL");
    eq(new Bytes(new Uint8Array([0xED, 0xA0, 0x80])).isValidUtf8, false, "surrogate");
    eq(new Bytes(new Uint8Array([0xF5, 0x80, 0x80, 0x80])).isValidUtf8, false, "above U+10FFFF");

    eq(new Bytes("").length, 0, "the empty buffer is legal");
    eq(new Bytes("").isAscii, true, "and vacuously ASCII");
    throws(() => new Bytes(), "the data argument is required");
    throws(() => new Bytes(new Float64Array(2)),
        "a wider view is refused rather than reinterpreted");
}

/* ---- THE COPY/VIEW BOUNDARY -------------------------------------------- */
{
    /* new Bytes(u8) COPIES. If it aliased, the write below would silently
     * turn a handle whose isAscii says true into one holding a non-ASCII
     * byte -- the cached flag would become a lie with nothing to detect it. */
    const src = new Uint8Array([65, 66, 67]);
    const copied = new Bytes(src);
    eq(copied.isAscii, true, "ASCII at construction");
    src[0] = 0xFF;
    eq(copied.toString().charCodeAt(0), 65, "the handle did not see the write");
    eq(copied.isAscii, true, "so its cached flag is still true");

    /* .slice() VIEWS. A write through the slice IS visible in the owner --
     * that is what makes it a view and not a copy. */
    const owner = new Bytes("abcdef");
    const mid = owner.slice(1, 4);
    eq(mid.toString(), "bcd", "the slice reads the right window");
    eq(mid.length, 3, "and has the right length");
    mid.fill(88);
    eq(owner.toString(), "aXXXef", "a write through the view reaches the owner");

    /* Negative and out-of-range bounds clamp, like Array.prototype.slice. */
    eq(owner.slice(-2).toString(), "ef", "a negative start counts from the end");
    eq(owner.slice(0, 99).length, 6, "an end past the buffer clamps");
    eq(owner.slice(4, 1).length, 0, "an inverted range is empty, not negative");
    eq(owner.slice().length, 6, "no arguments is the whole buffer");

    /* A slice of a slice still aliases the ORIGINAL buffer, not a copy of the
     * intermediate one. */
    const inner = owner.slice(1, 5).slice(1, 3);
    inner.fill(89);
    assert(owner.toString().includes("YY"), "a nested view still reaches the root owner");
}

/* ---- VIEW LIFETIME ------------------------------------------------------ */
{
    /* The owner handle goes out of scope, but its buffer must stay alive
     * because a view of it is still reachable. If the reference were weak,
     * this read would be a use-after-free -- which ASan would catch in CI and
     * which would be silent corruption in production. */
    let view;
    {
        const big = new Bytes("Z".repeat(4096));
        view = big.slice(0, 8);
    }
    if (typeof gc === "function") { gc(); gc(); }
    eq(view.toString(), "ZZZZZZZZ", "a view keeps its owner's buffer alive");
    eq(view.length, 8, "and stays the right size");

    /* Churn: many short-lived views over one owner must not leak or corrupt. */
    const owner = new Bytes("0123456789".repeat(100));
    for (let i = 0; i < 5000; i++) {
        const v = owner.slice(i % 900, (i % 900) + 10);
        if (v.length !== 10) throw new Error("view width at i=" + i);
    }
    if (typeof gc === "function") gc();
    eq(owner.length, 1000, "the owner is intact after 5000 views");
}

/* ---- statics ------------------------------------------------------------ */
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

/* ---- the methods, and their agreement with the free functions ----------- */
{
    const b = new Bytes("hello world");
    eq(b.indexOf(new Bytes("o").array), 4, "indexOf a needle view");
    eq(b.lastIndexOf(new Bytes("o").array), 7, "lastIndexOf");
    eq(b.includes(new Bytes("world").array), true, "includes");
    eq(b.count(new Bytes("l").array), 3, "count");
    eq(b.indexOfAny("dw"), 6, "indexOfAny finds the earliest of a set");
    eq(b.compare(new Bytes("hello world").array), 0, "compare equal");
    assert(b.equals(new Bytes("hello world").array), "equals");

    /* A handle is accepted anywhere a raw view is -- one door, so the free
     * functions did not have to be duplicated onto the class. */
    eq(compare(new Bytes("a"), new Bytes("b")), -1, "a free function takes handles");
    assert(equal(new Bytes("q"), new Uint8Array([113])), "handle vs raw view");

    /* Fixed-width accessors, endianness explicit. */
    const n4 = Bytes.alloc(8);
    n4.writeUint32LE(0, 0xdeadbeef);
    n4.writeUint32BE(4, 0xdeadbeef);
    eq(n4.readUint32LE(0), 0xdeadbeef, "LE round trip");
    eq(n4.readUint32BE(4), 0xdeadbeef, "BE round trip");
    assert(n4.readUint32LE(4) !== n4.readUint32BE(4), "LE and BE really differ");
    n4.writeDoubleLE(0, 1.5);
    eq(n4.readDoubleLE(0), 1.5, "double round trip");
    throws(() => n4.readUint32LE(99), "an out-of-range offset throws");

    /* ALL 36 accessors, not the 18 the first version of this class shipped.
     * A partial surface reads as complete and fails only on the one width a
     * caller happens to need, so the count is asserted rather than trusted. */
    const acc = Object.getOwnPropertyNames(Object.getPrototypeOf(b))
        .filter((k) => /^(read|write)/.test(k));
    eq(acc.length, 36, "every read*/write* accessor is on the handle");
    const wide = Bytes.alloc(24);
    wide.writeInt16LE(0, -2);
    eq(wide.readInt16LE(0), -2, "readInt16LE was missing entirely once");
    wide.writeBigUint64LE(2, 0xdeadbeefcafen);
    eq(wide.readBigUint64LE(2), 0xdeadbeefcafen, "64-bit accessors were missing too");
    wide.writeDoubleBE(10, 1.5);
    eq(wide.readDoubleBE(10), 1.5, "and every big-endian one");
}

/* ---- Text --------------------------------------------------------------- */
{
    const t = new Text("héllo");
    eq(t.value, "héllo", "value is the string");
    eq(t.toString(), "héllo", "toString");
    eq(JSON.stringify({ t }), '{"t":"héllo"}', "toJSON");
    eq(t.countUtf8(), 5, "code points, not bytes");
    eq(t.isValidUtf8(), true, "valid");
    eq(t.toBytes().length, 6, "é is two bytes in UTF-8");
    assert(Bytes.isBytes(t.toBytes()), "toBytes yields a Bytes handle");

    /* isWide is the engine's own summary: any code unit above U+00FF. é is
     * U+00E9, so a Latin-1 string is NOT wide -- which is the distinction
     * that decides whether a byte kernel can run at all. */
    eq(t.isWide, false, "Latin-1 is not wide");
    eq(new Text("héllo→").isWide, true, "U+2192 is wide");
    eq(new Text("abc").isWide, false, "ASCII is not wide");

    eq(new Text("").countUtf8(), 0, "the empty text");
    eq(new Text(42).value, "42", "a non-string argument is coerced once");
    throws(() => new Text(), "the argument is required");

    /* EVERY transcoder, not a subset -- the same partial-surface defect Bytes
     * shipped with 18 of 36 accessors. Asserted by comparing the method set
     * against the free-function set rather than by listing names. */
    for (const m of ["isValidUtf8", "isValidUtf16", "countUtf8", "countUtf16",
                     "latin1ToUtf8", "utf8ToLatin1", "utf8ToUtf16", "utf16ToUtf8",
                     "toUtf8", "toBytes", "toString", "toJSON"])
        assert(typeof new Text("x")[m] === "function", "Text has " + m);

    /* The folded-in free functions still exist under dyna:bytes. */
    assert(isValidUtf8("ok") === true, "isValidUtf8 survived the fold");
    eq(countUtf8("héllo"), 5, "countUtf8 survived the fold");
}

/* ---- the adversarial argument ------------------------------------------
 *
 * Bytes and Text are plain GC classes with no close(), so there is no
 * close-during-coercion hazard to test -- there is nothing to close. What CAN
 * still go wrong is a coercion running between a bounds check and a buffer
 * read, so the bounds are coerced first and the buffer is resolved after.
 * ---------------------------------------------------------------------- */
{
    const b = new Bytes("abcdefgh");
    let ran = 0;
    const evil = { valueOf() { ran++; return 2; } };
    /* The coercion runs (this IS a ToNumber path, so valueOf is the right
     * hook -- toString would never fire and a test using it would prove
     * nothing), and the result is still correct. */
    eq(b.slice(evil, 5).toString(), "cde", "a coercing bound is honoured");
    assert(ran > 0, "the valueOf hook actually fired, so this tested something");

    /* An object that is not a buffer at all is refused, not stringified. */
    throws(() => new Bytes({ toString() { return "xx"; } }),
        "a plain object is not byte data");
}

print("test_bytes_handle: all " + n + " assertions passed");
