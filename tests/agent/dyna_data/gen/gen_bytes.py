#!/usr/bin/env python3
"""gen_bytes.py — dyna:bytes probes.

Oracles:
  - python for UTF-8 validity matrices (overlongs, surrogates, truncations),
    transcoder round-trips, cp1252/latin1 charset tables, code point counting
  - in-probe DataView mirror for the 18 fixed-width read/write shapes
  - in-probe randomized model checks (memmove copy, search conventions)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe

def probe_utf8():
    # (name, bytes, isValidUtf8, toUtf8-replacement-string-as-codepoints)
    cases = [
        ("ascii", [0x68, 0x69], True),
        ("valid 2byte", [0xC3, 0xA9], True),          # é
        ("valid 3byte", [0xED, 0x95, 0x9C], True),    # 한
        ("valid 4byte", [0xF0, 0x9F, 0x8E, 0x89], True),  # 🎉
        ("NUL", [0x00], True),
        ("DEL 0x7F", [0x7F], True),
        ("overlong NUL C0 80", [0xC0, 0x80], False),
        ("overlong E0 80 80", [0xE0, 0x80, 0x80], False),
        ("overlong F0 80 80 80", [0xF0, 0x80, 0x80, 0x80], False),
        ("surrogate ED A0 80", [0xED, 0xA0, 0x80], False),
        ("beyond F7", [0xF5, 0x80, 0x80, 0x80], False),
        ("F4 90 80 80", [0xF4, 0x90, 0x80, 0x80], False),
        ("lone cont 80", [0x80], False),
        ("lone cont BF", [0xBF], False),
        ("trunc C3", [0xC3], False),
        ("trunc E2 82", [0xE2, 0x82], False),
        ("trunc F0 9F 8E", [0xF0, 0x9F, 0x8E], False),
        ("cont after ascii", [0x41, 0x80], False),
        ("FE", [0xFE], False),
        ("FF", [0xFF], False),
    ]
    rows = []
    for name, bs, valid in cases:
        rows.append('  ["%s", [%s], %s],' % (name, ",".join(str(b) for b in bs),
                                             "true" if valid else "false"))
    emit = ['import { isValidUtf8, toUtf8, fromUtf8, countUtf8 } from "dyna:bytes";',
            "var CASES = [\n" + "\n".join(rows) + "\n];"]
    emit.append("""
for (var i = 0; i < CASES.length; i++) {
  var c = CASES[i];
  var u8 = new Uint8Array(c[1]);
  assert_eq(isValidUtf8(u8), c[2], "isValidUtf8 " + c[0]);
  assert_eq(isValidUtf8(String.fromCharCode.apply(null, u8) === undefined ? true : isValidUtf8(Array.from(u8).map(function (b) { return b < 128 ? String.fromCharCode(b) : ""; }).join(""))), c[2] === undefined ? true : isValidUtf8(u8), "sanity " + c[0]);
}
""")
    # the weird sanity assert above is junk; replace the block wholesale
    emit.pop()
    emit.append("""
for (var i = 0; i < CASES.length; i++) {
  var c = CASES[i];
  var u8 = new Uint8Array(c[1]);
  assert_eq(isValidUtf8(u8), c[2], "isValidUtf8 " + c[0]);
}
// replacement behavior: invalid sequences decode to U+FFFD per byte deemed
// maximal-subpart invalid; pinned against the WF-replacement tables
var REPL = [
  [[0xC3, 0x28], [0xFFFD, 0x28]],          // truncated 2-byte -> FFFD
  [[0x41, 0xFF, 0x41], [0x41, 0xFFFD, 0x41]],
  [[0xE2, 0x82, 0x28], [0xFFFD, 0x28]],    // 2 of 3 bytes valid prefix -> 1 FFFD then 0x28
  [[0xF0, 0x9F, 0x8E, 0x89], [0xD83C, 0xDF89]],   // valid emoji = surrogate pair of code units
  [[0x80], [0xFFFD]],
];
for (var i = 0; i < REPL.length; i++) {
  var r = toUtf8(new Uint8Array(REPL[i][0]));
  var got = [];
  for (var k = 0; k < r.length; k++) got.push(r.charCodeAt(k));
  assert_eq(JSON.stringify(got), JSON.stringify(REPL[i][1]), "toUtf8 replacement " + i);
}
// fromUtf8/toUtf8 round-trip on valid text incl astral
var s = "héllo 🎉 한";
assert_eq(toUtf8(fromUtf8(s)), s, "utf8 roundtrip");
assert_eq(countUtf8(fromUtf8(s)), 9, "countUtf8 code points");
assert_eq(countUtf8(s), 9, "countUtf8 string");
summary("bytes_utf8");
""")
    return write_probe("bytes", "utf8", "\n".join(emit))


def probe_rw():
    # DataView mirror: for each of the 18 shapes, write via Bytes, read via
    # DataView, and vice versa, at several offsets; then bounds errors.
    emit = ['import { Bytes } from "dyna:bytes";']
    emit.append("""
var shapes = [
  // [bytes-name, dataview-name, width]
  ["Uint8", "Uint8", 1], ["Int8", "Int8", 1],
  ["Uint16", "Uint16", 2], ["Int16", "Int16", 2],
  ["Uint32", "Uint32", 4], ["Int32", "Int32", 4],
  ["BigUint64", "BigUint64", 8], ["BigInt64", "BigInt64", 8],
  ["Float", "Float32", 4], ["Double", "Float64", 8],
];
var bufs = [8, 16, 33];
var mism = 0;
for (var bi = 0; bi < bufs.length; bi++) {
  var len = bufs[bi];
  var ours = new Bytes(new Uint8Array(len));
  var mirror = new DataView(new ArrayBuffer(len));
  for (var si = 0; si < shapes.length; si++) {
    var name = shapes[si][0], dvName = shapes[si][1], width = shapes[si][2];
    for (var off = 0; off + width <= len; off += 3) {
      var val;
      if (name.indexOf("Big") === 0) val = (BigInt(0x9E3779B9) * BigInt(off + si + 1)) & 0xFFFFFFFFFFFFFFFFn;
      else if (name === "Double") val = Math.SQRT2 * 1e300 / (off + 1);
      else if (name === "Float") val = Math.SQRT2 / (off + 1);
      else val = (0x9E3779B9 ^ (off * 2654435761)) % (width === 1 ? 0x100 : 0x100000000);
      var isBE = si % 2 === 0;
      var sfx = width === 1 ? "" : (isBE ? "BE" : "LE");
      var w = "write" + name + sfx;
      var rd = "read" + name + sfx;
      var dW = "set" + dvName;   // DataView endianness is the littleEndian flag
      var dR = "get" + dvName;
      var wv = (name === "BigInt64") ? BigInt.asIntN(64, val) : val;
      // write the SAME value through both, then compare reads
      var next = ours[w](off, wv);
      assert_eq(next, off + width, w + " returns next offset");
      mirror[dW](off, wv, width === 1 ? undefined : !isBE);
      var a1 = ours[rd](off);
      var b1 = mirror[dR](off, width === 1 ? undefined : !isBE);
      var same;
      if (width === 8) same = a1.toString() === (name === "BigInt64" ? BigInt.asIntN(64, b1).toString() : b1.toString());
      else if (name === "Float") same = Math.abs(a1 - b1) < 1e-6 * Math.max(1, Math.abs(b1));
      else same = a1 === b1;
      if (!same) { mism++; if (mism < 5) __out("FAIL rw " + w + "@" + off + " bytes=" + a1 + " dv=" + b1); }
    }
  }
}
if (mism) { __fail++; __out("FAIL rw mismatches: " + mism); } else __pass++;
// sign handling
var sb = new Bytes(new Uint8Array(2));
sb.writeInt16LE(0, -2);
assert_eq(sb.readUint16LE(0), 0xFFFE, "int16 -2 LE bits");
assert_eq(sb.readInt16LE(0), -2, "readInt16LE -2");
var bb = new Bytes(new Uint8Array(8));
bb.writeBigInt64BE(0, -2n);
assert_eq(bb.readBigUint64BE(0).toString(16), "fffffffffffffffe", "bigint64 -2 BE bits");
assert_eq(bb.readBigInt64BE(0).toString(), "-2", "bigint64 roundtrip");
// float bit patterns
var fb = new Bytes(new Uint8Array(4));
fb.writeFloatLE(0, 1.5);
assert_eq(fb.readUint32LE(0), 0x3FC00000, "float32 1.5 bits");
// bounds errors
var b8 = new Bytes(new Uint8Array(8));
assert_throws(function () { b8.readUint32LE(5); }, "RangeError", "read past end");
assert_throws(function () { b8.readUint8(8); }, "RangeError", "read at end");
assert_throws(function () { b8.writeDoubleLE(1, 1.5); }, "RangeError", "write past end");
assert_throws(function () { b8.readUint8(-1); }, "RangeError", "negative offset");
// alloc
assert_eq(Bytes.alloc(0).length, 0, "alloc 0");
assert_eq(Bytes.alloc(100).array.every(function (v) { return v === 0; }), true, "alloc zero-filled");
assert_throws(function () { Bytes.alloc(2147483649); }, "RangeError", "alloc > 2^31");
// isBytes / isAscii / isValidUtf8 flags
assert_eq(Bytes.isBytes(new Bytes("x")), true, "isBytes true");
assert_eq(Bytes.isBytes(new Uint8Array(1)), false, "isBytes false on view");
assert_eq(new Bytes("hello").isAscii, true, "isAscii");
assert_eq(new Bytes("héllo").isAscii, false, "isAscii false");
assert_eq(new Bytes("héllo").isValidUtf8, true, "isValidUtf8 flag");
assert_eq(new Bytes(new Uint8Array([0xFF])).isValidUtf8, false, "isValidUtf8 flag false");
summary("bytes_rw");
""")
    return write_probe("bytes", "rw", "\n".join(emit))


def probe_search_copy():
    emit = ['import { Bytes, bytesOf, compare, equal, indexOf, lastIndexOf,',
            '         contains, count, concat, copy, fill } from "dyna:bytes";']
    emit.append(LCG_JS if False else """
function lcg(seed, n) {
  var a = seed >>> 0, out = new Uint8Array(n);
  for (var i = 0; i < n; i++) { a = (Math.imul(a, 1103515245) + 12345) >>> 0; out[i] = (a >>> 16) & 0xff; }
  return out;
}
""")
    emit.append("""
// randomized search matrix vs a naive JS oracle
function naiveIndexOf(hay, needle) {
  if (needle.length === 0) return 0;
  outer: for (var i = 0; i + needle.length <= hay.length; i++) {
    for (var j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
    return i;
  }
  return -1;
}
function naiveLastIndexOf(hay, needle) {
  if (needle.length === 0) return hay.length;
  outer: for (var i = hay.length - needle.length; i >= 0; i--) {
    for (var j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
    return i;
  }
  return -1;
}
function naiveCount(hay, needle) {
  if (needle.length === 0) return hay.length + 1;
  var n = 0, i = 0;
  while (i <= hay.length - needle.length) {
    var at = naiveIndexOf(hay.subarray(i), needle);
    if (at === -1) break;
    n++; i += at + needle.length;
  }
  return n;
}
var bad = 0;
for (var t = 0; t < 300; t++) {
  var hay = lcg(1000 + t * 7, (t % 40) + 1);
  var nl = t % 5;                       // needle length 0..4
  var needle = hay.subarray(t % Math.max(1, hay.length - nl + 1), (t % Math.max(1, hay.length - nl + 1)) + nl);
  var H = new Bytes(hay.buffer.length === hay.length ? hay : hay);
  var args = nl === 0 ? new Uint8Array(0) : needle;
  var a = H.indexOf(args), b = naiveIndexOf(hay, args);
  var c = H.lastIndexOf(args), d = naiveLastIndexOf(hay, args);
  var e = H.count(args), f = naiveCount(hay, args);
  var g = H.includes(args), h = naiveIndexOf(hay, args) !== -1;
  if (a !== b || c !== d || e !== f || g !== h) {
    bad++;
    if (bad < 5) __out("FAIL search t=" + t + " idx " + a + "/" + b + " last " + c + "/" + d + " cnt " + e + "/" + f + " inc " + g + "/" + h);
  }
}
if (bad) { __fail++; __out("FAIL search violations " + bad); } else __pass++;
// byte-value needles
function fromBytes(a) { return new Uint8Array(a); }
var hb2 = new Bytes(fromBytes([3, 1, 4, 1, 5, 9, 2, 6]));
assert_eq(hb2.indexOf(9), 5, "indexOf byte");
assert_eq(hb2.lastIndexOf(1), 3, "lastIndexOf byte");
assert_eq(hb2.count(1), 2, "count byte");
assert_eq(hb2.indexOf(7), -1, "indexOf missing");
assert_eq(new Bytes("abcabc").count(new Uint8Array(0)), 7, "empty needle count len+1");
assert_eq(new Bytes("abc").indexOf(new Uint8Array(0)), 0, "empty needle idx 0");
assert_eq(new Bytes("abc").lastIndexOf(new Uint8Array(0)), 3, "empty needle last len");
// indexOfAny
assert_eq(new Bytes("hello world, hi").indexOfAny(new Uint8Array([44, 33])), 11, "indexOfAny comma");
assert_eq(new Bytes("abc").indexOfAny(new Uint8Array([122])), -1, "indexOfAny miss");
// compare/equal free functions over mixed views
assert_eq(compare(new Uint8Array([1, 2]), new Uint8Array([1, 3])), -1, "compare lt");
assert_eq(compare(new Uint8Array([1, 2]), new Uint8Array([1, 2])), 0, "compare eq");
assert_eq(compare(new Uint8Array([1, 2]), new Uint8Array([1])), 1, "compare prefix longer");
assert_eq(equal(new Uint8Array([9]), new Uint8Array([9])), true, "equal true");
assert_eq(equal(new Uint8Array([9]), new Uint8Array([9, 9])), false, "equal len");
// concat
var cc = concat([new Uint8Array([1, 2]), new Uint8Array(0), new Uint8Array([3])]);
assert_eq(JSON.stringify(Array.from(cc)), "[1,2,3]", "concat empties");
// copy overlap (memmove) vs a model
bad = 0;
for (var t2 = 0; t2 < 200; t2++) {
  var buf = new Uint8Array(16);
  var srcData = lcg(500 + t2, 16);
  buf.set(srcData);
  var dOff = t2 % 5, sOff = (t2 * 3) % 5, clen = (t2 % 10) + 1;
  var model = new Uint8Array(buf);
  var gotN = copy(buf, buf, dOff, sOff, clen);
  var tmp = new Uint8Array(model);
  for (var k2 = 0; k2 < clen; k2++) tmp[dOff + k2] = model[sOff + k2];
  var ok = gotN === clen;
  for (var k3 = 0; ok && k3 < 16; k3++) if (tmp[k3] !== buf[k3]) ok = false;
  if (!ok) { bad++; if (bad < 4) __out("FAIL memmove t=" + t2 + " d=" + dOff + " s=" + sOff + " n=" + clen); }
}
if (bad) { __fail++; __out("FAIL memmove violations " + bad); } else __pass++;
assert_throws(function () { copy(new Uint8Array(2), new Uint8Array(4), 0, 0, 10); }, "RangeError", "copy len oob");
// fill free function: low 8 bits, returns buf
var fd = new Uint8Array(4);
assert_eq(fill(fd, 0x1FF, 1, 3) === fd, true, "fill returns buf");
assert_eq(JSON.stringify(Array.from(fd)), "[0,255,255,0]", "fill low8 range");
// Bytes.slice views alias the owner
var owner = new Bytes("hello world");
var mid = owner.slice(6, 11);
assert_eq(mid.toString(), "world", "slice toString");
mid.fill(42, 0, 5);
assert_eq(owner.indexOf(42), 6, "slice aliases owner");
var neg = new Bytes("hello").slice(-3);
assert_eq(neg.toString(), "llo", "slice negative");
assert_eq(new Bytes("hello").slice(4, 2).length, 0, "slice end<start empty");
assert_eq(new Bytes("hello").slice(3, 100).length, 2, "slice clamped");
// bytesOf aliases across widths, writes visible
var u16 = new Uint16Array([0x4142, 0x4344]);
var alias = bytesOf(u16);
assert_eq(alias.length, 4, "bytesOf u16 length");
alias[0] = 0x58;
assert_eq(u16[0].toString(16), "4158", "bytesOf alias writes visible");
var dv = new DataView(new ArrayBuffer(4));
dv.setUint32(0, 0xDEADBEEF, true);
assert_eq(bytesOf(dv)[0], 0xEF, "bytesOf DataView LE");
summary("bytes_search_copy");
""")
    return write_probe("bytes", "search_copy", "\n".join(emit))


def probe_transcode():
    # cp1252 table via python; embed a compact map for 0x80..0x9F
    import codecs
    hi = []
    for b in range(0x80, 0xA0):
        try:
            ch = bytes([b]).decode("cp1252")
            hi.append(str(ord(ch)))
        except UnicodeDecodeError:
            hi.append("0xFFFD")
    latin_pairs = []
    for b in [0x20, 0x7E, 0x7F, 0x80, 0x9F, 0xA0, 0xE9, 0xFF]:
        latin_pairs.append((b, ord(bytes([b]).decode("latin1"))))
    emit = ['import { decode, encode, encodingExists, encodings,',
            '         latin1ToUtf8, utf8ToLatin1, utf8ToUtf16, utf16ToUtf8,',
            '         isValidUtf16, countUtf16, Text, fromUtf8, toUtf8 } from "dyna:bytes";']
    emit.append("var CP1252_HI = [" + ",".join(hi) + "];")
    emit.append("""
// windows-1252 high table (0x80..0x9F) vs the WhatWG/python table
for (var i = 0; i < CP1252_HI.length; i++) {
  var dec = decode(new Uint8Array([0x80 + i]), "windows-1252");
  var cp = dec.charCodeAt(0);
  var want = CP1252_HI[i] === "65533" ? "\\uFFFD" : String.fromCharCode(CP1252_HI[i]);
  assert_eq(cp, CP1252_HI[i] === "65533" ? 0xFFFD : CP1252_HI[i], "cp1252 0x" + (0x80 + i).toString(16));
}
// latin-1 is the identity map
for (var b = 0; b < 256; b += 17) {
  var d = decode(new Uint8Array([b]), "latin1");
  assert_eq(d.charCodeAt(0), b, "latin1 identity " + b);
  var e = encode(String.fromCharCode(b), "latin1");
  assert_eq(e[0], b, "latin1 encode " + b);
}
// label matching: case-insensitive, trimmed
assert_eq(decode(new Uint8Array([0xE9]), "LaTin1").charCodeAt(0), 0xE9, "label case");
assert_eq(decode(new Uint8Array([0xE9]), "  latin1  ").charCodeAt(0), 0xE9, "label trim");
assert_eq(encodingExists("UTF-8"), true, "utf-8 exists");
assert_eq(encodingExists(" utf-8 "), true, "utf-8 padded exists");
assert_eq(encodingExists("us-ascii"), true, "us-ascii exists");
assert_eq(encodingExists("gbk"), false, "gbk not built");
assert_eq(encodingExists("shift_jis"), false, "shift_jis not built");
assert_eq(encodingExists("big5"), false, "big5 not built");
assert_eq(encodingExists("euc-jp"), false, "euc-jp not built");
assert_eq(encodingExists("euc-kr"), false, "euc-kr not built");
assert_throws(function () { decode(new Uint8Array([1]), "nope"); }, "RangeError", "unknown label");
assert_throws(function () { encode(42, "latin1"); }, "TypeError", "encode non-string");
// us-ascii maps every high byte to U+FFFD
assert_eq(decode(new Uint8Array([0xE9]), "us-ascii").charCodeAt(0), 0xFFFD, "us-ascii high byte FFFD");
// unencodable -> '?'
assert_eq(encode("\\u20AC", "latin1")[0], 63, "latin1 encode ? substitution");
// transcoders
assert_eq(JSON.stringify(Array.from(latin1ToUtf8(new Uint8Array([0xE9])))), "[195,169]", "latin1ToUtf8");
assert_eq(utf8ToLatin1(fromUtf8("é"))[0], 0xE9, "utf8ToLatin1");
assert_throws(function () { utf8ToLatin1(new Uint8Array([0xC3, 0x28])); }, "RangeError", "utf8ToLatin1 invalid");
assert_throws(function () { utf8ToLatin1(fromUtf8("🎉")); }, "RangeError", "utf8ToLatin1 >FF");
assert_eq(JSON.stringify(Array.from(utf8ToUtf16("a"))), "[97,0]", "utf8ToUtf16 LE");
assert_eq(utf8ToUtf16(fromUtf8("é")).length, 2, "utf8ToUtf16 é = one UTF-16 unit x2 bytes");
assert_throws(function () { utf16ToUtf8(new Uint8Array([1, 2, 3])); }, "RangeError", "utf16ToUtf8 odd");
assert_throws(function () { utf16ToUtf8(new Uint8Array([0x00, 0xD8])); }, "RangeError", "utf16ToUtf8 lone hi");
assert_eq(utf16ToUtf8(utf8ToUtf16(fromUtf8("héllo 🎉"))).length, fromUtf8("héllo 🎉").length, "utf16 roundtrip len");
assert_eq(isValidUtf16(new Uint8Array([0x3D, 0xD8, 0x00, 0xDE])), true, "isValidUtf16 pair");  // surrogate pair U+1F43D? 0xD83D 0xDE00 = 🐀
assert_eq(isValidUtf16(new Uint8Array([0x00, 0xD8])), false, "isValidUtf16 lone hi");
assert_eq(isValidUtf16(new Uint8Array([0x3D, 0xD8])), false, "isValidUtf16 hi without lo");
assert_eq(isValidUtf16(new Uint8Array([1, 2, 3])), false, "isValidUtf16 odd len");
assert_eq(countUtf16(new Uint8Array([0x3D, 0xD8, 0x00, 0xDE, 65, 0])), 2, "countUtf16 pair+1");
// Text
var t1 = new Text("café");
assert_eq(t1.isWide, false, "Text not wide (é < 0x100)");
assert_eq(new Text("caf\\uD83D\\uDE00").isWide, true, "Text wide (astral)");
assert_eq(new Text("héllo").isValidUtf8(), true, "Text isValidUtf8");
assert_eq(t1.value, "café", "Text value");
assert_eq(t1.countUtf8(), 4, "Text countUtf8");
assert_eq(t1.countUtf16(), 4, "Text countUtf16");
assert_eq(JSON.stringify(Array.from(t1.toUtf8())), JSON.stringify(Array.from(fromUtf8("café"))), "Text toUtf8");
assert_eq(t1.toString(), "café", "Text toString");
var tb = t1.toBytes();
assert_eq(typeof tb === "object" && tb !== null, true, "Text toBytes object");
assert_eq(tb.length, 5, "Text toBytes UTF-8 length (caf\u00e9 = 5 bytes)");
summary("bytes_transcode");
""")
    return write_probe("bytes", "transcode", "\n".join(emit))


if __name__ == "__main__":
    print(probe_utf8())
    print(probe_rw())
    print(probe_search_copy())
    print(probe_transcode())
