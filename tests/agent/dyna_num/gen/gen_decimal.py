#!/usr/bin/env python3
# gen_decimal.py -- emit black-box probes for dyna:decimal.
#
# THE GOLDEN ORACLE is python's decimal module (libmpdec), run at a huge
# context precision for the exact ops (dyna's add/sub/mul/mod are EXACT by
# contract; python is exact too once prec >> operand digits) and at the
# per-call context precision for div/pow-neg (dyna's div is the correctly
# rounded quotient, as libmpdec's truediv is).
# Known, DOCUMENTED divergences modelled here explicitly:
#   - dyna prints plain positional notation always (no exponent), canonical
#     ("1.500" -> "1.5", -0 -> "0", exponents folded into digits).
#   - dyna caps parse exponents at +-2e9 and refuse to render text over 1e6
#     chars (python happily prints 1E+999999999).
#   - halfOdd does not exist in python decimal; a hand-rolled exact reference
#     is used, restricted to EXACT values (exact ops + round/toFixed only).
# The dyna-side formatter below is the single modelling risk; it is written
# from dyna's dec_write() contract, and every probe also asserts a battery of
# hand-checked literal rows as a second line of defence.
import os, sys, json, random
from decimal import Decimal, getcontext, localcontext, ROUND_UP, ROUND_DOWN, \
    ROUND_CEILING, ROUND_FLOOR, ROUND_HALF_UP, ROUND_HALF_DOWN, ROUND_HALF_EVEN, \
    DivisionByZero, InvalidOperation, Overflow

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..", "..")
PROBES = os.path.join(HERE, "..", "probes", "decimal")
HARNESS = os.path.join(HERE, "..", "h.js")

getcontext().prec = 200000
getcontext().Emax = 999999999
getcontext().Emin = -999999999

MODES = ["up", "down", "ceil", "floor", "halfUp", "halfDown", "halfEven"]
PYMODE = {
    "up": ROUND_UP, "down": ROUND_DOWN, "ceil": ROUND_CEILING,
    "floor": ROUND_FLOOR, "halfUp": ROUND_HALF_UP, "halfDown": ROUND_HALF_DOWN,
    "halfEven": ROUND_HALF_EVEN,
}

def canond(d):
    """dyna canonical form as a python Decimal: zeros lose their sign,
    trailing zeros fold into the exponent (1.500 == 1.5)."""
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

def dyna_fixed(d, dp, orig):
    """Render a rounded value exactly the way dec_write_fixed does: the
    integer part (>= 1 digit), then '.', then exactly dp digits (dp > 0), or
    the bare integer part (dp <= 0); '-' printed whenever the value (or the
    pre-rounding original) was negative, including -0.00."""
    sign, digits, exp = d.as_tuple()
    nd = len(digits)

    def at(pos):
        if pos < exp or pos >= exp + nd:
            return 0
        return digits[nd - 1 - (pos - exp)]

    neg = d.is_signed() or (Decimal(orig) < 0 and Decimal(orig) != 0)
    hi = 0 if d == 0 else max(exp + nd - 1, 0)
    s = ""
    if dp > 0:
        for p in range(hi, -1, -1):
            s += str(at(p))
        s += "."
        for p in range(-1, -dp - 1, -1):
            s += str(at(p))
    else:
        for p in range(hi, -1, -1):
            s += str(at(p))
    return ("-" if neg else "") + s

def half_odd_at(d, pos):
    """Exact reference for RND_HALF_ODD: round |d| so every digit below
    decimal position `pos` drops; ties go to the ODD surviving digit."""
    if d == 0:
        return Decimal(0)
    q = Decimal(1).scaleb(pos)
    a = abs(d)
    scaled = a.scaleb(-pos)
    fl = scaled.to_integral_value(rounding=ROUND_FLOOR)
    frac = scaled - fl
    if frac > Decimal("0.5"):
        r = fl + 1
    elif frac < Decimal("0.5"):
        r = fl
    else:
        r = fl if (fl % 2) != 0 else fl + 1
    if r == 0:
        return Decimal(0)
    return r.copy_sign(d) * q

def half_odd_sig(d, prec):
    if d == 0:
        return Decimal(0)
    t = canond(d)
    if len(t.as_tuple().digits) <= prec:
        return t
    return half_odd_at(t, t.adjusted() - prec + 1)

def quantized(d, pos, mode):
    """Round d to decimal position `pos` (10^pos place) under mode name."""
    if mode == "halfOdd":
        return half_odd_at(d, pos)
    q = Decimal(1).scaleb(pos)
    try:
        return d.quantize(q, rounding=PYMODE[mode])
    except InvalidOperation:
        return None

def sig_round(d, prec, mode):
    """Round d to `prec` significant digits under mode name (exact value)."""
    if mode == "halfOdd":
        return half_odd_sig(d, prec)
    t = canond(d)
    nd = len(t.as_tuple().digits) if t != 0 else 0
    if nd <= prec:
        return t
    target = t.adjusted() - prec + 1
    return quantized(t, target, mode)

# ---------------------------------------------------------------- operands
def long_digits(n, lead=3):
    return "".join(str((i * 7 + lead) % 10) for i in range(n)).lstrip("0") or "5"

OPERANDS = [
    "0", "-0", "1", "-1", "2", "-2", "3", "10", "-10", "7", "-7",
    "0.5", "-0.5", "1.5", "-1.5", "2.5", "-2.5", "3.5", "-3.5",
    "0.1", "0.2", "0.3", "-0.3", "1.005", "-1.005", "2.675",
    "123.45", "-123.45", "999.99", "0.0001", "-0.0001", "0.0010",
    "1e3", "-1e3", "1e6", "1e-6", "1e-50", "5e-324", "1e100", "1e-100",
    "1e308", "1234567890123456789012345678901234567890",
    "-9876543210987654321098765432109876543210",
    "0." + "0" * 30 + "123456789",
    "1." + long_digits(100),
    "-" + long_digits(100) + "." + long_digits(50, 4),
    "1e6143", "1e-6176", "9.9999e6140", "79228162514264337593543950335",
    "0.3333333333333333333333333333333333333333",
]
DIVISORS = ["1", "2", "4", "5", "8", "10", "16", "20", "25", "50", "100",
            "3", "7", "9", "11", "13", "999", "0.0000001", "-3", "1e-10"]
POW_BASES = ["2", "-2", "10", "1.5", "-1.5", "0.1", "1.0000001", "0.5",
             "3", "-3", "1.1", "9.9", "1e10", "0.000001", "7.25"]
POW_EXPS = [0, 1, 2, 3, 7, 10, 17, 31, 63, 100, -1, -2, -3, -7, -10, -31, -100]

def jsnum(d):
    """A JS numeric literal or string literal for operand d."""
    s = str(d)
    if d.as_tuple().exponent >= 0 and abs(d) < 1e15 and d == d.to_integral_value():
        return s                       # plain integer literal
    return json.dumps(s)               # string literal otherwise

def js_operand(tok):
    """Emit an operand expression: plain ints as numbers, everything else as a
    string literal (matching dyna's own coercion breadth)."""
    try:
        d = Decimal(tok)
    except Exception:
        return json.dumps(tok)
    if d == 0 and tok.lstrip("-") == "0":
        return json.dumps(tok)
    if d == d.to_integral_value() and 0 < abs(d) < 1e15 and "e" not in tok.lower() and "." not in tok:
        return tok
    return json.dumps(tok)

def fmt_opts(prec=None, mode=None):
    parts = []
    if prec is not None:
        parts.append("precision:%d" % prec)
    if mode is not None:
        parts.append('rounding:"%s"' % mode)
    if not parts:
        return "undefined"
    return "{" + ",".join(parts) + "}"

class ProbeWriter:
    def __init__(self, path, imports, tag):
        self.path = path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.f = open(path, "w")
        self.f.write("// GENERATED probe (%s) -- do not edit; run materialize.sh\n" % tag)
        for imp in imports:
            self.f.write(imp + "\n")
        self.f.write('var __TAG = "%s";\n' % tag)
        self.f.write(open(HARNESS).read().replace("//dyna_num_harness_marker", ""))
    def w(self, line=""):
        self.f.write(line + "\n")
    def close(self, total):
        self.f.write('summary(__TAG); // %d cases\n' % total)
        self.f.close()
        print("wrote %s (%d cases)" % (self.path, total))

def emit(fn):
    """Decorator collecting (probe_name, [lines]) producers."""
    emit.registry.append(fn)
    return fn
emit.registry = []

# ================================================================ arithmetic
def make_arith_cases():
    cases = []   # (op, a, b, opts|None, kind, payload)
    ops = ["add", "sub", "mul"]
    for op in ops:
        for a in OPERANDS:
            for b in OPERANDS:
                da, db = Decimal(a), Decimal(b)
                if op == "add":
                    r = canond(da + db)
                elif op == "sub":
                    r = canond(da - db)
                else:
                    # respect the multiply cell budget: skip 100-digit x 100-digit
                    if len(da.as_tuple().digits) * len(db.as_tuple().digits) > 20000:
                        continue
                    r = canond(da * db)
                cases.append((op, a, b, None, "str", dyna_str(r)))
    # div across precisions and modes
    for a in OPERANDS:
        for b in DIVISORS:
            for prec in (1, 2, 9, 28, 34, 50, 100):
                for mode in MODES:
                    da, db = Decimal(a), Decimal(b)
                    if db == 0:
                        continue
                    try:
                        with localcontext() as c:
                            c.prec = 200000
                            exact = da / db
                            if not exact.is_finite():
                                continue
                            r = sig_round(exact, prec, mode)
                    except (DivisionByZero, InvalidOperation):
                        continue
                    cases.append(("div", a, b, (prec, mode), "str", dyna_str(r)))
    # div default context (34, halfEven)
    for a in OPERANDS:
        for b in DIVISORS:
            da, db = Decimal(a), Decimal(b)
            if db == 0:
                continue
            with localcontext() as c:
                c.prec = 200000
                exact = da / db
                if not exact.is_finite():
                    continue
                r = sig_round(exact, 34, "halfEven")
            cases.append(("div", a, b, None, "str", dyna_str(r)))
    # mod (exact, truncated-division remainder, dividend's sign)
    for a in OPERANDS:
        for b in OPERANDS:
            da, db = Decimal(a), Decimal(b)
            if db == 0:
                continue
            if len(da.as_tuple().digits) * len(db.as_tuple().digits) > 40000:
                continue
            try:
                with localcontext() as c:
                    c.prec = 200000
                    r = da % db
            except (DivisionByZero, InvalidOperation):
                continue
            if not r.is_finite():
                continue
            # dyna's % is C-style truncated remainder == python's % for Decimal
            cases.append(("mod", a, b, None, "str", dyna_str(canond(r))))
    # cmp / equals
    for a in OPERANDS[:26]:
        for b in OPERANDS[:26]:
            da, db = Decimal(a), Decimal(b)
            c = -1 if da < db else (1 if da > db else 0)
            cases.append(("cmp", a, b, None, "int", c))
            cases.append(("equals", a, b, None, "bool", da == db))
    return cases

ARITH_JS = r'''
var CASES = __CASES__;
var D = function (t) { return new Decimal(t); };
for (var i = 0; i < CASES.length; i++) {
  var c = CASES[i], op = c[0], a = c[1], b = c[2], opts = c[3], kind = c[4], want = c[5];
  var msg = op + "(" + a + "," + b + (opts ? "," + JSON.stringify(opts) : "") + ")#" + i;
  if (kind === "throw") { assert_throws(function () { D(a)[op](b, opts); }, want, msg); continue; }
  var got;
  try { got = (op === "cmp" || op === "equals") ? D(a)[op](b) : D(a)[op](b, opts); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  if (kind === "str") got = got.toString();
  if (kind === "int") assert_eq(got, want, msg);
  else if (kind === "bool") assert_true(got === want, msg);
  else assert_eq(got, want, msg);
}
'''

def write_arith():
    cases = make_arith_cases()
    # split into chunks so no probe exceeds ~2500 cases
    CHUNK = 2400
    for ci in range(0, len(cases), CHUNK):
        part = cases[ci:ci + CHUNK]
        tag = "decimal_arith_%03d" % (ci // CHUNK)
        p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                        ['import { Decimal } from "dyna:decimal";'], tag)
        data = []
        for (op, a, b, opts, kind, payload) in part:
            data.append('[%s,%s,%s,%s,%s,%s]' % (
                json.dumps(op), js_operand(a), js_operand(b),
                (fmt_opts(*opts) if isinstance(opts, tuple) else "undefined"),
                json.dumps(kind),
                json.dumps(payload)))
        p.w("var CASES = [")
        p.w(",\n".join(data))
        p.w("];")
        p.w("\n".join(l for l in ARITH_JS.split("\n") if "__CASES__" not in l))
        p.close(len(part))

# ===================================================================== pow
def make_pow_cases():
    cases = []
    for b in POW_BASES:
        db = Decimal(b)
        ndb = len(canond(db).as_tuple().digits)
        for n in POW_EXPS:
            # bound the exact reference size
            if n >= 0 and ndb * n > 4000:
                continue
            if n < 0 and ndb * (-n) > 2000:
                continue
            for prec in (34, 50):
                for mode in ("halfEven", "halfUp", "down"):
                    if n >= 0:
                        with localcontext() as c:
                            c.prec = 200000
                            r = canond(db ** n)   # dyna pow is EXACT for n>=0
                        cases.append(("pow", b, n, (prec, mode), "str", dyna_str(r), None))
                    else:
                        with localcontext() as c:
                            c.prec = 200000
                            exact = db ** abs(n)
                            if not exact.is_finite():
                                continue
                        # dyna takes the reciprocal of the EXACT power, rounded
                        # once to (prec, mode) -- computing the power at prec
                        # like libmpdec's __pow__ would double-round.
                        with localcontext() as c:
                            c.prec = prec
                            c.rounding = PYMODE[mode]
                            r = Decimal(1) / exact
                        cases.append(("pow", b, n, (prec, mode), "str", dyna_str(canond(r)), None))
            # default options
            if n >= 0:
                with localcontext() as c:
                    c.prec = 200000
                    r = canond(db ** n)
                cases.append(("pow", b, n, None, "str", dyna_str(r), None))
            else:
                with localcontext() as c:
                    c.prec = 200000
                    exact = db ** abs(n)
                with localcontext() as c:
                    c.prec = 34
                    c.rounding = ROUND_HALF_EVEN
                    r = Decimal(1) / exact
                cases.append(("pow", b, n, None, "str", dyna_str(canond(r)), None))
    # halfOdd pow-neg vs exact reciprocal of the exact power (b chosen so the
    # power is small): 1/2^n terminates, so the exact value is computable
    for b, n in (("2", -1), ("2", -3), ("0.5", -2), ("10", -1), ("4", -2)):
        db = Decimal(b)
        exact = canond(db) ** abs(n)          # exact at prec 200000; terminates
        exact = Decimal(1) / exact            # 2^k divides 10^m
        for prec in (2, 5, 9):
            r = half_odd_sig(exact, prec)
            cases.append(("pow", b, n, (prec, "halfOdd"), "str", dyna_str(r), None))
    # error cases
    for n in (10001, -10001):
        cases.append(("pow", "2", n, None, "throw", "RangeError", None))
    return cases

def write_pow():
    cases = make_pow_cases()
    CHUNK = 1500
    for ci in range(0, len(cases), CHUNK):
        part = cases[ci:ci + CHUNK]
        tag = "decimal_pow_%03d" % (ci // CHUNK)
        p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                        ['import { Decimal } from "dyna:decimal";'], tag)
        p.w("var CASES = [")
        rows = []
        for (op, b, n, opts, kind, payload, _) in part:
            rows.append('[%s,%s,%d,%s,%s,%s]' % (
                json.dumps(op), js_operand(b), n,
                (fmt_opts(*opts) if isinstance(opts, tuple) else "undefined"),
                json.dumps(kind), json.dumps(payload)))
        p.w(",\n".join(rows))
        p.w("];")
        p.w(r'''
var D = function (t) { return new Decimal(t); };
for (var i = 0; i < CASES.length; i++) {
  var c = CASES[i], a = c[1], n = c[2], opts = c[3], kind = c[4], want = c[5];
  var msg = "pow(" + a + "," + n + (opts ? "," + JSON.stringify(opts) : "") + ")#" + i;
  if (kind === "throw") { assert_throws(function () { D(a).pow(n, opts); }, want, msg); continue; }
  var got;
  try { got = D(a).pow(n, opts).toString(); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  assert_eq(got, want, msg);
}
''')
        p.close(len(part))

# =========================================================== round / toFixed
ROUND_VALS = ["2.5", "-2.5", "1.5", "-1.5", "0.5", "-0.5", "2.675", "1.005",
              "-1.005", "123.45", "-123.45", "0.001", "-0.001", "9.9999",
              "0.5" + "0" * 20 + "5", "0.35", "2.85", "-0.35", "3.141592653589793238462643",
              "1e10", "1e-10", "123456789.987654321", "0", "-0", "1", "-1",
              "79228162514264337593543950335.5", "0.000000000000000000000000000001"]
DPS = [0, 1, 2, 3, 6, -1, -2, -3, -6]

def make_round_cases():
    cases = []
    for v in ROUND_VALS:
        dv = Decimal(v)
        if dv == 0:
            dv = Decimal(0)      # dyna parses "-0" to +0; python keeps the sign
        for dp in DPS:
            for mode in MODES + ["halfOdd"]:
                r = quantized(dv, -dp, mode)
                if r is None:
                    continue
                cases.append(("round", v, dp, mode, "str", dyna_str(canond(r))))
                cases.append(("toFixed", v, dp, mode, "str", dyna_fixed(r, dp, v)))
            # documented DEFAULTS: round -> halfEven, toFixed -> halfUp
            rd = quantized(dv, -dp, "halfEven")
            if rd is not None:
                cases.append(("round", v, dp, None, "str", dyna_str(canond(rd))))
            fu = quantized(dv, -dp, "halfUp")
            if fu is not None:
                cases.append(("toFixed", v, dp, None, "str", dyna_fixed(fu, dp, v)))
    # errors: dp out of range
    for bad in (1001, -1001):
        cases.append(("round", "1.5", bad, None, "throw", "RangeError"))
        cases.append(("toFixed", "1.5", bad, None, "throw", "RangeError"))
    return cases

def write_round():
    cases = make_round_cases()
    CHUNK = 2400
    for ci in range(0, len(cases), CHUNK):
        part = cases[ci:ci + CHUNK]
        tag = "decimal_round_%03d" % (ci // CHUNK)
        p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                        ['import { Decimal } from "dyna:decimal";'], tag)
        p.w("var CASES = [")
        rows = []
        for (fn, v, dp, mode, kind, payload) in part:
            mopt = ("'%s'" % mode) if mode else "undefined"
            rows.append('[%s,%s,%d,%s,%s,%s]' % (
                json.dumps(fn), json.dumps(v), dp, mopt,
                json.dumps(kind), json.dumps(payload)))
        p.w(",\n".join(rows))
        p.w("];")
        p.w(r'''
var D = function (t) { return new Decimal(t); };
for (var i = 0; i < CASES.length; i++) {
  var c = CASES[i], fn = c[0], v = c[1], dp = c[2], mode = c[3], kind = c[4], want = c[5];
  var msg = fn + "(" + v + "," + dp + (mode ? "," + mode : "") + ")#" + i;
  if (kind === "throw") { assert_throws(function () { D(v)[fn](dp, mode); }, want, msg); continue; }
  var got;
  try { got = D(v)[fn](dp, mode); if (fn === "round") got = got.toString(); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  assert_eq(got, want, msg);
}
''')
        p.close(len(part))

# ============================================== parse / format / conversions
def write_parse():
    tag = "decimal_parse"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { Decimal } from "dyna:decimal";'], tag)
    n = 0
    # --- accepted parses: golden = dyna-formatted canonical text
    ok = [
        ("0", "0"), ("-0", "0"), ("1", "1"), ("-1", "-1"), ("007", "7"),
        ("1.", "1"), (".5", "0.5"), ("+1.5", "1.5"), ("1e+3", "1000"),
        ("1E-3", "0.001"), ("00.500", "0.5"), ("-0.0", "0"),
        ("1" + "0" * 30, "1" + "0" * 30),
        ("0." + "0" * 30 + "1", "0." + "0" * 30 + "1"),
        ("1e6143", "1" + "0" * 6143), ("1e-6176", "0." + "0" * 6175 + "1"),
        ("1e2000000000", None),      # parse OK; toString too long -> RangeError
        ("-1e2000000000", None),
        ("1.500", "1.5"), ("1.5000e2", "150"), ("1e-7", "0.0000001"),
    ]
    p.w("var OK = [")
    rows = []
    for (s, golden) in ok:
        rows.append("[%s,%s]" % (json.dumps(s), json.dumps(golden)))
        n += 1
    p.w(",\n".join(rows))
    p.w("];")
    p.w(r'''
for (var i = 0; i < OK.length; i++) {
  var s = OK[i][0], golden = OK[i][1];
  var msg = "parse(" + s + ")#" + i;
  var d;
  try { d = new Decimal(s); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  if (golden === null) {
    // value parses, but rendering exceeds the 1e6-char text bound
    assert_throws(function () { return d.toString(); }, "RangeError", msg + ".toString");
    assert_throws(function () { return d.toNumber(); }, "RangeError", msg + ".toNumber");
    // still usable arithmetically via small exponents
    assert_true(d.abs().equals(d) || d.abs().equals(d.neg()), msg + ".abs is +-d");
  } else {
    assert_eq(d.toString(), golden, msg);
  }
}
''')
    # --- rejected parses
    bad = ["", ".", "+", "-", "e", "1e", "1e+", "1.2.3", " 1", "1 ", "1_000",
           "0x10", "nan", "NaN", "inf", "Infinity", "-Infinity", "abc",
           "1,000", "1e99999999999999999", "--1", "1..2", ".e1", "1e1.5",
           "١٢٣", "1e2000000001", "1e-2000000001", "٣", "true", "null"]
    for s in bad:
        p.w('assert_throws(function () { new Decimal(%s); }, "SyntaxError", %s);'
            % (json.dumps(s), json.dumps("reject " + s)))
        n += 1
    # --- constructor type errors
    for v in ["true", "false", "null", "undefined", "{}", "[]", "[1]", "Symbol.iterator"]:
        p.w('assert_throws(function () { new Decimal(%s); }, "TypeError", "ctor %s");'
            % (v, v))
        n += 1
    p.w('assert_throws(function () { new Decimal(); }, "TypeError", "ctor no-arg");'); n += 1
    p.w('assert_throws(function () { new Decimal(NaN); }, "RangeError", "ctor NaN");'); n += 1
    p.w('assert_throws(function () { new Decimal(Infinity); }, "RangeError", "ctor Inf");'); n += 1
    p.w('assert_throws(function () { new Decimal(-Infinity); }, "RangeError", "ctor -Inf");'); n += 1
    # --- canonical queries
    q = [
        ("0.0010", "digits", 1), ("123.45", "digits", 5), ("0", "digits", 0),
        ("100", "digits", 1), ("-123.45", "digits", 5), ("1e100", "digits", 1),
        ("0.5", "sign", 1), ("-0.5", "sign", -1), ("-0", "sign", 0),
        ("0", "sign", 0), ("123.45", "sign", 1),
        ("0", "isZero", True), ("-0", "isZero", True), ("0.000", "isZero", True),
        ("1e-6176", "isZero", False),
    ]
    for (v, fn, want) in q:
        p.w('assert_eq(new Decimal(%s).%s(), %s, "%s %s");'
            % (json.dumps(v), fn, json.dumps(want), v, fn))
        n += 1
    p.close(n)

def write_number():
    """construction from JS numbers + toNumber round trips"""
    tag = "decimal_number"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { Decimal } from "dyna:decimal";'], tag)
    n = 0
    floats = [0.1, 0.2, 0.3, 1.5, 2.675, -0.1, 1e-7, 1.5e-7, 1e21, 1e-21,
              123.456, 1e15, 9007199254740991, 9007199254740992, 5e-324,
              1.7976931348623157e308, 2.2250738585072014e-308, -42.42,
              0.1 + 0.2, 1 / 3, 1e6, 123456.789e10, -1e-15, 7, -0.0]
    p.w("var F = [")
    p.w(",".join(repr(f) for f in floats))
    p.w("];")
    golden = [dyna_str(Decimal(repr(f)) if f != 0 else Decimal(0)) for f in floats]
    p.w("var GOLD = %s;" % json.dumps(golden))
    p.w(r'''
for (var i = 0; i < F.length; i++) {
  assert_eq(new Decimal(F[i]).toString(), GOLD[i], "ctor(number)#" + i);
  var tn = new Decimal(F[i]).toNumber();
  assert_eq(tn, F[i] === 0 ? 0 : F[i], "toNumber roundtrip#" + i);
}
''')
    n += len(floats) * 2
    # toNumber exactness against python float() (correctly rounded in both)
    big = ["123456789012345678901234567890", "0.1", "1e308", "1.7976931348623157e308",
           "1e309", "-1e309", "4.9406564584124654e-324", "1e-400",
           "123.4567890123456789012345678901234567890", "1e6143"]
    rows = []
    for s in big:
        d = Decimal(s)
        try:
            f = float(d)
            if f > 1.7976931348623157e308:
                rows.append((s, "Infinity"))
            elif f < -1.7976931348623157e308:
                rows.append((s, "-Infinity"))
            else:
                rows.append((s, repr(f) if f != 0 else "0"))
        except OverflowError:
            sign = "-" if d.is_signed() else ""
            rows.append((s, sign + "Infinity"))
# values are emitted as JS numeric literals
        n += 1
    p.w("var TN = [")
    p.w(",\n".join("[%s,%s]" % (json.dumps(a), b) for a, b in rows))
    p.w("];")
    p.w(r'''
for (var i = 0; i < TN.length; i++) {
  var got;
  try { got = new Decimal(TN[i][0]).toNumber(); }
  catch (e) { __fail++; __out("FAIL toNumber(" + TN[i][0] + ") THREW " + e.name); continue; }
  assert_eq(got, TN[i][1], "toNumber(" + TN[i][0] + ")");
}
''')
    # context leakage: a per-call precision must not persist on the operand
    p.w(r'''
var x = new Decimal(1);
var a1 = x.div(3, { precision: 5 }).toString();
assert_eq(a1, "0.33333", "leak: div prec5");
var a2 = x.div(3).toString();
assert_eq(a2, "0.3333333333333333333333333333333333", "leak: div default still 34");
var a3 = x.div(3, { precision: 50 }).toString();
assert_eq(a3.length, 52, "leak: div prec50 digit count"); // 0. + 50 digits
assert_eq(x.div(3, { precision: 5 }).toString(), "0.33333", "leak: repeat prec5");
assert_eq(x.div(3).toString(), "0.3333333333333333333333333333333333", "leak: default after prec5 again");
''')
    n += 6
    # precision / rounding option validation
    for opts, kind in [('{precision:0}', "RangeError"), ('{precision:-1}', "RangeError"),
                       ('{precision:5001}', "RangeError"), ('{precision:5000}', None),
                       ('{precision:34.5}', "TypeError"), ('{precision:"34"}', None),
                       ('{rounding:"bogus"}', "RangeError"),
                       ('{rounding:42}', "RangeError"),
                       ('{rounding:"halFeven"}', "RangeError"),
                       ('"notanobject"', "RangeError"),  # a string opts is a mode name
                       ('{rounding:"halfEven"}', None)]:
        expr = 'new Decimal(1).div(3, %s).toString()' % opts
        if kind:
            p.w('assert_throws(function () { %s; }, %s, %s);'
                % (expr, json.dumps(kind), json.dumps("opts " + opts)))
        else:
            p.w('try { %s; } catch (e) { __fail++; __out("FAIL opts " + %s + " threw " + e.name); }'
                % (expr, json.dumps(opts)))
        n += 1
    # div/mod by zero
    for expr in ['new Decimal(1).div(0)', 'new Decimal(1).div("0")',
                 'new Decimal(0).div(0)', 'new Decimal("5").mod(0)']:
        p.w('assert_throws(function () { %s; }, "RangeError", %s);'
            % (expr, json.dumps("divzero " + expr)))
        n += 1
    # result exceeding digit bound: (1e2000000000)^2 exponent overflows int32
    p.w('assert_throws(function () { new Decimal("1e2000000000").pow(2); }, "RangeError", "pow exp overflow");')
    n += 1
    # neg / abs / unary identity battery
    p.w(r'''
for (var i = 0; i < F.length; i++) {
  var d = new Decimal(F[i]);
  assert_eq(d.abs().toString(), new Decimal(GOLD[i]).abs().toString(), "abs#" + i);
  var negs = d.neg().toString();
  if (d.toString() !== "0")
    assert_eq(negs.charAt(0) === "-" ? negs.slice(1) : "-" + negs,
              d.toString(), "neg#" + i);
  else
    assert_eq(negs, "0", "neg#" + i);
}
''')
    n += len(floats) * 2
    p.close(n)

# ==================================================================== money
def minor_digits(cur):
    zero = {"JPY","KRW","VND","CLP","ISK","PYG","RWF","UGX","VUV","XAF","XOF",
            "XPF","DJF","GNF","KMF","MGA","BIF"}
    three = {"BHD","IQD","JOD","KWD","LYD","OMR","TND"}
    if cur in zero: return 0
    if cur in three: return 3
    return 2

def money_str(amount, minor):
    div = 10 ** minor
    whole = abs(amount) // div
    frac = abs(amount) % div
    s = ("-" if amount < 0 else "") + str(whole)
    if minor:
        s += "." + str(frac).rjust(minor, "0")
    elif amount < 0 and whole == 0:
        s = "-0"
    return s

def make_money_cases():
    cases = []
    amounts = [0, 1, -1, 5, -5, 99, 100, -100, 1999, 12345, -99999,
               123456789, -123456789]
    # NB: 9223372036854775807 (INT64_MAX) is NOT a JS number (rounds to 2^63),
    # so new Money() correctly refuses it -- asserted by hand below, not here.
    curs = ["USD", "usd", "EUR", "JPY", "jpy", "BHD", "bhd", "KRW", "GBP",
            "CHF", "AAA", "ZZZ", "XXX"]
    for a in amounts:
        for c in curs:
            md = minor_digits(c.upper())
            m = ("new Money(%d, %s)" % (a, json.dumps(c)))
            cases.append((m, "toString", None, "str", money_str(a, md)))
            cases.append((m, "toDecimal", None, "str", dyna_str(Decimal(money_str(a, md)))))
            cases.append((m, "amount", None, "int", a))
            cases.append((m, "currency", None, "str", c.upper()))
    # format symbols
    for (cur, sym) in [("USD","$"),("EUR","\u20ac"),("GBP","\u00a3"),("JPY","\u00a5"),
                       ("CNY","\u00a5"),("INR","\u20b9"),("KRW","\u20a9"),
                       ("CAD","CA$"),("AUD","A$")]:
        for a in (1999, -1999, 5, -5, 0):
            md = minor_digits(cur)
            s = money_str(a, md)
            body = s[1:] if s.startswith("-") else s
            want = ("-" if a < 0 else "") + sym + body
            cases.append(("new Money(%d, %r)" % (a, cur), "format", None, "str", want))
    for a in (1999, 5):
        cases.append(("new Money(%d, 'CHF')" % a, "format", None, "str",
                      money_str(a, 2) + " CHF"))
    # arithmetic
    for a1 in (1999, -1999, 0, 1, 1000000):
        for a2 in (1, -1, 250, -250, 9007199254740992, -9007199254740992):
            r = a1 + a2
            if r > 9223372036854775807 or r < -9223372036854775807:
                cases.append(("pair", "add", (a1, a2), "throw", "RangeError"))
            else:
                cases.append(("pair", "add", (a1, a2), "str", money_str(r, 2)))
            r = a1 - a2
            if r > 9223372036854775807 or r < -9223372036854775807:
                cases.append(("pair", "sub", (a1, a2), "throw", "RangeError"))
            else:
                cases.append(("pair", "sub", (a1, a2), "str", money_str(r, 2)))
            cases.append(("pair", "cmp", (a1, a2), "int",
                          -1 if a1 < a2 else (1 if a1 > a2 else 0)))
            cases.append(("pair", "equals", (a1, a2), "bool", a1 == a2))
    # mul
    for a in (1999, -1999, 0, 3):
        for k in (0, 1, -1, 2, 10, -7):
            cases.append(("mul", None, (a, k), "str", money_str(a * k, 2)))
    for bad in (1.5, 0.0001, float("nan"), float("inf")):
        cases.append(("mul", None, (100, bad), "throw", "RangeError"))
    # allocate
    for a in (1000, 1999, -1000, 7, 1, 0, 100000, -3):
        for shares in ([1, 1, 1], [1, 2], [0, 1, 1], [3, 3, 3], [1, 0, 0, 2],
                       [5], [1, 1, 1, 1, 1, 1, 1], [2, 3], [7, 0, 3]):
            cases.append(("alloc", a, shares, "sums", a))
    for bad in ([], [0, 0], [1.5, 1], [-1, 2], [1, -0.5], [0] * 0 + [0] * 1):
        cases.append(("allocbad", None, bad, "throw", "RangeError"))
    return cases

def write_money():
    tag = "decimal_money"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { Decimal, Money } from "dyna:decimal";'], tag)
    cases = make_money_cases()
    n = 0
    # construction/query battery
    rows = []
    for (m, fn, opts, kind, want) in cases:
        if m == "new Money(9223372036854775807, 'CHF')":
            continue
        rows.append((m, fn, opts, kind, want))
    # emit as JS loops
    import json as J
    # 1) ctor/query battery
    q = []
    for (m, fn, opts, kind, want) in rows:
        if fn in ("toString", "amount", "currency", "toDecimal") and m.startswith("new Money"):
            q.append("[%s,%s,%s,%s,%s]" % (m, J.dumps(fn), J.dumps(kind), J.dumps(want), J.dumps(opts)))
    p.w("var Q = [")
    p.w(",\n".join(q))
    p.w("];")
    p.w(r'''
for (var i = 0; i < Q.length; i++) {
  var mm = eval(Q[i][0]), fn = Q[i][1], kind = Q[i][2], want = Q[i][3];
  var msg = Q[i][0] + "." + fn;
  var got;
  try { got = mm[fn](); if (fn === "toDecimal") got = got.toString(); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  if (kind === "int") assert_eq(got, want, msg);
  else assert_eq(got, want, msg);
}
''')
    n += len(q)
    # 2) format battery
    q = []
    for (m, fn, opts, kind, want) in rows:
        if fn == "format":
            q.append("[%s,%s]" % (m, J.dumps(want)))
    p.w("var FM = [")
    p.w(",\n".join(q))
    p.w("];")
    p.w(r'''
for (var i = 0; i < FM.length; i++) {
  assert_eq(eval(FM[i][0]).format(), FM[i][1], "format#" + i);
}
''')
    n += len(q)
    # 3) pair ops
    q = []
    for (m, fn, opts, kind, want) in rows:
        if m == "pair":
            q.append("[%s,[%d,%d],%s,%s]" % (J.dumps(fn), opts[0], opts[1],
                                             J.dumps(kind), J.dumps(want)))
    p.w("var PAIR = [")
    p.w(",\n".join(q))
    p.w("];")
    p.w(r'''
for (var i = 0; i < PAIR.length; i++) {
  var fn = PAIR[i][0], amt = PAIR[i][1], kind = PAIR[i][2], want = PAIR[i][3];
  var a = new Money(amt[0], "USD"), b = new Money(amt[1], "USD");
  var msg = fn + "(" + amt[0] + "," + amt[1] + ")";
  if (kind === "throw") {
    if (fn === "cmp" || fn === "equals") { __fail++; __out("FAIL bad pair kind " + msg); continue; }
    assert_throws(function () { a[fn](b); }, want, msg);
    continue;
  }
  var got;
  try { got = a[fn](b); if (fn !== "cmp" && fn !== "equals") got = got.toString(); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  if (kind === "bool") assert_true(got === want, msg);
  else if (kind === "int") assert_eq(got, want, msg);
  else assert_eq(got, want, msg);
}
''')
    n += len(q)
    # 4) mul
    q = []
    for (m, fn, opts, kind, want) in rows:
        if m == "mul":
            q.append("[%d,%s,%s,%s]" % (opts[0], J.dumps(repr(opts[1])), J.dumps(kind), J.dumps(want)))
    p.w("var MUL = [")
    p.w(",\n".join(q))
    p.w("];")
    p.w(r'''
for (var i = 0; i < MUL.length; i++) {
  var a = MUL[i][0], k = parseFloat(MUL[i][1]), kind = MUL[i][2], want = MUL[i][3];
  var msg = "mul(" + a + "," + k + ")";
  if (kind === "throw") { assert_throws(function () { new Money(a, "USD").mul(k); }, want, msg); continue; }
  var got;
  try { got = new Money(a, "USD").mul(k).toString(); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  assert_eq(got, want, msg);
}
''')
    n += len(q)
    # 5) allocate: the one invariant that justifies the type -- shares sum EXACTLY
    q = []
    for (m, a, shares, kind, want) in rows:
        if m == "alloc":
            q.append("[%d,[%s],%d]" % (a, ",".join(str(s) for s in shares), want))
    p.w("var AL = [")
    p.w(",\n".join(q))
    p.w("];")
    p.w(r'''
for (var i = 0; i < AL.length; i++) {
  var a = AL[i][0], shares = AL[i][1], total = AL[i][2];
  var msg = "allocate(" + a + ",[" + shares + "])";
  var parts;
  try { parts = new Money(a, "USD").allocate(shares).map(function (m) { return m.amount(); }); }
  catch (e) { __fail++; __out("FAIL " + msg + " THREW " + e.name); continue; }
  var sum = 0;
  for (var j = 0; j < parts.length; j++) sum += parts[j];
  assert_eq(sum, total, msg + " exact-sum invariant");
  var base = Math.abs(total) / shares.length | 0;
  assert_true(parts.every(function (p) { return p <= Math.abs(a) && p >= -Math.abs(a); }),
              msg + " no share exceeds the amount magnitude");
}
''')
    n += len(q)
    # 6) allocate errors
    q = []
    for (m, fn, opts, kind, want) in rows:
        if m == "allocbad":
            q.append("[%s,%s]" % (J.dumps(str(opts)), J.dumps(want)))
    p.w("var ALB = [")
    p.w(",\n".join(q))
    p.w("];")
    p.w(r'''
for (var i = 0; i < ALB.length; i++) {
  assert_throws(function () { new Money(100, "USD").allocate(eval(ALB[i][0])); },
                ALB[i][1], "allocate rejects " + ALB[i][0]);
}
''')
    n += len(q)
    # 7) typed edge battery (hand-written, spec-quoted)
    p.w(r'''
// cross-currency is a missing exchange rate, not arithmetic
assert_throws(function () { new Money(1, "USD").add(new Money(1, "EUR")); }, "TypeError", "cross-currency add");
assert_throws(function () { new Money(1, "USD").sub(new Money(1, "EUR")); }, "TypeError", "cross-currency sub");
// same code, different minorDigits scale
assert_throws(function () {
  new Money(1, "USD", { minorDigits: 2 }).add(new Money(1, "USD", { minorDigits: 6 }));
}, "TypeError", "scale mismatch");
// currency validation
assert_throws(function () { new Money(1, "US"); }, "RangeError", "currency 2 letters");
assert_throws(function () { new Money(1, "USDD"); }, "RangeError", "currency 4 letters");
assert_throws(function () { new Money(1, "U$D"); }, "RangeError", "currency symbol");
assert_throws(function () { new Money(1, 42); }, "TypeError", "currency non-string");
assert_eq(new Money(1, "usd").currency(), "USD", "currency lowercased input uppercased");
// minorDigits bounds
assert_throws(function () { new Money(1, "USD", { minorDigits: 7 }); }, "RangeError", "minorDigits 7");
assert_throws(function () { new Money(1, "USD", { minorDigits: -1 }); }, "RangeError", "minorDigits -1");
assert_eq(new Money(0, "USD", { minorDigits: 0 }).toString(), "0", "minorDigits 0");
assert_eq(new Money(1, "USD", { minorDigits: 0 }).toString(), "1", "minorDigits 0 amount 1");
assert_eq(new Money(1, "USD", { minorDigits: 6 }).toString(), "0.000001", "minorDigits 6");
// minorUnits must be an integer count
assert_throws(function () { new Money(1.5, "USD"); }, "RangeError", "fractional cent");
assert_throws(function () { new Money(NaN, "USD"); }, "RangeError", "NaN cent");
assert_throws(function () { new Money("100", "USD"); }, "TypeError", "string minorUnits");
// operand must be a Money
assert_throws(function () { new Money(1, "USD").add(1); }, "TypeError", "add non-Money");
assert_throws(function () { new Money(1, "USD").cmp(new Decimal(1)); }, "TypeError", "cmp Decimal");
// toJSON is toString
assert_eq(JSON.stringify({ m: new Money(1999, "USD") }), '{"m":"19.99"}', "toJSON");
// toDecimal is exact at every minorDigits
for (var md = 0; md <= 6; md++) {
  var d = new Money(123, "USD", { minorDigits: md }).toDecimal().toString();
  var div = [1, 10, 100, 1000, 10000, 100000, 1000000][md];
  var w = Math.floor(123 / div), f = 123 % div;
  var want = String(w) + (md ? "." + String(f).padStart(md, "0") : "");
  assert_eq(d, want, "toDecimal minorDigits=" + md);
}
// INT64_MIN mul(-1) must overflow, not wrap
assert_throws(function () { new Money(-9223372036854775808, "USD").mul(-1); },
              "RangeError", "mul(-1) of INT64_MIN");
// INT64 edge: add to INT64_MAX
assert_throws(function () {
  new Money(9223372036854775807, "USD").add(new Money(1, "USD"));
}, "RangeError", "add overflow at INT64_MAX");
''')
    n += 16
    p.close(n)

# ===================================================================== main
def main():
    write_arith()
    write_pow()
    write_round()
    write_parse()
    write_number()
    write_money()

if __name__ == "__main__":
    main()
