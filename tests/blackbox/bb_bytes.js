// Black-box contract test for dyna:bytes, generated from dynajs.d.ts lines 1-368. Engine sources not consulted; every expectation cites the contract.
import * as BytesNS from "dyna:bytes";
import {
    Bytes, Text, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy,
    isValidUtf8, isValidUtf16, countUtf8, countUtf16, latin1ToUtf8,
    utf8ToLatin1, utf8ToUtf16, utf16ToUtf8, decode, encode, encodingExists, encodings,
} from "dyna:bytes";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function eqArr2(a, b) { return eqArr(Array.from(a), Array.from(b)); }
function u8(...bytes) { return new Uint8Array(bytes); }
function s2b(s) { return new TextEncoder().encode(s); }

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
    assertCases((k) => { const z = Bytes.alloc(k); let zero = true; for (let i = 0; i < z.length; i++) if (z.array[i] !== 0) zero = false; return [z.length, zero]; }, [
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
        [() => { const src = u8(0x41, 0x42); const b = new Bytes(src); src[0] = 0xFF; return b.array[0]; }, 0x41], // copied, never aliased
        [() => new Bytes("héllo").length, 6],            // string -> UTF-8 (é is 2 bytes)
        [() => { const d = new Bytes(new DataView((new Uint8Array([5, 6, 7])).buffer)); return [d.length, d.array[2]]; }, [3, 7]],
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
        // d.ts: the flags are "recomputed once `array` has been read, so writes through it are seen".
        [() => { const m = new Bytes("abc"); m.array.fill(0xFF); return [m.isAscii, m.isValidUtf8]; }, [false, false]],
        [() => { const m = new Bytes("abc"); const a = m.array; a[0] = 0xC3; a[1] = 0xA9; return [m.isAscii, m.isValidUtf8]; }, [false, true]],
    ], "flags");
}

/* ---------------- slice: a VIEW sharing the owner's ArrayBuffer ---------------- */
{
    assertCases((make) => make(), [
        // d.ts: "A new Bytes handle that is a view sharing the owner's ArrayBuffer."
        [() => { const o = new Bytes("hello world"); const s = o.slice(6, 11); return [Bytes.isBytes(s), s.length, s.toString()]; }, [true, 5, "world"]],
        [() => { const o = new Bytes("hello world"); const s = o.slice(6, 11); s.array.fill(0x2A); return o.indexOf(42); }, 6], // writes alias the owner
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

/* ---------------- array: the live, bounded view ---------------- */
{
    // d.ts: array "The live backing Uint8Array, bounded to this handle's bytes (a slice's array
    // starts and ends with the slice)"; Bytes: "fixed-width access is a DataView over `array`".
    assertCases((make) => make(), [
        [() => { const o = new Bytes("abcdefgh"); const a = o.slice(2, 6).array; return [a.byteLength, a.byteOffset - o.array.byteOffset, a.buffer === o.array.buffer]; }, [4, 2, true]],
        [() => { const o = Bytes.alloc(8); const a = o.slice(2, 6).array; const dv = new DataView(a.buffer, a.byteOffset, a.byteLength); dv.setUint16(0, 0xAABB); dv.setUint16(2, 0xCCDD, true); return Array.from(o.array); }, [0, 0, 0xAA, 0xBB, 0xDD, 0xCC, 0, 0]],
        [() => { const b = new Bytes("abcdef"); b.array.set([0x58, 0x59], 1); return [b.toString(), b.indexOf(0x59)]; }, ["aXYdef", 2]],
        [() => { const b = new Bytes("abcdef"); const a = b.array; a.copyWithin(0, 1, 4); return b.toString(); }, "bcddef"],
    ], "Bytes.array");
    assertCasesThrow((make) => make(), [
        [() => { const o = Bytes.alloc(8); const a = o.slice(2, 6).array; new DataView(a.buffer, a.byteOffset, a.byteLength).getUint8(4); }, RangeError],
    ], "Bytes.array bounds");
    // the module surface is the d.ts list: no accessor outside it is reachable.
    assertCases((name) => [name in Bytes.prototype, name in BytesNS], [
        ["fill", [false, false]],
        ["readUint32LE", [false, false]],
        ["writeBigInt64BE", [false, false]],
        ["readBytes", [false, false]],
        ["toUtf8", [false, false]],
        ["bytesOf", [false, false]],
    ], "not in the d.ts");
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
    // d.ts conversions on Text; toBytes "The string as a Bytes handle" (its UTF-8).
    assertCases((s) => Array.from(new Text(s).toBytes().array), [
        ["é", [0xC3, 0xA9]],
        ["a", [97]],
    ], "Text.toBytes().array");
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
    assertCases((s) => { const t = new Text(s); return eqArr(Array.from(t.utf16ToUtf8()), Array.from(t.toBytes().array)); }, [
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
        [s2b("é"), [0xE9]],
    ], "utf8ToLatin1");
    assertCasesThrow((...a) => utf8ToLatin1(...a), [
        [s2b("Ā"), RangeError],     // code point above 0xFF
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
        [s2b("😀"), 1],
        [u8(0x61, 0xC3, 0xA9), 2],
    ], "countUtf8");
    assertCases((...a) => countUtf16(...a), [
        [u8(0x41, 0x00), 1],
    ], "countUtf16");
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

/* ---------------- copy: bounds ---------------- */
{
    // d.ts: copy "Overlap-safe byte copy returning the number of bytes copied".
    assertCasesThrow((...a) => copy(...a), [
        [new Uint8Array(2), u8(1, 2, 3), 0, 0, 3, RangeError],
        [new Uint8Array(2), u8(1, 2, 3), 5, RangeError],
    ], "copy");
    assertCases((...a) => { const d = new Uint8Array(5); const k = copy(d, ...a); return [k, Array.from(d)]; }, [
        [u8(9, 8, 7), [3, [9, 8, 7, 0, 0]]],
        [u8(1, 2, 3, 4), 2, [3, [0, 0, 1, 2, 3]]],   // dstOff clips len to min avail (count = clipped 3)
    ], "copy");
}

print("bb_bytes: all tests passed (" + n + " assertions)");
