#!/usr/bin/env python3
"""Control probes: interleaved min-of-N wall-time, pristine vs candidate.
Usage: python3 measure_controls.py <candidate> [rounds]
"""
import subprocess, sys, time, os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
os.chdir(ROOT)
cand = sys.argv[1] if len(sys.argv) > 1 else "./dynajs"
pri = "./dynajs.pristine"
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 7
probes = ["i_bitops2M", "i_empty1M", "i_intadd2M", "i_propget1M", "i_localgetput"]

def best(binary, probe):
    b = None
    for _ in range(rounds):
        t0 = time.perf_counter()
        r = subprocess.run([binary, f"tests/agent/regen_probes/{probe}.js"],
                           capture_output=True)
        dt = time.perf_counter() - t0
        if r.returncode != 0:
            print(f"FAIL {probe} rc={r.returncode}: {r.stderr[:200]!r}")
            sys.exit(1)
        if b is None or dt < b:
            b = dt
    return b

for p in probes:
    tp = best(pri, p)
    tc = best(cand, p)
    print(f"{p} pri={tp*1e3:.1f}ms cand={tc*1e3:.1f}ms speedup={tp/tc:.4f}")
