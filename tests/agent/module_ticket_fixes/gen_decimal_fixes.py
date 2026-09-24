#!/usr/bin/env python3
# gen_decimal_fixes.py -- targeted probes for the module-ticket decimal fixes.
#
# FIX 1 (div guard digits): repeated-digit expansions are the killer input for
# the old bug -- dec_div trims the truncated quotient's low zero digits into
# the exponent BEFORE the sticky remainder is consumed, so an UP/CEIL/FLOOR
# round-up landed on the LSD instead of the target position
# (1/99999999.999999 @ prec 9 up -> 2e-8 instead of 1.00000001e-8).
# Matrix: repeated-digit dividends/divisors x 7 rounding modes x
# precisions {2,9,34,100}, oracle = python decimal under the identical
# context (dyna's div is the correctly-rounded quotient, like libmpdec).
#
# FIX 2 (pow/round validation): non-finite or out-of-documented-range
# arguments must throw RangeError (JS_ToInt64 is modulo 2^64: Infinity -> 0,
# 1e300 -> garbage-in-range). Working boundary values are checked against
# python decimal / exact integers.
#
# Goldens are baked in at generation time; the probe is self-contained JS.
# Re-run:  python3 gen_decimal_fixes.py && bash run.sh
import os, json
import decimal as _d
from decimal import Decimal, getcontext, localcontext

HERE = os.path.dirname(os.path.abspath(__file__))
PROBES = os.path.join(HERE, "probes")

getcontext().prec = 200000

MODES = ["up", "down", "ceil", "floor", "halfUp", "halfDown", "halfEven"]
PYMODE = {
    "up": _d.ROUND_UP, "down": _d.ROUND_DOWN, "ceil": _d.ROUND_CEILING,
    "floor": _d.ROUND_FLOOR, "halfUp": _d.ROUND_HALF_UP,
    "halfDown": _d.ROUND_HALF_DOWN, "halfEven": _d.ROUND_HALF_EVEN,
}
PRECS = [2, 9, 34, 100]

# Repeated-digit expansions and near-repeats: the class that hides a nonzero
# sticky remainder behind a long run of quotient zeros (and sign variants,
# so CEIL/FLOOR see both directions).
DIVIDENDS = [
    "1", "2", "10", "0.1", "3", "7", "9", "11",
    "99999999.999999", "0.99999999", "9999999.99999999", "99.9999999",
    "111111.11111", "0.1111111", "77777.77", "0.0077777",
    "3333333.3", "0.0003333", "121212.12", "0.0121212",
    "99999999999.9999999999", "0.0000009999999", "1e-8", "555500.0055",
    "-1", "-99999999.999999", "-0.1111111", "-77777.77", "-1e-8",
]
DIVISORS = [
    "1", "2", "4", "5", "10", "3", "7", "9", "11", "13",
    "99999999.999999", "0.99999999", "999999.9999999", "99.99",
    "111111.111", "0.0111111", "333.3333", "0.0009999",
    "88888888.8", "1e-7",
    "-3", "-0.99999999", "-111111.111", "-1e-7",
]

def canond(d):
    if not d.is_finite():
        raise ValueError("non-finite")
    if d == 0:
        return Decimal(0)
    with localcontext() as c:
        c.prec = 200000
        return d.normalize()

def dyna_str(d):
    """Render a Decimal the way dyna's dec_write does: plain positional,
    at least one integer digit, no exponent, sign only when nonzero."""
    d = canond(d)
    if d == 0:
        return "0"
    sign, digits, exp = d.as_tuple()
    s = "".join(map(str, digits))
    ndig = len(s)
    point = ndig + exp
    if point <= 0:
        out = "0." + "0" * (-point) + s
    elif point < ndig:
        out = s[:point] + "." + s[point:]
    else:
        out = s + "0" * (point - ndig)
    return ("-" if sign else "") + out

HARNESS = """// self-contained micro harness
var __pass = 0, __fail = 0;
function expect(msg, got, want) {
  if (got === want) { __pass++; return; }
  __fail++;
  print("FAIL " + msg + ": got " + JSON.stringify(got) + " want " + JSON.stringify(want));
}
function expect_throws(msg, cls, fn) {
  try { fn(); } catch (e) {
    var k = (e && e.constructor && e.constructor.name) || String(e);
    if (k === cls) { __pass++; } else { __fail++; print("FAIL " + msg + ": got " + k + ", want " + cls); }
    return;
  }
  __fail++;
  print("FAIL " + msg + ": no throw, want " + cls);
}
function summary(tag, total) {
  print("SUMMARY " + tag + " pass=" + __pass + " fail=" + __fail + " // " + total + " cases");
  print(__fail === 0 ? "RESULT PASS" : "RESULT FAIL");
  if (__fail !== 0) throw new Error("probe failed");
}
"""

def gen_div_matrix():
    cases = []  # (a, b, prec, mode, expected)
    for a in DIVIDENDS:
        for b in DIVISORS:
            da, dbv = Decimal(a), Decimal(b)
            for prec in PRECS:
                for mode in MODES:
                    with localcontext() as c:
                        c.prec = prec
                        c.rounding = PYMODE[mode]
                        q = da / dbv
                    cases.append((a, b, prec, mode, dyna_str(q)))
    hdr = []
    hdr.append('import { Decimal } from "dyna:decimal";')
    hdr.append(HARNESS)
    hdr.append('var __TAG = "decimal_fix1_div_matrix";')
    hdr.append('var M = ' + json.dumps([list(c) for c in cases]) + ';')
    hdr.append("""
for (var i = 0; i < M.length; i++) {
  var c = M[i];
  var got;
  try { got = new Decimal(c[0]).div(c[1], { precision: c[2], rounding: c[3] }).toString(); }
  catch (e) { got = "THROW:" + ((e && e.constructor && e.constructor.name) || e); }
  expect("div(" + c[0] + "/" + c[1] + ",p" + c[2] + "," + c[3] + ")", got, c[4]);
}
""")
    path = os.path.join(PROBES, "decimal_fix1_div_matrix.js")
    with open(path, "w") as f:
        f.write("// GENERATED probe (gen_decimal_fixes.py) -- do not edit\n")
        f.write("\n".join(hdr))
        f.write('summary(__TAG, %d);\n' % len(cases))
    print("wrote %s (%d cases)" % (path, len(cases)))

FIX2_GOOD_POW = [
    ("2", "0", "1"), ("2", "1", "2"), ("2", "10000", str(2 ** 10000)),
    ("10", "-10000", dyna_str(Decimal(1) / (Decimal(10) ** 10000))),
    ("1.5", "3", "3.375"), ("-2", "3", "-8"), ("-2", "2", "4"),
]
FIX2_BAD_POW = [
    ("2", "Infinity"), ("2", "-Infinity"), ("2", "NaN"),
    ("2", "18446744073709551616"),   # 2^64: int64 wrap -> 0, used to return 1
    ("2", "9223372036854775808"),    # 2^63: int64 wrap -> INT64_MIN
    ("2", "1e30"), ("2", "10001"), ("2", "-10001"), ("2", "1.9"),
]
FIX2_GOOD_ROUND = [  # (value, dpLiteral, options, method, expected)
    ("1.5", "2", "", "round", "1.5"),
    ("1.5", "2", "", "toFixed", "1.50"),
    ("1.5", "-1000", "", "round", "0"),
    ("1.5", "1000", "", "toFixed", "1." + "5" + "0" * 999),
    ("1.234", "2", ', { rounding: "up" }', "round", "1.24"),
]
FIX2_BAD_ROUND = ["Infinity", "-Infinity", "1e300", "1001", "-1001", "2.5"]

def gen_pow_round_validation():
    total = 0
    out = []
    out.append('import { Decimal } from "dyna:decimal";')
    out.append(HARNESS)
    out.append('var __TAG = "decimal_fix2_pow_round_validate";')
    for v, e, want in FIX2_GOOD_POW:
        out.append('expect("pow(%s,%s)", new Decimal(%s).pow(%s).toString(), %s);'
                   % (v, e, json.dumps(v), e, json.dumps(want)))
        total += 1
    for v, e in FIX2_BAD_POW:
        out.append('expect_throws("pow(%s,%s)", "RangeError", function () { new Decimal(%s).pow(%s); });'
                   % (v, e, json.dumps(v), e))
        total += 1
    for v, dp, opt, m, want in FIX2_GOOD_ROUND:
        out.append('expect("%s(%s,%s)", new Decimal(%s).%s(%s%s).toString(), %s);'
                   % (m, v, dp, json.dumps(v), m, dp, opt, json.dumps(want)))
        total += 1
    for dp in FIX2_BAD_ROUND:
        for m in ("round", "toFixed"):
            out.append('expect_throws("%s(%s)", "RangeError", function () { new Decimal("1.5").%s(%s); });'
                       % (m, dp, m, dp))
            total += 1
    path = os.path.join(PROBES, "decimal_fix2_pow_round_validate.js")
    with open(path, "w") as f:
        f.write("// GENERATED probe (gen_decimal_fixes.py) -- do not edit\n")
        f.write("\n".join(out))
        f.write('\nsummary(__TAG, %d);\n' % total)
    print("wrote %s (%d cases)" % (path, total))

if __name__ == "__main__":
    gen_div_matrix()
    gen_pow_round_validation()
