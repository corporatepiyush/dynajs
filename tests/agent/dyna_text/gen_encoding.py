#!/usr/bin/env python3
"""gen_encoding.py — dyna:encoding black-box matrix generator.

Oracle: python3 stdlib (base64, binascii, hashlib) + hand implementations of
base58/basex/varint (the algorithms are trivial and pinned here), plus the
documented dyna:encoding contract for impl-defined corners (impl-defined rows
are marked in comments). Emits probes/encoding/*.js.
"""
import base64, binascii, hashlib, json, os, random
from gen_common import (HERE, PROBES, jslit, jsnum, fnv1a, norm_str, norm_num,
                        norm_bytes, norm_json, write_probe, blob_set, u8expr)

FAM = "encoding"
blobs = blob_set()
cases = None  # (file, list) accumulator


def emit(fname, cases_list, note=""):
    body = "\n".join(cases_list)
    write_probe("%s/%s.js" % (FAM, fname), [("E", "dyna:encoding")], body,
                "%s %s" % (fname, note))


# ---------------------------------------------------------------- base32/64/hex/a85 oracles
def py_b64(data):    return base64.b64encode(data).decode()
def py_b64url(data): return base64.urlsafe_b64encode(data).decode().rstrip("=")
def py_b32(data):    return base64.b32encode(data).decode()
def py_b32hex(data): return base64.b32hexencode(data).decode()
def py_hex(data):    return binascii.hexlify(data).decode()
def py_a85(data):    return base64.a85encode(data, adobe=False).decode()

# ---------------------------------------------------------------- base58/basex
B58A = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

def b58encode(data: bytes, alpha=B58A) -> str:
    base = len(alpha)
    n = int.from_bytes(data, "big") if data else 0
    s = ""
    while n:
        n, r = divmod(n, base)
        s = alpha[r] + s
    pad = 0
    for b in data:
        if b:
            break
        pad += 1
    return alpha[0] * pad + s

def b58decode(s: str, alpha=B58A) -> bytes:
    base = len(alpha)
    n = 0
    for c in s:
        n = n * base + alpha.index(c)
    pad = 0
    for c in s:
        if c != alpha[0]:
            break
        pad += 1
    body = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    return b"\x00" * pad + body

def b58check_encode(data: bytes) -> str:
    chk = hashlib.sha256(hashlib.sha256(data).digest()).digest()[:4]
    return b58encode(data + chk)

# ---------------------------------------------------------------- varints
MASK64 = (1 << 64) - 1

def put_uvarint(x: int) -> bytes:
    out = bytearray()
    while x >= 0x80:
        out.append((x & 0x7F) | 0x80)
        x >>= 7
    out.append(x)
    return bytes(out)

def zigzag(x: int) -> int:
    ux = (x << 1) & MASK64
    if x < 0:
        ux = (~ux) & MASK64
    return ux

def put_varint(x: int) -> bytes:
    return put_uvarint(zigzag(x))


MAXSAFE = (1 << 53) - 1

# ================================================================ e01 vectors
def gen_vectors():
    c = []
    # RFC 4648 sec.10 test vectors, computed by python (which follows them)
    for data in [b"", b"f", b"fo", b"foo", b"foob", b"fooba", b"foobar"]:
        tid = data.decode() or "empty"
        c.append('case_eq("b64e_%s", %s, function(){ return E.Base64Encode(%s); });'
                 % (tid or "e", json.dumps(norm_str(py_b64(data))), u8expr(data)))
        c.append('case_eq("b64d_%s", %s, function(){ return E.Base64Decode(%s); });'
                 % (tid or "e", json.dumps(norm_bytes(data)), jslit(py_b64(data))))
        c.append('case_eq("b64ue_%s", %s, function(){ return E.Base64URLEncode(%s); });'
                 % (tid or "e", json.dumps(norm_str(py_b64url(data))), u8expr(data)))
        if data:
            # the padded url form must also decode (impl: re-pads before decode)
            c.append('case_eq("b64ud_%s", %s, function(){ return E.Base64URLDecode(%s); });'
                     % (tid, json.dumps(norm_bytes(data)), jslit(py_b64url(data))))
            # the standard-padded form handed to the url decoder ALSO decodes
            c.append('case_eq("b64udP_%s", %s, function(){ return E.Base64URLDecode(%s); });'
                     % (tid, json.dumps(norm_bytes(data)), jslit(py_b64(data))))
        c.append('case_eq("b32e_%s", %s, function(){ return E.Base32Encode(%s); });'
                 % (tid or "e", json.dumps(norm_str(py_b32(data))), u8expr(data)))
        c.append('case_eq("b32d_%s", %s, function(){ return E.Base32Decode(%s); });'
                 % (tid or "e", json.dumps(norm_bytes(data)), jslit(py_b32(data))))
        c.append('case_eq("b32he_%s", %s, function(){ return E.Base32HexEncode(%s); });'
                 % (tid or "e", json.dumps(norm_str(py_b32hex(data))), u8expr(data)))
        if data:
            c.append('case_eq("b32hd_%s", %s, function(){ return E.Base32HexDecode(%s); });'
                     % (tid, json.dumps(norm_bytes(data)), jslit(py_b32hex(data))))
        c.append('case_eq("hexe_%s", %s, function(){ return E.HexEncode(%s); });'
                 % (tid or "e", json.dumps(norm_str(py_hex(data))), u8expr(data)))
        if data:
            c.append('case_eq("hexd_%s", %s, function(){ return E.HexDecode(%s); });'
                     % (tid, json.dumps(norm_bytes(data)), jslit(py_hex(data))))
            # uppercase hex decodes too
            c.append('case_eq("hexdU_%s", %s, function(){ return E.HexDecode(%s); });'
                     % (tid, json.dumps(norm_bytes(data)), jslit(py_hex(data).upper())))
        # ascii85 (documented: verified vs python a85encode/a85decode(adobe=False))
        c.append('case_eq("a85e_%s", %s, function(){ return E.Base85Encode(%s); });'
                 % (tid or "e", json.dumps(norm_str(py_a85(data))), u8expr(data)))
        if data:
            c.append('case_eq("a85d_%s", %s, function(){ return E.Base85Decode(%s); });'
                     % (tid, json.dumps(norm_bytes(data)), jslit(py_a85(data))))
    emit("e01_rfc_vectors", c, "RFC 4648 sec.10 + ascii85 vs python base64")
    return len([x for x in c if "case_eq" in x])


# ================================================================ e02-e05 matrixes
def gen_matrix():
    counts = {}
    # length sweep on pseudo-random bytes
    rnd = random.Random(20260911)
    lens = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 16, 32, 33, 100, 255, 256, 1024, 4095, 4096, 65535]
    sweep = [bytes(rnd.randrange(256) for _ in range(n)) for n in lens]

    def matrix(fname, enc_name, dec_name, pyenc, pydec, note):
        c = []
        for i, data in enumerate(sweep):
            enc = pyenc(data)
            c.append('case_eq("%s_e%d", %s, function(){ return E.%s(%s); });'
                     % (enc_name, i, json.dumps(norm_str(enc)), enc_name, u8expr(data)))
            c.append('case_eq("%s_d%d", %s, function(){ return E.%s(%s); });'
                     % (dec_name, i, json.dumps(norm_bytes(data)), dec_name, jslit(enc)))
        emit(fname, c, note)
        counts[fname] = len(c)

    matrix("e02_b64_matrix", "Base64Encode", "Base64Decode", py_b64, None,
           "base64 encode/decode sweep vs python base64")
    matrix("e03_b32_matrix", "Base32Encode", "Base32Decode", py_b32, None,
           "base32 sweep vs python base64.b32encode")
    matrix("e03b_b32hex_matrix", "Base32HexEncode", "Base32HexDecode", py_b32hex, None,
           "base32hex sweep vs python base64.b32hexencode")
    matrix("e05_hex_matrix", "HexEncode", "HexDecode", py_hex, None,
           "hex sweep vs python binascii")

    # a85 with content classes (zeros fold to 'z'; python matches)
    c = []
    classes = [("zeros4", bytes(4)), ("zeros8", bytes(8)), ("zeros3", bytes(3)),
               ("zeros1", bytes(1)), ("zero_rand", bytes(4) + b"\x01" + bytes(8)),
               ("rand256", blobs["rand255"]), ("utf8", blobs["utf8"]),
               ("rand1k", blobs["rand1k"])]
    for name, data in classes:
        enc = py_a85(data)
        c.append('case_eq("a85e_%s", %s, function(){ return E.Base85Encode(%s); });'
                 % (name, json.dumps(norm_str(enc)), u8expr(data)))
        c.append('case_eq("a85d_%s", %s, function(){ return E.Base85Decode(%s); });'
                 % (name, json.dumps(norm_bytes(data)), jslit(enc)))
    # line-wrapped / whitespace-embedded ascii85 decodes (documented: skips
    # space/tab/CR/LF/VT/FF, matching python a85decode ignorechars)
    body = py_a85(blobs["rand255"])
    wrapped = "\n".join(body[i:i + 20] for i in range(0, len(body), 20))
    c.append('case_eq("a85d_wrapped", %s, function(){ return E.Base85Decode(%s); });'
             % (json.dumps(norm_bytes(blobs["rand255"])), jslit(wrapped)))
    ws_mixed = " \t".join(body[i:i + 7] for i in range(0, len(body), 7)) + "\r\n\v\f "
    c.append('case_eq("a85d_ws", %s, function(){ return E.Base85Decode(%s); });'
             % (json.dumps(norm_bytes(blobs["rand255"])), jslit(ws_mixed)))
    emit("e04_b85_matrix", c, "ascii85 vs python base64.a85encode(adobe=False)")
    counts["e04_b85_matrix"] = len(c)

    # b64 content classes incl utf-8 strings-as-bytes input
    c = []
    str_inputs = ["", "f", "foobar", "héllo wörld", "日本語テキスト 🌍🚀", "a" * 3000]
    for i, s in enumerate(str_inputs):
        want = base64.b64encode(s.encode()).decode()
        c.append('case_eq("b64s_e%d", %s, function(){ return E.Base64Encode(%s); });'
                 % (i, json.dumps(norm_str(want)), jslit(s)))
        c.append('case_eq("b64s_d%d", %s, function(){ return E.Base64Decode(%s); });'
                 % (i, json.dumps(norm_bytes(s.encode())), jslit(want)))
    # view forms: ArrayBuffer, subarray with offset, DataView of a bigger buffer
    data = blobs["rand1k"][:256]
    enc = py_b64(data)
    big = blobs["rand1k"]
    c.append('case_eq("b64_view_ab", %s, function(){ var b=new ArrayBuffer(256);'
             ' new Uint8Array(b).set(%s); return E.Base64Encode(b); });'
             % (json.dumps(norm_str(enc)), u8expr(big[:256])))
    c.append('case_eq("b64_view_sub", %s, function(){ var u=%s;'
             ' return E.Base64Encode(u.subarray(16, 272)); });'
             % (json.dumps(norm_str(py_b64(big[16:272]))), u8expr(big)))
    c.append('case_eq("b64_view_i16_rejected", {t:"e",n:"TypeError"},'
             ' function(){ return E.Base64Encode(new Int16Array(4)); });')
    c.append('case_eq("b64_string_of_view", %s, function(){ return E.Base64Encode(%s); });'
             % (json.dumps(norm_str(py_b64(blobs["utf8"][:64]))), u8expr(blobs["utf8"][:64])))
    emit("e02b_b64_forms", c, "base64 input forms (string/view/ArrayBuffer)")
    counts["e02b_b64_forms"] = len(c)
    return counts


# ================================================================ e06 base58/basex
def gen_basex58():
    c = []
    rnd = random.Random(0xB58)
    b58_inputs = [b"", b"\x00", b"\x00\x00", b"\x00" * 8, b"\x01", b"\xff",
                  b"hello world", bytes(range(64)), b"\x00\x00\xde\xad\xbe\xef",
                  bytes(rnd.randrange(256) for _ in range(256))]
    for i, data in enumerate(b58_inputs):
        want = b58encode(data)
        c.append('case_eq("b58e_%d", %s, function(){ return E.Base58Encode(%s); });'
                 % (i, json.dumps(norm_str(want)), u8expr(data)))
        c.append('case_eq("b58d_%d", %s, function(){ return E.Base58Decode(%s); });'
                 % (i, json.dumps(norm_bytes(data)), jslit(want)))
    # base58check: double-SHA256 checksum
    chk_inputs = [b"", b"\x00", b"hello", b"bitcoin", bytes(range(32)),
                  bytes(rnd.randrange(256) for _ in range(64))]
    for i, data in enumerate(chk_inputs):
        want = b58check_encode(data)
        c.append('case_eq("b58cke_%d", %s, function(){ return E.Base58CheckEncode(%s); });'
                 % (i, json.dumps(norm_str(want)), u8expr(data)))
        c.append('case_eq("b58ckd_%d", %s, function(){ return E.Base58CheckDecode(%s); });'
                 % (i, json.dumps(norm_bytes(data)), jslit(want)))
    # caps: 4096 ok, 4097 RangeError (documented; cap is input BYTES on
    # encode, decoded TEXT length on decode)
    c.append('case_eq("b58_cap_ok", %s, function(){ return E.Base58Encode(%s).length > 0; });'
             % (json.dumps({"t": "b", "v": "true"}), u8expr(bytes(4096))))
    c.append('case_err("b58_cap_over", "RangeError", function(){ return E.Base58Encode(%s); });'
             % u8expr(bytes(4097)))
    c.append('case_eq("b58d_cap_ok", %s, function(){'
             ' var u = E.Base58Decode("1".repeat(4096)); return u.length === 4096; });'
             % json.dumps({"t": "b", "v": "true"}))
    c.append('case_err("b58d_cap_over", "RangeError", function(){ return E.Base58Decode("1".repeat(4097)); });')

    # BaseX: arbitrary alphabets, both directions
    alphabets = {"bin": "01", "dec": "0123456789", "b36": "0123456789abcdefghijklmnopqrstuvwxyz",
                 "b62": "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",
                 "weird": "!\"#$%&'()*+,-./"}
    for name, alpha in alphabets.items():
        for i, data in enumerate([b"", b"\x00", b"\x00\x07", b"\xff", b"hi there",
                                  bytes(range(32)), bytes(rnd.randrange(256) for _ in range(128))]):
            want = b58encode(data, alpha)
            c.append('case_eq("basex_%s_e%d", %s, function(){ return E.BaseXEncode(%s, %s); });'
                     % (name, i, json.dumps(norm_str(want)), u8expr(data), jslit(alpha)))
            c.append('case_eq("basex_%s_d%d", %s, function(){ return E.BaseXDecode(%s, %s); });'
                     % (name, i, json.dumps(norm_bytes(data)), jslit(want), jslit(alpha)))
    # alphabet validation (documented: 2..255 distinct chars; duplicates RangeError)
    c.append('case_err("basex_alpha1", "RangeError", function(){ return E.BaseXEncode(%s, "0"); });'
             % u8expr(b"a"))
    c.append('case_err("basex_alpha256", "RangeError",'
             ' function(){ var a=""; for (var i=0;i<256;i++) a+=String.fromCharCode(32+i);'
             ' return E.BaseXEncode(%s, a); });' % u8expr(b"a"))
    c.append('case_err("basex_dup", "RangeError", function(){ return E.BaseXDecode("00", "0110"); });')
    c.append('case_err("basexd_offalpha", "SyntaxError", function(){ return E.BaseXDecode("0g", "01"); });')
    emit("e06_basex58_matrix", c, "base58/base58check/basex vs pinned python impls")
    return len(c)


# ================================================================ e07 varint
def gen_varint():
    c = []
    uvals = [0, 1, 127, 128, 300, 16383, 16384, 65535, 65536,
             1 << 20, MAXSAFE - 1, MAXSAFE]
    for i, v in enumerate(uvals):
        enc = put_uvarint(v)
        c.append('case_eq("pute%d", %s, function(){ return E.PutUvarint(%s); });'
                 % (i, json.dumps(norm_bytes(enc)), jsnum(v)))
        c.append('case_eq("ue%d", %s, function(){ return E.Uvarint(%s); });'
                 % (i, json.dumps(norm_json([v, len(enc)])), u8expr(enc)))
    # BigInt beyond 2^53 (documented: BigInt in, Number/BigInt out by fit)
    big = 1 << 63
    enc = put_uvarint(big)
    c.append('case_eq("putbig", %s, function(){ return E.PutUvarint(%sn); });'
             % (json.dumps(norm_bytes(enc)), big))
    c.append('case_eq("ubig_v", %s, function(){ return String(E.Uvarint(%s)[0]); });'
             % (json.dumps(norm_str(str(big))), u8expr(enc)))
    c.append('case_eq("ubig_t", {t:"s",v:"bigint"}, function(){ return typeof E.Uvarint(%s)[0]; });'
             % u8expr(enc))
    c.append('case_eq("ubig_n", %s, function(){ return E.Uvarint(%s)[1]; });'
             % (json.dumps(norm_num(len(enc))), u8expr(enc)))
    # 2^64-1 as BigInt, 10-byte form
    m64 = (1 << 64) - 1
    enc = put_uvarint(m64)
    c.append('case_eq("putm64", %s, function(){ return E.PutUvarint(%sn); });'
             % (json.dumps(norm_bytes(enc)), m64))
    c.append('case_eq("um64", %s, function(){ return String(E.Uvarint(%s)[0]); });'
             % (json.dumps(norm_str(str(m64))), u8expr(enc)))
    # signed
    svals = [0, -1, 1, -2, 2, -64, 63, -8192, 8191, -MAXSAFE, MAXSAFE]
    for i, v in enumerate(svals):
        enc = put_varint(v)
        c.append('case_eq("pute%d", %s, function(){ return E.PutVarint(%s); });'
                 % (i, json.dumps(norm_bytes(enc)), jsnum(v)))
        c.append('case_eq("ve%d", %s, function(){ return E.Varint(%s); });'
                 % (i, json.dumps(norm_json([v, len(enc)])), u8expr(enc)))
    c.append('case_eq("vbig", %s, function(){ return String(E.Varint(E.PutVarint(-%sn))[0]); });'
             % (json.dumps(norm_str(str(-(1 << 62)))), (1 << 62)))
    # range errors (documented)
    c.append('case_err("putu_neg", "RangeError", function(){ return E.PutUvarint(-1); });')
    c.append('case_err("putu_frac", "RangeError", function(){ return E.PutUvarint(1.5); });')
    c.append('case_err("putu_unsafe", "RangeError", function(){ return E.PutUvarint(%d); });' % (MAXSAFE + 1))
    c.append('case_err("putu_nan", "RangeError", function(){ return E.PutUvarint(NaN); });')
    c.append('case_err("putv_frac", "RangeError", function(){ return E.PutVarint(-0.5); });')
    # truncation: documented [0, 0]
    c.append('case_eq("u_trunc", %s, function(){ return E.Uvarint(hex2u8("ac")); });'
             % json.dumps(norm_json([0, 0])))
    c.append('case_eq("v_trunc", %s, function(){ return E.Varint(hex2u8("")); });'
             % json.dumps(norm_json([0, 0])))
    # overflow: >64 bits -> RangeError (the documented contract; the old
    # impl-defined [0, -11] sentinel leaked the codec's internal negative
    # bytesRead through the [value, bytesRead] API)
    c.append('case_err("u_overflow", "RangeError", function(){ return E.Uvarint(hex2u8("ffffffffffffffffff7f")); });')
    c.append('case_err("u_overflow_2p64", "RangeError", function(){ return E.Uvarint(hex2u8("80808080808080808002")); });')
    emit("e07_varint_matrix", c, "LEB128/zigzag vs pinned python impl")
    return len(c)


# ================================================================ e08 rejection
def gen_reject():
    c = []
    rejects = [
        ("hex_odd", 'E.HexDecode("abc")'),
        ("hex_bad", 'E.HexDecode("zz")'),
        ("hex_ws", 'E.HexDecode("de ad")'),
        ("b64_len2", 'E.Base64Decode("AQ")'),
        ("b64_len1", 'E.Base64Decode("A")'),
        ("b64_len3", 'E.Base64Decode("AQI")'),
        ("b64_bad", 'E.Base64Decode("AQ=D")'),
        ("b64_ws", 'E.Base64Decode("AQ ID")'),
        ("b64_pct", 'E.Base64Decode("%2B")'),
        ("b64u_len1", 'E.Base64URLDecode("A")'),
        ("b64u_stdchar", 'E.Base64URLDecode("AQ=D")'),
        ("b64u_stdplus", 'E.Base64URLDecode("A+B")'),
        ("b64u_stdslash", 'E.Base64URLDecode("A/B")'),
        ("b64u_ok_nopad", 'E.Base64URLDecode("AQI")', True),
        ("b32_unpad", 'E.Base32Decode("MY")'),
        ("b32_badchar", 'E.Base32Decode("MY======")'.replace("MY", "M1")),
        ("b32_badshape", 'E.Base32Decode("M=======")'),
        ("b32_padmid", 'E.Base32Decode("MY======MZXQ====")'),
        ("b32h_bad", 'E.Base32HexDecode("W1======")'),
        ("a85_overflow", 'E.Base85Decode("uuuuu")'),
        ("a85_overflow2", 'E.Base85Decode("s8W-\\"")'),
        ("a85_badchar", 'E.Base85Decode("|!!!!!")'),
        ("a85_stray_z", 'E.Base85Decode("!z!!!")'),
        ("a85_lone", 'E.Base85Decode("!")'),
        ("b58_bad0", 'E.Base58Decode("O0")'),
        ("b58_badI", 'E.Base58Decode("Il")'),
        ("b58ck_short", 'E.Base58CheckDecode("1")'),
        ("b58ck_badsum", 'E.Base58CheckDecode(%s)' % jslit(b58check_encode(b"hello")[:-4] + ("1" if b58check_encode(b"hello")[-1] != "1" else "2"))),
    ]
    for name, expr in [(r[0], r[1]) for r in rejects if len(r) == 2]:
        c.append('case_err("%s", "SyntaxError", function(){ return %s; });' % (name, expr))
    # the one OK row in this probe: unpadded base64url decodes
    c.append('case_eq("b64u_ok_nopad", %s, function(){ return E.Base64URLDecode("AQI"); });'
             % json.dumps(norm_bytes(b"\x01\x02")))
    emit("e08_reject_matrix", c, "malformed-input rejection (hostile matrix)")
    return len(c)


# ================================================================ e09 JSON5
def gen_json5():
    c = []
    # JSON-subset differential vs python json (restricted to canonical doubles)
    rnd = random.Random(0x5)
    docs = []
    docs.append(("obj_nested", {"a": {"b": {"c": [1, 2, {"d": None}]}}}))
    docs.append(("arr", [1, "two", True, False, None, [1], {}]))
    docs.append(("strs", {"s": "hello \"world\" \\ / \n\t\r" + chr(0x1F600)}))
    docs.append(("esc", {"a": "A" + chr(0xE9) + chr(0x4E2D)}))
    docs.append(("ws", {"k": [1, 2]}))
    docs.append(("empty", {}))
    docs.append(("ears", []))
    docs.append(("dupkey_last", {"a": 1, "a": 2}))
    for name, doc in docs:
        text = json.dumps(doc)
        c.append('case_eq("j5_%s", %s, function(){ return E.JSON5Parse(%s); });'
                 % (name, json.dumps(norm_json(doc)), jslit(text)))
    # numbers: JSON text -> double; keep values with short canonical reprs
    num_texts = ["0", "-0", "1", "-1", "0.5", "-0.25", "3.14159", "1e3", "1.5e-3",
                 "123456789", "9007199254740991", "-9007199254740991", "1e999"]
    for t in num_texts:
        v = float(t)
        want = "Infinity" if v == float("inf") else v
        c.append('case_eq("j5num_%s", %s, function(){ return E.JSON5Parse(%s); });'
                 % (t.replace(".", "_").replace("-", "m").replace("+", "p"),
                    json.dumps(norm_num(want if want != "Infinity" else "Infinity")),
                    jslit(t)))
    # JSON5 extras (documented superset)
    extras = [
        ("unquoted_key", '{a:1}', {"t": "j", "v": '{"a":1}'}),
        ("single_quote", "{'a':'b'}", {"t": "j", "v": '{"a":"b"}'}),
        ("trailing_comma", '[1,2,]', {"t": "j", "v": "[1,2]"}),
        ("obj_trailing", '{a:1,}', {"t": "j", "v": '{"a":1}'}),
        ("comment_line", '{a:1,//c\nb:2}', {"t": "j", "v": '{"a":1,"b":2}'}),
        ("comment_block", '{/*x*/a:1}', {"t": "j", "v": '{"a":1}'}),
        ("hex", '0x1F', {"t": "n", "v": "31"}),
        ("hex_neg", '-0x10', {"t": "n", "v": "-16"}),
        ("plus", '+5', {"t": "n", "v": "5"}),
        ("leading_dot", '.5', {"t": "n", "v": "0.5"}),
        ("trailing_dot", '5.', {"t": "n", "v": "5"}),
        ("infinity", 'Infinity', {"t": "n", "v": "Infinity"}),
        ("neg_infinity", '-Infinity', {"t": "n", "v": "-Infinity"}),
        ("nan", 'NaN', {"t": "n", "v": "NaN"}),
        ("multiline_str", '"a\\u000ab"', {"t": "s", "v": "a\nb"}),
    ]
    for name, text, want in extras:
        c.append('case_eq("j5x_%s", %s, function(){ return E.JSON5Parse(%s); });'
                 % (name.strip(), json.dumps(want), jslit(text)))
    c.append('case_eq("j5x_inf_str", {t:"s",v:"Infinity"},'
             ' function(){ return String(E.JSON5Parse("Infinity")); });')
    # __proto__ stays an own property (documented safety stance)
    c.append('case_eq("j5_proto_own", {t:"s",v:"own"}, function(){'
             ' var o=E.JSON5Parse("{__proto__:1}");'
             ' return o.hasOwnProperty("__proto__") ? "own" : "TAINT"; });')
    c.append('case_eq("j5_proto_keys", {t:"s",v:"__proto__"}, function(){'
             ' return Object.keys(E.JSON5Parse("{__proto__:1}")).join(","); });')
    # rejection matrix. NOTE: "{a:1,}" (trailing comma), '"\x41"' (hex escape),
    # and "{'a':1,}" are VALID JSON5 — the lenient rows below pin them (and the
    # impl-defined "+"->NaN / "-"->NaN / "01"->1 rows) instead of expecting
    # SyntaxError.
    bad = ["", "{", "}", "[", "[1,", '{"a"}', '{"a":}', "{'a':1", '"unterminated',
           "'unterminated", '{"a":1} garbage', "0x", "1e", "..",
           "[1 2]", "{a:1 b:2}", "tru", "undefined"]
    for i, t in enumerate(bad):
        c.append('case_err("j5bad_%d", "SyntaxError", function(){ return E.JSON5Parse(%s); });'
                 % (i, jslit(t)))
    # impl-defined lenient rows (documented divergence from the JSON5 grammar;
    # pinned so a silent change is caught)
    c.append('case_eq("j5lenient_plus", {t:"n",v:"NaN"}, function(){ return E.JSON5Parse("+"); });')
    c.append('case_eq("j5lenient_minus", {t:"n",v:"NaN"}, function(){ return E.JSON5Parse("-"); });')
    c.append('case_eq("j5lenient_01", {t:"n",v:"1"}, function(){ return E.JSON5Parse("01"); });')
    x41_text = '"' + chr(92) + 'x41"'  # JS source text: "\x41"
    c.append('case_eq("j5valid_x41", {t:"s",v:"A"}, function(){ return E.JSON5Parse(%s); });'
             % jslit(x41_text))
    c.append('case_eq("j5valid_trailcomma", %s, function(){ return E.JSON5Parse("{a:1,}"); });'
             % json.dumps(norm_json({"a": 1})))
    # depth cap: 256 passes, 300 refuses (documented: checked before descending)
    ok_nest = "[" * 256 + "]" * 256
    c.append('case_eq("j5_depth256", {t:"b",v:"true"}, function(){'
             ' return E.JSON5Parse(%s) instanceof Array; });' % jslit(ok_nest))
    deep_nest = "[" * 257 + "]" * 257
    c.append('case_err("j5_depth257", "RangeError", function(){ return E.JSON5Parse(%s); });'
             % jslit(deep_nest))
    # JSON5Stringify
    c.append('case_eq("j5s_basic", %s, function(){ return E.JSON5Stringify({a:1,b:[1,2]}); });'
             % json.dumps(norm_str('{"a":1,"b":[1,2]}')))
    c.append('case_eq("j5s_inf", %s, function(){ return E.JSON5Stringify({a:Infinity}); });'
             % json.dumps(norm_str('{"a":Infinity}')))
    c.append('case_eq("j5s_nan", %s, function(){ return E.JSON5Stringify({a:NaN}); });'
             % json.dumps(norm_str('{"a":NaN}')))
    c.append('case_eq("j5s_indent2", %s, function(){ return E.JSON5Stringify({a:1},{indent:2}); });'
             % json.dumps(norm_str('{\n  "a": 1\n}')))
    c.append('case_eq("j5s_indent10", {t:"b",v:"true"}, function(){'
             ' var s=E.JSON5Stringify({a:1},{indent:99});'
             ' return s.indexOf(" ") === 2 && s.length < 40; });')
    c.append('case_err("j5s_cycle", "TypeError", function(){ var o={}; o.o=o;'
             ' return E.JSON5Stringify(o); });')
    c.append('case_eq("j5s_dag", %s, function(){ var s={x:1};'
             ' return E.JSON5Stringify({a:s,b:s}); });'
             % json.dumps(norm_str('{"a":{"x":1},"b":{"x":1}}')))
    emit("e09_json5", c, "JSON5 vs python json (subset) + documented superset")
    return len(c)


# ================================================================ e10 stable jsonpath
def gen_stable_jsonpath():
    c = []
    # RFC 8785 canonical form vs python oracle (simple numeric corpus)
    def canon_s(s):
        # RFC 8785: escape " \ and C0 only; everything else stays raw
        out = '"'
        for ch in s:
            o = ord(ch)
            if ch == '"':
                out += '\\"'
            elif ch == "\\":
                out += "\\\\"
            elif o == 8:
                out += "\\b"
            elif o == 9:
                out += "\\t"
            elif o == 10:
                out += "\\n"
            elif o == 12:
                out += "\\f"
            elif o == 13:
                out += "\\r"
            elif o < 0x20:
                out += "\\u%04x" % o
            else:
                out += ch
        return out + '"'
    def stable_py(value):
        def sort_key(k):
            return k.encode("utf-16-be", "surrogatepass")
        if isinstance(value, dict):
            return "{" + ",".join('%s:%s' % (canon_s(k), stable_py(v))
                                  for k, v in sorted(value.items(), key=lambda kv: sort_key(kv[0]))) + "}"
        if isinstance(value, list):
            return "[" + ",".join(stable_py(v) for v in value) + "]"
        if value is True:
            return "true"
        if value is False:
            return "false"
        if value is None:
            return "null"
        if isinstance(value, int):
            return str(value)
        raise ValueError("unsupported float in corpus")
    docs = [{"b": 1, "a": 2}, {"z": {"c": 3, "a": [1, 2]}}, {"a": -0},
            [3, 1, 2], {"": 1, "a": 2}, {"😀": 1, "a": 2, "~": 3, " ": 4},
            {"nested": {"deep": {"deeper": {"deepest": [True, None]}}}}]
    for i, doc in enumerate(docs):
        want = stable_py(doc)
        c.append('case_eq("stab%d", %s, function(){ return E.StableStringify(%s); });'
                 % (i, json.dumps(norm_str(want)), json.dumps(doc)))
    c.append('case_eq("stab_neg0", %s, function(){ return E.StableStringify({a:-0}); });'
             % json.dumps(norm_str('{"a":0}')))
    c.append('case_eq("stab_indent_ignored", %s, function(){'
             ' return E.StableStringify({b:1,a:2},{indent:5}); });'
             % json.dumps(norm_str('{"a":2,"b":1}')))
    c.append('case_err("stab_nan", "TypeError", function(){ return E.StableStringify({a:NaN}); });')
    c.append('case_err("stab_inf", "TypeError", function(){ return E.StableStringify([Infinity]); });')
    c.append('case_eq("stab_keyorder_utf16", %s, function(){'
             ' return E.StableStringify({"\\uFFFF":1,"a":2,"😀":3,"\\u0000":4}); });'
             # RFC 8785 escapes NUL as the 6-char \u0000 sequence; astral and
             # U+FFFF stay raw; sorted by UTF-16 code unit: 0000 < 0061 < D83D DE00 < FFFF
             % json.dumps(norm_str('{"\\u0000":4,"a":2,"😀":3,"￿":1}')))

    # JSONPath per RFC 9535 examples (documented subset: data-only queries)
    store = {"store": {"book": [
        {"category": "reference", "author": "Nigel Rees", "title": "Sayings of the Century", "price": 8.95},
        {"category": "fiction", "author": "Evelyn Waugh", "title": "Sword of Honour", "price": 12.99},
        {"category": "fiction", "author": "Herman Melville", "title": "Moby Dick", "isbn": "0-553-21311-3", "price": 8.99},
        {"category": "fiction", "author": "J. R. R. Tolkien", "title": "The Lord of the Rings", "isbn": "0-395-19395-8", "price": 22.99}],
        "bicycle": {"color": "red", "price": 19.95}}}
    P = 'new E.JSONPath(%s)'
    D = "STORE"
    c.append("var STORE = %s;" % json.dumps(store))
    cases = [
        ("$.store.book[*].author", ["Nigel Rees", "Evelyn Waugh", "Herman Melville", "J. R. R. Tolkien"]),
        ("$..author", ["Nigel Rees", "Evelyn Waugh", "Herman Melville", "J. R. R. Tolkien"]),
        ("$.store.*", None),  # count only
        ("$.store..price", [8.95, 12.99, 8.99, 22.99, 19.95]),
        ("$..book[2]", [{"category": "fiction", "author": "Herman Melville", "title": "Moby Dick", "isbn": "0-553-21311-3", "price": 8.99}]),
        ("$..book[-1]", [{"category": "fiction", "author": "J. R. R. Tolkien", "title": "The Lord of the Rings", "isbn": "0-395-19395-8", "price": 22.99}]),
        ("$..book[0,1]", None),
        ("$..book[:2]", None),
        ("$..book[1:3]", None),
        ("$..book[?(@.isbn)]", None),
        ("$..book[?(@.price<10)]", None),
        ("$..book[?(@.price>8 && @.price<20)]", None),
        ("$.store.bicycle.color", ["red"]),
        ("$['store']['book'][0]['title']", ["Sayings of the Century"]),
        ("$.nonexistent", []),
    ]
    for i, (expr, want) in enumerate(cases):
        if want is None:
            continue
        c.append('case_eq("jp%d_v", %s, function(){ return %s.all(%s); });'
                 % (i, json.dumps(norm_json(want)), P % jslit(expr), D))
    counts = [("$..book[0,1]", 2), ("$..book[:2]", 2), ("$..book[1:3]", 2),
              ("$..book[?(@.isbn)]", 2), ("$..book[?(@.price<10)]", 2),
              ("$..book[?(@.price>8 && @.price<20)]", 3), ("$.store.*", 2),
              ("$.store.book[*]", 4)]
    for i, (expr, n) in enumerate(counts):
        c.append('case_eq("jpc%d", %s, function(){ return %s.all(%s).length; });'
                 % (i, json.dumps(norm_num(n)), P % jslit(expr), D))
    c.append('case_eq("jp_first", %s, function(){ return %s.first(%s); });'
             % (json.dumps(norm_num(8.95)), P % jslit("$.store.book[*].price"), D))
    c.append('case_eq("jp_first_none", {t:"u"}, function(){ return %s.first(%s); });'
             % (P % jslit("$.nope"), D))
    c.append('case_eq("jp_paths", %s, function(){ return %s.paths(%s).join("|"); });'
             % (json.dumps(norm_str("$['store']['book'][0]['author']")),
                P % jslit("$.store.book[0].author"), D))
    c.append('case_err("jp_syntax", "SyntaxError", function(){ return %s; });'
             % (P % jslit("$.store.book[")))
    c.append('case_eq("jp_no_leak", {t:"b",v:"true"}, function(){'
             ' var r = %s.all(%s); return r.length === 0; });'
             % (P % jslit("$.__proto__"), D))
    emit("e10_stable_jsonpath", c, "StableStringify(RFC 8785) + JSONPath(RFC 9535)")
    return len(c)


# ================================================================ e12 detect
def gen_detect():
    c = []
    det = [
        ("utf32be_bom", b"\x00\x00\xfe\xff" + b"a\x00\x00\x00", "utf-32be"),
        ("utf32le_bom", b"\xff\xfe\x00\x00" + b"a\x00\x00\x00", "utf-32le"),
        ("utf8_bom", b"\xef\xbb\xbfhello", "utf-8"),
        ("utf16be_bom", b"\xfe\xff\x00a", "utf-16be"),
        ("utf16le_bom", b"\xff\xfe\x61\x00", "utf-16le"),
        ("ascii", b"hello world, plain ascii!", "utf-8"),
        ("valid_utf8", "héllo 世界 🌍 ok".encode(), "utf-8"),
        ("empty", b"", "utf-8"),
    ]
    for name, data, want in det:
        c.append('case_eq("det_%s", %s, function(){ return E.DetectEncoding(%s); });'
                 % (name, json.dumps(norm_str(want)), u8expr(data)))
    # invalid utf-8 falls through to CJK statistics / fallback (documented)
    bad = b"\xc3\x28" * 8
    c.append('case_eq("det_fallback", %s, function(){'
             ' return E.DetectEncoding(%s, {fallback:"windows-1252"}); });'
             % (json.dumps(norm_str("windows-1252")), u8expr(bad)))
    # sjis kana text (documented example vector class)
    sjis = bytes.fromhex("82a082a182a282a382a482a582a682a7")
    c.append('case_eq("det_sjis", %s, function(){ return E.DetectEncoding(%s); });'
             % (json.dumps(norm_str("shift_jis")), u8expr(sjis)))
    # allowList semantics (documented: case-insensitive member or fallback)
    c.append('case_eq("det_allow_hit", %s, function(){'
             ' return E.DetectEncoding(%s, {allowList:["UTF-8"]}); });'
             % (json.dumps(norm_str("utf-8")), u8expr(b"\xef\xbb\xbfhi")))
    c.append('case_eq("det_allow_fb", %s, function(){'
             ' return E.DetectEncoding(%s, {fallback:"shift_jis", allowList:["shift_jis"]}); });'
             % (json.dumps(norm_str("shift_jis")), u8expr(b"\x81\x40" * 8)))
    c.append('case_err("det_allow_miss", "TypeError", function(){'
             ' return E.DetectEncoding(%s, {allowList:["gbk"]}); });' % u8expr(b"\xef\xbb\xbfhi"))
    # lowercase alias exists
    c.append('case_eq("det_lower_alias", {t:"b",v:"true"}, function(){'
             ' return E.detectEncoding(%s) === E.DetectEncoding(%s); });'
             % (u8expr(b"\xef\xbb\xbfhi"), u8expr(b"\xef\xbb\xbfhi")))
    # input form validation
    c.append('case_err("det_string_rejected", "TypeError", function(){'
             ' return E.DetectEncoding("plain string"); });')
    emit("e12_detect", c, "charset detection vs documented deterministic rules")
    return len(c)


def main():
    n = 0
    n += gen_vectors()
    n += sum(gen_matrix().values())
    n += gen_basex58()
    n += gen_varint()
    n += gen_reject()
    n += gen_json5()
    n += gen_stable_jsonpath()
    n += gen_detect()
    print("gen_encoding: %d cases emitted" % n)


if __name__ == "__main__":
    main()
