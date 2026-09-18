"""common.py — shared helpers for the dyna_data probe generators.

Every generator emits self-contained .js probes into probes/<module>/ that
import the portable harness h.js (copied verbatim from tests/agent/strnum_bb).
Oracles (python hashlib/base64/uuid/reference implementations, openssl, node)
run at GENERATION time and their outputs are baked into the probes as plain
literals — the probe never computes its own expected answer.
"""
import os
import subprocess
import hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
PROBES = os.path.abspath(os.path.join(HERE, "..", "probes"))

HJS = "//@JS@"  # marker replaced with the relative include path


def write_probe(module, name, body, node_ok=False, imports=None):
    """Emit probes/<module>/<name>.js with the h.js harness inlined.

    `imports` is a list of import statements (or one multi-line string) —
    dynajs module autodetect requires import declarations to come FIRST in
    the file, so they are emitted before the harness text."""
    d = os.path.join(PROBES, module)
    os.makedirs(d, exist_ok=True)
    if node_ok:
        name = "n_" + name
    hpath = os.path.join(HERE, "..", "h.js")
    with open(hpath) as f:
        harness = f.read()
    path = os.path.join(d, name + ".js")
    if isinstance(body, str):
        body = body.split("\n")
    if imports is None:
        # auto-extract leading import statements from body (generated bodies
        # conventionally start with them); a statement ends at a line with ';'
        imports = []
        rest = []
        collecting = False
        for ln in body:
            if not collecting and ln.startswith("import"):
                collecting = True
                imports.append(ln)
                if ";" in ln:
                    collecting = False
                continue
            if collecting:
                imports.append(ln)
                if ";" in ln:
                    collecting = False
                continue
            rest.append(ln)
        body = rest
    elif isinstance(imports, str):
        imports = [imports]
    with open(path, "w") as f:
        f.write("// GENERATED probe (gen/%s.py) — do not edit; regenerate.\n" % module)
        for imp in imports:
            f.write(imp.rstrip() + "\n")
        f.write(harness)
        f.write("\n".join(body) + "\n")
    return path


def js_bytes_literal(data):
    """Uint8Array literal for arbitrary bytes."""
    return "new Uint8Array([" + ",".join(str(b) for b in data) + "])"


def js_str_literal(s):
    """A JS string literal safe for any BMP/astral content (no lone surrogates)."""
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif 32 <= o < 127:
            out.append(ch)
        elif o <= 0xFFFF:
            out.append("\\u%04x" % o)
        else:
            # astral: emit surrogate pair escapes so the source is ASCII-safe
            v = o - 0x10000
            hi = 0xD800 + (v >> 10)
            lo = 0xDC00 + (v & 0x3FF)
            out.append("\\u%04x\\u%04x" % (hi, lo))
    return '"' + "".join(out) + '"'


def py_sha(info, data):
    return hashlib.new(info, data).hexdigest()


def run_node(expr, timeout=30):
    """Evaluate a JS expression under node, return stdout stripped."""
    r = subprocess.run(["node", "-e", expr], capture_output=True,
                       text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError("node failed: %s\n%s" % (expr, r.stderr))
    return r.stdout.strip()


# deterministic pseudo-random byte streams shared between generator and probe:
# the probe regenerates the same stream with the same tiny LCG so probes can
# hash kilobytes without megabyte literals.
LCG_JS = """\
function lcg(seed, n) {          // n bytes, xorshift-ish LCG, deterministic
  var a = seed >>> 0, out = new Uint8Array(n);
  for (var i = 0; i < n; i++) {
    a = (Math.imul(a, 1103515245) + 12345) >>> 0;
    out[i] = (a >>> 16) & 0xff;
  }
  return out;
}
"""

def py_lcg(seed, n):
    out = bytearray(n)
    a = seed & 0xFFFFFFFF
    for i in range(n):
        a = (a * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (a >> 16) & 0xFF
    return bytes(out)
