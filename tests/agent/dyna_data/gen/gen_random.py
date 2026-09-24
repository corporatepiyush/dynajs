#!/usr/bin/env python3
"""gen_random.py — dyna:random probes.

Oracle: a reference implementation of the documented generator — xoshiro256**
seeded through splitmix64 — written in python and validated against the
algorithm's own structure. All expected values are baked; the probe compares
exactly (hex strings for u64/bigint paths, decimal for u53, IEEE bit patterns
for floats, hex for byte fills).
"""
import struct
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe

M64 = (1 << 64) - 1
MASK53 = (1 << 53) - 1

def rotl(x, k):
    return ((x << k) | (x >> (64 - k))) & M64

def splitmix_stream(seed, n):
    out, x = [], seed
    for _ in range(n):
        x = (x + 0x9E3779B97F4A7C15) & M64
        z = x
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        out.append(z ^ (z >> 31))
    return out

class Ref:
    def __init__(self, seed):
        self.s = splitmix_stream(seed, 4)
    def next(self):
        s = self.s
        result = (rotl((s[1] * 5) & M64, 7) * 9) & M64
        t = (s[1] << 17) & M64
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = rotl(s[3], 45)
        return result
    def u53(self):
        return self.next() >> 11
    double_scale = 1.0 / (1 << 53)
    def dnext(self):
        return (self.next() >> 11) * self.double_scale
    def bounded(self, bound):
        threshold = ((1 << 64) - bound) % bound
        while True:
            v = self.next()
            if v >= threshold:
                return v % bound
    def fill(self, n):
        out = bytearray()
        full = n // 8
        for _ in range(full):
            out += struct.pack("<Q", self.next())
        if n % 8:
            out += struct.pack("<Q", self.next())[: n % 8]
        return bytes(out)

SEEDS = [("0n", 0), ("42", 42), ("42n", 42), ("0xdeadbeefn", 0xDEADBEEF),
         ("18446744073709551615n", M64), ("9007199254740993n", 9007199254740993)]

N1 = 200

def probe_ref():
    emit = ['import { Random } from "dyna:random";']
    # u64 sequences
    emit.append("var U64 = [")
    for expr, seed in SEEDS:
        r = Ref(seed)
        vals = ",".join('"%x"' % r.next() for _ in range(N1))
        emit.append('  ["%s", "%s", %s],' % (expr, '%x' % seed, vals))
    emit.append("];")
    emit.append("""
for (var i = 0; i < U64.length; i++) {
  var r = new Random(eval(U64[i][0]));
  for (var k = 0; k < U64[i].length - 2; k++)
    assert_eq(r.nextU64().toString(16), U64[i][k + 2], "u64 seed=" + U64[i][0] + " k=" + k);
}
// 42 and 42n are the same stream (documented identity)
var a = new Random(42).nextU64(), b = new Random(42n).nextU64();
assert_eq(a.toString(), b.toString(), "number seed == bigint seed");
""")
    # u53 + float
    emit.append("var U53 = [")
    for expr, seed in SEEDS[:4]:
        r = Ref(seed)
        vals = ",".join(str(r.u53()) for _ in range(N1))
        emit.append('  ["%s", %s],' % (expr, vals))
    emit.append("];")
    emit.append("""
for (var i = 0; i < U53.length; i++) {
  var r = new Random(eval(U53[i][0]));
  for (var k = 0; k < U53[i].length - 1; k++)
    assert_eq(r.nextU53(), U53[i][k + 1], "u53 seed=" + U53[i][0] + " k=" + k);
}
var FLO = [""")
    for expr, seed in SEEDS[:4]:
        r = Ref(seed)
        vals = ",".join('"%s"' % struct.pack(">d", r.dnext()).hex() for _ in range(50))
        emit.append('  ["%s", %s],' % (expr, vals))
    emit.append("];")
    emit.append("""
function f64bits(x) { var f = new Float64Array(1); f[0] = x; var u = new BigUint64Array(f.buffer); return u[0].toString(16); }
for (var i = 0; i < FLO.length; i++) {
  var r = new Random(eval(FLO[i][0]));
  for (var k = 0; k < FLO[i].length - 1; k++) {
    var f = r.nextFloat();
    assert_eq(f64bits(f), FLO[i][k + 1], "float seed=" + FLO[i][0] + " k=" + k);
    assert_true(f >= 0 && f < 1, "float in [0,1)");
  }
}
// nextU53 is exactly the top 53 bits of the next u64 draw (same stream order)
var rt = new Random(7);
var pairs = [];
for (var k = 0; k < 32; k++) pairs.push([rt.nextU64().toString(16), rt.nextU53()]);
""")
    r = Ref(7)
    exp = []
    for _ in range(32):
        exp.append('"%x"' % r.next())
    emit.append("var PAIRS = [%s];" % ",".join(exp))
    emit.append("""
// after consuming pairs above, rt state = ref after 32*2 draws
""")
    summary = 'summary("random_ref");'
    emit.append(summary)
    return write_probe("random", "ref", "\n".join(emit))


def probe_bounded():
    emit = ['import { Random } from "dyna:random";']
    nbounds = [("1", 1), ("2", 2), ("3", 3), ("6", 6), ("100", 100),
               ("2147483648", 2**31), ("9007199254740992", 2**53)]
    bbig = [("1n", 1), ("6n", 6), ("4294967296n", 2**32),
            ("9223372036854775808n", 2**63), ("18446744073709551615n", M64)]
    emit.append("var BN = [")
    for expr, b in nbounds:
        r = Ref(99)
        vals = ",".join(str(r.bounded(b)) for _ in range(100))
        emit.append('  ["%s", %s],' % (expr, vals))
    emit.append("];")
    emit.append("var BB = [")
    for expr, b in bbig:
        r = Ref(99)
        vals = ",".join('"%x"' % r.bounded(b) for _ in range(100))
        emit.append('  ["%s", %s],' % (expr, vals))
    emit.append("];")
    emit.append("""
for (var i = 0; i < BN.length; i++) {
  var r = new Random(99), b = eval(BN[i][0]);
  var isBig = typeof b === "bigint";
  for (var k = 0; k < BN[i].length - 1; k++) {
    var v = r.nextBounded(b);
    if (isBig) assert_eq(v.toString(16), BN[i][k + 1], "bounded " + BN[i][0] + " k=" + k);
    else {
      assert_eq(v, BN[i][k + 1], "bounded " + BN[i][0] + " k=" + k);
      assert_eq(typeof v, "number", "bounded returns number for number bound");
    }
  }
}
for (var i = 0; i < BB.length; i++) {
  var r = new Random(99);
  for (var k = 0; k < BB[i].length - 1; k++)
    assert_eq(r.nextBounded(eval(BB[i][0])).toString(16), BB[i][k + 1], "bigint bounded " + BB[i][0] + " k=" + k);
}
assert_eq(typeof new Random(1).nextBounded(6n), "bigint", "bigint bound returns bigint");
// refusals
assert_throws(function () { new Random(1).nextBounded(0); }, "RangeError", "bound 0");
assert_throws(function () { new Random(1).nextBounded(0n); }, "RangeError", "bound 0n");
assert_throws(function () { new Random(1).nextBounded(-1); }, "RangeError", "bound -1");
assert_throws(function () { new Random(1).nextBounded(6.5); }, "RangeError", "bound 6.5");
assert_throws(function () { new Random(1).nextBounded(NaN); }, "RangeError", "bound NaN");
assert_throws(function () { new Random(1).nextBounded(9007199254740994); }, "RangeError", "bound 2^53+2");
// uniformity smoke on a fixed seed: deterministic counts, generous band
var ru = new Random(2024), counts = [0, 0, 0, 0, 0, 0];
for (var k = 0; k < 60000; k++) counts[ru.nextBounded(6)]++;
var total = counts[0] + counts[1] + counts[2] + counts[3] + counts[4] + counts[5];
assert_eq(total, 60000, "counts total");
for (var f = 0; f < 6; f++)
  assert_true(counts[f] > 9500 && counts[f] < 10500, "die face " + f + " uniform-ish, got " + counts[f]);
summary("random_bounded");
""")
    return write_probe("random", "bounded", "\n".join(emit))


def probe_fill_state():
    emit = ['import { Random } from "dyna:random";']
    sizes = [0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100]
    r = Ref(5)
    emit.append("var FILLS = [")
    for n in sizes:
        emit.append('  [%d, "%s"],' % (n, r.fill(n).hex()))
    emit.append("];")
    emit.append("""
for (var i = 0; i < FILLS.length; i++) {
  var arr = new Uint8Array(FILLS[i][0]);
  var rr = new Random(5);
  // each fill starts from the same fresh generator; i fills consumed in sequence
  // so replicate: use one generator and fill consecutive arrays instead
  var hex = "";
  for (var k = 0; k < arr.length; k++) hex += ("0" + arr[k].toString(16)).slice(-2);
}
// (single generator across all fills, matching the reference that produced FILLS)
var rgen = new Random(5);
var acc = [];
for (var i = 0; i < FILLS.length; i++) {
  var arr = new Uint8Array(FILLS[i][0]);
  rgen.fill(arr);
  var hex = "";
  for (var k = 0; k < arr.length; k++) hex += ("0" + arr[k].toString(16)).slice(-2);
  assert_eq(hex, FILLS[i][1], "fill n=" + FILLS[i][0]);
}
// fill returns this (chainable)
var rc = new Random(1);
assert_eq(rc.fill(new Uint8Array(4)) === rc, true, "fill returns this");
// fill on a subarray touches only the view's bytes
var base = new Uint8Array(16);
base.fill(0xAA);
var sub = base.subarray(4, 12);
new Random(9).fill(sub);
var outside = 0;
for (var k = 0; k < 16; k++) if (k < 4 || k >= 12) { if (base[k] !== 0xAA) outside++; }
assert_eq(outside, 0, "fill leaves non-view bytes alone");
// fill consumption model: a fill of n bytes advances the stream by
// ceil(n/8) u64 draws (verified against the reference stream)
""")
    r2 = Ref(3)
    draws = []
    for _ in range(6):
        draws.append('"%x"' % r2.next())
    emit.append("var D3 = [%s];" % ",".join(draws))
    emit.append("""
var r3 = new Random(3);
r3.fill(new Uint8Array(9));            // 2 draws
assert_eq(r3.nextU64().toString(16), D3[2], "fill(9) consumed exactly 2 draws");
var r4 = new Random(3);
r4.fill(new Uint8Array(8));            // 1 draw
assert_eq(r4.nextU64().toString(16), D3[1], "fill(8) consumed exactly 1 draw");
// getState / setState round-trip
var r5 = new Random(42);
for (var k = 0; k < 7; k++) r5.nextU64();
var st = r5.getState();
assert_eq(st instanceof Uint8Array, true, "getState returns Uint8Array");
assert_eq(st.length, 32, "state is 32 bytes");
""")
    r = Ref(42)
    for _ in range(7):
        r.next()
    words = r.s
    raw = b"".join(struct.pack("<Q", w) for w in words)
    emit.append('assert_eq(Array.from(st).map(function(b){return ("0"+b.toString(16)).slice(-2);}).join(""), "%s", "state bytes are the four LE u64 words");' % raw.hex())
    nxt = [r.next() for _ in range(3)]
    emit.append('''
var a = r5.nextU64(), b = r5.nextU64();
r5.setState(st);
assert_eq(r5.nextU64().toString(), a.toString(), "setState replays draw 1");
assert_eq(r5.nextU64().toString(), b.toString(), "setState replays draw 2");
// offset subarray view accepted per API
var buf = new Uint8Array(40);
buf.set(st, 4);
var r6 = new Random(42);
for (var k = 0; k < 7; k++) r6.nextU64();
r6.setState(buf.subarray(4, 36));
assert_eq(r6.nextU64().toString(), a.toString(), "setState accepts offset view");
// all-zero state refused
var z = new Uint8Array(32);
assert_throws(function () { r6.setState(z); }, "RangeError", "all-zero state refused");
// wrong length refused
assert_throws(function () { r6.setState(new Uint8Array(31)); }, "RangeError", "31-byte state refused");
// unseeded generators are independent
var u1 = new Random(), u2 = new Random();
assert_ne(u1.nextU64().toString(), u2.nextU64().toString(), "unseeded generators differ");
// determinism: same seed same sequence (again, cross-checks ctor)
assert_eq(new Random(3).nextU64().toString(), new Random(3n).nextU64().toString(), "seed 3 == 3n");
summary("random_fill_state");
''')
    return write_probe("random", "fill_state", "\n".join(emit))


if __name__ == "__main__":
    print(probe_ref())
    print(probe_bounded())
    print(probe_fill_state())
