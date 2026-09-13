#!/usr/bin/env python3
"""gen_common.py — shared helpers for the dyna_text probe generators.

Every generator: python3 stdlib is the ORACLE; expectations are computed here
and baked into generated .js probes as __norm-shaped JSON (see h.js). Probes
run on dynajs only (dyna:* modules are dynajs-only); oracle-vs-engine
agreement IS the differential test.
"""
import json, math, os, random

HERE = os.path.dirname(os.path.abspath(__file__))
PROBES = os.path.join(HERE, "probes")

# ----------------------------------------------------------------- literals
def jslit(s):
    """python str -> ASCII-safe JS string literal (astral as surrogate pairs)."""
    return json.dumps(s, ensure_ascii=True)

def jsnum(x):
    """python float/int -> JS number literal that denotes the same double."""
    if isinstance(x, bool):
        return "true" if x else "false"
    if isinstance(x, int):
        return str(x)
    if x == 0 and repr(x)[0] == "-":
        return "-0"
    return repr(x)

# ----------------------------------------------------------------- __norm mirror
def fnv1a(s):
    """32-bit FNV-1a over the UTF-16 code units of `s` (hex strings are ASCII,
    so byte-wise == code-unit-wise). Mirrors h.js __fnv1a exactly."""
    h = 0x811C9DC5
    for ch in s:
        h ^= ord(ch)
        h = (h * 16777619) & 0xFFFFFFFF
    return h

def norm_str(s):
    """__norm shape for a JS string result."""
    if len(s) <= 512:
        return {"t": "s", "v": s}
    mid = len(s) >> 1
    return {"t": "S", "len": len(s), "fnv": fnv1a(s),
            "h": s[:64], "a": s[-64:], "m": s[mid - 32:mid + 32]}

def norm_num(x):
    """__norm shape for a JS number result (x: int/float, or '-0').
    Integral floats are formatted like JS String(number): 0.0 -> "0";
    -0.0 keeps the sign bit (h.js __norm distinguishes it)."""
    if x == "-0":
        return {"t": "n", "v": "-0"}
    if isinstance(x, float):
        if x != x:
            v = "NaN"
        elif x == float("inf"):
            v = "Infinity"
        elif x == float("-inf"):
            v = "-Infinity"
        elif x == 0 and math.copysign(1.0, x) < 0:
            return {"t": "n", "v": "-0"}
        elif x.is_integer() and abs(x) <= 2 ** 53:
            v = str(int(x))
        else:
            v = repr(x)
        return {"t": "n", "v": v}
    return {"t": "n", "v": str(x)}

def norm_bytes(data):
    """__norm shape for a Uint8Array result (hex digest, mirroring h.js)."""
    hexs = data.hex()
    if len(hexs) <= 512:
        return {"t": "x", "v": hexs}
    mid = len(hexs) >> 1
    return {"t": "X", "len": len(data), "fnv": fnv1a(hexs),
            "h": hexs[:64], "a": hexs[-64:], "m": hexs[mid - 32:mid + 32]}

def norm_json(value):
    """__norm shape for any JSON-able result: JSON text if short, else digest.
    Serialization MUST match JS JSON.stringify: ensure_ascii=False keeps
    non-ASCII raw (verified identical escaping vs node)."""
    j = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    if len(j) <= 1024:
        return {"t": "j", "v": j}
    return {"t": "J", "len": len(j), "fnv": fnv1a(j), "h": j[:96], "a": j[-96:]}

# ----------------------------------------------------------------- probe emit
HEADER_MARK = "// GENERATED probe (dyna_text) — DO NOT EDIT; run materialize.sh\n"

def read_hjs():
    with open(os.path.join(HERE, "h.js")) as f:
        src = f.read()
    # strip the leading contract comment so the GENERATED marker stays first
    lines = src.split("\n")
    i = 0
    while i < len(lines) and lines[i].startswith("//"):
        i += 1
    return "\n".join(lines[i:])

def write_probe(relpath, imports, body, tag):
    """Emit probes/<relpath>: GENERATED marker + imports + h.js + cases.

    NOTE: imports come BEFORE any statement — the dynajs CLI treats a file as
    a module only when `import` appears before other top-level statements
    (engine loader heuristic; node accepts imports anywhere at top level)."""
    path = os.path.join(PROBES, relpath)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    imp = "".join('import * as %s from "%s";\n' % (alias, mod) for alias, mod in imports)
    with open(path, "w") as f:
        f.write(HEADER_MARK)
        f.write("// %s\n" % tag)
        f.write(imp)
        f.write("\n" + read_hjs())
        f.write("\n" + body)
        f.write("\nsummary(%s);\n" % jslit(tag))
    return path

# ----------------------------------------------------------------- blobs
def blob_set():
    """Deterministic input corpus shared by the codec generators."""
    rnd = random.Random(0xD9A)
    blobs = {}
    blobs["empty"] = b""
    blobs["one"] = b"\x00"
    blobs["three"] = b"abc"
    blobs["zeros64"] = bytes(64)
    blobs["rand255"] = bytes(range(256))
    blobs["utf8"] = "héllo, wörld — 日本語テキスト 🌍🚀 ok".encode("utf-8")
    blobs["text1k"] = ("The quick brown fox jumps over the lazy dog. " * 24).encode()
    blobs["rand1k"] = bytes(rnd.randrange(256) for _ in range(1024))
    blobs["rand64k"] = bytes(rnd.randrange(256) for _ in range(65536))
    return blobs

# js expression: Uint8Array from a hex literal
def u8expr(data):
    return "hex2u8(%s)" % jslit(data.hex())


def norm_any(value):
    """__norm shape dispatching on the python type of the oracle value."""
    if value is None:
        return {"t": "z"}
    if value is True:
        return {"t": "b", "v": "true"}
    if value is False:
        return {"t": "b", "v": "false"}
    if isinstance(value, (int, float)):
        return norm_num(value)
    if isinstance(value, str):
        return norm_str(value)
    return norm_json(value)
