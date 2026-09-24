#!/usr/bin/env python3
"""x5 MATRIX: spread observes GetMethod(@@iterator) EXACTLY ONCE.

Dims:
  sites    : arr_lit, arr_lit2 (1, ...it, 9), call_args, call_args2,
             new_args, args_spread ([...arguments]), yield_star,
             yield_star_break, array_from, destructure
  behavior : same (getter returns one iterator), diff (fresh iterator per
             get -- FIRST one must be the one iterated), throws, nonobject
             (getter returns 42), null (getter returns null)
  len      : L0, L2, L3 elements
  double   : two spreads in one expression for arr_lit / call_args
Each probe asserts the exact side-effect log: getter-call count, iterator
identity markers (which iterator was actually iterated), next/ret events,
result values, error class.
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bb_common as C

PRELUDE = r'''
function etag(e) {
  var cls = (e && e.constructor && e.constructor.name) ? e.constructor.name : String(e);
  var msg = e && e.message;
  if (msg && "|bv|rej|sthrow|gthrow|nx|finthrow|boom|".indexOf("|" + msg + "|") >= 0) { return cls + ":" + msg; }
  return cls;
}
function mkIt(tag, len, hasRet) {
  var n = 0;
  var it = {};
  it.next = function(){
    LOG(tag + ".next:" + n);
    n++;
    if (n > len) { return { value: undefined, done: true }; }
    return { value: tag + ":" + n, done: false };
  };
  if (hasRet) { it.return = function(v){ LOG(tag + ".ret"); return { value: v, done: true }; }; }
  return it;
}
function mkIterable(behavior, len, hasRet) {
  var calls = 0;
  var obj = {};
  Object.defineProperty(obj, Symbol.iterator, {
    configurable: true,
    get: function(){
      calls++;
      LOG("get#" + calls);
      if (behavior === "throws") { throw new Error("gthrow"); }
      if (behavior === "nonobject") { return 42; }
      if (behavior === "null") { return null; }
      if (behavior === "diff") { return mkIt("it" + calls, len, hasRet); }
      return mkIt("itA", len, hasRet);
    }
  });
  return obj;
}
function ITTER(behavior, len, hasRet) { return mkIterable(behavior, len, hasRet); }
'''

SITES = {
    "arr_lit": 'var r = [%IT%];\nLOG("res:" + r.join("|"));',
    "arr_lit2": 'var r = [1, %IT%, 9];\nLOG("res:" + r.join("|"));',
    "call_args": ('function cf(){ var a = []; for (var i = 0; i < arguments.length; i++) a.push(String(arguments[i])); LOG("args:" + a.join("|")); }\ncf(%IT%);'),
    "call_args2": ('function cf(){ var a = []; for (var i = 0; i < arguments.length; i++) a.push(String(arguments[i])); LOG("args:" + a.join("|")); }\ncf(1, %IT%, 9);'),
    "new_args": ('function NF(){ this.a = Array.prototype.slice.call(arguments); }\nvar o = new NF(%IT%);\nLOG("new:" + o.a.join("|"));'),
    "args_spread": ('function ga(){\n'
                    '  Object.defineProperty(arguments, Symbol.iterator, { configurable: true, get: function(){ LOG("get#" + (++arguments.__gc || (arguments.__gc = 1))); return GETTER_RET(); } });\n'
                    '  var r = [...arguments];\n'
                    '  LOG("res:" + r.join("|"));\n'
                    '}\n'),
    "yield_star": ('function* yg(){ yield* %IT%; }\n'
                   'var gi = yg();\n'
                   'var acc = [];\n'
                   'var st;\n'
                   'while (!(st = gi.next()).done) { acc.push(String(st.value)); }\n'
                   'LOG("res:" + acc.join("|"));'),
    "yield_star_break": ('function* yg(){ yield* %IT%; LOG("yg-unreached"); }\n'
                         'var gi = yg();\n'
                         'var st1 = gi.next();\n'
                         'LOG("v1:" + st1.value);\n'
                         'gi.return("stop");\n'
                         'LOG("after-gi");'),
    "array_from": 'var r = Array.from(%IT%);\nLOG("res:" + r.join("|"));',
    "destructure": 'var [a, b] = %IT%;\nLOG("d:" + a + "," + b);',
}

# args_spread needs custom wiring (getter installed ON the arguments object):
ARGS_SPREAD_BODY = '''function ga(){
  var gc = 0;
  Object.defineProperty(arguments, Symbol.iterator, {
    configurable: true,
    get: function(){ gc++; LOG("get#" + gc); %(retexpr2)s }
  });
  var r = [...arguments];
  LOG("res:" + r.join("|"));
}
ga();
'''

BEHAVIORS = ["same", "diff", "throws", "nonobject", "null"]
LENS = [("L0", 0), ("L2", 2), ("L3", 3)]

PROBE = PRELUDE + """(function(){
try {
%s
} catch (e) {
  LOG("caught:" + etag(e));
}
LOG("after");
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ log: __h_log }));
  __h_exit__(0);
}
assert_eq(__h_log, __EXPECT__.log, "trace");
})();
__finish__();
"""


def it_expr(behavior, lenname, hasret=True):
    return 'ITTER("%s", %s, %s)' % (behavior, int(lenname[1:]), "true" if hasret else "false")


def ret_for(behavior, ln, hasret):
    if behavior == "throws":
        return 'throw new Error("gthrow");'
    if behavior == "nonobject":
        return "return 42;"
    if behavior == "null":
        return "return null;"
    if behavior == "diff":
        return 'return mkIt("it" + gc, %d, %s);' % (ln, "true" if hasret else "false")
    return 'return mkIt("itA", %d, %s);' % (ln, "true" if hasret else "false")


def main():
    rows = []
    for site in SITES:
        for beh in BEHAVIORS:
            for lname, ln in LENS:
                if site == "args_spread":
                    body = ARGS_SPREAD_BODY % {"retexpr2": ret_for(beh, ln, True)}
                    code = body
                else:
                    code = SITES[site].replace("%IT%", it_expr(beh, lname))
                pid = "x5_%s_%s_%s" % (site, beh, lname)
                dims = "site=%s behavior=%s len=%s" % (site, beh, lname)
                p = C.write_probe("x5", pid, PROBE % code)
                data = C.record_node(p)
                C.bake(p, data)
                rows.append((pid, dims, ""))

    # double-spread variants (getter must be observed once per spread site)
    for beh in BEHAVIORS:
        for lname, ln in LENS:
            ie = it_expr(beh, lname)
            code = 'var r = [%s, %s];\nLOG("res:" + r.join("|"));' % (ie, ie)
            pid = "x5d_arr_lit_%s_%s" % (beh, lname)
            dims = "site=arr_lit double behavior=%s len=%s" % (beh, lname)
            p = C.write_probe("x5", pid, PROBE % code)
            data = C.record_node(p)
            C.bake(p, data)
            rows.append((pid, dims, ""))

            code2 = ('function cf(){ var a = []; for (var i = 0; i < arguments.length; i++) a.push(String(arguments[i])); LOG("args:" + a.join("|")); }\n'
                     'cf(%s, %s);' % (ie, ie))
            pid2 = "x5d_call_args_%s_%s" % (beh, lname)
            dims2 = "site=call_args double behavior=%s len=%s" % (beh, lname)
            p2 = C.write_probe("x5", pid2, PROBE % code2)
            data2 = C.record_node(p2)
            C.bake(p2, data2)
            rows.append((pid2, dims2, ""))

    # native-iterator controls (no observable getter; value semantics only)
    for cname, expr in [
        ("string", '"ab\\u{1D49C}c"'),
        ("map", 'new Map([[1, "one"], [2, "two"]])'),
        ("set", 'new Set(["s1", "s2"])'),
    ]:
        code = 'var r = [%s];\nLOG("res:" + r.join("|"));' % expr
        pid = "x5c_%s" % cname
        dims = "control native %s" % cname
        p = C.write_probe("x5", pid, PROBE % code)
        data = C.record_node(p)
        C.bake(p, data)
        rows.append((pid, dims, ""))

    # destructure with return-less iterator (close must be skipped silently)
    for lname, ln in [("L3", 3), ("L2", 2)]:
        code = 'var [a, b] = %s;\nLOG("d:" + a + "," + b);' % it_expr("same", lname, hasret=False)
        pid = "x5c_destructure_noret_%s" % lname
        dims = "control destructure len=%s hasret=false" % lname
        p = C.write_probe("x5", pid, PROBE % code)
        data = C.record_node(p)
        C.bake(p, data)
        rows.append((pid, dims, ""))

    C.manifest("x5", rows)
    C.log("x5 total probes: %d" % len(rows))

if __name__ == "__main__":
    main()
