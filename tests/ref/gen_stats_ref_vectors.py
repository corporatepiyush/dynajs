#!/usr/bin/env python3
"""Reference vectors for dyna:mathx stats distributions and hypothesis tests, from scipy.

    python3 tests/ref/gen_stats_ref_vectors.py > tests/corpus/ref/stats_vectors.js

Needs numpy and scipy (made with scipy 1.18.1, numpy 2.5.4). Samples come from a fixed numpy
Generator seed and are stored with the answers, so the JS suite needs no RNG agreement. Where the
native contract deliberately differs from scipy it is generated from the documented definition:
ks2Samp's two-sided 'asymptotic' p is the Kolmogorov limit kstwobign.sf(sqrt(en) * D) (scipy uses
the finite-n kstwo), holm / hochberg / bonferroni are written out here (statsmodels is not a
dependency), and psi is the quantile-binned definition in dynajs.d.ts.
"""
import json
import math
import os
import sys

import numpy as np
import scipy
from scipy import special
from scipy import stats as st


def pinned(name, have):
    """The version written into the vectors is the pin in tests/ref/requirements.txt, never the
    running one: a runtime version string made the file's sha256 differ between two machines
    that computed the same numbers. A different running version is reported on stderr."""
    here = os.path.dirname(os.path.abspath(__file__))
    for line in open(os.path.join(here, "requirements.txt")):
        if line.startswith(name + "=="):
            pin = line.strip().split("==", 1)[1]
            if pin != have:
                sys.stderr.write("%s: %s %s is running, the vectors record the pin %s\n" % (os.path.basename(__file__), name, have, pin))
            return pin
    raise SystemExit("tests/ref/requirements.txt pins no " + name)

rng = np.random.default_rng(20261114)
ALTS = ["two-sided", "less", "greater"]


def f(x):
    x = float(x)
    if math.isnan(x):
        return "NaN"
    if math.isinf(x):
        return "Infinity" if x > 0 else "-Infinity"
    return x


def arr(a):
    return [f(v) for v in np.asarray(a, dtype=float).ravel()]


def rnd(x, sig):
    """Results (never inputs) are written to `sig` significant digits: scipy's special functions
    and numpy's libm-backed kernels differ in the last bits across machines and builds, and a
    full-precision repr made the file's sha256 differ with them. A magnitude under 1e-300 (the
    suite's absolute floor; subnormals differ the most) is written as 0. 11 digits sit 20x under the
    suite's 1e-9 tolerance; 14 digits sit under its 1e-12 tolerance for pure arithmetic (ranks,
    adjusted p-values, df, expected counts, psi)."""
    x = f(x)
    if isinstance(x, str) or abs(x) < 1e-300:
        return x if isinstance(x, str) else 0
    if JITTER:
        x = jitter(x, sig)
    return float("%.*g" % (sig, x))


JITTER = int(os.environ.get("DYNA_REF_ULP_JITTER", "0"))
_jn = [0]


def jitter(x, sig):
    """DYNA_REF_ULP_JITTER=k (k > 0 or < 0) moves every result by k ulps (k/16, at least 1, for the 14-digit
    pure arithmetic) before it is rounded, the way another libm or BLAS would. The rounded file must
    not change: `./dynajs --std tools/test-data.js repro` runs the generator at -k, 0 and +k and compares."""
    k = JITTER if sig <= 11 else max(1, abs(JITTER) // 16) * (1 if JITTER > 0 else -1)
    return x + math.copysign(abs(k), k) * math.ulp(x)


def r11(x):
    return rnd(x, 11)


def r14(x):
    return rnd(x, 14)


def arr14(a):
    return [r14(v) for v in np.asarray(a, dtype=float).ravel()]


POOL, POOL_AT = [], {}


def ref(a):
    """Samples are stored once in out['samples']; a test names one by its index."""
    key = tuple(arr(a))
    if key not in POOL_AT:
        POOL_AT[key] = len(POOL)
        POOL.append(list(key))
    return POOL_AT[key]


out = {"scipy": pinned("scipy", scipy.__version__), "numpy": pinned("numpy", np.__version__)}

xs = [-40, -8.5, -3, -1.96, -0.5, -1e-9, 0, 1e-300, 0.25, 1, 1.645, 2.5, 6, 9, 38]
ps = [1e-300, 1e-12, 1e-6, 0.001, 0.025, 0.1, 0.3, 0.5, 0.7, 0.9, 0.975, 0.999, 1 - 1e-9]
dfs = [0.5, 1, 2, 3.5, 10, 30, 250, 1e6]
D = []
for x in xs:
    D += [["normPdf", x, r11(st.norm.pdf(x))], ["normCdf", x, r11(st.norm.cdf(x))], ["normSf", x, r11(st.norm.sf(x))]]
for p in ps:
    D.append(["normPpf", p, r11(st.norm.ppf(p))])
for df in dfs:
    for x in xs:
        D += [["tPdf", x, df, r11(st.t.pdf(x, df))], ["tCdf", x, df, r11(st.t.cdf(x, df))], ["tSf", x, df, r11(st.t.sf(x, df))]]
        if x >= 0:
            D += [["chi2Pdf", x, df, r11(st.chi2.pdf(x, df))], ["chi2Cdf", x, df, r11(st.chi2.cdf(x, df))],
                  ["chi2Sf", x, df, r11(st.chi2.sf(x, df))]]
    for p in ps[1:-1]:
        D += [["tPpf", p, df, r11(st.t.ppf(p, df))], ["chi2Ppf", p, df, r11(st.chi2.ppf(p, df))]]
for d1, d2 in [(1, 1), (2, 7), (5, 30), (12.5, 3), (100, 100)]:
    for x in [0, 0.01, 0.5, 1, 2.2, 7, 40]:
        D += [["fCdf", x, d1, d2, r11(st.f.cdf(x, d1, d2))], ["fSf", x, d1, d2, r11(st.f.sf(x, d1, d2))]]
    for p in [0.01, 0.1, 0.5, 0.9, 0.99]:
        D.append(["fPpf", p, d1, d2, r11(st.f.ppf(p, d1, d2))])
for n, p in [(1, 0.5), (10, 0.3), (40, 0.02), (1000, 0.5), (25, 0.999)]:
    for k in [-1, 0, 1, 3, n // 2, n - 1, n]:
        D += [["binomPmf", k, n, p, r11(st.binom.pmf(k, n, p))], ["binomCdf", k, n, p, r11(st.binom.cdf(k, n, p))],
              ["binomSf", k, n, p, r11(st.binom.sf(k, n, p))]]
for mu in [0.1, 1, 4.5, 30, 800]:
    for k in [-1, 0, 1, 3, int(mu), int(mu * 2) + 1]:
        D += [["poissonPmf", k, mu, r11(st.poisson.pmf(k, mu))], ["poissonCdf", k, mu, r11(st.poisson.cdf(k, mu))],
              ["poissonSf", k, mu, r11(st.poisson.sf(k, mu))]]
for x in [0, 0.1, 0.3, 0.5, 0.8, 0.99, 1, 1.2, 1.36, 2, 3.5]:
    D.append(["kolmogorovSf", x, r11(special.kolmogorov(x))])
out["dist"] = D


def sample(kind, n):
    if kind == "norm":
        return np.round(rng.normal(10, 3, n), 4)
    if kind == "skew":
        return np.round(rng.lognormal(1, 0.8, n), 4)
    if kind == "int":
        return rng.integers(0, 12, n).astype(float)
    return np.round(rng.normal(10.6, 3, n), 4)


T = []
for ka, kb, na, nb in [("norm", "shift", 12, 15), ("norm", "norm", 5, 5), ("skew", "norm", 40, 33), ("int", "int", 30, 30),
                       ("norm", "shift", 2, 3), ("skew", "shift", 200, 180)]:
    a, b = sample(ka, na), sample(kb, nb)
    for alt in ALTS:
        r1 = st.ttest_1samp(a, 10.2, alternative=alt)
        T.append({"op": "ttest1samp", "a": ref(a), "mu": 10.2, "opts": {"alternative": alt},
                  "r": [r11(r1.statistic), r11(r1.pvalue), r14(r1.df)]})
        for ev in [True, False]:
            r = st.ttest_ind(a, b, equal_var=ev, alternative=alt)
            T.append({"op": "ttestInd", "a": ref(a), "b": ref(b), "opts": {"alternative": alt, "equalVar": ev},
                      "r": [r11(r.statistic), r11(r.pvalue), r14(r.df)]})
        m = min(na, nb)
        r = st.ttest_rel(a[:m], b[:m], alternative=alt)
        T.append({"op": "ttestRel", "a": ref(a[:m]), "b": ref(b[:m]), "opts": {"alternative": alt},
                  "r": [r11(r.statistic), r11(r.pvalue), r14(r.df)]})
        if m >= 3:
            for op, fn in [("pearson", st.pearsonr), ("spearman", st.spearmanr)]:
                r = fn(a[:m], b[:m], alternative=alt)
                T.append({"op": op, "a": ref(a[:m]), "b": ref(b[:m]), "opts": {"alternative": alt},
                          "r": [r11(r.statistic), r11(r.pvalue)]})
        for method in ["auto", "exact", "asymptotic"]:
            if method == "exact" and (na * nb > 4096 or ka == "int"):
                continue
            for cont in [True, False]:
                r = st.mannwhitneyu(a, b, alternative=alt, method=method, use_continuity=cont)
                T.append({"op": "mannWhitneyU", "a": ref(a), "b": ref(b),
                          "opts": {"alternative": alt, "method": method, "continuity": cont},
                          "r": [r11(r.statistic), r11(r.pvalue)]})
        for method in ["auto", "exact", "asymptotic"]:
            r = st.ks_2samp(a, b, alternative=alt, method="asymp" if method == "asymptotic" else method)
            p = r.pvalue
            if method == "asymptotic" and alt == "two-sided":
                en = na * nb / (na + nb)
                p = st.kstwobign.sf(math.sqrt(en) * r.statistic)
            T.append({"op": "ks2Samp", "a": ref(a), "b": ref(b), "opts": {"alternative": alt, "method": method},
                      "r": [r11(r.statistic), r11(p)]})
for na, nb in [(3, 9), (8, 20), (1, 7), (6, 6), (8, 300)]:
    a, b = np.round(rng.normal(0, 1, na), 6), np.round(rng.normal(0.4, 1, nb), 6)
    for alt in ALTS:
        r = st.mannwhitneyu(a, b, alternative=alt)
        T.append({"op": "mannWhitneyU", "a": ref(a), "b": ref(b), "opts": {"alternative": alt},
                  "r": [r11(r.statistic), r11(r.pvalue)]})
for na, nb in [(400, 400), (500, 730), (1, 9), (60, 7), (1200, 900)]:
    a, b = np.round(rng.normal(0, 1, na), 5), np.round(rng.normal(0.1, 1.1, nb), 5)
    for alt in ALTS:
        r = st.ks_2samp(a, b, alternative=alt)
        T.append({"op": "ks2Samp", "a": ref(a), "b": ref(b), "opts": {"alternative": alt},
                  "r": [r11(r.statistic), r11(r.pvalue)]})
for c1, n1, c2, n2 in [(50, 1000, 65, 1000), (3, 40, 9, 41), (500, 10000, 480, 9000)]:
    p1, p2, p = c1 / n1, c2 / n2, (c1 + c2) / (n1 + n2)
    z = (p1 - p2) / math.sqrt(p * (1 - p) * (1 / n1 + 1 / n2))
    for alt in ALTS:
        pv = {"two-sided": 2 * st.norm.sf(abs(z)), "less": st.norm.cdf(z), "greater": st.norm.sf(z)}[alt]
        T.append({"op": "proportionsZTest", "args": [c1, n1, c2, n2], "opts": {"alternative": alt}, "r": [r11(z), r11(pv)]})
for obs, exp, ddof in [([16, 18, 16, 14, 12, 12], None, 0), ([16, 18, 16, 14, 12, 12], [16, 16, 16, 16, 16, 8], 1),
                       ([5, 0, 3], None, 0)]:
    r = st.chisquare(obs, exp, ddof=ddof)
    T.append({"op": "chiSquare", "a": obs, "b": exp, "opts": {"ddof": ddof},
              "r": [r11(r.statistic), r11(r.pvalue), len(obs) - 1 - ddof]})
for tab, corr in [([[10, 20], [30, 25]], True), ([[10, 20], [30, 25]], False), ([[12, 5, 9, 1], [3, 8, 11, 6], [7, 7, 2, 9]], True),
                  ([[4, 6, 1]], True), ([[1, 0], [0, 1]], True), ([[5, 5], [5, 6]], True), ([[7, 2], [3, 9]], True)]:
    r = st.chi2_contingency(tab, correction=corr)
    T.append({"op": "chi2Contingency", "table": tab, "opts": {"correction": corr},
              "r": [r11(r.statistic), r11(r.pvalue), r14(r.dof)], "expected": [arr14(row) for row in r.expected_freq]})
for meth in ["average", "min", "max", "dense", "ordinal"]:
    v = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, -0.0, 0.0, -2.5]
    T.append({"op": "rankData", "a": v, "opts": {"method": meth}, "r": arr14(st.rankdata(v, method=meth))})


def holm(p):
    n = len(p)
    o = sorted(range(n), key=lambda i: p[i])
    out, run = [0.0] * n, 0.0
    for k, i in enumerate(o):
        run = max(run, min(1.0, p[i] * (n - k)))
        out[i] = run
    return out


def hochberg(p):
    n = len(p)
    o = sorted(range(n), key=lambda i: p[i])
    out, run = [0.0] * n, 1.0
    for k in range(n - 1, -1, -1):
        run = min(run, p[o[k]] * (n - k))
        out[o[k]] = run
    return out


for p in [[0.01, 0.04, 0.03, 0.2], list(np.round(rng.uniform(0, 0.2, 25), 6)), [0.5], [0.001, 0.001, 0.9, 1.0, 0.0]]:
    T.append({"op": "pAdjust", "a": p, "method": "bonferroni", "r": [r14(min(1.0, v * len(p))) for v in p]})
    T.append({"op": "pAdjust", "a": p, "method": "holm", "r": [r14(v) for v in holm(p)]})
    T.append({"op": "pAdjust", "a": p, "method": "hochberg", "r": [r14(v) for v in hochberg(p)]})
    T.append({"op": "pAdjust", "a": p, "method": "bh", "r": arr14(st.false_discovery_control(p, method="bh"))})
    T.append({"op": "pAdjust", "a": p, "method": "by", "r": arr14(st.false_discovery_control(p, method="by"))})


def psi_counts(e, a, eps):
    se, sa = sum(e), sum(a)
    return sum((max(y / sa, eps) - max(x / se, eps)) * math.log(max(y / sa, eps) / max(x / se, eps)) for x, y in zip(e, a))


for e, a in [([10, 20, 30], [12, 18, 30]), ([100, 0, 5, 40], [80, 7, 0, 60])]:
    T.append({"op": "psiCounts", "a": e, "b": a, "opts": {}, "r": r14(psi_counts(e, a, 1e-4))})
for bins in [10, 4]:
    e, a = np.round(rng.normal(0, 1, 500), 5), np.round(rng.normal(0.3, 1.2, 300), 5)
    s = np.sort(e)
    edges = [s[min(len(s) - 1, ((k + 1) * len(s)) // bins)] for k in range(bins - 1)]
    he = np.bincount(np.searchsorted(edges, e, side="right"), minlength=bins)
    ha = np.bincount(np.searchsorted(edges, a, side="right"), minlength=bins)
    T.append({"op": "psi", "a": arr(e), "b": arr(a), "opts": {"bins": bins}, "r": r14(psi_counts(list(he), list(ha), 1e-4))})
out["tests"] = T
out["samples"] = POOL
print("// generated by tests/ref/gen_stats_ref_vectors.py; do not edit")
print("export default " + json.dumps(out, separators=(",", ":")).replace('"NaN"', "NaN").replace('"Infinity"', "Infinity")
      .replace('"-Infinity"', "-Infinity") + ";")
