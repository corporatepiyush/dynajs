#!/usr/bin/env python3
"""x2 MATRIX: for-let per-iteration binding copies vs labeled continue/break.

Dims:
  depth   : 1..4 nested labeled for-let loops
  exit    : none, continue, continue_L1, continue_L2, break_L1, break_L2,
            return, throw, tryfin (continue in try/finally), tryfin_L1
  iters   : 0, 1, 2, 3, 10 (per level)
  capture : arrow, funcdecl (annex-B block fn), letcap, closure2
Each probe asserts the FULL per-iteration binding trace array (level entries,
body entries, finally/return/catch events, then post-loop closure values).
Extras: update-expression side effects, destructuring heads, for-const,
for-in / for-of controls.
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bb_common as C

VARS = ["i", "j", "k", "l"]

EXITS = ["none", "continue", "continue_L1", "continue_L2", "break_L1",
         "break_L2", "return", "throw", "tryfin", "tryfin_L1"]
CAPTURES = ["arrow", "funcdecl", "letcap", "closure2"]
ITERS = [0, 1, 2, 3, 10]
DEPTHS = [1, 2, 3, 4]


def concat_vars(depth):
    return '"" + ' + ' + "," + '.join(VARS[:depth])


def exit_stmt(kind, depth):
    if kind == "none":
        return ""
    if kind == "continue":
        return "continue;"
    if kind == "continue_L1":
        return "continue L1;"
    if kind == "continue_L2":
        return "continue L2;"
    if kind == "break_L1":
        return "break L1;"
    if kind == "break_L2":
        return "break L2;"
    if kind == "return":
        return "return 7;"
    if kind == "throw":
        return 'throw new Error("boom");'
    if kind == "tryfin":
        return 'try { continue; } finally { LOG("fin"); }'
    if kind == "tryfin_L1":
        return 'try { continue L1; } finally { LOG("fin"); }'
    raise ValueError(kind)


def capture_stmt(kind, depth):
    cv = concat_vars(depth)
    if kind == "arrow":
        return "cs.push(function(){ return %s; });" % cv
    if kind == "funcdecl":
        return "function cap(){ return %s; } cs.push(cap);" % cv
    if kind == "letcap":
        return "let cc = %s; cs.push(function(){ return cc; });" % cv
    if kind == "closure2":
        return "cs.push(function(){ return (function(){ return %s; })(); });" % cv
    raise ValueError(kind)


def build_loops(depth, iters, ekind, ckind):
    src = ""
    for lvl in range(1, depth + 1):
        v = VARS[lvl - 1]
        src += "%sL%d: for (let %s = 0; %s < %d; %s++) {\n" % ("  " * lvl, lvl, v, v, iters, v)
        src += '%sLOG("L%d:" + %s);\n' % ("  " * (lvl + 1), lvl, v)
    pad = "  " * (depth + 1)
    src += pad + 'LOG("b");\n'
    src += pad + capture_stmt(ckind, depth) + "\n"
    src += pad + exit_stmt(ekind, depth) + "\n"
    for lvl in range(depth, 0, -1):
        src += "  " * lvl + "}\n"
    return src


PROBE = """(async function(){
var cs = [];
var res;
try {
  res = (function(){
%s
    return 0;
  })();
  LOG("fnret:" + res);
} catch (e) {
  var cls = (e && e.constructor && e.constructor.name) ? e.constructor.name : String(e);
  LOG("caught:" + ((e && e.message === "boom") ? cls + ":boom" : cls));
}
LOG("end");
for (var q = 0; q < cs.length; q++) { LOG("c:" + cs[q]()); }
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ log: __h_log }));
  __h_exit__(0);
}
assert_eq(__h_log, __EXPECT__.log, "trace");
})().then(function(){ __finish__(); }, function(e){ __h_fail += 1; __h_failed.push("top: " + (e && e.message)); __finish__(); });
"""


def gen_grid():
    rows = []
    for d in DEPTHS:
        for e in EXITS:
            if e in ("continue_L2", "break_L2") and d < 2:
                continue
            for n in ITERS:
                for c in CAPTURES:
                    pid = "x2_d%d_%s_n%02d_%s" % (d, e, n, c)
                    dims = "depth=%d exit=%s iters=%d cap=%s" % (d, e, n, c)
                    body = PROBE % build_loops(d, n, e, c)
                    p = C.write_probe("x2", pid, body)
                    data = C.record_node(p)
                    C.bake(p, data)
                    rows.append((pid, dims, ""))
    return rows


EXTRA_PROBES = [
    ("x2x_upd_side", "for-let update side effects (i += observed()) + tryfin continue", """
var upds = [];
L1: for (let i = 0; i < 3; i += (upds.push("u" + i), 1)) {
  LOG("b:" + i);
  cs.push(function(){ return i; });
  if (i === 1) { try { continue L1; } finally { LOG("fin:" + i); } }
}
LOG("upds:" + upds.join("|"));
"""),
    ("x2x_destr_head", "for-let destructuring head + continue L1", """
L1: for (let [a, b] of [[0, 10], [1, 11], [2, 12]]) {
  LOG("b:" + a + "," + b);
  cs.push(function(){ return a + ":" + b; });
  if (a === 1) continue L1;
}
"""),
    ("x2x_const_head", "for-const-of head + continue L1", """
L1: for (const x of [0, 1, 2]) {
  LOG("b:" + x);
  cs.push(function(){ return x; });
  if (x === 1) continue L1;
}
"""),
    ("x2x_const_classic", "classic for-const head + continue L1", """
L1: for (const i = 0; i < 3; i++) {
  LOG("b:" + i);
  cs.push(function(){ return i; });
  if (i === 1) continue L1;
}
"""),
    ("x2x_forin", "for-in head + continue L1 every iteration (never broken)", """
L1: for (let p in { a: 1, b: 2, c: 3 }) {
  LOG("b:" + p);
  cs.push(function(){ return p; });
  continue L1;
}
"""),
    ("x2x_forof_nested", "nested for-of, continue L2 from inner, break L1 at outer 1", """
L1: for (let a of [0, 1]) {
  LOG("L1:" + a);
  L2: for (let b of [0, 1, 2]) {
    LOG("L2:" + b);
    cs.push(function(){ return a + "/" + b; });
    if (b === 1) continue L2;
    if (a === 1) break L1;
  }
}
"""),
    ("x2x_tryfin_all", "continue L1 inside try/finally every iteration", """
L1: for (let i = 0; i < 3; i++) {
  try { LOG("t:" + i); cs.push(function(){ return i; }); continue L1; }
  finally { LOG("f:" + i); }
  LOG("unreachable:" + i);
}
"""),
    ("x2x_mixed_d3", "depth-3 mixed: continue L2 from L3, break L2 from L3", """
L1: for (let i = 0; i < 2; i++) {
  LOG("L1:" + i);
  L2: for (let j = 0; j < 2; j++) {
    LOG("L2:" + j);
    L3: for (let k = 0; k < 3; k++) {
      LOG("L3:" + k);
      cs.push(function(){ return i + "," + j + "," + k; });
      if (k === 0) continue L2;
      if (k === 2) break L2;
    }
    LOG("afterL3:" + j);
  }
}
"""),
    ("x2x_ret_in_fin", "return inside try with finally observing per-iteration", """
var res = (function(){
  L1: for (let i = 0; i < 3; i++) {
    LOG("b:" + i);
    cs.push(function(){ return i; });
    try { if (i === 1) return 42; } finally { LOG("f:" + i); }
  }
  return 0;
})();
LOG("fnret:" + res);
"""),
    ("x2x_upd_labeled", "labeled continue with update observing, depth 2", """
L1: for (let i = 0; i < 2; i++) {
  LOG("L1:" + i);
  L2: for (let j = 0; j < 3; j += (LOG("u:" + i + j), 1)) {
    LOG("L2:" + j);
    cs.push(function(){ return i + "/" + j; });
    if (j === 1) continue L1;
  }
}
"""),
]


def gen_extras():
    rows = []
    for pid, dims, snippet in EXTRA_PROBES:
        body = PROBE % snippet
        p = C.write_probe("x2", pid, body)
        data = C.record_node(p)
        C.bake(p, data)
        rows.append((pid, "extra " + dims, ""))
        C.log("x2 %s baked" % pid)
    return rows


def main():
    rows = gen_grid() + gen_extras()
    C.manifest("x2", rows)
    C.log("x2 total probes: %d" % len(rows))

if __name__ == "__main__":
    main()
