// flags: --std
// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_persist");
import * as ml from "dyna:ml";
import { CRC32C } from "dyna:hash";
import { Path } from "dyna:file";
import * as std from "std";
const SCRATCH = `${std.getenv("TMPDIR") || "/tmp"}`;

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}
async function rejects(fn, kind, m) {
    n++;
    let p;
    try { p = fn(); } catch (e) { throw new Error((m || "rejection") + ": threw synchronously instead of rejecting: " + e); }
    if (!(p instanceof Promise)) throw new Error((m || "rejection") + ": did not return a promise");
    try { await p; } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return;
    }
    throw new Error((m || "expected a rejection") + " but none happened");
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }
const rnd = lcg(20260727);
const X = [], y = [], ycls = [];
for (let i = 0; i < 200; i++) {
    const a = rnd() * 4 - 2, b = rnd() * 4 - 2, c = rnd() * 4 - 2;
    X.push([a, b, c]);
    y.push(1.5 * a - 2 * b + 0.5 * c + 0.3);
    ycls.push(a + b > 0 ? 1 : 0);
}
const Xt = X.slice(0, 40);

const MODELS = [
    ["LinearRegression", async () => { const m = new ml.LinearRegression(); await m.fit(X, y); return m; },
     async m => [await m.predict(Xt), m.coef, m.intercept]],
    ["LogisticRegression", async () => { const m = new ml.LogisticRegression(); await m.fit(X, ycls); return m; },
     async m => [await m.predict(Xt), m.coef, m.intercept]],
    ["KMeans", async () => { const m = new ml.KMeans(3, { seed: 7 }); await m.fit(X); return m; },
     async m => [await m.predict(Xt), m.centroids, m.inertia]],
    ["PCA", async () => { const m = new ml.PCA(2); await m.fit(X); return m; },
     async m => [await m.transform(Xt), m.components, m.explainedVarianceRatio]],
    ["GaussianNB", async () => { const m = new ml.GaussianNB(); await m.fit(X, ycls); return m; },
     async m => [await m.predict(Xt), await m.predictProba(Xt)]],
    ["KNClassifier", async () => { const m = new ml.KNClassifier(5); await m.fit(X, ycls); return m; },
     async m => await m.predict(Xt)],
    ["KNRegressor", async () => { const m = new ml.KNRegressor(5); await m.fit(X, y); return m; },
     async m => await m.predict(Xt)],
    ["StandardScaler", async () => { const m = new ml.StandardScaler(); await m.fit(X); return m; },
     async m => [await m.transform(Xt), await m.inverseTransform(await m.transform(Xt))]],
    ["MinMaxScaler", async () => { const m = new ml.MinMaxScaler(); await m.fit(X); return m; },
     async m => [await m.transform(Xt), await m.inverseTransform(await m.transform(Xt))]],
    ["SVC", async () => { const m = new ml.SVC({ kernel: "rbf" }); await m.fit(X, ycls); return m; },
     async m => await m.predict(Xt)],
    ["GaussianMixture", async () => { const m = new ml.GaussianMixture(2, { seed: 3 }); await m.fit(X); return m; },
     async m => [await m.predict(Xt), m.logLikelihood, m.weights, m.means]],
    ["DecisionTreeClassifier", async () => { const m = new ml.DecisionTreeClassifier(); await m.fit(X, ycls); return m; },
     async m => await m.predict(Xt)],
    ["DecisionTreeRegressor", async () => { const m = new ml.DecisionTreeRegressor({ maxDepth: 4 }); await m.fit(X, y); return m; },
     async m => await m.predict(Xt)],
    ["RandomForestClassifier", async () => { const m = new ml.RandomForestClassifier({ nEstimators: 8, seed: 1 }); await m.fit(X, ycls); return m; },
     async m => await m.predict(Xt)],
    ["RandomForestRegressor", async () => { const m = new ml.RandomForestRegressor({ nEstimators: 6, maxDepth: 4, seed: 1 }); await m.fit(X, y); return m; },
     async m => await m.predict(Xt)],
    ["GradientBoostingRegressor", async () => { const m = new ml.GradientBoostingRegressor({ nEstimators: 10, seed: 1 }); await m.fit(X, y); return m; },
     async m => await m.predict(Xt)],
    ["GradientBoostingClassifier", async () => { const m = new ml.GradientBoostingClassifier({ nEstimators: 8, maxDepth: 2, seed: 1 }); await m.fit(X, ycls); return m; },
     async m => [await m.predict(Xt), await m.predictProba(Xt), await m.apply(Xt)]],
    ["XGBRegressor", async () => { const m = new ml.XGBRegressor({ nEstimators: 8, maxDepth: 3, lambda: 2, alpha: 0.5, colsampleByTree: 0.8, seed: 1 }); await m.fit(X, y); return m; },
     async m => [await m.predict(Xt), m.bestRounds]],
    ["XGBClassifier", async () => { const m = new ml.XGBClassifier({ nEstimators: 8, maxDepth: 3, gamma: 0.01, seed: 1 }); await m.fit(X, ycls); return m; },
     async m => [await m.predict(Xt), await m.predictProba(Xt), await m.apply(Xt), m.bestRounds]],
    ["DBScan", async () => { const m = new ml.DBScan(0.7, 4); await m.fit(X); return m; },
     m => [m.labels, m.nClusters]],
];

await __kitS.section("bad magic", async () => {
    const model = (await MODELS.find(m => m[0] === "LinearRegression")[1]());
    const good = model.serialize();

    const bad = (mutate, why) => {
        const b = good.slice();
        mutate(b);
        throws(() => ml.LinearRegression.deserialize(b), Error, why);
    };
    bad(b => { b[0] ^= 1; }, "bad magic");
    bad(b => { b[4] = 9; }, "bad version");
    bad(b => { b[b.length - 1] ^= 0xff; }, "bad CRC");
    bad(b => { b[24] ^= 0xff; }, "corrupted payload");
    throws(() => ml.LinearRegression.deserialize(good.subarray(0, 12)), Error,
           "truncated");
    throws(() => ml.LinearRegression.deserialize("nope"), TypeError,
           "not bytes");
    await rejects(() => ml.LinearRegression.load(new Path("/tmp/definitely/not/here.dyns")),
           TypeError, "missing file");

    {
        const b = new Uint8Array(good.length + 8);
        b.set(good.subarray(0, good.length - 4));
        const plen = good.length - 24 + 8;
        for (let i = 0; i < 8; i++) b[12 + i] = (plen >>> (i * 8)) & 0xff;
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        throws(() => ml.LinearRegression.deserialize(b), Error,
               "a record with unconsumed trailing bytes");
    }
});

await __kitS.section("the sweep covers every model class", async () => {
    function repair(b) {
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    }
    const SMALL = X.slice(0, 30), SY = y.slice(0, 30), SC = ycls.slice(0, 30);
    const St = SMALL.slice(0, 5);
    const cases = [
        ["LinearRegression", async () => { const m = new ml.LinearRegression(); await m.fit(SMALL, SY); return m; }, async m => await m.predict(St)],
        ["LogisticRegression", async () => { const m = new ml.LogisticRegression(); await m.fit(SMALL, SC); return m; }, async m => await m.predict(St)],
        ["KMeans", async () => { const m = new ml.KMeans(2, { seed: 1 }); await m.fit(SMALL); return m; }, async m => await m.predict(St)],
        ["PCA", async () => { const m = new ml.PCA(2); await m.fit(SMALL); return m; }, async m => await m.transform(St)],
        ["GaussianNB", async () => { const m = new ml.GaussianNB(); await m.fit(SMALL, SC); return m; }, async m => [await m.predict(St), await m.predictProba(St)]],
        ["KNClassifier", async () => { const m = new ml.KNClassifier(3); await m.fit(SMALL, SC); return m; }, async m => await m.predict(St)],
        ["KNRegressor", async () => { const m = new ml.KNRegressor(3); await m.fit(SMALL, SY); return m; }, async m => await m.predict(St)],
        ["StandardScaler", async () => { const m = new ml.StandardScaler(); await m.fit(SMALL); return m; }, async m => await m.transform(St)],
        ["MinMaxScaler", async () => { const m = new ml.MinMaxScaler(); await m.fit(SMALL); return m; }, async m => await m.transform(St)],
        ["SVC", async () => { const m = new ml.SVC({ kernel: "linear", maxIter: 50 }); await m.fit(SMALL, SC); return m; }, async m => await m.predict(St)],
        ["GaussianMixture", async () => { const m = new ml.GaussianMixture(2, { seed: 1, maxIter: 10 }); await m.fit(SMALL); return m; }, async m => await m.predict(St)],
        ["DecisionTreeClassifier", async () => { const m = new ml.DecisionTreeClassifier({ maxDepth: 2 }); await m.fit(SMALL, SC); return m; }, async m => await m.predict(St)],
        ["DecisionTreeRegressor", async () => { const m = new ml.DecisionTreeRegressor({ maxDepth: 2 }); await m.fit(SMALL, SY); return m; }, async m => await m.predict(St)],
        ["RandomForestClassifier", async () => { const m = new ml.RandomForestClassifier({ nEstimators: 2, maxDepth: 2, seed: 1 }); await m.fit(SMALL, SC); return m; }, async m => await m.predict(St)],
        ["RandomForestRegressor", async () => { const m = new ml.RandomForestRegressor({ nEstimators: 2, maxDepth: 2, seed: 1 }); await m.fit(SMALL, SY); return m; }, async m => await m.predict(St)],
        ["GradientBoostingRegressor", async () => { const m = new ml.GradientBoostingRegressor({ nEstimators: 3, maxDepth: 2, seed: 1 }); await m.fit(SMALL, SY); return m; }, async m => await m.predict(St)],
        ["GradientBoostingClassifier", async () => { const m = new ml.GradientBoostingClassifier({ nEstimators: 3, maxDepth: 2, seed: 1 }); await m.fit(SMALL, SC); return m; }, async m => await m.predictProba(St)],
        ["XGBRegressor", async () => { const m = new ml.XGBRegressor({ nEstimators: 3, maxDepth: 2, seed: 1 }); await m.fit(SMALL, SY); return m; }, async m => await m.predict(St)],
        ["XGBClassifier", async () => { const m = new ml.XGBClassifier({ nEstimators: 3, maxDepth: 2, seed: 1 }); await m.fit(SMALL, SC); return m; }, async m => await m.predictProba(St)],
        ["DBScan", async () => { const m = new ml.DBScan(0.9, 3); await m.fit(SMALL); return m; }, m => m.labels],
    ];
    assert(cases.length === MODELS.length, "the sweep covers every model class");

    let attempts = 0, decoded = 0;
    for (const [name, build, use] of cases) {
        const good = (await build()).serialize();
        const step = Math.max(1, Math.ceil((good.length - 24) / 120));
        for (let i = 20; i < good.length - 4; i += step) {
            for (const mask of [0xff, 0x80]) {
                const b = good.slice();
                b[i] ^= mask;
                repair(b);
                attempts++;
                try {
                    const v = ml[name].deserialize(b);
                    decoded++;
                    await use(v);
                } catch (e) {
                    if (!(e instanceof Error))
                        throw new Error(name + ": non-Error from deserialize");
                }
            }
        }
    }
    assert(attempts > 1500, "all-model sweep ran (" + attempts + " cases)");
    assert(decoded > 0, "the sweep must actually reach the readers");
    print("  all-model sweep:   " + attempts + " forged records across " +
          cases.length + " readers, " + decoded + " decoded and used");
});

await __kitS.section("save() rejects a non-Path before coercing anything", async () => {
    const m = new ml.LinearRegression();
    await m.fit(X, y);
    let ran = false;
    const attacker = {
        toString() { ran = true; m.close(); return "/tmp/dyna_ml_attack.dyns"; },
    };
    await rejects(() => m.save(attacker), TypeError,
           "save() rejects a non-Path before coercing anything");
    assert(!ran, "the toString hook never ran -- save() borrows, it does not coerce");
    assert(m.closed === false, "the model the attacker tried to close is still open");
    const ok = new Path(`${SCRATCH}/dj_ml_attack_ok.${Date.now() % 10000000}.dyns`);
    await m.save(ok);
    assert((await ml.LinearRegression.load(ok)) instanceof ml.LinearRegression,
           "the same call succeeds with a real Path");
});
__kitS.end();

print("test_ml_persist: all " + n + " assertions passed");
