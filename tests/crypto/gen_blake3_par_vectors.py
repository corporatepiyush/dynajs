# Independent BLAKE3 reference (written from the BLAKE3 specification, section 2-5: compression
# function, chunk chaining, the binary tree with the left subtree as the largest power of two chunks
# strictly smaller than the input, ROOT on the final node, XOF by output block counter). It shares no
# code with src/modules/crypto/crypto-blake.c. Self-check: every one of the 105 official BLAKE3 values
# (tests/crypto/blake3_official_vectors.json: hash, keyed_hash and derive_key at 35 lengths, 131-byte
# XOF output) is asserted before anything is printed. Output: the vector table of
# tests/crypto/test_crypto_blake3_par.js (the large inputs official vectors do not reach).
# Usage: python3 -I tests/crypto/gen_blake3_par_vectors.py
import struct, sys

IV = [0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19]
PERM = [2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8]
CHUNK_START, CHUNK_END, PARENT, ROOT, KEYED, DERIVE_CTX, DERIVE_MAT = 1, 2, 4, 8, 16, 32, 64
M32 = 0xFFFFFFFF

def g(v, a, b, c, d, x, y):
    v[a] = (v[a] + v[b] + x) & M32; v[d] ^= v[a]; v[d] = ((v[d] >> 16) | (v[d] << 16)) & M32
    v[c] = (v[c] + v[d]) & M32; v[b] ^= v[c]; v[b] = ((v[b] >> 12) | (v[b] << 20)) & M32
    v[a] = (v[a] + v[b] + y) & M32; v[d] ^= v[a]; v[d] = ((v[d] >> 8) | (v[d] << 24)) & M32
    v[c] = (v[c] + v[d]) & M32; v[b] ^= v[c]; v[b] = ((v[b] >> 7) | (v[b] << 25)) & M32

def compress(cv, block, counter, blen, flags):
    m = list(struct.unpack("<16I", block))
    v = list(cv) + IV[:4] + [counter & M32, (counter >> 32) & M32, blen, flags]
    for r in range(7):
        g(v, 0, 4, 8, 12, m[0], m[1]); g(v, 1, 5, 9, 13, m[2], m[3])
        g(v, 2, 6, 10, 14, m[4], m[5]); g(v, 3, 7, 11, 15, m[6], m[7])
        g(v, 0, 5, 10, 15, m[8], m[9]); g(v, 1, 6, 11, 12, m[10], m[11])
        g(v, 2, 7, 8, 13, m[12], m[13]); g(v, 3, 4, 9, 14, m[14], m[15])
        if r < 6:
            m = [m[i] for i in PERM]
    return [v[i] ^ v[i + 8] for i in range(8)] + [v[i + 8] ^ cv[i] for i in range(8)]

def chunk_node(key, data, counter, flags):
    # returns (cv_in, block, counter, blen, flags) of the chunk's last block, not yet compressed
    blocks = [data[i:i + 64] for i in range(0, len(data), 64)] or [b""]
    cv = key
    for i, b in enumerate(blocks):
        f = flags | (CHUNK_START if i == 0 else 0)
        if i == len(blocks) - 1:
            return (cv, b.ljust(64, b"\0"), counter, len(b), f | CHUNK_END)
        cv = compress(cv, b, counter, 64, f)[:8]

def tree_node(key, data, counter, flags):
    if len(data) <= 1024:
        return chunk_node(key, data, counter, flags)
    chunks = (len(data) + 1023) // 1024
    left = 1
    while left * 2 < chunks:
        left *= 2
    l = node_cv(tree_node(key, data[:left * 1024], counter, flags))
    r = node_cv(tree_node(key, data[left * 1024:], counter + left, flags))
    return (key, struct.pack("<8I", *l) + struct.pack("<8I", *r), 0, 64, flags | PARENT)

def node_cv(n):
    cv, block, counter, blen, flags = n
    return compress(cv, block, counter, blen, flags)[:8]

def root_bytes(n, outlen):
    cv, block, _, blen, flags = n
    out = b""
    k = 0
    while len(out) < outlen:
        out += struct.pack("<16I", *compress(cv, block, k, blen, flags | ROOT))
        k += 1
    return out[:outlen]

def blake3(data, outlen=32, key=None, context=None):
    if context is not None:
        ck = root_bytes(tree_node(IV, context.encode(), 0, DERIVE_CTX), 32)
        return root_bytes(tree_node(list(struct.unpack("<8I", ck)), data, 0, DERIVE_MAT), outlen)
    if key is not None:
        return root_bytes(tree_node(list(struct.unpack("<8I", key)), data, 0, KEYED), outlen)
    return root_bytes(tree_node(IV, data, 0, 0), outlen)

pat = lambda n: bytes(i % 251 for i in range(n))
import json, os
OFF = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "blake3_official_vectors.json")))
OKEY = OFF["key"].encode()
for c in OFF["cases"]:
    data = pat(c["input_len"])
    n = len(c["hash"]) // 2
    assert blake3(data, n).hex() == c["hash"], ("hash", c["input_len"])
    assert blake3(data, n, key=OKEY).hex() == c["keyed_hash"], ("keyed", c["input_len"])
    assert blake3(data, n, context=OFF["context_string"]).hex() == c["derive_key"], ("derive", c["input_len"])

KEY = b"whats the Elephant and Castle my"
CTX = "BLAKE3 2019-12-27 16:29:52 test vectors context"
MiB = 1 << 20
rows = []
for n in [128 * 1024 - 1, 128 * 1024, 128 * 1024 + 1, 128 * 1024 + 1025, 192 * 1024 + 1, MiB - 1, MiB, MiB + 1, MiB + 1024, MiB + 1025, 2 * MiB + 1, 3 * MiB + 5, 4 * MiB + 1025, 6 * MiB]:
    rows.append((n, "plain", 32, blake3(pat(n)).hex()))
for n in [2 * MiB + 1, 3 * MiB + 1024]:
    rows.append((n, "keyed", 32, blake3(pat(n), key=KEY).hex()))
    rows.append((n, "derive", 32, blake3(pat(n), context=CTX).hex()))
    rows.append((n, "xof", 131, blake3(pat(n), 131).hex()))
    rows.append((n, "keyed-xof", 1000, blake3(pat(n), 1000, key=KEY).hex()))
for n, mode, ln, hx in rows:
    print('    [%d, "%s", %d, "%s"],' % (n, mode, ln, hx))
