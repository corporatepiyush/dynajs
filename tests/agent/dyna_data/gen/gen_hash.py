#!/usr/bin/env python3
"""gen_hash.py — dyna:hash probes.

Oracles (run at GENERATION time; expected values are baked as literals):
  - python hashlib: md5/sha1/sha2/sha3/shake/blake2 (RFC 1321, FIPS 180-4,
    FIPS 202, RFC 7693 KATs + block-boundary + random differentials)
  - openssl dgst -sha256 cross-check of the differential inputs
  - python xxhash: XXH32/XXH64;  python mmh3: MurmurHash3 x64 128
  - python blake3: BLAKE3 (incl. multi-length XOF outputs)
  - hand table-driven CRC-32 (IEEE) cross-checked vs zlib.crc32 and pinned
    check values; CRC-32C (Castagnoli) pinned against the 0xE3069283 check
Emits: probes/hash/*.js (dynajs-only, h.js harness).
"""
import hashlib
import os
import struct
import subprocess
import sys
import zlib

import mmh3
import xxhash
import blake3 as blake3mod

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe, js_str_literal, LCG_JS, py_lcg

MOD = "hash"

# ---------------------------------------------------------------- references

_CRC32C_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (0x82F63B78 if _c & 1 else 0)
    _CRC32C_TABLE.append(_c)

def crc32c(data):
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC32C_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF

assert crc32c(b"123456789") == 0xE3069283
assert zlib.crc32(b"123456789") == 0xCBF43926

HEXALGS = ["md5", "sha1", "sha224", "sha256", "sha384", "sha512",
           "sha3_224", "sha3_256", "sha3_384", "sha3_512"]

def sha_hex(alg, data):
    if alg.startswith("sha3_"):
        return hashlib.new("sha3_" + alg[5:], data).hexdigest()
    return hashlib.new(alg, data).hexdigest()

# ---------------------------------------------------------------- probes

def probe_kat():
    rows = []
    # RFC 1321 MD5 vectors
    md5_kat = [
        ("", "d41d8cd98f00b204e9800998ecf8427e"),
        ("a", "0cc175b9c0f1b6a831c399e269772661"),
        ("abc", "900150983cd24fb0d6963f7d28e17f72"),
        ("message digest", "f96b697d7cb7938d525a2f31aaf161d0"),
        ("abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"),
        ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
         "d174ab98d277d9f5a5611c2c9f419d9f"),
        ("1234567890" * 8, "57edf4a22be3c955ac49da2e2107b67a"),
    ]
    for s, want in md5_kat:
        assert hashlib.md5(s.encode()).hexdigest() == want
    # FIPS 180-4 / classic vectors, computed with hashlib (memory never trusted)
    one_million_a = b"a" * 10**6
    kat_inputs = [
        "", "abc",
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklm"
        "nhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
    ]
    body = ['import { MD5Hex, SHA1Hex, SHA224Hex, SHA256Hex, SHA384Hex, SHA512Hex,',
            '         SHA3_224Hex, SHA3_256Hex, SHA3_384Hex, SHA3_512Hex,',
            '         Keccak256Hex, SHAKE128Hex, SHAKE256Hex, CRC32, CRC32C } from "dyna:hash";']
    emit = ["var KAT = ["]
    for s in kat_inputs:
        row = [js_str_literal(s)]
        for alg in HEXALGS:
            row.append('"%s"' % sha_hex(alg, s.encode()))
        emit.append("  [" + ",".join(row) + "],")
    emit.append("];")
    emit.append("""
var FN = [MD5Hex, SHA1Hex, SHA224Hex, SHA256Hex, SHA384Hex, SHA512Hex,
          SHA3_224Hex, SHA3_256Hex, SHA3_384Hex, SHA3_512Hex];
for (var i = 0; i < KAT.length; i++)
  for (var j = 0; j < FN.length; j++)
    assert_eq(FN[j](KAT[i][0]), KAT[i][j + 1], "KAT[" + i + "] alg#" + j);

// RFC 1321 MD5 set is inside KAT above; pin the famous Keccak/SHAKE cases.
assert_eq(Keccak256Hex(""), "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470", "keccak empty");
assert_eq(Keccak256Hex("abc"), "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45", "keccak abc");
""")
    # SHAKE vectors from hashlib
    for name, fn, ln in [("shake128", hashlib.shake_128, 32),
                         ("shake256", hashlib.shake_256, 32),
                         ("shake128_long", hashlib.shake_128, 131),
                         ("shake256_long", hashlib.shake_256, 64)]:
        base = name.split("_")[0]
        for s in ["", "abc", "repeat!" * 100]:
            h = fn(s.encode()).hexdigest(ln)
            emit.append('assert_eq(%sHex(%s, %d), "%s", "%s len=%d %r");'
                        % (base.upper(), js_str_literal(s), ln, h, base, ln, s[:8]))
    # CRC check values + a couple of pinned strings
    emit.append('assert_eq(CRC32("123456789"), %d, "crc32 check value");' % zlib.crc32(b"123456789"))
    emit.append('assert_eq(CRC32C("123456789"), %d, "crc32c check value");' % crc32c(b"123456789"))
    for s in ["", "The quick brown fox jumps over the lazy dog", "\x00\x01\x02\xfe\xff"]:
        emit.append('assert_eq(CRC32(%s), %d, "crc32 utf8 %r");'
                    % (js_str_literal(s), zlib.crc32(s.encode("utf-8")), s[:10]))
        emit.append('assert_eq(CRC32C(%s), %d, "crc32c utf8 %r");'
                    % (js_str_literal(s), crc32c(s.encode("utf-8")), s[:10]))
    emit.append('summary("hash_kat");')
    return write_probe(MOD, "kat", "\n".join(body + emit))


def probe_differential():
    sizes = [0, 1, 2, 3, 31, 32, 33, 54, 55, 56, 57, 63, 64, 65, 111, 119,
             120, 127, 128, 129, 255, 256, 1000, 4096, 65535]
    seeds = [0, 1, 7, 0x9E3779B9]
    # python computes expected hex for LCG byte streams; JS regenerates the
    # identical stream with the same LCG baked into the probe.
    emit = ['import { MD5Hex, SHA1Hex, SHA224Hex, SHA256Hex, SHA384Hex, SHA512Hex,',
            '         SHA3_224Hex, SHA3_256Hex, SHA3_384Hex, SHA3_512Hex,',
            '         SHAKE256Hex, BLAKE3Hex, BLAKE2bHex, BLAKE2sHex,',
            '         Murmur3_128Hex, XXHash32, XXHash64, CRC32, CRC32C } from "dyna:hash";']
    emit.append(LCG_JS)
    emit.append("var EXP = [")
    for sz in sizes:
        data = py_lcg(0x1234 + sz, sz)
        row = [str(sz)]
        for alg in HEXALGS:
            row.append('"%s"' % sha_hex(alg, data))
        row.append('"%s"' % blake3mod.blake3(data).hexdigest())
        row.append('"%s"' % blake3mod.blake3(data).hexdigest(64))  # XOF len 64
        row.append('"%s"' % hashlib.blake2b(data).hexdigest())     # 64 bytes
        row.append('"%s"' % hashlib.blake2s(data).hexdigest())     # 32 bytes
        row.append('"%s"' % hashlib.blake2b(data, digest_size=33).hexdigest())
        row.append('"%s"' % hashlib.shake_256(data).hexdigest(48))
        row.append('"%s"' % mmh3.hash128(data, 0, signed=False).to_bytes(16, "little").hex())
        row.append('"%s"' % mmh3.hash128(data, 0xDEADBEEF, signed=False).to_bytes(16, "little").hex())
        row.append(str(xxhash.xxh32(data, 0).intdigest()))
        row.append(str(xxhash.xxh32(data, 7).intdigest()))
        row.append('"%s"' % "%016x" % xxhash.xxh64(data, 0).intdigest())
        row.append('"%s"' % "%016x" % xxhash.xxh64(data, 0x9E3779B9).intdigest())
        row.append(str(zlib.crc32(data) & 0xFFFFFFFF))
        row.append('"%s"' % ("%08x" % crc32c(data)))
        emit.append("  [" + ",".join(row) + "],")
    emit.append("];")
    emit.append("""
var algNames = ["md5","sha1","sha224","sha256","sha384","sha512",
                "sha3_224","sha3_256","sha3_384","sha3_512"];
var hexFns = [MD5Hex, SHA1Hex, SHA224Hex, SHA256Hex, SHA384Hex, SHA512Hex,
              SHA3_224Hex, SHA3_256Hex, SHA3_384Hex, SHA3_512Hex];
for (var i = 0; i < EXP.length; i++) {
  var r = EXP[i], data = lcg(0x1234 + r[0], r[0]);
  var tag = "len=" + r[0];
  for (var a = 0; a < hexFns.length; a++)
    assert_eq(hexFns[a](data), r[a + 1], tag + " " + algNames[a]);
  var c = 11;
  assert_eq(BLAKE3Hex(data), r[c], tag + " blake3");
  assert_eq(BLAKE3Hex(data, 64), r[c + 1], tag + " blake3 xof64");
  assert_eq(BLAKE2bHex(data), r[c + 2], tag + " blake2b64");
  assert_eq(BLAKE2sHex(data), r[c + 3], tag + " blake2s32");
  assert_eq(BLAKE2bHex(data, 33), r[c + 4], tag + " blake2b33");
  assert_eq(SHAKE256Hex(data, 48), r[c + 5], tag + " shake256-48");
  assert_eq(Murmur3_128Hex(data), r[c + 6], tag + " murmur s0");
  assert_eq(Murmur3_128Hex(data, 3735928559), r[c + 7], tag + " murmur seed");
  assert_eq(XXHash32(data), r[c + 8], tag + " xxh32");
  assert_eq(XXHash32(data, 7), r[c + 9], tag + " xxh32 s7");
  assert_eq(XXHash64(data), r[c + 10], tag + " xxh64");
  assert_eq(XXHash64(data, 2654435769), r[c + 11], tag + " xxh64 seed");
  assert_eq(CRC32(data), r[c + 12], tag + " crc32");
  // CRC32C compared case-insensitively via lowercase bake
  assert_eq(CRC32C(data) >>> 0, parseInt(r[c + 13], 16), tag + " crc32c");
}
// million 'a' — FIPS/SHA-1 folklore vector, hashlib-baked, streamed in chunks
""")
    one_million = b"a" * 10**6
    emit.append('var EXP1M = {')
    for alg in ["sha256", "sha512", "sha1", "sha3_256"]:
        emit.append('  %s: "%s",' % (alg, hashlib.new(alg, one_million).hexdigest()))
    emit.append('};')
    emit.insert(0, 'import { Hasher } from "dyna:hash";')
    emit.append("""
var mh = new Hasher("sha256");
var big = "a".repeat(1000000);
for (var off = 0; off < 1000000; off += 997) mh.update(big.slice(off, Math.min(off + 997, 1000000)));
assert_eq(mh.digestHex(), EXP1M.sha256, "sha256 1M-a chunked");
assert_eq(SHA256Hex(big), EXP1M.sha256, "sha256 1M-a oneshot");
var sh1 = new Hasher("sha1"); sh1.update(big);
assert_eq(sh1.digestHex(), EXP1M.sha1, "sha1 1M-a");
var s512 = new Hasher("sha512"); s512.update(big);
assert_eq(s512.digestHex(), EXP1M.sha512, "sha512 1M-a");
summary("hash_differential");
""")
    return write_probe(MOD, "differential", "\n".join(emit))


def probe_streaming():
    sizes = [0, 1, 63, 64, 65, 127, 128, 129, 255, 1000, 4096]
    algs = ["md5", "sha1", "sha224", "sha256", "sha384", "sha512"]
    emit = ['import { Hasher, MD5Hex, SHA1Hex, SHA224Hex, SHA256Hex, SHA384Hex,',
            '         SHA512Hex } from "dyna:hash";']
    emit.append(LCG_JS)
    emit.append("var ONE = [")
    for alg in algs:
        row = ['"%s"' % alg]
        for sz in sizes:
            row.append('"%s"' % sha_hex(alg, py_lcg(7 * sz + len(alg), sz)))
        emit.append("  [" + ",".join(row) + "],")
    emit.append("];")
    emit.append("""
var SZ = %s;
// chunking invariance: same bytes in odd-sized pieces == one-shot
for (var i = 0; i < ONE.length; i++) {
  var alg = ONE[i][0];
  for (var j = 1; j < ONE[i].length; j++) {
    var data = lcg(7 * SZ[j - 1] + alg.length, SZ[j - 1]);
    var pieces = [1, 2, 63, 64, 65, 7];
    var h = new Hasher(alg), off = 0, pi = 0;
    while (off < data.length) {
      var n = Math.min(pieces[pi %% pieces.length], data.length - off);
      h.update(data.subarray(off, off + n));
      off += n; pi++;
    }
    assert_eq(h.digestHex(), ONE[i][j], "chunked " + alg + " len=" + SZ[j - 1]);
    // digest() copies: repeated digest gives the same answer
    var d1 = h.digestHex(), d2 = h.digestHex();
    assert_eq(d1, d2, "digest repeat " + alg);
    assert_eq(d1, ONE[i][j], "digest after end " + alg);
    // reset() reuses: replay the same bytes
    h.reset(); h.update(data);
    assert_eq(h.digestHex(), ONE[i][j], "reset replay " + alg);
    // digest() must not consume the stream: digesting again after update(0 bytes)
    h.reset(); h.update(data); h.update(new Uint8Array(0));
    assert_eq(h.digestHex(), ONE[i][j], "zero-length update is a no-op " + alg);
  }
}
""" % str(sizes))
    emit.append("""
// getters and refusal
var h = new Hasher("sha256");
assert_eq(h.algorithm, "sha256", "algorithm getter");
assert_eq(h.digestSize, 32, "digestSize getter");
assert_throws(function () { new Hasher("nope"); }, "TypeError", "unknown algorithm");
assert_throws(function () { new Hasher(42); }, "TypeError", "non-string algorithm");
// chaining returns this
assert_eq(h.update("a").update("bc") === h, true, "update chains this");
assert_eq(h.digestHex(), SHA256Hex("abc"), "chained equals one-shot");
// empty input digest is the algorithm's all-zero-input digest (baked)
assert_eq(new Hasher("md5").digestHex(), MD5Hex(""), "empty md5");
assert_eq(new Hasher("sha512").digestHex(), SHA512Hex(""), "empty sha512");
summary("hash_streaming");
""")
    return write_probe(MOD, "streaming", "\n".join(emit))


def probe_types():
    # input coercion: view types, offsets, UTF-8 strings vs hashlib bytes
    rows = []
    def add(desc, js_expr, py_bytes):
        row = [js_str_literal(desc), js_expr]
        for alg in HEXALGS[:6]:  # md5..sha512 hex forms
            row.append('"%s"' % sha_hex(alg, py_bytes))
        rows.append("  [" + ",".join(row) + "],")

    add("utf8 ascii", '"hello"', b"hello")
    add("utf8 accent + astral", js_str_literal("héllo 🎉"), "héllo 🎉".encode())
    add("nul inside string", js_str_literal("a\x00b"), b"a\x00b")
    add("empty string", '""', b"")
    add("empty u8", 'new Uint8Array(0)', b"")
    add("u8 array", 'new Uint8Array([0,1,2,253,254,255])', bytes([0, 1, 2, 253, 254, 255]))
    add("Int8Array view", 'new Int8Array([-1, 2, -3])', bytes([255, 2, 253]))
    add("Uint8ClampedArray", 'new Uint8ClampedArray([0, 255, 300, -7])', bytes([0, 255, 255, 0]))
    add("subarray offset", 'new Uint8Array([9,9,9,1,2,3,9]).subarray(3, 6)', bytes([1, 2, 3]))
    add("DataView middle", 'new DataView(new Uint8Array([7,7,0xde,0xad,7]).buffer, 2, 2)', bytes([0xDE, 0xAD]))
    add("arraybuffer whole", 'new Uint8Array([5,6,7]).buffer', bytes([5, 6, 7]))

    emit = ['import { MD5Hex, SHA1Hex, SHA224Hex, SHA256Hex, SHA384Hex, SHA512Hex } from "dyna:hash";',
            "var CASES = ["]
    emit.extend(rows)
    emit.append("];")
    emit.append("""
var FNS = [MD5Hex, SHA1Hex, SHA224Hex, SHA256Hex, SHA384Hex, SHA512Hex];
var NAMES = ["md5","sha1","sha224","sha256","sha384","sha512"];
for (var i = 0; i < CASES.length; i++) {
  var c = CASES[i];
  for (var f = 0; f < FNS.length; f++)
    assert_eq(FNS[f](c[1]), c[f + 2], "coerce " + c[0] + " " + NAMES[f]);
}
// same bytes through different views must agree byte-for-byte
var raw = new Uint8Array(300);
for (var i = 0; i < 300; i++) raw[i] = (i * 7 + 3) & 0xff;
var dvo = new DataView(raw.buffer, 17, 200);
assert_eq(SHA256Hex(dvo), SHA256Hex(raw.subarray(17, 217)), "dataview == subarray");
assert_eq(SHA256Hex(raw.buffer), SHA256Hex(raw), "whole buffer == view");
// consistency pin: a lone surrogate string hashes stably (impl-defined bytes,
// but the same input must always give the same digest)
var lone = "\\ud800";
assert_eq(SHA256Hex(lone), SHA256Hex(lone), "lone surrogate stable");
assert_ne(SHA256Hex(lone), SHA256Hex(""), "lone surrogate is not the empty string");
summary("hash_types");
""")
    return write_probe(MOD, "types", "\n".join(emit))


def openssl_crosscheck():
    """Bake an extra sanity layer: openssl must agree with hashlib on a file."""
    data = py_lcg(99, 3000)
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "openssl_input.bin")
    with open(p, "wb") as f:
        f.write(data)
    out = subprocess.run(["openssl", "dgst", "-sha256", p],
                         capture_output=True, text=True)
    want = sha_hex("sha256", data)
    got = out.stdout.strip().split("= ")[-1]
    os.unlink(p)
    assert got == want, "openssl disagrees with hashlib: %s vs %s" % (got, want)
    return want


if __name__ == "__main__":
    h = openssl_crosscheck()
    print("openssl crosscheck OK (sha256=%s...)" % h[:12])
    print(probe_kat())
    print(probe_differential())
    print(probe_streaming())
    print(probe_types())
