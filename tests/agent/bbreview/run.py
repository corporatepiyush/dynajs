#!/usr/bin/env python3
"""Black-box differential runner: dynajs vs node vs dynajs_baseline.
Verdicts: PASS (stdout+rc match primary oracle), DIFF (mismatch -> triage),
DIFF-BASELINE (vs baseline). E* run under TZ=America/New_York for all engines."""
import subprocess, os, sys, shutil, glob, json, atexit, tempfile

# Per-run scratch (was the fixed /tmp/bbreview, which collided across runs and
# went stale for weeks). Populated FROM THE SIBLING COMMITTED PROBES so this
# runner works from a fresh checkout exactly as it did when /tmp/bbreview was
# hand-seeded; removed on exit.
HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DIR = tempfile.mkdtemp(prefix="bbreview.")
atexit.register(shutil.rmtree, DIR, ignore_errors=True)
for _p in sorted(glob.glob(os.path.join(HERE, "*.js"))):
    shutil.copy2(_p, DIR)
OUT = os.path.join(DIR, "out")
os.makedirs(OUT, exist_ok=True)
DYNA = os.path.join(DIR, "dynajs_snap")
BASE = os.path.join(DIR, "base_snap")
NODE = shutil.which("node")

_eng = os.path.join(TREE, "dynajs")
if os.path.exists(_eng):
    shutil.copy2(_eng, DYNA)
else:
    sys.exit(f"run.py: {TREE}/dynajs not found -- build the tree first")
_baseline = os.path.join(TREE, "dynajs.base")
if os.path.exists(_baseline):
    shutil.copy2(_baseline, BASE)
else:
    # No baseline build in this tree: run the base leg against the same
    # binary (trivially matching) and say so, instead of crashing mid-run.
    shutil.copy2(_eng, BASE)
    print("run.py: NOTE dynajs.base absent -- base leg runs against ./dynajs")

# primary oracle mode per file: "node" (default), "base" (baseline binding, node informational)
MODE = {
    "e06_date_pre1900.js": "base",
    "g02_snan_abs.js": "base",
}
TIMEOUT = 120

def run(binary, path, env):
    try:
        p = subprocess.run([binary, path], capture_output=True, text=True,
                           timeout=TIMEOUT, env=env)
        return p.stdout, p.stderr, p.returncode
    except subprocess.TimeoutExpired:
        return "", "<<TIMEOUT>>", -99

def main():
    files = sorted(glob.glob(os.path.join(DIR, "*.js")))
    files = [f for f in files if os.path.basename(f) not in ("timing_probe.js",)]
    ny = dict(os.environ, TZ="America/New_York")
    rows, findings = [], []
    for f in files:
        name = os.path.basename(f)
        env = ny if name.startswith("e") else os.environ.copy()
        do, de, drc = run(DYNA, f, env)
        no, ne, nrc = run(NODE, f, env)
        bo, be, brc = run(BASE, f, env)
        mode = MODE.get(name, "node")
        prim_d, prim_o = (do, no) if mode == "node" else (do, bo)
        prim_orc_rc = nrc if mode == "node" else brc
        node_match = (do == no) and ((drc == 0) == (nrc == 0))
        base_match = (do == bo) and ((drc == 0) == (brc == 0))
        if mode == "node":
            verdict = "PASS" if node_match else "DIFF-NODE"
        else:
            verdict = "PASS-BASE" if base_match else "DIFF-BASE"
        rows.append((name, verdict, drc, nrc, brc, "node=" + ("M" if node_match else "X"),
                     "base=" + ("M" if base_match else "X"), len(do), len(no), len(bo)))
        if verdict != "PASS":
            findings.append((name, verdict, do, no, bo, de, ne, be, drc, nrc, brc))
    print(f"{'file':34} {'verdict':11} {'rc d/n/b':>12}  {'node':>5} {'base':>5}  outlen d/n/b")
    for r in rows:
        print(f"{r[0]:34} {r[1]:11} {r[2]:>4}/{r[3]:>3}/{r[4]:>3}  {r[5]:>5} {r[6]:>5}  {r[7]}/{r[8]}/{r[9]}")
    print("\n=== DIFF DETAILS ===")
    for (name, verdict, do, no, bo, de, ne, be, drc, nrc, brc) in findings:
        print(f"\n--- {name} [{verdict}] rc dyna={drc} node={nrc} base={brc}")
        import difflib
        dl = list(difflib.unified_diff(no.splitlines(), do.splitlines(), "node", "dynajs", lineterm="", n=1))
        for l in dl[:40]:
            print(l)
        bl = list(difflib.unified_diff(bo.splitlines(), do.splitlines(), "baseline", "dynajs", lineterm="", n=1))
        for l in bl[:40]:
            print(l)
        for tag, err in (("dyna-err", de), ("node-err", ne), ("base-err", be)):
            e = err.strip().splitlines()
            if e and (drc != 0 or nrc != 0 or brc != 0):
                print(f"  {tag}: {e[-1][:160]}")
    print(f"\nTOTALS: {len(rows)} files, {sum(1 for r in rows if r[1]=='PASS' or r[1]=='PASS-BASE')} pass, {len(findings)} need triage")

if __name__ == "__main__":
    main()
