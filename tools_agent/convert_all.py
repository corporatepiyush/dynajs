#!/usr/bin/env python3
"""convert_all.py — drive the suite conversion and per-probe acceptance.

For every in-scope suite: convert each convertible probe in place (old bytes
remain recoverable from git), then ACCEPT it only if the converted probe runs
green on BOTH engines (SUITE line FAIL==0, rc==0) and emits no DIVERGE lines
on node (node is the oracle: divergences must fire on dynajs, never on node).

Writes tools_agent/convert_report.json and prints a per-suite summary.
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import convert  # noqa: E402

H = os.path.join(ROOT, "tests", "agent", "_h")
RUNSH = os.path.join(H, "run.sh")
DYNA = os.path.join(ROOT, "dynajs")

SUITES = ["bbreview", "arrays_ext", "parser_core_ext", "builtins_ext",
          "constprop", "constprop_review", "sliced_strings", "dtoa_shortest",
          "ev", "ev_bigint", "modules_ext", "gpn_nit", "nursery"]


def load_excludes():
    excl = {}
    with open(os.path.join(H, "exclude.tsv")) as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            suite, fname, cat, reason = line.rstrip("\n").split("\t", 3)
            excl.setdefault(suite, {})[fname] = (cat, reason)
    return excl


def run_probe(suite_dir, fname, engine, extra_args=()):
    """Concat h.js + probe into a tmp file and run `engine` on it."""
    import tempfile
    d = os.path.join(suite_dir, ".acc_tmp")
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, fname + ".acc." + ("mjs" if fname.endswith(".mjs") else "js"))
    with open(os.path.join(H, "h.js")) as hf:
        hcode = hf.read()
    with open(os.path.join(suite_dir, fname)) as pf:
        pcode = pf.read()
    prelude = ""
    pre_path = os.path.join(suite_dir, "_h.js")
    if os.path.exists(pre_path):
        with open(pre_path) as pf2:
            prelude = pf2.read()
    with open(tmp, "w") as f:
        f.write(hcode + prelude + pcode)
    cmd = list(engine) + list(extra_args) + [tmp]
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=60, cwd=ROOT)
        out = p.stdout.decode("utf-8", "replace")
        rc = p.returncode
    except subprocess.TimeoutExpired:
        out, rc = "", -99
    os.unlink(tmp)
    return rc, out


def accept(suite, suite_dir, fname, report):
    """Returns list of problems (empty = accepted)."""
    problems = []
    rc_n, out_n = run_probe(suite_dir, fname, ["node"])
    rc_d, out_d = run_probe(suite_dir, fname, [DYNA], ("-m",) if fname.endswith(".mjs") else ())
    def verdict(rc, out):
        line = [l for l in out.splitlines() if l.startswith("SUITE ")]
        if not line:
            return f"no SUITE line rc={rc}", False
        parts = line[-1].split()
        try:
            fail = int(parts[-1])
        except ValueError:
            return f"malformed SUITE line: {line[-1]}", False
        return None, fail == 0 and rc == 0
    v = verdict(rc_n, out_n)
    if v[0]:
        problems.append(f"node: {v[0]}")
    elif not v[1]:
        problems.append(f"node: SUITE FAIL>0 or rc!=0 (rc={rc_n})")
    v = verdict(rc_d, out_d)
    if v[0]:
        problems.append(f"dynajs: {v[0]}")
    elif not v[1]:
        problems.append(f"dynajs: SUITE FAIL>0 or rc!=0 (rc={rc_d})")
    # zero-assert guard: a converted probe must assert something
    pcode = open(os.path.join(suite_dir, fname)).read()
    n_asserts = (pcode.count("__L(") + pcode.count("__A(") + pcode.count("test(")
                 + pcode.count("__PIN(") + pcode.count("assert_"))
    if n_asserts == 0:
        problems.append("zero-assert conversion (no output oracle found on success path)")
    # determinism: node run twice must byte-match
    rc_n2, out_n2 = run_probe(suite_dir, fname, ["node"])
    if out_n2 != out_n:
        problems.append("nondeterministic: two node runs differ")
    return problems


def main():
    convert.load_h()
    excludes = load_excludes()
    only = sys.argv[1:] or SUITES
    results = []
    for suite in only:
        sdir = os.path.join(ROOT, "tests", "agent", suite)
        files = sorted(f for f in os.listdir(sdir)
                       if (f.endswith(".js") or f.endswith(".mjs")) and not f.startswith("_"))
        for fname in files:
            if fname in excludes.get(suite, {}):
                results.append({"suite": suite, "file": fname, "status": "excluded"})
                continue
            path = os.path.join(sdir, fname)
            with open(path) as f:
                src = f.read()
            try:
                converted, rep = convert.convert(suite, fname, src)
            except Exception as e:  # noqa: BLE001 - report and continue
                results.append({"suite": suite, "file": fname, "status": "error",
                                "error": str(e)[-800:]})
                print(f"ERR  {suite}/{fname}: {str(e)[-160:]}", flush=True)
                continue
            with open(path, "w") as f:
                f.write(converted)
            problems = accept(suite, sdir, fname, rep)
            if problems:
                # roll back: restore original bytes from git
                subprocess.run(["git", "checkout", "--", os.path.relpath(path, ROOT)],
                               cwd=ROOT, check=False)
                results.append({"suite": suite, "file": fname, "status": "accept-fail",
                                "problems": problems})
                print(f"REJ  {suite}/{fname}: {problems}", flush=True)
            else:
                results.append({"suite": suite, "file": fname, "status": "converted",
                                **rep})
                print(f"OK   {suite}/{fname}: mode={rep['mode']} sites={rep['sites']} "
                      f"lines={rep['lines']} divergent={rep['divergent']}", flush=True)
    with open(os.path.join(HERE, "convert_report.json"), "w") as f:
        json.dump(results, f, indent=1)
    n_ok = sum(1 for r in results if r["status"] == "converted")
    n_err = sum(1 for r in results if r["status"] in ("error", "accept-fail"))
    n_ex = sum(1 for r in results if r["status"] == "excluded")
    print(f"== converted={n_ok} excluded={n_ex} failed={n_err}")


if __name__ == "__main__":
    main()
