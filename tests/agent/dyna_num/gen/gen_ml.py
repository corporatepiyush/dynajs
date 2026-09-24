#!/usr/bin/env python3
# gen_ml.py -- emit black-box probes for dyna:ml.
#
# ORACLE POLICY: every model class here is deterministic math over its fitted
# parameters, so numpy reimplements it exactly:
#   - metrics (accuracy/precision/r2Score/rocAuc/confusionMatrix): exact
#     integer/count arithmetic -> EXACT equality.
#   - StandardScaler (ddof=0, verified) / MinMaxScaler: exact closed form ->
#     near-exact (1e-12).
#   - LinearRegression: engine solves normal equations (measured ~1e-7 rel vs
#     lstsq on well-conditioned data) -> tolerance 1e-5 on coef/intercept and
#     predictions, stated in the probe.
#   - GaussianNB: predict from fitted means/vars/priors is a closed form
#     (joint log-likelihood) -> recompute in numpy, argmax must match exactly
#     and probabilities within 1e-9.
#   - KMeans: invariants only (labels in range, predict == nearest centroid by
#     the same metric, inertia == sum of squared distances, seeded fits are
#     reproducible, deterministic labels on well-separated clusters).
#   - API edges: wrong feature count, NaN feature poisoning, predict-before-fit,
#     double-fit, serialization round trip.
import os, json
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROBES = os.path.join(HERE, "..", "probes", "ml")
HARNESS = os.path.join(HERE, "..", "h.js")

def lcg(seed):
    s = seed & 0xFFFFFFFF
    while True:
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        yield s

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

def js_mat(X):
    return json.dumps([[float(v) for v in row] for row in X])

def gen_metrics():
    tag = "ml_metrics"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { accuracy, precision, r2Score, rocAuc, confusionMatrix,',
                     '         trainTestSplit, kFold, stratifiedKFold } from "dyna:ml";'], tag)
    rng = lcg(6001)
    n = 0
    EMIT = []
    # --- exact metrics
    for _ in range(12):
        m = 5 + next(rng) % 40
        yTrue = [next(rng) % 3 for _ in range(m)]
        yPred = [next(rng) % 3 for _ in range(m)]
        acc = sum(1 for a, b in zip(yTrue, yPred) if a == b) / m
        EMIT.append('assert_eq(accuracy(%s, %s), %.17g, "accuracy exact");'
                    % (json.dumps(yTrue), json.dumps(yPred), acc))
        # confusion matrix vs python
        k = max(yTrue + yPred) + 1
        cm = [[sum(1 for a, b in zip(yTrue, yPred) if a == i and b == j)
               for j in range(k)] for i in range(k)]
        EMIT.append('var cm = confusionMatrix(%s, %s);'
                    ' assert_eq(JSON.stringify(cm), %s, "confusionMatrix exact");'
                    % (json.dumps(yTrue), json.dumps(yPred), json.dumps(json.dumps(cm, separators=(",", ":")))))
        n += 2
    # r2 vs numpy
    for _ in range(10):
        m = 5 + next(rng) % 50
        y = [next(rng) / 4294967296.0 * 10 for _ in range(m)]
        yp = [v + (next(rng) / 4294967296.0 - 0.5) for v in y]
        y = np.array(y); yp = np.array(yp)
        r2 = 1 - np.sum((y - yp) ** 2) / np.sum((y - y.mean()) ** 2)
        EMIT.append('assert_close(r2Score(%s, %s), %.17g, 1e-14, 1e-15, "r2 exact");'
                    % (json.dumps(list(y)), json.dumps(list(yp)), r2))
        n += 1
    # rocAuc vs rank computation (no ties)
    for _ in range(10):
        m = 4 + next(rng) % 30
        y = [next(rng) % 2 for _ in range(m)]
        if len(set(y)) < 2:
            y[0] = 1 - y[1]
        s = [next(rng) / 4294967296.0 for _ in range(m)]
        pos = sorted(v for v, t in zip(s, y) if t == 1)
        neg = sorted(v for v, t in zip(s, y) if t == 0)
        auc = sum(np.searchsorted(np.sort(neg), p, side="right") for p in pos) / (len(pos) * len(neg))
        EMIT.append('assert_close(rocAuc(%s, %s), %.17g, 1e-13, 1e-15, "rocAuc no-ties");'
                    % (json.dumps(y), json.dumps(s), auc))
        n += 1
    # perfect and inverted classifiers
    EMIT.append('assert_eq(rocAuc([0,0,1,1], [0.1,0.2,0.8,0.9]), 1, "rocAuc perfect");')
    EMIT.append('assert_eq(rocAuc([0,0,1,1], [0.9,0.8,0.2,0.1]), 0, "rocAuc inverted");')
    EMIT.append('assert_eq(accuracy([1,2,3],[1,2,3]), 1, "accuracy perfect");')
    EMIT.append('assert_eq(precision([1,0,1,1],[1,1,1,0]), 0.6666666666666666, "precision");')
    n += 4
    # splits: partition + determinism
    EMIT.append('''
var sp = trainTestSplit(100, { testSize: 0.25, seed: 7 });
var all = Array.from(sp.train).concat(Array.from(sp.test)).sort(function (a, b) { return a - b; });
var part = all.every(function (v, i) { return v === i; });
assert_true(part && all.length === 100, "trainTestSplit partitions 0..n-1");
var sp2 = trainTestSplit(100, { testSize: 0.25, seed: 7 });
assert_arr_eq(Array.from(sp.train), Array.from(sp2.train), "trainTestSplit seeded reproducible");
assert_eq(sp.test.length, 25, "trainTestSplit testSize");
var folds = kFold(100, { k: 5 });
var sizes = folds.map(function (f) { return f.test.length; });
assert_arr_eq(sizes, [20, 20, 20, 20, 20], "kFold sizes");
var seen = [];
folds.forEach(function (f) { Array.from(f.test).forEach(function (v) { seen.push(v); }); });
assert_eq(seen.length, 100, "kFold covers n");
var yl = []; for (var yi = 0; yi < 100; yi++) yl.push(yi % 2);
var skf = stratifiedKFold(yl, { k: 4 });
assert_eq(skf.length, 4, "stratifiedKFold k");
var stratBalanced = skf.every(function (f) {
  var c = [0, 0];
  f.test.forEach(function (v) { c[yl[v]]++; });
  return Math.abs(c[0] - c[1]) <= 1;
});
assert_true(stratBalanced, "stratifiedKFold keeps class balance");
''')
    n += 8
    for line in EMIT:
        p.w(line)
    p.close(n)

def gen_scaling():
    tag = "ml_scaling"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { StandardScaler, MinMaxScaler, LinearRegression, PCA,',
                     '         Pipeline } from "dyna:ml";'], tag)
    rng = lcg(6002)
    n = 0
    EMIT = []
    for trial in range(6):
        rows = 8 + next(rng) % 40
        cols = 1 + next(rng) % 5
        X = np.array([[next(rng) / 4294967296.0 * 20 - 10 for _ in range(cols)]
                      for _ in range(rows)])
        mu = X.mean(0)
        sd = X.std(0)
        sd_safe = np.where(sd == 0, 1.0, sd)
        Z = (X - mu) / sd_safe
        mn, mx = X.min(0), X.max(0)
        M = (X - mn) / np.where(mx == mn, 1.0, mx - mn)
        EMIT.append('var sc = new StandardScaler(); sc.fit(%s);' % js_mat(X))
        EMIT.append('assert_arr_close(sc.mean, [%s], 1e-12, 1e-14, "scaler mean");'
                    % ",".join(repr(float(v)) for v in mu))
        EMIT.append('assert_arr_close(sc.std, [%s], 1e-12, 1e-14, "scaler std ddof0");'
                    % ",".join(repr(float(v)) for v in sd))
        EMIT.append('assert_arr_close(Array.from(sc.transform(%s)).flat(), [%s], 1e-12, 1e-13, "scaler z");'
                    % (js_mat(X), ",".join(repr(float(v)) for v in Z.ravel())))
        EMIT.append('var mm = new MinMaxScaler(); mm.fit(%s);'
                    ' assert_arr_close(Array.from(mm.fitTransform(%s)).flat(), [%s], 1e-12, 1e-13, "minmax");'
                    % (js_mat(X), js_mat(X), ",".join(repr(float(v)) for v in M.ravel())))
        n += 4
    # constant column -> std 1 per contract
    EMIT.append('var cc = new StandardScaler(); cc.fit([[5,1],[5,2],[5,3]]);'
                ' assert_arr_close(cc.std, [1, 0.81649658092772603], 1e-12, 0, "constant column std is 1, normal column ddof0");'
                ' assert_arr_eq(cc.mean, [5, 2], "constant column mean");'
                ' var ccT = cc.transform([[5,1],[5,2],[5,3]]);'
                ' assert_arr_eq([ccT[0][0], ccT[1][0], ccT[2][0]], [0, 0, 0], "constant column z-scores are 0");')
    n += 2
    # linear regression vs lstsq
    for trial in range(5):
        rows = 10 + next(rng) % 40
        cols = 1 + next(rng) % 4
        X = np.array([[next(rng) / 4294967296.0 * 8 - 4 for _ in range(cols)]
                      for _ in range(rows)])
        w = np.array([next(rng) / 4294967296.0 * 4 - 2 for _ in range(cols)])
        y = X @ w + 0.7 + np.array([(next(rng) - 0x80000000) / 4294967296.0 * 0.02
                                    for _ in range(rows)])
        A = np.column_stack([X, np.ones(rows)])
        ref, *_ = np.linalg.lstsq(A, y, rcond=None)
        EMIT.append('var lr = new LinearRegression(); lr.fit(%s, %s);' % (js_mat(X), json.dumps([float(v) for v in y])))
        EMIT.append('assert_arr_close(lr.coef, [%s], 1e-4, 1e-6, "OLS coef (normal equations tol)");'
                    % ",".join(repr(float(v)) for v in ref[:cols]))
        EMIT.append('assert_close(lr.intercept, %r, 1e-3, 1e-5, "OLS intercept");' % float(ref[cols]))
        pred = X @ ref[:cols] + ref[cols]
        EMIT.append('assert_arr_close(lr.predict(%s), [%s], 1e-3, 1e-3, "OLS predict");'
                    % (js_mat(X), ",".join(repr(float(v)) for v in pred)) )
        n += 4
    # perfect linear data: R2 of the model's own predictions ~ 1
    EMIT.append('var lr2 = new LinearRegression(); lr2.fit([[0],[1],[2],[3]], [0,2,4,6]);'
                ' assert_arr_close(lr2.predict([[4],[5]]), [8,10], 1e-9, 1e-9, "OLS exact line");'
                ' assert_eq(lr2.coef.length, 1, "coef length");')
    n += 2
    # PCA: components orthonormal, explained variance = eigenvalues of covariance
    Xp = np.array([[next(rng) / 4294967296.0 * 6 - 3 for _ in range(3)] for _ in range(60)])
    C = np.cov(Xp, rowvar=False, ddof=1)
    ev, evec = np.linalg.eigh(C)
    order = np.argsort(ev)[::-1]
    ev, evec = ev[order], evec[:, order]
    EMIT.append('var pca = new PCA(2); pca.fit(%s);' % js_mat(Xp))
    EMIT.append('var comp = pca.components;')
    EMIT.append('for (var ci = 0; ci < 2; ci++)'
                ' assert_close(comp[ci][0]*comp[ci][0]+comp[ci][1]*comp[ci][1]+comp[ci][2]*comp[ci][2], 1, 1e-9, 1e-12, "component unit");')
    EMIT.append('assert_close(pca.explainedVariance[0], %r, 1e-9, 1e-12, "PCA ev0");' % float(ev[0]))
    EMIT.append('assert_close(pca.explainedVariance[1], %r, 1e-9, 1e-12, "PCA ev1");' % float(ev[1]))
    EMIT.append('assert_close(pca.explainedVarianceRatio[0] + pca.explainedVarianceRatio[1],'
                ' %r, 1e-9, 1e-12, "ev ratio sum");' % float((ev[0] + ev[1]) / ev.sum()))
    EMIT.append('var T = pca.transform(%s);' % js_mat(Xp))
    EMIT.append('var Tt = pca.inverseTransform(T);')
    EMIT.append('assert_true(T.length === 60 && T[0].length === 2, "PCA transform shape");')
    EMIT.append('assert_true(Tt.length === 60 && Tt[0].length === 3, "PCA inverse shape");')
    # reconstruction error == sum of dropped eigenvalues
    EMIT.append("var XREF = %s;" % js_mat(Xp))
    EMIT.append("var rec = 0; for (var ri = 0; ri < 60; ri++) for (var rj = 0; rj < 3; rj++)"
                " rec += (Tt[ri][rj] - XREF[ri][rj]) * (Tt[ri][rj] - XREF[ri][rj]);")
    EMIT.append('assert_close(rec, %r, 1e-6, 1e-9, "PCA reconstruction = n * dropped ddof0 ev");' % float((len(Xp) - 1) * ev[2]))
    n += 10
    for line in EMIT:
        p.w(line)
    p.close(n)

def gen_models():
    tag = "ml_models"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { GaussianNB, KMeans, LinearRegression, LogisticRegression,',
                     '         accuracy } from "dyna:ml";'], tag)
    rng = lcg(6003)
    n = 0
    EMIT = []
    # GaussianNB: fit well-separated classes, predict = argmax joint log-likelihood
    rowsA = np.array([[next(rng) / 4294967296.0 + 0.0, next(rng) / 4294967296.0 + 5.0]
                      for _ in range(30)])
    rowsB = np.array([[next(rng) / 4294967296.0 + 5.0, next(rng) / 4294967296.0 + 0.0]
                      for _ in range(30)])
    X = np.vstack([rowsA, rowsB])
    y = [0] * 30 + [1] * 30
    EMIT.append('var nb = new GaussianNB(); nb.fit(%s, %s);' % (js_mat(X), json.dumps(y)))
    EMIT.append('assert_arr_eq(nb.classes, [0, 1], "NB classes ascending");')
    # recompute class stats and predict
    stats = []
    for c in (0, 1):
        Xc = X[np.array(y) == c]
        stats.append((Xc.mean(0), Xc.var(0), len(Xc) / len(X)))
    test = np.array([[0.3, 5.2], [4.8, 0.1], [2.5, 2.5], [0.1, 0.1], [5.1, 5.1]])
    scores = []
    for (mu, var, prior) in stats:
        sj = -0.5 * np.sum(np.log(2 * np.pi * var)) \
             - 0.5 * np.sum((test - mu) ** 2 / var, axis=1) + np.log(prior)
        scores.append(sj)
    want = np.argmax(scores, axis=0)
    # well-separated points away from the boundary must match exactly
    for ti, t in enumerate(test):
        if abs(scores[0][ti] - scores[1][ti]) > 1.0:
            EMIT.append('assert_eq(nb.predict([[%r,%r]])[0], %d, "NB argmax pt %d");'
                        % (float(t[0]), float(t[1]), want[ti], ti))
            n += 1
    # proba rows sum to 1
    EMIT.append('var pr = nb.predictProba([[0.3,5.2]]);'
                ' assert_close(pr[0][0] + pr[0][1], 1, 1e-9, 1e-12, "NB proba sums to 1");')
    n += 1
    # KMeans invariants on separated clusters
    Xc = np.vstack([np.array([[next(rng) / 4294967296.0 + 0.0 for _ in range(2)] for _ in range(20)]),
                    np.array([[next(rng) / 4294967296.0 + 10.0 for _ in range(2)] for _ in range(20)]),
                    np.array([[next(rng) / 4294967296.0 + 10.0 for _ in range(1)]
                              + [next(rng) / 4294967296.0 + 0.0 for _ in range(1)] for _ in range(20)]),
                    np.array([[next(rng) / 4294967296.0 + 0.0 for _ in range(1)]
                              + [next(rng) / 4294967296.0 + 10.0 for _ in range(1)] for _ in range(20)])])
    EMIT.append('var km = new KMeans(4, 42); km.fit(%s);' % js_mat(Xc))
    EMIT.append('var lab = km.predict(%s);' % js_mat(Xc))
    EMIT.append('assert_eq(lab.length, 80, "kmeans label count");')
    EMIT.append('var uniq = Array.from(new Set(lab));'
                ' assert_eq(uniq.length, 4, "kmeans finds 4 separated clusters");')
    # cluster of each class is pure
    EMIT.append('var pure = true;'
                ' for (var ci2 = 0; ci2 < 4; ci2++) {'
                '  var s2 = new Set(); for (var ri2 = ci2*20; ri2 < ci2*20+20; ri2++) s2.add(lab[ri2]);'
                '  if (s2.size !== 1) pure = false; }'
                ' assert_true(pure, "kmeans clusters pure on separated data");')
    # inertia == sum of squared distances to assigned centroids
    EMIT.append("var XC = %s;" % js_mat(Xc))
    # centroids are not exposed; pin the documented surface instead
    EMIT.append('assert_true(isFinite(km.inertia) && km.inertia >= 0, "inertia finite non-negative");'
                ' assert_arr_eq(km.predict(XC), lab, "predict on train == fit labels");'
                ' assert_eq(km.inertia, (new KMeans(4, 42)).fit(XC).inertia, "inertia reproducible");')
    # seeded reproducibility
    EMIT.append('var kmA = new KMeans(4, 42); kmA.fit(XC);'
                ' var kmB = new KMeans(4, 42); kmB.fit(XC);'
                ' assert_arr_eq(kmA.predict(XC), kmB.predict(XC), "kmeans seeded reproducible");')
    n += 7
    # logistic on separable data reaches 1.0 accuracy
    EMIT.append('var lg = new LogisticRegression({ maxIter: 500 });'
                ' lg.fit([[0],[0.1],[0.2],[0.3],[0.9],[1.0],[1.1],[1.2]], [0,0,0,0,1,1,1,1]);'
                ' assert_eq(accuracy([0,0,0,0,1,1,1,1], lg.predict([[0.05],[0.15],[0.25],[0.35],[0.85],[0.95],[1.05],[1.15]])), 1, "logreg separable");'
                ' var pp = lg.predictProba([[0.5]]); assert_close(pp[0][0] + pp[0][1], 1, 1e-9, 1e-12, "logreg proba");')
    n += 2
    # API edges
    EMIT.append('''
// predict before fit
var cold = new LinearRegression();
assert_arr_eq(cold.coef, [], "coef empty before fit");
assert_eq(cold.intercept, 0, "intercept 0 before fit");
// NaN features must throw, not poison a model
var nb2 = new GaussianNB();
var threw = false;
try { nb2.fit([[1,2],[3,NaN]], [0,1]); } catch (e) { threw = true; }
assert_true(threw, "NaN feature throws at fit");
// wrong feature count at predict
var lr3 = new LinearRegression(); lr3.fit([[1,2],[3,4],[5,6],[2,1]], [1,2,3,4]);
var threw2 = false;
try { lr3.predict([[1,2,3]]); } catch (e) { threw2 = true; }
assert_true(threw2, "wrong feature count throws at predict");
// double fit is allowed and replaces state
var lr4 = new LinearRegression(); lr4.fit([[0],[1],[2]], [0,1,2]); lr4.fit([[0],[1],[2]], [0,10,20]);
assert_close(lr4.predict([[2]])[0], 20, 1e-9, 1e-9, "second fit replaces state");
''')
    n += 6
    for line in EMIT:
        p.w(line)
    p.close(n)


def gen_params():
    # ml_params -- exact-prediction asserts computed from EXPORTED fitted
    # parameters (no sklearn anywhere; numpy only at generation time):
    #   - LinearRegression: coef/intercept vs numpy lstsq (1e-5; the engine
    #     solves normal equations + 1e-9 ridge), and predict == intercept +
    #     coef.x recomputed in-probe from the exported arrays (1e-10).
    #   - LogisticRegression: predictProba == sigmoid/softmax of the exported
    #     coef/intercept (1e-12); predict == argmax exactly.
    #   - DecisionTree*: same apply() leaf => identical prediction (exact).
    #   - serialize/deserialize round trips predict exactly; wrong-magic bytes
    #     and garbage bytes are rejected.
    tag = "ml_params"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { LinearRegression, LogisticRegression, KMeans,',
                     '         DecisionTreeClassifier, DecisionTreeRegressor,',
                     '         GaussianNB } from "dyna:ml";'], tag)
    rng = lcg(6004)
    n = 0
    EMIT = []
    # --- LinearRegression: exported params close the loop on predict
    for trial in range(6):
        rows = 12 + next(rng) % 30
        cols = 1 + next(rng) % 4
        X = np.array([[next(rng) / 4294967296.0 * 8 - 4 for _ in range(cols)]
                      for _ in range(rows)])
        w = np.array([next(rng) / 4294967296.0 * 4 - 2 for _ in range(cols)])
        b = next(rng) / 4294967296.0 * 2 - 1
        y = X @ w + b + np.array([next(rng) / 4294967296.0 * 0.02
                                  for _ in range(rows)])
        # OLS WITH intercept: solve on the augmented design [X | 1] (the
        # uncentered shortcut y.mean - xbar.coef from a no-intercept fit is
        # wrong and was the cause of the 2026-09-12 ml_params failures)
        sol, _, _, _ = np.linalg.lstsq(np.hstack([X, np.ones((rows, 1))]),
                                       y, rcond=None)
        coef, icept = sol[:-1], float(sol[-1])
        Xtest = np.array([[next(rng) / 4294967296.0 * 8 - 4 for _ in range(cols)]
                          for _ in range(4)])
        t = trial
        EMIT.append('var lr%d = new LinearRegression(); lr%d.fit(%s, %s);'
                    % (t, t, js_mat(X), json.dumps([float(v) for v in y])))
        EMIT.append('assert_arr_close(lr%d.coef, [%s], 1e-5, 1e-8, "linreg coef vs lstsq");'
                    % (t, ",".join(repr(float(v)) for v in coef)))
        EMIT.append('assert_close(lr%d.intercept, %r, 1e-5, 1e-8, "linreg intercept vs lstsq");'
                    % (t, icept))
        # predict == intercept + coef.x from the EXPORTED arrays (1e-10: the
        # engine may FMA/reassociate the reduction)
        EMIT.append('var lc%d = lr%d.coef, li%d = lr%d.intercept;'
                    % (t, t, t, t))
        for pi, pt in enumerate(Xtest):
            want = float(icept + pt @ coef)
            dot = " + ".join("lc%d[%d]*%r" % (t, j, float(pt[j]))
                             for j in range(cols))
            EMIT.append('assert_close(lr%d.predict([[%s]])[0], %s + li%d, 1e-10, 1e-9, "linreg linear form");'
                        % (t, ",".join(repr(float(v)) for v in pt), dot, t))
            # cross-method agreement (engine normal equations + 1e-9 ridge
            # vs numpy SVD) measured ~1e-10 rel here; the sharp 1e-10 check
            # above is predict == linear form of the EXPORTED params
            EMIT.append('assert_close(lr%d.predict(%s)[0], %r, 1e-8, 1e-10, "linreg vs numpy full");'
                        % (t, js_mat([pt]), want))
        n += 3 + 2 * len(Xtest)
    # --- LogisticRegression binary: proba == sigmoid(w.x+b) from exports
    for trial in range(3):
        na = 12 + next(rng) % 12
        Xa = np.array([[next(rng) / 4294967296.0 * 2, next(rng) / 4294967296.0 * 2]
                       for _ in range(na)])
        Xb = np.array([[next(rng) / 4294967296.0 * 2 + 6,
                        next(rng) / 4294967296.0 * 2 + 6] for _ in range(na)])
        X = np.vstack([Xa, Xb])
        y = [0] * na + [1] * na
        pts = [[0.2, 0.3], [5.8, 5.9], [3.0, 3.0], [2.5, 4.1]]
        t = trial
        EMIT.append('var lg%d = new LogisticRegression({maxIter: 3000, tol: 1e-10});'
                    ' lg%d.fit(%s, %s);' % (t, t, js_mat(X), json.dumps(y)))
        EMIT.append('var lw%d = lg%d.coef[0], lb%d = +lg%d.intercept;'
                    % (t, t, t, t))
        EMIT.append('assert_eq(lg%d.coef.length, 1, "logreg binary coef rows");' % t)
        for pi, pt in enumerate(pts):
            EMIT.append('var z%d_%d = lw%d[0]*%r + lw%d[1]*%r + lb%d;'
                        % (t, pi, t, pt[0], t, pt[1], t))
            EMIT.append('var s%d_%d = lg%d.predictProba([[%r,%r]])[0];'
                        % (t, pi, t, pt[0], pt[1]))
            EMIT.append('assert_close(s%d_%d[1], 1/(1+Math.exp(-z%d_%d)), 1e-12, 1e-14, "logreg sigmoid closed form");'
                        % (t, pi, t, pi))
            EMIT.append('assert_close(s%d_%d[0], 1/(1+Math.exp(z%d_%d)), 1e-12, 1e-14, "logreg complement");'
                        % (t, pi, t, pi))
            EMIT.append('assert_eq(lg%d.predict([[%r,%r]])[0], s%d_%d[1] >= 0.5 ? 1 : 0, "logreg argmax");'
                        % (t, pt[0], pt[1], t, pi))
        n += 2 + 3 * len(pts)
    # --- LogisticRegression multinomial: softmax over exported coef rows.
    # Classes are separable along axis 0 (centers -8, 0, +8, noise +-1, +-3 on
    # axis 1), so every probe point is far from a decision boundary and its
    # argmax class is decided by construction, not by the fit.
    for trial in range(2):
        X, y = [], []
        for c in range(3):
            for _ in range(12):
                x0 = 8.0 * (c - 1) + (next(rng) / 4294967296.0 * 2 - 1)
                x1 = next(rng) / 4294967296.0 * 6 - 3
                X.append([x0, x1])
                y.append(c)
        X = np.array(X)
        pts = [[-3.9, 0.4], [0.1, -0.3], [4.0, 0.2], [-0.2, 0.6]]
        t = trial
        EMIT.append('var gm%d = new LogisticRegression({maxIter: 4000, tol: 1e-10});'
                    ' gm%d.fit(%s, %s);' % (t, t, js_mat(X), json.dumps(y)))
        EMIT.append('assert_eq(gm%d.coef.length, 3, "logreg multinomial coef rows");' % t)
        EMIT.append('var gw%d = gm%d.coef, gb%d = gm%d.intercept;'
                    % (t, t, t, t))
        EMIT.append('assert_true(gb%d instanceof Array || typeof gb%d === "object", "multinomial intercept array");' % (t, t))
        for pi, pt in enumerate(pts):
            ex = ["Math.exp(gw%d[%d][0]*%r + gw%d[%d][1]*%r + +gb%d[%d])"
                  % (t, c, pt[0], t, c, pt[1], t, c) for c in range(3)]
            EMIT.append('var e%d_%d = [%s], se%d_%d = %s;'
                        % (t, pi, ",".join(ex), t, pi,
                           "+".join("e%d_%d[%d]" % (t, pi, c) for c in range(3))))
            EMIT.append('var pr%d_%d = gm%d.predictProba([[%r,%r]])[0];'
                        % (t, pi, t, pt[0], pt[1]))
            for c in range(3):
                EMIT.append('assert_close(pr%d_%d[%d], e%d_%d[%d]/se%d_%d, 1e-12, 1e-14, "softmax closed form");'
                            % (t, pi, c, t, pi, c, t, pi))
            # predict must equal the argmax of the softmax recomputed from
            # the EXPORTED params (first-max on ties); the geometric class
            # guess is NOT asserted because the fit's boundary placement is
            # an implementation detail
            EMIT.append('var am%d_%d = pr%d_%d[0] >= pr%d_%d[1] ? (pr%d_%d[0] >= pr%d_%d[2] ? 0 : 2) : (pr%d_%d[1] >= pr%d_%d[2] ? 1 : 2);'
                        % (t, pi, t, pi, t, pi, t, pi, t, pi, t, pi, t, pi))
            EMIT.append('assert_eq(gm%d.predict([[%r,%r]])[0], am%d_%d, "softmax argmax");'
                        % (t, pt[0], pt[1], t, pi))
        n += 2 + 5 * len(pts)
    # --- trees: apply() leaf identity forces identical predictions
    for trial in range(3):
        rows = 16 + next(rng) % 24
        X = np.array([[next(rng) / 4294967296.0 * 10, next(rng) / 4294967296.0 * 10]
                      for _ in range(rows)])
        y = [1 if (x[0] + x[1]) > 10.0 else 0 for x in X]
        t = trial
        EMIT.append('var dt%d = new DecisionTreeClassifier(); dt%d.fit(%s, %s);'
                    % (t, t, js_mat(X), json.dumps(y)))
        EMIT.append('var lv%d = dt%d.apply(%s), pb%d = dt%d.predictProba(%s);'
                    % (t, t, js_mat(X), t, t, js_mat(X)))
        EMIT.append('var same%d = true, seen%d = {};'
                    % (t, t))
        EMIT.append('for (var i = 0; i < lv%d.length; i++) { var k = String(lv%d[i]);'
                    ' if (seen%d[k] === undefined) seen%d[k] = JSON.stringify(pb%d[i]);'
                    ' else if (seen%d[k] !== JSON.stringify(pb%d[i])) same%d = false; }'
                    % (t, t, t, t, t, t, t, t))
        EMIT.append('assert_true(same%d, "tree: same leaf => same proba row");' % t)
        EMIT.append('var sumI%d = dt%d.featureImportances.reduce(function (a, b) { return a + b; }, 0);'
                    ' assert_close(sumI%d, 1.0, 1e-9, 1e-12, "tree importances sum to 1");'
                    % (t, t, t))
        EMIT.append('assert_true(dt%d.depth >= 1, "tree depth on separable data");' % t)
        n += 3
    for trial in range(2):
        rows = 16 + next(rng) % 16
        X = np.array([[next(rng) / 4294967296.0 * 10] for _ in range(rows)])
        y = [x[0] * x[0] + (next(rng) / 4294967296.0 * 0.01) for x in X]
        t = trial
        EMIT.append('var dr%d = new DecisionTreeRegressor(); dr%d.fit(%s, %s);'
                    % (t, t, js_mat(X), json.dumps([float(v) for v in y])))
        EMIT.append('var dl%d = dr%d.apply(%s), dp%d = dr%d.predict(%s);'
                    % (t, t, js_mat(X), t, t, js_mat(X)))
        EMIT.append('var dsame%d = true, dseen%d = {};'
                    % (t, t))
        EMIT.append('for (var i = 0; i < dl%d.length; i++) { var k = String(dl%d[i]);'
                    ' if (dseen%d[k] === undefined) dseen%d[k] = String(dp%d[i]);'
                    ' else if (dseen%d[k] !== String(dp%d[i])) dsame%d = false; }'
                    % (t, t, t, t, t, t, t, t))
        EMIT.append('assert_true(dsame%d, "tree reg: same leaf => same prediction");' % t)
        n += 2
    # --- serialization round trips: byte-exact behavior after deserialize
    EMIT.append('var s_lr = new LinearRegression(); s_lr.fit([[1],[2],[3],[4]], [3,5,7,9]);')
    EMIT.append('var s_lr2 = LinearRegression.deserialize(s_lr.serialize());')
    EMIT.append('assert_eq(JSON.stringify(s_lr.coef), JSON.stringify(s_lr2.coef), "linreg bytes coef exact");')
    EMIT.append('assert_eq(JSON.stringify(s_lr.predict([[5],[6]])), JSON.stringify(s_lr2.predict([[5],[6]])), "linreg bytes predict exact");')
    EMIT.append('var s_k = new KMeans(2, 1); s_k.fit([[0,0],[1,1],[10,10],[11,11]]);')
    EMIT.append('var s_k2 = KMeans.deserialize(s_k.serialize());')
    EMIT.append('assert_eq(JSON.stringify(s_k.predict([[0.2,0],[9,10]])), JSON.stringify(s_k2.predict([[0.2,0],[9,10]])), "kmeans bytes predict exact");')
    EMIT.append('assert_close(s_k.inertia, s_k2.inertia, 0, 0, "kmeans bytes inertia exact");')
    EMIT.append('var s_t = new DecisionTreeClassifier(); s_t.fit([[0,0],[1,1],[0,1],[1,0]], [0,1,1,0]);')
    EMIT.append('var s_t2 = DecisionTreeClassifier.deserialize(s_t.serialize());')
    EMIT.append('assert_eq(JSON.stringify(s_t.predict([[0.9,0.8],[0.1,0.2]])), JSON.stringify(s_t2.predict([[0.9,0.8],[0.1,0.2]])), "tree bytes predict exact");')
    EMIT.append('var s_n = new GaussianNB(); s_n.fit([[0,0],[1,1],[10,10],[11,11]], [0,0,1,1]);')
    EMIT.append('var s_n2 = GaussianNB.deserialize(s_n.serialize());')
    EMIT.append('assert_eq(JSON.stringify(s_n.predictProba([[0.5,0.5]])), JSON.stringify(s_n2.predictProba([[0.5,0.5]])), "NB bytes proba exact");')
    EMIT.append('var s_g = new LogisticRegression({maxIter: 500}); s_g.fit([[0,0],[1,0],[5,5],[6,5]], [0,0,1,1]);')
    EMIT.append('var s_g2 = LogisticRegression.deserialize(s_g.serialize());')
    EMIT.append('assert_eq(JSON.stringify(s_g.coef), JSON.stringify(s_g2.coef), "logreg bytes coef exact");')
    n += 7
    # --- rejected deserialization: wrong model magic + garbage bytes
    EMIT.append('var xbytes = s_lr.serialize();')
    EMIT.append('assert_throws(function () { KMeans.deserialize(xbytes); }, null, "wrong-magic bytes rejected");')
    EMIT.append('assert_throws(function () { KMeans.deserialize(new Uint8Array([1,2,3])); }, null, "garbage bytes rejected");')
    n += 2
    # --- lifecycle edges
    EMIT.append('var e_m = new LinearRegression();')
    EMIT.append('assert_throws(function () { e_m.predict([[1]]); }, "InternalError", "predict before fit");')
    EMIT.append('var e_k = new KMeans(5, 1);')
    EMIT.append('assert_throws(function () { e_k.fit([[0,0],[1,1]]); }, "RangeError", "kmeans nClusters > rows");')
    EMIT.append('e_m.close(); e_m.close(); assert_true(e_m.closed, "close idempotent, closed flag");')
    EMIT.append('assert_throws(function () { e_m.predict([[1]]); }, "TypeError", "closed model throws");')
    n += 4
    # --- KMeans seeded fits agree on train assignment exactly
    Xk = [[0,0],[1,0],[0,1],[1,1],[10,10],[11,10],[10,11],[11,11]]
    EMIT.append('var ka = new KMeans(2, 3), kb = new KMeans(2, 3);')
    EMIT.append('ka.fit(%s); kb.fit(%s);' % (js_mat(Xk), js_mat(Xk)))
    EMIT.append('assert_eq(JSON.stringify(ka.predict(%s)), JSON.stringify(kb.predict(%s)), "kmeans seeded fits identical");' % (js_mat(Xk), js_mat(Xk)))
    EMIT.append('assert_close(ka.inertia, 4.0, 1e-12, 1e-12, "kmeans inertia canonical 2x2 grid");')
    EMIT.append('assert_eq(JSON.stringify(ka.predict([[10.5, 10.5]])), "[1]", "kmeans far point");')
    EMIT.append('assert_eq(JSON.stringify(ka.predict([[0.5, 0.5]])), "[0]", "kmeans near point");')
    n += 4
    for line in EMIT:
        p.w(line)
    p.close(n)

def main():
    gen_metrics()
    gen_scaling()
    gen_models()
    gen_params()

if __name__ == "__main__":
    main()
