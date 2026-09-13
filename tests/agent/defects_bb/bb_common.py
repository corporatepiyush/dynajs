"""Shared helpers for defects_bb black-box probe generation.

Every generated probe is DUAL-MODE:
  - record mode (node + recpreload.js at bake time): prints "RECDATA:{...json}"
  - assert mode (final artifact, runs on dynajs/pristine/node): asserts against
    the node-baked __EXPECT__ literal injected into the file.

Generators bake node's exact outcome at generation time, per the mandate.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))  # work36 root
PROBES = os.path.join(HERE, "probes")
HARNESS = open(os.path.join(HERE, "h.js")).read()
REC_PRELOAD = os.path.join(HERE, "recpreload.js")
NODE = os.environ.get("NODE_BIN", "node")


def probe_path(matrix, pid):
    d = os.path.join(PROBES, matrix)
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, pid + ".js")


def write_probe(matrix, pid, body, ext=".js"):
    """harness + body with a null __EXPECT__ placeholder; returns path."""
    p = probe_path(matrix, pid)
    if ext == ".mjs":
        p = p[:-3] + ".mjs"
    src = HARNESS + "\nvar __EXPECT__ = null; /*BAKE*/\n" + body + "\n"
    with open(p, "w") as f:
        f.write(src)
    return p


def bake(path, payload, matrix_mode=False):
    """Recorded payload replaces the null __EXPECT__ literal."""
    with open(path) as f:
        src = f.read()
    marker = "var __EXPECT__ = null; /*BAKE*/"
    assert marker in src, "no bake marker in " + path
    src = src.replace(marker, "var __EXPECT__ = " + json.dumps(payload) + "; /*BAKE*/")
    with open(path, "w") as f:
        f.write(src)


def record_node(path, module=False):
    """Run probe under node in record mode; returns parsed RECDATA dict."""
    cmd = [NODE]
    if module:
        cmd += ["-r", REC_PRELOAD]
    else:
        cmd += ["-r", REC_PRELOAD]
    cmd.append(path)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60, cwd=HERE)
    for line in r.stdout.splitlines():
        if line.startswith("RECDATA:"):
            return json.loads(line[len("RECDATA:"):])
    # No RECDATA: bubble up the failure with context
    raise RuntimeError(
        "record produced no RECDATA rc=%s\nstdout=%s\nstderr=%s\nfile=%s"
        % (r.returncode, r.stdout[:2000], r.stderr[:2000], path))


def node_outcome(path, module=False, extra_flags=None):
    """Plain node run of a file-level snippet probe: (rc, stdout)."""
    cmd = [NODE] + (extra_flags or []) + [path]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60, cwd=HERE)
    return r.returncode, r.stdout


def manifest(matrix, rows):
    """rows: list of (id, dims, tag)."""
    p = os.path.join(PROBES, matrix, "manifest.tsv")
    with open(p, "w") as f:
        for pid, dims, tag in rows:
            f.write("%s\t%s\t%s\n" % (pid, dims, tag))
    return p


def log(msg):
    print("[gen] " + msg, flush=True)


def fail(msg):
    print("[gen][FATAL] " + msg, file=sys.stderr, flush=True)
    sys.exit(1)
