#!/usr/bin/env python3
# gen_dtoa.py — black-box double->string matrix generator.
# Matrices:
#  D1 powers+ulp-neighbors: k in [-1074,1023], variants {+2^k, -2^k, -1ulp, +1ulp, -2^k-1ulp, -2^k+1ulp}
#  D2 formats: toFixed (18 digit counts incl. 0..100), toPrecision(1..21), toExponential(0..20),
#              toString(2/8/16/36) on 200 sampled bit rows
#  D3 random: 100,000 seeded LCG bit-pattern doubles, String() vs node-baked expectations
#  D4 integer fast-path / specials / -0 / valueOf contexts (dtoa_specials.js, hand-written)
# Expectations computed by node (oracle) at generation time; probes bake
# (hi32, lo32, kind, arg, expected) rows and assert engine output === expected,
# plus parseFloat round-trip exactness on decimal rows.
import json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECT = os.path.join(HERE, "expect_dtoa.js")
EXPFILE = os.path.join(HERE, "exp_dtoa.jsonl")
PDIR = os.path.join(HERE, "probes", "dtoa")
ROWSF = os.path.join(HERE, "rows_dtoa.jsonl")

PRELUDE = """
var __dv = new DataView(new ArrayBuffer(8));
function mk(hi, lo) { __dv.setUint32(0, lo >>> 0, true); __dv.setUint32(4, hi >>> 0, true); return __dv.getFloat64(0, true); }
"""

SPECIALS = [
    ("zero", 0.0), ("negzero", -0.0), ("nan", None), ("inf", "p"), ("ninf", "n"),
    ("minsub", 5e-324), ("minsub2", 4.9e-324),
    ("minnormal", 2.2250738585072014e-308), ("maxdbl", 1.7976931348623157e308),
    ("one_eps_lo", 1 - 2**-53), ("one_eps_hi", 1 + 2**-52),
    ("maxsafe", 9007199254740991.0), ("maxsafe_p2", 9007199254740993.0),
    ("twop53", 9007199254740992.0), ("e21", 1e21), ("e20", 1e20),
    ("e21_minus", 999999999999999999999.0), ("tenth3", 0.1 + 0.2),
    ("third", 1 / 3), ("two3", 2 / 3), ("e-6", 1e-6), ("e-7", 1e-7),
    ("pi", 3.141592653589793), ("eul", 2.718281828459045),
    ("n123_456", 123.456), ("e15", 1e15), ("e16", 1e16), ("e17", 1e17),
    ("half", 0.5), ("i255", 255.0), ("i1e6", 1e6), ("nege7", -1e-7),
    ("nextup1", 1.0000000000000002), ("nine99", 0.9999999999999999),
]

def bits_of(x):
    (hi, lo) = struct.unpack(">II", struct.pack(">d", x))
    return (hi, lo)

def pow_bits(k):
    return bits_of(float.fromhex("0x1p%d" % k))

def variant_bits(k, sign, delta):
    (hi, lo) = pow_bits(k)
    if delta == 1:
        if lo == 0xFFFFFFFF:
            lo = 0; hi += 1
        else:
            lo += 1
    elif delta == -1:
        if lo == 0:
            if hi != 0:
                hi -= 1; lo = 0xFFFFFFFF
            # else: below of min denorm -> +0
        else:
            lo -= 1
    if sign < 0:
        hi |= 0x80000000
    return (hi & 0xFFFFFFFF, lo & 0xFFFFFFFF)

rows = []  # (rid, hi, lo, kind, arg)

def add(rid, hi, lo, kind="str", arg=0):
    rows.append((rid, hi, lo, kind, arg))

def build_rows():
    # D1: every power, six ulp-variants
    for k in range(-1074, 1024):
        for (tag, sign, delta) in (("pos", 1, 0), ("neg", -1, 0), ("below", 1, -1),
                                    ("above", 1, 1), ("nbelow", -1, -1), ("nabove", -1, 1)):
            (hi, lo) = variant_bits(k, sign, delta)
            add("pw_%d_%s" % (k, tag), hi, lo)
    # D2 formats
    fmt_vals = []
    for (nm, x) in SPECIALS:
        if x is None:                       # NaN
            fmt_vals.append(("sp_" + nm, 0x7FF80000, 0))
        elif x == "p":
            fmt_vals.append(("sp_" + nm, 0x7FF00000, 0))
        elif x == "n":
            fmt_vals.append(("sp_" + nm, 0xFFF00000, 0))
        else:
            (hi, lo) = bits_of(x)
            fmt_vals.append(("sp_" + nm, hi, lo))
    for k in range(-1074, 1024, 61):
        (hi, lo) = pow_bits(k)
        fmt_vals.append(("pw2_%d" % k, hi, lo))
    (hi, lo) = variant_bits(1023, 1, 1)
    fmt_vals.append(("maxstep", hi, lo))
    for (nm, hi, lo) in fmt_vals:
        for d in (0, 1, 2, 3, 4, 5, 6, 7, 10, 13, 17, 20, 25, 33, 50, 72, 99, 100):
            add("fx_%s_%d" % (nm, d), hi, lo, "toFixed", d)
        for p in range(1, 22):
            add("tp_%s_%d" % (nm, p), hi, lo, "toPrecision", p)
        for d in range(0, 21):
            add("te_%s_%d" % (nm, d), hi, lo, "toExponential", d)
    # D2 radix: 200 sampled rows
    ks = list(range(-1074, 1024, 5))
    rad_rows = []
    for k in ks[::2]:
        for (tag, sign, delta) in (("pos", 1, 0), ("above", 1, 1), ("neg", -1, 0)):
            (hi, lo) = variant_bits(k, sign, delta)
            rad_rows.append(("rad_k%d_%s" % (k, tag), hi, lo))
    rad_rows = rad_rows[:200]
    for (nm, hi, lo) in rad_rows:
        for r in (2, 8, 16, 36):
            add("rb_%s_%d" % (nm, r), hi, lo, "radix", r)
    # D3 random 100k LCG doubles
    state = 0x243F6A8885A308D3
    M = (1 << 64) - 1
    A = 6364136223846793005
    C = 1442695040888963407
    for i in range(100000):
        state = (state * A + C) & M
        hi = (state >> 32) & 0xFFFFFFFF
        lo = state & 0xFFFFFFFF
        if i % 2 == 0:
            expf = (hi >> 20) & 0x7FF
            if expf == 0x7FF:  # clamp away NaN/Inf: keep sign+mantissa, exponent -> max finite
                hi = (hi & 0x800FFFFF) | (0x7FE00000 | (hi & 0x80000000))
        else:
            s2 = (state * A + C) & M
            e = s2 % 2047
            sign = (s2 >> 20) & 0x80000000
            hi = sign | (e << 20) | ((s2 >> 8) & 0xFFFFF)
            lo = ((s2 << 24) | (state & 0xFFFFFF)) & 0xFFFFFFFF
        add("lcg_%05d" % i, hi, lo)

def emit_expect():
    build_rows()
    with open(EXPECT, "w") as f:
        f.write("// generated by gen_dtoa.py — expectation oracle (node)\n")
        f.write("function __out2(s) { if (typeof print === 'function') print(s); else console.log(s); }\n")
        f.write(PRELUDE)
        f.write("var ROWS = [\n")
        for (rid, hi, lo, kind, arg) in rows:
            f.write("[%s,%d,%d,%s,%d],\n" % (json.dumps(rid), hi, lo, json.dumps(kind), arg))
        f.write("];\n")
        f.write("""
for (var i = 0; i < ROWS.length; i++) {
  var rid = ROWS[i][0], hi = ROWS[i][1], lo = ROWS[i][2], kind = ROWS[i][3], arg = ROWS[i][4];
  var v = mk(hi, lo);
  var out;
  if (kind === 'toFixed') out = v.toFixed(arg);
  else if (kind === 'toPrecision') out = v.toPrecision(arg);
  else if (kind === 'toExponential') out = v.toExponential(arg);
  else if (kind === 'radix') out = v.toString(arg);
  else out = String(v);
  __out2(JSON.stringify([rid, out]));
}
""")
    with open(ROWSF, "w") as f:
        for r in rows:
            f.write(json.dumps(list(r)) + "\n")
    print("rows:", len(rows))

def write_probe(tag, buf):
    path = os.path.join(PDIR, tag + ".js")
    with open(path, "w") as g:
        g.write("// GENERATED probe (gen_dtoa.py)\nvar __TAG = %s;\n" % json.dumps(tag))
        with open(os.path.join(HERE, "h.js")) as h:
            g.write(h.read())
        g.write(PRELUDE)
        g.write("var R = [\n")
        for (row, exp) in buf:
            g.write("[%d,%d,%s,%d,%s],\n" % (row[1], row[2], json.dumps(row[3]), row[4], json.dumps(exp)))
        g.write("];\n")
        g.write("""
for (var i = 0; i < R.length; i++) {
  var hi = R[i][0], lo = R[i][1], kind = R[i][2], arg = R[i][3], want = R[i][4];
  var v = mk(hi, lo);
  var got;
  if (kind === 'toFixed') got = v.toFixed(arg);
  else if (kind === 'toPrecision') got = v.toPrecision(arg);
  else if (kind === 'toExponential') got = v.toExponential(arg);
  else if (kind === 'radix') got = v.toString(arg);
  else got = String(v);
  assert_eq(got, want, __TAG + '#' + i + ' ' + kind + ' hi=' + hi + ' lo=' + lo);
  // parseFloat round-trip exactness: shortest-repr rows only (toFixed etc. are
  // lossy by design; radix rows are non-decimal and parseFloat would misparse).
  if (kind === 'str') {
    if (want === 'NaN') assert_true(isNaN(parseFloat(want)), 'rt#' + i);
    else assert_true(parseFloat(want) === v || (want === '0' && v === 0) || (want === '0' && Object.is(v, -0)), 'rt#' + i + ' want=' + want);
  }
}
summary(__TAG);
""")

def bake(expfile):
    exps = {}
    with open(expfile) as f:
        for line in f:
            if line.strip():
                (rid, val) = json.loads(line)
                exps[rid] = val
    with open(ROWSF) as f:
        allrows = [json.loads(x) for x in f if x.strip()]
    os.makedirs(PDIR, exist_ok=True)
    groups = {"pw": 300, "fx": 400, "tp": 400, "te": 400, "rb": 300, "lcg": 2000}
    counts = {g: 0 for g in groups}
    bufs = {g: [] for g in groups}
    for row in allrows:
        g = row[0].split("_", 1)[0]
        if row[0] not in exps:
            print("MISSING expectation", row[0]); sys.exit(2)
        bufs[g].append((row, exps[row[0]]))
        if len(bufs[g]) >= groups[g]:
            write_probe("d_%s_%03d" % (g, counts[g]), bufs[g]); counts[g] += 1; bufs[g] = []
    for g in groups:
        if bufs[g]:
            write_probe("d_%s_%03d" % (g, counts[g]), bufs[g]); counts[g] += 1
    print("probes:", counts)

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--bake":
        bake(sys.argv[2])
        return
    emit_expect()

if __name__ == "__main__":
    main()
