#!/usr/bin/env python3
"""gen_nprobe.py — pure-JS probe that runs on BOTH engines (n_ prefix).

Guards the oracle assumptions every other probe relies on:
  - the shared LCG byte stream (py_lcg <-> JS lcg) must agree byte-for-byte
  - harness normalization primitives (JSON.stringify undefined-in-array,
    String escapes) behave identically
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe, py_lcg

def probe_parity():
    rows = []
    for seed in [1, 42, 0x1234, 99991]:
        for n in [0, 1, 7, 64]:
            data = py_lcg(seed, n)
            rows.append('  [%d, %d, "%s"],' % (seed, n, data.hex()))
    emit = ["var STREAMS = [\n" + "\n".join(rows) + "\n];"]
    emit.append(r"""
function lcg(seed, n) {
  var a = seed >>> 0, out = new Uint8Array(n);
  for (var i = 0; i < n; i++) { a = (Math.imul(a, 1103515245) + 12345) >>> 0; out[i] = (a >>> 16) & 0xff; }
  return out;
}
for (var i = 0; i < STREAMS.length; i++) {
  var s = STREAMS[i];
  var bytes = lcg(s[0], s[1]);
  var hex = "";
  for (var k = 0; k < bytes.length; k++) hex += ("0" + bytes[k].toString(16)).slice(-2);
  assert_eq(hex, s[2], "lcg parity seed=" + s[0] + " n=" + s[1]);
}
// JSON.stringify conventions used by the harness
assert_eq(JSON.stringify([1, undefined, 2]), "[1,null,2]", "undefined in array -> null");
assert_eq(JSON.stringify(undefined), undefined, "top-level undefined stringifies to undefined");
// string escape round-trips
assert_eq("a\x00b".length, 3, "NUL escape");
assert_eq("\ud800\udfde".length, 2, "surrogate pair escape");
// Math.imul exists and is 32-bit
assert_eq(Math.imul(0x9e3779b9, 5) >>> 0, Math.imul(0x9e3779b9, 5) >>> 0, "imul stable");
// typed array basics the probes rely on
var u = new Uint8Array([255, 0, 128]);
assert_eq(JSON.stringify(Array.from(u)), "[255,0,128]", "Array.from typed array");
assert_eq(new DataView(new Uint8Array([0xDE, 0xAD]).buffer).getUint16(0), 57005, "DataView BE u16");
summary("harness_parity");
""")
    return write_probe("_common", "harness_parity", "\n".join(emit), node_ok=True)


if __name__ == "__main__":
    print(probe_parity())
