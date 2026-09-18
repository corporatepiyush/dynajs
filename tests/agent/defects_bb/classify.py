#!/usr/bin/env python3
"""Classify 3-way (pristine / patched / node) probe results.

Reads out/<engine>/<matrix>/<id>.txt plus probes/<matrix>/manifest.tsv and
writes summary.tsv + a class tally. Exit nonzero on REGRESSION or GEN-ERR.
"""
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
MATRICES = ["x1", "x2", "x3", "x5", "x6"]
TAG_CLASS = {
    "residual-r1": "residual-confirms",
    "residual-r2": "residual-confirms",
    "ticket-x6": "ticket-x6",
}


def probe_kind(matrix, pid):
    """assert-probe vs rc-probe (x1 file-level compile rejection rows)."""
    for suffix in (".js.expect", ".mjs.expect"):
        exp = os.path.join(HERE, "probes", matrix, pid + suffix)
        if os.path.exists(exp):
            return "rc-nonzero"
    return "assert"


def read_result(path, kind):
    """-> (ok: bool, text). ok = rc==0 and HARNESS fail=0 (assert probes),
    or rc!=0 with no HARNESS line (rc probes: whole-file compile rejection)."""
    try:
        with open(path) as f:
            txt = f.read()
    except OSError:
        return False, "MISSING-OUTPUT"
    lines = [l for l in txt.splitlines() if l.strip()]
    rc = None
    for l in reversed(lines):
        if l.startswith("RC="):
            try:
                rc = int(l[3:])
            except ValueError:
                rc = None
            break
    if rc is None:
        return False, "NO-RC (timeout or crash)"
    body = "\n".join(lines[:-1]) if lines else ""
    if rc == 0:
        ok = ("HARNESS pass=" in body) and (" fail=0" in body.split("HARNESS ")[-1].splitlines()[0])
        return ok, "rc0"
    if kind == "rc-nonzero":
        ok = "HARNESS" not in body  # must not have partially executed
        return ok, "rc-nonzero"
    return False, "rc%s" % rc


def failed_names(path):
    try:
        with open(path) as f:
            txt = f.read()
    except OSError:
        return []
    names = []
    for l in txt.splitlines():
        if l.startswith("FAILED: "):
            names.append(l[len("FAILED: "):].split(":")[0].strip())
    return names


def main():
    rows = []
    tally = Counter()
    generr = 0
    for m in MATRICES:
        mpath = os.path.join(HERE, "probes", m, "manifest.tsv")
        if not os.path.exists(mpath):
            continue
        with open(mpath) as f:
            for line in f:
                pid, dims, tag = line.rstrip("\n").split("\t")
                d = os.path.join(HERE, "out")
                kind = probe_kind(m, pid)
                n_ok, n_txt = read_result(os.path.join(d, "node", m, pid + ".txt"), kind)
                p_ok, p_txt = read_result(os.path.join(d, "dynajs", m, pid + ".txt"), kind)
                pr_ok, pr_txt = read_result(os.path.join(d, "pristine", m, pid + ".txt"), kind)
                if not n_ok:
                    cls = "GEN-ERR"
                    generr += 1
                elif p_ok and pr_ok:
                    cls = "PASS"
                elif p_ok and not pr_ok:
                    cls = "IMPROVED"
                elif not p_ok and pr_ok:
                    cls = "REGRESSION"
                else:
                    if tag in TAG_CLASS:
                        cls = TAG_CLASS[tag]
                    else:
                        po = open(os.path.join(d, "dynajs", m, pid + ".txt")).read()
                        pro = open(os.path.join(d, "pristine", m, pid + ".txt")).read()
                        cls = "PRE-EXISTING" if po == pro else "PRE-EXISTING-VAR"
                if cls in ("PRE-EXISTING", "PRE-EXISTING-VAR") and m == "x6":
                    # distinguish comparator-call-count-only nits from real state drift
                    fn = failed_names(os.path.join(d, "dynajs", m, pid + ".txt"))
                    if fn and all(x in ("calls",) for x in fn):
                        cls = "NIT-CALLCOUNT"
                tally[cls] += 1
                rows.append((m, pid, dims, tag, n_txt, p_txt, pr_txt, cls))
    with open(os.path.join(HERE, "summary.tsv"), "w") as f:
        f.write("matrix\tid\tdims\ttag\tnode\tpatched\tpristine\tclass\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    print("CLASS TALLY:")
    for k, v in sorted(tally.items()):
        print("  %-20s %d" % (k, v))
    print("total probes: %d" % len(rows))
    bad = tally.get("REGRESSION", 0) + tally.get("GEN-ERR", 0)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
