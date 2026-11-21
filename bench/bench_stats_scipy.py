#!/usr/bin/env python3
"""scipy timings for bench/bench_stats.js on the SAME samples (the raw float64 file that script's 2nd argument writes).

    ./dynajs bench/bench_stats.js 5 stats.f64
    python3 bench/bench_stats_scipy.py stats.f64 3
"""
import sys
import time

import numpy as np
import scipy
from scipy import stats as st

raw = np.fromfile(sys.argv[1], dtype="<f8")
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
sizes = [1000000, 1000000, 200000, 200000, 100000, 100000, 5000, 5000, 100000, 10000]
A, B, M1, M2, K1, K2, E1, E2, P, BS = np.split(raw, np.cumsum(sizes)[:-1])
BA = np.abs(B)
TAB = [[10 + ((i * 31 + j * 17) % 23) for j in range(50)] for i in range(50)]


def best(f):
    b = float("inf")
    for _ in range(runs):
        t = time.perf_counter()
        f()
        b = min(b, time.perf_counter() - t)
    return b * 1000


rows = [
    ("normCdf x1e6", lambda: st.norm.cdf(A)),
    ("normPpf x1e5", lambda: st.norm.ppf(P)),
    ("tSf df=12 x1e6", lambda: st.t.sf(A, 12)),
    ("chi2Sf k=5 x1e6", lambda: st.chi2.sf(BA, 5)),
    ("ttestInd Welch 1e6 vs 1e6", lambda: st.ttest_ind(A, B, equal_var=False)),
    ("mannWhitneyU 2e5 vs 2e5", lambda: st.mannwhitneyu(M1, M2)),
    ("ks2Samp asymp 1e5 vs 1e5", lambda: st.ks_2samp(K1, K2)),
    ("ks2Samp exact 5000 vs 5000", lambda: st.ks_2samp(E1, E2)),
    ("pearson 1e6", lambda: st.pearsonr(A, B)),
    ("spearman 1e6", lambda: st.spearmanr(A, B)),
    ("rankData 1e6", lambda: st.rankdata(A)),
    ("pAdjust bh 1e5", lambda: st.false_discovery_control(P, method="bh")),
    ("chi2Contingency 50x50", lambda: st.chi2_contingency(TAB)),
    ("bootstrap median n=1e4 R=2000", lambda: st.bootstrap((BS,), np.median, n_resamples=2000, method="percentile", random_state=0)),
]
print("scipy %s numpy %s: best of %d" % (scipy.__version__, np.__version__, runs))
for name, f in rows:
    print("%-32s%10.2f ms" % (name, best(f)))
