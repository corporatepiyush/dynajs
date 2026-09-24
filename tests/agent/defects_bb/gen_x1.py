#!/usr/bin/env python3
"""x1 MATRIX: same-scope lexical/function-declaration collisions.

Dims:
  pairs    : ordered (a, b) over {let, const, class, function, var}   (25)
  order    : implicit in the ordered pair (a first, b second)
  scopes   : 11 eval-realized (eval_indirect, eval_strict, eval_direct,
             eval_direct_strict, newfn, newfn_strict, block, switch_case,
             for_head, static_block, enclosing_lex)
             + 3 file-level (script, script_strict, module)
  names    : x, eval, arguments, astral(\\u{1D49C})
  body     : function-with-body vs empty-function
For every (pair, order, scope, name, body) cell: node's exact outcome
(SyntaxError-kind vs accepted + observed binding values) is baked at
generation time and asserted.
"""
import json
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bb_common as C

KINDS = ["let", "const", "class", "function", "var"]

def decl(kind, name, full_body):
    n = name
    if kind == "let":
        return "let %s = 1;" % n
    if kind == "const":
        return "const %s = 2;" % n
    if kind == "class":
        return "class %s { constructor() { this.tag = 3; } }" % n
    if kind == "function":
        return ("function %s() { return 4; }" if full_body else "function %s() {}") % n
    if kind == "var":
        return "var %s = 5;" % n
    raise ValueError(kind)

def decl_head(kind, name):  # for for-head placement (no trailing semicolon)
    if kind == "let":
        return "let %s = 1" % name
    if kind == "var":
        return "var %s = 5" % name
    return None

NAMES = [("x", "x"), ("ev", "eval"), ("arg", "arguments"), ("ast", "\\u{1D49C}")]

# ---- eval-realized scopes ------------------------------------------------
# wrapper(src) builds a JS expression string that compiles+runs src in the
# target scope; compile/run errors surface as thrown values.
def w_eval_indirect(src):   return '(0,eval)(%s)' % js_str(src)
def w_eval_strict(src):     return '(0,eval)("%s" + %s)' % ('use strict;\\n', js_str(src))
def w_eval_direct(src):     return '(function(){ eval(%s); })()' % js_str(src)
def w_eval_direct_strict(src): return '(function(){ "use strict"; eval(%s); })()' % js_str(src)
def w_newfn(src):           return '(new Function(%s))()' % js_str(src)
def w_newfn_strict(src):    return '(new Function("%s" + %s))()' % ('use strict;\\n', js_str(src))
def w_block(src):           return '(new Function("{" + %s + "}"))()' % js_str(src)
def w_switch(src):          return '(new Function("switch(0){ case 0: " + %s + " }"))()' % js_str(src)
def w_static(src):          return '(new Function("class CWx { static { " + %s + " } }"))()' % js_str(src)

def js_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'

EVAL_SCOPES = [
    ("eval_indirect", w_eval_indirect),
    ("eval_strict", w_eval_strict),
    ("eval_direct", w_eval_direct),
    ("eval_direct_strict", w_eval_direct_strict),
    ("newfn", w_newfn),
    ("newfn_strict", w_newfn_strict),
    ("block", w_block),
    ("switch_case", w_switch),
    ("static_block", w_static),
]

def build_src(pair, name, full_body, scope):
    """Source text placed inside the scope; observations appended inside."""
    a, b = pair
    if scope == "for_head":
        ha = decl_head(a, name)
        if ha is None:
            return None
        body = decl(b, name, full_body) + " __OBS__(__SER__(%s));" % name
        return "for (%s; false;) { %s }" % (ha, body)
    if scope == "enclosing_lex":
        outer = decl(a, name, full_body)
        inner = decl(b, name, full_body)
        return ("var __o1, __o2; %s { %s __OBS__(__SER__(%s)); } "
                "__OBS__(__SER__(%s));" % (outer, inner, name, name))
    return decl(a, name, full_body) + " " + decl(b, name, full_body) + \
        " __OBS__(__SER__(%s));" % name

def scope_wrap(scope, src):
    if scope == "for_head":
        return '(new Function(%s))()' % js_str(src)
    if scope == "enclosing_lex":
        return '(new Function(%s))()' % js_str(src)
    return EVAL_SCOPES_DICT[scope](src)

EVAL_SCOPES_DICT = dict(EVAL_SCOPES)

PROBE_HEAD = """(function(){
var __cells = {};
function tryCell(key, fn) {
  var out;
  try { fn(); out = { threw: null }; }
  catch (e) { out = { threw: (e && e.constructor && e.constructor.name) ? e.constructor.name : String(e && e.name || e) }; }
  if (out.threw === null) { out.obs = __h_obs.splice(0, __h_obs.length); }
  else { __h_obs.length = 0; }
  __cells[key] = out;
}
function RUN(){ return __EXPECT__; }
"""

PROBE_TAIL = """if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ cells: __cells }));
  __h_exit__(0);
}
var __keys = Object.keys(__EXPECT__.cells), __i;
for (__i = 0; __i < __keys.length; __i++) {
  (function(k){
    test(k, function(){
      var got = __cells[k];
      var want = __EXPECT__.cells[k];
      if (want.threw !== null) {
        assert(got && got.threw !== null, "expected throw " + want.threw + " got " + JSON.stringify(got));
        assert_eq(got.threw, want.threw, "threw kind");
      } else {
        assert(got && got.threw === null, "unexpected throw " + (got && got.threw));
        assert_eq(got.obs, want.obs, "obs");
      }
    });
  })(__keys[__i]);
}
})();
__finish__();
"""


def gen_eval_scopes():
    # ONE CELL PER FILE: indirect eval leaks global bindings (var/function to
    # the global object; eval-scope lexicals persist on dynajs), so batching
    # cells with the same name into one probe made outcomes order-dependent.
    rows = []
    pairs = [(a, b) for a in KINDS for b in KINDS]
    scope_names = [s for s, _ in EVAL_SCOPES] + ["for_head", "enclosing_lex"]
    for pi, (a, b) in enumerate(pairs):
        for scope in scope_names:
            for nkey, nsrc in NAMES:
                for fb in (1, 0):
                    src = build_src((a, b), nsrc, fb, scope)
                    if src is None:
                        continue
                    pid = "x1_p%02d_%s_%s_%s_%s_b%d" % (pi, a, b, scope, nkey, fb)
                    dims = "pair=%s+%s scope=%s name=%s body=%d" % (a, b, scope, nkey, fb)
                    key = "%s|%s|b%d" % (scope, nkey, fb)
                    if scope in ("for_head", "enclosing_lex"):
                        call = 'tryCell(%s, function(){ return (new Function(%s))(); })' % (js_str(key), js_str(src))
                    else:
                        call = 'tryCell(%s, function(){ return %s; })' % (js_str(key), scope_wrap(scope, src))
                    body = PROBE_HEAD + call + "\n" + PROBE_TAIL
                    p = C.write_probe("x1", pid, body)
                    data = C.record_node(p)
                    C.bake(p, data)
                    rows.append((pid, dims, ""))
    return rows


def gen_file_level():
    rows = []
    pairs = [(a, b) for a in KINDS for b in KINDS]
    for pi, (a, b) in enumerate(pairs):
        for nkey, nsrc in [("x", "x"), ("ev", "eval")]:
            for fb in (1,):
                for scope, ext, flags, strictpro in [
                    ("script", ".js", [], False),
                    ("script_strict", ".js", [], True),
                    ("module", ".mjs", [], False),
                ]:
                    pid = "x1f_p%02d_%s_%s_%s_%s_b%d" % (pi, a, b, nkey, scope, fb)
                    snippet = decl(a, nsrc, fb) + " __OBS__(__SER__(%s)); " % nsrc + \
                              decl(b, nsrc, fb) + " __OBS__(__SER__(%s));" % nsrc
                    if strictpro:
                        snippet = '"use strict";\n' + snippet
                    # probe = harness + snippet at true top level (or module)
                    body = snippet + """
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ obs: __h_obs }));
} else {
  test("file-level values", function(){ assert_eq(__h_obs, __EXPECT__.obs, "obs"); });
  __finish__();
}
"""
                    p = C.write_probe("x1", pid, body, ext=ext)
                    # bake: node decides accept (obs) vs whole-file SyntaxError
                    rc, out = C.node_outcome(p, extra_flags=(["-r", C.REC_PRELOAD]))
                    if rc == 0 and "RECDATA:" in out:
                        data = json.loads(out[out.index("RECDATA:"):].splitlines()[0][8:])
                        C.bake(p, data)
                        rows.append((pid, "pair=%s+%s name=%s body=%s scope=%s" % (a, b, nkey, fb, scope), ""))
                    else:
                        # node rejects the whole file: reduce to the bare decl
                        # snippet; runner asserts rc!=0 + empty stdout (baked).
                        bare = "// x1 file-level: expected whole-file compile rejection (node rc=%d)\n" % rc + \
                               decl(a, nsrc, fb) + "\n" + decl(b, nsrc, fb) + "\n"
                        with open(p, "w") as f:
                            f.write(bare)
                        with open(p + ".expect", "w") as f:
                            f.write("rc-nonzero\nstdout-empty\n")
                        rows.append((pid, "pair=%s+%s name=%s body=%s scope=%s" % (a, b, nkey, fb, scope), "rcprobe"))
                    C.log("x1f %s rc=%d" % (pid, rc))
    return rows


def main():
    rows = []
    rows += gen_eval_scopes()
    rows += gen_file_level()
    C.manifest("x1", rows)
    C.log("x1 total probes: %d" % len(rows))

if __name__ == "__main__":
    main()
