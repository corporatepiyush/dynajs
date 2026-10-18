#!/usr/bin/env python3
"""gen_param.py — parametric assert-probe expansion for constprop + sliced_strings.

Emits >= 300 extra assert-based probes from small matrices, expectations baked
from the node oracle (v22) at generation time via a single baker run per suite.
Deterministic; bounded loops; both engines must agree by spec (any disagreement
surfaces as an assert failure, which is the point).

  constprop/px_*.js    : switch dispatch over const members — counts x variants
  sliced_strings/sx_*.js: sliced-string operations — lengths x widths x ops
"""
import json
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NODE = "node"

COUNTS = [1, 2, 3, 5, 8, 17, 31, 50, 100, 127, 128, 200, 255, 256, 257, 300, 1000]
VARIANTS = ["plain", "frozen", "write", "str", "big"]
LENGTHS = [0, 1, 63, 64, 65, 127, 128, 255, 1000]
WIDTHS = ["latin1", "wide", "astral"]
OPS = ["charAt", "at", "index", "indexOf", "slice", "substring", "json", "template", "mapkey"]

SWITCH_HEAD = """// parametric probe (gen_param.py): switch over const object with {n} members,
// variant {v}. Expectations baked from the node oracle at generation time.
"use strict" if (false); void 0;
"""


def switch_probe(n, v):
    members = ", ".join(f'K{i}: {i} * 2' for i in range(n))
    if v == "str":
        members = ", ".join(f'S{i}: "s{i}"' for i in range(n))
    if v == "big":
        members = ", ".join(f'B{i}: {i}n' for i in range(n))
    obj = "Object.freeze({ " + members + " })" if v == "frozen" else ("{ " + members + " }")
    lines = []
    lines.append(f"const OP = {obj};")
    if v == "write":
        lines.append(f"try {{ OP.K0 = 999{'n' if v == 'big' else ''}; }} catch (e) {{}}")
    # dispatch function
    cases = []
    for i in range(n):
        if v == "str":
            cases.append(f'        case OP.S{i}: return "s" + {i};')
        elif v == "big":
            cases.append(f"        case OP.B{i}: return {i}n + 1n;")
        else:
            cases.append(f"        case OP.K{i}: return x + {i};")
    lines.append("function dispatch(op, x) {\n    switch (op) {\n" + "\n".join(cases) + "\n        default: return -1;\n    }\n}")
    lines.append("function key(i) { return OP[Object.keys(OP)[i]]; }")
    return "\n".join(lines)


def switch_case_code(n, v):
    """body: list of (testname, exprsrc) asserts"""
    asserts = []
    hits = min(n, 60)  # bound work per probe: first hits + a tail hit + miss
    idxs = list(range(hits))
    if n > hits:
        idxs.append(n - 1)
    for i in idxs:
        if v == "str":
            asserts.append((f"hit{i}", f'dispatch(OP.S{i}, 0)', f'"s{i}"' if False else None))
        elif v == "big":
            asserts.append((f"hit{i}", f"dispatch(OP.B{i}, 0)", None))
        else:
            asserts.append((f"hit{i}", f"dispatch(OP.K{i}, 10)", None))
    asserts.append(("miss", "dispatch(undefined, 5)", None))
    if v != "big" and v != "str":
        asserts.append(("arith", "dispatch(key(3), 1)", None))
    asserts.append(("keycount", "Object.keys(OP).length", None))
    asserts.append(("frozentype", "typeof OP", None))
    return asserts


def string_probe(length, width, op):
    lines = []
    if width == "latin1":
        body = '        s += String.fromCharCode(97 + (i % 26));'
    elif width == "wide":
        body = '        s += String.fromCharCode(0x100 + ((i * 7) % 500));'
    else:
        body = '        s += String.fromCodePoint(0x1F600 + (i % 8));'
    fn = ("function mkParent(n) {\n"
          '    var s = "";\n'
          "    for (var i = 0; i < n; i++) {\n"
          + body + "\n"
          "    }\n"
          "    return s;\n"
          "}")
    lines.append(fn)
    lines.append("var parent = mkParent(1200);")
    # slice of the parent (sliced-string pressure): mid-window of target length
    if width == "astral":
        lines.append("var s = parent.slice(0, " + str(max(length, 1)) + ");")
    else:
        lines.append("var s = parent.slice(100, 100 + " + str(length) + ");")
    return "\n".join(lines)


def string_case_code(length, width, op):
    a = []
    k = min(3, max(length - 1, 0))
    if op == "charAt":
        a.append(("charat0", "s.charAt(0)", None))

        a.append(("charat-k", f"s.charAt({k})", None))

        a.append(("charat-oob", f"s.charAt({length + 5})", None))

    elif op == "at":
        a.append(("at0", "s.at(0)", None))

        a.append(("at-neg", "s.at(-1)", None))

        a.append(("at-oob", f"s.at({length + 5})", None))

    elif op == "index":
        a.append(("idx0", "s[0]", None))

        a.append(("idxk", f"s[{k}]", None))

        a.append(("idx-oob", f"s[{length + 5}]", None))

    elif op == "indexOf":
        a.append(("indexOf-head", "s.indexOf(s.charAt(0))" if length else "s.indexOf('x')", None))

        kk = max(1, min(3, length))
        a.append(("indexOf-sub", f"s.indexOf(s.slice(0, {kk}))" if length else "s.indexOf('x')", None))

        a.append(("indexOf-miss", "s.indexOf('\\u0001')", None))
    elif op == "slice":
        a.append(("slice01", "s.slice(0, 1)", None))

        a.append(("slice-neg", "s.slice(-2)", None))

        a.append(("slice-mid", f"s.slice(1, {max(1, k)})", None))

        a.append(("slice-len", "s.length", None))

    elif op == "substring":
        a.append(("substring01", "s.substring(0, 1)", None))

        a.append(("substring-mid", f"s.substring(0, {max(0, k)})", None))

        a.append(("substring-swap", f"s.substring({max(0, k)}, 0)", None))

    elif op == "json":
        a.append(("json", "JSON.stringify(s)", None))

    elif op == "template":
        a.append(("template", "`t:" + "${s}" + "`", None))

        a.append(("template-len", "`l:" + "${s.length}" + "`", None))

    elif op == "mapkey":
        a.append(("mapkey", "(function(){ var m = new Map(); m.set(s, 7); return m.get(s); })()", None))

        a.append(("mapsize", "(function(){ var m2 = new Map(); m2.set(s.slice(0, 1), 1); return m2.size; })()", None))

    a.append(("len", "s.length", None))

    return a


def build_constprop():
    cases = []  # (probe_name, prelude_src, [(tname, expr), ...])
    for n in COUNTS:
        for v in VARIANTS:
            name = f"px_switch_n{n}_{v}.js"
            cases.append((name, switch_probe(n, v), switch_case_code(n, v)))
    return cases


def build_sliced():
    cases = []
    for L in LENGTHS:
        for w in WIDTHS:
            for op in OPS:
                name = f"sx_{w}_l{L}_{op.lower()}.js"
                cases.append((name, string_probe(L, w, op), string_case_code(L, w, op)))
    return cases


def bake(cases, tmpname):
    """One node run computes all expr values; returns {(probe, tname): literal_src}."""
    baker = ["var __OUT = [];"]
    for pi, (name, prelude, asserts) in enumerate(cases):
        baker.append(f"// probe {pi}")
        baker.append("{  // block scope: probes redeclare consts")
        baker.append(prelude)
        for ti, (tn, expr, _u) in enumerate(asserts):
            tmpl = ("try { __OUT.push([@NAME, @TN, String((function(){ return @EXPR; })())]); } "
                    "catch (e) { __OUT.push([@NAME, @TN, "
                    "'!threw:' + ((e && e.constructor && e.constructor.name) || 'unknown')]); }")
            line = (tmpl.replace("@NAME", json.dumps(name)).replace("@TN", json.dumps(tn))
                        .replace("@EXPR", expr))
            baker.append(line)
        baker.append("}")
    baker.append("console.log(JSON.stringify(__OUT));")
    path = os.path.join(ROOT, "tools_agent", "_capture_tmp", tmpname)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(baker))
    r = subprocess.run([NODE, path], capture_output=True, text=True, timeout=300, cwd=ROOT)
    os.unlink(path)
    if r.returncode != 0:
        dbg = path + ".fail.js"
        with open(path, "w") as f:
            f.write("\n".join(baker))
        os.rename(path, dbg)
        raise RuntimeError(f"baker failed (kept {dbg}): {r.stderr[-400:]}")
    line = [l for l in r.stdout.split("\n") if l.startswith("[")][-1]
    out = json.loads(line)
    baked = {}
    for name, tn, val in out:
        baked[(name, tn)] = val
    return baked


def lit(val):
    """baked string value -> JS literal for comparison via assert_eq(String(expr), ...)."""
    return json.dumps(val, ensure_ascii=True)


def emit(cases, suite, baked):
    outdir = os.path.join(ROOT, "tests", "agent", suite)
    count = 0
    for name, prelude, asserts in cases:
        lines = [f"// parametric probe generated by tools_agent/gen_param.py",
                 f"// expectations baked from the node oracle at generation time",
                 "__EXP = null;",
                 prelude]
        for tn, expr, _ in asserts:
            val = baked.get((name, tn))
            if val is None:
                continue
            if val.startswith("!threw:"):
                ctor = val.split(":", 1)[1]
                lines.append(f"test({json.dumps(tn)}, function () {{ assert_throws(function () {{ return {expr}; }}, {json.dumps(ctor)}); }});")
            else:
                lines.append(f"test({json.dumps(tn)}, function () {{ assert_eq(String((function(){{ return {expr}; }})()), {lit(val)}, {json.dumps(tn)}); }});")
        lines.append(f"summary({json.dumps(suite)});")
        with open(os.path.join(outdir, name), "w") as f:
            f.write("\n".join(lines) + "\n")
        count += 1
    return count


def main():
    cp = build_constprop()
    sl = build_sliced()
    print(f"constprop probes: {len(cp)}; sliced_strings probes: {len(sl)}")
    baked_cp = bake(cp, "bake_constprop.js")
    baked_sl = bake(sl, "bake_sliced.js")
    n1 = emit(cp, "constprop", baked_cp)
    n2 = emit(sl, "sliced_strings", baked_sl)
    print(f"emitted {n1} constprop + {n2} sliced_strings parametric probes")


if __name__ == "__main__":
    main()
