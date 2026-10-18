#!/usr/bin/env python3
"""Generate tests/corpus/ref/mathx_vectors.js: dyna:mathx reference values.

The expected values come from outside the engine:
  - special functions: mpmath (arbitrary precision; 60 significant digits,
    raised where an argument needs it), rounded once to the nearest double;
  - inverse functions (erfcinv, gammaincinv, betaincinv): mp.findroot on the
    forward function at the same precision, started from scipy's estimate;
  - stats: exact rational arithmetic (fractions.Fraction) over the very
    doubles the test passes in, rounded once.
Nothing here reads an answer from dynajs. Regenerate with the pinned
environment (tmp_audit/refenv: mpmath, scipy, numpy):

    tmp_audit/refenv/bin/python tools/gen_mathx_ref_vectors.py > tests/corpus/ref/mathx_vectors.js
"""
import math
import random
import sys
from fractions import Fraction

import mpmath as mp
import numpy
import scipy
import scipy.special as sp

mp.mp.dps = 60


def f2s(x):
    x = float(x)
    if math.isnan(x):
        return "NaN"
    if math.isinf(x):
        return "Infinity" if x > 0 else "-Infinity"
    return repr(x)


def mpd(v):
    return float(mp.mpf(v)) if not isinstance(v, mp.mpc) else float(v.real)


rows = {}


def add(fam, args, want):
    rows.setdefault(fam, []).append((args, want))


# ---- one-argument special functions ---------------------------------------
GAMMA_X = [1e-300, 1e-20, 1e-8, 0.001, 0.1, 0.5, 0.75, 1.0, 1.5, 2.0, 2.5, 3.0,
           4.5, 7.25, 10.0, 17.5, 33.3, 100.5, 141.0, 170.5, 171.6,
           -0.5, -1.5, -2.25, -10.1, -50.5, -100.25, -170.5]
for x in GAMMA_X:
    add("gamma", [x], mpd(mp.gamma(x)))
LGAM_X = GAMMA_X + [200.0, 1e3, 1e5, 1e10, 1e100, 1e300, 0.999999, 1.000001,
                    1.999999, 2.000001]
for x in LGAM_X:
    add("gammaln", [x], mpd(mp.log(abs(mp.gamma(x)))))
for x in [1e-300, 1e-12, 0.01, 0.5, 1.0, 1.4616321449683623, 2.0, 3.7, 10.0,
          100.0, 1e6, 1e15, -0.5, -1.25, -7.9, -100.5]:
    add("psi", [x], mpd(mp.digamma(x)))
ERF_X = [0.0, 1e-300, 1e-20, 1e-8, 1e-3, 0.1, 0.3, 0.46875, 0.5, 0.84375, 1.0,
         1.25, 2.0, 2.5, 3.0, 4.0, 5.5, 6.0, 26.5, -1e-8, -0.5, -1.0, -3.0]
for x in ERF_X:
    add("erf", [x], mpd(mp.erf(x)))
for x in ERF_X + [8.0, 10.0, 27.0, -6.0]:
    add("erfc", [x], mpd(mp.erfc(x)))
for x in [0.0, 1e-8, 0.5, 1.0, 2.0, 5.0, 10.0, 25.0, 100.0, 1e4, 1e8, 1e150,
          -0.5, -1.0, -5.0, -20.0]:
    add("erfcx", [x], mpd(mp.exp(mp.mpf(x) ** 2) * mp.erfc(x)))
for y in [0.0, 1e-300, 1e-12, 1e-6, 0.001, 0.1, 0.3, 0.5, 0.7, 0.9, 0.99,
          0.999999, 0.9999999999, -0.25, -0.9, -0.999999999]:
    add("erfinv", [y], mpd(mp.erfinv(y)))
for y in [1e-300, 1e-100, 1e-20, 1e-10, 0.001, 0.1, 0.5, 1.0, 1.5, 1.9,
          1.999, 1.99999999]:
    with mp.workdps(700):
        add("erfcinv", [y], mpd(mp.erfinv(1 - mp.mpf(y)))
            if y > 1e-12 else mpd(mp.findroot(lambda t: mp.erfc(t) - mp.mpf(y),
                                              mp.mpf(float(sp.erfcinv(y))))))
for x in [1e-300, 1e-10, 1e-3, 0.1, 0.5, 1.0, 1.5, 2.5, 5.0, 10.0, 50.0, 200.0,
          700.0]:
    add("expint", [x], mpd(mp.e1(x)))
for x in [1e-300, -1e-300, 1e-17, 1e-10, -1e-10, 1e-5, 0.1, -0.1, 0.5, 1.0,
          -1.0, 10.0, 700.0, -40.0]:
    add("expm1", [x], mpd(mp.expm1(x)))
for x in [1e-300, 1e-17, -1e-17, 1e-10, 1e-5, 0.1, -0.5, -0.9999999, 1.0,
          10.0, 1e10, 1e300]:
    add("log1p", [x], mpd(mp.log1p(x)))

# ---- two-argument special functions ---------------------------------------
for a, b in [(0.5, 0.5), (1, 1), (2, 3), (1e-8, 1), (1e-300, 2), (10, 20),
             (100, 0.5), (300, 400), (1e5, 1e-5), (0.25, 7.5), (2.5, -0.5),
             (50, 50)]:
    add("beta", [a, b], mpd(mp.beta(a, b)))
    add("betaln", [a, b], mpd(mp.log(abs(mp.beta(a, b)))))
for n, x in [(0, 0.5), (0, 10), (1, 1), (1, 1e-5), (2, 3.5), (3, -2.5),
             (5, 0.25), (10, 100), (20, 1.5), (1, 1e10), (4, 1e-3), (0, -0.5)]:
    add("polygamma", [n, x], mpd(mp.polygamma(n, x)))
# gammainc(x, a [, "upper"]) = P(a, x) / Q(a, x)
GI = [(0.0, 1.0), (1e-10, 1.0), (0.5, 0.5), (1.0, 1.0), (2.0, 3.0),
      (10.0, 10.0), (9.0, 10.0), (11.0, 10.0), (0.1, 30.0), (60.0, 30.0),
      (100.0, 100.0), (1e-3, 1e-3), (1000.0, 1050.0), (5.0, 0.25),
      (50.0, 20.0), (1e4, 1e4 + 100), (3.0, 1e-8)]
for x, a in GI:
    add("gammainc_lower", [x, a], mpd(mp.gammainc(a, 0, x, regularized=True)))
    add("gammainc_upper", [x, a], mpd(mp.gammainc(a, x, mp.inf, regularized=True)))
for x, a, b in [(0.0, 1, 1), (0.5, 0.5, 0.5), (0.3, 2, 3), (0.9, 10, 2),
                (0.01, 0.1, 50), (0.5, 100, 100), (0.49, 500, 500),
                (0.999, 3, 0.25), (1e-10, 2, 2), (0.7, 1e-3, 1e-3),
                (0.2, 30, 5), (1.0, 4, 5)]:
    add("betainc", [x, a, b], mpd(mp.betainc(a, b, 0, x, regularized=True)))
def invert(f, est, lo, hi):
    # f is increasing on [lo, hi]; bisect in log space when the bracket spans
    # decades. 400 halvings are far past 60 digits, and nothing can stall.
    lo, hi = mp.mpf(lo), mp.mpf(hi)
    est = mp.mpf(est)
    a_, b_ = max(lo, est * (1 - mp.mpf("1e-3"))), min(hi, est * (1 + mp.mpf("1e-3")))
    if not (f(a_) <= 0 <= f(b_)):
        a_, b_ = lo, hi
    for _ in range(400):
        m = mp.sqrt(a_ * b_) if a_ > 0 and b_ / a_ > 4 else (a_ + b_) / 2
        if f(m) < 0:
            a_ = m
        else:
            b_ = m
    return (a_ + b_) / 2


for p, a in [(0.5, 1.0), (0.1, 2.0), (0.9, 10.0), (1e-10, 3.0),
             (0.999999, 5.0), (0.5, 0.1), (0.01, 100.0), (0.5, 1000.0)]:
    g = invert(lambda t: mp.gammainc(a, 0, t, regularized=True) - p,
               float(sp.gammaincinv(a, p)), mp.mpf("1e-300"), mp.mpf(1e6))
    add("gammaincinv", [p, a], mpd(g))
for p, a, b in [(0.5, 1, 1), (0.5, 2, 3), (0.1, 0.5, 0.5), (0.9, 10, 2),
                (1e-8, 2, 5), (0.999, 3, 0.25), (0.5, 100, 100), (0.3, 0.1, 9)]:
    g = invert(lambda t: mp.betainc(a, b, 0, t, regularized=True) - p,
               float(sp.betaincinv(a, b, p)), mp.mpf(0), mp.mpf(1))
    add("betaincinv", [p, a, b], mpd(g))

def gP(a, x):
    try:
        return mp.gammainc(a, 0, x, regularized=True)
    except mp.libmp.libhyper.NoConvergence:
        a, x = mp.mpf(a), mp.mpf(x)
        return mp.exp(a * mp.log(x) - x - mp.loggamma(a + 1)) * mp.hyp1f1(1, a + 1, x, maxterms=10 ** 8)


def gQ_cf(a, x):
    # Legendre's continued fraction for Gamma(a, x), modified Lentz, at the
    # working precision; valid (and fast) for x > a + 1. Unlike 1 - P it keeps
    # full relative precision however small Q is.
    a, x = mp.mpf(a), mp.mpf(x)
    tiny = mp.mpf(10) ** -400
    b = x + 1 - a
    c = 1 / tiny
    d = 1 / b
    h = d
    i = 1
    while True:
        an = -i * (i - a)
        b += 2
        d = an * d + b
        d = tiny if abs(d) < tiny else d
        c = b + an / c
        c = tiny if abs(c) < tiny else c
        d = 1 / d
        de = d * c
        h *= de
        if abs(de - 1) < mp.mpf(10) ** -58:
            break
        i += 1
    return mp.exp(-x + a * mp.log(x) - mp.loggamma(a)) * h


def gQ(a, x):
    if x > a + 1:
        return gQ_cf(a, x)
    try:
        return mp.gammainc(a, x, mp.inf, regularized=True)
    except mp.libmp.libhyper.NoConvergence:
        with mp.workdps(120):
            return 1 - gP(a, x)


def bI(x, a, b):
    try:
        return mp.betainc(a, b, 0, x, regularized=True)
    except mp.libmp.libhyper.NoConvergence:
        a, b, x = mp.mpf(a), mp.mpf(b), mp.mpf(x)
        return (mp.exp(a * mp.log(x) + b * mp.log1p(-x) - mp.log(a) - mp.log(mp.beta(a, b)))
                * mp.hyp2f1(a + b, 1, a + 1, x, maxterms=10 ** 8))


# ---- seeded sweeps: dense coverage of the regimes the fixed points straddle
sw = random.Random(0xD15EA5E)
for _ in range(240):
    a = 10 ** sw.uniform(-3, 5)
    x = a * 10 ** sw.uniform(-1.5, 1.0) if sw.random() < 0.7 else a * (1 + sw.uniform(-0.6, 0.6))
    x = float(x)
    a = float(a)
    with mp.workdps(60):
        add("gammainc_sweep", [x, a], [mpd(gP(a, x)), mpd(gQ(a, x))])
for _ in range(160):
    a = float(10 ** sw.uniform(-2, 3.5))
    b = float(10 ** sw.uniform(-2, 3.5))
    x = float(min(max(a / (a + b) * (1 + sw.uniform(-0.8, 0.8)), 1e-12), 1 - 1e-12))
    add("betainc_sweep", [x, a, b], mpd(bI(x, a, b)))
for _ in range(80):
    a = float(10 ** sw.uniform(-2, 4))
    p = float(sw.choice([sw.uniform(1e-6, 0.5), sw.uniform(0.5, 1 - 1e-9), 10 ** sw.uniform(-12, -1)]))
    g = invert(lambda t: gP(a, t) - p,
               float(sp.gammaincinv(a, p)), mp.mpf("1e-300"), mp.mpf(1e8))
    add("gammaincinv_sweep", [p, a], mpd(g))
for _ in range(120):
    a = float(10 ** sw.uniform(-3, 6))
    b = float(10 ** sw.uniform(-3, 6))
    add("betaln_sweep", [a, b], mpd(mp.log(mp.beta(a, b))))
for _ in range(120):
    x = float(sw.choice([sw.uniform(-60, -0.01), sw.uniform(0.01, 4), 10 ** sw.uniform(-6, 8)]))
    if x == int(x):
        x += 0.5
    n = sw.randint(0, 9)
    add("polygamma_sweep", [n, x], mpd(mp.polygamma(n, x)))

# ---- Bessel ----------------------------------------------------------------
# points avoid the zeros of J/Y so a relative error is meaningful; the test
# measures oscillating kinds against the envelope as well.
for n, x in [(0, 1e-10), (0, 0.5), (0, 2.0), (0, 10.0), (0, 100.5), (0, 1e5 + 0.3),
             (1, 1e-8), (1, 3.0), (1, 25.5), (2, 0.1), (5, 1.0), (5, 7.0),
             (10, 3.0), (10, 30.0), (50, 10.0), (50, 70.0), (100, 150.0),
             (-3, 2.5), (3, -2.5), (200, 1.0)]:
    add("besselj", [n, x], mpd(mp.besselj(n, x)))
for n, x in [(0, 1e-5), (0, 0.5), (0, 3.0), (0, 50.5), (1, 1e-3), (1, 2.0),
             (1, 33.0), (2, 0.5), (5, 1.0), (5, 12.0), (10, 3.0), (30, 45.0),
             (-2, 1.5), (60, 10.0)]:
    add("bessely", [n, x], mpd(mp.bessely(n, x)))
for nu, x in [(0, 0.0), (0, 1e-5), (0, 1.0), (0, 10.0), (0, 700.0), (1, 0.5),
              (0.5, 2.0), (2.5, 5.0), (10, 1.0), (10, 50.0), (1 / 3, 0.1),
              (100, 10.0), (-0.5, 1.0), (-2, 3.0)]:
    add("besseli", [nu, x], mpd(mp.besseli(nu, x)))
    add("besseliScaled", [nu, x], mpd(mp.besseli(nu, x) * mp.exp(-x)))
for nu, x in [(0, 1e-10), (0, 0.5), (0, 2.0), (0, 30.0), (0, 600.0), (1, 1e-3),
              (1, 5.0), (0.5, 1.0), (1 / 3, 2.0), (2.5, 0.1), (10, 2.0),
              (10, 40.0), (50, 10.0), (-1.5, 2.0)]:
    add("besselk", [nu, x], mpd(mp.besselk(nu, x)))
    add("besselkScaled", [nu, x], mpd(mp.besselk(nu, x) * mp.exp(x)))
for n, x in [(0, 1.0), (1, 2.5), (3, 10.0), (-2, 4.0)]:
    add("besselh1", [n, x], [mpd(mp.besselj(n, x)), mpd(mp.bessely(n, x))])
    add("besselh2", [n, x], [mpd(mp.besselj(n, x)), -mpd(mp.bessely(n, x))])

# ---- elliptic, Legendre, Airy ---------------------------------------------
for m in [0.0, 1e-10, 0.1, 0.5, 0.9, 0.99, 0.999999, -1.0, -10.0]:
    add("ellipke", [m], [mpd(mp.ellipk(m)), mpd(mp.ellipe(m))])
for u, m in [(0.0, 0.5), (0.5, 0.0), (0.5, 1.0), (0.3, 0.3), (1.0, 0.7),
             (2.0, 0.99), (-1.2, 0.4), (10.0, 0.5)]:
    sn, cn, dn = (mp.ellipfun(k, u, m=m) for k in ("sn", "cn", "dn"))
    add("ellipj", [u, m], [mpd(sn), mpd(cn), mpd(dn)])
def assoc_legendre(n, m, x):
    # P_n^m(x) with the Condon-Shortley phase (-1)^m (MATLAB legendre, scipy
    # lpmv), by the three-term recurrence in 60-digit arithmetic
    x = mp.mpf(x)
    pmm = mp.mpf(1)
    if m:
        pmm = (-1) ** m * mp.fac2(2 * m - 1) * (1 - x * x) ** (mp.mpf(m) / 2)
    if n == m:
        return pmm
    p1 = x * (2 * m + 1) * pmm
    if n == m + 1:
        return p1
    p0 = pmm
    for l in range(m + 2, n + 1):
        p0, p1 = p1, ((2 * l - 1) * x * p1 - (l + m - 1) * p0) / (l - m)
    return p1


for n, m, x in [(0, 0, 0.3), (1, 0, 0.3), (1, 1, 0.3), (2, 0, -0.7), (2, 1, -0.7),
                (2, 2, -0.7), (5, 3, 0.5), (10, 0, 0.99), (10, 5, 0.2),
                (30, 10, -0.4), (3, 0, 1.0), (3, 2, 0.0)]:
    add("legendreP", [n, m, x], mpd(assoc_legendre(n, m, x)))
    add("legendre_col", [n, x], [mpd(assoc_legendre(n, k, x)) for k in range(n + 1)])
for x in [0.0, 1e-5, 0.5, 1.0, 2.0, 5.0, 10.0, -1.0, -2.2, -5.0, -15.0]:
    add("airy", [x], [mpd(mp.airyai(x)), mpd(mp.airyai(x, derivative=1)),
                      mpd(mp.airybi(x)), mpd(mp.airybi(x, derivative=1))])

# ---- stats: exact rational arithmetic over the input doubles --------------
def exact_stats(xs):
    fs = [Fraction(x) for x in xs]
    n = len(fs)
    s = sum(fs)
    mean = s / n
    ss = sum((f - mean) ** 2 for f in fs)
    return s, mean, ss


rng = random.Random(20261017)
STATS = {
    "small ints": [1.0, 2.0, 3.0, 4.0, 5.0],
    "cancelling sum": [1e16, 1.0, -1e16, 1.0, 1e-3],
    "large offset small spread": [1e9 + 0.1 * i for i in range(50)],
    "huge offset": [1e15 + (i % 7) for i in range(200)],
    "mixed magnitudes": [rng.uniform(-1, 1) * 10 ** rng.randint(-12, 12) for _ in range(300)],
    "two values": [0.1, 0.2],
    "negatives": [-3.5, -1.25, -7.0, -0.5, -2.0, -9.75],
    "ill-conditioned": [1 + 2 ** -40 * i for i in range(64)],
}
for name, xs in STATS.items():
    s, mean, ss = exact_stats(xs)
    n = len(xs)
    add("stats_sum", [name, xs], float(s))
    add("stats_mean", [name, xs], float(mean))
    add("stats_variance", [name, xs], float(ss / (n - 1)))
    add("stats_stddev", [name, xs], math.sqrt(ss / (n - 1)))
for name, (xs, ys) in {
    "line": ([1.0, 2.0, 3.0, 4.0], [2.0, 4.0, 6.0, 8.0]),
    "anti": ([1.0, 2.0, 3.0], [3.0, 2.0, 1.0]),
    "offset": ([1e9 + i for i in range(30)], [5e8 - 2 * i + (i % 3) for i in range(30)]),
    "noise": ([rng.gauss(0, 1) for _ in range(100)], [rng.gauss(0, 1) for _ in range(100)]),
}.items():
    fx, fy = [Fraction(x) for x in xs], [Fraction(y) for y in ys]
    n = len(fx)
    mx, my = sum(fx) / n, sum(fy) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(fx, fy))
    sxx = sum((a - mx) ** 2 for a in fx)
    syy = sum((b - my) ** 2 for b in fy)
    add("stats_cov", [name, xs, ys], float(sxy / (n - 1)))
    with mp.workdps(80):
        r = mp.mpf(sxy.numerator) / sxy.denominator / mp.sqrt(
            (mp.mpf(sxx.numerator) / sxx.denominator) * (mp.mpf(syy.numerator) / syy.denominator))
    add("stats_corr", [name, xs, ys], float(r))


def js(v):
    if isinstance(v, str):
        return '"' + v + '"'
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(js(x) for x in v) + "]"
    return f2s(v)


out = sys.stdout
out.write("// GENERATED by tools/gen_mathx_ref_vectors.py -- do not edit by hand.\n")
out.write(f"// mpmath {mp.__version__}, scipy {scipy.__version__}, numpy {numpy.__version__};\n")
out.write("// stats rows are exact rational arithmetic (Python fractions) over the inputs.\n")
out.write("export const VECTORS = {\n")
for fam, rs in rows.items():
    out.write(f"    {fam}: [\n")
    for args, want in rs:
        out.write(f"        [{js(args)}, {js(want)}],\n")
    out.write("    ],\n")
out.write("};\n")
