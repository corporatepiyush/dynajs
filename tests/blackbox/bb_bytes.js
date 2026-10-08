// Black-box contract test for dyna:bytes, generated from dynajs.d.ts lines 1-368. Engine sources not consulted; every expectation cites the contract.
import {
    Bytes, Text, bytesOf, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy,
    fill, toUtf8, fromUtf8, isValidUtf8, isValidUtf16, countUtf8, countUtf16, latin1ToUtf8,
    utf8ToLatin1, utf8ToUtf16, utf16ToUtf8, decode, encode, encodingExists, encodings,
    readUint8, readUint16BE, readUint32LE, readUint32BE, readBigUint64LE, readBigInt64LE,
    readFloatLE, readFloatBE, readDoubleLE, readDoubleBE,
    writeUint8, writeUint16BE, writeUint16LE, writeUint32LE, writeUint32BE, writeBigUint64LE,
} from "dyna:bytes";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function eqArr2(a, b) { return eqArr(Array.from(a), Array.from(b)); }
function u8(...bytes) { return new Uint8Array(bytes); }
function s2b(s) { return fromUtf8(s); }

/* Table runners: every row is [args..., expected] (or [args..., ErrorClass] for refusals).
   An expected may be a predicate(got). The failure message always names the row. */
function labelOf(name, args) {
    return name + "(" + args.map(a => typeof a === "string" ? JSON.stringify(a) : (a instanceof Uint8Array ? "u8[" + a.length + "]" : String(a))).join(", ") + ")";
}
function J(v) { return typeof v === "bigint" ? v + "n" : JSON.stringify(v, (k, x) => typeof x === "bigint" ? x + "n" : x); }
function assertCases(fn, cases, name) {
    for (const row of cases) {
        const args = row.slice(0, row.length - 1);
        const want = row[row.length - 1];
        n++;
        let got, threw = null;
        try { got = fn(...args); } catch (e) { threw = e; }
        const label = labelOf(name, args);
        if (threw) throw new Error("case threw " + threw + ": " + label);
        if (typeof want === "function") { if (!want(got)) throw new Error("case predicate failed: " + label + " — got |" + J(got) + "|"); }
        else if (J(got) !== J(want)) throw new Error("case failed: " + label + " — got |" + J(got) + "| expected |" + J(want) + "|");
    }
}
function assertCasesThrow(fn, cases, name) {
    for (const row of cases) {
        const args = row.slice(0, row.length - 1);
        const Err = row[row.length - 1];
        n++;
        let threw = false, err = null;
        try { fn(...args); } catch (e) { threw = true; err = e; }
        const label = labelOf(name, args);
        if (!threw) throw new Error("expected throw: " + label);
        if (Err && !(err instanceof Err)) throw new Error("wrong error class " + (err && err.constructor ? err.constructor.name : String(err)) + ": " + label);
    }
}

/* ---------------- Bytes.isBytes / alloc / static concat vs free concat ---------------- */
{
    // d.ts: "True when `v` is a Bytes handle."
    assertCases((v) => Bytes.isBytes(v), [
        [new Bytes("x"), true],
        [new Uint8Array(1), false],
        [new DataView(new ArrayBuffer(2)), false],
        ["x", false],
        [null, false],
    ], "Bytes.isBytes");

    // d.ts: "Zero-filled buffer; lengths up to 2^31 bytes."
    assertCases((k) => { const z = Bytes.alloc(k); let zero = true; for (let i = 0; i < z.length; i++) if (z.readUint8(i) !== 0) zero = false; return [z.length, zero]; }, [
        [0, [0, true]],
        [16, [16, true]],
    ], "Bytes.alloc");

    // d.ts: `static concat(...): Bytes` (class door) vs free "Concatenates byte views into one Uint8Array".
    assertCases((...views) => { const r = Bytes.concat(...views); return [Bytes.isBytes(r), r.length, Array.from(r.array)]; }, [
        [[u8(1, 2), u8(3)], [true, 3, [1, 2, 3]]],      // array form
        [u8(4), u8(5, 6), [true, 3, [4, 5, 6]]],        // "Variadic form"
        [[u8(1)], u8(2, 3), [true, 3, [1, 2, 3]]],      // "a leading array and further views mix freely"
    ], "Bytes.concat");
    assertCases((list) => { const r = concat(list); return [r instanceof Uint8Array, Bytes.isBytes(r), Array.from(r)]; }, [
        [[u8(1, 2), u8(3)], [true, false, [1, 2, 3]]],  // free door: Uint8Array, NOT a handle
        [[], [true, false, []]],
        [[u8(), u8()], [true, false, []]],
    ], "concat(free)");
}

/* ---------------- constructor: copied bytes + cached flags ---------------- */
{
    // d.ts: constructor(data: string | ByteView); "Copied byte buffer".
    assertCases((make) => make(), [
        [() => { const src = u8(0x41, 0x42); const b = new Bytes(src); src[0] = 0xFF; return b.readUint8(0); }, 0x41], // copied, never aliased
        [() => new Bytes("héllo").length, 6],            // string -> UTF-8 (é is 2 bytes)
        [() => { const d = new Bytes(new DataView((new Uint8Array([5, 6, 7])).buffer)); return [d.length, d.readUint8(2)]; }, [3, 7]],
        [() => new Bytes(new Uint8Array([9, 8]).buffer).length, 2],
    ], "new Bytes");

    // ByteView excludes Uint16Array — wrong container type must refuse (TypeError).
    assertCasesThrow((v) => new Bytes(v), [
        [new Uint16Array(2), TypeError],
        [[1, 2, 3], TypeError],
    ], "new Bytes");

    // d.ts: "computed once at construction" flags; dynajs.d.ts: a slice "inherits both flags without rescanning".
    assertCases((make) => make(), [
        [() => new Bytes("hello").isAscii, true],
        [() => new Bytes("héllo").isAscii, false],                    // é has the high bit set
        [() => new Bytes(s2b("hi")).isValidUtf8, true],
        [() => new Bytes(u8(0xC3, 0x28)).isValidUtf8, false],         // truncated 2-byte sequence
        [() => new Bytes(u8(0xC3, 0xA9)).isValidUtf8, true],
        [() => { const a = Bytes.alloc(4); return [a.isAscii, a.isValidUtf8]; }, [true, true]], // NULs are ASCII + valid UTF-8
        [() => { const b = new Bytes("abcdef"); const s = b.slice(1, 4); return [s.isAscii, s.isValidUtf8]; }, [true, true]],
        // DOC-TENSION: d.ts says the flags are "computed once at construction", but fill() is a
        // documented post-construction mutator (chainable handle); we pin recompute-on-write.
        [() => { const m = new Bytes("abc"); m.fill(0xFF); return [m.isAscii, m.isValidUtf8]; }, [false, false]],
    ], "flags");
}

/* ---------------- slice: a VIEW sharing the owner's ArrayBuffer ---------------- */
{
    assertCases((make) => make(), [
        // d.ts: "A new Bytes handle that is a view sharing the owner's ArrayBuffer."
        [() => { const o = new Bytes("hello world"); const s = o.slice(6, 11); return [Bytes.isBytes(s), s.length, s.toString()]; }, [true, 5, "world"]],
        [() => { const o = new Bytes("hello world"); const s = o.slice(6, 11); s.fill(0x2A); return o.indexOf(42); }, 6], // writes alias the owner
        [() => new Bytes("hello world").slice(-3).toString(), "rld"],   // negative counts from the end
        [() => new Bytes("hello").slice(2, 1).length, 0],               // end < start -> empty view
        [() => new Bytes("hello").slice(100).length, 0],                // clamped past end
        [() => new Bytes("hello").slice(100, 105).length, 0],
    ], "Bytes.slice");
}

/* ---------------- method <-> free map: equals / includes / compare ---------------- */
{
    // d.ts map: "method Bytes.equals(v) <-> free equal(a, b)"; includes <-> contains.
    const b = new Bytes("abc");
    const raw = u8(0x61, 0x62, 0x63);
    assertCases((f) => f(), [
        [() => b.compare(raw), 0],
        [() => b.equals(raw) === equal(b.array, raw), true],      // parity across doors
        [() => b.equals(u8(0x61, 0x62)), false],                  // length mismatch
        [() => b.includes(0x62) === contains(b.array, 0x62), true],
        [() => b.includes(0x7A) === contains(b.array, 0x7A), true],
        [() => compare(u8(1, 2), u8(1, 2, 3)), -1],               // prefix is less
        [() => compare(u8(1, 2, 3), u8(1, 2)), 1],
        [() => compare(u8(), u8()), 0],
    ], "compare/equals/includes");
}

/* ---------------- indexOf / lastIndexOf / count conventions ---------------- */
{
    const hay = new Bytes("abcabc");
    // d.ts: indexOf "`fromIndex` starts the search there (negative clamps to 0)"
    assertCases((...a) => hay.indexOf(...a), [
        [0x62, 1],
        [0x62, 2, 4],
        [0x62, -10, 1],
        [u8(), 0],                       // empty needle -> 0
        [u8(), 3, 3],                    // empty needle at the clamped fromIndex
        [u8(0x62, 0x63), 1],
        [u8(0x62, 0x63), 2, 4],
    ], "Bytes.indexOf");
    // a string is NOT a ByteView: the needle union is `number | ByteView` only.
    assertCasesThrow((...a) => hay.indexOf(...a), [
        ["bc", TypeError],
    ], "Bytes.indexOf");
    // d.ts: lastIndexOf "the empty needle matches at `length`. With `fromIndex`, the match must
    // start at or before it (the search is backward); negative clamps to 0."
    assertCases((...a) => hay.lastIndexOf(...a), [
        [0x62, 4],
        [u8(0x62, 0x63), 4],
        [u8(0x62, 0x63), 3, 1],
        [0x61, 2, 0],
        [0x61, -5, 0],
        [u8(), 6],                       // empty needle -> length
        [u8(), 2, 2],                    // empty needle at fromIndex
        [u8(0x62, 0x63), -1, -1],        // clamped to 0; 'bc' does not start at 0
    ], "Bytes.lastIndexOf");
    // d.ts: count "non-overlapping"; empty needle -> length+1; with fromIndex -> length-fromIndex+1;
    // negative fromIndex clamps to 0.
    assertCases((...a) => hay.count(...a), [
        [0x61, 2],
    ], "Bytes.count");
    assertCases((...a) => new Bytes("aaaa").count(...a), [
        [u8(0x61, 0x61), 2],
        [u8(0x61, 0x61), 1, 1],          // scan starts at fromIndex
    ], "Bytes.count");
    assertCases((...a) => new Bytes("ab").count(...a), [
        [u8(), 3],                       // length + 1
        [u8(), 1, 2],                    // length - fromIndex + 1
        [u8(), -3, 3],                   // negative clamps to 0
    ], "Bytes.count");
    assertCases((...a) => new Bytes("").count(...a), [
        [u8(), 1],                       // empty haystack, empty needle -> 1
    ], "Bytes.count");
    // free-function mirrors over plain views (d.ts: same names, buffer first).
    assertCases((...a) => indexOf(...a), [
        [u8(1, 2, 3, 2, 3), 3, 3, 4],
        [u8(1, 2, 3), u8(), 0],
    ], "indexOf(free)");
    assertCases((...a) => lastIndexOf(...a), [
        [u8(1, 2, 3, 2, 3), u8(2, 3), 2, 1],
    ], "lastIndexOf(free)");
    assertCases((...a) => count(...a), [
        [u8(5, 5, 5, 5), 5, 2, 2],
    ], "count(free)");
    assertCases((...a) => contains(...a), [
        [u8(1, 2, 3), u8(), true],
        [u8(1, 2, 3), 9, false],
    ], "contains(free)");
}

/* ---------------- indexOfAny / startsWith / endsWith (method-only doors) ---------------- */
{
    const b = new Bytes("a,b,,c"); // commas at 1, 3, 4
    // d.ts: "First position at or after `fromIndex` holding any byte of the `chars` view, or -1."
    assertCases((...a) => b.indexOfAny(...a), [
        [u8(44), 1],
        // DOC-TENSION: dynajs.d.ts's example prints 4 for indexOfAny([44], 3) on "a,b,,c", but the d.ts
        // sentence is unambiguous — "at or after fromIndex" — and position 3 holds a comma.
        [u8(44), 3, 3],
        [u8(44), 5, -1],
        [u8(0x7E), -1],
        [u8(98, 44), 2, 2],
        // TEST-FIX: the row here previously expected 11, but 11 is the ',' position in
        // "hello world, hello bytes" — that API-pinned vector lives in the table below. On
        // "a,b,,c" the d.ts answer for indexOfAny([',','!']) is 1 (',' at index 1), and with a
        // fromIndex past the end nothing is "at or after" it -> -1.
        [u8(44, 33), 1],
        [u8(44, 33), 11, -1],
    ], "Bytes.indexOfAny");
    assertCases((...a) => new Bytes("hello world, hello bytes").indexOfAny(...a), [
        [u8(44, 33), 11],
    ], "Bytes.indexOfAny");

    // d.ts: startsWith "String.prototype.startsWith semantics; default 0" — the SEMANTICS phrase
    // covers fromIndex handling; the needle union is `number | ByteView` (a string is NOT a
    // ByteView, same as indexOf — TEST-FIX: rows previously passed strings).
    const w = new Bytes("hello world");
    const HELLO = u8(0x68, 0x65, 0x6C, 0x6C, 0x6F), WORLD = u8(0x77, 0x6F, 0x72, 0x6C, 0x64);
    assertCases((...a) => w.startsWith(...a), [
        [HELLO, true],
        [0x68, true],
        [WORLD, 6, true],
        [HELLO, 1, false],
        [HELLO, -5, true],               // negative fromIndex clamps to 0
        [HELLO, 100, false],
    ], "Bytes.startsWith");
    assertCasesThrow((...a) => w.startsWith(...a), [
        ["hello", TypeError],            // string is outside the needle union
    ], "Bytes.startsWith");
    // d.ts: endsWith "a match must END there" (end exclusive, default length).
    assertCases((...a) => w.endsWith(...a), [
        [WORLD, true],
        [0x64, true],
        [HELLO, 5, true],
        [HELLO, false],
        [WORLD, 10, false],              // end = 10 cuts the final 'd'
        [u8(), 3, true],                 // the empty needle ends anywhere
        // DOC-TENSION: dynajs.d.ts's example prints true for endsWith(44, 1) on "a,b,,c"; the d.ts
        // wording ("a match must END there", String semantics: compare at end - needle.length)
        // places the needle at [0, 1) where the byte is 'a'. We follow the d.ts.
    ], "Bytes.endsWith");
    assertCases((...a) => b.endsWith(...a), [
        [44, 1, false],
        [44, 2, true],                   // a comma ends at index 2
    ], "Bytes.endsWith");
}

/* ---------------- readBytes / writeBytes / fill ---------------- */
{
    // d.ts: readBytes "A FRESH Uint8Array copying buf[off .. off+len); RangeError out of bounds."
    const b = new Bytes("abcdef");
    assertCases((...a) => { const r = b.readBytes(...a); return [r instanceof Uint8Array, Bytes.isBytes(r), Array.from(r)]; }, [
        [1, 3, [true, false, [0x62, 0x63, 0x64]]],   // fresh copy, NOT a handle (d.ts: 'A copy by design')
        [0, 0, [true, false, []]],
    ], "Bytes.readBytes");
    assertCases((...a) => { const r = b.readBytes(...a); r[0] = 0; return b.readUint8(1); }, [
        [1, 3, 0x62],                    // mutating the copy leaves the handle unchanged
    ], "Bytes.readBytes");
    assertCasesThrow((...a) => b.readBytes(...a), [
        [4, 3, RangeError],
        [0, 7, RangeError],
    ], "Bytes.readBytes");

    // d.ts: writeBytes "Copies `src` at `off`; overlap-safe; returns the byte count; RangeError if
    // the write would pass the end."
    assertCases((...a) => { const d = new Bytes("abcd"); const k = d.writeBytes(...a); return [k, d.toString()]; }, [
        [1, u8(0x58, 0x59), 2, [2, "aXYd"]],   // TEST-FIX: writeBytes "returns the byte count" -> [2, "aXYd"]
        [0, new Bytes("ab"), 2, [2, "abcd"]],    // a Bytes handle is accepted as src ('a','b' already in place)
    ], "Bytes.writeBytes");
    assertCases(() => { const e = new Bytes("abcd"); const k = e.writeBytes(0, e.array.subarray(1, 4)); return [k, e.toString()]; }, [
        [undefined, [3, "bcdd"]],        // overlap-safe (memmove): src read before overwrite
    ], "Bytes.writeBytes(self)");
    assertCasesThrow((...a) => { const p = new Bytes("hello"); p.writeBytes(...a); }, [
        [3, u8(1, 2, 3), RangeError],
    ], "Bytes.writeBytes");
    assertCases((...a) => { const p = new Bytes("hello"); try { p.writeBytes(...a); } catch (e) { } return p.toString(); }, [
        [3, u8(1, 2, 3), "hello"],       // nothing is written partially on refusal
    ], "Bytes.writeBytes");

    // d.ts BEHAVIOR CHANGE: fill "returns the HANDLE (chainable)", free fill "still returns the view".
    assertCases((...a) => { const f = new Bytes("abcdef"); const r = f.fill(...a); return [r === f, Bytes.isBytes(r), f.toString()]; }, [
        [45, 1, 3, [true, true, "a--def"]],
        [46, [true, true, "......"]],    // full range: all 6 bytes
        [0x141, [true, true, "AAAAAA"]], // the value is written as its low 8 bits (0x141 -> 0x41), full range
    ], "Bytes.fill");
    assertCases((...a) => { const v = new Uint8Array(3); const r = fill(v, ...a); return [r === v, Array.from(v)]; }, [
        [7, [true, [7, 7, 7]]],
        [7, 1, 3, [true, [0, 7, 7]]],
    ], "fill(free)");
    assertCasesThrow((...a) => new Bytes("abc").fill(...a), [
        [1, 0, 4, RangeError],
        [1, 2, 1, RangeError],
    ], "Bytes.fill");
}

/* ---------------- fixed-width accessors: parametric width table ---------------- */
{
    // d.ts: every write "returns the offset after the value"; 64-bit reads are "always BigInt";
    // reads "throw RangeError when offset + width exceeds the buffer".
    const WIDTHS = [
        ["Uint8", (b, o, v) => b.writeUint8(o, v), (b, o) => b.readUint8(o), 255, 1, false],
        ["Int8", (b, o, v) => b.writeInt8(o, v), (b, o) => b.readInt8(o), -128, 1, false],
        ["Uint16LE", (b, o, v) => b.writeUint16LE(o, v), (b, o) => b.readUint16LE(o), 0xBEEF, 2, false],
        ["Uint16BE", (b, o, v) => b.writeUint16BE(o, v), (b, o) => b.readUint16BE(o), 0xBEEF, 2, false],
        ["Int16LE", (b, o, v) => b.writeInt16LE(o, v), (b, o) => b.readInt16LE(o), -12345, 2, false],
        ["Int16BE", (b, o, v) => b.writeInt16BE(o, v), (b, o) => b.readInt16BE(o), -12345, 2, false],
        ["Uint32LE", (b, o, v) => b.writeUint32LE(o, v), (b, o) => b.readUint32LE(o), 0xDEADBEEF, 4, false],
        ["Uint32BE", (b, o, v) => b.writeUint32BE(o, v), (b, o) => b.readUint32BE(o), 0xDEADBEEF, 4, false],
        ["Int32LE", (b, o, v) => b.writeInt32LE(o, v), (b, o) => b.readInt32LE(o), -123456789, 4, false],
        ["Int32BE", (b, o, v) => b.writeInt32BE(o, v), (b, o) => b.readInt32BE(o), -123456789, 4, false],
        ["BigUint64LE", (b, o, v) => b.writeBigUint64LE(o, v), (b, o) => b.readBigUint64LE(o), 0x0102030405060708n, 8, true],
        ["BigUint64BE", (b, o, v) => b.writeBigUint64BE(o, v), (b, o) => b.readBigUint64BE(o), 0x0102030405060708n, 8, true],
        ["BigInt64LE", (b, o, v) => b.writeBigInt64LE(o, v), (b, o) => b.readBigInt64LE(o), -2n, 8, true],
        ["BigInt64BE", (b, o, v) => b.writeBigInt64BE(o, v), (b, o) => b.readBigInt64BE(o), -2n, 8, true],
        ["FloatLE", (b, o, v) => b.writeFloatLE(o, v), (b, o) => b.readFloatLE(o), Math.fround(3.14), 4, false],
        ["FloatBE", (b, o, v) => b.writeFloatBE(o, v), (b, o) => b.readFloatBE(o), Math.fround(3.14), 4, false],
        ["DoubleLE", (b, o, v) => b.writeDoubleLE(o, v), (b, o) => b.readDoubleLE(o), -2.718281828, 8, false],
        ["DoubleBE", (b, o, v) => b.writeDoubleBE(o, v), (b, o) => b.readDoubleBE(o), -2.718281828, 8, false],
    ];
    for (const [nm, wr, rd, val, size, big] of WIDTHS) {
        const buf = new Bytes(new Uint8Array(8));
        assertEq(wr(buf, 0, val), size, nm + ": write returns the offset after the value (d.ts)");
        const got = rd(buf, 0);
        assertEq(typeof got, big ? "bigint" : "number", nm + ": 64-bit reads are always BigInt (d.ts)");
        const same = big ? got === val : Object.is(got, val) || (typeof val === "number" && Number.isNaN(got) && Number.isNaN(val));
        if (!same) throw new Error("case failed: " + nm + " round trip — got |" + got + "| expected |" + val + "|");
        n++;
        assertThrows(() => rd(buf, 8 - size + 1), nm + ": read past the end (offset + width)", RangeError);
    }
    // boundary refusals for a representative spread of writers (same d.ts sentence)
    assertCasesThrow((...a) => { const p = new Bytes(new Uint8Array(26)); p.writeDoubleBE(...a); }, [
        [19, 1, RangeError],
    ], "Bytes.writeDoubleBE");
    assertCasesThrow((...a) => { const p = new Bytes(new Uint8Array(4)); p.writeBigUint64LE(...a); }, [
        [0, 1n, RangeError],
    ], "Bytes.writeBigUint64LE");

    // byte-order rows: the bytes land MSB-first for BE, LSB-first for LE (fixed-width contract).
    // TEST-FIX: rows previously listed only the written bytes; the handle is an 8-byte buffer,
    // so Array.from(b.array) includes the zero tail.
    assertCases((wr, val) => { const b = new Bytes(new Uint8Array(8)); wr(b, 0, val); return Array.from(b.array); }, [
        [(b, o, v) => b.writeUint16BE(o, v), 0x0102, [1, 2, 0, 0, 0, 0, 0, 0]],
        [(b, o, v) => b.writeUint16LE(o, v), 0x0102, [2, 1, 0, 0, 0, 0, 0, 0]],
        [(b, o, v) => b.writeUint32BE(o, v), 0x01020304, [1, 2, 3, 4, 0, 0, 0, 0]],
        [(b, o, v) => b.writeUint32LE(o, v), 0x01020304, [4, 3, 2, 1, 0, 0, 0, 0]],
        [(b, o, v) => b.writeBigUint64BE(o, v), 0x0102030405060708n, [1, 2, 3, 4, 5, 6, 7, 8]],
        [(b, o, v) => b.writeBigUint64LE(o, v), 0x0102030405060708n, [8, 7, 6, 5, 4, 3, 2, 1]],
    ], "byte order");
    // LE write read back as BE is the byte swap
    assertCases((wr, val, rd) => { const b = new Bytes(new Uint8Array(8)); wr(b, 0, val); return rd(b, 0); }, [
        [(b, o, v) => b.writeUint16LE(o, v), 0xBEEF, (b, o) => b.readUint16BE(o), 0xEFBE],
        [(b, o, v) => b.writeUint32LE(o, v), 0xDEADBEEF, (b, o) => b.readUint32BE(o), 0xEFBEADDE],
        [(b, o, v) => b.writeBigUint64LE(o, v), 0x0102030405060708n, (b, o) => b.readBigUint64BE(o), 0x0807060504030201n],
    ], "swap read");
    // signed/unsigned share bits
    assertCases(() => { const b = new Bytes(new Uint8Array(8)); b.writeBigInt64LE(0, -2n); return [b.readBigInt64LE(0), b.readBigUint64LE(0)]; }, [
        [undefined, [-2n, 18446744073709551614n]],   // i64 -2 as u64 is 2^64 - 2
    ], "64-bit bit-cast");

    // writes chain by next-offset: 2+4+8+4+8 = 26 (mixed endianness round trip)
    const pkt = new Bytes(new Uint8Array(26));
    let off = 0;
    for (const [wr, val, sz] of [
        [(b, o, v) => b.writeUint16BE(o, v), 0xCAFE, 2],
        [(b, o, v) => b.writeUint32LE(o, v), 123456, 4],
        [(b, o, v) => b.writeBigUint64BE(o, v), 0x0102030405060708n, 8],
        [(b, o, v) => b.writeFloatLE(o, v), Math.fround(3.14), 4],
        [(b, o, v) => b.writeDoubleBE(o, v), -2.718281828, 8],
    ]) {
        off = wr(pkt, off, val);
        assertEq(off, (off - sz) + sz, "write chain advanced by its width");
    }
    assertEq(off, 26, "writes chain by next offset (2+4+8+4+8) (d.ts)");
    assertCases((rd, o) => rd(pkt, o), [
        [(b, o) => b.readUint16BE(o), 0, 0xCAFE],
        [(b, o) => b.readUint32LE(o), 2, 123456],
        [(b, o) => b.readBigUint64BE(o), 6, 0x0102030405060708n],
        [(b, o) => b.readFloatLE(o), 14, Math.fround(3.14)],
        [(b, o) => b.readDoubleBE(o), 18, -2.718281828],
    ], "packet readback");
    // float specials through the handle door
    assertCases((wr, val, rd) => { const b = new Bytes(new Uint8Array(8)); wr(b, 0, val); return rd(b, 0); }, [
        [(b, o, v) => b.writeFloatLE(o, v), -0, (b, o) => b.readFloatLE(o), (v) => Object.is(v, -0)],
        [(b, o, v) => b.writeFloatLE(o, v), Infinity, (b, o) => b.readFloatLE(o), (v) => v === Infinity],
        [(b, o, v) => b.writeFloatLE(o, v), NaN, (b, o) => b.readFloatLE(o), (v) => Number.isNaN(v)],
        [(b, o, v) => b.writeDoubleLE(o, v), Math.PI, (b, o) => b.readDoubleLE(o), (v) => v === Math.PI],
        [(b, o, v) => b.writeDoubleBE(o, v), -0, (b, o) => b.readDoubleBE(o), (v) => Object.is(v, -0)],
        [(b, o, v) => b.writeDoubleBE(o, v), NaN, (b, o) => b.readDoubleBE(o), (v) => Number.isNaN(v)],
    ], "float specials");

    // d.ts map: method <-> the same-named free function must behave identically
    assertCases((v) => { const a = new Bytes(new Uint8Array(4)); const raw = new Uint8Array(4); const m = a.writeUint32LE(0, v); const f = writeUint32LE(raw, 0, v); return [m === f, Array.from(a.array), Array.from(raw), a.readUint32LE(0), readUint32LE(raw, 0)]; }, [
        [0xDEADBEEF, [true, [0xEF, 0xBE, 0xAD, 0xDE], [0xEF, 0xBE, 0xAD, 0xDE], 0xDEADBEEF, 0xDEADBEEF]], // TEST-FIX: the case returns m===f (parity), not the offset
    ], "method/free writeUint32LE parity");
    assertCases((...a) => writeUint8(...a), [
        [new Uint8Array(2), 0, 255, 1],
    ], "writeUint8(free)");
}

/* ---------------- Text ---------------- */
{
    // d.ts: isWide "True when any code unit is above U+00FF."
    assertCases((s) => { const t = new Text(s); return [t.isWide, t.value, t.toJSON(), t.toString()]; }, [
        ["héllo", [false, "héllo", "héllo", "héllo"]],   // U+00E9 is NOT above U+00FF
        ["ǆ", [true, "ǆ", "ǆ", "ǆ"]],                    // U+01C6
        ["日本", [true, "日本", "日本", "日本"]],
    ], "new Text");
    // d.ts: isValidUtf8 is a GETTER; "calling it now throws 'not a function'".
    assertCases((s) => { const t = new Text(s); return [typeof t.isValidUtf8, t.isValidUtf8]; }, [
        ["hello", ["boolean", true]],
        ["\uD800", ["boolean", false]],     // a lone surrogate is the one ill-formed case
    ], "Text.isValidUtf8(getter)");
    assertCasesThrow((s) => new Text(s).isValidUtf8(), [
        ["abc", TypeError],
    ], "Text.isValidUtf8() method spelling");
    // d.ts: isValidUtf16 IS a method: "True when the string has no lone surrogate."
    assertCases((s) => new Text(s).isValidUtf16(), [
        ["\uD800\uDC00", true],
        ["\uD800", false],
        ["plain", true],
    ], "Text.isValidUtf16()");
    // d.ts: countUtf8 "UTF-8 code points"; countUtf16 "surrogate pairs counted once".
    assertCases((s) => [new Text(s).countUtf8(), new Text(s).countUtf16()], [
        ["héllo", [5, 5]],
        ["😀", [1, 1]],
        ["ab😀", [3, 3]],
    ], "Text counts");
    // d.ts conversions on Text
    assertCases((s) => Array.from(new Text(s).toUtf8()), [
        ["é", [0xC3, 0xA9]],
        ["a", [97]],
    ], "Text.toUtf8");
    assertCases((s) => Array.from(new Text(s).latin1ToUtf8()), [
        // TEST-FIX: dynajs.d.ts 2677 on the Text door: "The string's OWN BYTES, each taken as a
        // Latin-1 code point re-encoded to UTF-8" — i.e. the string's UTF-8 bytes get expanded,
        // not the code units. "é" -> utf8 [0xC3,0xA9] -> each byte widened: [0xC3,0x83,0xC2,0xA9].
        ["é", [0xC3, 0x83, 0xC2, 0xA9]],
    ], "Text.latin1ToUtf8");
    assertCases((s) => Array.from(new Text(s).utf8ToUtf16()), [
        ["a", [97, 0]],
        ["héllo", [0x68, 0, 0xE9, 0, 0x6C, 0, 0x6C, 0, 0x6F, 0]],
    ], "Text.utf8ToUtf16");
    // dynajs.d.ts on this door: utf16ToUtf8 "On a Text this is the string's own UTF-8".
    assertCases((s) => { const t = new Text(s); return eqArr(Array.from(t.utf16ToUtf8()), Array.from(t.toUtf8())); }, [
        ["héllo", true],
        ["😀", true],
    ], "Text.utf16ToUtf8");
    // d.ts: toBytes "The string as a Bytes handle."
    assertCases((s) => { const b = new Text(s).toBytes(); return [Bytes.isBytes(b), b.toString()]; }, [
        ["abc", [true, "abc"]],
    ], "Text.toBytes");
}

/* ---------------- free UTF-8 / UTF-16 / latin-1 conversions ---------------- */
{
    // d.ts: utf16ToUtf8 "Strict UTF-16LE to UTF-8; throws on odd length or an ill-formed surrogate."
    assertCases((...a) => Array.from(utf16ToUtf8(...a)), [
        [u8(97, 0), [97]],
        [u8(0x00, 0xD8, 0x00, 0xDC), [0xF0, 0x90, 0x80, 0x80]],  // U+10000
    ], "utf16ToUtf8");
    assertCasesThrow((...a) => utf16ToUtf8(...a), [
        [u8(97), RangeError],            // odd byte length
        [u8(0x00, 0xD8), RangeError],    // ill-formed (lone high) surrogate
    ], "utf16ToUtf8");
    // d.ts: isValidUtf16 "even length, paired surrogates".
    assertCases((...a) => isValidUtf16(...a), [
        [u8(0x41, 0x00), true],
        [u8(0x00, 0xD8), false],
        [u8(0x00, 0xD8, 0x00, 0xDC), true],
        [u8(0x41), false],               // odd length
    ], "isValidUtf16");
    // d.ts: latin1ToUtf8 expands; utf8ToLatin1 "Throws RangeError on invalid UTF-8 or any code
    // point above 0xFF."
    assertCases((...a) => Array.from(latin1ToUtf8(...a)), [
        [u8(0xE9), [0xC3, 0xA9]],
        [u8(0x41), [0x41]],
    ], "latin1ToUtf8");
    assertCases((...a) => Array.from(utf8ToLatin1(...a)), [
        [fromUtf8("é"), [0xE9]],
    ], "utf8ToLatin1");
    assertCasesThrow((...a) => utf8ToLatin1(...a), [
        [fromUtf8("Ā"), RangeError],     // code point above 0xFF
        [u8(0xC3, 0x28), RangeError],    // invalid UTF-8
    ], "utf8ToLatin1");
    // d.ts: utf8ToUtf16 "Strict UTF-8 to UTF-16LE; throws on malformed input."
    assertCasesThrow((...a) => utf8ToUtf16(...a), [
        [u8(0xFF), RangeError],
    ], "utf8ToUtf16");
    // d.ts: isValidUtf8 "over a string or byte view".
    assertCases((...a) => isValidUtf8(...a), [
        ["hello", true],
        ["\uD800", false],
        [u8(0xF0, 0x9F, 0x98, 0x80), true],
        [u8(0x80), false],
    ], "isValidUtf8");
    // d.ts: countUtf8 / countUtf16
    assertCases((...a) => countUtf8(...a), [
        ["😀", 1],
        [fromUtf8("😀"), 1],
        [u8(0x61, 0xC3, 0xA9), 2],
    ], "countUtf8");
    assertCases((...a) => countUtf16(...a), [
        [u8(0x41, 0x00), 1],
    ], "countUtf16");
    // d.ts: toUtf8 lossy decode; fromUtf8 the inverse.
    assertCases((...a) => toUtf8(...a), [
        [fromUtf8("a😀b"), "a😀b"],
        [u8(0x61, 0xC3, 0x28), "a\uFFFD("],     // truncated 2-byte -> U+FFFD
        [u8(), ""],
    ], "toUtf8");
    assertCases((s) => eqArr(Array.from(fromUtf8(s)), Array.from(s2b(s))), [
        ["", true],
        ["hello", true],
        ["😀", true],
    ], "fromUtf8");
}

/* ---------------- legacy charsets: decode / encode / encodingExists / encodings ---------------- */
{
    // d.ts: "Decodes a byte view from a legacy single-byte charset; unknown label throws RangeError."
    assertCases((...a) => decode(...a), [
        [u8(0xE9), "latin1", "é"],
        [u8(0xC3, 0xA9), "utf-8", "é"],
        [u8(0x80), "us-ascii", "\uFFFD"],   // us-ascii maps every high byte to U+FFFD
    ], "decode");
    assertCasesThrow((...a) => decode(...a), [
        [u8(0xE9), "klingon", RangeError],
    ], "decode");
    // d.ts: encode "unexpressible code points become `?`"; "Both arguments must be strings".
    assertCases((...a) => Array.from(encode(...a)), [
        ["é", "latin1", [233]],
        ["Ā", "latin1", [0x3F]],
        ["é", "us-ascii", [0x3F]],
    ], "encode");
    assertCasesThrow((...a) => encode(...a), [
        ["é", 5, TypeError],
    ], "encode");
    // d.ts: "True for every built label plus utf-8/utf8; ASCII case-insensitive."
    assertCases((...a) => encodingExists(...a), [
        ["latin1", true],
        ["UTF-8", true],
        ["utf8", true],
        ["  latin1  ", true],   // leading/trailing space ignored (Encoding Standard rule)
        ["gbk", false],         // the CJK multi-byte families are NOT built (documented boundary)
    ], "encodingExists");
    // d.ts: encodings() "beginning with utf-8"; every listed label satisfies encodingExists.
    assertCases(() => { const list = encodings(); let ok = true; for (const l of list) if (!encodingExists(l)) ok = false; return [Array.isArray(list), list[0], ok]; }, [
        [undefined, [true, "utf-8", true]],
    ], "encodings()");
}

/* ---------------- bytesOf: the only non-copying function ---------------- */
{
    assertCases((make) => make(), [
        // d.ts: "A Uint8Array aliasing exactly the bytes `view` spans."
        [() => { const i32 = new Int32Array([0x11223344]); const al = bytesOf(i32); const shape = [al instanceof Uint8Array, al.length, al[0], al[3]]; al[0] = 0xFF; return [shape, i32[0], Array.from(new Uint8Array(0))]; },
            [[true, 4, 0x44, 0x11], 0x112233FF, []]],
        [() => bytesOf(new DataView((new Uint8Array([1, 2])).buffer)).length, 2],
        [() => bytesOf(new ArrayBuffer(3)).length, 3],
    ], "bytesOf");
    assertCasesThrow((v) => bytesOf(v), [
        [[1, 2, 3], TypeError],
        ["abc", TypeError],
    ], "bytesOf");
    // free copy/fill refusals (bounds)
    assertCasesThrow((...a) => copy(...a), [
        [new Uint8Array(2), u8(1, 2, 3), 0, 0, 3, RangeError],
        [new Uint8Array(2), u8(1, 2, 3), 5, RangeError],
    ], "copy");
    assertCasesThrow((...a) => fill(...a), [
        [new Uint8Array(4), 1, 3, 2, RangeError],
        [new Uint8Array(4), 1, 0, 5, RangeError],
    ], "fill");
    assertCases((...a) => { const d = new Uint8Array(5); const k = copy(d, ...a); return [k, Array.from(d)]; }, [
        [u8(9, 8, 7), [3, [9, 8, 7, 0, 0]]],
        [u8(1, 2, 3, 4), 2, [3, [0, 0, 1, 2, 3]]],   // dstOff clips len to min avail (count = clipped 3)
    ], "copy");
}

print("bb_bytes: all tests passed (" + n + " assertions)");
