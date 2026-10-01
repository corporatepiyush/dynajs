// flags: --std
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
    ["LinearRegression", () => { const m = new ml.LinearRegression(); m.fit(X, y); return m; },
     m => [m.predict(Xt), m.coef, m.intercept]],
    ["LogisticRegression", () => { const m = new ml.LogisticRegression(); m.fit(X, ycls); return m; },
     m => [m.predict(Xt), m.coef, m.intercept]],
    ["KMeans", () => { const m = new ml.KMeans(3, { seed: 7 }); m.fit(X); return m; },
     m => [m.predict(Xt), m.centroids, m.inertia]],
    ["PCA", () => { const m = new ml.PCA(2); m.fit(X); return m; },
     m => [m.transform(Xt), m.components, m.explainedVarianceRatio]],
    ["GaussianNB", () => { const m = new ml.GaussianNB(); m.fit(X, ycls); return m; },
     m => [m.predict(Xt), m.predictProba(Xt)]],
    ["KNClassifier", () => { const m = new ml.KNClassifier(5); m.fit(X, ycls); return m; },
     m => m.predict(Xt)],
    ["KNRegressor", () => { const m = new ml.KNRegressor(5); m.fit(X, y); return m; },
     m => m.predict(Xt)],
    ["StandardScaler", () => { const m = new ml.StandardScaler(); m.fit(X); return m; },
     m => [m.transform(Xt), m.inverseTransform(m.transform(Xt))]],
    ["MinMaxScaler", () => { const m = new ml.MinMaxScaler(); m.fit(X); return m; },
     m => [m.transform(Xt), m.inverseTransform(m.transform(Xt))]],
    ["SVC", () => { const m = new ml.SVC({ kernel: "rbf" }); m.fit(X, ycls); return m; },
     m => m.predict(Xt)],
    ["GaussianMixture", () => { const m = new ml.GaussianMixture(2, { seed: 3 }); m.fit(X); return m; },
     m => [m.predict(Xt), m.logLikelihood, m.weights, m.means]],
    ["DecisionTreeClassifier", () => { const m = new ml.DecisionTreeClassifier(); m.fit(X, ycls); return m; },
     m => m.predict(Xt)],
    ["DecisionTreeRegressor", () => { const m = new ml.DecisionTreeRegressor({ maxDepth: 4 }); m.fit(X, y); return m; },
     m => m.predict(Xt)],
    ["RandomForestClassifier", () => { const m = new ml.RandomForestClassifier({ nEstimators: 8, seed: 1 }); m.fit(X, ycls); return m; },
     m => m.predict(Xt)],
    ["RandomForestRegressor", () => { const m = new ml.RandomForestRegressor({ nEstimators: 6, maxDepth: 4, seed: 1 }); m.fit(X, y); return m; },
     m => m.predict(Xt)],
    ["GradientBoostingRegressor", () => { const m = new ml.GradientBoostingRegressor({ nEstimators: 10, seed: 1 }); m.fit(X, y); return m; },
     m => m.predict(Xt)],
    ["GradientBoostingClassifier", () => { const m = new ml.GradientBoostingClassifier({ nEstimators: 8, maxDepth: 2, seed: 1 }); m.fit(X, ycls); return m; },
     m => [m.predict(Xt), m.predictProba(Xt), m.apply(Xt)]],
    ["XGBRegressor", () => { const m = new ml.XGBRegressor({ nEstimators: 8, maxDepth: 3, lambda: 2, alpha: 0.5, colsampleByTree: 0.8, seed: 1 }); m.fit(X, y); return m; },
     m => [m.predict(Xt), m.bestRounds]],
    ["XGBClassifier", () => { const m = new ml.XGBClassifier({ nEstimators: 8, maxDepth: 3, gamma: 0.01, seed: 1 }); m.fit(X, ycls); return m; },
     m => [m.predict(Xt), m.predictProba(Xt), m.apply(Xt), m.bestRounds]],
    ["DBScan", () => { const m = new ml.DBScan(0.7, 4); m.fit(X); return m; },
     m => [m.labels, m.nClusters]],
];

{
    for (const [name, build, project] of MODELS) {
        const model = build();
        const want = JSON.stringify(project(model));
        const bytes = model.serialize();
        assert(bytes instanceof Uint8Array && bytes.length > 20,
               name + " serialize returns a record");
        assert(String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]) === "DYNS",
               name + " uses the shared envelope");

        const back = ml[name].deserialize(bytes);
        assert(back.constructor.name === name, name + " decodes to its own class");
        assert(JSON.stringify(project(back)) === want,
               name + ": predictions are not bit-identical after deserialize");

        const path = new Path(`${SCRATCH}/dj_ml_persist_${Date.now() % 10000000}_${name}.dyns`);
        const wrote = model.save(path);
        assert(wrote === bytes.length, name + " save() reports the record length");
        const loaded = ml[name].load(path);
        assert(JSON.stringify(project(loaded)) === want,
               name + ": predictions are not bit-identical after load");

        const again = loaded.serialize();
        assert(again.length === bytes.length, name + " re-encode length");
        let diff = -1;
        for (let i = 0; i < bytes.length; i++)
            if (bytes[i] !== again[i]) { diff = i; break; }
        assert(diff < 0, name + " re-encode differs at byte " + diff);
    }
}

{
    const rfr = MODELS.find(m => m[0] === "RandomForestRegressor")[1]();
    const bytes = rfr.serialize();
    throws(() => ml.RandomForestClassifier.deserialize(bytes), TypeError,
           "a regressor record loaded as a classifier");
    try { ml.RandomForestClassifier.deserialize(bytes); }
    catch (e) {
        assert(/RandomForestRegressor/.test(e.message) &&
               /RandomForestClassifier/.test(e.message),
               "the error names both types, got: " + e.message);
    }
    throws(() => ml.LinearRegression.deserialize(bytes), TypeError,
           "a forest record loaded as a linear model");

    throws(() => ml.KMeans.deserialize(new Uint8Array([68, 89, 78, 83, 1, 0, 1, 0,
                                                       0, 0, 0, 0, 0, 0, 0, 0,
                                                       0, 0, 0, 0, 0, 0, 0, 0])),
           TypeError, "a structures type_id");
}

{
    for (const C of [ml.LinearRegression, ml.KMeans, ml.PCA, ml.GaussianNB,
                     ml.SVC, ml.DecisionTreeClassifier]) {
        const m = C === ml.KMeans ? new C(2) : C === ml.PCA ? new C(1) : new C();
        throws(() => m.serialize(), TypeError, C.name + " unfitted serialize");
        throws(() => m.save(new Path("/tmp/should_not_exist.dyns")), TypeError,
               C.name + " unfitted save");
    }
}

{
    const model = MODELS.find(m => m[0] === "LinearRegression")[1]();
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
    throws(() => ml.LinearRegression.load(new Path("/tmp/definitely/not/here.dyns")),
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
}

{
    const model = new ml.DecisionTreeRegressor({ maxDepth: 3 });
    model.fit(X, y);
    const good = model.serialize();
    function repair(b) {
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    }
    let attempts = 0, decoded = 0, threw = 0;
    const step = Math.max(1, Math.ceil((good.length - 24) / 250));
    for (let i = 20; i < good.length - 4; i += step) {
        for (const mask of [0xff, 0x40]) {
            const b = good.slice();
            b[i] ^= mask;
            repair(b);
            attempts++;
            try {
                const v = ml.DecisionTreeRegressor.deserialize(b);
                decoded++;
                v.predict(Xt);
            } catch (e) {
                threw++;
                if (!(e instanceof Error)) throw new Error("non-Error from deserialize");
            }
        }
    }
    assert(attempts > 100, "forged-tree sweep ran (" + attempts + " cases)");
    assert(decoded > 0, "the sweep must actually reach the reader");
    print("  forged-tree sweep: " + attempts + " records, " + threw +
          " rejected, " + decoded + " decoded and predicted with");
}

{
    function repair(b) {
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    }
    const SMALL = X.slice(0, 30), SY = y.slice(0, 30), SC = ycls.slice(0, 30);
    const St = SMALL.slice(0, 5);
    const cases = [
        ["LinearRegression", () => { const m = new ml.LinearRegression(); m.fit(SMALL, SY); return m; }, m => m.predict(St)],
        ["LogisticRegression", () => { const m = new ml.LogisticRegression(); m.fit(SMALL, SC); return m; }, m => m.predict(St)],
        ["KMeans", () => { const m = new ml.KMeans(2, { seed: 1 }); m.fit(SMALL); return m; }, m => m.predict(St)],
        ["PCA", () => { const m = new ml.PCA(2); m.fit(SMALL); return m; }, m => m.transform(St)],
        ["GaussianNB", () => { const m = new ml.GaussianNB(); m.fit(SMALL, SC); return m; }, m => [m.predict(St), m.predictProba(St)]],
        ["KNClassifier", () => { const m = new ml.KNClassifier(3); m.fit(SMALL, SC); return m; }, m => m.predict(St)],
        ["KNRegressor", () => { const m = new ml.KNRegressor(3); m.fit(SMALL, SY); return m; }, m => m.predict(St)],
        ["StandardScaler", () => { const m = new ml.StandardScaler(); m.fit(SMALL); return m; }, m => m.transform(St)],
        ["MinMaxScaler", () => { const m = new ml.MinMaxScaler(); m.fit(SMALL); return m; }, m => m.transform(St)],
        ["SVC", () => { const m = new ml.SVC({ kernel: "linear", maxIter: 50 }); m.fit(SMALL, SC); return m; }, m => m.predict(St)],
        ["GaussianMixture", () => { const m = new ml.GaussianMixture(2, { seed: 1, maxIter: 10 }); m.fit(SMALL); return m; }, m => m.predict(St)],
        ["DecisionTreeClassifier", () => { const m = new ml.DecisionTreeClassifier({ maxDepth: 2 }); m.fit(SMALL, SC); return m; }, m => m.predict(St)],
        ["DecisionTreeRegressor", () => { const m = new ml.DecisionTreeRegressor({ maxDepth: 2 }); m.fit(SMALL, SY); return m; }, m => m.predict(St)],
        ["RandomForestClassifier", () => { const m = new ml.RandomForestClassifier({ nEstimators: 2, maxDepth: 2, seed: 1 }); m.fit(SMALL, SC); return m; }, m => m.predict(St)],
        ["RandomForestRegressor", () => { const m = new ml.RandomForestRegressor({ nEstimators: 2, maxDepth: 2, seed: 1 }); m.fit(SMALL, SY); return m; }, m => m.predict(St)],
        ["GradientBoostingRegressor", () => { const m = new ml.GradientBoostingRegressor({ nEstimators: 3, maxDepth: 2, seed: 1 }); m.fit(SMALL, SY); return m; }, m => m.predict(St)],
        ["GradientBoostingClassifier", () => { const m = new ml.GradientBoostingClassifier({ nEstimators: 3, maxDepth: 2, seed: 1 }); m.fit(SMALL, SC); return m; }, m => m.predictProba(St)],
        ["XGBRegressor", () => { const m = new ml.XGBRegressor({ nEstimators: 3, maxDepth: 2, seed: 1 }); m.fit(SMALL, SY); return m; }, m => m.predict(St)],
        ["XGBClassifier", () => { const m = new ml.XGBClassifier({ nEstimators: 3, maxDepth: 2, seed: 1 }); m.fit(SMALL, SC); return m; }, m => m.predictProba(St)],
        ["DBScan", () => { const m = new ml.DBScan(0.9, 3); m.fit(SMALL); return m; }, m => m.labels],
    ];
    assert(cases.length === MODELS.length, "the sweep covers every model class");

    let attempts = 0, decoded = 0;
    for (const [name, build, use] of cases) {
        const good = build().serialize();
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
                    use(v);
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
}

{
    const m = new ml.RandomForestClassifier({ nEstimators: 4, seed: 2 });
    m.fit(X, ycls);
    const before = JSON.stringify(m.predict(Xt));
    for (let i = 0; i < 20; i++) m.serialize();
    assert(JSON.stringify(m.predict(Xt)) === before,
           "serializing 20 times did not disturb the model");

    const bytes = m.serialize();
    m.close();
    throws(() => m.serialize(), Error, "serialize after close");
    const back = ml.RandomForestClassifier.deserialize(bytes);
    assert(JSON.stringify(back.predict(Xt)) === before,
           "a record outlives the model it came from");
    back.close();
}

{
    const m = new ml.LinearRegression();
    m.fit(X, y);
    let ran = false;
    const attacker = {
        toString() { ran = true; m.close(); return "/tmp/dyna_ml_attack.dyns"; },
    };
    throws(() => m.save(attacker), TypeError,
           "save() rejects a non-Path before coercing anything");
    assert(!ran, "the toString hook never ran -- save() borrows, it does not coerce");
    assert(m.closed === false, "the model the attacker tried to close is still open");
    const ok = new Path(`${SCRATCH}/dj_ml_attack_ok.${Date.now() % 10000000}.dyns`);
    m.save(ok);
    assert(ml.LinearRegression.load(ok) instanceof ml.LinearRegression,
           "the same call succeeds with a real Path");
}

{
    const m = new ml.KNClassifier(3);
    m.fit(X.slice(0, 30), ycls.slice(0, 30));
    const good = m.serialize();
    const K_OFF = 20 + 4 + 4;
    function forgeK(k) {
        const b = good.slice();
        for (let i = 0; i < 4; i++) b[K_OFF + i] = (k >>> (i * 8)) & 0xff;
        for (let i = 4; i < 8; i++) b[K_OFF + i] = 0;
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    }
    throws(() => ml.KNClassifier.deserialize(forgeK(0)), Error, "k = 0");
    throws(() => ml.KNClassifier.deserialize(forgeK(31)), Error, "k > rows");
    throws(() => ml.KNClassifier.deserialize(forgeK(0x7fffffff)), Error,
           "k = 2^31-1");
    const ok = ml.KNClassifier.deserialize(forgeK(3));
    assert(JSON.stringify(ok.predict(Xt)) === JSON.stringify(m.predict(Xt)),
           "k = 3 still round-trips");
}

{
    function repair(b) {
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    }
    const le64 = (b, off, v) => { for (let i = 0; i < 8; i++) b[off + i] = (v >>> (i * 8)) & 0xff; };
    const le32 = (b, off, v) => { for (let i = 0; i < 4; i++) b[off + i] = (v >>> (i * 8)) & 0xff; };
    const forged = (model, mutate, why) => {
        const b = model.serialize();
        mutate(b);
        repair(b);
        throws(() => model.constructor.deserialize(b), Error, why);
    };
    const km = new ml.KMeans(3, { seed: 7 }); km.fit(X);
    forged(km, b => le64(b, 20 + 0, 0), "kmeans record with k=0 refused");
    const svc = new ml.SVC({ kernel: "rbf" }); svc.fit(X, ycls);
    forged(svc, b => le64(b, 20 + 56, 3), "svm record with ncl/nbin mismatch refused");
    forged(svc, b => le32(b, 20 + 20, 0x7fffffff), "svm record with degree 2^31 refused");
    const dtc = new ml.DecisionTreeClassifier(); dtc.fit(X, ycls);
    forged(dtc, b => le64(b, 20 + 32, 1), "classifier record with n_classes=1 refused");
    forged(dtc, b => le64(b, 20 + 208, 0), "record with a 0-node tree refused");
    const gmm = new ml.GaussianMixture(2, { seed: 3 }); gmm.fit(X);
    forged(gmm, b => le64(b, 20 + 0, 0), "gmm record with k=0 refused");
    const nb = new ml.GaussianNB(); nb.fit(X, ycls);
    forged(nb, b => le64(b, 20 + 8, 0), "nb record with ncl=0 refused");
}

print("test_ml_persist: all " + n + " assertions passed");
