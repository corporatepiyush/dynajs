#!/usr/bin/env python3
"""gen_serialize.py — dyna:serialize probes.

Oracles:
  - RFC 8949 (CBOR) appendix-style pinned vectors for core types
  - the MessagePack spec's canonical example {"compact":true,"schema":0}
  - format-boundary round-trips (fixstr/str8/str16..., fixarray/array16...)
  - corruption fuzz: every single-byte mutation and truncation of a real
    blob plus seeded random garbage -- assert clean error or clean value,
    never a crash or hang
"""
import json as jsonmod
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe

def probe_cbor():
    # RFC 8949 pinned vectors (spec-fixed encodings)
    pins = [
        ("0", "00"), ("1", "01"), ("10", "0a"), ("23", "17"), ("24", "1818"),
        ("25", "1819"), ("100", "1864"), ("1000", "1903e8"),
        ("1000000", "1a000f4240"), ("1000000000000", "1b000000e8d4a51000"),
        ("-1", "20"), ("-10", "29"), ("-100", "3863"), ("-1000", "3903e7"),
        ('"a"', "6161"), ('"IETF"', "6449455446"),
        ('""', "60"),
        ('"\\u00fc"', "62c3bc"),
        ('"\\u6c34"', "63e6b0b4"),
        ('"\\ud800\\udf48"', "64f0908d88"),  # U+10348 (utf-8 via python)
        ("false", "f4"), ("true", "f5"), ("null", "f6"),
        ("[]", "80"), ("{}", "a0"),
        ("new Uint8Array([])", "40"),
        ("new Uint8Array([1, 2, 3, 4])", "4401020304"),
        ("[1, 2, 3]", "83010203"),
        ("[1, [2, 3], [4, 5]]", "8301820203820405"),
        ("{a: 1, b: [2, 3]}", "a26161016162820203"),
        ("1.5", "fb3ff8000000000000"),
        ("-0", "fb8000000000000000"),
        ("NaN", "fb7ff8000000000000"),
        ("Infinity", "fb7ff0000000000000"),
        ("-Infinity", "fbfff0000000000000"),
        ("9007199254740992", "1b0020000000000000"),  # 2^53 (exact double)
    ]
    rows = "\n".join('  [%s, "%s"],' % (jsonmod.dumps(v), h) for v, h in pins)
    emit = ['import { CBOREncode, CBORDecode, CBORCanonical, ValueHash } from "dyna:serialize";',
            "var PINS = [\n" + rows + "\n];"]
    emit.append("""
function hex(u8) { var s = ""; for (var i = 0; i < u8.length; i++) s += ("0" + u8[i].toString(16)).slice(-2); return s; }
for (var i = 0; i < PINS.length; i++) {
  var v = eval("(" + PINS[i][0] + ")");
  assert_eq(hex(CBOREncode(v)), PINS[i][1], "cbor pin " + PINS[i][0]);
}
// format boundaries round-trip exactly (str, array, map, bytes)
var LENS = [0, 1, 22, 23, 24, 25, 255, 256, 65535, 65536];
for (var i = 0; i < LENS.length; i++) {
  var n = LENS[i];
  var s = "";
  for (var k = 0; k < n; k++) s += "x";
  assert_eq(CBORDecode(CBOREncode(s)), s, "cbor str len=" + n);
  var arr = [];
  for (var k = 0; k < n; k++) arr.push(k);
  var back = CBORDecode(CBOREncode(arr));
  assert_eq(back.length, n, "cbor array len=" + n);
  var bytes = new Uint8Array(n);
  if (n > 0) bytes[0] = 9;
  var rt = CBORDecode(CBOREncode(bytes));
  if (n > 0) assert_eq(rt[0], 9, "cbor bytes len=" + n);
  else assert_eq(rt.length, 0, "cbor bytes len=0");
}
// deep nesting caps at 256 on BOTH sides (probe depth 250 ok, 300 throws)
function nest(n) { var cur = []; var root = cur; for (var i = 1; i < n; i++) { cur[0] = []; cur = cur[0]; } return root; }
assert_eq(CBORDecode(CBOREncode(nest(250))) !== undefined, true, "cbor depth 250");
assert_throws(function () { CBOREncode(nest(300)); }, "RangeError", "cbor encode depth");
assert_throws(function () { CBORDecode(CBOREncode(nest(250)).slice(0, 10)); }, null, "cbor truncated junk");
""")
    emit.append("""
// canonical: map keys sorted byte-wise, deterministic across calls
assert_eq(hex(CBORCanonical({ b: 1, a: 2 })), "a2616102616201", "canonical sorted keys");
assert_eq(hex(CBORCanonical({ a: 1 })), hex(CBORCanonical({ a: 1 })), "canonical deterministic");
// ValueHash: key order never matters; distinct values differ (mostly)
assert_eq(ValueHash({ b: 1, a: 2 }) === ValueHash({ a: 2, b: 1 }), true, "ValueHash order-free");
assert_ne(ValueHash({ a: 1 }), ValueHash({ a: 2 }), "ValueHash distinct");
assert_ne(ValueHash([1, 2]), ValueHash([2, 1]), "ValueHash array order matters");
assert_eq(ValueHash("x").length, 16, "ValueHash 16 hex chars");
summary("serialize_cbor");
""")
    return write_probe("serialize", "cbor", "\n".join(emit))


def probe_msgpack():
    pins = [
        ("nil-ish undefined", "c0"),
        ("true", "c3"), ("false", "c2"),
        ("0", "00"), ("127", "7f"), ("128", "cc80"), ("255", "ccff"),
        ("256", "cd0100"), ("65535", "cdffff"), ("65536", "ce00010000"),
        ("-1", "ff"), ("-33", "ccd f".replace(" ", "")),  # placeholder fixed below
        ('"abc"', "a3616263"),
        ('""', "a0"),
        ("[]", "90"), ("[1, 2, 3]", "93010203"),
        ("{}", "80"),
        ("3.14", "cb40091eb851eb851f"),
        ("new Uint8Array([1, 2, 3])", None),  # bin format impl choice; skip pin
    ]
    # fix the -33 row explicitly: uint8 family for negative under -32 is ccd f? no:
    # MessagePack: -33 = 0xd0 0xdf (int8). Compute honestly:
    rows = []
    for v, h in pins:
        if h is None:
            continue
        if v == "-33":
            h = "d0df"
        cell = jsonmod.dumps("undefined") if v == "nil-ish undefined" else jsonmod.dumps(v)
        rows.append('  [%s, "%s"],' % (cell, h))
    emit = ['import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";',
            "var PINS = [\n" + "\n".join(rows) + "\n];"]
    emit.append("""
function hex(u8) { var s = ""; for (var i = 0; i < u8.length; i++) s += ("0" + u8[i].toString(16)).slice(-2); return s; }
for (var i = 0; i < PINS.length; i++) {
  var p = PINS[i];
  if (p[0] === "nil-ish undefined") {
    assert_eq(hex(MsgPackEncode(undefined)), p[1], "msgpack undefined");
    continue;
  }
  var v = eval("(" + p[0] + ")");
  assert_eq(hex(MsgPackEncode(v)), p[1], "msgpack pin " + p[0]);
}
// the MessagePack spec canonical example
assert_eq(hex(MsgPackEncode({ compact: true, schema: 0 })),
  "82a7636f6d70616374c3a6736368656d6100", "msgpack spec example");
// format boundaries round-trip
var LENS = [0, 1, 31, 32, 255, 256, 65535, 65536];
for (var i = 0; i < LENS.length; i++) {
  var n = LENS[i];
  var s = "";
  for (var k = 0; k < n; k++) s += "y";
  assert_eq(MsgPackDecode(MsgPackEncode(s)), s, "msgpack str len=" + n);
  var arr = [];
  for (var k = 0; k < n; k++) arr.push(k);
  assert_eq(MsgPackDecode(MsgPackEncode(arr)).length, n, "msgpack array len=" + n);
}
// -0 and floats survive
assert_eq(1 / MsgPackDecode(MsgPackEncode(-0)), -Infinity, "msgpack -0 preserved");
assert_eq(MsgPackDecode(MsgPackEncode(3.14)), 3.14, "msgpack f64");
assert_ne(isNaN(MsgPackDecode(MsgPackEncode(NaN))), false, "msgpack NaN");
assert_eq(MsgPackDecode(MsgPackEncode(Infinity)), Infinity, "msgpack Inf");
// refusals documented
assert_throws(function () { MsgPackEncode(function () {}); }, "TypeError", "fn refused");
assert_throws(function () { MsgPackEncode(Symbol("s")); }, "TypeError", "symbol refused");
// trailing garbage refused
var enc = MsgPackEncode([1, 2]);
var padded = new Uint8Array(enc.length + 1);
padded.set(enc);
padded[enc.length] = 0x01;
assert_throws(function () { MsgPackDecode(padded); }, null, "msgpack trailing");
""")
    emit.append("""
// deep nesting cap
function nest(n) { var cur = []; var root = cur; for (var i = 1; i < n; i++) { cur[0] = []; cur = cur[0]; } return root; }
assert_eq(MsgPackDecode(MsgPackEncode(nest(250))) !== undefined, true, "msgpack depth 250");
assert_throws(function () { MsgPackEncode(nest(300)); }, "RangeError", "msgpack encode depth");
summary("serialize_msgpack");
""")
    return write_probe("serialize", "msgpack", "\n".join(emit))


def probe_fuzz():
    emit = ['import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode,',
            '         ASN1, Proto } from "dyna:serialize";']
    emit.append("""
function lcg(seed, n) {
  var a = seed >>> 0, out = new Uint8Array(n);
  for (var i = 0; i < n; i++) { a = (Math.imul(a, 1103515245) + 12345) >>> 0; out[i] = (a >>> 16) & 0xff; }
  return out;
}
function safeDecode(decode, blob) {
  var threw = null, r;
  try { r = decode(blob); } catch (e) { threw = (e && typeof e.name === "string") ? e.name : "?"; }
  if (threw === null) return "ok";
  if (threw === "?") return "BAD";     // thrown non-Error
  return "threw";
}
var bad = 0, calls = 0;
// a real blob with map/array/str/int/bool/bytes members
var blob = MsgPackEncode({ a: 1, b: "xyz", c: [1, 2, 3], d: true, e: new Uint8Array([9, 8]), f: -12345.6789 });
var decoders = [
  function (m) { return MsgPackDecode(m); },
  function (m) { return CBORDecode(m); },
];
// every single-byte mutation of the blob through both decoders
var mutants = [0x01, 0x7F, 0x80, 0xFF, 0x00];
for (var i = 0; i < blob.length; i++) {
  for (var mi = 0; mi < mutants.length; mi++) {
    var m = new Uint8Array(blob);
    m[i] = (m[i] ^ mutants[mi]) & 0xff;
    for (var di = 0; di < decoders.length; di++) {
      calls++;
      var res = safeDecode(decoders[di], m);
      if (res === "BAD") { bad++; if (bad < 4) __out("FAIL non-Error throw i=" + i + " m=" + mutants[mi] + " d=" + di); }
    }
  }
}
// every truncation
for (var i = 0; i < blob.length; i++) {
  var t = blob.subarray(0, i);
  for (var di = 0; di < decoders.length; di++) {
    calls++;
    var res = safeDecode(decoders[di], t);
    if (res === "BAD") { bad++; if (bad < 4) __out("FAIL trunc non-Error i=" + i + " d=" + di); }
  }
}
// seeded random garbage
for (var t2 = 0; t2 < 400; t2++) {
  var len = (t2 % 40) + 1;
  var g = lcg(31337 + t2, len);
  for (var di = 0; di < decoders.length; di++) {
    calls++;
    var res = safeDecode(decoders[di], g);
    if (res === "BAD") { bad++; if (bad < 4) __out("FAIL garbage non-Error t=" + t2); }
  }
}
if (bad) { __fail += 1; __out("FAIL fuzz violations " + bad); } else __pass++;
// ASN.1 hostile forms documented as refused with named reasons
assert_throws(function () { ASN1.decode(new Uint8Array([0x30, 0x80, 0x01, 0x00, 0x00, 0x00])); }, "SyntaxError", "asn1 indefinite");
assert_throws(function () { ASN1.decode(new Uint8Array([0x30, 0x82, 0x00, 0x02, 0x01, 0x00])); }, "SyntaxError", "asn1 non-minimal len");
assert_throws(function () { ASN1.decode(new Uint8Array([0x04, 0x05, 0x61, 0x62])); }, "SyntaxError", "asn1 overrun");
assert_throws(function () { ASN1.decode(new Uint8Array([0x02, 0x09, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF])); }, "SyntaxError", "asn1 int >8 bytes");
assert_throws(function () { ASN1.decode(new Uint8Array([0x30, 0x03, 0x02])); }, "SyntaxError", "asn1 trunc");
// canonical DER re-encode identity
var der = ASN1.encode(ASN1.seq([ASN1.int(12345), ASN1.utf8("hi")]));
assert_eq(ASN1.encode(ASN1.decode(der)).every(function (v, k) { return v === der[k]; }), true, "asn1 reencode identity");
assert_eq(hexs(der), "3008020230390c026869", "asn1 pinned bytes");
function hexs(u8) { var s = ""; for (var i = 0; i < u8.length; i++) s += ("0" + u8[i].toString(16)).slice(-2); return s; }
// Proto decoder fuzz: random bytes against a schema never crash
var schema = { fields: [ { name: "id", number: 1, type: "int32" }, { name: "name", number: 2, type: "string" } ] };
bad = 0;
for (var t3 = 0; t3 < 400; t3++) {
  var g = lcg(4242 + t3, (t3 % 30) + 1);
  var res = safeDecode(function (m) { return Proto.decode(m, schema); }, g);
  if (res === "BAD") bad++;
}
// every truncation of a valid message
var pmsg = Proto.encode({ id: 42, name: "test" }, schema);
for (var i = 0; i < pmsg.length; i++) {
  var res = safeDecode(function (m) { return Proto.decode(m, schema); }, pmsg.subarray(0, i));
  if (res === "BAD") bad++;
}
if (bad) { __fail += 1; __out("FAIL proto fuzz violations " + bad); } else __pass++;
// unknown fields: hidden, preserved, re-emitted
var withUnknown = new Uint8Array([0x08, 0x01, 0xA2, 0x06, 0x01, 0x41]);
var decoded = Proto.decode(withUnknown, schema);
assert_eq(decoded.id, 1, "proto known field decoded");
assert_eq(JSON.stringify(Object.keys(decoded)), '["id"]', "proto unknown hidden");
var re = Proto.encode(decoded, schema);
var hexs2 = "";
for (var i = 0; i < re.length; i++) hexs2 += ("0" + re[i].toString(16)).slice(-2);
assert_eq(hexs2, "0801a2060141", "proto unknown re-emitted");
summary("serialize_fuzz");
""")
    return write_probe("serialize", "fuzz", "\n".join(emit))


def probe_clone():
    emit = ['import { structuredClone, ValueHash, MsgPackEncode, MsgPackDecode } from "dyna:serialize";']
    emit.append("""
// cycles and shared references survive with identity preserved
var doc = { x: 1 };
doc.self = doc;
var cl = structuredClone(doc);
assert_eq(cl.self === cl, true, "cycle identity");
assert_eq(cl.x, 1, "scalar copied");
var shared = { v: 1 };
var holder = { a: shared, b: shared };
var cl2 = structuredClone(holder);
assert_eq(cl2.a === cl2.b, true, "shared ref memo");
assert_ne(cl2.a === shared, true, "shared ref copied not aliased");
// typed arrays and buffers copied, never aliased
var u8 = new Uint8Array([1, 2, 3]);
var cu8 = structuredClone(u8);
cu8[0] = 9;
assert_eq(u8[0], 1, "typed array copied");
var ab = new ArrayBuffer(8);
var cab = structuredClone(ab);
assert_eq(cab.byteLength, 8, "arraybuffer copied");
assert_ne(cab === ab, true, "arraybuffer new object");
// functions refused
assert_throws(function () { structuredClone(function () {}); }, "TypeError", "fn refused");
// Date/Map clone as plain objects of own enumerable props (documented)
assert_eq(JSON.stringify(structuredClone(new Date(0))), "{}", "Date -> plain");
assert_eq(JSON.stringify(structuredClone(new Map([[1, 2]]))), "{}", "Map -> plain");
// plain data matrix
var vals = [null, true, false, 0, -1, 3.14, "", "text", [1, [2, [3]]], { a: { b: { c: 1 } } }];
for (var i = 0; i < vals.length; i++) {
  (function (v) {
    var c = structuredClone(v);
    var s1 = JSON.stringify(v), s2 = JSON.stringify(c);
    if (s1 !== s2) { __fail++; __out("FAIL clone deep " + s1 + " vs " + s2); } else __pass++;
  })(vals[i]);
}
// ValueHash sanity
assert_eq(ValueHash(42) === ValueHash(42), true, "ValueHash deterministic");
assert_ne(ValueHash(42), ValueHash(43), "ValueHash distinct ints");
assert_ne(ValueHash({ x: 1 }), ValueHash({ x: 2 }), "ValueHash distinct nested");
// msgpack round-trip of mixed object
var mixed = { i: 42, s: "str", f: 2.5, b: true, n: null, arr: [1, "two", 3.5], neg: -77 };
var rt = MsgPackDecode(MsgPackEncode(mixed));
assert_eq(JSON.stringify(rt), JSON.stringify(mixed), "msgpack mixed roundtrip");
// ALL 11 typed-array kinds round-trip their constructor (review F2)
var kinds = [
  ["Uint8Array", [5]], ["Int8Array", [-3]], ["Uint8ClampedArray", [301]],
  ["Int16Array", [-300]], ["Uint16Array", [7]], ["Int32Array", [-70000]],
  ["Uint32Array", [4294967290]], ["Float32Array", [0.5]], ["Float64Array", [1.25]],
];
for (var ki = 0; ki < kinds.length; ki++) {
  (function (k) {
    var a = new (globalThis[k[0]])(k[1]);
    var c = structuredClone(a);
    assert_eq(c.constructor.name, k[0], "clone kind " + k[0]);
    assert_eq(c[0], a[0], "clone kind value " + k[0]);
    assert_ne(c.buffer === a.buffer, true, "clone kind fresh buffer " + k[0]);
  })(kinds[ki]);
}
var b64a = new BigInt64Array([-5n]);
assert_eq(structuredClone(b64a).constructor.name, "BigInt64Array", "clone kind BigInt64Array");
assert_eq(structuredClone(b64a)[0], -5n, "clone kind BigInt64Array value");
var bu64a = new BigUint64Array([7n]);
assert_eq(structuredClone(bu64a)[0], 7n, "clone kind BigUint64Array value");
// shared references survive for typed arrays of every width (review F3)
var shared8 = new Uint8Array([1]);
var o8 = structuredClone({ a: shared8, b: shared8 });
assert_eq(o8.a === o8.b, true, "shared Uint8Array identity");
var shared64 = new Float64Array([2]);
var o64 = structuredClone({ a: shared64, b: shared64 });
assert_eq(o64.a === o64.b, true, "shared Float64Array identity");
var sharedI8 = new Int8Array([3]);
var oI8 = structuredClone({ a: sharedI8, b: sharedI8 });
assert_eq(oI8.a === oI8.b, true, "shared Int8Array identity");
var abShared = new ArrayBuffer(4);
var oAb = structuredClone({ a: abShared, b: abShared });
assert_eq(oAb.a === oAb.b, true, "shared ArrayBuffer identity");
assert_eq(oAb.a.byteLength, 4, "shared ArrayBuffer bytes");
summary("serialize_clone");
""")
    return write_probe("serialize", "clone", "\n".join(emit))


if __name__ == "__main__":
    print(probe_cbor())
    print(probe_msgpack())
    print(probe_fuzz())
    print(probe_clone())
