// Customer segmentation — PCA, k-means, a Gaussian mixture and DBSCAN on standardised behavioural features, with the
// number of segments chosen by silhouette and every clustering scored against planted ground truth.
//
// WHAT IT IS
//   The unsupervised half of a marketing analytics stack:
//
//   features    20 behavioural features for 6 000 customers drawn from 5 planted personas plus 2% outliers, standardised
//               with dyna:ml StandardScaler
//   PCA         dyna:ml PCA to 3 components; the first component is checked against power iteration on the
//               covariance matrix (simd gemv + normL2), and the explained-variance share is computed from both
//   choosing k  DBSCAN runs first and its noise is set aside (outliers otherwise win a cluster of their own);
//               k-means for k = 2..8 is scored by mean silhouette over a sample (simd distL2); the best k must be 5
//   compare     k-means, a Gaussian mixture (best of four EM seeds by silhouette) and DBSCAN on the PCA projection, each scored with the adjusted Rand
//               index (contingency counts in a Multiset); DBSCAN's noise label must catch most planted outliers
//   profiles    per-segment feature means as a Table — what marketing actually reads
//
// MODULES  dyna:ml (StandardScaler, PCA, KMeans, GaussianMixture, DBScan) · dyna:simd (gemv, normL2, scale, dot,
//          distL2) · dyna:structures (Multiset) · dyna:mathx (nchoosek) · dyna:random · dyna:cli · Array extensions
//
// RUN      dynajs examples/ml/customer-segmentation.js

import { StandardScaler, PCA, KMeans, GaussianMixture, DBScan } from "dyna:ml";
import { gemv, normL2, scale, dot, distL2 } from "dyna:simd";
import { Multiset } from "dyna:structures";
import { nchoosek } from "dyna:mathx";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(1967);
const N = 6000,
    D = 20,
    K_TRUE = 5;
const FEATURES = [
    "visits",
    "basket",
    "discount_use",
    "returns",
    "app_share",
    "night_share",
    "weekend_share",
    "categories",
    "reviews",
    "support_calls",
    "tenure",
    "email_open",
    "push_open",
    "referrals",
    "wishlist",
    "cart_abandon",
    "premium",
    "gift_share",
    "bulk_share",
    "intl_share",
];

// =============================================================================
// Planted personas
// =============================================================================
const personas = Array.from({ length: K_TRUE }, () => Array.from({ length: D }, () => rng.normal() * 2.2));
const X = [],
    truth = new Int32Array(N);
for (let i = 0; i < N; i++) {
    const outlier = rng.nextFloat() < 0.02;
    const p = rng.nextBounded(K_TRUE);
    truth[i] = outlier ? -1 : p;
    X.push(Array.from({ length: D }, (_, d) => (outlier ? rng.normal() * 9 : personas[p][d] + rng.normal())));
}
const scaler = await new StandardScaler().fit(X);
const Z = await scaler.transform(X);

// =============================================================================
// PCA, cross-checked by power iteration
// =============================================================================
const pca = await new PCA(3).fit(Z);
const P = await pca.transform(Z);
{
    const cov = new Float32Array(D * D);
    for (const row of Z)
        for (let a = 0; a < D; a++) for (let b = 0; b < D; b++) cov[a * D + b] += (row[a] * row[b]) / (N - 1);
    let v = new Float32Array(D).fill(1),
        w = new Float32Array(D);
    for (let it = 0; it < 300; it++) {
        gemv(w, cov, v, D, D, 0);
        scale(w, 1 / normL2(w));
        [v, w] = [w, v];
    }
    const pc1 = Float32Array.from(pca.components[0]);
    results["PCA's first component matches power iteration (|cos| > 0.999)"] =
        Math.abs(dot(pc1, v)) / normL2(pc1) > 0.999;
    gemv(w, cov, v, D, D, 0);
    const lambda1 = dot(w, v),
        trace = Array.from({ length: D }, (_, d) => cov[d * D + d]).sum();
    const projVar = P.map((r) => r[0] ** 2).sum() / (N - 1);
    results["variance along PC1 equals the top eigenvalue"] = Math.abs(projVar - lambda1) / lambda1 < 1e-3;
    print(`PCA: PC1 explains ${((100 * lambda1) / trace).toFixed(1)}% of standardised variance`);
}

// =============================================================================
// Scoring helpers
// =============================================================================
function ari(a, b) {
    const pairs = new Multiset(),
        ra = new Multiset(),
        rb = new Multiset();
    for (let i = 0; i < a.length; i++) {
        pairs.add(`${a[i]}|${b[i]}`);
        ra.add(String(a[i]));
        rb.add(String(b[i]));
    }
    const c2 = (n) => (n < 2 ? 0 : nchoosek(n, 2));
    const sumPairs = pairs.entrySet().reduce((s, [, n]) => s + c2(n), 0);
    const sa = ra.entrySet().reduce((s, [, n]) => s + c2(n), 0),
        sb = rb.entrySet().reduce((s, [, n]) => s + c2(n), 0);
    const expected = (sa * sb) / c2(a.length),
        max = (sa + sb) / 2;
    return (sumPairs - expected) / (max - expected);
}
results["adjusted Rand index is 1 for a relabelled copy and ~0 for noise"] =
    Math.abs(
        ari(
            truth,
            truth.map((x) => (x + 2) * 7),
        ) - 1,
    ) < 1e-12 &&
    Math.abs(
        ari(
            truth,
            Int32Array.from({ length: N }, () => rng.nextBounded(5)),
        ),
    ) < 0.01;
let sample = Array.from({ length: 700 }, () => rng.nextBounded(N));
const Pf = P.map((r) => Float32Array.from(r));
function silhouette(labels) {
    let total = 0;
    for (const i of sample) {
        const sums = new Map(),
            counts = new Map();
        for (const j of sample) {
            if (i === j) continue;
            const l = labels[j];
            sums.set(l, (sums.get(l) ?? 0) + distL2(Pf[i], Pf[j]));
            counts.set(l, (counts.get(l) ?? 0) + 1);
        }
        const a = sums.get(labels[i]) / counts.get(labels[i]);
        let b = Infinity;
        for (const [l, s] of sums) if (l !== labels[i]) b = Math.min(b, s / counts.get(l));
        total += (b - a) / Math.max(a, b);
    }
    return total / sample.length;
}

// =============================================================================
// DBSCAN first: density noise is set aside before choosing k
// =============================================================================
const db = await new DBScan(0.6, 10).fit(P);
const dbLabels = db.labels;
const clean = [...dbLabels.keys()].filter((i) => dbLabels[i] !== -1);
const Pc = clean.map((i) => P[i]);
sample = sample.filter((i) => dbLabels[i] !== -1);

// =============================================================================
// Choosing k on the points DBSCAN did not call noise
// =============================================================================
const sweep = [];
for (let k = 2; k <= 8; k++) {
    const km = await new KMeans(k, 11).fit(Pc);
    const labels = await km.predict(P);
    sweep.push({ k, sil: silhouette(labels), inertia: km.inertia, labels });
    km.close();
}
const best = sweep.reduce((a, b) => (b.sil > a.sil ? b : a));
print(
    Table(
        sweep.map((s) => [s.k, s.sil.toFixed(3), s.inertia.format(0)]),
        { head: ["k", "silhouette", "inertia"] },
    ),
);
results["silhouette picks the planted 5 segments"] = best.k === K_TRUE;
results["inertia falls monotonically with k"] = sweep.every(
    (s, i) => i === 0 || s.inertia <= sweep[i - 1].inertia * 1.001,
);

// =============================================================================
// k-means vs GMM vs DBSCAN
// =============================================================================
const inliers = [...truth.keys()].filter((i) => truth[i] >= 0);
const sub = (labels) => inliers.map((i) => labels[i]);
const kmLabels = best.labels;
// EM finds local optima; keep the best of four seeds by silhouette (an unsupervised criterion, no peeking at truth).
let gmm = null,
    gmmLabels = null,
    gmmSil = -Infinity;
for (const seed of [1, 2, 3, 4]) {
    const g = await new GaussianMixture(K_TRUE, { seed }).fit(Pc);
    const labels = await g.predict(P),
        sil = silhouette(labels);
    if (sil > gmmSil) {
        gmm?.close();
        gmm = g;
        gmmLabels = labels;
        gmmSil = sil;
    } else g.close();
}
const scores = {
    "k-means": ari(sub(truth), sub(kmLabels)),
    "Gaussian mixture": ari(sub(truth), sub(gmmLabels)),
    DBSCAN: ari(sub(truth), sub(dbLabels)),
};
const outliers = [...truth.keys()].filter((i) => truth[i] < 0);
const caught = outliers.filter((i) => dbLabels[i] === -1).length / outliers.length;
const falseNoise = inliers.filter((i) => dbLabels[i] === -1).length / inliers.length;
print(
    Table(
        Object.entries(scores).map(([n, s]) => [n, s.toFixed(3)]),
        { head: ["method (inliers)", "adjusted Rand"] },
    ),
);
results["k-means recovers the personas (ARI > 0.95)"] = scores["k-means"] > 0.95;
results["the Gaussian mixture recovers the personas (ARI > 0.95)"] = scores["Gaussian mixture"] > 0.95;
results["DBSCAN finds the personas among inliers (ARI > 0.9)"] = scores.DBSCAN > 0.9;
results["DBSCAN labels most planted outliers as noise"] = caught > 0.6 && falseNoise < 0.05;
print(
    `DBSCAN: ${db.nClusters} clusters, ${(100 * caught).toFixed(0)}% of outliers flagged, ${(100 * falseNoise).toFixed(1)}% of inliers flagged`,
);

// =============================================================================
// Segment profiles in original units
// =============================================================================
{
    const prof = Array.from({ length: K_TRUE }, () => ({ n: 0, sum: new Float64Array(D) }));
    X.forEach((row, i) => {
        const p = prof[kmLabels[i]];
        p.n++;
        row.forEach((v, d) => (p.sum[d] += v));
    });
    const top = [0, 1, 2, 4, 15];
    print(
        Table(
            prof.map((p, k) => [k, p.n, ...top.map((d) => (p.sum[d] / p.n).toFixed(2))]),
            { head: ["segment", "customers", ...top.map((d) => FEATURES[d])] },
        ),
    );
    results["every customer, outliers included, is assigned to a segment"] = prof.reduce((s, p) => s + p.n, 0) === N;
}

for (const m of [scaler, pca, gmm, db]) m.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
