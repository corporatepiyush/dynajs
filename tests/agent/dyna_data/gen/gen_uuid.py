#!/usr/bin/env python3
"""gen_uuid.py — dyna:uuid probes.

Oracles:
  - python uuid module for v3/v5 name-based vectors (RFC 4122 algorithm)
  - RFC 4122 shape rules (version/variant bit masks) verified in-probe
  - python for parse/format of every accepted textual form
  - in-probe monotonicity + entropy-shape invariants for v4/v7/ULID/NanoID
"""
import os
import sys
import uuid as pyuuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe

NS_HEX = {
    "NAMESPACE_DNS": "6ba7b810-9dad-11d1-80b4-00c04fd430c8",
    "NAMESPACE_URL": "6ba7b811-9dad-11d1-80b4-00c04fd430c8",
    "NAMESPACE_OID": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
    "NAMESPACE_X500": "6ba7b814-9dad-11d1-80b4-00c04fd430c8",
}

def named_vectors():
    rows = []
    cases = [
        ("NAMESPACE_DNS", pyuuid.NAMESPACE_DNS, "example.com"),
        ("NAMESPACE_DNS", pyuuid.NAMESPACE_DNS, "python.org"),
        ("NAMESPACE_URL", pyuuid.NAMESPACE_URL, "http://example.com"),
        ("NAMESPACE_OID", pyuuid.NAMESPACE_OID, "1.2.3.4"),
        ("NAMESPACE_X500", pyuuid.NAMESPACE_X500, "cn=Test,ou=People"),
        ("NAMESPACE_DNS", pyuuid.NAMESPACE_DNS, ""),  # empty name
        ("NAMESPACE_DNS", pyuuid.NAMESPACE_DNS, "a" * 300),  # long name
    ]
    ns_custom = pyuuid.UUID('2e3a4e5e-1a1b-2c3d-4e5f-6a7b8c9d0e1f')
    cases.append(("custom", ns_custom, "dynatest"))
    out = []
    for tag, ns, name in cases:
        v3 = str(pyuuid.uuid3(ns, name))
        v5 = str(pyuuid.uuid5(ns, name))
        out.append((tag, ns, name, v3, v5))
    return out


def probe_named():
    rows = []
    for tag, ns, name, v3, v5 in named_vectors():
        if tag == "custom":
            ns_expr = '"2e3a4e5e-1a1b-2c3d-4e5f-6a7b8c9d0e1f"'
        else:
            ns_expr = tag
        rows.append('  ["%s", %s, %s, "%s", "%s"],'
                    % (tag, ns_expr, json_str(name), v3, v5))
    emit = ['import { v3, v5, NAMESPACE_DNS, NAMESPACE_URL, NAMESPACE_OID,',
            '         NAMESPACE_X500 } from "dyna:uuid";',
            'import { fromUtf8 } from "dyna:bytes";',
            "var CASES = [", *rows, "];",
            """
for (var i = 0; i < CASES.length; i++) {
  var c = CASES[i];
  assert_eq(v3(c[1], c[2]), c[3], "v3 " + c[0] + " " + JSON.stringify(c[2]).slice(0, 20));
  assert_eq(v5(c[1], c[2]), c[4], "v5 " + c[0] + " " + JSON.stringify(c[2]).slice(0, 20));
  // determinism
  assert_eq(v5(c[1], c[2]), v5(c[1], c[2]), "v5 deterministic " + c[0]);
}
// namespace constants are the RFC values (lowercase canonical)
assert_eq(NAMESPACE_DNS, "6ba7b810-9dad-11d1-80b4-00c04fd430c8", "DNS ns");
assert_eq(NAMESPACE_URL, "6ba7b811-9dad-11d1-80b4-00c04fd430c8", "URL ns");
assert_eq(NAMESPACE_OID, "6ba7b812-9dad-11d1-80b4-00c04fd430c8", "OID ns");
assert_eq(NAMESPACE_X500, "6ba7b814-9dad-11d1-80b4-00c04fd430c8", "X500 ns");
// custom namespace as raw 16 bytes (Uint8Array) must give the same UUID
var nsBytes = new Uint8Array([0x2e,0x3a,0x4e,0x5e,0x1a,0x1b,0x2c,0x3d,0x4e,0x5f,0x6a,0x7b,0x8c,0x9d,0x0e,0x1f]);
assert_eq(v5(nsBytes, "dynatest"), v5("2e3a4e5e-1a1b-2c3d-4e5f-6a7b8c9d0e1f", "dynatest"), "namespace bytes == string");
// name as bytes
var nameBytes = fromUtf8("dynatest");
assert_eq(v5(nsBytes, nameBytes), v5(nsBytes, "dynatest"), "name bytes == string");
// malformed namespaces refuse
assert_throws(function () { v5("not-a-uuid", "x"); }, "SyntaxError", "bad ns string");
assert_throws(function () { v5(new Uint8Array(15), "x"); }, "TypeError", "15-byte ns");
assert_throws(function () { v5(42, "x"); }, "TypeError", "number ns");
summary("uuid_named");
"""]
    return write_probe("uuid", "named", "\n".join(emit))


def json_str(s):
    out = []
    for ch in s:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append("\\u%04x" % ord(ch))
    return '"' + "".join(out) + '"'


def probe_shape():
    # RFC 4122/9562 shape + parse/format round-trips
    bad_forms = [
        '"len35"', '"12345678-1234-1234-1234-1234567890123"',
        '"1234567-1234-1234-1234-123456789012"', '"{12345678-1234-1234-1234-123456789012"',
        '"12345678-1234-1234-1234-12345678901g"', '"g2345678-1234-1234-1234-123456789012"',
        '"urn:uuid:12345678-1234-1234-1234-12345678901"', '"123456781234123412341234567890123"',
        '""', '"   "', '"12345678_1234-1234-1234-123456789012"',
    ]
    emit = ['import { v4, v7, parse, validate, version, variant, bytes, fromBytes,',
            '         NIL, MAX } from "dyna:uuid";']
    emit.append("var BAD = [" + ",".join(bad_forms) + "];")
    emit.append("""
// ---- v4 shape: 200 samples, version 4, RFC4122 variant, 36 chars
for (var i = 0; i < 200; i++) {
  var u = v4();
  assert_eq(u.length, 36, "v4 length");
  assert_eq(u[14], "4", "v4 version nibble");
  assert_true(validate(u), "v4 validates");
  assert_eq(version(u), 4, "v4 version()");
  assert_eq(variant(u), "RFC4122", "v4 variant()");
  // hex-only characters
  assert_eq(u.replace(/[0-9a-f-]/g, "").length, 0, "v4 charset");
  // distinctness (collision probability ~0 for 200 draws)
  assert_ne(u, v4(), "v4 distinct");
}
// ---- v7 shape + monotonicity: 5000 ids must be non-decreasing; many strictly increase
var prev = "", strictly = 0;
for (var i = 0; i < 5000; i++) {
  var u = v7();
  if (i > 0) {
    if (u > prev) strictly++;
    else assert_true(u >= prev, "v7 monotonic: " + prev + " then " + u);
    assert_eq(u.slice(0, 12) === prev.slice(0, 12) ? version(u) : 7, 7, "v7 version stable");
  }
  assert_eq(u[14], "7", "v7 version nibble");
  assert_eq(variant(u), "RFC4122", "v7 variant");
  prev = u;
}
assert_true(strictly > 4000, "v7 strictly increasing mostly (got " + strictly + "/4999)");
// v7 timestamp ~ now (millisecond field vs Date.now())
var u7 = v7();
var ts = parseInt(u7.slice(0, 8), 16) * 65536 + parseInt(u7.slice(9, 13), 16);
assert_true(Math.abs(ts - Date.now()) < 10000, "v7 timestamp near now: " + ts + " vs " + Date.now());
// ---- parse round-trips over all accepted forms
var u = v4();
var upper = u.toUpperCase();
assert_eq(parse(u), u, "parse canonical");
assert_eq(parse(upper), u, "parse uppercase");
assert_eq(parse("urn:uuid:" + u), u, "parse urn");
assert_eq(parse("{" + u + "}"), u, "parse braces");
assert_eq(parse(u.replace(/-/g, "")), u, "parse raw hex");
assert_eq(parse("URN:UUID:" + u), u, "parse URN upper");
// constants
assert_eq(NIL, "00000000-0000-0000-0000-000000000000", "NIL constant");
assert_eq(MAX, "ffffffff-ffff-ffff-ffff-ffffffffffff", "MAX constant");
assert_eq(parse(NIL), NIL, "parse NIL");
assert_eq(parse(MAX), MAX, "parse MAX");
assert_eq(version(NIL), 0, "NIL version 0");
assert_eq(variant(NIL), "NCS", "NIL variant NCS");
assert_eq(variant(MAX), "Future", "MAX variant Future");
// malformed forms throw (SyntaxError) for parse; validate returns false
for (var i = 0; i < BAD.length; i++) {
  (function (b) {
    assert_throws(function () { parse(b); }, "SyntaxError", "parse rejects " + b);
    assert_eq(validate(b), false, "validate false for " + b);
  })(BAD[i]);
}
// validate non-strings are false, never throw
assert_eq(validate(42), false, "validate number");
assert_eq(validate(null), false, "validate null");
assert_eq(validate(undefined), false, "validate undefined");
assert_eq(validate({}), false, "validate object");
assert_eq(validate(v4()), true, "validate v4 true");
// version/variant also throw on malformed
assert_throws(function () { version("nope"); }, "SyntaxError", "version malformed");
assert_throws(function () { variant("nope"); }, "SyntaxError", "variant malformed");
// ---- bytes round-trip
var b = bytes(u);
assert_eq(b.length, 16, "bytes length");
assert_eq(b instanceof Uint8Array, true, "bytes is Uint8Array");
assert_eq(fromBytes(b), u, "fromBytes(parse bytes) == u");
assert_eq(fromBytes(bytes(MAX)), MAX, "fromBytes(MAX bytes)");
assert_throws(function () { fromBytes(new Uint8Array(15)); }, "RangeError", "fromBytes 15 bytes");
assert_throws(function () { fromBytes(new Uint8Array(17)); }, "RangeError", "fromBytes 17 bytes");
// bytes() returns a fresh copy: mutating it must not corrupt the module state
var b2 = bytes(u);
b2[0] = 0xff;
assert_eq(bytes(u)[0], parseInt(u.slice(0, 2), 16), "bytes() fresh copy");
// explicit byte layout: NIL is 16 zero bytes
var nb = bytes(NIL);
var allz = true;
for (var i = 0; i < 16; i++) if (nb[i] !== 0) allz = false;
assert_eq(allz, true, "NIL bytes all zero");
summary("uuid_shape");
""")
    return write_probe("uuid", "shape", "\n".join(emit))


def probe_ulid_nanoid():
    # ULID: fixed-timestamp round-trips (baked via Crockford decode), shape,
    # lexicographic time order; NanoID alphabet/shape.
    fixed = [0, 1, 1234567890123, 2**48 - 1, 1700000000000]
    emit = ['import { ULID, ULIDTime, NanoID, NanoIDAlphabet } from "dyna:uuid";',
            'import { fromUtf8 } from "dyna:bytes";']
    emit.append("var FIXED = [" + ",".join(str(m) for m in fixed) + "];")
    emit.append("var EXPECT = [" + ",".join(str(m) for m in fixed) + "];")
    NANOID_ALPHA = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLFGQZbfghjklqvwyzrict"
    emit3 = ["""
for (var i = 0; i < FIXED.length; i++) {
  var ul = ULID(FIXED[i]);
  assert_eq(ul.length, 26, "ULID length ms=" + FIXED[i]);
  assert_eq(ULIDTime(ul), EXPECT[i], "ULIDTime round-trip ms=" + FIXED[i]);
  // 26 Crockford chars, uppercase-only alphabet
  assert_eq(ul.replace(/[0-9ABCDEFGHJKMNPQRSTVWXYZ]/g, "").length, 0, "ULID charset");
}
// explicit timestamp lexicographic ordering
assert_true(ULID(1000) < ULID(2000), "ULID time orders");
assert_true(ULID(0) < ULID(281474976710655), "ULID epoch < max");
// clock-based ULID has a plausible timestamp
var ulNow = ULID();
assert_true(Math.abs(ULIDTime(ulNow) - Date.now()) < 10000, "ULID now-ish");
// 48-bit guard
assert_throws(function () { ULID(281474976710656); }, "RangeError", "ULID > 48 bits");
assert_throws(function () { ULID(-1); }, "RangeError", "ULID negative");
// ULIDTime refusals
assert_throws(function () { ULIDTime("short"); }, "TypeError", "ULIDTime short");
assert_throws(function () { ULIDTime("0000000000" + "0".repeat(16) + "!"); }, "TypeError", "ULIDTime bad symbol");
assert_throws(function () { ULIDTime("I0000000000000000000000000"); }, "TypeError", "ULIDTime 'I' refused");
// lowercase accepted (documented impl-defined leniency): same timestamp
var lower = ULID(1700000000000).toLowerCase();
assert_eq(ULIDTime(lower), 1700000000000, "ULIDTime accepts lowercase");
// ---- NanoID
for (var i = 0; i < 100; i++) {
  var id = NanoID();
  assert_eq(id.length, 21, "NanoID default length");
  assert_eq(id.replace(/[A-Za-z0-9_~-]/g, "").length, 0, "NanoID default charset");
}
for (var i = 0; i < 50; i++) {
  var id8 = NanoID(8);
  assert_eq(id8.length, 8, "NanoID(8) length");
}
assert_throws(function () { NanoID(0); }, "RangeError", "NanoID(0)");
assert_throws(function () { NanoID(4097); }, "RangeError", "NanoID(4097)");
assert_throws(function () { NanoID(2.5); }, "RangeError", "NanoID fractional");
assert_eq(NanoID(4096).length, 4096, "NanoID(4096) ok");
// custom alphabet
var ALPHA = "ABCDEF";
for (var i = 0; i < 50; i++) {
  var id = NanoIDAlphabet(ALPHA, 12);
  assert_eq(id.length, 12, "alphabet id length");
  assert_eq(id.replace(/[ABCDEF]/g, "").length, 0, "alphabet id charset");
}
assert_throws(function () { NanoIDAlphabet("A", 8); }, "RangeError", "1-symbol alphabet");
assert_throws(function () { NanoIDAlphabet("AB\\u00e9", 8); }, "TypeError", "non-ASCII alphabet");
assert_eq(NanoIDAlphabet("01", 64).replace(/[01]/g, "").length, 0, "binary alphabet");
// both generators never repeat identity across draws (probabilistic)
assert_ne(NanoID(), NanoID(), "NanoID distinct");
summary("uuid_ulid_nanoid");
"""]
    return write_probe("uuid", "ulid_nanoid", "\n".join(emit + emit3))


if __name__ == "__main__":
    print(probe_named())
    print(probe_shape())
    print(probe_ulid_nanoid())
