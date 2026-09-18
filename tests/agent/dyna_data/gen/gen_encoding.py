#!/usr/bin/env python3
"""gen_encoding.py — dyna:encoding probes.

Oracles:
  - python base64/binascii: RFC 4648 base64/base64url/base32/base32hex/hex
  - python base64.a85encode(adobe=False): ascii85 (incl. z shorthand rules)
  - python reference implementations: Base58 (bitcoin alphabet), Base58Check
    (double SHA-256 checksum), BaseX (arbitrary alphabet), LEB128 varints
  - hashlib.sha256 for Base58Check
"""
import base64
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe, LCG_JS, py_lcg, js_str_literal

B58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

def b58encode(data):
    n = int.from_bytes(data, "big")
    out = ""
    while n > 0:
        n, r = divmod(n, 58)
        out = B58_ALPHABET[r] + out
    pad = 0
    for b in data:
        if b == 0:
            pad += 1
        else:
            break
    return "1" * pad + out

def b58decode(s):
    n = 0
    for c in s:
        n = n * 58 + B58_ALPHABET.index(c)
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    pad = 0
    for c in s:
        if c == "1":
            pad += 1
        else:
            break
    return b"\x00" * pad + raw

def b58check(data):
    return b58encode(data + hashlib.sha256(hashlib.sha256(data).digest()).digest()[:4])

def leb128_u(x):
    out = bytearray()
    while x >= 0x80:
        out.append((x & 0x7F) | 0x80)
        x >>= 7
    out.append(x)
    return bytes(out)

def leb128_i(x):
    # zigzag
    z = (x << 1) ^ (x >> 63) if x < 0 else (x << 1)
    return leb128_u(z & (2**64 - 1))

def probe_rfc4648():
    sizes = list(range(0, 11)) + [31, 32, 33, 63, 64, 65, 127, 128, 1000]
    rows = []
    for sz in sizes:
        data = py_lcg(11 + sz, sz)
        rows.append('  [%s, [%s], %s, %s, %s, %s, %s, %s],' % (
            sz, ",".join(str(b) for b in data),
            js_str_literal(base64.b16encode(data).decode().lower()),
            js_str_literal(base64.b64encode(data).decode()),
            js_str_literal(base64.urlsafe_b64encode(data).decode().rstrip("=")),
            js_str_literal(base64.b32encode(data).decode()),
            js_str_literal(base64.b32hexencode(data).decode()),
            js_str_literal(base64.a85encode(data, adobe=False).decode())))
    emit = ['import { HexEncode, Base64Encode, Base64URLEncode, Base32Encode,',
            '         Base32HexEncode, Base85Encode, HexDecode, Base64Decode,',
            '         Base64URLDecode, Base32Decode, Base32HexDecode, Base85Decode } from "dyna:encoding";',
            LCG_JS.rstrip(),
            "var EXP = [\n" + "\n".join(rows) + "\n];"]
    emit.append("""
for (var i = 0; i < EXP.length; i++) {
  var r = EXP[i];
  var data = new Uint8Array(r[1]);
  assert_eq(HexEncode(data), r[2], "hex len=" + r[0]);
  assert_eq(Base64Encode(data), r[3], "b64 len=" + r[0]);
  assert_eq(Base64URLEncode(data), r[4], "b64url len=" + r[0]);
  assert_eq(Base32Encode(data), r[5], "b32 len=" + r[0]);
  assert_eq(Base32HexEncode(data), r[6], "b32hex len=" + r[0]);
  assert_eq(Base85Encode(data), r[7], "a85 len=" + r[0]);
  // decode side round-trips
  assert_eq(JSON.stringify(Array.from(HexDecode(r[2]))), JSON.stringify(r[1]), "hexdec len=" + r[0]);
  assert_eq(JSON.stringify(Array.from(Base64Decode(r[3]))), JSON.stringify(r[1]), "b64dec len=" + r[0]);
  assert_eq(JSON.stringify(Array.from(Base64URLDecode(r[4]))), JSON.stringify(r[1]), "b64urldec len=" + r[0]);
  assert_eq(JSON.stringify(Array.from(Base32Decode(r[5]))), JSON.stringify(r[1]), "b32dec len=" + r[0]);
  assert_eq(JSON.stringify(Array.from(Base32HexDecode(r[6]))), JSON.stringify(r[1]), "b32hexdec len=" + r[0]);
  assert_eq(JSON.stringify(Array.from(Base85Decode(r[7]))), JSON.stringify(r[1]), "a85dec len=" + r[0]);
}
// RFC 4648 classic test vectors ("", "f", "fo", "foo", "foob", ...)
var RFC = ["", "f", "fo", "foo", "foob", "fooba", "foobar"];
var B64V = ["", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"];
var B32V = ["", "MY======", "MZXQ====", "MZXW6===", "MZXW6YQ=", "MZXW6YTB", "MZXW6YTBOI======"];
var B32HV = ["", "CO======", "CPNG====", "CPNMU===", "CPNMUOG=", "CPNMUOJ1", "CPNMUOJ1E8======"];
var B16V = ["", "66", "666f", "666f6f", "666f6f62", "666f6f6261", "666f6f626172"];
for (var i = 0; i < RFC.length; i++) {
  assert_eq(Base64Encode(RFC[i]), B64V[i], "rfc b64 '" + RFC[i] + "'");
  assert_eq(Base32Encode(RFC[i]), B32V[i], "rfc b32 '" + RFC[i] + "'");
  assert_eq(Base32HexEncode(RFC[i]), B32HV[i], "rfc b32hex '" + RFC[i] + "'");
  assert_eq(HexEncode(RFC[i]), B16V[i], "rfc hex '" + RFC[i] + "'");
}
// RFC 4648 §5 base64url: +/ -> -_ and NO padding
assert_eq(Base64URLEncode(new Uint8Array([0xFB, 0xFF])), "-_8", "b64url -_8 (API.md example)");
assert_eq(Base64URLEncode(RFC[6]), "Zm9vYmFy", "b64url foobar");
// decoder refusals
assert_throws(function () { HexDecode("DED"); }, "SyntaxError", "hex odd");
assert_throws(function () { HexDecode("zz"); }, "SyntaxError", "hex digit");
assert_throws(function () { Base64Decode("AQ ID"); }, "SyntaxError", "b64 whitespace");
assert_throws(function () { Base64Decode("AQI"); }, "SyntaxError", "b64 unpadded");
assert_throws(function () { Base64Decode("AQ*D"); }, "SyntaxError", "b64 digit");
assert_throws(function () { Base64URLDecode("AQ+D"); }, "SyntaxError", "b64url plus");
assert_throws(function () { Base64URLDecode("A"); }, "SyntaxError", "b64url 4k+1");
assert_throws(function () { Base32Decode("mfraz"); }, "SyntaxError", "b32 lowercase");
// uppercase hex accepted on decode
assert_eq(JSON.stringify(Array.from(HexDecode("DEAD"))), JSON.stringify([222, 173]), "hex upper decode");
// Base85 specific: z shorthand + whitespace + overflow
assert_eq(JSON.stringify(Array.from(Base85Decode("z"))), JSON.stringify([0, 0, 0, 0]), "a85 z group");
assert_throws(function () { Base85Decode("Az"); }, "SyntaxError", "a85 z mid-group");
assert_throws(function () { Base85Decode('s8W-"'); }, "SyntaxError", "a85 overflow");
assert_eq(JSON.stringify(Array.from(Base85Decode("A Q I D"))), JSON.stringify([101, 86, 215]), "a85 whitespace");
// inputs beyond byte views: strings encode as UTF-8
assert_eq(HexEncode("dé"), "64c3a9", "hex of utf8 string");
summary("encoding_rfc4648");
""")
    return write_probe("encoding", "rfc4648", "\n".join(emit))


def probe_basex58():
    sizes = [0, 1, 2, 3, 4, 8, 15, 16, 20, 32, 64, 255, 256, 300]
    rows = []
    for sz in sizes:
        data = py_lcg(77 + sz, sz)
        chk = b58check(data)
        rows.append('  [[%s], %s, %s],' % (
            ",".join(str(b) for b in data),
            js_str_literal(b58encode(data)), js_str_literal(chk)))
    # BaseX round-trips: hex alphabet + binary alphabet over the same data
    rows16 = []
    for sz in [0, 1, 5, 32]:
        data = py_lcg(999 + sz, sz)
        n = int.from_bytes(data, "big") if data else 0
        enc = ""
        if data:
            # leading zeros -> alphabet[0]
            pad = 0
            for b in data:
                if b == 0:
                    pad += 1
                else:
                    break
            v = int.from_bytes(data, "big")
            enc = ""
            while v:
                v, r = divmod(v, 16)
                enc = "0123456789abcdef"[r] + enc
            enc = "0" * pad + enc
        rows16.append('  [[%s], %s],' % (",".join(str(b) for b in data), js_str_literal(enc)))
    emit = ['import { Base58Encode, Base58Decode, Base58CheckEncode,',
            '         Base58CheckDecode, BaseXEncode, BaseXDecode } from "dyna:encoding";',
            "var B58 = [\n" + "\n".join(rows) + "\n];",
            "var B16 = [\n" + "\n".join(rows16) + "\n];"]
    emit.append("""
for (var i = 0; i < B58.length; i++) {
  var r = B58[i];
  var data = new Uint8Array(r[0]);
  assert_eq(Base58Encode(data), r[1], "b58 len=" + r[0].length);
  assert_eq(JSON.stringify(Array.from(Base58Decode(r[1]))), JSON.stringify(r[0]), "b58dec len=" + r[0].length);
  assert_eq(Base58CheckEncode(data), r[2], "b58chk len=" + r[0].length);
  assert_eq(JSON.stringify(Array.from(Base58CheckDecode(r[2]))), JSON.stringify(r[0]), "b58chkdec len=" + r[0].length);
}
for (var i = 0; i < B16.length; i++) {
  var r = B16[i];
  var data = new Uint8Array(r[0]);
  assert_eq(BaseXEncode(data, "0123456789abcdef"), r[1], "basex16 len=" + r[0].length);
  assert_eq(JSON.stringify(Array.from(BaseXDecode(r[1], "0123456789abcdef"))), JSON.stringify(r[0]), "basex16dec len=" + r[0].length);
}
// all-zero bytes become leading alphabet[0]
assert_eq(Base58Encode(new Uint8Array(4)), "1111", "b58 leading zeros");
assert_eq(BaseXEncode(new Uint8Array(3), "01"), "000", "basex leading zeros");
assert_eq(JSON.stringify(Array.from(Base58Decode("11"))), JSON.stringify([0, 0]), "b58 decode zeros");
// known bitcoin genesis-style vector
assert_eq(Base58CheckEncode(new Uint8Array([0])), "1Wh4bh", "b58check single 0");
// refusals
assert_throws(function () { Base58Decode("0OIl"); }, "SyntaxError", "b58 foreign chars");
assert_throws(function () { Base58CheckDecode("a"); }, "SyntaxError", "b58chk too short");
assert_throws(function () { Base58CheckDecode("2g"), 0; }, "SyntaxError", "b58chk bad checksum");
assert_throws(function () { Base58Encode(new Uint8Array(4097)); }, "RangeError", "b58 input cap");
assert_throws(function () { BaseXEncode(new Uint8Array(1), "aab"); }, "RangeError", "basex dup alphabet");
assert_throws(function () { BaseXDecode("zz", "0123456789abcdef"); }, "SyntaxError", "basex foreign char");
// 4096 exactly is allowed
assert_eq(Base58Encode(new Uint8Array(4096)).length > 0, true, "b58 4096 ok");
summary("encoding_basex58");
""")
    return write_probe("encoding", "basex58", "\n".join(emit))


def probe_varint():
    # LEB128 reference baked for boundary values and random values
    vals_u = [0, 1, 127, 128, 129, 16383, 16384, 2**20, 2**28 - 1, 2**28,
              2**31, 2**32, 2**32 + 1, 2**53 - 1]
    vals_bi = [2**53, 2**53 + 1, 2**63, 2**64 - 1]
    vals_i = [0, -1, 1, -2, 63, -64, 64, 2**31 - 1, -(2**31), 2**53 - 1,
              -(2**53 - 1)]
    vals_i_bi = [-(2**63), 2**63 - 1]
    rows = []
    for v in vals_u:
        rows.append('  [%d, [%s], "number"],' % (v, ",".join(str(b) for b in leb128_u(v))))
    for v in vals_bi:
        rows.append('  ["%dn", [%s], "bigint"],' % (v, ",".join(str(b) for b in leb128_u(v))))
    rows_i = []
    for v in vals_i:
        rows_i.append('  [%d, [%s], "number"],' % (v, ",".join(str(b) for b in leb128_i(v))))
    for v in vals_i_bi:
        rows_i.append('  ["%dn", [%s], "bigint"],' % (v, ",".join(str(b) for b in leb128_i(v))))
    emit = ['import { PutUvarint, Uvarint, PutVarint, Varint } from "dyna:encoding";',
            "var UV = [\n" + "\n".join(rows) + "\n];",
            "var IV = [\n" + "\n".join(rows_i) + "\n];"]
    emit.append("""
for (var i = 0; i < UV.length; i++) {
  var r = UV[i];
  var v = eval(r[0]);
  assert_eq(JSON.stringify(Array.from(PutUvarint(v))), JSON.stringify(r[1]), "putu " + r[0]);
  var dec = Uvarint(new Uint8Array(r[1]));
  assert_eq(dec[1], r[1].length, "uvarint nbytes " + r[0]);
  if (r[2] === "bigint") assert_eq(dec[0].toString(), v.toString(), "uvarint big value " + r[0]);
  else assert_eq(dec[0], v, "uvarint value " + r[0]);
}
for (var i = 0; i < IV.length; i++) {
  var r = IV[i];
  var v = eval(r[0]);
  assert_eq(JSON.stringify(Array.from(PutVarint(v))), JSON.stringify(r[1]), "putv " + r[0]);
  var dec = Varint(new Uint8Array(r[1]));
  assert_eq(dec[1], r[1].length, "varint nbytes " + r[0]);
  if (r[2] === "bigint") assert_eq(dec[0].toString(), v.toString(), "varint big value " + r[0]);
  else assert_eq(dec[0], v, "varint value " + r[0]);
}
// truncated buffers decode to [0, 0]
assert_eq(JSON.stringify(Uvarint(new Uint8Array([0xAC]))), "[0,0]", "uvarint truncated");
assert_eq(JSON.stringify(Uvarint(new Uint8Array(0))), "[0,0]", "uvarint empty");
// trailing bytes after a complete value: bytesRead counts only consumed
assert_eq(JSON.stringify(Uvarint(new Uint8Array([0xAC, 0x02, 0xFF]))), "[300,2]", "uvarint trailing");
// refusals
assert_throws(function () { PutUvarint(1.5); }, "RangeError", "uvarint frac");
assert_throws(function () { PutUvarint(-1); }, "RangeError", "uvarint neg");
assert_throws(function () { PutUvarint(9007199254740992); }, "RangeError", "uvarint 2^53 number");
assert_throws(function () { PutUvarint(18446744073709551616n); }, "RangeError", "uvarint 2^64 bigint");
assert_throws(function () { PutUvarint(-1n); }, "RangeError", "uvarint -1n");
assert_throws(function () { PutVarint(1.5); }, "RangeError", "varint frac");
// ToBigInt64 identity: 2^63 aliases -2^63 for signed varint (impl convention)
var aliased = PutVarint(9223372036854775808n);
assert_eq(JSON.stringify(Array.from(aliased)), JSON.stringify(Array.from(PutVarint(-9223372036854775808n))), "varint 2^63 alias");
// multi-byte value with maximum length: 10 bytes for 2^64-1
assert_eq(PutUvarint(18446744073709551615n).length, 10, "uvarint max bytes");
summary("encoding_varint");
""")
    return write_probe("encoding", "varint", "\n".join(emit))


if __name__ == "__main__":
    print(probe_rfc4648())
    print(probe_basex58())
    print(probe_varint())
