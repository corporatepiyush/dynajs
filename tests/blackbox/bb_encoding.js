// Black-box contract test for dyna:encoding, generated from dynajs.d.ts lines 1302-1471. Engine sources not consulted; every expectation cites the contract.
import {
    HexEncode, HexDecode, hexEncodeInto, hexDecodeInto,
    Base64Encode, Base64Decode, base64EncodeInto, base64DecodeInto,
    Base64URLEncode, Base64URLDecode, base64UrlEncodeInto, base64UrlDecodeInto,
    Base32Encode, Base32Decode, base32EncodeInto, base32DecodeInto,
    Base32HexEncode, Base32HexDecode, base32HexEncodeInto, base32HexDecodeInto,
    Base85Encode, Base85Decode, base85EncodeInto, base85DecodeInto,
    Base58Encode, Base58Decode, base58EncodeInto, base58DecodeInto,
    Base58CheckEncode, Base58CheckDecode, base58CheckEncodeInto, base58CheckDecodeInto,
    BaseXEncode, BaseXDecode,
    PutUvarint, PutVarint, Uvarint, Varint, appendUvarint, uvarintAt, varintAt,
    DetectEncoding, detectEncoding,
    JSON5Parse, JSON5Stringify, StableStringify, JSONPath,
    QREncode, QRToString,
} from "dyna:encoding";
import { fromUtf8 } from "dyna:bytes";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function u8(...bytes) { return new Uint8Array(bytes); }
function bytes(len) { const a = new Uint8Array(len); for (let i = 0; i < len; i++) a[i] = (i * 37 + 11) & 0xFF; return a; } // deterministic pattern

/* Table runners: rows are [args..., expected] (expected may be a predicate) or [args..., ErrorClass]. */
function labelOf(name, args) {
    return name + "(" + args.map(a => typeof a === "string" ? JSON.stringify(a.length > 24 ? a.slice(0, 24) + "…" : a) : (a instanceof Uint8Array ? "u8[" + a.length + "]" : String(a))).join(", ") + ")";
}
function J(v) {
    if (typeof v === "bigint") return v + "n";
    if (Array.isArray(v)) return "[" + v.map(J).join(",") + "]";  // bigints inside arrays
    return JSON.stringify(v);
}
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

/* ---------------- hex ---------------- */
{
    // d.ts: HexEncode "Lowercase hex string"; data is BytesInput (a string is taken as UTF-8).
    assertCases((...a) => HexEncode(...a), [
        [u8(), ""],
        [u8(0xDE, 0xAD), "dead"],
        [u8(0xDE, 0xAD, 0xBE, 0xEF), "deadbeef"],
        ["", ""],
        ["é", "c3a9"],                        // the string's UTF-8 bytes
    ], "HexEncode");
    // d.ts: HexDecode "throws SyntaxError on odd length or an invalid digit".
    assertCases((...a) => Array.from(HexDecode(...a)), [
        ["", []],
        ["dead", [0xDE, 0xAD]],
        ["DEADBEEF", [0xDE, 0xAD, 0xBE, 0xEF]],   // uppercase accepted
    ], "HexDecode");
    assertCasesThrow((...a) => HexDecode(...a), [
        ["abc", SyntaxError],
        ["zz", SyntaxError],
    ], "HexDecode");
    // parametric round trip over lengths 0..8 (hex encode/decode inverse)
    // (element compare, not assertEq: Object.is on two fresh arrays never holds,
    // which mis-failed the len=0 [] vs [] row)
    for (let len = 0; len <= 8; len++) {
        const p = bytes(len);
        assert(eqArr(Array.from(HexDecode(HexEncode(p))), Array.from(p)), "hex round trip len=" + len);
    }
    // d.ts: hexEncodeInto "returns the 2*n ASCII bytes written at offset 0. `out` needs 2*n bytes
    // or RangeError (and nothing is written on that path). Input/output overlap refuses."
    assertCases((data, cap) => { const out = new Uint8Array(cap); const k = hexEncodeInto(data, out); return [k, Array.from(out)]; }, [
        ["é", 8, [4, [99, 51, 97, 57, 0, 0, 0, 0]]],          // ASCII of "c3a9"
        [u8(0xDE, 0xAD), 4, [4, [100, 101, 97, 100]]],        // ASCII of "dead"
    ], "hexEncodeInto");
    assertCasesThrow((data, out) => hexEncodeInto(data, out), [
        [u8(1, 2), new Uint8Array(3), RangeError],
    ], "hexEncodeInto");
    assertCases((data, cap) => { const out = new Uint8Array(cap); try { hexEncodeInto(data, out); } catch (e) { return Array.from(out); } return "no-throw"; }, [
        [u8(1, 2), 3, [0, 0, 0]],              // nothing written on the refusal path
    ], "hexEncodeInto");
    // d.ts: hexDecodeInto "returns the byte count written. `out` needs at least text.length/2
    // bytes or RangeError" (checked BEFORE decoding — API.md).
    assertCases((text, cap) => { const out = new Uint8Array(cap).fill(0xAA); const k = hexDecodeInto(text, out); return [k, Array.from(out)]; }, [
        ["deadbeef", 8, [4, [222, 173, 190, 239, 170, 170, 170, 170]]],   // tail untouched
        ["", 2, [0, [170, 170]]],
    ], "hexDecodeInto");
    assertCasesThrow((text, out) => hexDecodeInto(text, out), [
        ["abcdef", new Uint8Array(2), RangeError],
        ["zz", new Uint8Array(8), SyntaxError],
    ], "hexDecodeInto");
    // overlap refuses with TypeError (d.ts: "Input/output overlap refuses.")
    {
        const shared = new ArrayBuffer(8);
        const outW = new Uint8Array(shared, 0, 8);
        const inW = new Uint8Array(shared, 0, 2);
        assertThrows(() => hexEncodeInto(inW, outW), "hexEncodeInto: input/output overlap refuses (d.ts)", TypeError);
    }
}

/* ---------------- base64 / base64url ---------------- */
{
    // d.ts: "RFC 4648 base64 (`+/`, padded)" — byte-level codec (the atob/btoa WARNING).
    assertCases((...a) => Base64Encode(...a), [
        [u8(), ""],
        [u8(1, 2, 3), "AQID"],
        [u8(0), "AA=="],
        [u8(0xFF), "/w=="],
        [u8(0xFB, 0xFF), "+/8="],        // + and / are the 62/63 chars
        ["hi", Base64Encode(fromUtf8("hi"))],   // a string input is its UTF-8 bytes
    ], "Base64Encode");
    assertCases((...a) => Array.from(Base64Decode(...a)), [
        ["", []],
        ["AQID", [1, 2, 3]],
        ["AA==", [0]],
    ], "Base64Decode");
    assertCasesThrow((...a) => Base64Decode(...a), [
        ["a", SyntaxError],
        ["****", SyntaxError],
        ["aGk =", SyntaxError],          // whitespace is rejected (API.md on the Into door)
        // Regression (decode-cap sizing): unpadded input is REJECTED (RFC 4648 canonical
        // form; d.ts strict door), which is what keeps the 3*(n/4) output cap exact —
        // "QQQ" would decode to 2 bytes against a 0-byte cap if it were accepted.
        ["QQQ", SyntaxError],
        ["A", SyntaxError],
    ], "Base64Decode");
    // parametric round trip over lengths 0..8 (element compare; see hex note)
    for (let len = 0; len <= 8; len++) {
        const p = bytes(len);
        assert(eqArr(Array.from(Base64Decode(Base64Encode(p))), Array.from(p)), "base64 round trip len=" + len);
    }
    // d.ts: base64EncodeInto "`out` needs 4*ceil(n/3) bytes or RangeError"
    assertCases((data, cap) => { const out = new Uint8Array(cap); const k = base64EncodeInto(data, out); return [k, Array.from(out)]; }, [
        [u8(1, 2, 3), 4, [4, [65, 81, 73, 68]]],      // "AQID"
        [u8(0), 4, [4, [65, 65, 61, 61]]],            // "AA=="
    ], "base64EncodeInto");
    assertCasesThrow((data, out) => base64EncodeInto(data, out), [
        [u8(1, 2, 3), new Uint8Array(3), RangeError],
    ], "base64EncodeInto");
    assertCases((data, cap) => { const out = new Uint8Array(cap); try { base64EncodeInto(data, out); } catch (e) { return Array.from(out); } return "no-throw"; }, [
        [u8(1, 2, 3), 3, [0, 0, 0]],     // capacity is checked BEFORE writing (API.md)
    ], "base64EncodeInto");
    // d.ts: base64DecodeInto "`out` needs at least 3*(text.length/4) bytes or RangeError"
    assertCases((text, cap) => { const out = new Uint8Array(cap); const k = base64DecodeInto(text, out); return [k, Array.from(out)]; }, [
        ["AQID", 3, [3, [1, 2, 3]]],
    ], "base64DecodeInto");
    assertCasesThrow((text, out) => base64DecodeInto(text, out), [
        ["AQID", new Uint8Array(2), RangeError],
        ["AQ ID", new Uint8Array(3), SyntaxError],
    ], "base64DecodeInto");

    // d.ts: "RFC 4648 section 5 base64url (no padding)"; API-pinned vector [0xFB,0xFF] -> "-_8".
    assertCases((...a) => Base64URLEncode(...a), [
        [u8(), ""],
        [u8(0), "AA"],
        [u8(0xFB, 0xFF), "-_8"],
        [u8(0xFF), "_w"],
    ], "Base64URLEncode");
    assertCases((...a) => Array.from(Base64URLDecode(...a)), [
        ["-_8", [0xFB, 0xFF]],
        ["AA", [0]],
    ], "Base64URLDecode");
    assertCasesThrow((...a) => Base64URLDecode(...a), [
        ["a", SyntaxError],              // length 4k+1 encodes no byte count (d.ts)
        ["+/8=", SyntaxError],           // a stray +/-/ is rejected (d.ts)
    ], "Base64URLDecode");
    // unpadded output length: ceil(8n/6) characters
    assertCases((nBytes) => Base64URLEncode(bytes(nBytes)).length, [
        [0, 0], [1, 2], [2, 3], [3, 4], [4, 6],
    ], "Base64URLEncode length");
    // d.ts: base64UrlEncodeInto "up to 3 pad bytes past the returned count are scratch ... never
    // past the bound" — only the returned count's bytes are asserted.
    assertCases((data, cap) => { const out = new Uint8Array(cap); const k = base64UrlEncodeInto(data, out); return [k, Array.from(out.slice(0, k))]; }, [
        [u8(0xFB, 0xFF), 4, [3, [45, 95, 56]]],       // "-_8"
    ], "base64UrlEncodeInto");
    // d.ts: base64UrlDecodeInto "`out` needs at least 3*((text.length+3)/4) bytes or RangeError"
    assertCases((text, cap) => { const out = new Uint8Array(cap); const k = base64UrlDecodeInto(text, out); return [k, Array.from(out.slice(0, k))]; }, [
        ["-_8", 3, [2, [251, 255]]],
    ], "base64UrlDecodeInto");
    assertCasesThrow((text, out) => base64UrlDecodeInto(text, out), [
        ["-_8", new Uint8Array(2), RangeError],
    ], "base64UrlDecodeInto");
}

/* ---------------- base32 / base32hex (RFC 4648 test vectors) ---------------- */
{
    const V = [
        ["", "", ""],
        ["f", "MY======", "CO======"],
        ["fo", "MZXQ====", "CPNG===="],
        ["foo", "MZXW6===", "CPNMU==="],
        ["foob", "MZXW6YQ=", "CPNMUOG="],
        // 5 bytes fill a full base32 group: NO padding (fooba != foobar;
        // generation had duplicated foobar's vectors onto this row).
        ["fooba", "MZXW6YTB", "CPNMUOJ1"],
        ["foobar", "MZXW6YTBOI======", "CPNMUOJ1E8======"],
    ];
    // RFC 4648 §10 test vectors (T: base32, base32hex)
    assertCases((s) => Base32Encode(fromUtf8(s)), V.map(r => [r[0], r[1]]), "Base32Encode");
    assertCases((s) => Base32HexEncode(fromUtf8(s)), V.map(r => [r[0], r[2]]), "Base32HexEncode");
    assertCases((s) => Array.from(Base32Decode(s)), V.filter(r => r[0] !== "").map(r => [r[1], Array.from(fromUtf8(r[0]))]), "Base32Decode");
    assertCases((s) => Array.from(Base32HexDecode(s)), V.filter(r => r[0] !== "").map(r => [r[2], Array.from(fromUtf8(r[0]))]), "Base32HexDecode");
    // d.ts: "the codec requires a padded, length % 8 == 0 string"
    assertCasesThrow((...a) => Base32Decode(...a), [
        ["MZXW6", SyntaxError],
        ["MZXW6YTB1===", SyntaxError],   // '1' is outside the A-Z2-7 alphabet
        // Regression (decode-cap sizing): a partial group is rejected (length % 8 == 0
        // contract), which is what keeps the (len/8)*5 output cap exact — a 12-char
        // input would decode to 7 bytes against a 5-byte cap if it were accepted.
        ["AAAAAAAAAAAA", SyntaxError],
    ], "Base32Decode");
    // d.ts: base32EncodeInto "((n+4)/5)*8"; base32DecodeInto "(text.length/8)*5"
    assertCases((s, cap) => { const out = new Uint8Array(cap); const k = base32EncodeInto(fromUtf8(s), out); return [k, Array.from(out)]; }, [
        ["f", 8, [8, [77, 89, 61, 61, 61, 61, 61, 61]]],      // "MY======"
        ["foobar", 16, [16, Array.from(fromUtf8("MZXW6YTBOI======"))]],
    ], "base32EncodeInto");
    assertCases((s, cap) => { const out = new Uint8Array(cap); const k = base32DecodeInto(s, out); return [k, Array.from(out.slice(0, k))]; }, [
        ["MY======", 5, [1, [102]]],
        ["MZXW6YTBOI======", 10, [6, Array.from(fromUtf8("foobar"))]],
    ], "base32DecodeInto");
    assertCasesThrow((s, out) => base32DecodeInto(s, out), [
        ["MY======", new Uint8Array(4), RangeError],
        ["MZXW6", new Uint8Array(5), SyntaxError],
    ], "base32DecodeInto");
}

/* ---------------- base85 (ascii85, Adobe-less) ---------------- */
{
    // d.ts: "Adobe-less ascii85 with the `z` shorthand"; partial groups are count+1 chars.
    assertCases((...a) => Base85Encode(...a), [
        [u8(0, 0, 0, 0), "z"],                       // the z shorthand for an all-zero group
        [u8(0x68, 0x65, 0x6C, 0x6C), "BOu!r"],       // 0x68656C6C, hand-derived
        [fromUtf8("hello"), "BOu!rDZ"],              // + a 1-byte partial as 2 chars
    ], "Base85Encode");
    assertCases((s) => Base85Encode(fromUtf8(s)).length, [
        ["hello", 7],                    // 5 chars for the full group + 2 for the partial (d.ts)
        ["hell", 5],
    ], "Base85Encode length");
    assertCases((...a) => Array.from(Base85Decode(...a)), [
        ["z", [0, 0, 0, 0]],
        ["BOu!r", [0x68, 0x65, 0x6C, 0x6C]],
        ["BOu !r", [0x68, 0x65, 0x6C, 0x6C]],        // whitespace is skipped (d.ts)
        ["s8W-!", [255, 255, 255, 255]],             // 2^32-1, the largest accepted (d.ts)
        ["BOu!rDZ", Array.from(fromUtf8("hello"))],
    ], "Base85Decode");
    assertCasesThrow((...a) => Base85Decode(...a), [
        // z is recognized only AT a group boundary (doc; Python a85decode
        // parity) -- consecutive z's are each a zero group and DO decode;
        // a z INSIDE a group is the stray that throws.
        ["BOz!r", SyntaxError],
        ['s8W-"', SyntaxError],          // a group naming 2^32 (d.ts names s8W-")
        ["uuuuu", SyntaxError],          // the doc's other overflow example
    ], "Base85Decode");
    // d.ts: base85EncodeInto "((n+3)/4)*5"; base85DecodeInto "at least 4*text.length (whitespace-safe bound)"
    assertCases((data, cap) => { const out = new Uint8Array(cap); const k = base85EncodeInto(data, out); return [k, Array.from(out.slice(0, k))]; }, [
        [u8(0x68, 0x65, 0x6C, 0x6C), 10, [5, [66, 79, 117, 33, 114]]],
    ], "base85EncodeInto");
    assertCases((text, cap) => { const out = new Uint8Array(cap); const k = base85DecodeInto(text, out); return [k, Array.from(out.slice(0, k))]; }, [
        ["BOu!r", 20, [4, [104, 101, 108, 108]]],
        // bound is 4*text.length (whitespace-safe): "BO u ! r".length = 8 -> 32
        ["BO u ! r", 32, [4, [104, 101, 108, 108]]],   // whitespace survives the bound
    ], "base85DecodeInto");
    assertCasesThrow((text, out) => base85DecodeInto(text, out), [
        ["BOu!r", new Uint8Array(15), RangeError],
    ], "base85DecodeInto");
}

/* ---------------- base58 / base58check ---------------- */
{
    // d.ts: "Bitcoin base58; leading zero bytes become leading `1`s."
    assertCases((...a) => Base58Encode(...a), [
        [u8(), ""],
        [u8(0), "1"],
        [u8(0, 0, 0), "111"],
        [fromUtf8("hello"), "Cn8eVZg"],      // Bitcoin alphabet, hand-derived vector
    ], "Base58Encode");
    assertCases((...a) => Array.from(Base58Decode(...a)), [
        ["", []],
        ["1", [0]],
        ["Cn8eVZg", Array.from(fromUtf8("hello"))],
    ], "Base58Decode");
    assertCasesThrow((...a) => Base58Decode(...a), [
        ["0OIl", SyntaxError],           // chars excluded from the Bitcoin alphabet
    ], "Base58Decode");
    assertCasesThrow((...a) => Base58Encode(...a), [
        [new Uint8Array(4097), RangeError],  // input is capped at 4096 bytes (API.md)
    ], "Base58Encode");
    // d.ts: base58EncodeInto "(n*8)/5+1", zero-allocation, "n == 0 writes nothing", 4096 cap
    assertCases((data, cap) => { const out = new Uint8Array(cap); const k = base58EncodeInto(data, out); return [k, Array.from(out.slice(0, k))]; }, [
        [fromUtf8("hello"), 9, [7, [67, 110, 56, 101, 86, 90, 103]]],   // "Cn8eVZg"
        [u8(0), 2, [1, [49]]],                                          // "1"; bound (n*8)/5+1 = 2
        [u8(), 1, [0, []]],
    ], "base58EncodeInto");
    assertCasesThrow((data, out) => base58EncodeInto(data, out), [
        [new Uint8Array(4097), new Uint8Array(6600), RangeError],
    ], "base58EncodeInto");
    // d.ts: base58DecodeInto "at least text.length bytes or RangeError"
    assertCases((text, cap) => { const out = new Uint8Array(cap); const k = base58DecodeInto(text, out); return [k, Array.from(out.slice(0, k))]; }, [
        ["Cn8eVZg", 7, [5, [104, 101, 108, 108, 111]]],
    ], "base58DecodeInto");
    assertCasesThrow((text, out) => base58DecodeInto(text, out), [
        ["Cn8eVZg", new Uint8Array(6), RangeError],
    ], "base58DecodeInto");

    // d.ts: Base58Check "Base58 with a double-SHA256 checksum appended"; decode refuses a bad
    // checksum or an input "too short to carry one".
    const payload = fromUtf8("hello");
    const chk = Base58CheckEncode(payload);
    assertCases((p) => Array.from(Base58CheckDecode(Base58CheckEncode(p))), [
        [u8(), []],
        [u8(0), [0]],
        [payload, Array.from(payload)],
    ], "Base58Check round trip");
    assertCasesThrow((...a) => Base58CheckDecode(...a), [
        ["2", SyntaxError],                                  // too short to carry a 4-byte checksum
        [(chk[0] === "1" ? "2" : "1") + chk.slice(1), SyntaxError],   // corrupted payload -> mismatch
    ], "Base58CheckDecode");
    assertCases((p) => { const out = new Uint8Array(15); const k = base58CheckEncodeInto(p, out); return [k, Array.from(out.slice(0, k))]; }, [
        // d.ts: base58CheckEncodeInto capacity "((n+4)*8)/5+1" = 15 for n = 5
        // (the case fn returns [k, bytes]; destructure k from it)
        [payload, ([k]) => k === chk.length && k <= 15],
    ], "base58CheckEncodeInto");
    assertCasesThrow((text, out) => base58CheckDecodeInto(text, out), [
        [chk, new Uint8Array(chk.length - 1), RangeError],   // capacity as base58DecodeInto (d.ts)
    ], "base58CheckDecodeInto");
}

/* ---------------- baseX (custom alphabet) ---------------- */
{
    // d.ts: "Encode in a caller-supplied alphabet of 2..255 distinct characters"; leading zero
    // bytes become the alphabet's first character (API-pinned ff example).
    assertCases((...a) => BaseXEncode(...a), [
        [u8(0xFF), "0123456789abcdef", "ff"],
        [u8(0, 0, 255), "01", "0011111111"],
        [u8(), "01", ""],
    ], "BaseXEncode");
    assertCases((...a) => Array.from(BaseXDecode(...a)), [
        ["ff", "0123456789abcdef", [255]],
        ["0011111111", "01", [0, 0, 255]],
    ], "BaseXDecode");
    assertCasesThrow((...a) => BaseXDecode(...a), [
        ["fq", "0123456789abcdef", SyntaxError],    // a character outside the alphabet
    ], "BaseXDecode");
    assertCasesThrow((...a) => BaseXEncode(...a), [
        [u8(1), "aab", RangeError],     // a repeated alphabet character throws at the call (API.md)
        [u8(1), "a", null],             // alphabet must have 2..255 distinct characters (d.ts)
        [new Uint8Array(4097), "01", null],   // 4096-byte input cap (API.md)
    ], "BaseXEncode");
    assertCasesThrow((...a) => BaseXDecode(...a), [
        ["ff", "abab", RangeError],     // repeated alphabet character (decode side)
    ], "BaseXDecode");
}

/* ---------------- varints ---------------- */
{
    // d.ts: PutUvarint "LEB128 encoding of a non-negative value, at most 10 bytes"; API-pinned
    // vectors 300 -> [172, 2] and PutVarint(-2) -> [3].
    assertCases((...a) => Array.from(PutUvarint(...a)), [
        [0, [0]],
        [127, [0x7F]],
        [128, [0x80, 0x01]],
        [300, [0xAC, 0x02]],
        [2n ** 53n, [0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x10]],   // 8 bytes, BigInt accepted
        [0xFFFFFFFFFFFFFFFFn, [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01]],  // 10-byte max
    ], "PutUvarint");
    assertCasesThrow((...a) => PutUvarint(...a), [
        [-1, RangeError],
        [1.5, RangeError],
        [2 ** 53, RangeError],           // an unsafe number refuses (API.md)
    ], "PutUvarint");
    // d.ts: PutVarint "Zigzag-encoded signed LEB128"
    assertCases((...a) => Array.from(PutVarint(...a)), [
        [0, [0]],
        [-1, [1]],
        [1, [2]],
        [-2, [3]],
        [2, [4]],
        [63, [126]],
        [-64, [127]],
        [64, [0x80, 0x01]],
        [-65, [0x81, 0x01]],
    ], "PutVarint");
    assertCasesThrow((...a) => PutVarint(...a), [
        [1.5, RangeError],
    ], "PutVarint");
    // d.ts: Uvarint "[value, bytesRead]"; Number <= 2^53-1 else BigInt; truncated -> [0, 0];
    // 64-bit overflow -> RangeError.
    assertCases((...a) => Uvarint(...a), [
        [u8(0xAC, 0x02), [300, 2]],
        [u8(0x80), [0, 0]],                       // truncated stream
        [u8(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01), [0xFFFFFFFFFFFFFFFFn, 10]],
        [PutUvarint(2n ** 53n), (v) => v[0] === 2n ** 53n && typeof v[0] === "bigint" && v[1] === 8],
        [PutUvarint(9007199254740991), (v) => v[0] === 9007199254740991 && typeof v[0] === "number"],
    ], "Uvarint");
    assertCasesThrow((...a) => Uvarint(...a), [
        [u8(0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02), RangeError],   // final byte above the 64-bit boundary
        [u8(0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01), RangeError],   // an 11th continuation byte (API.md)
    ], "Uvarint");
    assertCases((...a) => Varint(...a), [
        [u8(3), [-2, 1]],
        [PutVarint(-65), [-65, 2]],
        [PutVarint(64), [64, 2]],
    ], "Varint");
    // d.ts: appendUvarint "returns the NEW END offset. No reallocation: a buffer too small at
    // `offset` throws RangeError and nothing is written."
    assertCases((v, off) => { const b = new Uint8Array(16); return appendUvarint(b, v, off); }, [
        [300, undefined, 2],
        [300, 0, 2],
        [300, 2, 4],
        [7, 3, 4],
        [2n ** 53n, 0, 8],
    ], "appendUvarint");
    assertCases((v, off) => { const b = new Uint8Array(16); appendUvarint(b, v, off); return Array.from(b.slice(0, 4)); }, [
        [300, 0, [0xAC, 0x02, 0, 0]],
        [7, 3, [0, 0, 0, 7]],
    ], "appendUvarint content");
    // 300 is 2 bytes and FITS a 4-byte buffer at offset 0; the contracted
    // refusal is "a buffer too small at `offset`": 2 bytes at offset 3 of a
    // 4-byte buffer cannot fit.
    assertCasesThrow((v, off) => { const b = new Uint8Array(4); appendUvarint(b, v, off); }, [
        [300, 3, RangeError],
    ], "appendUvarint");
    assertCases((v, off) => { const b = new Uint8Array(4); try { appendUvarint(b, v, off); } catch (e) { return Array.from(b); } return "no-throw"; }, [
        [300, 3, [0, 0, 0, 0]],             // nothing written before the refusal (API.md)
    ], "appendUvarint");
    // d.ts: uvarintAt — "a truncated stream returns 0 ... an offset BEYOND the end is RangeError,
    // while offset == length is an empty stream (returns 0)"; varintAt is the zigzag signed form.
    assertCases((...a) => uvarintAt(...a), [
        [u8(0xAC, 0x02), 300],
        [u8(0xAC, 0x02), 1, 2],
        [u8(), 0],                       // offset == length -> empty stream -> 0
        [u8(0x80), 0],                   // truncated -> 0
        [PutUvarint(0xFFFFFFFFFFFFFFFFn), (v) => v === 0xFFFFFFFFFFFFFFFFn],
    ], "uvarintAt");
    assertCasesThrow((...a) => uvarintAt(...a), [
        [u8(0xAC), 5, RangeError],       // offset beyond the end
    ], "uvarintAt");
    assertCases((...a) => varintAt(...a), [
        [u8(2), 1],
        [u8(3), -2],
        [PutVarint(-65), -65],
    ], "varintAt");
}

/* ---------------- charset detection ---------------- */
{
    // d.ts: "Deterministic charset detection"; "Same as DetectEncoding under its lowercase name";
    // "throws TypeError when allowList excludes the verdict" (and the fallback).
    assertCases((...a) => DetectEncoding(...a), [
        [fromUtf8("plain ascii text!!"), "utf-8"],
        [fromUtf8("x"), { allowList: ["utf-8"] }, "utf-8"],
    ], "DetectEncoding");
    assertCases((...a) => detectEncoding(...a), [
        [fromUtf8("plain ascii text!!"), "utf-8"],
    ], "detectEncoding");
    assertCasesThrow((...a) => DetectEncoding(...a), [
        [fromUtf8("x"), { allowList: ["gbk"] }, TypeError],   // verdict AND fallback excluded
    ], "DetectEncoding");
    assertEq(detectEncoding(fromUtf8("hello")), DetectEncoding(fromUtf8("hello")), "detectEncoding is the same door under its lowercase name (d.ts)");
}

/* ---------------- JSON5 / StableStringify ---------------- */
{
    // d.ts: "JSON5 superset parsing; depth capped at 256"; API-pinned example.
    assertCases((...a) => JSON5Parse(...a), [
        ["{ a: 1, b: 'x', }", (v) => v.a === 1 && v.b === "x"],
        ["0x10", 16],                        // hex literal
        ["[1, 2, ]", (v) => Array.isArray(v) && v.length === 2],
        ["// c\n[1]", (v) => Array.isArray(v) && v.length === 1],
        // 200-deep nesting is UNDER the 256 cap: it parses as a chain of
        // 200 single-element arrays around one empty array (depth, not
        // outer length, is what the cap bounds).
        ["[".repeat(200) + "]".repeat(200), (v) => {
            let d = 0, c = v;
            while (Array.isArray(c)) { d++; c = c[0]; }
            return d === 200 && c === undefined;
        }],
    ], "JSON5Parse");
    assertCasesThrow((...a) => JSON5Parse(...a), [
        ['["'.repeat(300) + '"]'.repeat(300), null],   // nest bomb refused by the 256 cap (d.ts)
        ["{a:1", SyntaxError],
    ], "JSON5Parse");
    // d.ts: JSON5Stringify "unquoted keys and NaN/Infinity literals; indent clamped to 0-10";
    // cycles throw, DAG repeats are legal (API.md).
    assertCases((v) => JSON5Parse(JSON5Stringify(v)), [
        [{ a: 1, b: "x", c: [true, false, null] }, { a: 1, b: "x", c: [true, false, null] }],
        [{ n: NaN, i: Infinity }, (v) => Number.isNaN(v.n) && v.i === Infinity],
    ], "JSON5Stringify round trip");
    assertCases(() => JSON5Stringify({ a: 1 }), [
        [undefined, (s) => s.indexOf("\n") === -1],
    ], "JSON5Stringify indent 0");
    assertCases((ind) => JSON5Stringify({ a: 1 }, { indent: ind }).indexOf("\n") !== -1, [
        [2, true],
        [10, true],
    ], "JSON5Stringify indent");
    assertCases(() => { const shared = { x: 1 }; return JSON5Stringify({ a: shared, b: shared }).length > 0; }, [
        [undefined, true],               // a repeated node in a DAG is legal (API.md)
    ], "JSON5Stringify DAG");
    assertCasesThrow((v) => JSON5Stringify(v), [
        [(() => { const c = {}; c.self = c; return c; })(), TypeError],   // cycles throw (API.md)
    ], "JSON5Stringify");

    // d.ts: StableStringify "RFC 8785 canonical JSON; NaN/Infinity rejected. ... `indent` is
    // accepted and ignored."
    assertCases((v, o) => StableStringify(v, o), [
        [{ b: 1, a: 2 }, undefined, '{"a":2,"b":1}'],
        [{ b: { d: 1, c: 2 }, a: 1 }, undefined, '{"a":1,"b":{"c":2,"d":1}}'],
        [{ x: -0 }, undefined, '{"x":0}'],            // RFC 8785: -0 canonicalizes to 0
        [{ b: 1, a: 2 }, { indent: 5 }, '{"a":2,"b":1}'],   // indent ignored
        [[3, 1], undefined, "[3,1]"],
    ], "StableStringify");
    assertCasesThrow((v) => StableStringify(v), [
        [{ x: NaN }, TypeError],
        [{ x: Infinity }, TypeError],
    ], "StableStringify");
}

/* ---------------- JSONPath ---------------- */
{
    // d.ts: "Compiled RFC 9535 JSONPath expression, reusable across queries."
    const doc = { store: { book: [{ author: "a1" }, { author: "a2" }] } };
    assertCases((expr, v) => new JSONPath(expr).all(v), [
        ["$..author", doc, ["a1", "a2"]],
        ["$.store.nope", doc, []],
        ["$..author", doc, ["a1", "a2"]],    // the compiled expression is reusable
    ], "JSONPath.all");
    assertCases((expr, v) => new JSONPath(expr).first(v), [
        ["$..author", doc, "a1"],
        ["$.store.nope", doc, undefined],
    ], "JSONPath.first");
    assertCases((expr, v) => new JSONPath(expr).paths(v), [
        // d.ts: "Normalized path strings like $['store']['book'][0]['author']."
        ["$.store.book[*]", doc, (ps) => ps.length === 2 && ps[0].indexOf("$['store']['book'][0]") === 0 && ps[1].indexOf("$['store']['book'][1]") === 0],
    ], "JSONPath.paths");
    assertCasesThrow((expr) => new JSONPath(expr), [
        ["$[unlikely", null],
    ], "new JSONPath");
}

/* ---------------- QR ---------------- */
{
    // d.ts: "QR options: ecc L/M/Q/H, version 1-40, mask 0-7"; "a symbol holds at most 2953 bytes".
    assertCases((t, o) => { const r = QREncode(t, o); return [r.version >= 1 && r.version <= 40, r.modules instanceof Uint8Array, r.modules.length === r.size * r.size]; }, [
        ["hello", undefined, [true, true, true]],
        ["hello", { ecc: "H" }, [true, true, true]],
        ["hello", { mask: 7 }, [true, true, true]],
        ["hello", { ecc: "L", version: 1 }, [true, true, true]],
    ], "QREncode");
    assertCases((t) => { const s = QRToString(t); return typeof s === "string" && s.indexOf("\n") !== -1; }, [
        ["hello", true],
        ["", true],
    ], "QRToString");
    assertCasesThrow((...a) => QREncode(...a), [
        ["x".repeat(2954), null],        // one byte over the documented 2953 maximum
        ["hello", { mask: 8 }, null],    // mask outside the documented 0-7
    ], "QREncode");
}

print("bb_encoding: all tests passed (" + n + " assertions)");
