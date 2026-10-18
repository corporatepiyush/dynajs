#!/usr/bin/env python3
"""Generate tests/corpus/ref/decimal_vectors.js: dyna:decimal reference results.

The reference is CPython's decimal module (libmpdec), which implements the
General Decimal Arithmetic specification; its sqrt/exp/ln/log10 are
documented as correctly rounded half-even, which is what dynajs.d.ts promises
for those four. Exact operations run at a precision far above any operand's
digit count, so they are exact. Mapping of the d.ts rounding names:
  up ROUND_UP, down ROUND_DOWN, ceil ROUND_CEILING, floor ROUND_FLOOR,
  halfUp ROUND_HALF_UP, halfDown ROUND_HALF_DOWN, halfEven ROUND_HALF_EVEN;
  halfOdd has no libmpdec mode and is derived here from the two neighbours.
Expected text is the canonical form dynajs prints: plain notation, no
exponent, trailing fractional zeros removed, zero as "0".

    python3 tools/gen_decimal_ref_vectors.py > tests/corpus/ref/decimal_vectors.js
"""
import random
import sys
from decimal import (Context, Decimal as D, ROUND_CEILING, ROUND_DOWN, ROUND_FLOOR,
                     ROUND_HALF_DOWN, ROUND_HALF_EVEN, ROUND_HALF_UP, ROUND_UP,
                     localcontext)

MODES = {"up": ROUND_UP, "down": ROUND_DOWN, "ceil": ROUND_CEILING, "floor": ROUND_FLOOR,
         "halfUp": ROUND_HALF_UP, "halfDown": ROUND_HALF_DOWN, "halfEven": ROUND_HALF_EVEN}
EXACT = Context(prec=100000, Emax=999999999, Emin=-999999999)


def canon(d):
    if d.is_zero():
        return "0"
    s = format(d.normalize(EXACT), "f")
    return s


def half_odd(fn_floor, fn_ceil, fn_even, value):
    lo, hi = fn_floor(), fn_ceil()
    if lo == hi:
        return lo
    even = fn_even()
    # a tie is the only case half-even and half-odd disagree: take the other
    dlo, dhi = abs(value - lo), abs(hi - value)
    if dlo != dhi:
        return even
    return hi if even == lo else lo


def ctx_round(p, mode, fn):
    if mode == "halfOdd":
        c = lambda m: fn(Context(prec=p, rounding=m, Emax=999999999, Emin=-999999999))
        exact = fn(EXACT)
        return half_odd(lambda: c(ROUND_FLOOR), lambda: c(ROUND_CEILING), lambda: c(ROUND_HALF_EVEN), exact)
    return fn(Context(prec=p, rounding=MODES[mode], Emax=999999999, Emin=-999999999))


def quant(d, dp, mode):
    q = D(1).scaleb(-dp)
    if mode == "halfOdd":
        exact = d
        return half_odd(lambda: d.quantize(q, ROUND_FLOOR, EXACT), lambda: d.quantize(q, ROUND_CEILING, EXACT),
                        lambda: d.quantize(q, ROUND_HALF_EVEN, EXACT), exact)
    return d.quantize(q, MODES[mode], EXACT)


rng = random.Random(0xDEC1)


def rnd_dec():
    kind = rng.random()
    if kind < 0.1:
        return rng.choice(["0", "1", "-1", "0.5", "-0.5", "2", "10", "0.1", "999999999999999999999", "0.000001"])
    nd = rng.choice([1, 2, 3, 5, 9, 17, 18, 19, 20, 34, 35, 60])
    digits = "".join(rng.choice("0123456789") for _ in range(nd)).lstrip("0") or "0"
    exp = rng.randint(-30, 20)
    d = D(("-" if rng.random() < 0.4 else "") + digits).scaleb(exp)
    return canon(d)


rows = []


def add(op, args, want):
    rows.append((op, args, want))


for _ in range(400):
    a, b = rnd_dec(), rnd_dec()
    A, B = D(a), D(b)
    add("add", [a, b], canon(EXACT.add(A, B)))
    add("sub", [a, b], canon(EXACT.subtract(A, B)))
    add("mul", [a, b], canon(EXACT.multiply(A, B)))
    add("cmp", [a, b], (A > B) - (A < B))
    add("equals", [a, b], A == B)
    if not B.is_zero():
        q = EXACT.divide_int(A, B)
        r = EXACT.remainder(A, B)
        add("mod", [a, b], canon(r))
        add("divmod", [a, b], [canon(q), canon(r)])
        for p in [1, 5, 34, 50]:
            mode = rng.choice(list(MODES) + ["halfOdd"])
            add("div", [a, b, p, mode], canon(ctx_round(p, mode, lambda c: c.divide(A, B))))
# ties and near-ties for every rounding mode, where modes actually disagree
for num, den in [(1, 8), (-1, 8), (5, 2), (-5, 2), (15, 4), (-15, 4), (1, 3), (2, 3), (-2, 3), (25, 1000), (35, 1000)]:
    for mode in list(MODES) + ["halfOdd"]:
        for p in [1, 2]:
            add("div", [str(num), str(den), p, mode], canon(ctx_round(p, mode, lambda c: c.divide(D(num), D(den)))))
# integer powers: positive exact, negative rounded at the precision
for _ in range(80):
    a = rnd_dec()
    A = D(a)
    n = rng.randint(0, 12)
    add("pow", [a, n, 34, "halfEven"], canon(EXACT.power(A, n)))
    if not A.is_zero():
        n2 = -rng.randint(1, 6)
        add("pow", [a, n2, 34, "halfEven"], canon(Context(prec=34, rounding=ROUND_HALF_EVEN, Emax=999999999, Emin=-999999999).power(A, n2)))
# correctly rounded transcendental functions
for v in ["2", "0.5", "10", "1e-20", "123456.789", "0.999999999", "1.000000001", "3", "1e30", "7.5"]:
    for p in [1, 7, 34, 60]:
        c = Context(prec=p, rounding=ROUND_HALF_EVEN, Emax=999999999, Emin=-999999999)
        add("sqrt", [v, p], canon(c.sqrt(D(v))))
        add("ln", [v, p], canon(c.ln(D(v))))
        add("log10", [v, p], canon(c.log10(D(v))))
for v in ["0", "1", "-1", "0.5", "10", "-10", "1e-20", "100", "-100", "2.302585092994045684017991454684364"]:
    for p in [1, 7, 34, 60]:
        c = Context(prec=p, rounding=ROUND_HALF_EVEN, Emax=999999999, Emin=-999999999)
        add("exp", [v, p], canon(c.exp(D(v))))
for v in ["100", "1000", "1e-5", "0.001"]:
    add("log10", [v, 34], canon(Context(prec=34).log10(D(v))))
# round / toFixed / integer parts
for _ in range(200):
    a = rnd_dec()
    A = D(a)
    dp = rng.randint(-3, 8)
    mode = rng.choice(list(MODES) + ["halfOdd"])
    add("round", [a, dp, mode], canon(quant(A, dp, mode) if dp >= 0 else quant(A.scaleb(dp), 0, mode).scaleb(-dp)))
    if dp >= 0:
        txt = format(quant(A, dp, mode), "f")
        add("toFixed", [a, dp, mode], txt)
    add("floor", [a], canon(A.to_integral_value(ROUND_FLOOR)))
    add("ceil", [a], canon(A.to_integral_value(ROUND_CEILING)))
    add("trunc", [a], canon(A.to_integral_value(ROUND_DOWN)))
    add("digits", [a], 0 if A.is_zero() else len(A.normalize(EXACT).as_tuple().digits))
    add("toNumber", [a], float(A))
# binary doubles in: the shortest round-trip text, exactly
for v in [0.1, 0.2 + 0.1, 1e21, 1e-7, 5e-324, 1.7976931348623157e308, -2.5, 123456789.125, 1 / 3]:
    add("fromNumber", [v], canon(D(repr(v))))


def js(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, str):
        return '"' + v + '"'
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(js(x) for x in v) + "]"
    if isinstance(v, float):
        return repr(v)
    return str(v)


out = sys.stdout
out.write("// GENERATED by tools/gen_decimal_ref_vectors.py -- do not edit by hand.\n")
out.write(f"// CPython {sys.version.split()[0]} decimal (libmpdec): the General Decimal Arithmetic specification.\n")
out.write("export const ROWS = [\n")
for op, args, want in rows:
    out.write(f"    [{js(op)}, {js(args)}, {js(want)}],\n")
out.write("];\n")
