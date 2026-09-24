#!/usr/bin/env python3
"""gen_linreg_oracle.py -- emits the golden blocks for tests/test_ml_linreg_exact.js.

TWO blocks, both generated here so the test never re-derives its own goldens:

  1. SYSTEMS -- the forced-optimum battery. Every system is EXACTLY solvable by
     construction (integer design, integer coefficients, integer targets), so
     the OLS solution IS c* in real arithmetic and any deviation is solver
     rounding. numpy.linalg.lstsq (SVD) is the independent oracle.

  2. BAND -- the numerically-singular band: designs that are exactly singular
     (a constant column, a column that is a multiple of another, duplicate
     columns/rows, a zero column, x3 = x1 + k*x2) across dyadic AND non-dyadic
     collinearity ratios. Their coefficients are NOT unique, so only
     IDENTIFIABLE quantities are pinned: the prediction at a probe row whose
     direction lies in the design, which numpy.linalg.lstsq also computes (the
     residual is exactly zero for every row here, so that prediction is unique
     however the minimiser splits the null space). `oracleCoef` is the
     minimum-norm SVD solution: the solver's coefficients must not AMPLIFY
     past it, and its identifiable prediction must MATCH it.

  3. NEAR -- the conditioning gate's battery: WELL-POSED near-singular designs
     (x2 = 3*x1 + d*z) straddling the 1000*eps gate with null-heavy and smooth
     targets, plus exactly-singular MIXED-UNIT designs (a constant column at
     1/1e4/1e6 beside unit-scale features). Every row carries the ELIMINATION
     PIVOT RATIO the C solver's gate judges -- computed here by replicating
     dyn_solve op-for-op (build_ata + pivot_ratio below, same accumulation and
     elimination order) -- and the side of the gate the row must land on
     (`side`: "exact" above the cut, "ridge" below it), so the test asserts
     the routing, not just an error bound.

Usage: python3 gen_linreg_oracle.py > golden.js   (--report: values to stderr)
"""
import json
import math
import sys

import numpy as np

rng = np.random.default_rng(12345)

EPS = 2.220446049250313e-16
GATE = 1000.0 * EPS  # DYN_SOLVE_MIN_PIVOT in src/dyna-ml.c (~2.2e-13 relative)


# ------------------------------------------------- the C solver's pivot ratio
def build_ata(X, w=None):
    """AtA for [X | 1], accumulated in the dense path's exact op order
    (rows outer, upper triangle inner) so the rounding matches the C."""
    rows = len(X)
    cols = len(X[0])
    p = cols + 1
    AtA = [0.0] * (p * p)
    for i in range(rows):
        xi = X[i]
        wi = 1.0 if w is None else w[i]
        for a in range(cols):
            va = wi * xi[a]
            for b in range(a, cols):
                AtA[a * p + b] += va * xi[b]
            AtA[a * p + cols] += va
        AtA[cols * p + cols] += wi
    return AtA


def pivot_ratio(AtA):
    """Replicate dyn_solve's elimination (partial pivoting, strict > swap,
    same op order) and return min_pivot / max-diagonal. This is the ratio the
    C conditioning gate judges."""
    A = list(AtA)
    p = int(round(math.isqrt(len(A))))
    for a in range(p):
        for b in range(a + 1, p):
            A[b * p + a] = A[a * p + b]
    ascale = 0.0
    for a in range(p):
        if A[a * p + a] > ascale:
            ascale = A[a * p + a]
    min_pivot = math.inf
    for col in range(p):
        maxv = abs(A[col * p + col])
        piv = col
        for r in range(col + 1, p):
            v = abs(A[r * p + col])
            if v > maxv:
                maxv = v
                piv = r
        if maxv <= 0.0 or not math.isfinite(maxv):
            return 0.0
        min_pivot = min(min_pivot, maxv)
        if piv != col:
            for c in range(p):
                A[col * p + c], A[piv * p + c] = A[piv * p + c], A[col * p + c]
        fc = A[col * p + col]
        for r in range(col + 1, p):
            f = A[r * p + col] / fc
            for c in range(col, p):
                A[r * p + c] -= f * A[col * p + c]
    return min_pivot / ascale


def lstsq(X, y, probe, w=None):
    """Independently solve the (optionally weighted) system with SVD.

    The weights are normalized by their maximum before the solve: they are
    finite positives, scaling them all by a constant leaves the weighted
    least-squares solution unchanged, and a 1e300 scale inside the SVD would
    otherwise square into an overflow and destroy the oracle."""
    X = np.asarray(X, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    A = np.hstack([X, np.ones((X.shape[0], 1))])
    if w is not None:
        wv = np.asarray(w, dtype=np.float64)
        wv = wv / wv.max()
        sw = np.sqrt(wv)
        Aw = A * sw[:, None]
        yw = y * sw
    else:
        Aw, yw = A, y
    coef, *_ = np.linalg.lstsq(Aw, yw, rcond=None)
    resid = float(np.sum((Aw @ coef - yw) ** 2))
    pred = float(np.asarray(list(probe) + [1.0]) @ coef)
    return coef, pred, resid


# ---------------------------------------------------------------- block 1
systems = []


def add(name, X, c, intercept, note):
    X = np.asarray(X, dtype=np.int64)
    c = np.asarray(c, dtype=np.int64)
    y = X @ c + intercept
    A = np.hstack([X, np.ones((X.shape[0], 1), dtype=np.int64)])
    coef, *_ = np.linalg.lstsq(A.astype(np.float64), y.astype(np.float64),
                               rcond=None)
    systems.append((name, X, y, [int(v) for v in c] + [int(intercept)],
                    [float(v) for v in coef], note))


add("A", [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 1]], [2, -1, 3], 1,
    "4x4 identity block")
add("B", [[1, 2, 3], [4, 5, 6], [7, 8, 10], [1, 1, 1]], [1, -2, 0], 1,
    "4x4 dense")
add("C", [[2, 0, 0], [0, 3, 0], [0, 0, 4], [1, 1, 1]], [6, -6, 2], 1,
    "4x4 scaled columns (integers stay exact)")
add("D", [[1000, 0, 0], [0, 1000, 0], [0, 0, 1000], [1, 1, 1]], [1, 2, 3], 1,
    "features x 1e3")
add("E", [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1000, 1000, 1000]], [1, 2, 3], 1,
    "features x 1e-3 equivalent (one large row)")
Xf = rng.integers(-20, 21, size=(10, 3))
add("F", Xf, [1, -2, 3], 5, "10x3 seeded random integers")
Xg = rng.integers(-50, 51, size=(25, 4))
add("G", Xg, [3, 1, -4, 2], -7, "25x4 seeded random integers")

# ---------------------------------------------------------------- block 2
band = []


def band_row(name, note, X, y, probe, w=None, exact=True):
    """One numerically-singular system: the SVD oracle fixes its identifiable
    prediction (unique -- for the `exact` rows the residual is zero) and the
    minimum-norm coefficients the solver must not amplify past. `exact=False`
    marks a singular design that is NOT realizable (a zero design has no model
    to recover): its least-squares prediction is still unique."""
    coef, pred, resid = lstsq(X, y, probe, w)
    if exact:
        scale = max(1.0, float(np.sum(np.asarray(y, dtype=np.float64) ** 2)))
        assert resid <= 1e-12 * scale, \
            "band system %s is not exactly solvable (residual %g)" % (name, resid)
    band.append((name, note, X, y, probe, w,
                 [float(v) for v in coef], pred, exact))


# family 1: a CONSTANT column (x2 = k*1), y = 2*x1 -- pred at [4, k] is 8
for k in (1, 2, 4, 8, 0.5, 0.25):
    band_row("constcol k=%g (dyadic)" % k, "constant column, dyadic ratio",
             [[1, k], [2, k], [3, k]], [2, 4, 6], [4, k])
for k in (3, 5, 6, 7, 9, 10, 11, 13, 100, 0.3, 0.1):
    band_row("constcol k=%g" % k, "constant column, non-dyadic ratio",
             [[1, k], [2, k], [3, k]], [2, 4, 6], [4, k])
# family 2: x2 = k*x1 (through the origin), y = x1 -- pred at [2, 2k] is 2
for k in (1, 2, 4, 0.5, 0.25):
    band_row("colin k=%g (dyadic)" % k, "collinear through origin, dyadic",
             [[1, k], [2, 2 * k], [3, 3 * k]], [1, 2, 3], [2, 2 * k])
for k in (3, 5, 6, 7, 9, 10, 11, 100, 1.5):
    band_row("colin k=%g" % k, "collinear through origin, non-dyadic",
             [[1, k], [2, 2 * k], [3, 3 * k]], [1, 2, 3], [2, 2 * k])
# family 3: duplicate columns (and a duplicated pair beside a constant column)
band_row("dup cols", "one column twice",
         [[1, 1], [2, 2], [3, 3]], [1, 2, 3], [4, 4])
band_row("dup cols + const", "one column twice beside a constant column",
         [[1, 1, 7], [2, 2, 7], [3, 3, 7]], [1, 2, 3], [4, 4, 7])
band_row("dup cols k=5", "a duplicated pair with a non-dyadic ratio",
         [[1, 5], [2, 10], [3, 15]], [2, 4, 6], [4, 20])
# family 4: identical rows (rank below the parameter count)
band_row("identical rows", "all three rows identical",
         [[1, 5], [1, 5], [1, 5]], [2, 2, 2], [1, 5])
band_row("dup row + 1", "one row twice, two distinct rows in all",
         [[1, 1], [1, 1], [2, 3]], [2, 2, 5], [1, 1])
# family 5: a zero column
band_row("zero col", "a column of zeros",
         [[1, 0], [2, 0], [3, 0]], [2, 4, 6], [4, 0])
band_row("zero col + dup", "zero column beside a duplicated column",
         [[1, 0, 1], [2, 0, 2], [3, 0, 3]], [2, 4, 6], [4, 0, 4])
# family 6: x3 = x1 + k*x2 (a three-column dependence), y = x1 + x2. The probe
# row satisfies the same relation -- x3 = x1 + k*x2 = 1 + 2k -- so its
# prediction is the identifiable y = 1 + 2 = 3 for every ratio.
for k in (1, 2, 3, 5):
    band_row("x3=x1+%g*x2" % k, "three-column linear dependence",
             [[1, 1, 1 + k], [2, 3, 2 + 3 * k], [3, 2, 3 + 2 * k],
              [4, 4, 4 + 4 * k]],
             [2, 5, 5, 8], [1, 2, 1 + 2 * k])
# weighted variants
band_row("constcol k=5 w ones", "canonical band row, all-ones weights",
         [[1, 5], [2, 5], [3, 5]], [2, 4, 6], [4, 5], [1, 1, 1])
band_row("constcol k=5 w 1:2:3", "canonical band row, unequal weights",
         [[1, 5], [2, 5], [3, 5]], [2, 4, 6], [4, 5], [1, 2, 3])
band_row("colin k=3 w", "collinear, unequal weights",
         [[1, 3], [2, 6], [3, 9]], [1, 2, 3], [2, 6], [0.5, 1, 2])
band_row("dup cols w 1e300", "canonical duplicate columns, huge weights",
         [[1, 1], [2, 2], [3, 3]], [1, 2, 3], [4, 4], [1e300] * 3)
band_row("dup cols w 1e-320", "duplicate columns, subnormal weights",
         [[1, 1], [2, 2], [3, 3]], [1, 2, 3], [4, 4], [1e-320] * 3)
band_row("zero col w 1e-320", "zero column, subnormal weights (ridge underflow)",
         [[0], [0]], [1, 2], [0], [1e-320, 1e-320], exact=False)

# ---------------------------------------------------------------- block 3
# NEAR -- well-posed but near-singular designs, and mixed-unit scales. These
# rows pin the CONDITIONING GATE: a design whose elimination pivot clears
# 1000*eps (~2.2e-13) of A^T A's largest diagonal is solved UN-RIDGED, and its
# coefficients are the exact OLS answer to rounding; a design below the cut
# (genuine rank deficiency -- including every exactly-singular design, whose
# elimination leaves either an exact zero pivot or a residue <= ~1e-16
# relative) goes to the ridge fallback, where only identifiable quantities are
# pinned. The gate side of every row below is COMPUTED here by replicating the
# C elimination op-for-op (pivot_ratio above), so the test asserts the side
# the solver must take, not just an error bound.
near = []

x1 = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]
zz = [1.0, -1.0, 1.0, -1.0, 1.0, -1.0, 1.0, -1.0]


def near_row(d, target):
    """x2 = 3*x1 + d*z (z alternating +-1, so it cannot fold into the
    intercept). The `null` target y = -d*z IS 3*x1 - x2: its true OLS
    coefficients (3, -1, 0) lie along the near-null direction -- exactly what
    a ridge bias would destroy. The `smooth` target y = 2*x1 + 1 is
    realizable with no near-null component. Both are EXACTLY solvable
    (integer design, integer targets), so `want` is the true answer."""
    X = [[v, 3 * v + d * z] for v, z in zip(x1, zz)]
    if target == "null":
        y = [-d * z for z in zz]
        want = [3.0, -1.0, 0.0]
        probe_want = -float(d)          # 3*2.5 - (3*2.5 + d)
    else:
        y = [2 * v + 1 for v in x1]
        want = [2.0, 0.0, 1.0]
        probe_want = 6.0                # 2*2.5 + 1
    probe = [2.5, 3 * 2.5 + d]
    coef, pred, resid = lstsq(X, y, probe)
    assert resid <= 1e-12 * max(1.0, float(np.sum(np.asarray(y) ** 2))), \
        "near system d=%g %s is not exactly solvable" % (d, target)
    ratio = pivot_ratio(build_ata(X))
    side = "ridge" if ratio < GATE else "exact"
    near.append(({"name": "near d=%g %s" % (d, target),
                  "note": "x2=3*x1+d*z, pivot ratio %.3e %s 1000*eps: %s" %
                          (ratio, "<" if side == "ridge" else ">=", side),
                  "X": X, "y": y, "probe": probe,
                  "want": want, "oracle": [float(v) for v in coef],
                  "oraclePred": pred, "probeWant": probe_want,
                  "ratio": ratio, "side": side, "singular": False}))


# the ladder: ratios 2.0e-7 .. 2.0e-19, straddling BOTH the old eps/DYN_RIDGE
# gate (2.2e-7: d=1e3 sits just below it) and the 1000*eps gate (d=5e5 is the
# last row above it, d=1e6 the first below it)
for d in (1e3, 1e4, 1e5, 5e5):
    near_row(d, "null")
    near_row(d, "smooth")
for d in (1e6, 2e6, 1e9):
    near_row(d, "null")
    near_row(d, "smooth")

# MIXED-UNIT: a constant column at magnitude 1e0/1e4/1e6 beside unit-scale
# features (two units in one design). All three are EXACTLY singular (the
# constant column is collinear with the intercept: the elimination leaves an
# exact zero pivot at every magnitude), so every positive gate routes them to
# the ridge -- and the ridge load must be blind to the design's internal
# scales for the unit-scale direction to survive. `want` is (2, 0, 5).
mixed = []
for M in (1e0, 1e4, 1e6):
    X = [[1, M], [2, M], [3, M], [4, M]]
    y = [7, 9, 11, 13]
    coef, pred, resid = lstsq(X, y, [2.5, M])
    assert resid <= 1e-12 * float(np.sum(np.asarray(y, dtype=float) ** 2))
    ratio = pivot_ratio(build_ata(X))
    side = "ridge" if ratio < GATE else "exact"
    mixed.append(({"name": "mixed-unit M=%g" % M,
                   "note": "const column at %g beside unit features, "
                           "pivot ratio %.3e: %s" % (M, ratio, side),
                   "X": X, "y": y, "probe": [2.5, M],
                   "want": [2.0, 0.0, 5.0],
                   "oracle": [float(v) for v in coef],
                   "oraclePred": pred, "probeWant": 10.0,
                   "ratio": ratio, "side": side, "singular": True}))

print("/* Generated by tests/agent/t2d_ml_df_time/gen_linreg_oracle.py")
print(" * (numpy %s, numpy.linalg.lstsq / SVD). Every system is exactly" %
      np.__version__)
print(" * solvable: integer design, integer coefficients, integer targets, so")
print(" * the OLS solution IS `want` in real arithmetic and every deviation is")
print(" * solver rounding. `oracle` is the independent SVD solve.")
print(" * GOLDEN-START */")
print("const SYSTEMS = [")
for name, X, y, want, oracle, note in systems:
    print("    { name: \"%s\", note: %s," % (name, json.dumps(note)))
    print("      X: %s," % json.dumps(X.tolist()))
    print("      y: %s," % json.dumps(y.tolist()))
    print("      want: %s," % json.dumps(want))
    print("      oracle: %s }," % json.dumps(oracle))
print("];")
print("/* BAND-GOLDEN-START */")
print("const BAND = [")
for name, note, X, y, probe, w, coef, pred, realizable in band:
    print("    { name: \"%s\", note: %s," % (name, json.dumps(note)))
    print("      X: %s," % json.dumps(X))
    print("      y: %s, probe: %s," % (json.dumps(y), json.dumps(probe)))
    print("      w: %s, realizable: %s," % (json.dumps(w),
                                            "true" if realizable else "false"))
    print("      oracleCoef: %s," % json.dumps(coef))
    print("      oraclePred: %s }," % json.dumps(pred))
print("];")
print("/* BAND-GOLDEN-END */")
print("/* NEAR-GOLDEN-START */")
print("const NEAR = [")
for row in near + mixed:
    print("    { name: \"%s\", note: %s," % (row["name"], json.dumps(row["note"])))
    print("      X: %s," % json.dumps(row["X"]))
    print("      y: %s, probe: %s," % (json.dumps(row["y"]),
                                       json.dumps(row["probe"])))
    print("      want: %s, oracle: %s," % (json.dumps(row["want"]),
                                           json.dumps(row["oracle"])))
    print("      probeWant: %s, oraclePred: %s," %
          (json.dumps(row["probeWant"]), json.dumps(row["oraclePred"])))
    print("      ratio: %s, side: \"%s\", singular: %s }," %
          (json.dumps(row["ratio"]), row["side"],
           "true" if row["singular"] else "false"))
print("];")
print("/* NEAR-GOLDEN-END */")
print("/* GOLDEN-END */")

if "--report" in sys.argv:
    print("rows: systems=%d band=%d near=%d" %
          (len(systems), len(band), len(near) + len(mixed)), file=sys.stderr)
    for row in near + mixed:
        print("%-28s ratio=%.4e side=%-5s probeWant=%g" %
              (row["name"], row["ratio"], row["side"], row["probeWant"]),
              file=sys.stderr)
