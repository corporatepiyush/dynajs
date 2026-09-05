#!/usr/bin/env python3
# gen_ml_sklearn_vectors.py -- generate tests/test_ml_sklearn_vectors.js.
#
# The ONLY external value oracle for dyna:ml: fits the reference implementation
# (scikit-learn) on deterministic datasets and emits the fitted values as a
# vendored JS module, so the test suite needs no numpy/sklearn at test time.
# Regenerate with the venv pinned in the header; a vector change without a
# version change in the diff is a regression warning, not a routine update.
#
#   python3 -m venv tmp_audit/sklenv && tmp_audit/sklenv/bin/pip install scikit-learn==1.7.2
#   tmp_audit/sklenv/bin/python tools/gen_ml_sklearn_vectors.py
import json
import numpy as np
import sklearn
from sklearn.linear_model import LinearRegression, LogisticRegression
from sklearn.cluster import KMeans
from sklearn.decomposition import PCA
from sklearn.naive_bayes import GaussianNB
from sklearn.preprocessing import StandardScaler, MinMaxScaler
from sklearn.svm import SVC
from sklearn.tree import DecisionTreeRegressor, DecisionTreeClassifier
from sklearn.mixture import GaussianMixture
from sklearn.neighbors import KNeighborsClassifier, KNeighborsRegressor
from sklearn.metrics import (r2_score, mean_absolute_error, mean_squared_error,
                             log_loss, accuracy_score, roc_auc_score,
                             average_precision_score, confusion_matrix)

# Deterministic LCG, same generator shape as the JS tests, so the datasets are
# reproducible from source in any language.
class LCG:
    def __init__(self, seed):
        self.s = seed & 0xffffffff
    def next(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xffffffff
        return self.s / 4294967296.0

def dataset(seed, n, cols, fn):
    r = LCG(seed)
    X = np.zeros((n, cols))
    for i in range(n):
        for j in range(cols):
            X[i, j] = r.next() * 4.0 - 2.0
    y = fn(X, r)
    return X, y

X_lin, y_lin = dataset(101, 60, 3,
                       lambda X, r: 2.0 * X[:, 0] - X[:, 1] + 0.5 * X[:, 2]
                                    + 0.3 + (r.next() - 0.5) * 0.2)
X_clf, y_clf = dataset(102, 80, 3,
                       lambda X, r: (1.5 * X[:, 0] - X[:, 1] + X[:, 2]
                                     + (r.next() - 0.5) * 2.0 > 0.0).astype(float))
X_svc, y_svc = dataset(103, 60, 2,
                       lambda X, r: (X[:, 0] * X[:, 1] > 0.0).astype(float))
X_tree, y_tree = dataset(104, 60, 3,
                         lambda X, r: 2.0 * X[:, 0] - X[:, 1] + 0.5 * X[:, 2]
                                      + (r.next() - 0.5) * 0.4)
X_tc, y_tc = dataset(105, 60, 3,
                     lambda X, r: (2.0 * X[:, 0] - X[:, 1] + X[:, 2]
                                   + (r.next() - 0.5) * 0.8 > 0.0).astype(float))
X_nb, y_nb = dataset(106, 100, 3,
                     lambda X, r: (X[:, 0] + 0.5 * X[:, 1] - X[:, 2]
                                   + (r.next() - 0.5) * 0.6 > 0.0).astype(float))
X_knn, y_knn = dataset(107, 40, 3,
                       lambda X, r: (X[:, 0] - 0.3 * X[:, 1] + 0.7 * X[:, 2]
                                     + (r.next() - 0.5) * 0.5 > 0.0).astype(float))
X_scale, _ = dataset(108, 30, 3, lambda X, r: None)
X_scale[:, 2] = 7.0    # a constant column: zero variance

# well-separated blobs: any sane init converges to the same optimum, so the
# different RNGs of the two implementations cannot split the comparison
def blobs(seed, n):
    r = LCG(seed)
    X = np.zeros((n, 2))
    for i in range(n):
        c = i % 3
        cx, cy = ((0.0, 0.0), (6.0, 0.0), (3.0, 5.0))[c]
        X[i, 0] = cx + (r.next() - 0.5) * 0.6
        X[i, 1] = cy + (r.next() - 0.5) * 0.6
    return X

X_km = blobs(201, 90)
X_gmm = blobs(202, 90)
X_pca, _ = dataset(109, 40, 3, lambda X, r: None)
X_pca[:, 1] = 2.0 * X_pca[:, 0] + X_pca[:, 1]
X_pca[:, 2] = -0.5 * X_pca[:, 0] + 2.0 * X_pca[:, 2]

def arr(a):
    return [float(x) for x in np.ravel(a)]

E = {}

lin = LinearRegression().fit(X_lin, y_lin)
E['lin'] = {
    'coef': arr(lin.coef_), 'intercept': float(lin.intercept_),
    'pred': arr(lin.predict(X_lin)),
}

log = LogisticRegression(C=1.0, tol=1e-8, max_iter=20000,
                         solver='lbfgs').fit(X_clf, y_clf)
E['log'] = {'proba': arr(log.predict_proba(X_clf)), 'pred': arr(log.predict(X_clf)),
            'coef': arr(log.coef_), 'intercept': float(log.intercept_[0])}

km = KMeans(3, init='k-means++', n_init=1, random_state=42).fit(X_km)
E['km'] = {'inertia': float(km.inertia_)}

pca = PCA(2).fit(X_pca)
E['pca'] = {
    'explained': arr(pca.explained_variance_),
    'ratio': arr(pca.explained_variance_ratio_),
    'transform': arr(pca.transform(X_pca)),
}

nb = GaussianNB(var_smoothing=1e-9).fit(X_nb, y_nb)
E['nb'] = {'proba': arr(nb.predict_proba(X_nb)), 'pred': arr(nb.predict(X_nb))}

ss = StandardScaler().fit(X_scale)
ss_scale = ss.scale_ if hasattr(ss, 'scale_') else ss.std_
E['ss'] = {'mean': arr(ss.mean_), 'scale': arr(ss_scale),
           'transform': arr(ss.transform(X_scale))}
mm = MinMaxScaler().fit(X_scale)
E['mm'] = {'min': arr(mm.min_), 'scale': arr(mm.scale_),
           'transform': arr(mm.transform(X_scale))}

svc = SVC(kernel='rbf', C=1.0).fit(X_svc, y_svc)
E['svc'] = {'pred': arr(svc.predict(X_svc))}

dtr = DecisionTreeRegressor(max_depth=4).fit(X_tree, y_tree)
E['dtr'] = {'pred': arr(dtr.predict(X_tree))}
dtc = DecisionTreeClassifier(max_depth=4).fit(X_tc, y_tc)
E['dtc'] = {'pred': arr(dtc.predict(X_tc)), 'proba': arr(dtc.predict_proba(X_tc))}

# covariance_type='diag' matches this engine's model class (diagonal-only);
# ll is the TOTAL over samples -- lower_bound_ alone is the per-sample mean
gmm = GaussianMixture(3, covariance_type='diag', random_state=42,
                      max_iter=1000, reg_covar=1e-6).fit(X_gmm)
E['gmm'] = {'ll': float(gmm.score_samples(X_gmm).sum()),
            'means': arr(gmm.means_[np.argsort(gmm.means_[:, 0])]),
            'weights': arr(gmm.weights_[np.argsort(gmm.means_[:, 0])])}

knc = KNeighborsClassifier(5).fit(X_knn, y_knn)
knr = KNeighborsRegressor(5).fit(X_knn, y_knn)
E['knn'] = {'clf': arr(knc.predict(X_knn)), 'reg': arr(knr.predict(X_knn))}

yt = [1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
ys = [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.05]
yprob = [0.9, 0.8, 0.7, 0.4, 0.3, 0.2, 0.1, 0.05, 0.15, 0.02]
E['metrics'] = {
    'r2': float(r2_score(yt, [0.95, 0.85, 0.65, 0.55, 0.45, 0.35, 0.25, 0.15, 0.15, 0.05])),
    'mae': float(mean_absolute_error(yt, ys)),
    'mse': float(mean_squared_error(yt, ys)),
    'logloss': float(log_loss(yt, yprob)),
    'acc': float(accuracy_score(yt, [1, 1, 0, 1, 0, 0, 0, 0, 0, 0])),
    'auc': float(roc_auc_score(yt, ys)),
    'ap': float(average_precision_score(yt, ys)),
    'conf': arr(confusion_matrix(yt, [1, 1, 0, 1, 0, 0, 0, 0, 0, 0])),
}

DS = {
    'lin': [arr(X_lin), arr(y_lin)],
    'clf': [arr(X_clf), arr(y_clf)],
    'svc': [arr(X_svc), arr(y_svc)],
    'tree': [arr(X_tree), arr(y_tree)],
    'tc': [arr(X_tc), arr(y_tc)],
    'nb': [arr(X_nb), arr(y_nb)],
    'knn': [arr(X_knn), arr(y_knn)],
    'scale': [arr(X_scale), None],
    'km': [arr(X_km), None],
    'gmm': [arr(X_gmm), None],
    'pca': [arr(X_pca), None],
}

header = f"""/* GENERATED by tools/gen_ml_sklearn_vectors.py -- DO NOT EDIT BY HAND.
 * The external value oracle for dyna:ml: values fitted by the reference
 * implementation on deterministic datasets, vendored so the suite needs no
 * numpy/sklearn at test time. A diff in this file must be accompanied by a
 * diff in tools/gen_ml_sklearn_vectors.py; anything else is a silent
 * re-pinning against this engine, which is the failure this oracle exists
 * to prevent.
 *
 *   sklearn {sklearn.__version__}  numpy {np.__version__}
 */
export const DS = {json.dumps(DS, indent=0)};
export const E = {json.dumps(E, indent=0)};
"""
with open('tests/test_ml_sklearn_vectors.js', 'w') as f:
    f.write(header)
print('wrote tests/test_ml_sklearn_vectors.js')
print('sklearn', sklearn.__version__, 'numpy', np.__version__)
for k, v in E.items():
    print(' ', k)
