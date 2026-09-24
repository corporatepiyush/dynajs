#!/usr/bin/env python3
# gen_probes.py — parametric probe generator for the DynaJS constprop black-box review.
#
# Two-phase:
#   phase 1: emit capture-form probes, run them under node (spec oracle), record values/throws
#   phase 2: re-emit final probes with node-computed expectations BAKED into asserts
#
# Every assert in every final probe compares against the node-computed expectation.
# Probe metadata (family, mode, declined flag, matrix cells) goes to manifest.tsv.

import json, os, subprocess, sys, shutil
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
PROBES = os.path.join(HERE, "probes")
CAPD = os.path.join(HERE, "probes_capture")
HARNESS = open(os.path.join(HERE, "h.js")).read()
NODE = shutil.which("node")
assert NODE, "node not found"

# ---------------- matrix dimensions ----------------
MC = [1, 2, 3, 5, 8, 17, 31, 50, 255, 256, 257, 300, 1000]      # member counts (cpool classes)
CS = ["dense", "stride", "negonly", "i32edge", "mixedsign",
      "hitdefault", "gapheavy", "reversed", "dupvalues"]         # case-value shapes
NM_SAFE_TOP = ["OP", "op", "_", "$", "\\u{10400}bc", "argum", "argumes",
               "argss", "Object1", "globalThis", "constructor"]  # legal at script top level
NM_SAFE_FN = NM_SAFE_TOP + ["NaN", "undefined"]                  # additionally legal in fn/block/module
SC = ["top", "infn", "inblock", "fninblock", "deep10", "inswitch", "foradj"]
DP = ["first", "after1", "after5", "after50", "afterconst", "afterfn", "intry"]
UK = ["case", "read", "hotloop", "incall", "derived", "ternary"]
MODES = ["sloppy", "strict", "module"]

probes = []


def add_probe(family, mode, declined, cells, body, snippets, preamble=None):
    pid = "%s_%04d" % (family, len([p for p in probes if p["family"] == family]))
    probes.append({"pid": pid, "family": family, "mode": mode, "declined": 1 if declined else 0,
                   "cells": cells, "body": body, "snippets": snippets, "preamble": preamble})


def S(code, expr):
    return {"code": code, "expr": expr}


# ---------------- member/case builders ----------------

def case_vals(cs, n):
    if n <= 0:
        return []
    if cs == "dense":
        return [str(i) for i in range(n)]
    if cs == "stride":
        return [str(i * 7) for i in range(n)]
    if cs == "negonly":
        return [str(-(i + 1)) for i in range(n)]
    if cs == "i32edge":
        vals = []
        for i in range(n):
            if i % 2 == 0:
                vals.append(str(-2147483648 + i // 2))
            else:
                vals.append(str(2147483647 - i // 2))
        return vals
    if cs == "mixedsign":
        vals = []
        for i in range(n):
            vals.append(str(i // 2) if i % 2 == 0 else str(-(i // 2 + 1)))
        return vals
    if cs == "hitdefault":
        return [str(1000 + i) for i in range(n)]
    if cs == "gapheavy":
        return [str(i * 1000 + (i % 3) * 7) for i in range(n)]
    if cs == "reversed":
        return [str(n - 1 - i) for i in range(n)]
    if cs == "dupvalues":
        return [str(i % 3) for i in range(n)]
    raise ValueError(cs)


def decl_line(nm, keys, vals, frozen, junk=0):
    pairs = ["%s: %s" % (k, v) for k, v in zip(keys, vals)]
    if junk:
        pairs += ["J0: %s" % json.dumps("junkstr"), "J1: 1.5", "J2: undefined"]
    chunk = ", ".join(pairs)
    obj = "{ " + chunk + " }"
    return "const %s = %s;" % (nm, ("Object.freeze(%s)" % obj) if frozen else obj)


def dispatch_inputs(cs, vals, n):
    v = [int(x) for x in vals[:n]]
    inputs = [v[0], v[len(v) // 2], v[-1], v[0] + 1, 999999999]
    if cs == "i32edge":
        inputs = [-2147483648, 2147483647, v[0], v[-1], 999999999]
    if cs == "negonly":
        inputs = [v[0], v[-1], -1, 0, 999999999]
    return inputs


# ---------------- scope builders ----------------
# Each returns (body_lines, H) where H is the handle expression prefix, e.g. "__H.sw".

def scope_body(sc, nm, keys, vals, frozen, dp, junk=0):
    fill = []
    if dp == "after1":
        fill = ["var __f1 = 1;"]
    elif dp == "after5":
        fill = ["var __f%d = %d;" % (i, i) for i in range(1, 6)]
    elif dp == "after50":
        fill = ["var __f%d = %d;" % (i, i) for i in range(1, 51)]
    elif dp == "afterconst":
        fill = ["const __other = { q1: 11, q2: 22 };"]
    elif dp == "afterfn":
        fill = ["function __earlierfn() { return 2; }"]

    d = decl_line(nm, keys, vals, frozen, junk)

    def mk_helpers(ind):
        pad = "  " * ind
        case_lines = "\n".join('%scase %s.%s: r = "c%d"; break;' % (pad + "  ", nm, k, i)
                               for i, k in enumerate(keys))
        body_h = "%sfunction __sw(x) { var r = \"D\"; switch (x) {\n%s\n%sdefault: r = \"D\"; break;\n%s}\n%sreturn r; }\n" % (
            pad, case_lines, pad, pad, pad)
        body_h += "%sfunction __rd() { return %s.%s; }\n" % (pad, nm, keys[0])
        body_h += "%sfunction __rdl() { return %s.%s; }\n" % (pad, nm, keys[-1])
        body_h += "%sfunction __rd2() { return (function () { return %s.%s; })(); }\n" % (pad, nm, keys[0])
        body_h += "%sfunction __d1() { return %s.%s + 1; }\n" % (pad, nm, keys[0])
        body_h += "%sfunction __d2() { return %s.%s * 2 - 1; }\n" % (pad, nm, keys[0])
        body_h += "%sfunction __hl() { var s = 0; for (var i = 0; i < 500; i++) { s += %s.%s; } return s; }\n" % (pad, nm, keys[0])
        body_h += "%sfunction __t() { return %s.%s > 0 ? (%s.%s > 0 ? %s.%s : %s.%s) : -(%s.%s); }\n" % (
            pad, nm, keys[0], nm, keys[-1], nm, keys[0], nm, keys[-1], nm, keys[0])
        body_h += "%s__H = { sw: __sw, rd: __rd, rdl: __rdl, rd2: __rd2, d1: __d1, d2: __d2, hl: __hl, t: __t };\n" % pad
        return body_h

    def indent(ls, pad):
        return [pad + l for l in ls]

    # inner construction: fillers + decl + helpers (assignment to __H)
    inner = fill + [d] + mk_helpers(0).splitlines()

    lines = []
    if dp == "intry":
        # decl inside try; __H declared outside try (var hoisting keeps it visible)
        pre = ["var __H;"]
        tryp = ["try {"]
        tryp += indent(inner, "  ")
        tryp += ["} catch (e) { __H = null; }"]
        inner_block = pre + tryp
    else:
        inner_block = ["var __H;"] + inner

    if sc == "top":
        lines += inner_block
    elif sc == "infn":
        lines.append("function __mk() {")
        lines += indent(inner_block, "  ")
        lines.append("  return __H;")
        lines.append("}")
        lines.append("var __H = __mk();")
    elif sc == "inblock":
        lines.append("{")
        lines += indent(inner_block, "  ")
        lines.append("}")
    elif sc == "fninblock":
        lines.append("var __H;")
        lines.append("{")
        lines.append("  function __mk() {")
        lines += indent(inner_block, "    ")
        lines.append("    return __H;")
        lines.append("  }")
        lines.append("  __H = __mk();")
        lines.append("}")
    elif sc == "deep10":
        for i in range(10):
            lines.append("{")
        lines += indent(inner_block, "  " * 10)
        for i in range(10):
            lines.append("}")
    elif sc == "inswitch":
        lines.append("var __H;")
        lines.append("function __outer(k) {")
        lines.append("  switch (k) {")
        lines.append("    case 0: {")
        lines += indent(inner_block, "      ")
        lines.append("      break;")
        lines.append("    }")
        lines.append("    default: __H = null; break;")
        lines.append("  }")
        lines.append("}")
        lines.append("__outer(0);")
    elif sc == "foradj":
        lines.append("for (var __i = 0; __i < 1; __i++) {")
        lines += indent(inner_block, "  ")
        lines.append("}")
    else:
        raise ValueError(sc)
    return lines


# ---------------- big grids ----------------

def emit_biggrid(tag, mc_off, dp_rot):
    idx = 0
    for cs in CS:
        for sc in SC:
            for uk in UK:
                for mode in MODES:
                    i = idx
                    idx += 1
                    mc = MC[(i + mc_off) % len(MC)]
                    names = NM_SAFE_FN if sc in ("infn", "fninblock", "deep10", "inblock") or mode == "module" else NM_SAFE_TOP
                    nm = names[(i * 7 + mc_off) % len(names)]
                    dp = DP[(i + dp_rot) % len(DP)]
                    vals = case_vals(cs, mc)
                    keys = ["A%d" % j for j in range(mc)]
                    declined = False
                    body = scope_body(sc, nm, keys, vals, True, dp)
                    snips = []
                    for j, inp in enumerate(dispatch_inputs(cs, vals, mc)):
                        snips.append(S(["return __H.sw(%d);" % inp], "__H.sw(%d)" % inp))
                    if uk == "read":
                        snips.append(S(["return __H.rd();"], "__H.rd()"))
                        snips.append(S(["return __H.rdl();"], "__H.rdl()"))
                    elif uk == "hotloop":
                        snips.append(S(["return __H.hl();"], "__H.hl()"))
                    elif uk == "incall":
                        snips.append(S(["return __H.rd2();"], "__H.rd2()"))
                    elif uk == "derived":
                        snips.append(S(["return __H.d1();"], "__H.d1()"))
                        snips.append(S(["return __H.d2();"], "__H.d2()"))
                    elif uk == "ternary":
                        snips.append(S(["return __H.t();"], "__H.t()"))
                    cells = "cs=%s sc=%s uk=%s mc=%d nm=%s dp=%s frz=1" % (cs, sc, uk, mc, nm, dp)
                    add_probe(tag, mode, declined, cells, body, snips)


def emit_switch_core():
    for cs in CS:
        for mc in MC:
            for junk in (0, 3):
                for mode in MODES:
                    vals = case_vals(cs, mc)
                    keys = ["A%d" % j for j in range(mc)]
                    body = scope_body("top", "OP", keys, vals, True, "first", junk)
                    snips = [S(["return __H.sw(%d);" % inp], "__H.sw(%d)" % inp)
                             for inp in dispatch_inputs(cs, vals, mc)]
                    if junk:
                        snips.append(S(["return OP.J0;"], "OP.J0"))
                        snips.append(S(["return __H.sw(\"junkstr\");"], "__H.sw(\"junkstr\")"))
                    cells = "cs=%s mc=%d junk=%d frz=1 sc=top uk=case nm=OP" % (cs, mc, junk)
                    add_probe("switchcore", mode, 0, cells, body, snips)


def emit_names():
    for mode in MODES:
        names = NM_SAFE_FN if mode == "module" else NM_SAFE_TOP
        for nm in names:
            for uk in UK:
                vals = case_vals("dense", 5)
                keys = ["A%d" % j for j in range(5)]
                body = scope_body("top", nm, keys, vals, True, "first")
                snips = [S(["return __H.sw(%d);" % inp], "__H.sw(%d)" % inp)
                         for inp in dispatch_inputs("dense", vals, 5)]
                if uk == "read":
                    snips.append(S(["return __H.rd();"], "__H.rd()"))
                elif uk == "hotloop":
                    snips.append(S(["return __H.hl();"], "__H.hl()"))
                elif uk == "incall":
                    snips.append(S(["return __H.rd2();"], "__H.rd2()"))
                elif uk == "derived":
                    snips.append(S(["return __H.d1();"], "__H.d1()"))
                elif uk == "ternary":
                    snips.append(S(["return __H.t();"], "__H.t()"))
                # the binding itself must be the frozen object, not a global shadow
                snips.append(S(["return typeof %s;" % nm], "typeof %s" % nm))
                cells = "nm=%s uk=%s frz=1 sc=top" % (nm, uk)
                add_probe("names", mode, 0, cells, body, snips)


def emit_declpos():
    for dp in DP:
        for mode in MODES:
            for mc in (5, 17, 256):
                vals = case_vals("dense", mc)
                keys = ["A%d" % j for j in range(mc)]
                body = scope_body("top", "OP", keys, vals, True, dp)
                snips = [S(["return __H.sw(%d);" % inp], "__H.sw(%d)" % inp)
                         for inp in dispatch_inputs("dense", vals, mc)[:3]]
                cells = "dp=%s mc=%d" % (dp, mc)
                add_probe("declpos", mode, 0, cells, body, snips)


def emit_mutations():
    # mutation applied as a snippet (try-wrapped) so strict/module throws are observed, not fatal
    MUT_SNIPPET = {
        "none": None,
        "pre-use-write": S(["try { OP.A = 99; return \"set-ok\"; } catch (e) { return \"threw:\" + e.name; }"],
                           "\"unused\""),
        "post-use-write": S(["try { OP.A = 99; return \"set-ok\"; } catch (e) { return \"threw:\" + e.name; }"],
                            "\"unused\""),
        "eval-write": S(["try { eval(\"OP.A = 99\"); return \"eval-ok\"; } catch (e) { return \"threw:\" + e.name; }"],
                        "\"unused\""),
        "reflect-set": S(["try { return \"r=\" + Reflect.set(OP, \"A\", 99); } catch (e) { return \"threw:\" + e.name; }"],
                         "\"unused\""),
        "dp-writable-flip": S(["try { Object.defineProperty(OP, \"A\", { writable: true }); return \"dp-ok\"; } catch (e) { return \"threw:\" + e.name; }"],
                              "\"unused\""),
        "delete": S(["try { return \"del=\" + (delete OP.A); } catch (e) { return \"threw:\" + e.name; }"],
                    "\"unused\""),
        "seal-after-freeze": S(["try { Object.seal(OP); OP.A = 99; return \"set-ok\"; } catch (e) { return \"threw:\" + e.name; }"],
                               "\"unused\""),
        "freeze-twice": S(["try { Object.freeze(OP); return \"fz-ok\"; } catch (e) { return \"threw:\" + e.name; }"],
                          "\"unused\""),
    }
    for frozen in (1, 0):
        for mut in ["none", "pre-use-write", "post-use-write", "eval-write", "reflect-set",
                    "dp-writable-flip", "delete", "seal-after-freeze", "freeze-twice"]:
            for mode in MODES:
                body = ["var __H;"]
                body.append(decl_line("OP", ["A", "B", "C"], ["1", "2", "3"], frozen))
                body.append("__H = {")
                body.append("  sw: function (x) { var r = \"D\"; switch (x) { case OP.A: r = \"cA\"; break; case OP.B: r = \"cB\"; break; case OP.C: r = \"cC\"; break; default: r = \"D\"; break; } return r; },")
                body.append("  rd: function () { return OP.A; }")
                body.append("};")
                snips = []
                if mut == "post-use-write":
                    snips.append(S(["return OP.A;"], "OP.A"))  # read BEFORE mutation
                if MUT_SNIPPET[mut] is not None:
                    snips.append(MUT_SNIPPET[mut])
                snips += [S(["return __H.sw(%d);" % i], "__H.sw(%d)" % i) for i in (1, 2, 3, 4)]
                snips.append(S(["return __H.rd();"], "__H.rd()"))
                declined = 0 if (frozen and mut in ("none", "freeze-twice")) else 1
                cells = "frz=%d mut=%s" % (frozen, mut)
                add_probe("mutation", mode, declined, cells, body, snips)


def emit_objattack():
    attacks = {}

    def a_var_later(mode_ok=True):
        body = [
            "var __H; var __log = [];",
            "const OP = Object.freeze({ A: 1, B: 2 });",
            "__H = { rd: function () { return OP.A; }, sw: function (x) { var r = \"D\"; switch (x) { case OP.A: r = \"cA\"; break; default: r = \"D\"; break; } return r; } };",
            "var r1;",
            "try { eval(\"var Object = 1;\"); const Q = Object.freeze({ B: 2 }); r1 = \"init-ok\"; } catch (e) { r1 = \"threw:\" + e.name; }",
            "var r2;",
            "try { r2 = \"read:\" + Q.B; } catch (e) { r2 = \"threw:\" + e.name; }",
        ]
        snips = [S(["return __H.rd();"], "__H.rd()"),
                 S(["return __H.sw(1);"], "__H.sw(1)"),
                 S(["return r1;"], "r1"),
                 S(["return r2;"], "r2")]
        return body, snips, 1

    def a_freeze_assign_pre():
        body = [
            "var __H;",
            "var __sv = Object.freeze;",
            "Object.freeze = function (o) { return { A: 42 }; };",
            "const OP = Object.freeze({ A: 1 });",
            "Object.freeze = __sv;",
            "__H = { rd: function () { return OP.A; }, sw: function (x) { var r = \"D\"; switch (x) { case OP.A: r = \"cA\"; break; case 42: r = \"c42\"; break; default: r = \"D\"; break; } return r; } };",
        ]
        snips = [S(["return __H.rd();"], "__H.rd()"),
                 S(["return __H.sw(42);"], "__H.sw(42)"),
                 S(["return __H.sw(1);"], "__H.sw(1)")]
        return body, snips, 1

    def a_freeze_bracket_pre():
        body = [
            "var __H;",
            "var __sv = Object[\"freeze\"];",
            "Object[\"freeze\"] = function (o) { return { A: 43 }; };",
            "const OP = Object.freeze({ A: 1 });",
            "Object[\"freeze\"] = __sv;",
            "__H = { rd: function () { return OP.A; }, sw: function (x) { var r = \"D\"; switch (x) { case OP.A: r = \"cA\"; break; case 43: r = \"c43\"; break; default: r = \"D\"; break; } return r; } };",
        ]
        snips = [S(["return __H.rd();"], "__H.rd()"),
                 S(["return __H.sw(43);"], "__H.sw(43)")]
        return body, snips, 1

    def a_delete_freeze():
        body = [
            "var __H;",
            "const OP = Object.freeze({ A: 1 });",
            "var __del = delete Object.freeze;",
            "__H = { rd: function () { return OP.A; }, got: function () { return typeof Object.freeze; } };",
            "var r1; try { const Q = Object.freeze({ B: 2 }); r1 = \"init-ok\"; } catch (e) { r1 = \"threw:\" + e.name; }",
            "var r2; try { r2 = \"read:\" + Q.B; } catch (e) { r2 = \"threw:\" + e.name; }",
        ]
        snips = [S(["return __H.rd();"], "__H.rd()"),
                 S(["return __H.got() + \":\" + __del;"], "__H.got() + \":\" + __del"),
                 S(["return r1;"], "r1"),
                 S(["return r2;"], "r2")]
        return body, snips, 1

    def a_gT_clobber():
        body = [
            "var __H; ",
            "const OP = Object.freeze({ A: 1 });",
            "__H = { rd: function () { return OP.A; } };",
            "var r1; try { (0,eval)(\"globalThis.Object = 1\"); const Q = Object.freeze({ B: 2 }); r1 = \"init-ok\"; } catch (e) { r1 = \"threw:\" + e.name; }",
            "var r2; try { r2 = \"read:\" + Q.B; } catch (e) { r2 = \"threw:\" + e.name; }",
        ]
        snips = [S(["return __H.rd();"], "__H.rd()"),
                 S(["return r1;"], "r1"),
                 S(["return r2;"], "r2")]
        return body, snips, 1

    def a_ieval_shadow():
        body = [
            "var __H;",
            "(0,eval)(\"var ObjectZed = 1;\");",  # harmless global var, sanity of ieval
            "const OP = Object.freeze({ A: 1 });",
            "__H = { rd: function () { return OP.A; } };",
            "var r1; try { (0,eval)(\"var Object = 1;\"); const Q = Object.freeze({ B: 2 }); r1 = \"init-ok\"; } catch (e) { r1 = \"threw:\" + e.name; }",
            "var r2; try { r2 = \"read:\" + Q.B; } catch (e) { r2 = \"threw:\" + e.name; }",
        ]
        snips = [S(["return __H.rd();"], "__H.rd()"),
                 S(["return r1;"], "r1"),
                 S(["return r2;"], "r2")]
        return body, snips, 1

    def a_let_inner():
        body = [
            "var __H;",
            "const OP = Object.freeze({ A: 1 });",
            "var __innerType;",
            "{ let OP = { A: 7 }; __innerType = \"shadow:\" + OP.A; }",
            "__H = { rd: function () { return OP.A; } };",
        ]
        snips = [S(["return __H.rd();"], "__H.rd()"),
                 S(["return __innerType;"], "__innerType")]
        return body, snips, 0

    attacks = {
        "var-obj-later": a_var_later,
        "freeze-assign-pre": a_freeze_assign_pre,
        "freeze-bracket-pre": a_freeze_bracket_pre,
        "delete-obj-freeze": a_delete_freeze,
        "gT-object-clobber": a_gT_clobber,
        "ieval-obj-shadow": a_ieval_shadow,
        "let-obj-inner": a_let_inner,
    }
    for aname, fn in attacks.items():
        for mode in MODES:
            body, snips, declined = fn()
            cells = "attack=%s" % aname
            add_probe("objattack", mode, declined, cells, body, snips)


def emit_timing():
    def t_samestmt(frozen):
        body = [
            "var __H;",
            "const OP = %s;" % decl_obj(["A", "B"], ["1", "2"], frozen),
            "var __same = OP.A + (function () { return OP.A; })();",
            "__H = { rd: function () { return OP.A; } };",
        ]
        snips = [S(["return __same;"], "__same"), S(["return __H.rd();"], "__H.rd()")]
        return body, snips, 0 if frozen else 1

    def t_closure_escape(frozen):
        body = [
            "var __H;",
            "const OP = %s;" % decl_obj(["A"], ["1"], frozen),
            "var __f = function () { return OP.A; };",
            "var __before = __f();",
            "try { (0,eval)(\"globalThis.Object = 1\"); } catch (e) {}",
            "var __after = __f();",
            "__H = {};",
        ]
        snips = [S(["return __before;"], "__before"), S(["return __after;"], "__after")]
        return body, snips, 1

    def t_second_script(frozen):
        body = [
            "var __H;",
            "const OP = %s;" % decl_obj(["A"], ["1"], frozen),
            "globalThis.__OP = OP;",
            "(0,eval)(\"globalThis.__sink = globalThis.__OP.A;\");",
            "var __sink = globalThis.__sink;",
        ]
        if not frozen:
            body.append("(0,eval)(\"globalThis.__OP.A = 5;\");")
        body.append("var __aftermut = globalThis.__OP.A;")
        body.append("__H = {};")
        snips = [S(["return __sink;"], "__sink"), S(["return __aftermut;"], "__aftermut")]
        return body, snips, 1

    def t_newfn(frozen):
        body = [
            "var __H;",
            "const OP = %s;" % decl_obj(["A"], ["1"], frozen),
            "globalThis.__OPg = OP;",
            "var __v = new Function(\"return globalThis.__OPg.A;\")();",
            "__H = {};",
        ]
        snips = [S(["return __v;"], "__v")]
        return body, snips, 1

    def decl_obj(keys, vals, frozen):
        pairs = ", ".join("%s: %s" % (k, v) for k, v in zip(keys, vals))
        return ("Object.freeze({ %s })" % pairs) if frozen else ("{ %s }" % pairs)

    for tname, tf in [("samestmt", t_samestmt), ("closure-escape", t_closure_escape),
                      ("second-script", t_second_script), ("new-fn", t_newfn)]:
        for frozen in (1, 0):
            for mode in MODES:
                body, snips, declined = tf(frozen)
                cells = "t=%s frz=%d" % (tname, frozen)
                add_probe("timing", mode, declined, cells, body, snips)


def emit_nonint():
    kinds = {
        "string": ('{ A: "x" }', [
            S(["return __H.sw2(\"x\");"], "__H.sw2(\"x\")"),
            S(["return __H.sw2(1);"], "__H.sw2(1)"),
            S(["return OP.A;"], "OP.A")]),
        "double": ('{ A: 1.5 }', [
            S(["return __H.sw2(1.5);"], "__H.sw2(1.5)"),
            S(["return __H.sw2(1);"], "__H.sw2(1)"),
            S(["return OP.A;"], "OP.A")]),
        "nan": ('{ A: NaN }', [
            S(["return __H.sw2(0);"], "__H.sw2(0)"),
            S(["return __fmt(OP.A);"], "__fmt(OP.A)")]),
        "inf": ('{ A: Infinity }', [
            S(["return __H.sw2(Infinity);"], "__H.sw2(Infinity)"),
            S(["return __H.sw2(0);"], "__H.sw2(0)")]),
        "negzero": ('{ A: -0 }', [
            S(["return 1 / OP.A;"], "1 / OP.A"),
            S(["return __fmt(OP.A);"], "__fmt(OP.A)"),
            S(["return __H.sw2(0);"], "__H.sw2(0)")]),
        "p31": ('{ A: 2147483648 }', [
            S(["return __H.sw2(-2147483648);"], "__H.sw2(-2147483648)"),
            S(["return OP.A === 2147483648;"], "OP.A === 2147483648")]),
        "p32": ('{ A: 4294967296 }', [
            S(["return __H.sw2(0);"], "__H.sw2(0)"),
            S(["return OP.A === 4294967296;"], "OP.A === 4294967296")]),
        "p53m1": ('{ A: 9007199254740991 }', [
            S(["return __H.sw2(1);"], "__H.sw2(1)"),
            S(["return OP.A === 9007199254740991;"], "OP.A === 9007199254740991")]),
        "np31": ('{ A: -2147483649 }', [
            S(["return __H.sw2(2147483647);"], "__H.sw2(2147483647)"),
            S(["return OP.A === -2147483649;"], "OP.A === -2147483649")]),
        "bigint": ('{ A: 1n }', [
            S(["return __H.sw2(1n);"], "__H.sw2(1n)"),
            S(["return __H.sw2(1);"], "__H.sw2(1)"),
            S(["return __fmt(OP.A);"], "__fmt(OP.A)")]),
        "undefmember": ('{ A: undefined }', [
            S(["return __H.sw2(undefined);"], "__H.sw2(undefined)"),
            S(["return __H.sw2(0);"], "__H.sw2(0)")]),
        "getter": (None, [
            S(["return OP.A;"], "OP.A"),
            S(["return OP.A;"], "OP.A"),
            S(["return __ctr;"], "__ctr")]),
        "dupekey": ('{ A: 1, A: 9 }', [
            S(["return OP.A;"], "OP.A"),
            S(["return __H.sw2(9);"], "__H.sw2(9)")]),
        "protokey": ('{ __proto__: null, A: 3 }', [
            S(["return OP.A;"], "OP.A"),
            S(["return Object.getPrototypeOf(OP);"], "Object.getPrototypeOf(OP)")]),
    }
    for kind, (objlit, snips) in kinds.items():
        for mode in MODES:
            body = ["var __H; var __ctr = 0;"]
            if kind == "getter":
                body.append("var __gv = 0;")
                body.append("const OP = Object.freeze({ get A() { __ctr++; return __ctr; } });")
            else:
                body.append("const OP = Object.freeze(%s);" % objlit)
            body.append("__H = { sw2: function (x) { var r = \"D\"; switch (x) { case OP.A: r = \"hit\"; break; default: r = \"D\"; break; } return r; } };")
            declined = 1
            cells = "kind=%s" % kind
            add_probe("nonint", mode, declined, cells, body, list(snips))


def emit_tdz():
    variants = {}

    def v_pre():
        body = ["var __r;",
                "try { var __v = OP.A; __r = \"no-throw:\" + __v; } catch (e) { __r = \"threw:\" + e.name; }",
                "const OP = Object.freeze({ A: 1 });"]
        return body, [S(["return __r;"], "__r")], 1

    def v_in_call_before():
        body = ["var __r;",
                "function __probe() { try { return \"no-throw:\" + OP.A; } catch (e) { return \"threw:\" + e.name; } }",
                "__r = __probe();",
                "const OP = Object.freeze({ A: 1 });"]
        return body, [S(["return __r;"], "__r")], 1

    def v_failed_init():
        body = ["var __r1, __r2;",
                "try { eval(\"var Object = 1;\"); const Q = Object.freeze({ B: 2 }); __r1 = \"init-ok\"; } catch (e) { __r1 = \"threw:\" + e.name; }",
                "try { __r2 = \"read:\" + Q.B; } catch (e) { __r2 = \"threw:\" + e.name; }"]
        return body, [S(["return __r1;"], "__r1"), S(["return __r2;"], "__r2")], 1

    def v_case_let():
        body = ["var __H;",
                "const OP = Object.freeze({ A: 1, B: 2 });",
                "var __r;",
                "try { __r = (function (x) { switch (x) { case OP.A: __y = 1; return \"cA:\" + __y; case OP.B: let __y = 2; return \"cB:\" + __y; } return \"D\"; })(1); } catch (e) { __r = \"threw:\" + e.name; }"]
        return body, [S(["return __r;"], "__r")], 0

    def v_after_in_try():
        body = ["var __r;",
                "try { __r = \"read:\" + OP.A; } catch (e) { __r = \"threw:\" + e.name; }",
                "try { const OP = Object.freeze({ A: 1 }); __r += \"|init-ok\"; } catch (e) { __r += \"|init:\" + e.name; }"]
        return body, [S(["return __r;"], "__r")], 1

    def v_fn_later():
        body = ["var __r;",
                "function __late() { return OP.A; }",
                "try { __r = \"call:\" + __late(); } catch (e) { __r = \"threw:\" + e.name; }",
                "const OP = Object.freeze({ A: 1 });",
                "try { __r += \"|after:\" + __late(); } catch (e) { __r += \"|after:\" + e.name; }"]
        return body, [S(["return __r;"], "__r")], 0

    def v_eval_end():
        body = ["var __r;",
                "try { __r = \"read:\" + OP.A; } catch (e) { __r = \"threw:\" + e.name; }",
                "const OP = Object.freeze({ A: 1 });",
                "eval(\"/* eval at end */\");"]
        return body, [S(["return __r;"], "__r")], 1

    def v_loop():
        body = ["var __r = \"\";",
                "for (var i = 0; i < 2; i++) {",
                "  try { __r += \"read:\" + OP.A + \";\"; } catch (e) { __r += \"threw:\" + e.name + \";\"; }",
                "  if (i === 0) { const OP = Object.freeze({ A: 1 }); __r += \"decl:\" + OP.A + \";\"; }",
                "}"]
        return body, [S(["return __r;"], "__r")], 1

    variants = {"pre": v_pre, "in-call-before": v_in_call_before, "failed-init": v_failed_init,
                "case-let": v_case_let, "after-in-try": v_after_in_try, "fn-later": v_fn_later,
                "eval-end": v_eval_end, "loop": v_loop}
    for vname, vf in variants.items():
        for mode in MODES:
            body, snips, declined = vf()
            cells = "tdz=%s" % vname
            add_probe("tdz", mode, declined, cells, body, snips)


def emit_evalwith():
    # eval/with anywhere must kill the fold; observable via plain-object eval-write
    for epos in ["first", "mid", "last"]:
        for mode in ["sloppy", "strict", "module"]:
            body = ["var __H;"]
            if epos == "first":
                body.append("eval(\"/* early */\");")
            body.append("const OP = { A: 7 };")
            if epos == "mid":
                body.append("eval(\"/* mid */\");")
            body.append("__H = { rd: function () { return OP.A; } };")
            if epos == "last":
                body.append("eval(\"/* late */\");")
            sn = [S(["return __H.rd();"], "__H.rd()"),
                  S(["eval(\"OP.A = 9\");", "return OP.A;"], "OP.A")]
            cells = "epos=%s" % epos
            add_probe("evalkill", mode, 1, cells, body, sn)
    # with-kill (sloppy script only is legal; strict/module must SyntaxError)
    body = ["var __r = \"\";",
            "with ({ Object: 1 }) { try { const WQ = Object.freeze({ B: 2 }); __r = \"ok:\" + WQ.B; } catch (e) { __r = \"threw:\" + e.name; } }"]
    sn = [S(["return __r;"], "__r")]
    add_probe("evalkill", "sloppy", 1, "with-kill", body, sn)
    body2 = ["var __r = \"\";",
             "try { eval('with ({}) { 1; }'); __r = \"with-ok\"; } catch (e) { __r = \"threw:\" + e.name; }"]
    add_probe("evalkill", "strict", 1, "with-in-strict", body2, [S(["return __r;"], "__r")])
    add_probe("evalkill", "module", 1, "with-in-module", body2, [S(["return __r;"], "__r")])


def emit_syntax():
    illegal = [
        ("const-NaN-top", "const NaN = Object.freeze({ A: 1 });"),
        ("const-undefined-top", "const undefined = Object.freeze({ A: 1 });"),
        ("astral-nd-start", "const \\u1040bc = Object.freeze({ A: 1 });"),
        ("dup-const", "const OP = Object.freeze({ A: 1 }); const OP = Object.freeze({ A: 2 });"),
        ("case-let-dup", "switch (0) { case 0: let x; break; case 1: let x; break; }"),
        ("const-in-if-no-block", "if (1) const OP = Object.freeze({ A: 1 });"),
    ]
    for name, code in illegal:
        for mode in ["sloppy", "strict", "module"]:
            esc = code.replace("\\", "\\\\").replace("'", "\\'").replace("\n", "\\n")
            body = ["var __r1 = \"x\", __r2 = \"x\";",
                    "try { (0,eval)('%s'); __r1 = \"no-throw\"; } catch (e) { __r1 = e.name; }" % esc,
                    "try { new Function('%s'); __r2 = \"no-throw\"; } catch (e) { __r2 = e.name; }" % esc]
            sn = [S(["return __r1;"], "__r1"), S(["return __r2;"], "__r2")]
            add_probe("syntax", mode, 1, "illegal=%s" % name, body, sn)


def emit_misc():
    for mode in MODES:
        body = [
            "var __H;",
            "const OP = Object.freeze({ A: 1, B: 2 });",
            "const CP = Object.assign({}, OP);",
            "const PR = Object.create(OP);",
            "__H = {",
            "  sw: function (x) { var r = \"D\"; switch (x) { case OP.A: r = \"cA\"; break; case OP.B: r = \"cB\"; break; default: r = \"D\"; break; } return r; },",
            "};",
        ]
        sn = [
            S(["return CP.A;"], "CP.A"),
            S(["return PR.A;"], "PR.A"),
            S(["return Object.keys(OP).join(\",\");"], "Object.keys(OP).join(\",\")"),
            S(["return Object.values(OP).join(\",\");"], "Object.values(OP).join(\",\")"),
            S(["return OP.hasOwnProperty(\"A\");"], 'OP.hasOwnProperty("A")'),
            S(["return \"B\" in OP;"], '"B" in OP'),
            S(["return OP.A + OP.B;"], "OP.A + OP.B"),
            S(["return typeof OP.A;"], "typeof OP.A"),
            S(["return JSON.stringify(OP);"], "JSON.stringify(OP)"),
            S(["return Object.getOwnPropertyDescriptor(OP, \"A\").writable;"], 'Object.getOwnPropertyDescriptor(OP, "A").writable'),
            S(["return Reflect.get(OP, \"A\");"], 'Reflect.get(OP, "A")'),
            S(["return __H.sw(2);"], "__H.sw(2)"),
        ]
        add_probe("misc", mode, 0, "ops-on-frozen", body, sn)


def emit_byteid():
    # plain non-frozen decls across shapes: patched must be byte-identical to pristine
    shapes = [
        ("simple", ["const OP = { A: 1, B: 2 };"], "OP.A"),
        ("nested", ["const OP = { A: 1, B: { C: 2 } };"], "OP.B.C"),
        ("arr", ["const OP = { A: [1, 2, 3] };"], "OP.A[1]"),
        ("fnmember", ["const OP = { A: function () { return 5; } };"], "OP.A()"),
        ("manymembers", ["const OP = { %s };" % ", ".join("k%d: %d" % (i, i) for i in range(50))], "OP.k49"),
    ]
    muts = ["none", "write", "delete", "freeze-after"]
    for sname, decl, expr in shapes:
        for mut in muts:
            for mode in MODES:
                body = ["var __H;"] + decl
                if mut == "write":
                    body.append("OP.A = 99;")
                elif mut == "delete":
                    body.append("delete OP.B;")
                elif mut == "freeze-after":
                    body.append("Object.freeze(OP);")
                body.append("__H = { rd: function () { return %s; } };" % expr)
                sn = [S(["return __H.rd();"], "__H.rd()")]
                add_probe("byteid", mode, 1, "shape=%s mut=%s" % (sname, mut), body, sn)


def emit_modules_focus():
    for cs in CS:
        for mc in (2, 17, 256):
            for uk in ("case", "read"):
                vals = case_vals(cs, mc)
                keys = ["A%d" % j for j in range(mc)]
                body = scope_body("top", "OP", keys, vals, True, "first")
                snips = [S(["return __H.sw(%d);" % inp], "__H.sw(%d)" % inp)
                         for inp in dispatch_inputs(cs, vals, mc)[:4]]
                if uk == "read":
                    snips.append(S(["return __H.rd();"], "__H.rd()"))
                add_probe("modfocus", "module", 0, "cs=%s mc=%d uk=%s" % (cs, mc, uk), body, snips)


def emit_shadow():
    # lazily-bound names (params, self-names, arguments) must kill the fold
    for mode in MODES:
        # arg shadow: inner OP (param) is a plain object; fold using outer frozen => wrong case hit
        body = [
            "var __H;",
            "const OP = Object.freeze({ A: 3 });",
            "function __f(OP) { var r = \"D\"; switch (3) { case OP.A: r = \"hit3\"; break; case 1: r = \"hit1\"; break; } return r; }",
            "__H = { f: __f, outer: function () { return OP.A; } };",
        ]
        sn = [S(["return __H.f({ A: 3 });"], "__H.f({ A: 3 })"),
              S(["return __H.f(OP);"], "__H.f(OP)"),
              S(["return __H.outer();"], "__H.outer()")]
        add_probe("shadow", mode, 1, "kind=arg-shadow", body, sn)
        # function self-name: inner OP is the function object, not the frozen const
        body = [
            "var __H;",
            "const OP = Object.freeze({ A: 7 });",
            "var __g = function OP(v) { if (v === \"self\") { return typeof OP.A; } return OP === __g ? \"selfobj\" : \"outer\"; };",
            "__H = { g: __g };",
        ]
        sn = [S(["return __g(\"self\");"], "__g(\"self\")"),
              S(["return __g(\"cmp\");"], "__g(\"cmp\")")]
        add_probe("shadow", mode, 1, "kind=fn-self-name", body, sn)
        # class self-name (block-wrapped to avoid const collision)
        body = [
            "var __H; var __r = \"x\";",
            "const OP = Object.freeze({ A: 7 });",
            "try { { class OP { m() { return typeof OP.A; } } __r = new OP().m(); } } catch (e) { __r = \"threw:\" + e.name; }",
            "__H = { outer: function () { return OP.A; } };",
        ]
        sn = [S(["return __r;"], "__r"), S(["return __H.outer();"], "__H.outer()")]
        add_probe("shadow", mode, 1, "kind=class-self-name", body, sn)
        # arguments binding: sloppy fn legal, strict contexts SyntaxError (oracle-baked either way)
        if mode == "sloppy":
            body = [
                "var __H; var __r1 = \"x\";",
                "function __a() { try { const arguments = Object.freeze({ A: 5 }); return \"args:\" + arguments.A; } catch (e) { return \"threw:\" + e.name; } }",
                "__r1 = __a();",
                "__H = {};",
            ]
        else:
            body = [
                "var __H; var __r1 = \"x\";",
                "try { eval(\"function __a() { const arguments = Object.freeze({ A: 5 }); return \\\"args:\\\" + arguments.A; } __r1 = __a();\"); } catch (e) { __r1 = \"threw:\" + e.name; }",
                "__H = {};",
            ]
        body += ["try { new Function(\"\\\"use strict\\\"; const arguments = Object.freeze({ A: 5 }); return arguments.A;\")(); __H.strict = \"legal\"; } catch (e) { __H.strict = \"threw:\" + e.name; }"]
        sn = [S(["return __r1;"], "__r1"), S(["return __H.strict;"], "__H.strict")]
        add_probe("shadow", mode, 1, "kind=arguments-name", body, sn)


def emit_int32trap():
    # member values whose ToInt32 differs from ===-identity; a fold that loses precision shows here
    kinds = {
        "2p32plus1": ("{ A: 4294967297 }", ["1", "4294967297"]),   # ToInt32==1; switch(1) must NOT hit
        "neg2p32plus1": ("{ A: -4294967295 }", ["1", "-4294967295"]),
        "1e21": ("{ A: 1e21 }", ["0", "1e21"]),
        "2p31half": ("{ A: 2147483648.5 }", ["-2147483648", "2147483648.5"]),
        "2p53": ("{ A: 9007199254740992 }", ["0", "9007199254740992"]),
        "n2p53": ("{ A: -9007199254740992 }", ["0", "-9007199254740992"]),
        "bool-true": ("{ A: true }", ["1", "true"]),
        "bool-false": ("{ A: false }", ["0", "false"]),
        "nullmember": ("{ A: null }", ["0", "null"]),
        "strnum": ("{ A: \"1\" }", ["1", "\"1\""]),
        "minuseps": ("{ A: -0.0000001 }", ["0", "-0.0000001"]),
    }
    for kind, (objlit, inputs) in kinds.items():
        for mode in MODES:
            body = ["var __H;",
                    "const OP = Object.freeze(%s);" % objlit,
                    "__H = { sw: function (x) { var r = \"D\"; switch (x) { case OP.A: r = \"hit\"; break; default: r = \"D\"; break; } return r; } };"]
            sn = [S(["return __H.sw(%s);" % i], "__H.sw(%s)" % i) for i in inputs]
            sn.append(S(["return __fmt(OP.A);"], "__fmt(OP.A)"))
            add_probe("int32trap", mode, 1, "kind=%s" % kind, body, sn)


def emit_switchshape():
    shapes = {}
    shapes["default-first"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { default: r += \"d\"; break; case OP.A: r += \"a\"; break; case OP.B: r += \"b\"; break; } return r; })",
    ], ["1", "2", "3", "9"])
    shapes["default-middle-fallthrough"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { case OP.A: r += \"a\"; break; default: r += \"d\"; case OP.B: r += \"b\"; break; case OP.C: r += \"c\"; break; } return r; })",
    ], ["1", "2", "3", "9"])
    shapes["fallthrough-pair"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { case OP.A: case OP.B: r += \"ab\"; break; case OP.C: r += \"c\"; break; } return r; })",
    ], ["1", "2", "3", "4"])
    shapes["no-default"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { case OP.A: r += \"a\"; break; case OP.C: r += \"c\"; break; } return r; })",
    ], ["1", "2", "3", "7"])
    shapes["only-default"] = ([
        "__H.r = (function (x) { switch (x) { default: return \"onlyd\"; } return \"no\"; })",
    ], ["1", "9"])
    shapes["discriminant-member"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { case OP.A: r += \"a\"; break; case OP.B: r += \"b\"; break; default: r += \"d\"; break; } return r; })",
    ], ["OP.A", "OP.B", "OP.C"])
    shapes["two-case-holes"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { case OP.A: r += \"a\"; break; case OP.C: r += \"c\"; break; } return r; })",
    ], ["0", "1", "2", "3", "4", "-1"])
    shapes["outlier-i32max"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { case OP.A: r += \"a\"; break; case OP.B: r += \"b\"; break; case 2147483647: r += \"M\"; break; } return r; })",
    ], ["1", "2", "2147483647", "2147483646", "-2147483648"])
    shapes["continue-in-loop"] = ([
        "__H.r = (function () { var r = \"\"; for (var i = 0; i < 4; i++) { switch (i) { case OP.A: r += \"a\"; continue; case OP.B: r += \"b\"; break; default: r += \"d\"; } r += \"|\"; } return r; })",
    ], [])
    shapes["labeled-break"] = ([
        "__H.r = (function (x) { var r = \"\"; outer: for (var i = 0; i < 3; i++) { switch (x) { case OP.A: r += \"a\"; break outer; case OP.B: r += \"b\"; break; } r += \".\"; } return r; })",
    ], ["1", "2", "3"])
    shapes["recursive-case"] = ([
        "function __rec(n) { var r = \"\"; switch (n) { case OP.A: r += \"a\"; break; case OP.B: r += \"b\" + __rec(n - 1); break; default: r += \"d\"; break; } return r; }",
        "__H.r = (function (x) { return __rec(x); })",
    ], ["2", "1", "0"])
    shapes["nested-shared-const"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { case OP.A: r += \"a\"; switch (OP.B) { case OP.B: r += \"ib\"; break; default: r += \"id\"; } break; case OP.B: r += \"b\"; break; default: r += \"d\"; } return r; })",
    ], ["1", "2", "3"])
    shapes["dup-same-member-label"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { case OP.A: r += \"first\"; break; case OP.A: r += \"second\"; break; default: r += \"d\"; } return r; })",
    ], ["1", "2"])
    shapes["no-match-no-default"] = ([
        "__H.r = (function (x) { var r = \"pre\"; switch (x) { case OP.A: r = \"a\"; break; case OP.B: r = \"b\"; break; } return r; })",
    ], ["99", "-5"])
    shapes["switch-in-switch-elsewhere"] = ([
        "__H.r = (function (x) { var r = \"\"; switch (x) { case OP.A: r += \"a\"; break; default: r += \"d\"; } return r; })",
    ], ["1", "5"])

    for sname in shapes:
        fnlines, inputs = shapes[sname]
        for mode in MODES:
            body = ["var __H;",
                    "const OP = Object.freeze({ A: 1, B: 2, C: 3 });",
                    "__H = { r: \"\" };"] + fnlines
            sn = [S(["return __H.r(%s);" % i], "__H.r(%s)" % i) for i in inputs]
            add_probe("switchshape", mode, 0, "shape=%s" % sname, body, sn)


def emit_mixedlabels():
    for mode in MODES:
        body = ["var __H;",
                "const OP = Object.freeze({ A: 1, B: 5, C: 9 });",
                "__H = { r: function (x) { var r = \"\"; switch (x) { case OP.A: r += \"m1\"; break; case 1: r += \"l1\"; break; case OP.B: r += \"m5\"; break; case 9: r += \"l9\"; break; default: r += \"d\"; break; } return r; } };"]
        sn = [S(["return __H.r(%d);" % i], "__H.r(%d)" % i) for i in (1, 5, 9, 2, 10)]
        add_probe("mixedlabels", mode, 0, "member+literal dup labels", body, sn)


def emit_computed():
    for mode in MODES:
        body = ["var __H;",
                "var __key = \"A\";",
                "const OP = Object.freeze({ A: 1, B: 2, 0: 7 });",
                "const ALIAS = OP;",
                "__H = {};"]
        sn = [
            S(["return OP[\"A\"];"], "OP[\"A\"]"),
            S(["return OP[__key];"], "OP[__key]"),
            S(["return OP[\"A\" + \"\"];"], "OP[\"A\" + \"\"]"),
            S(["return OP[0];"], "OP[0]"),
            S(["return OP[\"0\"];"], "OP[\"0\"]"),
            S(["return OP && OP.A;"], "OP && OP.A"),
            S(["return ALIAS.A;"], "ALIAS.A"),
            S(["return ALIAS[\"B\"];"], "ALIAS[\"B\"]"),
            S(["var __t = 0; switch (1) { case OP[\"A\"]: __t = 1; break; default: __t = 2; } return __t;"], "(function () { var __t = 0; switch (1) { case OP[\"A\"]: __t = 1; break; default: __t = 2; } return __t; })()"),
            S(["try { return OP?.A; } catch (e) { return \"no-optchain:\" + e.name; }"], "(function () { try { return OP?.A; } catch (e) { return \"no-optchain:\" + e.name; } })()"),
        ]
        add_probe("computed", mode, 0, "computed/alias reads", body, sn)


def emit_keyatoms():
    for mode in MODES:
        body = ["var __H;",
                "const OP = Object.freeze({ default: 1, case: 2, switch: 3, Object: 4, freeze: 5, constructor: 6, \"0\": 7 });",
                "__H = { sw: function (k, x) { var r = \"D\"; if (k === 1) { switch (x) { case OP.default: r = \"kd\"; break; default: r = \"D\"; break; } } else { switch (x) { case OP.Object: r = \"kO\"; break; case OP[\"freeze\"]: r = \"kf\"; break; default: r = \"D\"; break; } } return r; } };"]
        sn = [
            S(["return OP.default;"], "OP.default"),
            S(["return OP[\"case\"];"], "OP[\"case\"]"),
            S(["return OP.switch;"], "OP.switch"),
            S(["return OP.Object;"], "OP.Object"),
            S(["return OP[\"freeze\"];"], "OP[\"freeze\"]"),
            S(["return OP.constructor;"], "OP.constructor"),
            S(["return OP[0];"], "OP[0]"),
            S(["return __H.sw(1, 1);"], "__H.sw(1, 1)"),
            S(["return __H.sw(2, 4);"], "__H.sw(2, 4)"),
            S(["return __H.sw(2, 5);"], "__H.sw(2, 5)"),
        ]
        add_probe("keyatoms", mode, 0, "keyword/atom-colliding keys", body, sn)


def emit_nestedobj():
    for mode in MODES:
        body = ["var __H;",
                "const INNER = Object.freeze({ A: 1, B: 2 });",
                "const OUTER = Object.freeze({ IN: INNER, C: 3 });",
                "const OUTER2 = Object.freeze({ IN: Object.freeze({ A: 4 }) });",
                "__H = { sw: function (x) { var r = \"D\"; switch (x) { case OUTER.IN.A: r = \"ia\"; break; case OUTER.IN.B: r = \"ib\"; break; case OUTER.C: r = \"c\"; break; default: r = \"D\"; break; } return r; } };"]
        sn = [
            S(["return OUTER.IN.A;"], "OUTER.IN.A"),
            S(["return OUTER2.IN.A;"], "OUTER2.IN.A"),
            S(["return __H.sw(1);"], "__H.sw(1)"),
            S(["return __H.sw(2);"], "__H.sw(2)"),
            S(["return __H.sw(3);"], "__H.sw(3)"),
            S(["return __H.sw(4);"], "__H.sw(4)"),
        ]
        add_probe("nestedobj", mode, 0, "nested frozen consts", body, sn)


def emit_multiconst():
    for n in (2, 3, 10):
        for mode in MODES:
            body = ["var __H;"]
            for i in range(n):
                body.append("const K%d = Object.freeze({ A: %d, B: %d });" % (i, i + 1, i + 101))
            labels = []
            for i in range(n):
                labels.append('case K%d.A: r += "a%d"; break;' % (i, i))
            for i in range(n):
                labels.append('case K%d.B: r += "b%d"; break;' % (i, i))
            body.append("__H = { sw: function (x) { var r = \"\"; switch (x) { %s default: r += \"d\"; break; } return r; } };"
                        % " ".join(labels))
            sn = [S(["return __H.sw(%d);" % v], "__H.sw(%d)" % v) for v in (1, n, n + 101, 0)]
            body.append("const DUP1 = Object.freeze({ V: 77 });")
            body.append("const DUP2 = Object.freeze({ V: 77 });")
            body.append("__H.dup = function (x) { var r = \"\"; switch (x) { case DUP1.V: r += \"d1\"; break; case DUP2.V: r += \"d2\"; break; default: r += \"dd\"; break; } return r; };")
            sn.append(S(["return __H.dup(77);"], "__H.dup(77)"))
            add_probe("multiconst", mode, 0, "nconst=%d + cross-const dup" % n, body, sn)


def emit_accessorconvert():
    for mode in MODES:
        body = ["var __H; var __cv = 0;",
                "const OP = { get A() { __cv += 1; return 3; } };",
                "Object.defineProperty(OP, \"A\", { value: 3, writable: false, configurable: false });",
                "Object.freeze(OP);",
                "__H = { sw: function (x) { var r = \"D\"; switch (x) { case OP.A: r = \"hit\"; break; default: r = \"D\"; break; } return r; } };"]
        sn = [S(["return OP.A;"], "OP.A"),
              S(["return __cv;"], "__cv"),
              S(["return __H.sw(3);"], "__H.sw(3)"),
              S(["return Object.isFrozen(OP);"], "Object.isFrozen(OP)")]
        add_probe("accessorconv", mode, 1, "getter->data->freeze", body, sn)


def emit_objattack2():
    for mode in MODES:
        body = ["var __H;",
                "const OP = Object.freeze({ A: 1 });",
                "__H = { rd: function () { return OP.A; } };",
                "var r1; try { Object = 1; const Q = Object.freeze({ B: 2 }); r1 = \"init-ok\"; } catch (e) { r1 = \"threw:\" + e.name; }",
                "var r2; try { r2 = \"read:\" + Q.B; } catch (e) { r2 = \"threw:\" + e.name; }"]
        sn = [S(["return __H.rd();"], "__H.rd()"), S(["return r1;"], "r1"), S(["return r2;"], "r2")]
        add_probe("objattack2", mode, 1, "attack=Object-assign", body, sn)



def emit_evaldecl():
    # const decl + switch inside separate compile units (indirect eval / new Function)
    for mode in MODES:
        body = ["var __H;", "__H = {};"]
        sn = [
            S(["var r = (0,eval)(\"const EV = Object.freeze({A:1}); var r; switch(1){case EV.A: r='hit'; break; default: r='def';} r\"); return r;"],
              "(0,eval)(\"const EV = Object.freeze({A:1}); var r; switch(1){case EV.A: r='hit'; break; default: r='def';} r\")"),
            S(["var r = new Function(\"const FV = Object.freeze({A:2}); var r; switch(2){case FV.A: r='hit'; break; default: r='def';} return r;\")(); return r;"],
              "new Function(\"const FV = Object.freeze({A:2}); var r; switch(2){case FV.A: r='hit'; break; default: r='def';} return r;\")()"),
            S(["var r = new Function(\"\\\"use strict\\\"; const FS = Object.freeze({A:3}); var r; switch(3){case FS.A: r=\\\"hit\\\"; break; default: r=\\\"def\\\";} return r;\")(); return r;"],
              "new Function(\"\\\"use strict\\\"; const FS = Object.freeze({A:3}); var r; switch(3){case FS.A: r=\\\"hit\\\"; break; default: r=\\\"def\\\";} return r;\")()"),
            S(["var r = (0,eval)(\"const EG = Object.freeze({A:1}); EG.A\"); try { r += \"|\" + EG.A; } catch (e) { r += \"|t:\" + e.name; } return r;"],
              "(function () { var r = (0,eval)(\"const EG = Object.freeze({A:1}); EG.A\"); try { r += \"|\" + EG.A; } catch (e) { r += \"|t:\" + e.name; } return r; })()"),
        ]
        add_probe("evaldecl", mode, 1, "decl-in-eval/newFn unit", body, sn)


def emit_freezeswap_family():
    # freeze-mutation timing relative to decl/use, on int members with case labels
    variants = {
        "swap-before-decl": [
            "var __sv = Object.freeze;",
            "Object.freeze = function (o) { o.__swapped = 1; return o; };",
            "const OP = Object.freeze({ A: 1 });",
            "Object.freeze = __sv;",
        ],
        "swap-after-decl-before-use": [
            "const OP = Object.freeze({ A: 1 });",
            "var __sv = Object.freeze;",
            "Object.freeze = function (o) { return o; };",
            "Object.freeze = __sv;",
        ],
        "seal-only": [
            "const OP = Object.seal({ A: 1 });",
        ],
        "freeze-twice-direct": [
            "const OP = Object.freeze(Object.freeze({ A: 1 }));",
        ],
        "define-property-frozen": [
            "const OP = Object.freeze(Object.defineProperty({ A: 1 }, \"A\", { writable: false }));",
        ],
        "null-proto": [
            "const OP = Object.freeze(Object.create(null, { A: { value: 1, enumerable: true } }));",
        ],
    }
    for vname, decl in variants.items():
        for mode in MODES:
            body = ["var __H;"] + decl
            body.append("__H = { sw: function (x) { var r = \"D\"; switch (x) { case OP.A: r = \"cA\"; break; default: r = \"D\"; break; } return r; }, rd: function () { return OP.A; } };")
            sn = [S(["return __H.rd();"], "__H.rd()"),
                  S(["return __H.sw(1);"], "__H.sw(1)"),
                  S(["return __H.sw(2);"], "__H.sw(2)"),
                  S(["return Object.isFrozen(OP);"], "Object.isFrozen(OP)")]
            declined = 0 if vname in ("freeze-twice-direct", "define-property-frozen", "null-proto") else 1
            add_probe("freezevar", mode, declined, "v=%s" % vname, body, sn)


# ---------------- emission ----------------

def emit_file(p, exps=None):
    lines = []
    if p["mode"] == "strict":
        lines.append('"use strict";')
    lines.append(HARNESS)
    lines.extend(p["body"])
    for sidx, sn in enumerate(p["snippets"]):
        sid_code = "\n".join(sn["code"])
        fn = "function () {\n%s\nreturn (%s);\n}" % (sid_code, sn["expr"])
        pid = p["pid"]
        sid = "s%d" % sidx
        if exps is None:
            lines.append('try { __cap("%s", "%s", __fmt((%s)())); } catch (e) { __capT("%s", "%s", e); }'
                         % (pid, sid, fn, pid, sid))
        else:
            kind, want = exps[sid]
            if kind == "V":
                lines.append('try { __chk("%s.%s", __fmt((%s)()), %s); } catch (e) { __unexp_throw("%s.%s", __etag(e)); }'
                             % (pid, sid, fn, json.dumps(want), pid, sid))
            else:
                lines.append('try { (%s)(); __no_throw("%s.%s"); } catch (e) { __chkThrow("%s.%s", __etag(e), %s); }'
                             % (fn, pid, sid, pid, sid, json.dumps(want)))
    lines.append("__summary();")
    return "\n".join(lines) + "\n"


def run_node(path, module):
    argv = [NODE]
    if module:
        argv.append("--input-type=module")
    with open(path, "rb") as f:
        cp = subprocess.run(argv, stdin=f, capture_output=True, text=True, timeout=30)
    return cp


def main():
    os.makedirs(PROBES, exist_ok=True)
    os.makedirs(CAPD, exist_ok=True)
    for f in os.listdir(PROBES):
        os.unlink(os.path.join(PROBES, f))
    for f in os.listdir(CAPD):
        os.unlink(os.path.join(CAPD, f))

    # ---- build probe list ----
    emit_switch_core()
    emit_biggrid("biggrid", 0, 0)
    emit_biggrid("biggrid2", 1, 3)
    emit_names()
    emit_declpos()
    emit_mutations()
    emit_objattack()
    emit_objattack2()
    emit_timing()
    emit_shadow()
    emit_int32trap()
    emit_switchshape()
    emit_mixedlabels()
    emit_computed()
    emit_keyatoms()
    emit_nestedobj()
    emit_multiconst()
    emit_accessorconvert()
    emit_evaldecl()
    emit_nonint()
    emit_tdz()
    emit_evalwith()
    emit_syntax()
    emit_misc()
    emit_byteid()
    emit_modules_focus()
    emit_freezeswap_family()

    print("probes: %d" % len(probes))
    with open(os.path.join(HERE, "gen_count.txt"), "w") as f:
        f.write(str(len(probes)) + "\n")

    # ---- phase 1: capture under node ----
    def cap_one(p):
        path = os.path.join(CAPD, p["pid"] + ".js")
        with open(path, "w") as f:
            f.write(emit_file(p, None))
        try:
            cp = run_node(path, p["mode"] == "module")
        except subprocess.TimeoutExpired:
            return p, None, "TIMEOUT"
        if cp.returncode != 0:
            return p, None, "rc=%d %s" % (cp.returncode, cp.stderr.strip().splitlines()[-1] if cp.stderr.strip() else "")
        exps = {}
        for line in cp.stdout.splitlines():
            parts = line.split(" ", 3)
            if len(parts) >= 4 and parts[0] in ("V", "T"):
                exps[parts[2]] = (parts[0], parts[3])
        return p, exps, None

    results = {}
    errors = []
    with ThreadPoolExecutor(max_workers=8) as ex:
        for p, exps, err in ex.map(cap_one, probes):
            if err:
                errors.append((p["pid"], err))
            else:
                results[p["pid"]] = exps

    if errors:
        print("CAPTURE ERRORS: %d" % len(errors))
        for pid, err in errors[:20]:
            print("  %s: %s" % (pid, err))
        sys.exit(2)

    # ---- phase 2: final probes with baked expectations ----
    man = []
    for p in probes:
        exps = results[p["pid"]]
        missing = [i for i in range(len(p["snippets"])) if ("s%d" % i) not in exps]
        if missing:
            print("missing expectations for %s: %s" % (p["pid"], missing))
            sys.exit(2)
        path = os.path.join(PROBES, p["pid"] + ".js")
        with open(path, "w") as f:
            f.write(emit_file(p, exps))
        man.append("\t".join([p["pid"], p["family"], p["mode"], str(p["declined"]), p["cells"],
                              str(len(p["snippets"]))]))
    with open(os.path.join(HERE, "manifest.tsv"), "w") as f:
        f.write("pid\tfamily\tmode\tdeclined\tcells\tsnippets\n")
        f.write("\n".join(man) + "\n")
    print("final probes written: %d" % len(probes))


if __name__ == "__main__":
    main()