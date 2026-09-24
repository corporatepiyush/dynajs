#!/usr/bin/env python3
# fuzz.py — differential fuzzer: random small scripts exercising the constprop feature grammar,
# run on patched (dynajs), pristine (pre-feature dynajs), and node; stdout compared 3-way.
# Any patched!=pristine diff => SOUNDNESS candidate; patched!=node => REGRESSION candidate.
# Repros of any diff get converted into assert-based probes for the main suite.

import random, subprocess, sys, os, shutil
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
FUZZD = os.path.join(HERE, "fuzz")
os.makedirs(FUZZD, exist_ok=True)

NAMES = ["OP", "op", "_", "$", "argum", "Object1", "globalThis", "constructor", "Q"]  # NaN/undefined illegal at script top level (node-CJS oracle cannot emulate)
SPICY = ["0", "1", "-1", "2", "7", "2147483647", "-2147483648", "2147483648",
         "4294967297", "-0", "1.5", "NaN", "Infinity", "1e21", "9007199254740991",
         "true", "false", "null", "undefined", "\"1\"", "\"x\"", "1n"]
KEYS = ["A", "B", "C", "D", "default", "case", "Object", "freeze", "0"]

FMT = """
function __f(v) {
  if (v === undefined) return "undefined";
  if (v === null) return "null";
  var t = typeof v;
  if (t === "number") { if (v !== v) return "NaN"; if (v === Infinity) return "Infinity"; if (v === -Infinity) return "-Infinity"; if (v === 0 && 1/v === -Infinity) return "-0"; return String(v); }
  if (t === "bigint") return String(v) + "n";
  if (t === "string") return JSON.stringify(v);
  if (t === "boolean") return String(v);
  return "obj";
}
var __n = 0;
function L(x) { console.log("s" + (__n++) + "=" + __f(x)); }
"""


def gen_script(rng):
    nm = rng.choice(NAMES)
    k = rng.randint(1, 8)
    keys = rng.sample(KEYS, min(k, len(KEYS))) + ["K%d" % i for i in range(max(0, k - len(KEYS)))]
    keys = keys[:k]
    vals = [rng.choice(SPICY) for _ in range(k)]
    frozen = rng.random() < 0.6
    pairs = ", ".join("%s: %s" % (kk, vv) for kk, vv in zip(keys, vals))
    decl = "const %s = %s;" % (nm, ("Object.freeze({ %s })" % pairs) if frozen else ("{ %s }" % pairs))
    strict = rng.random() < 0.4

    lines = []
    if strict:
        lines.append('"use strict";')
    lines.append(FMT)

    # random fillers before decl (decl position)
    for _ in range(rng.randint(0, 3)):
        lines.append("var __f%d = %d;" % (rng.randint(0, 99), rng.randint(0, 99)))

    attack = rng.choice(["none", "none", "none", "freeze-swap", "arg-shadow", "let-shadow",
                         "tdz-read", "eval-write", "obj-clobber", "delete-prop"])
    if attack == "arg-shadow":
        # param shadows the name entirely; no const decl at all
        lines.append("function __w(%s) {" % nm)
        shadow_obj = "{ %s }" % ", ".join("%s: %s" % (kk, vv) for kk, vv in zip(keys, vals))
        lines.append("var __shadowarg = %s;" % shadow_obj)
    elif attack == "let-shadow":
        lines.append(decl)
        lines.append("function __w() { { let %s = { %s: 42 }; L(%s.%s); } try { L(%s.%s); } catch (e) { L(\"t\" + e.name); } return \"w\"; }"
                     % (nm, keys[0], nm, keys[0], nm, keys[0]))
    else:
        lines.append(decl)

    # switch construction
    ncases = rng.randint(0, 6)
    labels = []
    for _ in range(ncases):
        if rng.random() < 0.65:
            labels.append("%s.%s" % (nm, rng.choice(keys)))
        else:
            labels.append(rng.choice(SPICY[:10]))
    order = rng.sample(range(len(labels)), len(labels))
    body_parts = []
    for idx in order:
        fall = " " if rng.random() < 0.15 else " break; "
        body_parts.append('case %s: L("%d");%s' % (labels[idx], idx, fall))
    has_default = rng.random() < 0.7
    default_pos = rng.randint(0, len(body_parts))
    default_stmt = 'default: L("d"); %s' % ("break; " if rng.random() < 0.6 else "")
    parts = list(body_parts)
    parts.insert(default_pos, default_stmt) if has_default else None
    disc = rng.choice(["%s.%s" % (nm, keys[0]), rng.choice(SPICY[:10]), "undefined"])
    sw = "try { switch (%s) { %s } } catch (e) { L(\"thr:\" + e.name); }" % (disc, " ".join(parts))
    lines.append(sw)

    # extra use sites
    for _ in range(rng.randint(1, 4)):
        use = rng.choice([
            "L(%s.%s);" % (nm, rng.choice(keys)),
            "L(%s[\"%s\"]);" % (nm, rng.choice(keys)),
            "L(%s.%s + 1);" % (nm, keys[0]),
            "L(typeof %s);" % nm,
            "L(Object.isFrozen(%s));" % nm,
        ])
        lines.append("try { %s } catch (e) { L(\"thr:\" + e.name); }" % use)

    # attack payload
    if attack == "freeze-swap":
        lines.append("var __sv = Object.freeze; Object.freeze = function (o) { return o; };")
        lines.append("try { L(%s.%s); } catch (e) { L(\"thr:\" + e.name); }" % (nm, keys[0]))
        lines.append("Object.freeze = __sv;")
    elif attack == "eval-write":
        lines.append("try { eval(\"%s.%s = 99\"); L(%s.%s); } catch (e) { L(\"thr:\" + e.name); }"
                     % (nm, keys[0], nm, keys[0]))
    elif attack == "obj-clobber":
        lines.append("try { (0,eval)(\"this.Object = 1\"); L(%s.%s); } catch (e) { L(\"thr:\" + e.name); }"
                     % (nm, keys[0]))
        lines.append("var __r1; try { const QQ = Object.freeze({ Z: 1 }); __r1 = \"init-ok\"; } catch (e) { __r1 = \"t:\" + e.name; }")
        lines.append("var __r2; try { __r2 = QQ.Z; } catch (e) { __r2 = \"t:\" + e.name; }")
        lines.append("L(__r1); L(__r2);")
    elif attack == "delete-prop":
        lines.append("try { L(delete %s.%s); L(%s.%s); } catch (e) { L(\"thr:\" + e.name); }"
                     % (nm, keys[0], nm, keys[0]))
    elif attack == "tdz-read":
        pass  # decl already emitted; add a second decl+pre-read in nested scope
    if attack == "tdz-read":
        lines.append("(function () { var __o = \"x\"; try { __o = \"pre:\" + ZZ.A; } catch (e) { __o = \"t:\" + e.name; } try { const ZZ = Object.freeze({ A: 5 }); __o += \"|post:\" + ZZ.A; } catch (e) { __o += \"|t:\" + e.name; } L(__o); })();")
    if attack == "arg-shadow":
        lines.append("}")
        lines.append("try { L(__w(__shadowarg)); } catch (e) { L(\"thr:\" + e.name); }")
        lines.append("try { L(__w(undefined)); } catch (e) { L(\"thr:\" + e.name); }")
    elif attack == "let-shadow":
        lines.append("try { L(__w()); } catch (e) { L(\"thr:\" + e.name); }")

    return "\n".join(lines) + "\n"


def run_one(args):
    i, src = args
    path = os.path.join(FUZZD, "f%05d.js" % i)
    with open(path, "w") as f:
        f.write(src)
    tmo = 10

    def go(cmd, stdin_file=None):
        try:
            if stdin_file:
                with open(stdin_file, "rb") as fh:
                    return subprocess.run(cmd, stdin=fh, capture_output=True, text=True, timeout=tmo)
            return subprocess.run(cmd, capture_output=True, text=True, timeout=tmo)
        except subprocess.TimeoutExpired:
            return None

    pd = go([os.path.join(TREE, "dynajs"), path])
    pr = go([os.path.join(TREE, "dynajs.prefeature"), path])
    nd = go(["node", path])
    pd_o = (pd.stdout, pd.returncode) if pd else ("TIMEOUT", -99)
    pr_o = (pr.stdout, pr.returncode) if pr else ("TIMEOUT", -99)
    nd_o = (nd.stdout, nd.returncode) if nd else ("TIMEOUT", -99)
    return i, pd_o, pr_o, nd_o


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 20260911
    rng = random.Random(seed)
    scripts = [(i, gen_script(rng)) for i in range(n)]
    diffs = []
    import time
    last_beat = time.time()
    with ThreadPoolExecutor(max_workers=8) as ex:
        for i, pd, pr, nd in ex.map(run_one, scripts):
            if time.time() - last_beat > 180:
                open(os.path.join(TREE, "heartbeat"), "w").close()
                last_beat = time.time()
            if pd != pr:
                diffs.append((i, "SOUNDNESS-CAND", pd, pr, nd))
            elif pd != nd:
                diffs.append((i, "REGRESSION-CAND", pd, pr, nd))
    print("fuzz: %d scripts, seed=%d" % (n, seed))
    print("diffs: %d" % len(diffs))
    for d in diffs[:20]:
        print("  f%05d %s" % (d[0], d[1]))
        print("    patched : %r" % (d[2],))
        print("    pristine: %r" % (d[3],))
        print("    node    : %r" % (d[4],))
    with open(os.path.join(HERE, "fuzz_result.txt"), "w") as f:
        f.write("n=%d seed=%d diffs=%d\n" % (n, seed, len(diffs)))
        for d in diffs:
            f.write("f%05d %s\n" % (d[0], d[1]))
    sys.exit(1 if diffs else 0)


if __name__ == "__main__":
    main()
