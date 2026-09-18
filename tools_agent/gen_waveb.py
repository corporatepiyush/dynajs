#!/usr/bin/env python3
"""gen_waveb.py — parametric probe expansion for the constprop wave B suite.

Emits the tests/agent/constprop_waveB/ matrix (assert-based, expectations
baked from the node oracle at generation time, same bake discipline as
gen_param.py). Folding is semantically transparent, so every probe must
agree between node, the patched engine and the unfolder engine: any
divergence is a fold bug, which is the point.

  wb1_*.js : B1 — dot reads of string/bool/null members fold; value-kind x
             context x legality-kill matrix (bracket reads stay dynamic but
             agree; canonical-numeric string values never fold).
  wb2_*js  : B2 — function-local frozen consts (first-statement subset):
             kernels, arrows, methods, recursion, shadow-of-top, and the
             decline battery (captures, non-first decl, generator/async,
             eval/with, block-scoped decl) which must stay CORRECT.
  wb3_*.js : B3 — folded reads under labels/loops/switch/try and adjacent
             folds: stream compaction must not move semantics or line
             attribution (pc2line cross-checked by pc2line_check.sh).
"""
import json
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NODE = "node"
OUTDIR = os.path.join(ROOT, "tests", "agent", "constprop_waveB")

STR = 'Object.freeze({ S: "hello", W: "\\u00e9\\u4e2d", C: "7", T: true, F: false, N: null, I: 42 })'

cases = []  # (name, prelude, [(tname, expr)])


def add(name, prelude, asserts):
    cases.append((name, prelude, asserts))


# ---------------------------------------------------------------- B1 kinds
def b1_value(kind):
    """(decl, reads) for one value kind"""
    decls = {
        "str":  f'const M = Object.freeze({{ S: "hello" }});',
        "wide": f'const M = Object.freeze({{ W: "\\u00e9\\u4e2d" }});',
        "canon": f'const M = Object.freeze({{ C: "7", D: "00" }});',
        "bool": f'const M = Object.freeze({{ T: true, F: false }});',
        "null": f'const M = Object.freeze({{ N: null }});',
        "mixed": f'const M = {STR};',
    }
    return decls[kind]


B1_VALUE_KINDS = ["str", "wide", "canon", "bool", "null", "mixed"]

B1_SHAPES = {
    "stmt":   "M.{k}",
    "arg":    "[1, M.{k}][1]",
    "cond":   "(M.{k} ? 1 : 0)",
    "tmpl":   '"t=" + M.{k}',
    "eq":     "(M.{k} === M.{k})",
    "chain":  None,  # per kind
    "bracket": "M[\"{k}\"]",
}


def chain_expr(kind):
    if kind in ("str", "wide"):
        return "M.S.length" if kind == "str" else "M.W.length"
    if kind == "bool":
        return "(M.T === true)"
    if kind == "null":
        return "(M.N === null)"
    return "M.I + 1"


def chain_expr(kind):
    if kind == "str":
        return "M.S.length"
    if kind == "wide":
        return "M.W.length"
    if kind == "canon":
        return "M.C.length"
    if kind == "bool":
        return "(M.T === true)"
    if kind == "null":
        return "(M.N === null)"
    return "M.I + 1"


SHAPE_KEY = {"str": "S", "wide": "W", "canon": "C", "bool": "T",
             "null": "N", "mixed": "I"}

for kind in B1_VALUE_KINDS:
    for shape in ("stmt", "arg", "cond", "tmpl", "eq", "chain", "bracket"):
        name = f"wb1_{kind}_{shape}.js"
        prelude = b1_value(kind)
        if shape == "chain":
            exprs = [chain_expr(kind)]
        elif shape == "bracket":
            # canonical-numeric string members: bracket read of "7" must
            # agree with 7 too (same property at runtime)
            if kind == "canon":
                exprs = ["M[\"C\"]", "M.C", "M[7]", "M[\"D\"]", "M.D"]
            else:
                exprs = [f'M[\"{SHAPE_KEY[kind]}\"]']
        else:
            if shape == "stmt" and kind == "mixed":
                exprs = ["M.S", "M.W", "M.C", "M.T", "M.F", "M.N", "M.I"]
            else:
                exprs = [B1_SHAPES[shape].format(k=SHAPE_KEY[kind])]
        asserts = [(f"{shape}{i}", e) for i, e in enumerate(exprs)]
        asserts.append(("typeof", "typeof M.S + typeof M.T + typeof M.N")
                       if kind == "mixed" else ("id", "1"))
        add(name, prelude, asserts)

# B1 switch: string case labels must NOT qualify for the int table but must
# dispatch correctly (probe-chain); int case labels in the same switch force
# the mixed-qualification path through cp_disable_switch if anything declines
sw_prelude = (
    'const M = Object.freeze({ S: "go", T: true, I: 42 });\n'
    "function ds(x) {\n"
    "    switch (x) {\n"
    '        case M.S: return "s";\n'
    "        case M.I: return 1;\n"
    '        default: return "d";\n'
    "    }\n"
    "}\n"
    "function ds2(x) {\n"
    "    switch (x) {\n"
    '        case M.S: return "s";\n'
    '        case "no": return "n";\n'
    "        default: return \"d2\";\n"
    "    }\n"
    "}\n"
)
add("wb1_switch_str_case.js", sw_prelude, [
    ("hit_str", "ds(M.S)"),
    ("hit_int", "ds(M.I)"),
    ("miss", "ds(0)"),
    ("str2", "ds2(M.S)"),
    ("miss2", "ds2(1)"),
])

# B1 kills: each kill changes fold legality for the WHOLE binding; values
# must agree with node regardless of whether the fold applied
B1_KILLS = {
    "write_atom":  "try { M.S = 1; } catch (e) {}",
    "delete_atom": "try { delete M.S; } catch (e) {}",
    "write_dyn":   "try { M[\"dyn\" + 1] = 1; } catch (e) {}",
    "alias_obj":   "const h = { m: M }; try { h.m.S = 9; } catch (e) {}",
    "alias_arr":   "const a = [M]; try { a[0].S = 9; } catch (e) {}",
    "alias_ret":   "function id() { return M; } try { id().S = 9; } catch (e) {}",
    "bare_ref":    "void M;",
    "assign_cpool": "const sink = []; sink.push(M);",
}
for kill, stmt in B1_KILLS.items():
    name = f"wb1_kill_{kill}.js"
    prelude = f'const M = Object.freeze({{ S: "hello", I: 42 }});\n{stmt}\n'
    add(name, prelude, [
        ("post", "M.S"),
        ("postI", "M.I"),
        ("still", "typeof M"),
    ])

# B1 shadow battery: a shadowing declaration kills the fold; values agree
B1_SHADOWS = {
    "param": ("function f(M) { return M.S; }", "f({S: \"shadow\"})"),
    "let":   ("function f() { let M = { S: \"shadow\" }; return M.S; }",
              "f()"),
    "var":   ("function h() { var M = { S: \"var-shadow\" }; return M.S; }",
              "h()"),
    "fnexpr": ("const g = function M() { return typeof M; };",
               "g() === \"function\" && M.S"),
    "class":  ("const K = class M { };", "M.S"),
    "inner_const": ("function f() { const M = Object.freeze({ S: \"in\" });"
                    " return M.S; }", "f() + M.S"),
}
for kill, (decl, expr) in B1_SHADOWS.items():
    name = f"wb1_shadow_{kill}.js"
    prelude = f'const M = Object.freeze({{ S: "hello" }});\n{decl}\n'
    add(name, prelude, [(kill, expr), ("top", "M.S")])

# B1 TDZ + eval/with + Object-shadow: binding-wide declines
add("wb1_tdz_early_fn.js", """
var tdz = "no";
function early() { return M.S; }
try { tdz = String(early()); } catch (e) { tdz = (e && e.constructor && e.constructor.name) || "unknown"; }
const M = Object.freeze({ S: "hello" });
""", [
    ("tdz", "tdz"),
    ("after", "M.S"),
])

for bad, decl in (("eval", "eval('1');"), ("with", "with ({}) { }")):
    name = f"wb1_kill_{bad}.js"
    prelude = f'const M = Object.freeze({{ S: "hello", I: 7 }});\n'
    add(name, prelude + decl + "\n", [("post", "M.S + M.I")])

add("wb1_kill_object_shadow.js",
    'var Object2 = Object;\nfunction fake() { return Object2; }\nconst M = Object.freeze({ S: "hi" });\n',
    [("post", "M.S"), ("objok", "typeof fake()")])

# ---------------------------------------------------------------- B2
B2_OK = {
    "kernel": (
        "function run(p) {\n"
        "    const OP = Object.freeze({ PUSH: 0, ADD: 1, SUB: 2, HALT: 3 });\n"
        "    let sp = -1, st = [], pc = 0;\n"
        "    while (pc < p.length) {\n"
        "        const op = p[pc++];\n"
        "        switch (op) {\n"
        "            case OP.PUSH: st[++sp] = p[pc++]; break;\n"
        "            case OP.ADD: { const b = st[sp--], a = st[sp--]; st[++sp] = a + b; break; }\n"
        "            case OP.SUB: { const b = st[sp--], a = st[sp--]; st[++sp] = a - b; break; }\n"
        "            case OP.HALT: return st[sp];\n"
        "            default: return NaN;\n"
        "        }\n"
        "    }\n"
        "    return NaN;\n"
        "}\n"
        "var prog = [0, 10, 0, 4, 1, 0, 2, 3];\n"),
    "values": (
        "function f() {\n"
        "    const M = Object.freeze({ S: \"ss\", T: true, N: null, I: -5 });\n"
        "    return M.S + \":\" + (M.T === true) + \":\" + (M.N === null) + \":\" + (M.I < 0);\n"
        "}\n"),
    "arrow": (
        "var f = () => {\n"
        "    const M = Object.freeze({ A: 2 });\n"
        "    return M.A * M.A;\n"
        "};\n"),
    "method": (
        "var o = { f() { const M = Object.freeze({ A: 3 }); return M.A; } };\n"),
    "ctor": (
        "function C() { const M = Object.freeze({ A: 4 }); this.v = M.A; }\n"),
    "iife": (
        "var r = (function () { const M = Object.freeze({ A: 5 }); return M.A; })();\n"),
    "recursion": (
        "function r(n) { const K = Object.freeze({ V: 3 });\n"
        "    if (n <= 0) return 0; return K.V + r(n - 1); }\n"),
    "multi": (
        "function f() { const A = Object.freeze({ X: 1 });\n"
        "    const B = Object.freeze({ Y: 2 });\n"
        "    { const C = Object.freeze({ Z: 3 }); return A.X + B.Y + C.Z; } }\n"),
    "shadow_top": (
        "const T = Object.freeze({ A: 1 });\n"
        "function f() { const T = Object.freeze({ A: 100 }); return T.A; }\n"),
    "two_funcs": (
        "function f() { const M = Object.freeze({ A: 1 }); return M.A; }\n"
        "function g() { const M = Object.freeze({ A: 2 }); return M.A; }\n"),
    "block_use": (
        "function f(c) { const M = Object.freeze({ A: 9 });\n"
        "    if (c) { return M.A; } return -M.A; }\n"),
    "loop_labels": (
        "function f() {\n"
        "    const M = Object.freeze({ A: 1, B: 2, S: \"x\" });\n"
        "    let acc = 0;\n"
        "    outer:\n"
        "    for (let i = 0; i < 3; i++) {\n"
        "        for (let j = 0; j < 3; j++) {\n"
        "            if (j === 1) { acc += M.A; continue outer; }\n"
        "            acc += M.B + M.S.length;\n"
        "        }\n"
        "    }\n"
        "    return acc;\n"
        "}\n"),
    "switch_fn_ints": (
        "function ds(x) {\n"
        "    const OP = Object.freeze({ K0: 1, K1: 7, K2: 13 });\n"
        "    switch (x) {\n"
        "        case OP.K0: return \"a\";\n"
        "        case OP.K1: return \"b\";\n"
        "        case OP.K2: return \"c\";\n"
        "        default: return \"d\";\n"
        "    }\n"
        "}\n"),
}
B2_OK_ASSERTS = {
    "kernel": [("run", "run(prog)"), ("again", "run(prog)")],
    "values": [("f", "f()"), ("f2", "f()")],
    "arrow": [("f", "f()")],
    "method": [("f", "o.f()")],
    "ctor": [("f", "new C().v")],
    "iife": [("r", "r")],
    "recursion": [("r4", "r(4)"), ("r0", "r(0)")],
    "multi": [("f", "f()")],
    "shadow_top": [("f", "f()"), ("top", "T.A")],
    "two_funcs": [("fg", "f() + \"|\" + g()")],
    "block_use": [("t", "f(true)"), ("u", "f(false)")],
    "loop_labels": [("f", "f()")],
    "switch_fn_ints": [("h0", "ds(1)"), ("h1", "ds(7)"),
                       ("h2", "ds(13)"), ("hm", "ds(5)")],
}
for k in B2_OK:
    add(f"wb2_ok_{k}.js", B2_OK[k], B2_OK_ASSERTS[k])

# B2 declines: folding must be declined but behavior must remain correct
B2_DECLINE = {
    "capture_read": (
        "function f() { const M = Object.freeze({ A: 7 }); return function () { return M.A; }; }\n"
        "var g = f();\n"),
    "capture_write": (
        "function f() { const M = Object.freeze({ A: 7 }); return function () { try { M.A = 1; } catch (e) {} return M.A; }; }\n"
        "var g = f();\n"),
    "not_first": (
        "function f() { var t = 1; const M = Object.freeze({ A: 8 }); return t + M.A; }\n"),
    "generator": (
        "function* g() { const M = Object.freeze({ A: 6 }); yield M.A; }\n"
        "var it = g();\n"),
    "async_fn": (
        "async function f() { const M = Object.freeze({ A: 6 }); return M.A; }\n"),
    "eval_fn": (
        "function f() { eval('1'); const M = Object.freeze({ A: 3 }); return M.A; }\n"),
    "with_fn": (
        "function f() { with ({}) { } const M = Object.freeze({ A: 4 }); return M.A; }\n"),
    "block_decl": (
        "function f() { { const M = Object.freeze({ A: 5 }); return M.A; } }\n"),
    "fn_str_kill": (
        "function f() { const M = Object.freeze({ S: \"hello\" });"
        " try { M.S = 2; } catch (e) {} return M.S; }\n"),
}
B2_DECLINE_ASSERTS = {
    "capture_read": [("g", "g()")],
    "capture_write": [("g", "g()")],
    "not_first": [("f", "f()")],
    "generator": [("v", "it.next().value")],
    "async_fn": [("t", "typeof f()")],
    "eval_fn": [("f", "f()")],
    "with_fn": [("f", "f()")],
    "block_decl": [("f", "f()")],
    "fn_str_kill": [("f", "f()")],
}
for k in B2_DECLINE:
    add(f"wb2_decline_{k}.js", B2_DECLINE[k], B2_DECLINE_ASSERTS[k])

# ---------------------------------------------------------------- B3
B3 = {
    "labels_mixed": (
        'const M = Object.freeze({ A: 1, B: 2, S: "x" });\n'
        "let acc = 0;\n"
        "outer:\n"
        "for (let i = 0; i < 2; i++) {\n"
        "    acc += M.A;\n"
        "    for (let j = 0; j < 3; j++) {\n"
        "        if (j === 1) { acc += M.S.length; continue outer; }\n"
        "        acc += M.B;\n"
        "    }\n"
        "}\n"),
    "try_finally": (
        'const M = Object.freeze({ A: 3, S: "yy" });\n'
        "function f() {\n"
        "    let r = M.A;\n"
        "    try { r += M.S.length; throw new Error('x'); }\n"
        "    catch (e) { r += M.A; } finally { r += M.A; }\n"
        "    return r;\n"
        "}\n"),
    "adjacent_args": (
        'const M = Object.freeze({ A: 1, B: 2, S: "z" });\n'
        "function sum() { let t = M.A; for (var i = 0; i < arguments.length; i++) t += arguments[i]; return t; }\n"),
    "multiline": (
        'const M = Object.freeze({\n    A: 11,\n    S: "line"\n});\n'
        "function f() {\n    let x = M.\nA;\n"
        "    let y = M[\n\"S\"];\n    return x + \":\" + y;\n}\n"),
    "switch_mixed_case": (
        'const M = Object.freeze({ I1: 5, I2: 9, S: "sx" });\n'
        "function f(x) {\n    switch (x) {\n"
        "        case M.I1: return \"i1\";\n"
        "        case M.S: return \"s\";\n"
        "        case M.I2: return \"i2\";\n"
        "        default: return \"d\";\n    }\n}\n"),
    "while_cond": (
        'const M = Object.freeze({ A: 2, L: 3 });\n'
        "function f() { let n = 0; while (n < M.L) { n += M.A; } return n; }\n"),
}
B3_ASSERTS = {
    "labels_mixed": [("acc", "acc")],
    "try_finally": [("f", "f()")],
    "adjacent_args": [("s", "sum(M.A, M.B, M.S)")],
    "multiline": [("f", "f()"), ("line", "1")],
    "switch_mixed_case": [("i1", "f(M.I1)"), ("s", "f(M.S)"),
                          ("i2", "f(M.I2)"), ("d", "f(0)")],
    "while_cond": [("f", "f()")],
}
for k in B3:
    add(f"wb3_{k}.js", B3[k], B3_ASSERTS[k])

# ---------------------------------------------------------------- bake+emit


def bake():
    baker = ["var __OUT = [];"]
    for pi, (name, prelude, asserts) in enumerate(cases):
        baker.append(f"// probe {pi}")
        baker.append("{  // block scope: probes redeclare consts")
        baker.append(prelude)
        for tn, expr in asserts:
            tmpl = ("try { __OUT.push([@NAME, @TN, String((function(){ return @EXPR; })())]); } "
                    "catch (e) { __OUT.push([@NAME, @TN, "
                    "'!threw:' + ((e && e.constructor && e.constructor.name) || 'unknown')]); }")
            line = (tmpl.replace("@NAME", json.dumps(name)).replace("@TN", json.dumps(tn))
                        .replace("@EXPR", expr))
            baker.append(line)
        baker.append("}")
    baker.append("console.log(JSON.stringify(__OUT));")
    path = os.path.join(ROOT, "tools_agent", "_capture_tmp", "bake_waveb.js")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(baker))
    try:
        r = subprocess.run([NODE, path], capture_output=True, text=True,
                           timeout=300, cwd=ROOT)
    finally:
        os.unlink(path)
    if r.returncode != 0:
        raise RuntimeError(f"baker failed: {r.stderr[-400:]}")
    line = [l for l in r.stdout.split("\n") if l.startswith("[")][-1]
    out = json.loads(line)
    baked = {}
    for name, tn, val in out:
        baked[(name, tn)] = val
    return baked


def lit(val):
    return json.dumps(val, ensure_ascii=True)


def main():
    baked = bake()
    os.makedirs(OUTDIR, exist_ok=True)
    expdir = os.path.join(OUTDIR, "expected")
    os.makedirs(expdir, exist_ok=True)
    count = 0
    for name, prelude, asserts in cases:
        lines = ["// parametric probe generated by tools_agent/gen_waveb.py",
                 "// expectations baked from the node oracle at generation time",
                 "__EXP = null;",
                 prelude]
        exp_lines = []
        for tn, expr in asserts:
            val = baked.get((name, tn))
            if val is None:
                raise RuntimeError(f"missing bake for {name}:{tn}")
            if val.startswith("!threw:"):
                ctor = val.split(":", 1)[1]
                lines.append(f"test({json.dumps(tn)}, function () {{ assert_throws(function () {{ return {expr}; }}, {json.dumps(ctor)}); }});")
            else:
                lines.append(f"test({json.dumps(tn)}, function () {{ assert_eq(String((function(){{ return {expr}; }})()), {lit(val)}, {json.dumps(tn)}); }});")
            exp_lines.append(f"{tn}={val}")
        lines.append('summary("constprop_waveB");')
        with open(os.path.join(OUTDIR, name), "w") as f:
            f.write("\n".join(lines) + "\n")
        # standalone fold-active variant: NO harness prelude, and the print
        # shim is a hoisted function DECLARATION placed AFTER the probe's
        # const decls (REVIEW-FIX F3: the previous `var __p = ...` shim
        # emitted code ahead of the decls, so every top-level binding lost
        # decl_first and all B1 folds silently declined). With the decls
        # first, top-level decl_first holds and B1/B2/B3 folds are active;
        # transcripts are unchanged and still compared by run_fold.sh.
        # Lives in fold/ so the harness runner (top-level *.js) skips it.
        fl = ["// fold-active standalone variant of " + name +
              " (gen_waveb.py); transcript checked by run_fold.sh",
              prelude.rstrip(),
              'function __p(s) { if (typeof print === "function") print(s); else console.log(s); }']
        for tn, expr in asserts:
            fl.append(f'try {{ __p("{tn}=" + String((function(){{ return {expr}; }})())); }} '
                      f'catch (e) {{ __p("{tn}=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }}')
        foldir = os.path.join(OUTDIR, "fold")
        os.makedirs(foldir, exist_ok=True)
        with open(os.path.join(foldir, "fold_" + name), "w") as f:
            f.write("\n".join(fl) + "\n")
        with open(os.path.join(expdir, "fold_" + name + ".txt"), "w") as f:
            f.write("\n".join(exp_lines) + "\n")
        count += 1
    print(f"emitted {count} constprop_waveB probes (harness) + {count} fold-active standalone variants")


if __name__ == "__main__":
    main()
