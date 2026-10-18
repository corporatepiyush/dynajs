#!/usr/bin/env python3
"""Generate tests/corpus/ref/dataframe_vectors.js: dyna:dataframe reference values.

The datasets are NOT stored: tests/dataframe/test_dataframe_ref.js rebuilds them from
the same splitmix64 stream using only + - * and floor, which IEEE 754 makes
bit-identical in Python and in JS. Only the expected answers are vendored.

Where each expected value comes from:
  - moments (sum, mean, variances, skew/kurtosis in both conventions, sem,
    cov, corr, the REGR family, mad, weighted mean, dot product): exact
    rational arithmetic (fractions.Fraction) over the input doubles, rounded
    once (square roots in 50-digit mpmath);
  - order statistics, ranks, scans, rolling windows, EMA, groupby, value
    counts, histogram: numpy / pandas / scipy, each mapped to the convention
    the dynajs.d.ts doc states (cited per verb below).
Nothing here reads an answer from dynajs. Regenerate with the pinned env:

    tmp_audit/refenv/bin/python tools/gen_dataframe_ref_vectors.py > tests/corpus/ref/dataframe_vectors.js
"""
import math
import sys
from fractions import Fraction

import mpmath as mp
import numpy as np
import pandas as pd
import scipy
import scipy.stats as st

mp.mp.dps = 50
M64 = (1 << 64) - 1


class SplitMix:
    def __init__(self, seed):
        self.s = seed & M64

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & M64
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)

    def u(self):
        return (self.next() >> 11) * (1.0 / 9007199254740992.0)


# dataset recipes: keep in lockstep with makeData() in tests/dataframe/test_dataframe_ref.js
def make(kind, n, seed):
    r = SplitMix(seed)
    x, y, w, k = [], [], [], []
    for _ in range(n):
        if kind == "uniform":
            v = r.u() * 200.0 - 100.0
        elif kind == "normalish":
            v = 0.0
            for _ in range(12):
                v += r.u()
            v -= 6.0
        elif kind == "offset":
            v = 1e9 + (r.u() - 0.5)
        elif kind == "ties":
            v = float(math.floor(r.u() * 7.0))
        elif kind == "near1":
            v = 1.0 + (r.u() - 0.5) * 0.02
        else:
            raise ValueError(kind)
        x.append(v)
        y.append(0.5 * v + (r.u() - 0.5) * 10.0)
        w.append(r.u())
        k.append(float(math.floor(r.u() * 13.0)))
    return x, y, w, k


def F(xs):
    return [Fraction(v) for v in xs]


def fl(q):
    return float(q) if not isinstance(q, mp.mpf) else float(q)


def msqrt(q):
    return mp.sqrt(mp.mpf(q.numerator) / q.denominator)


def moments(xs):
    f = F(xs)
    n = len(f)
    s = sum(f)
    mean = s / n
    d = [v - mean for v in f]
    m2 = sum(v * v for v in d) / n
    m3 = sum(v ** 3 for v in d) / n
    m4 = sum(v ** 4 for v in d) / n
    return f, n, s, mean, d, m2, m3, m4


rows = []


def add(ds, verb, args, want):
    rows.append((ds, verb, args, want))


def reductions(ds, x, y, w, k):
    f, n, s, mean, d, m2, m3, m4 = moments(x)
    add(ds, "SUM", ["x"], fl(s))
    add(ds, "MEAN", ["x"], fl(mean))
    add(ds, "VARIANCE_POP", ["x"], fl(m2))
    add(ds, "STDDEV_POP", ["x"], fl(msqrt(m2)))
    if n > 1:
        add(ds, "VARIANCE", ["x"], fl(m2 * n / (n - 1)))
        add(ds, "STDDEV", ["x"], fl(msqrt(m2 * n / (n - 1))))
        add(ds, "SEM", ["x"], fl(msqrt(m2 * n / (n - 1)) / mp.sqrt(n)))
    if m2 > 0:
        g1 = mp.mpf(m3.numerator) / m3.denominator / msqrt(m2) ** 3
        g2 = (m4 / (m2 * m2)) - 3
        add(ds, "SKEW", ["x"], fl(g1))
        add(ds, "KURTOSIS", ["x"], fl(g2))
        # pandas Series.skew / .kurt: the adjusted Fisher-Pearson estimators
        add(ds, "SKEW_SAMP", ["x"], fl(g1 * mp.sqrt(n * (n - 1)) / (n - 2)) if n >= 3 else 0.0)
        add(ds, "KURT_SAMP", ["x"],
            fl(((n + 1) * g2 + 6) * Fraction(n - 1, (n - 2) * (n - 3))) if n >= 4 else 0.0)
    add(ds, "MAD", ["x"], fl(sum(abs(v) for v in d) / n))
    srt = sorted(x)
    add(ds, "MIN", ["x"], srt[0])
    add(ds, "MAX", ["x"], srt[-1])
    add(ds, "ARG_MIN", ["x"], int(np.argmin(x)))
    add(ds, "ARG_MAX", ["x"], int(np.argmax(x)))
    add(ds, "MEDIAN", ["x"], float(np.median(np.array(x))))
    med = Fraction(float(np.median(np.array(x))))
    add(ds, "MEDIAN_ABSOLUTE_DEVIATION", ["x"], float(np.median([float(abs(v - med)) for v in f])))
    for q in [0.0, 0.1, 0.25, 0.5, 0.9, 0.999, 1.0]:
        lin = float(np.quantile(np.array(x), q, method="linear"))
        add(ds, "QUANTILE", ["x", q], lin)
        add(ds, "PERCENTILE_CONT", ["x", q], lin)
        add(ds, "QUANTILE_EXACT_LOW", ["x", q], srt[math.floor(q * (n - 1))])
        add(ds, "QUANTILE_EXACT_HIGH", ["x", q], srt[math.ceil(q * (n - 1))])
        if q > 0:
            add(ds, "PERCENTILE_DISC", ["x", q], srt[math.ceil(q * n) - 1])
    qs = [0.05, 0.5, 0.95]
    add(ds, "QUANTILES", ["x", qs], [float(np.quantile(np.array(x), q)) for q in qs])
    # pairwise, exact
    g = F(y)
    my = sum(g) / n
    sxx = sum(v * v for v in d)
    syy = sum((b - my) ** 2 for b in g)
    sxy = sum(a * (b - my) for a, b in zip(d, g))
    add(ds, "COV_POP", ["x", "y"], fl(sxy / n))
    if n > 1:
        add(ds, "COV_SAMP", ["x", "y"], fl(sxy / (n - 1)))
    if sxx > 0 and syy > 0:
        add(ds, "CORR", ["x", "y"], fl(mp.mpf(sxy.numerator) / sxy.denominator / (msqrt(sxx) * msqrt(syy))))
        add(ds, "REGR_SLOPE", ["y", "x"], fl(sxy / sxx))
        add(ds, "REGR_INTERCEPT", ["y", "x"], fl(my - (sxy / sxx) * mean))
        add(ds, "REGR_R2", ["y", "x"], fl(sxy * sxy / (sxx * syy)))
    add(ds, "REGR_SXX", ["y", "x"], fl(sxx))
    add(ds, "REGR_SYY", ["y", "x"], fl(syy))
    add(ds, "REGR_SXY", ["y", "x"], fl(sxy))
    add(ds, "REGR_AVG_X", ["y", "x"], fl(mean))
    add(ds, "REGR_AVG_Y", ["y", "x"], fl(my))
    add(ds, "DOT_PRODUCT", ["x", "y"], fl(sum(a * b for a, b in zip(f, g))))
    wf = F(w)
    add(ds, "MEAN_WEIGHTED", ["x", "w"], fl(sum(a * b for a, b in zip(f, wf)) / sum(wf)))
    add(ds, "DELTA_SUM", ["x"], fl(sum(max(f[i] - f[i - 1], Fraction(0)) for i in range(1, n))))
    if n > 2:
        add(ds, "RANK_CORR", ["x", "y"], float(st.spearmanr(x, y).statistic))


def discrete(ds, x, k):
    vals, counts = np.unique(np.array(x), return_counts=True)
    n = len(x)
    p = [Fraction(int(c), n) for c in counts]
    add(ds, "ENTROPY", ["x"], fl(-sum(mp.mpf(q.numerator) / q.denominator * mp.log(mp.mpf(q.numerator) / q.denominator, 2) for q in p)))
    add(ds, "N_UNIQUE", ["x"], len(vals))
    # MODE: the most frequent; ties go to the first in ROW order (d.ts)
    best = max(counts)
    first = next(v for v in x if counts[list(vals).index(v)] == best)
    add(ds, "MODE", ["x"], first)
    add(ds, "VALUE_COUNTS", ["x"], {repr(float(v)): int(c) for v, c in zip(vals, counts)})
    add(ds, "UNIQUE", ["x"], list(pd.unique(np.array(x))))
    # integer keys are dense (d.ts): every key 0..max(k) is a group, and an
    # empty one has sum 0, count 0 and NaN for mean/min/max
    df = pd.DataFrame({"k": k, "x": x})
    gb = df.groupby("k")["x"]
    every = [float(v) for v in range(int(max(k)) + 1)]
    empty = {"sum": 0.0, "mean": float("nan"), "min": float("nan"), "max": float("nan")}
    for verb, fn in [("GROUP_BY_SUM", "sum"), ("GROUP_BY_MEAN", "mean"), ("GROUP_BY_MIN", "min"),
                     ("GROUP_BY_MAX", "max")]:
        got = {float(kk): float(v) for kk, v in getattr(gb, fn)().items()}
        add(ds, verb, ["k", "x"], {repr(kk): got.get(kk, empty[fn]) for kk in every})
    cnt = {float(kk): int(v) for kk, v in gb.count().items()}
    add(ds, "GROUP_BY_COUNT", ["k"], {repr(kk): cnt.get(kk, 0) for kk in every})


def scans(ds, x, y):
    s = pd.Series(x)
    f = F(x)
    n = len(x)
    add(ds, "CUM_SUM", ["x"], list(np.cumsum(x)))
    add(ds, "CUM_MAX", ["x"], list(np.maximum.accumulate(x)))
    add(ds, "CUM_MIN", ["x"], list(np.minimum.accumulate(x)))
    for p in [1, 2, -3, 0, n + 5]:
        add(ds, "SHIFT", ["x", p], list(s.shift(p)))
    for p in [1, 3]:
        add(ds, "DIFF", ["x", p], list(s.diff(p)))
    # pandas' rolling(w) convention (min_periods = w: NaN until the window is
    # full), but the values exact: pandas' own online var/std update is not
    # correctly rounded (it gives 0.30000000000000554 for a window of small
    # integers whose variance is 3/10)
    f = F(x)
    nan = float("nan")
    for wdw in [1, 5, n, n + 50]:
        rs, rm, rmin, rmax, rv, rsd = [], [], [], [], [], []
        for i in range(n):
            if i + 1 < wdw:
                for lst in (rs, rm, rmin, rmax, rv, rsd):
                    lst.append(nan)
                continue
            win = f[i + 1 - wdw:i + 1]
            tot = sum(win)
            mu = tot / wdw
            rs.append(fl(tot))
            rm.append(fl(mu))
            rmin.append(float(min(win)))
            rmax.append(float(max(win)))
            if wdw > 1:
                v = sum((t - mu) ** 2 for t in win) / (wdw - 1)
                rv.append(fl(v))
                rsd.append(fl(msqrt(v)) if v > 0 else 0.0)
        add(ds, "ROLLING_SUM", ["x", wdw], rs)
        add(ds, "ROLLING_MEAN", ["x", wdw], rm)
        add(ds, "ROLLING_MIN", ["x", wdw], rmin)
        add(ds, "ROLLING_MAX", ["x", wdw], rmax)
        if wdw > 1:
            add(ds, "ROLLING_VAR", ["x", wdw], rv)
            add(ds, "ROLLING_STD", ["x", wdw], rsd)
    for a in [0.3, 1.0]:
        add(ds, "EMA", ["x", a], list(s.ewm(alpha=a, adjust=False).mean()))
    # the d.ts formula (x[i] - x[i-p]) / x[i-p], exact; pandas computes
    # x[i] / x[i-p] - 1, which cancels when the change is small
    for p in [1, 4]:
        pc = []
        for i in range(n):
            if i < p:
                pc.append(float("nan"))
            elif x[i - p] == 0:
                pc.append(float("nan") if x[i] == 0 else math.copysign(math.inf, x[i]))
            else:
                pc.append(fl((f[i] - f[i - p]) / f[i - p]))
        add(ds, "PCT_CHANGE", ["x", p], pc)
    add(ds, "ZSCORE", ["x"], list((s - s.mean()) / s.std(ddof=1)))
    add(ds, "RANK", ["x"], list(st.rankdata(x, method="average").astype(float)))
    add(ds, "DENSE_RANK", ["x"], list(st.rankdata(x, method="dense").astype(float)))
    mn = st.rankdata(x, method="min")
    add(ds, "PERCENT_RANK", ["x"], [(float(v) - 1) / (n - 1) for v in mn])
    add(ds, "SORT", ["x"], sorted(x))
    add(ds, "N_LARGEST", ["x", 10], sorted(x, reverse=True)[:10])
    add(ds, "N_SMALLEST", ["x", 10], sorted(x)[:10])
    add(ds, "N_LARGEST", ["x", n + 3], sorted(x, reverse=True))
    lo, hi = min(x), max(x)
    cnt, edges = np.histogram(x, bins=10, range=(lo, hi))
    add(ds, "HISTOGRAM", ["x", 10], {"edges": list(edges), "counts": [float(c) for c in cnt]})


SETS = [("uniform", 1500, 11), ("normalish", 1500, 12), ("offset", 1500, 13), ("ties", 1500, 14),
        ("near1", 600, 15), ("uniform", 3, 16), ("uniform", 4, 17), ("ties", 2, 18)]
for kind, n, seed in SETS:
    ds = f"{kind}:{n}:{seed}"
    x, y, w, k = make(kind, n, seed)
    reductions(ds, x, y, w, k)
    if kind == "ties":
        discrete(ds, x, k)
    if kind == "near1":
        f = F(x)
        p = Fraction(1)
        for v in f:
            p *= v
        add(ds, "PRODUCT", ["x"], fl(mp.mpf(p.numerator) / p.denominator))
for kind, n, seed in [("uniform", 150, 21), ("ties", 150, 22), ("normalish", 150, 23)]:
    ds = f"{kind}:{n}:{seed}"
    x, y, w, k = make(kind, n, seed)
    scans(ds, x, y)


def js(v):
    if isinstance(v, str):
        return '"' + v + '"'
    if isinstance(v, dict):
        return "{" + ", ".join(js(kk) + ": " + js(vv) for kk, vv in v.items()) + "}"
    if isinstance(v, (list, tuple, np.ndarray)):
        return "[" + ", ".join(js(x) for x in v) + "]"
    if isinstance(v, (bool, np.bool_)):
        return "true" if v else "false"
    if isinstance(v, (int, np.integer)):
        return str(int(v))
    v = float(v)
    if math.isnan(v):
        return "NaN"
    if math.isinf(v):
        return "Infinity" if v > 0 else "-Infinity"
    return repr(v)


out = sys.stdout
out.write("// GENERATED by tools/gen_dataframe_ref_vectors.py -- do not edit by hand.\n")
out.write(f"// numpy {np.__version__}, pandas {pd.__version__}, scipy {scipy.__version__}, mpmath {mp.__version__};\n")
out.write("// moments are exact rational arithmetic over the input doubles.\n")
out.write("export const ROWS = [\n")
for ds, verb, args, want in rows:
    out.write(f"    [{js(ds)}, {js(verb)}, {js(args)}, {js(want)}],\n")
out.write("];\n")
