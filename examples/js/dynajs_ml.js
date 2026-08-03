/*
 * dyna:ml -- native machine learning self-contained, in-repo, with DETERMINISTIC
 * memory management (no GC reliance).
 *
 * Requires a CONFIG_NATIVE_MODULES build:
 *     make CONFIG_NATIVE_MODULES=y
 *     ./dynajs examples/js/dynajs_ml.js
 *
 * Each model owns a private arena; .close() (aliased .dispose()) frees it
 * immediately -- O(1), no GC. The finalizer is only a safety net, so production
 * code should always close explicitly (try/finally or the withResource helper).
 *
 * JS arrays go in (Array-of-Array<number> for X, Array<number> for y); the data
 * is COPIED into the native side, and predictions come back as plain JS Arrays.
 * Nothing native ever escapes into the JS heap.
 */
import { LinearRegression, LogisticRegression, KMeans } from "dyna:ml";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }

/* Deterministic-dispose helper: runs fn(resource) and always closes it. */
function withResource(resource, fn) {
    try { return fn(resource); }
    finally { resource.close(); }
}

/* ---- LinearRegression: closed-form OLS (y = 2x) ---- */
function demo_linear() {
    return withResource(new LinearRegression(), (m) => {
        m.fit([[1], [2], [3]], [2, 4, 6]);
        const y = m.predict([[4], [5]]);          // -> ~[8, 10]
        assert(Math.abs(y[0] - 8) < 1e-2, "predict(4) ~ 8");
        return y;
    });
}

/* ---- LogisticRegression: binary classification ---- */
function demo_logistic() {
    return withResource(new LogisticRegression(), (m) => {
        m.fit([[-2], [-1], [1], [2]], [0, 0, 1, 1]);
        const labels = m.predict([[-3], [3]]);    // -> [0, 1]
        /* predictProba is ROWS x CLASSES, like every other classifier here --
         * proba[0] is the row for the single sample and proba[0][1] is
         * P(class 1). It was binary-only once and returned a bare probability;
         * this example still read it that way, and comparing an ARRAY against
         * 0.5 is silently false rather than an error. */
        const proba = m.predictProba([[3]]);      // -> [[P(class 0), P(class 1)]]
        assert(labels[0] === 0 && labels[1] === 1, "separates classes");
        assert(proba[0].length === 2, "predictProba is rows x classes");
        assert(proba[0][1] > 0.5, "positive sample favours class 1");
        assert(Math.abs(proba[0][0] + proba[0][1] - 1) < 1e-12, "the row sums to 1");
        return { labels, proba };
    });
}

/* ---- KMeans: unsupervised clustering of two separated blobs ---- */
function demo_kmeans() {
    return withResource(new KMeans(2, 42), (m) => {
        const X = [[0, 0], [0.1, 0.1], [9, 9], [9.2, 8.9]];
        m.fit(X);
        const labels = m.predict(X);              // two distinct cluster ids
        assert(labels[0] === labels[1], "near points share a cluster");
        assert(labels[0] !== labels[2], "far points are in different clusters");
        return { labels, inertia: m.inertia };
    });
}

print("LinearRegression predict([[4],[5]]) =", JSON.stringify(demo_linear()));
const lg = demo_logistic();
print("LogisticRegression labels =", JSON.stringify(lg.labels),
      "P(class 1) =", lg.proba[0][1].toFixed(3));
const km = demo_kmeans();
print("KMeans labels =", JSON.stringify(km.labels),
      "inertia =", km.inertia.toFixed(4));

/* The point: deterministic release means flat memory across many models. */
let churn = 0;
for (let i = 0; i < 50000; i++) {
    const m = new LinearRegression();
    m.fit([[1], [2], [3]], [i, 2 * i, 3 * i]);
    m.close();                 // explicit, immediate arena free -- no GC
    churn++;
}
print("Deterministic-free demo:", churn, "models created+closed in constant memory");

/* closed resources reject further use (fail fast, not silent corruption) */
const dead = new LinearRegression();
dead.close();
let threw = false;
try { dead.predict([[1]]); } catch (e) { threw = true; }
assert(threw, "use-after-close must throw");
assert(dead.closed === true, "closed flag");

print("PASS");
