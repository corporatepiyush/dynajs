#!/usr/bin/env python3
"""equality.py — compare OLD suite pass/fail sets (baseline output-diff runs)
against NEW (converted, assert-harness) results, per engine, per probe.

OLD classes (from _baseline + old runner semantics):
  pass          node rc==0 (node is the oracle); dynajs rc==0 AND output byte-equal to node
  diverge-fail  dynajs rc==0 but output differs from node (engine divergence)
  fail          rc!=0 on that engine
  crash-agree   rc!=0 on BOTH engines with identical stdout (crash probes whose
                contract was "same crash") — converted to explicit throw contracts
Special: sliced_strings old runs need the _h.js prelude (handled below);
         parser_core_ext p0* were WHITELIST-DIVERGE in the old runner (wlist).

NEW classes (from tests/agent/<suite>/out/<engine>/):
  pass / diverge-pass / fail
"""
import json
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUITES = ["bbreview", "arrays_ext", "parser_core_ext", "builtins_ext",
          "constprop", "constprop_review", "sliced_strings", "dtoa_shortest",
          "ev", "ev_bigint", "modules_ext", "gpn_nit", "nursery"]
WL = {"parser_core_ext"}  # suites whose old runner had a WHITELIST-DIVERGE class


def load_excludes():
    excl = {}
    with open(os.path.join(ROOT, "tests", "agent", "_h", "exclude.tsv")) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                s, fn, cat, reason = line.rstrip("\n").split("\t", 3)
                excl.setdefault(s, {})[fn] = reason
    return excl


def old_status(suite, probe):
    d = os.path.join(ROOT, "_baseline", suite)
    p = probe
    if suite == "sliced_strings":
        # old runner: concat _h.js + probe, grep ^PASS, rc==0
        res = {}
        for eng, cmd in (("node", ["node"]), ("dynajs", ["./dynajs"])):
            tmp = os.path.join(ROOT, "tools_agent", "_capture_tmp", f"old_{p}")
            orig = subprocess.run(["git", "show", f"HEAD:tests/agent/sliced_strings/{p}"],
                                  capture_output=True, text=True, cwd=ROOT).stdout
            with open(tmp, "w") as f:
                f.write(open(os.path.join(ROOT, "tests/agent/sliced_strings/_h.js")).read())
                f.write(orig)
            r = subprocess.run(cmd + [tmp], capture_output=True, text=True, timeout=60, cwd=ROOT)
            res[eng] = "pass" if (r.returncode == 0 and any(l.startswith("PASS") for l in r.stdout.splitlines())) else "fail"
            os.unlink(tmp)
        return res
    try:
        rc_n = int(open(f"{d}/{p}.node.rc").read().strip())
        rc_d = int(open(f"{d}/{p}.dynajs.rc").read().strip())
        on = open(f"{d}/{p}.node.out", encoding="utf8", errors="replace").read()
        od = open(f"{d}/{p}.dynajs.out", encoding="utf8", errors="replace").read()
    except FileNotFoundError:
        return None
    st = {}
    st["node"] = "pass" if rc_n == 0 else "fail"
    if rc_d == 0 and od == on:
        st["dynajs"] = "pass"
    elif rc_d == 0:
        st["dynajs"] = "diverge-fail"
    else:
        st["dynajs"] = "fail" if rc_n == 0 else "crash-agree"
        if rc_n != 0:
            st["node"] = "crash-agree" if rc_n != 0 else st["node"]
    return st


def new_status(suite, probe):
    res = {}
    for eng in ("node", "dynajs"):
        f = os.path.join(ROOT, "tests", "agent", suite, "out", eng, probe + ".txt")
        try:
            txt = open(f, encoding="utf8", errors="replace").read()
        except FileNotFoundError:
            res[eng] = "missing"
            continue
        rc_line = [l for l in txt.splitlines() if l.startswith("__RC__=")]
        suite_line = [l for l in txt.splitlines() if l.startswith("SUITE ")]
        rc = int(rc_line[-1].split("=")[1]) if rc_line else 99
        fail = int(suite_line[-1].split()[-1]) if suite_line else 99
        if rc == 0 and fail == 0:
            res[eng] = "diverge-pass" if any(l.startswith("DIVERGE ") for l in txt.splitlines()) else "pass"
        else:
            res[eng] = "fail"
    return res


def main():
    excl = load_excludes()
    table = {}
    problems = []
    for suite in SUITES:
        sdir = os.path.join(ROOT, "tests", "agent", suite)
        probes = sorted(f for f in os.listdir(sdir)
                        if (f.endswith(".js") or f.endswith(".mjs")) and not f.startswith("_"))
        rows = []
        for p in probes:
            if p in excl.get(suite, {}) or p.startswith(("px_", "sx_")):
                continue  # parametric-added probes have no old oracle by definition
            o = old_status(suite, p)
            n = new_status(suite, p)
            rows.append({"probe": p, "old": o, "new": n})
            if o is None:
                problems.append((suite, p, "no-baseline"))
                continue
            for eng in ("node", "dynajs"):
                oc, nc = o[eng], n[eng]
                if oc == "pass" and nc == "pass":
                    continue
                if oc == "pass" and nc == "diverge-pass":
                    # old suite byte-oracle differences are explicit DIVERGE markers now
                    # (only possible if node/dynajs outputs differed in old too — flagged below)
                    problems.append((suite, p, f"{eng}: old=pass new=diverge-pass"))
                elif oc == "diverge-fail" and nc == "diverge-pass":
                    continue  # documented mapping
                elif oc == "crash-agree" and nc in ("pass", "diverge-pass"):
                    continue  # documented mapping (throw contract)
                elif oc == "fail" and nc in ("pass", "diverge-pass"):
                    if suite in WL:
                        continue  # whitelist-diverge probes
                    problems.append((suite, p, f"{eng}: old=fail new={nc}"))
                elif oc == "pass" and nc == "fail":
                    problems.append((suite, p, f"{eng}: REGRESSION old=pass new=fail"))
                elif oc == "diverge-fail" and nc == "fail":
                    problems.append((suite, p, f"{eng}: old=diverge-fail new=fail (acceptable but check)"))
                # fail->fail, crash-agree->fail etc are consistent
        table[suite] = rows
    with open(os.path.join(ROOT, "tools_agent", "equality_report.json"), "w") as f:
        json.dump(table, f, indent=1)
    n_ok = sum(1 for s in table for r in table[s] if not any(p[0] == s and p[1] == r["probe"] for p in problems))
    print("== equality problems:", len(problems))
    for p in problems[:40]:
        print("  ", p)
    # counts
    from collections import Counter
    cnt = Counter()
    for s in table:
        for r in table[s]:
            for eng in ("node", "dynajs"):
                cnt[(r["old"][eng] if r["old"] else "none", r["new"][eng])] += 1
    for k, v in sorted(cnt.items()):
        print(f"  old={k[0]:<14} new={k[1]:<14} n={v}")


if __name__ == "__main__":
    main()
