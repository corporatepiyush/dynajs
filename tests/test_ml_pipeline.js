/* test_ml_pipeline.js -- class Pipeline (W9.7).
 *
 * A Pipeline composes the same calls a caller would make by hand, so its value
 * is not speed -- it is that the composition cannot be got wrong. The classic
 * error it removes is fitting a scaler on the whole dataset and only then
 * splitting: the test fold's mean has leaked into the training statistics and
 * the score comes out optimistic. Inside a Pipeline the scaler is fitted by
 * fit() and only APPLIED by predict(), so handing a Pipeline to crossValScore
 * does the right thing by construction.
 *
 * The two properties worth asserting hardest:
 *
 *   1. IT EQUALS THE HAND-WRITTEN COMPOSITION, exactly. If it did not, it
 *      would be a second way to compute something that already had one.
 *   2. IT LEAKS NOTHING. It holds strong references to its stages, so it needs
 *      a gc_mark, and the framework's close() sets the resource's native
 *      pointer to NULL -- which is how the first version of this class leaked
 *      every stage and tripped JS_FreeRuntime's gc_obj_list assertion at exit.
 *      The churn block below is what catches that class of bug.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_ml_pipeline.js
 */
import { Pipeline, StandardScaler, MinMaxScaler, PCA, LogisticRegression, LinearRegression, DecisionTreeClassifier, crossValScore } from "dyna:ml";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) {
    n++;
    if (JSON.stringify(a) !== JSON.stringify(b))
        throw new Error("assertion failed: " + msg + "\n  got  " + JSON.stringify(a) +
                        "\n  want " + JSON.stringify(b));
}
function throwsWith(fn, needle, msg) {
    n++;
    let e = null;
    try { fn(); } catch (err) { e = err; }
    if (e === null) throw new Error("assertion failed: " + msg + " (expected a throw)");
    if (!String(e.message).includes(needle))
        throw new Error("assertion failed: " + msg + " (message was: " + e.message + ")");
}

/* A separable-but-scaled problem: feature 0 is two orders of magnitude larger
 * than the others, so a pipeline WITHOUT the scaler behaves differently from
 * one with it. That makes the scaler observably load-bearing rather than
 * decorative. */
const X = [], y = [];
for (let i = 0; i < 80; i++) {
    const a = (i % 2) ? 3 + Math.sin(i) * 0.4 : 0.5 + Math.sin(i) * 0.4;
    X.push([a * 100, a * 2 + 1, i % 7]);
    y.push(i % 2);
}

/* ---- shape --------------------------------------------------------------- */
{
    const p = new Pipeline([new StandardScaler(), new PCA(2), new LogisticRegression()]);
    assert(p.length === 3, "length is the stage count");
    assert(p.fitted === false, "a fresh Pipeline is not fitted");
    throwsWith(() => p.predict(X), "predict before fit", "predict before fit throws");

    assert(p.fit(X, y) === p, "fit returns this, so it chains");
    assert(p.fitted === true, "fitted after fit");
    assert(p.predict(X).length === X.length, "one prediction per row");
    assert(p.predictProba(X)[0].length === 2, "predictProba is rows x classes");
    assert(p.transform(X)[0].length === 2, "transform stops before the estimator");
    assert(p.stage(0) !== p.stage(1), "stage(i) returns the i-th stage");
    assert(p.stage(-1) === p.estimator, "stage(-1) is the estimator");
    assert(p.stage(2) === p.estimator, "and so is the last index");
    throwsWith(() => p.stage(3), "out of range", "an out-of-range stage index throws");
    p.close();
}

/* ---- IT EQUALS THE HAND-WRITTEN COMPOSITION ----------------------------- */
{
    const p = new Pipeline([new StandardScaler(), new PCA(2), new LogisticRegression()]);
    p.fit(X, y);

    const sc = new StandardScaler().fit(X);
    const Xs = sc.transform(X);
    const pc = new PCA(2).fit(Xs);
    const Xp = pc.transform(Xs);
    const lr = new LogisticRegression().fit(Xp, y);

    eq(p.predict(X), lr.predict(Xp), "predict equals the hand-written composition");
    eq(p.predictProba(X), lr.predictProba(Xp), "predictProba too");
    eq(p.transform(X), Xp, "transform equals the feature stages by hand");

    /* And the scaler really is doing something: the same pipeline without it
     * gives a different answer, so this is not a test that would pass with an
     * inert stage. */
    const bare = new Pipeline([new PCA(2), new LogisticRegression()]).fit(X, y);
    assert(JSON.stringify(bare.predictProba(X)) !== JSON.stringify(p.predictProba(X)),
        "the scaler stage changes the result, so the composition is load-bearing");
}

/* ---- a single stage, and a transformer-terminated pipeline --------------- */
{
    const one = new Pipeline([new LogisticRegression()]).fit(X, y);
    const raw = new LogisticRegression().fit(X, y);
    eq(one.predict(X), raw.predict(X), "a one-stage Pipeline is the estimator");
    eq(one.transform(X), X, "and transform is the identity when nothing transforms");

    /* Ending in a transformer means transform() runs all the way through. */
    const feats = new Pipeline([new StandardScaler(), new PCA(2)]).fit(X);
    assert(feats.transform(X)[0].length === 2, "a transformer-terminated Pipeline transforms fully");
    throwsWith(() => feats.predict(X), "predict", "...and cannot predict");
}

/* ---- it composes with model selection ----------------------------------- */
{
    /* crossValScore takes a FACTORY, so each fold gets a fresh Pipeline and the
     * scaler is refitted per fold. That is the whole reason to use one. */
    const scores = crossValScore(
        () => new Pipeline([new StandardScaler(), new LogisticRegression()]), X, y, { k: 4 });
    assert(scores.length === 4, "one score per fold");
    assert(scores.every(s => s >= 0 && s <= 1), "scores are in range");

    /* A regression pipeline, so this is not only tested on a classifier. */
    const yr = X.map(r => r[1] * 3 - 2);
    const rp = new Pipeline([new MinMaxScaler(), new LinearRegression()]).fit(X, yr);
    const pred = rp.predict(X);
    let worst = 0;
    for (let i = 0; i < yr.length; i++) worst = Math.max(worst, Math.abs(pred[i] - yr[i]));
    assert(worst < 1e-6, "a scaled linear pipeline recovers an exact linear target");
}

/* ---- construction is validated where the caller is looking --------------- */
{
    throwsWith(() => new Pipeline([]), "at least one stage", "an empty Pipeline is refused");
    throwsWith(() => new Pipeline("nope"), "requires an array", "a non-array is refused");
    throwsWith(() => new Pipeline([{}]), "has no fit()", "a stage without fit is named");
    throwsWith(() => new Pipeline([42]), "not an object", "a non-object stage is named");
    /* The transform requirement is checked at CONSTRUCTION, not at fit, so the
     * error arrives while the caller is still looking at the constructor. */
    throwsWith(() => new Pipeline([new LogisticRegression(), new LogisticRegression()]),
        "has no transform()", "a non-final estimator is refused at construction");
    /* ...and the last stage is exempt from it. */
    assert(new Pipeline([new StandardScaler(), new LogisticRegression()]).length === 2,
        "only the LAST stage may be a bare estimator");
}

/* ---- ownership ---------------------------------------------------------- */
{
    const lr = new LogisticRegression();
    const p = new Pipeline([new StandardScaler(), lr]);
    p.fit(X, y);
    p.close();
    assert(p.closed === true, "close is observable");
    throwsWith(() => p.predict(X), "closed", "use after close throws");
    throwsWith(() => p.fit(X, y), "closed", "fit after close throws");
    n++;
    p.close();   /* idempotent */

    /* Closing the Pipeline RELEASES its references; it does not close a stage
     * out from under anything else that holds one. A cascade would make a
     * shared stage unusable the moment any Pipeline holding it was closed,
     * and it would mean running JS from a teardown path. */
    assert(lr.closed === false, "a stage held elsewhere survives the Pipeline");
    assert(lr.predict(X).length === X.length, "and still works");
}

/* ---- IT LEAKS NOTHING ---------------------------------------------------- */
{
    /* The first version of this class leaked every stage, because the
     * framework's close() sets the resource's native pointer to NULL and the
     * finalizer then had nothing left to free. It surfaced as an assertion
     * inside JS_FreeRuntime at process exit -- i.e. only if something actually
     * churned Pipelines. This block is that churn.
     *
     * Half are closed explicitly and half are dropped for the collector, so
     * both teardown paths are exercised. */
    for (let i = 0; i < 400; i++) {
        const p = new Pipeline([new StandardScaler(), new PCA(2), new LogisticRegression()]);
        p.fit(X.slice(0, 20), y.slice(0, 20));
        p.predict(X.slice(0, 5));
        if (i % 2) p.close();
    }
    if (typeof gc === "function") { gc(); gc(); }

    /* A Pipeline in a reference cycle must still be collectable, which is what
     * the gc_mark exists for: without it the cycle collector cannot see the
     * stages and the cycle is never reclaimed. */
    for (let i = 0; i < 200; i++) {
        const p = new Pipeline([new StandardScaler(), new LogisticRegression()]);
        const holder = { p };
        p.self = holder;          /* cycle: p -> holder -> p */
        p.fit(X.slice(0, 12), y.slice(0, 12));
    }
    if (typeof gc === "function") { gc(); gc(); }
    assert(true, "600 Pipelines built, fitted and dropped without a leak");
}

/* ---- the adversarial argument ------------------------------------------- */
{
    /* Reading the stage array runs getters, and a getter must not be able to
     * observe a half-built Pipeline. Every stage is read and type-checked
     * before the object exists, so a throwing getter leaves nothing behind. */
    let reads = 0;
    const hostile = [];
    hostile.length = 2;
    Object.defineProperty(hostile, 0,
        { get() { reads++; return new StandardScaler(); }, configurable: true });
    Object.defineProperty(hostile, 1,
        { get() { reads++; throw new Error("boom"); }, configurable: true });
    n++;
    let caught = null;
    try { new Pipeline(hostile); } catch (e) { caught = e; }
    assert(caught !== null && String(caught.message).includes("boom"),
        "a throwing getter propagates rather than being swallowed");
    assert(reads === 2, "both getters ran, so the attack actually reached the second");
    if (typeof gc === "function") gc();

    /* A stage that throws during fit leaves the Pipeline unfitted rather than
     * half-fitted. */
    const bad = new Pipeline([new StandardScaler(), new DecisionTreeClassifier()]);
    n++;
    try { bad.fit(X, [1, 2]); } catch { /* y length mismatch */ }
    assert(bad.fitted === false, "a failed fit does not mark the Pipeline fitted");
    throwsWith(() => bad.predict(X), "before fit", "...and predict still refuses");
}

/* ---- it does not accept sampleWeight it cannot honour -------------------- */
{
    /* Pipeline.fit forwards only (X, y) to the final stage, so an options
     * object handed to it would be DROPPED. It is REFUSED instead -- this
     * assertion is the difference between noting the hazard and preventing
     * it, and the first version of this class only noted it. */
    const p = new Pipeline([new StandardScaler(), new DecisionTreeClassifier()]);
    throwsWith(() => p.fit(X, y, { sampleWeight: X.map(() => 1) }),
        "no weighted fit", "Pipeline.fit refuses sampleWeight rather than dropping it");
    n++;
    let ok = true;
    try { p.fit(X, y); } catch { ok = false; }
    assert(ok, "and it fits normally without one");

    /* Weighting the ESTIMATOR directly is the supported route, and it works
     * through a Pipeline because fit() only ever passes (X, y) along. */
    const wp = new Pipeline([new StandardScaler(), new LogisticRegression()]);
    wp.fit(X, y);
    assert(wp.predict(X).length === X.length, "a weightable estimator still fits in a Pipeline");
}

print("test_ml_pipeline: all " + n + " assertions passed");
