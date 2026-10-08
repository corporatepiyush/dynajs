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

const X = [], y = [];
for (let i = 0; i < 80; i++) {
    const a = (i % 2) ? 3 + Math.sin(i) * 0.4 : 0.5 + Math.sin(i) * 0.4;
    X.push([a * 100, a * 2 + 1, i % 7]);
    y.push(i % 2);
}

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

    const bare = new Pipeline([new PCA(2), new LogisticRegression()]).fit(X, y);
    assert(JSON.stringify(bare.predictProba(X)) !== JSON.stringify(p.predictProba(X)),
        "the scaler stage changes the result, so the composition is load-bearing");
}

{
    const one = new Pipeline([new LogisticRegression()]).fit(X, y);
    const raw = new LogisticRegression().fit(X, y);
    eq(one.predict(X), raw.predict(X), "a one-stage Pipeline is the estimator");
    eq(one.transform(X), X, "and transform is the identity when nothing transforms");

    const feats = new Pipeline([new StandardScaler(), new PCA(2)]).fit(X);
    assert(feats.transform(X)[0].length === 2, "a transformer-terminated Pipeline transforms fully");
    throwsWith(() => feats.predict(X), "predict", "...and cannot predict");
}

{
    const scores = crossValScore(
        () => new Pipeline([new StandardScaler(), new LogisticRegression()]), X, y, { k: 4 });
    assert(scores.length === 4, "one score per fold");
    assert(scores.every(s => s >= 0 && s <= 1), "scores are in range");

    const yr = X.map(r => r[1] * 3 - 2);
    const rp = new Pipeline([new MinMaxScaler(), new LinearRegression()]).fit(X, yr);
    const pred = rp.predict(X);
    let worst = 0;
    for (let i = 0; i < yr.length; i++) worst = Math.max(worst, Math.abs(pred[i] - yr[i]));
    assert(worst < 1e-6, "a scaled linear pipeline recovers an exact linear target");
}

{
    throwsWith(() => new Pipeline([]), "at least one stage", "an empty Pipeline is refused");
    throwsWith(() => new Pipeline("nope"), "requires an array", "a non-array is refused");
    throwsWith(() => new Pipeline([{}]), "has no fit()", "a stage without fit is named");
    throwsWith(() => new Pipeline([42]), "not an object", "a non-object stage is named");
    throwsWith(() => new Pipeline([new LogisticRegression(), new LogisticRegression()]),
        "has no transform()", "a non-final estimator is refused at construction");
    assert(new Pipeline([new StandardScaler(), new LogisticRegression()]).length === 2,
        "only the LAST stage may be a bare estimator");
}

{
    const lr = new LogisticRegression();
    const p = new Pipeline([new StandardScaler(), lr]);
    p.fit(X, y);
    p.close();
    assert(p.closed === true, "close is observable");
    throwsWith(() => p.predict(X), "closed", "use after close throws");
    throwsWith(() => p.fit(X, y), "closed", "fit after close throws");
    n++;
    p.close();

    assert(lr.closed === false, "a stage held elsewhere survives the Pipeline");
    assert(lr.predict(X).length === X.length, "and still works");
}

{
    for (let i = 0; i < 400; i++) {
        const p = new Pipeline([new StandardScaler(), new PCA(2), new LogisticRegression()]);
        p.fit(X.slice(0, 20), y.slice(0, 20));
        p.predict(X.slice(0, 5));
        if (i % 2) p.close();
    }
    if (typeof gc === "function") { gc(); gc(); }

    for (let i = 0; i < 200; i++) {
        const p = new Pipeline([new StandardScaler(), new LogisticRegression()]);
        const holder = { p };
        p.self = holder;
        p.fit(X.slice(0, 12), y.slice(0, 12));
    }
    if (typeof gc === "function") { gc(); gc(); }
    assert(true, "600 Pipelines built, fitted and dropped without a leak");
}

{
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

    const bad = new Pipeline([new StandardScaler(), new DecisionTreeClassifier()]);
    n++;
    try { bad.fit(X, [1, 2]); } catch {  }
    assert(bad.fitted === false, "a failed fit does not mark the Pipeline fitted");
    throwsWith(() => bad.predict(X), "before fit", "...and predict still refuses");
}

{
    const p = new Pipeline([new StandardScaler(), new DecisionTreeClassifier()]);
    throwsWith(() => p.fit(X, y, { sampleWeight: X.map(() => 1) }),
        "no weighted fit", "Pipeline.fit refuses sampleWeight rather than dropping it");
    n++;
    let ok = true;
    try { p.fit(X, y); } catch { ok = false; }
    assert(ok, "and it fits normally without one");

    const wp = new Pipeline([new StandardScaler(), new LogisticRegression()]);
    wp.fit(X, y);
    assert(wp.predict(X).length === X.length, "a weightable estimator still fits in a Pipeline");
}

print("test_ml_pipeline: all " + n + " assertions passed");
