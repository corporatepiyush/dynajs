#!/usr/bin/env python3
"""x3 MATRIX: for-await abrupt completion -> iterator close -> continuation ORDER.

Dims:
  kinds    : sync_array, sync_custom, asyncgen (async function*),
             afs_wrap (async gen delegating yield* over sync it => async-from-sync),
             async_custom (next() returns promises)
  completion: break, throw_err, throw_val, return_val, continue,
             continue_outer, break_outer
  close cfg: ok, reject, nonobject (fulfilled with primitive), sync_throw,
             none, getter (getter observes ret access); generators also
             fin_throw / fin_await
  body     : plain (no await point), await (await null before completion),
             await2 (two awaits; sync_custom/async_custom only), gen
             (loop inside async generator; driver breaks after first yield)
Every probe asserts the EXACT event-trace array vs the node-baked sequence,
with microtask tick probes interleaved to pin down continuation timing.
Residual confirmation probes r1* / r2* cover the two DOCUMENTED ticketed
residuals (tagged in manifest; dynajs mismatch there = residual-confirms).
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
function mkSyncIt(name, cfg) {
  var n = 0;
  var it = {};
  it.next = function(){
    LOG(name + ".next:" + n);
    n++;
    return { value: n * 10, done: n > 3 };
  };
  it[Symbol.iterator] = function(){ return it; };
  if (cfg === "ok") it.return = function(){ LOG(name + ".ret-call"); return { value: 99, done: true }; };
  else if (cfg === "reject") it.return = function(){ LOG(name + ".ret-call"); return Promise.reject(new Error("rej")); };
  else if (cfg === "nonobject") it.return = function(){ LOG(name + ".ret-call"); return Promise.resolve(42); };
  else if (cfg === "sync_throw") it.return = function(){ LOG(name + ".ret-call"); throw new Error("sthrow"); };
  else if (cfg === "getter") Object.defineProperty(it, "return", { get: function(){ LOG(name + ".ret-get"); return function(){ LOG(name + ".ret-call"); return { value: 99, done: true }; }; }, configurable: true });
  return it;
}
function mkAsyncIt(name, cfg) {
  var n = 0;
  var it = {};
  it.next = function(){
    LOG(name + ".next:" + n);
    n++;
    return Promise.resolve({ value: n * 10, done: n > 3 });
  };
  it[Symbol.asyncIterator] = function(){ return it; };
  if (cfg === "ok") it.return = function(){ LOG(name + ".ret-call"); return Promise.resolve({ value: 99, done: true }); };
  else if (cfg === "reject") it.return = function(){ LOG(name + ".ret-call"); return Promise.reject(new Error("rej")); };
  else if (cfg === "nonobject") it.return = function(){ LOG(name + ".ret-call"); return Promise.resolve(42); };
  else if (cfg === "sync_throw") it.return = function(){ LOG(name + ".ret-call"); throw new Error("sthrow"); };
  else if (cfg === "getter") Object.defineProperty(it, "return", { get: function(){ LOG(name + ".ret-get"); return function(){ LOG(name + ".ret-call"); return Promise.resolve({ value: 99, done: true }); }; }, configurable: true });
  return it;
}
async function* mkGen(name, cfg) {
  try {
    LOG(name + ".start");
    yield 11; yield 22; yield 33;
  } finally {
    if (cfg === "fin_await") { await null; }
    LOG(name + ".fin");
    if (cfg === "fin_throw") { throw new Error("finthrow"); }
  }
}
async function* mkAFS(name, cfg) {
  try {
    yield* mkSyncRet(name + ".i", cfg === "ok" || cfg === "getter");
  } finally {
    if (cfg === "fin_await") { await null; }
    LOG(name + ".fin");
    if (cfg === "fin_throw") { throw new Error("finthrow"); }
  }
}
function mkSyncRet(name, hasRet) {
  var n = 0;
  var it = {};
  it.next = function(){
    LOG(name + ".next:" + n);
    n++;
    return { value: n * 10, done: n > 3 };
  };
  it[Symbol.iterator] = function(){ return it; };
  if (hasRet) it.return = function(){ LOG(name + ".ret-call"); return { value: 99, done: true }; };
  return it;
}
'''

KINDS = ["sync_array", "sync_custom", "asyncgen", "afs_wrap", "async_custom"]
CFGS_ALL = ["ok", "reject", "nonobject", "sync_throw", "none", "getter"]
CFGS_GEN = ["ok", "none", "fin_throw", "fin_await"]  # for asyncgen / afs_wrap
COMPS_ALL = ["break", "throw_err", "throw_val", "return_val", "continue",
             "continue_outer", "break_outer"]
COMPS_GEN = ["break", "throw_err", "throw_val", "continue"]
BODIES_FULL = ["plain", "await", "await2"]
BODIES_BASIC = ["plain", "await"]


def iter_expr(kind, cfg):
    if kind == "sync_array":
        return "[10, 20, 30]"
    if kind == "sync_custom":
        return 'mkSyncIt("it", "%s")' % cfg
    if kind == "asyncgen":
        return 'mkGen("g", "%s")' % cfg
    if kind == "afs_wrap":
        return 'mkAFS("a", "%s")' % cfg
    if kind == "async_custom":
        return 'mkAsyncIt("ay", "%s")' % cfg
    raise ValueError(kind)


def body_pre(body):
    if body == "plain":
        return ""
    if body == "await":
        return "await null;\n      "
    if body == "await2":
        return "await null;\n      await null;\n      "
    raise ValueError(body)


def loop_code(kind, cfg, comp, body):
    IE = iter_expr(kind, cfg)
    PRE = body_pre(body)
    if comp == "break":
        return ('for await (const v of %s) {\n'
                '      LOG("body:" + v);\n      %s'
                '      Promise.resolve().then(function(){ LOG("tick"); });\n'
                '      break;\n'
                '    }\n'
                '    LOG("after-loop");') % (IE, PRE)
    if comp in ("throw_err", "throw_val"):
        t = 'throw new Error("bv");' if comp == "throw_err" else "throw 42;"
        return ('for await (const v of %s) {\n'
                '      LOG("body:" + v);\n      %s'
                '      Promise.resolve().then(function(){ LOG("tick"); });\n'
                '      %s\n'
                '    }\n'
                '    LOG("after-loop");') % (IE, PRE, t)
    if comp == "return_val":
        return ('for await (const v of %s) {\n'
                '      LOG("body:" + v);\n      %s'
                '      Promise.resolve().then(function(){ LOG("tick"); });\n'
                '      return 42;\n'
                '    }\n'
                '    LOG("after-loop");') % (IE, PRE)
    if comp == "continue":
        return ('for await (const v of %s) {\n'
                '      try {\n'
                '        LOG("body:" + v);\n      %s'
                '        Promise.resolve().then(function(){ LOG("tick"); });\n'
                '        if (v === 10) { continue; }\n'
                '      } finally { LOG("fin:" + v); }\n'
                '      break;\n'
                '    }\n'
                '    LOG("after-loop");') % (IE, PRE)
    if comp == "continue_outer":
        return ('outer: for (var q = 0; q < 2; q++) {\n'
                '      LOG("q:" + q);\n'
                '      for await (const v of %s) {\n'
                '        LOG("body:" + v);\n      %s'
                '        Promise.resolve().then(function(){ LOG("tick"); });\n'
                '        continue outer;\n'
                '      }\n'
                '      LOG("inner-done:" + q);\n'
                '    }\n'
                '    LOG("after-loop");') % (IE, PRE)
    if comp == "break_outer":
        return ('outer: for await (const a of %s) {\n'
                '      LOG("outer:" + a);\n'
                '      for await (const b of %s) {\n'
                '        LOG("inner:" + b);\n      %s'
                '        Promise.resolve().then(function(){ LOG("tick"); });\n'
                '        break outer;\n'
                '      }\n'
                '    }\n'
                '    LOG("after-loop");') % (IE, IE, PRE)
    raise ValueError(comp)


def gen_loop_code(kind, cfg, comp, body):
    """gen body shape: the loop lives inside async generator G; the driver
    breaks after the first yielded value, cascading a close into G."""
    IE = iter_expr(kind, cfg)
    PRE = body_pre(body)
    if comp == "break":
        mid = "if (v >= 20) { break; }"
    elif comp == "throw_err":
        mid = 'if (v >= 20) { throw new Error("bv"); }'
    elif comp == "throw_val":
        mid = "if (v >= 20) { throw 42; }"
    elif comp == "continue":
        mid = "if (v === 10) { continue; }"
    else:
        raise ValueError(comp)
    return ('async function* G(){\n'
            '    try {\n'
            '      for await (const v of %s) {\n'
            '        LOG("body:" + v);\n        %s'
            '        Promise.resolve().then(function(){ LOG("tick"); });\n'
            '        %s\n'
            '        yield v;\n'
            '      }\n'
            '      LOG("G-loop-done");\n'
            '    } finally { LOG("G.fin"); }\n'
            '  }\n'
            '  for await (const w of G()) {\n'
            '    LOG("drv:" + w);\n'
            '    break;\n'
            '  }\n'
            '  LOG("after-loop");') % (IE, PRE, mid)


PROBE = PRELUDE + """(async function(){
var res;
try {
  res = await (async function(){
    %s
    return "fallthrough";
  })();
  LOG("ret-ok:" + res);
} catch (e) {
  LOG("caught:" + etag(e));
}
LOG("after");
Promise.resolve().then(function(){ LOG("t1"); });
await null;
LOG("done");
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ log: __h_log }));
  __h_exit__(0);
}
assert_eq(__h_log, __EXPECT__.log, "trace");
})().then(function(){ __finish__(); }, function(e){ __h_fail += 1; __h_failed.push("top: " + (e && e.message)); __finish__(); });
"""

# Residual r1: async-from-sync wrapper continuation timing on abrupt
# completion (dense microtask grid; catch position is the signal).
R1 = PRELUDE + """(async function(){
var tag = %s;
try {
  if (tag === "arr") {
    for await (const v of [10, 20, 30]) {
      LOG("body:" + v);
      Promise.resolve().then(function(){ LOG("ta"); })
        .then(function(){ LOG("tb"); });
      break;
    }
  } else if (tag === "custom") {
    for await (const v of mkSyncIt("it", "ok")) {
      LOG("body:" + v);
      Promise.resolve().then(function(){ LOG("ta"); })
        .then(function(){ LOG("tb"); });
      if (v === 10) { throw new Error("bv"); }
    }
  } else if (tag === "custom_break") {
    for await (const v of mkSyncIt("it", "none")) {
      LOG("body:" + v);
      Promise.resolve().then(function(){ LOG("ta"); })
        .then(function(){ LOG("tb"); });
      break;
    }
  } else {
    for await (const v of mkSyncRet("s", true)) {
      LOG("body:" + v);
      Promise.resolve().then(function(){ LOG("ta"); })
        .then(function(){ LOG("tb"); });
      break;
    }
  }
  LOG("ret-ok");
} catch (e) {
  LOG("caught:" + etag(e));
}
LOG("after");
Promise.resolve().then(function(){ LOG("t1"); });
await null;
LOG("done");
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ log: __h_log }));
  __h_exit__(0);
}
assert_eq(__h_log, __EXPECT__.log, "trace");
})().then(function(){ __finish__(); }, function(e){ __h_fail += 1; __h_failed.push("top: " + (e && e.message)); __finish__(); });
"""

# Residual r2: async iterator whose next() REJECTS must not trigger close.
R2 = PRELUDE + """(async function(){
var n = 0;
var it = {
  next: function(){
    n++;
    if (n === %d) { return Promise.reject(new Error("nx")); }
    LOG("next:" + n);
    return Promise.resolve({ value: n * 10, done: n > 3 });
  }%s
};
it[Symbol.asyncIterator] = function(){ return it; };
try {
  for await (const v of it) { LOG("body:" + v); }
  LOG("ret-ok");
} catch (e) {
  LOG("caught:" + etag(e));
}
LOG("after");
await null;
LOG("done");
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ log: __h_log }));
  __h_exit__(0);
}
assert_eq(__h_log, __EXPECT__.log, "trace");
})().then(function(){ __finish__(); }, function(e){ __h_fail += 1; __h_failed.push("top: " + (e && e.message)); __finish__(); });
"""

EXHAUST = PRELUDE + """(async function(){
try {
  for await (const v of %s) { LOG("body:" + v); }
  LOG("ret-ok");
} catch (e) {
  LOG("caught:" + etag(e));
}
await null;
LOG("done");
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ log: __h_log }));
  __h_exit__(0);
}
assert_eq(__h_log, __EXPECT__.log, "trace");
})().then(function(){ __finish__(); }, function(e){ __h_fail += 1; __h_failed.push("top: " + (e && e.message)); __finish__(); });
"""


def main():
    rows = []
    for kind in KINDS:
        cfgs = CFGS_ALL if kind in ("sync_custom", "async_custom") else (
            ["ok"] if kind == "sync_array" else CFGS_GEN)
        bodies = BODIES_BASIC if kind == "sync_array" else BODIES_FULL
        for cfg in cfgs:
            for comp in COMPS_ALL:
                for body in bodies:
                    if body == "gen" and comp not in COMPS_GEN:
                        continue
                    pid = "x3_%s_%s_%s_%s" % (kind, cfg, comp, body)
                    dims = "kind=%s cfg=%s comp=%s body=%s" % (kind, cfg, comp, body)
                    code = gen_loop_code(kind, cfg, comp, body) if body == "gen" \
                        else loop_code(kind, cfg, comp, body)
                    p = C.write_probe("x3", pid, PROBE % code)
                    data = C.record_node(p)
                    C.bake(p, data)
                    rows.append((pid, dims, ""))
        C.log("x3 kind %s done (%d rows so far)" % (kind, len(rows)))

    # controls: done-exit must NOT close
    for pid, expr, dims in [
        ("x3c_exhaust_sync_custom", 'mkSyncIt("it", "ok")', "control done-exit no close (sync custom)"),
        ("x3c_exhaust_async_custom", 'mkAsyncIt("ay", "ok")', "control done-exit no close (async custom)"),
        ("x3c_exhaust_sync_array", "[10, 20, 30]", "control done-exit no close (array)"),
    ]:
        p = C.write_probe("x3", pid, EXHAUST % expr)
        data = C.record_node(p)
        C.bake(p, data)
        rows.append((pid, "control " + dims, ""))

    # documented residual r1 (4 shapes)
    for tag in ["arr", "custom", "custom_break", "native"]:
        pid = "x3r1_%s" % tag
        p = C.write_probe("x3", pid, R1 % ('"%s"' % tag))
        data = C.record_node(p)
        C.bake(p, data)
        rows.append((pid, "residual-r1 async-from-sync continuation timing tag=%s" % tag, "residual-r1"))

    # documented residual r2 (3 shapes)
    for failn, hasret in [(1, True), (2, True), (1, False)]:
        pid = "x3r2_n%d_ret%d" % (failn, 1 if hasret else 0)
        ret = ('\n  , return: function(){ LOG("ret-called"); return Promise.resolve({ value: undefined, done: true }); }'
               if hasret else "")
        p = C.write_probe("x3", pid, R2 % (failn, ret))
        data = C.record_node(p)
        C.bake(p, data)
        rows.append((pid, "residual-r2 async-gen next-reject close failnext=%d hasret=%d" % (failn, 1 if hasret else 0),
                     "residual-r2"))

    C.manifest("x3", rows)
    C.log("x3 total probes: %d" % len(rows))

if __name__ == "__main__":
    main()
