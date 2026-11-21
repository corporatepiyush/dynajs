// covers: dyna:ml.PCA
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_pca_pipeline");
import { Pipeline, StandardScaler, MinMaxScaler, PCA, LogisticRegression, LinearRegression, DecisionTreeClassifier, crossValScore } from "dyna:ml";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) {
    n++;
    if (JSON.stringify(a) !== JSON.stringify(b))
        throw new Error("assertion failed: " + msg + "\n  got  " + JSON.stringify(a) +
                        "\n  want " + JSON.stringify(b));
}
async function throwsWith(fn, needle, msg) {
    n++;
    let e = null;
    try { await fn(); } catch (err) { e = err; }
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

await __kitS.section("predict equals the hand-written composition", async () => {
    const p = new Pipeline([new StandardScaler(), new PCA(2), new LogisticRegression()]);
    await p.fit(X, y);

    const sc = await new StandardScaler().fit(X);
    const Xs = await sc.transform(X);
    const pc = await new PCA(2).fit(Xs);
    const Xp = await pc.transform(Xs);
    const lr = await new LogisticRegression().fit(Xp, y);

    eq(await p.predict(X), await lr.predict(Xp), "predict equals the hand-written composition");
    eq(await p.predictProba(X), await lr.predictProba(Xp), "predictProba too");
    eq(await p.transform(X), Xp, "transform equals the feature stages by hand");

    const bare = await new Pipeline([new PCA(2), new LogisticRegression()]).fit(X, y);
    assert(JSON.stringify(await bare.predictProba(X)) !== JSON.stringify(await p.predictProba(X)),
        "the scaler stage changes the result, so the composition is load-bearing");
});
__kitS.end();

print("test_ml_pipeline: all " + n + " assertions passed");
