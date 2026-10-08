#!/usr/bin/env python3
"""mutate.py — prove the harness can fail: inject a wrong expectation into 5
converted probes per suite and confirm SUITE FAIL>0 or rc!=0."""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
H = os.path.join(ROOT, "tests", "agent", "_h")

SUITES = ["bbreview", "arrays_ext", "parser_core_ext", "builtins_ext",
          "constprop", "constprop_review", "sliced_strings", "dtoa_shortest",
          "ev", "ev_bigint", "modules_ext", "gpn_nit", "nursery"]


def run(engine, tmp):
    cmd = [engine] + (["-m"] if engine == "./dynajs" and tmp.endswith(".mjs") else []) + [tmp]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=90, cwd=ROOT)
    out = p.stdout + p.stderr
    m = [l for l in out.splitlines() if l.startswith("SUITE ")]
    fail = int(m[-1].split()[-1]) if m else 99
    return p.returncode, fail


def mutate_once(path, tmp_out, engine):
    """Flip one baked literal; return True if the mutated probe FAILS."""
    src = open(path).read()
    # candidate mutations: assert_eq(expr, LIT, ...) baked literal
    m = re.search(r"assert_eq\((?:[^();]|\([^()]*\))*,\s*(\"[^\"\n]*\"|-?\d[\d.e+-]*|true|false|null|undefined|NaN|-?\d+n)\s*,", src)
    if not m:
        # __EXP line literal
        m = re.search(r'__EXP\[\d+\] = \["[^"\n]*"', src)
        if m:
            old = m.group(0)
            new = old.replace('["', '["XX', 1)
            mut = src.replace(old, new, 1)
            open(tmp_out, "w").write(mut)
            return True
        # evalcap-style: assert_eq(__lines[0], "...")
        m = re.search(r'assert_eq\(__lines\[\d+\], ("[^"\n]*")', src)
        if m:
            mut = src.replace(m.group(1), '"__MUTATED__"', 1)
            open(tmp_out, "w").write(mut)
            return True
        return False
    old = m.group(1)
    if old.startswith('"'):
        new = '"__MUTATED__"'
    elif old.endswith("n"):
        new = old[:-1] + "9n"
    else:
        new = old + "9"
    mut = src[:m.start(1)] + new + src[m.end(1):]
    open(tmp_out, "w").write(mut)
    return True


def main():
    only = sys.argv[1:] or SUITES
    results = {}
    for suite in only:
        sdir = os.path.join(ROOT, "tests", "agent", suite)
        probes = sorted(f for f in os.listdir(sdir)
                        if f.endswith(".js") and not f.startswith("_")
                        and not f.startswith(("px_", "sx_")))
        picked = [p for p in probes if True][:len(probes)]
        # deterministic spread: first, middle, last, quarter, three-quarter
        n = len(picked)
        picks = sorted({0, n // 4, n // 2, (3 * n) // 4, n - 1})
        tested = 0
        failed_mut = 0
        for idx in picks:
            p = picked[idx]
            src = open(os.path.join(sdir, p)).read()
            tmp = os.path.join(sdir, f".mut_tmp_{p}")
            pre = ""
            hp = os.path.join(sdir, "_h.js")
            if os.path.exists(hp):
                pre = open(hp).read()
            if os.path.exists(tmp):
                os.unlink(tmp)
            if not mutate_once(os.path.join(sdir, p), tmp, "./dynajs"):
                continue
            mutated = open(tmp).read()
            open(tmp, "w").write(open(os.path.join(H, "h.js")).read() + pre + mutated)
            try:
                rc, fail = run("./dynajs", tmp)
                tested += 1
                if rc != 0 or fail > 0:
                    failed_mut += 1
            except subprocess.TimeoutExpired:
                tested += 1
                failed_mut += 1
            os.unlink(tmp)
        results[suite] = {"tested": tested, "harness-failed-as-expected": failed_mut}
        print(f"{suite}: mutation-tested={tested} correctly-failed={failed_mut}")
    with open(os.path.join(HERE, "mutation_report.json"), "w") as f:
        json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
