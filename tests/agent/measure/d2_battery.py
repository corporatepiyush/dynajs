#!/usr/bin/env python3
"""D2 battery: interleaved min-of-7 A/B between two engine builds.

Usage: d2_battery.py <binaryA> <labelA> <binaryB> <labelB> [binaryC labelC ...]
Program set (the standard battery):
  - tests/agent/bbreview/*.js (54 committed task programs, via _h/h.js prelude)
  - tests/agent/regen_probes/i_*.js (interpreter kernels)
  - bench/ task kernels: markdown, strscan, xmlparse, stdlib_all, http_parse
Every invocation runs under timeout; per program we report min and median of
the 7 reps per binary, plus the ratio minB/minA. Interleaving is per-program,
per-rep (A,B,B,A... strictly alternating) so thermal drift hits both legs.
"""
import subprocess, sys, time, os, glob, statistics, math

TREE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
os.chdir(TREE)   # script lives at tests/agent/measure/d2_battery.py (4 levels deep) -> tree root

TIMEOUT = 90
REPS = 7

def program_set():
    progs = []
    h = "tests/agent/_h/h.js"
    for f in sorted(glob.glob("tests/agent/bbreview/*.js")):
        progs.append(("bb/" + os.path.basename(f), None, f, h))   # concat prelude
    for f in sorted(glob.glob("tests/agent/regen_probes/i_*.js")):
        progs.append(("rp/" + os.path.basename(f), None, f, None))
    for f in ["bench/bench_markdown.js", "bench/bench_strscan.js",
              "bench/bench_xmlparse.js", "bench/bench_stdlib_all.js"]:
        # http_parse.js excluded: >120s per invocation on the BASELINE, it
        # would dominate the battery wall-clock (7 reps x legs).
        if os.path.exists(f):
            progs.append(("bench/" + os.path.basename(f), None, f, None))
    return progs

def run_one(binary, cmdline, cwd_need):
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmdline, capture_output=True, timeout=TIMEOUT, cwd=cwd_need)
        dt = time.monotonic() - t0
        return dt, p.returncode
    except subprocess.TimeoutExpired:
        return TIMEOUT + 1.0, -99

def main():
    args = sys.argv[1:]
    if len(args) % 2 != 0:
        print("usage: d2_battery.py binA labelA binB labelB [...]")
        return 2
    legs = [(args[i], args[i + 1]) for i in range(0, len(args), 2)]
    for b, _ in legs:
        if not os.path.exists(b):
            print(f"missing binary {b}"); return 2

    rows = []
    for label, pre, path, concat in program_set():
        times = {l: [] for _, l in legs}
        # smoke: run leg A once to detect non-running programs
        cmd = [legs[0][0], path] if concat is None else ["sh", "-c",
               f"cat {concat} {path} > scratch/bat_concat.js && exec {legs[0][0]} scratch/bat_concat.js"]
        dt, rc = run_one(legs[0][0], cmd if concat is None else cmd, os.getcwd())
        if rc != 0:
            rows.append((label, None, None, "skip(rc=%d)" % rc))
            continue
        for rep in range(REPS):
            for binary, lab in legs:
                if concat is None:
                    cmd = [binary, path]
                else:
                    cmd = ["sh", "-c",
                           f"cat {concat} {path} > scratch/bat_concat.js && exec {binary} scratch/bat_concat.js"]
                dt, rc = run_one(binary, cmd, os.getcwd())
                if rc != 0:
                    dt = float("nan")
                times[lab].append(dt)
        ok = all(not any(math.isnan(x) for x in times[l]) for _, l in legs)
        entry = {"label": label}
        for _, lab in legs:
            v = [x for x in times[lab] if not math.isnan(x)]
            entry[lab] = (min(v), statistics.median(v)) if v else (float("nan"), float("nan"))
        entry["ok"] = ok
        rows.append((label, entry, times, ""))

    la, lb = legs[0][1], legs[1][1]
    print(f"{'program':38} {'n':>2} " + " ".join(f"{min(la,lb,key=len):>0}" for _ in legs))
    hdr = f"{'program':38} {'n':>2}"
    for _, lab in legs:
        hdr += f"  {lab + '/min':>10} {lab + '/med':>10}"
    hdr += f"  {'x=minB/minA':>11}"
    print(hdr)
    ratios = []
    totals = {lab: 0.0 for _, lab in legs}
    for label, entry, times, note in rows:
        if entry is None:
            print(f"{label:38}    {note}")
            continue
        line = f"{label:38} {'y' if entry['ok'] else 'p'}"
        for _, lab in legs:
            line += f"  {entry[lab][0]*1000:9.1f}m {entry[lab][1]*1000:9.1f}m"
        mn_a, mn_b = entry[la][0], entry[lb][0]
        if mn_a and mn_a == mn_a and mn_b == mn_b and mn_a > 0:
            r = mn_b / mn_a
            ratios.append(r)
            line += f"  {r:11.4f}"
        else:
            line += f"  {'n/a':>11}"
        print(line)
        for _, lab in legs:
            totals[lab] += entry[lab][0]
    print("\n== summary (min-of-7) ==")
    for _, lab in legs:
        print(f"total {lab:14}: {totals[lab]:8.2f}s")
    if ratios:
        gm = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
        print(f"bbreview+progs geomean x({lb}/{la}): {gm:.4f}  ({(gm-1)*100:+.2f}%)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
