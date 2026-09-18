#!/usr/bin/env python3
"""x6 MATRIX: Array.prototype.sort materializes prototype-resolved holes.

Dims:
  sizes     : 0, 1, 2, 3, 5, 17, 100, 10000
  holes     : none, leading, middle, trailing, all, alternating
  proto     : none, indexed (data props at hole positions), accessor
              (getter-only at hole positions -- DOCUMENTED ticket-x6 residual:
              patched dynajs throws where node skips; rows tagged ticket-x6),
              during (comparator adds proto prop mid-sort)
  comparator: identity, reverse, mutlen, writes, readproto, throws
  frozen    : control subset (frozen arrays must throw on write-back)
Asserts: final own-keys + values + descriptor snapshot (full for sizes <= 100,
compact for 10000) + comparator call-count, plus the full comparator arg
sequence for sizes <= 100. Note: comparator call COUNT is implementation-
defined per spec; count-only mismatches are classed nit-callcount by run.sh.
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bb_common as C

PRELUDE = r'''
function holeList(pattern, size) {
  var h = [];
  if (pattern === "leading") { if (size > 0) h.push(0); }
  else if (pattern === "middle") { if (size > 1) h.push(size >> 1); }
  else if (pattern === "trailing") { if (size > 0) h.push(size - 1); }
  else if (pattern === "all") { for (var i = 0; i < size; i++) h.push(i); }
  else if (pattern === "alternating") { for (var j = 0; j < size; j += 2) h.push(j); }
  return h;
}
function buildArr(size, pattern) {
  var holes = holeList(pattern, size);
  var a = new Array(size);
  for (var i = 0; i < size; i++) {
    if (holes.indexOf(i) === -1) { a[i] = i * 7; }
  }
  return a;
}
function buildProto(kind, pattern, size) {
  var p = {};
  if (kind === "none") { return p; }
  var holes = holeList(pattern, size);
  for (var i = 0; i < holes.length; i++) {
    var k = holes[i];
    if (kind === "indexed") { p[k] = 900000 + k; }
    else if (kind === "accessor") {
      Object.defineProperty(p, k, { get: function(){ return 900001; }, configurable: true });
    }
  }
  return p;
}
function snapFull(a) {
  var ks = Object.getOwnPropertyNames(a).sort();
  var out = [];
  for (var i = 0; i < ks.length; i++) {
    var k = ks[i];
    var d = Object.getOwnPropertyDescriptor(a, k);
    var v = ("value" in d) ? String(d.value) : "acc";
    out.push(k + "=" + v + "/" + (d.writable ? 1 : 0) + (d.enumerable ? 1 : 0) + (d.configurable ? 1 : 0));
  }
  return out;
}
function snapCompact(a) {
  var ks = Object.getOwnPropertyNames(a).sort();
  var head = [];
  for (var i = 0; i < Math.min(6, ks.length); i++) {
    var d = Object.getOwnPropertyDescriptor(a, ks[i]);
    head.push(ks[i] + "=" + (("value" in d) ? String(d.value) : "acc"));
  }
  var tail = [];
  for (var j = Math.max(0, ks.length - 6); j < ks.length; j++) {
    var d2 = Object.getOwnPropertyDescriptor(a, ks[j]);
    tail.push(ks[j] + "=" + (("value" in d2) ? String(d2.value) : "acc"));
  }
  return { len: a.length, n: ks.length, head: head, tail: tail };
}
'''

SIZES = [0, 1, 2, 3, 5, 17, 100, 10000]
PATTERNS = ["none", "leading", "middle", "trailing", "all", "alternating"]
PROTOS = ["none", "indexed", "accessor", "during"]
COMPS = ["identity", "reverse", "mutlen", "writes", "readproto", "throws"]

CMP_BODY = {
    "identity": "return x < y ? -1 : (x > y ? 1 : 0);",
    "reverse": "return y < x ? -1 : (y > x ? 1 : 0);",
    "mutlen": "if (cmpCalls === 1) { LOG(\"mutlen\"); a.length = 1; } return y < x ? -1 : (y > x ? 1 : 0);",
    "writes": "if (cmpCalls === 1) { LOG(\"write0\"); a[0] = 7777; } return x < y ? -1 : (x > y ? 1 : 0);",
    "readproto": "if (cmpCalls < 4) { LOG(\"read:\" + (a[HOLE0] === undefined ? \"u\" : String(a[HOLE0]))); } return x < y ? -1 : (x > y ? 1 : 0);",
    "throws": "if (cmpCalls === 2) { throw new Error(\"cmpboom\"); } return x < y ? -1 : (x > y ? 1 : 0);",
}

PROBE_FULL = PRELUDE + """(function(){
var size = %d, pattern = "%s", pk = "%s", ck = "%s";
var a = buildArr(size, pattern);
var proto = buildProto(pk === "during" ? "none" : pk, pattern, size);
Object.setPrototypeOf(a, proto);
var holes = holeList(pattern, size);
var HOLE0 = holes.length ? holes[0] : 0;
var cmpCalls = 0;
var added = false;
try {
  a.sort(function(x, y){
    cmpCalls++;
    LOG("cmp:" + (x === undefined ? "u" : String(x)) + "," + (y === undefined ? "u" : String(y)));
    if (pk === "during" && !added) {
      added = true;
      Object.defineProperty(proto, HOLE0, { value: 950000 + HOLE0, writable: true, enumerable: true, configurable: true });
      LOG("added-proto");
    }
    %s
  });
} catch (e) {
  LOG("sort-throw:" + (e && e.constructor && e.constructor.name ? e.constructor.name : String(e)));
}
LOG("calls:" + cmpCalls);
var SN = snapFull(a);
LOG("snap:" + SN.join(";"));
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ log: __h_log }));
  __h_exit__(0);
}
var cmpseq = [], snaparr = [], callsarr = [];
for (var __i = 0; __i < __h_log.length; __i++) {
  var e = __h_log[__i];
  if (e.indexOf("snap:") === 0) { snaparr.push(e); }
  else if (e.indexOf("calls:") === 0) { callsarr.push(e); }
  else { cmpseq.push(e); }
}
test("cmpseq", function(){ assert_eq(cmpseq, __EXPECT__.log.filter(function(e){ return e.indexOf("snap:") !== 0 && e.indexOf("calls:") !== 0; }), "cmpseq"); });
test("snap", function(){ assert_eq(snaparr, __EXPECT__.log.filter(function(e){ return e.indexOf("snap:") === 0; }), "snap"); });
test("calls", function(){ assert_eq(callsarr, __EXPECT__.log.filter(function(e){ return e.indexOf("calls:") === 0; }), "calls"); });
})();
__finish__();
"""

PROBE_COMPACT = PRELUDE + """(function(){
var size = %d, pattern = "%s", pk = "%s", ck = "%s";
var a = buildArr(size, pattern);
var proto = buildProto(pk === "during" ? "none" : pk, pattern, size);
Object.setPrototypeOf(a, proto);
var holes = holeList(pattern, size);
var HOLE0 = holes.length ? holes[0] : 0;
var cmpCalls = 0;
var added = false;
try {
  a.sort(function(x, y){
    cmpCalls++;
    if (pk === "during" && !added) {
      added = true;
      Object.defineProperty(proto, HOLE0, { value: 950000 + HOLE0, writable: true, enumerable: true, configurable: true });
      LOG("added-proto");
    }
    %s
  });
} catch (e) {
  LOG("sort-throw:" + (e && e.constructor && e.constructor.name ? e.constructor.name : String(e)));
}
LOG("calls:" + cmpCalls);
var SC = snapCompact(a);
LOG("snaplen:" + SC.len + ":" + SC.n);
LOG("snaphead:" + SC.head.join(";"));
LOG("snaptail:" + SC.tail.join(";"));
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ log: __h_log }));
  __h_exit__(0);
}
var cmpseq = [], snaparr = [], callsarr = [];
for (var __i = 0; __i < __h_log.length; __i++) {
  var e = __h_log[__i];
  if (e.indexOf("snaplen:") === 0 || e.indexOf("snaphead:") === 0 || e.indexOf("snaptail:") === 0) { snaparr.push(e); }
  else if (e.indexOf("calls:") === 0) { callsarr.push(e); }
  else { cmpseq.push(e); }
}
test("cmpseq", function(){ assert_eq(cmpseq, __EXPECT__.log.filter(function(e){ return e.indexOf("snaplen:") !== 0 && e.indexOf("snaphead:") !== 0 && e.indexOf("snaptail:") !== 0 && e.indexOf("calls:") !== 0; }), "cmpseq"); });
test("snap", function(){ assert_eq(snaparr, __EXPECT__.log.filter(function(e){ return e.indexOf("snaplen:") === 0 || e.indexOf("snaphead:") === 0 || e.indexOf("snaptail:") === 0; }), "snap"); });
test("calls", function(){ assert_eq(callsarr, __EXPECT__.log.filter(function(e){ return e.indexOf("calls:") === 0; }), "calls"); });
})();
__finish__();
"""

FROZEN_PROBE = PRELUDE + """(function(){
var size = %d, pattern = "%s", pk = "%s", ck = "%s";
var a = buildArr(size, pattern);
var proto = buildProto(pk, pattern, size);
Object.setPrototypeOf(a, proto);
Object.freeze(a);
var cmpCalls = 0;
try {
  a.sort(function(x, y){
    cmpCalls++;
    LOG("cmp:" + (x === undefined ? "u" : String(x)) + "," + (y === undefined ? "u" : String(y)));
    %s
  });
  LOG("no-throw");
} catch (e) {
  LOG("sort-throw:" + (e && e.constructor && e.constructor.name ? e.constructor.name : String(e)));
}
LOG("calls:" + cmpCalls);
var SN = snapFull(a);
LOG("snap:" + SN.join(";"));
if (typeof __REC__ !== "undefined") {
  console.log("RECDATA:" + JSON.stringify({ log: __h_log }));
  __h_exit__(0);
}
assert_eq(__h_log, __EXPECT__.log, "frozen log+snap");
})();
__finish__();
"""


def main():
    rows = []
    for size in SIZES:
        for pattern in PATTERNS:
            if size == 0 and pattern != "none":
                continue
            for pk in PROTOS:
                for ck in COMPS:
                    tag = "ticket-x6" if pk == "accessor" else ""
                    if size > 100:
                        tpl, pid_s = PROBE_COMPACT, "c"
                    else:
                        tpl, pid_s = PROBE_FULL, "f"
                    pid = "x6_%s_s%d_%s_%s_%s" % (pid_s, size, pattern, pk, ck)
                    dims = "size=%d holes=%s proto=%s cmp=%s%s" % (
                        size, pattern, pk, ck, " [ticket-x6 accessor]" if tag else "")
                    code = tpl % (size, pattern, pk, ck, CMP_BODY[ck])
                    p = C.write_probe("x6", pid, code)
                    data = C.record_node(p)
                    C.bake(p, data)
                    rows.append((pid, dims, tag))

    # frozen controls
    for size in (3, 5):
        for pattern in ("middle", "all"):
            for pk in ("none", "indexed"):
                for ck in ("reverse", "readproto"):
                    pid = "x6fr_s%d_%s_%s_%s" % (size, pattern, pk, ck)
                    dims = "frozen size=%d holes=%s proto=%s cmp=%s" % (size, pattern, pk, ck)
                    code = FROZEN_PROBE % (size, pattern, pk, ck, CMP_BODY[ck])
                    p = C.write_probe("x6", pid, code)
                    data = C.record_node(p)
                    C.bake(p, data)
                    rows.append((pid, dims, ""))
    C.manifest("x6", rows)
    C.log("x6 total probes: %d" % len(rows))

if __name__ == "__main__":
    main()
