// Parametric black-box contract test for dyna:serialize, generated from dynajs.d.ts lines 4500-4580. Engine sources not consulted.

import {
    Proto, ASN1,
    MsgPackEncode, MsgPackDecode,
    CBOREncode, CBORDecode, CBORCanonical,
    ValueHash, structuredClone,
} from "dyna:serialize";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertClose(actual, expected, eps, msg) { n++; if (!(Math.abs(actual - expected) <= eps)) throw new Error("assertion failed (close): " + msg + " — got |" + actual + "| expected |" + expected + "±" + eps + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function assertEqArr(actual, expected, msg) { n++; if (!eqArr(actual, expected)) throw new Error("assertion failed (arr): " + msg + " — got |" + JSON.stringify(Array.from(actual)) + "| expected |" + JSON.stringify(expected) + "|"); }

const SKIP = Symbol("skip");

/* ------------------------------------------------------------------ *
 *  Table 1: ASN1 — canonical DER encode/decode round trips.
 *  Row: [label, node, expectedDecodedValue | SKIP | predicate(Uint8Array|bigint|fn),
 *        expectedExactBytes | null, expectedCls, expectedConstructed]
 *  Exact bytes only where X.690 fixes them and the doc cites the rule
 *  ("minimal two's complement per X.690 8.3.2"; tags 22/30/28 named in doc).
 * ------------------------------------------------------------------ */
const ASN1_ROWS = [
    ["bool true", ASN1.bool(true), true, [0x01, 0x01, 0xff], 0, false],
    ["bool false", ASN1.bool(false), false, [0x01, 0x01, 0x00], 0, false],
    ["null", ASN1.null(), null, [0x05, 0x00], 0, false],
    ["int 0", ASN1.int(0), 0, [0x02, 0x01, 0x00], 0, false],
    ["int 1", ASN1.int(1), 1, [0x02, 0x01, 0x01], 0, false],
    ["int 127 boundary single byte", ASN1.int(127), 127, [0x02, 0x01, 0x7f], 0, false],
    ["int 128 grows to two bytes", ASN1.int(128), 128, [0x02, 0x02, 0x00, 0x80], 0, false],
    ["int 255 keeps leading zero", ASN1.int(255), 255, [0x02, 0x02, 0x00, 0xff], 0, false],
    ["int 256", ASN1.int(256), 256, [0x02, 0x02, 0x01, 0x00], 0, false],
    ["int -1 minimal two's complement", ASN1.int(-1), -1, [0x02, 0x01, 0xff], 0, false],
    ["int -128 minimal", ASN1.int(-128), -128, [0x02, 0x01, 0x80], 0, false],
    ["int -129 grows", ASN1.int(-129), -129, [0x02, 0x02, 0xff, 0x7f], 0, false],
    ["int 65536", ASN1.int(65536), 65536, [0x02, 0x03, 0x01, 0x00, 0x00], 0, false],
    ["int 2^53 decodes as Number", ASN1.int(9007199254740992), 9007199254740992, null, 0, false],
    ["int 2^53+1 decodes as exact BigInt", ASN1.int(9007199254740993n), 9007199254740993n, null, 0, false],
    ["int BigInt 2^62 decodes as BigInt", ASN1.int(4611686018427387904n), 4611686018427387904n, null, 0, false],
    ["int small BigInt within 2^53 decodes as Number", ASN1.int(5n), 5, null, 0, false],
    ["utf8 empty", ASN1.utf8(""), "", [0x0c, 0x00], 0, false],
    ["utf8 single ascii", ASN1.utf8("A"), "A", [0x0c, 0x01, 0x41], 0, false],
    ["utf8 multibyte", ASN1.utf8("caf\u00e9 \u4e2d"), "caf\u00e9 \u4e2d", null, 0, false],
    ["octets empty", ASN1.octets(new Uint8Array(0)), new Uint8Array(0), [0x04, 0x00], 0, false],
    ["octets 0x00 0xff", ASN1.octets(new Uint8Array([0x00, 0xff])), new Uint8Array([0x00, 0xff]), [0x04, 0x02, 0x00, 0xff], 0, false],
    ["bitString with 5 unused bits (content = unused-count octet)", ASN1.bitString(new Uint8Array([0xa0]), 5), SKIP, [0x03, 0x02, 0x05, 0xa0], 0, false],
    ["oid 1.2.3", ASN1.oid("1.2.3"), "1.2.3", [0x06, 0x02, 0x2a, 0x03], 0, false],
    ["ia5String ascii (doc: tag 22)", ASN1.ia5String("abc"), "abc", null, 0, false],
    ["printable string with space", ASN1.printable("abc 123"), "abc 123", null, 0, false],
    ["bmpString UCS-2 range (doc: tag 30)", ASN1.bmpString("a\u00e9\u4e2d"), "a\u00e9\u4e2d", null, 0, false],
    ["universalString UCS-4 (doc: tag 28)", ASN1.universalString("a\u00e9"), "a\u00e9", null, 0, false],
    ["utcTime passthrough", ASN1.utcTime("250101000000Z"), "250101000000Z", null, 0, false],
    ["generalizedTime passthrough", ASN1.generalizedTime("20250101000000Z"), "20250101000000Z", null, 0, false],
    // X.690 8.1.2.2: class bits are 00 universal, 01 application, 10 context-specific,
    // 11 private — 0x80/0xa1 is class 2, not 1.
    ["context primitive tag 0", ASN1.context(0, new Uint8Array([0x07])), new Uint8Array([0x07]), [0x80, 0x01, 0x07], 2, false],
    // d.ts types ASN1Node.value as `unknown` and pins no recursive decode for
    // context-constructed nodes (only the doc's "Canonical DER codec"); the
    // DER-meaningful property is X.690 8.1.3: a constructed node's contents
    // octets are the concatenation of its children's encodings ([0x05,0x00]).
    ["context constructed tag 1", ASN1.contextC(1, [ASN1.null()]), (v) => v instanceof Uint8Array && v.length === 2 && v[0] === 0x05 && v[1] === 0x00, [0xa1, 0x02, 0x05, 0x00], 2, true],
    ["seq of bool+int (SEQUENCE tag 16)", ASN1.seq([ASN1.bool(true), ASN1.int(3)]), (v) => Array.isArray(v) && v.length === 2 && v[0].tag === 1 && v[1].tag === 2, [0x30, 0x06, 0x01, 0x01, 0xff, 0x02, 0x01, 0x03], 0, true],
    ["set of null (SET tag 17)", ASN1.set([ASN1.null()]), (v) => Array.isArray(v) && v.length === 1 && v[0].tag === 5, [0x31, 0x02, 0x05, 0x00], 0, true],
];

for (const [label, node, expVal, expBytes, expCls, expCtor] of ASN1_ROWS) {
    const bytes = ASN1.encode(node);
    if (expBytes !== null) assertEqArr(Array.from(bytes), expBytes, label + " — exact DER bytes");
    const back = ASN1.decode(bytes);
    assertEq(back.cls, expCls, label + " — decoded cls");
    assertEq(back.constructed, expCtor, label + " — decoded constructed flag");
    if (expVal !== SKIP) {
        if (typeof expVal === "function") {
            assert(expVal(back.value), label + " — decoded value predicate");
        } else if (expVal instanceof Uint8Array) {
            assertEqArr(Array.from(back.value), Array.from(expVal), label + " — decoded bytes value");
        } else if (typeof expVal === "bigint") {
            n++;
            if (typeof back.value !== "bigint" || back.value !== expVal) throw new Error("assertion failed: " + label + " — decoded bigint — got |" + String(back.value) + "|");
        } else {
            assertEq(back.value, expVal, label + " — decoded value");
        }
    }
}

/* Table 1b: ASN1 documented refusals. */
const ASN1_REFUSALS = [
    ["ia5String refuses non-ASCII at encode", () => ASN1.encode(ASN1.ia5String("caf\u00e9"))],
    ["bmpString refuses lone surrogate", () => ASN1.encode(ASN1.bmpString("\ud800"))],
    ["bmpString refuses code point above U+FFFF", () => ASN1.encode(ASN1.bmpString("\ud83d\ude00"))],
    ["universalString refuses lone surrogate", () => ASN1.encode(ASN1.universalString("\ud800"))],
    ["int refuses content beyond the 64-byte cap at encode", () => ASN1.encode(ASN1.int(2n ** 520n))],
    ["int refuses content beyond the 64-byte cap at decode", () => ASN1.decode(new Uint8Array(67).fill(0, 2).fill(2, 0, 1).fill(65, 1, 2))],
    ["decode refuses a truncated length prefix", () => ASN1.decode(Uint8Array.from([0x02, 0x05, 0x01]))],
];
for (const [label, fn] of ASN1_REFUSALS) assertThrows(fn, label);

/* ------------------------------------------------------------------ *
 *  Table 2: Proto — protobuf wire encode/decode.
 *  Row: [label, value, schema, expectedDecoded, expectedExactBytes | null]
 *  Exact bytes only for the canonical wire examples derivable by hand.
 * ------------------------------------------------------------------ */
const F = (fields) => ({ fields });
const INT32_A = F([{ name: "a", number: 1, type: "int32" }]);
const STR_B = F([{ name: "b", number: 2, type: "string" }]);
const PROTO_ROWS = [
    ["varint int32 150 (canonical wire example)", { a: 150 }, INT32_A, { a: 150 }, [0x08, 0x96, 0x01]],
    ["length-delimited string (canonical wire example)", { b: "testing" }, STR_B, { b: "testing" }, [0x12, 0x07, 0x74, 0x65, 0x73, 0x74, 0x69, 0x6e, 0x67]],
    ["bool true round trip", { c: true }, F([{ name: "c", number: 1, type: "bool" }]), { c: true }, null],
    ["double 1.5 round trip", { d: 1.5 }, F([{ name: "d", number: 1, type: "double" }]), { d: 1.5 }, null],
    ["repeated packed int32 round trip", { e: [1, 2, 3] }, F([{ name: "e", number: 5, type: "int32", repeated: true, packed: true }]), { e: [1, 2, 3] }, null],
    ["nested message round trip", { outer: { inner: 7 } }, F([{ name: "outer", number: 1, type: "message", message: { fields: [{ name: "inner", number: 1, type: "int32" }] } }]), { outer: { inner: 7 } }, null],
];
for (const [label, value, schema, expected, expBytes] of PROTO_ROWS) {
    const bytes = Proto.encode(value, schema);
    if (expBytes !== null) assertEqArr(Array.from(bytes), expBytes, label + " — exact wire bytes");
    assertDeepEq(Proto.decode(bytes, schema), expected, label + " — decode(encode(value)) parity");
}

/* Table 2b: Proto documented refusals ("strict about JS types and
 * out-of-range numbers"; decode is "lengths validated"). */
const PROTO_REFUSALS = [
    ["int32 rejects a fractional value", () => Proto.encode({ a: 1.5 }, INT32_A)],
    ["int32 rejects out-of-range 2^31", () => Proto.encode({ a: 2147483648 }, INT32_A)],
    ["int32 rejects a string value (strict JS types)", () => Proto.encode({ a: "1" }, INT32_A)],
    ["decode rejects a length running past end of input", () => Proto.decode(Uint8Array.from([0x0a, 0x05]), STR_B)],
];
for (const [label, fn] of PROTO_REFUSALS) assertThrows(fn, label);

/* ------------------------------------------------------------------ *
 *  Table 3: MsgPack + CBOR round trips — same value rows driven
 *  through BOTH codec doors (parity across the two encoders).
 * ------------------------------------------------------------------ */
const CODEC_ROWS = [
    ["null", null],
    ["true", true],
    ["false", false],
    ["0", 0],
    ["1", 1],
    ["-1", -1],
    ["127 fixint boundary", 127],
    ["-33 negative boundary", -33],
    ["255", 255],
    ["256", 256],
    ["65536", 65536],
    ["2^32", 4294967296],
    ["-2^31", -2147483648],
    ["1.5", 1.5],
    ["-0.25", -0.25],
    ["1e300 float64", 1e300],
    ["empty string", ""],
    ["single char", "a"],
    ["multibyte string", "h\u00e9llo"],
    ["long string crosses 32-byte header", "x".repeat(40)],
    ["empty array", []],
    ["int array", [1, 2, 3]],
    ["nested arrays", [1, [2, [3]]]],
    ["empty map", {}],
    ["flat map", { a: 1 }],
    ["nested map", { a: { b: [1, true, null] } }],
    ["mixed container", { n: [1, { s: "t" }], f: 1.25 }],
];
let codecRowCount = 0;
for (const [label, v] of CODEC_ROWS) {
    assertDeepEq(MsgPackDecode(MsgPackEncode(v)), v, "msgpack round trip: " + label);
    assertDeepEq(CBORDecode(CBOREncode(v)), v, "cbor round trip: " + label);
    codecRowCount++;
}

/* Table 3b: codec refusals — "refuses symbols and functions"; the
 * decoders are hardened: depth-capped and length-validated, so hostile
 * payloads refuse instead of exhausting memory. 200-deep nesting is
 * far above any documented cap. */
const CODEC_REFUSALS = [
    ["MsgPackEncode refuses a function", () => MsgPackEncode(() => 1)],
    ["MsgPackEncode refuses a symbol", () => MsgPackEncode(Symbol("s"))],
    ["CBOREncode refuses a function", () => CBOREncode(() => 1)],
    ["CBOREncode refuses a symbol", () => CBOREncode(Symbol("s"))],
    ["MsgPackDecode refuses a 200-deep hostile nest (0x91 fixarray)", () => MsgPackDecode(new Uint8Array(200).fill(0x91))],
    ["CBORDecode refuses a 200-deep hostile nest (0x81 array)", () => CBORDecode(new Uint8Array(200).fill(0x81))],
];
for (const [label, fn] of CODEC_REFUSALS) assertThrows(fn, label);

/* Table 4: CBORCanonical — map keys sorted byte-wise, so insertion
 * order is irrelevant and repeat calls are byte-identical. */
const CANON_ROWS = [
    ["key insertion order erased", CBORCanonical({ b: 1, a: 2 }), CBORCanonical({ a: 2, b: 1 })],
    ["nested key order erased", CBORCanonical({ z: { y: 2, x: 1 } }), CBORCanonical({ z: { x: 1, y: 2 } })],
    ["deterministic across calls", CBORCanonical([1, "two", { k: 3 }]), CBORCanonical([1, "two", { k: 3 }])],
];
for (const [label, a, b] of CANON_ROWS) assertEqArr(Array.from(a), Array.from(b), label + " — canonical bytes identical");
assertEqArr(Array.from(CBORCanonical({ a: 1 })), [0xa1, 0x61, 0x61, 0x01], "canonical {a:1} exact CBOR bytes (map(1) text('a') int(1))");

/* Table 5: ValueHash — canonical CBOR through XXH64 as 16 lowercase
 * hex chars. Exact digests are XXH64-internal, so rows pin FORMAT,
 * determinism, order-invariance and distinctness instead. */
const hexRe = /^[0-9a-f]{16}$/;
assert(hexRe.test(ValueHash({ a: 1 })), "ValueHash returns 16 lowercase hex chars");
assertEq(ValueHash([1, 2, 3]), ValueHash([1, 2, 3]), "ValueHash deterministic for equal values");
assertEq(ValueHash({ a: 1, b: 2 }), ValueHash({ b: 2, a: 1 }), "ValueHash key-order invariant (canonical form)");
assert(ValueHash(1) !== ValueHash(2), "ValueHash differs for different scalars");
assert(ValueHash("a") !== ValueHash("b"), "ValueHash differs for different strings");

/* ------------------------------------------------------------------ *
 *  Table 6: structuredClone — deep clone with cycles, shared refs,
 *  Date, RegExp, Map, Set, ArrayBuffers, typed arrays.
 *  Row: [label, makeValue, check(original, clone)]
 * ------------------------------------------------------------------ */
const CLONE_ROWS = [
    ["number", () => 42, (v, c) => assertEq(c, 42, "number clone")],
    ["string", () => "s", (v, c) => assertEq(c, "s", "string clone")],
    ["boolean", () => true, (v, c) => assertEq(c, true, "boolean clone")],
    ["null", () => null, (v, c) => assertEq(c, null, "null clone")],
    ["undefined", () => undefined, (v, c) => assertEq(c, undefined, "undefined clone")],
    ["BigInt", () => 123n, (v, c) => { n++; if (c !== 123n) throw new Error("assertion failed: BigInt clone — got |" + String(c) + "|"); }],
    ["Date", () => new Date(1234567890), (v, c) => { assert(c instanceof Date, "Date stays a Date"); assertEq(c.getTime(), 1234567890, "Date time preserved"); assert(c !== v, "Date is a fresh object"); }],
    ["RegExp", () => /ab+c/gi, (v, c) => { assert(c instanceof RegExp, "RegExp stays a RegExp"); assertEq(c.source, "ab+c", "RegExp source"); assertEq(c.flags, "gi", "RegExp flags"); assert(c !== v, "RegExp is a fresh object"); }],
    ["Map", () => new Map([["a", 1], ["b", 2]]), (v, c) => { assert(c instanceof Map, "Map stays a Map"); assertEq(c.size, 2, "Map size"); assertEq(c.get("a"), 1, "Map entry"); assert(c !== v, "Map is a fresh object"); }],
    ["Set", () => new Set([1, 2, 3]), (v, c) => { assert(c instanceof Set, "Set stays a Set"); assertEq(c.size, 3, "Set size"); assert(c.has(2), "Set member"); assert(c !== v, "Set is a fresh object"); }],
    ["Uint8Array", () => new Uint8Array([1, 2, 3]), (v, c) => { assert(c instanceof Uint8Array, "typed array stays typed"); assertEqArr(Array.from(c), [1, 2, 3], "typed array bytes"); assert(c !== v, "typed array is a fresh view"); }],
    ["ArrayBuffer", () => new Uint8Array([9, 8]).buffer, (v, c) => { assert(c instanceof ArrayBuffer, "ArrayBuffer stays an ArrayBuffer"); assertEq(c.byteLength, 2, "ArrayBuffer byteLength"); assert(c !== v, "ArrayBuffer is fresh"); }],
    ["cycle preserved to the clone root", () => { const o = {}; o.self = o; return o; }, (v, c) => { assert(c !== v, "clone is a new object"); assert(c.self === c, "cycle points at the clone, not the original"); }],
    ["shared references preserved inside one clone", () => { const shared = { v: 1 }; return { a: shared, b: shared }; }, (v, c) => { assert(c.a === c.b, "shared reference identity preserved within the clone"); assert(c.a !== v.a, "shared object is itself cloned"); }],
    ["deep nesting", () => ({ x: { y: { z: [1, 2] } } }), (v, c) => assertDeepEq(c, { x: { y: { z: [1, 2] } } }, "deep structure equal")],
];
for (const [label, make, check] of CLONE_ROWS) {
    const v = make();
    try { check(v, structuredClone(v)); } catch (e) { throw new Error("row [" + label + "]: " + (e && e.message ? e.message : String(e))); }
}

/* Table 6b: structuredClone documented refusals (functions, accessor
 * properties). */
assertThrows(() => structuredClone(function () {}), "structuredClone refuses top-level functions");
assertThrows(() => structuredClone({ fn: () => 1 }), "structuredClone refuses nested functions");
{
    const o = {};
    Object.defineProperty(o, "g", { get: () => 1, enumerable: true });
    assertThrows(() => structuredClone(o), "structuredClone refuses accessor properties");
}

print("bb_serialize: all tests passed (" + n + " assertions)");
