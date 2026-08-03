/* test_ml_persist.js -- model persistence for every fitted class in dyna:ml
 * (STDLIB_OOP_PLAN W9.1; bench/ML_PRODUCTION_PLAN.md gap #1).
 *
 * The gate is **bit-identical predictions**, not "close": a model that comes
 * back a few ULP different is a model whose scores, thresholds and downstream
 * decisions have all silently moved. That is why the record stores IEEE-754 bit
 * patterns rather than text, and why every check below compares whole
 * prediction vectors through JSON rather than with a tolerance.
 *
 * Also covered, because a persistence format is an untrusted-input surface the
 * moment anyone calls `load()`:
 *   - a record of the wrong model type is refused by name, not reinterpreted;
 *   - a corrupted record throws;
 *   - a forged tree node cannot make predict() walk off the node array.
 */
import * as ml from "dyna:ml";
import { CRC32C } from "dyna:hash";
import { Path } from "dyna:file";

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

/* A deterministic dataset: the oracle is the model's own output, so the input
 * must not move between runs. */
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

/* name -> [fitted model, projection of everything observable about it] */
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

/* ==================================================================== *
 *  1. Bit-identical predictions after a round trip, in memory and on disk
 * ==================================================================== */
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

        const path = new Path("/tmp/dyna_ml_persist_" + name + ".dyns");
        const wrote = model.save(path);
        assert(wrote === bytes.length, name + " save() reports the record length");
        const loaded = ml[name].load(path);
        assert(JSON.stringify(project(loaded)) === want,
               name + ": predictions are not bit-identical after load");

        /* Re-serializing what was loaded must reproduce the record exactly. */
        const again = loaded.serialize();
        assert(again.length === bytes.length, name + " re-encode length");
        let diff = -1;
        for (let i = 0; i < bytes.length; i++)
            if (bytes[i] !== again[i]) { diff = i; break; }
        assert(diff < 0, name + " re-encode differs at byte " + diff);
    }
}

/* ==================================================================== *
 *  2. A record knows what it is
 * ==================================================================== */
{
    const rfr = MODELS.find(m => m[0] === "RandomForestRegressor")[1]();
    const bytes = rfr.serialize();
    /* The regressor and the classifier share a struct and a codec; only the
     * type_id distinguishes them. Loading one as the other would reinterpret
     * leaf values as class labels and silently produce nonsense. */
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

    /* And a structures record is not an ML record. */
    throws(() => ml.KMeans.deserialize(new Uint8Array([68, 89, 78, 83, 1, 0, 1, 0,
                                                       0, 0, 0, 0, 0, 0, 0, 0,
                                                       0, 0, 0, 0, 0, 0, 0, 0])),
           TypeError, "a structures type_id");
}

/* ==================================================================== *
 *  3. An unfitted model has nothing to save, and says so
 * ==================================================================== */
{
    for (const C of [ml.LinearRegression, ml.KMeans, ml.PCA, ml.GaussianNB,
                     ml.SVC, ml.DecisionTreeClassifier]) {
        const m = C === ml.KMeans ? new C(2) : C === ml.PCA ? new C(1) : new C();
        throws(() => m.serialize(), TypeError, C.name + " unfitted serialize");
        throws(() => m.save(new Path("/tmp/should_not_exist.dyns")), TypeError,
               C.name + " unfitted save");
    }
}

/* ==================================================================== *
 *  4. Corrupted and hostile records
 * ==================================================================== */
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

    /* Trailing bytes: a record that does not consume exactly its payload does
     * not describe what it claims. */
    {
        const b = new Uint8Array(good.length + 8);
        b.set(good.subarray(0, good.length - 4));
        /* extend payload_len by 8 and re-checksum */
        const plen = good.length - 24 + 8;
        for (let i = 0; i < 8; i++) b[12 + i] = (plen >>> (i * 8)) & 0xff;
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        throws(() => ml.LinearRegression.deserialize(b), Error,
               "a record with unconsumed trailing bytes");
    }
}

/* ==================================================================== *
 *  5. A forged tree cannot walk off its node array
 *
 *  The forest reader validates every child index against the node count. This
 *  repairs the checksum after mutating -- otherwise the CRC rejects everything
 *  and the reader is never reached at all, which is the trap recorded for
 *  W7.3.
 * ==================================================================== */
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
                v.predict(Xt);        /* walk the forged tree */
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

/* ==================================================================== *
 *  5b. The same sweep across EVERY model reader
 *
 *  The forged-tree sweep above found a calloc-overflow because it reached one
 *  reader. Sixteen others have the same shape -- a 64-bit dimension out of a
 *  record driving an allocation -- so they get the same treatment. Under ASan
 *  this is what turns "the reader validates its lengths" from a claim into a
 *  test. Each decoded model is then USED, because a reader that produced an
 *  internally inconsistent model must not be able to hide behind never being
 *  called.
 * ==================================================================== */
{
    function repair(b) {
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    }
    /* Small fits, so the sweep stays well under a second: the reader does not
     * care how well the model was trained. */
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
            /* 0xff and 0x80 between them reach both the low bits of a small
             * dimension and the high bits that make a 64-bit product wrap --
             * which is the mutation that found the calloc-overflow. */
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

/* ==================================================================== *
 *  6. Persistence does not disturb the model, and survives close()
 * ==================================================================== */
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

/* ==================================================================== *
 *  7. The adversarial coercion attack on save(path) -- and why it can no
 *     longer fire at all
 *
 *  This test USED to be the real thing: save() coerced its path with
 *  ToString, so an object whose toString() closed the model mid-coercion
 *  could produce a use-after-free, and the test hooked `toString` (never
 *  `valueOf`, which this path would not have run -- CLAUDE.md section 8).
 *
 *  save() now takes a Path, which is BORROWED and not coerced. No user JS
 *  runs anywhere inside the call, so the hazard is gone by construction
 *  rather than by ordering. That makes the old test dangerous in a new way:
 *  it still throws, so it still PASSES, but now for the wrong reason -- the
 *  attacker is rejected as "not a Path" before its toString is ever
 *  consulted. A test that passes without exercising anything is the exact
 *  failure mode CLAUDE.md section 8 records for `{valueOf(){...}}` against a
 *  ToString API.
 *
 *  So the assertion is inverted to pin the REASON: the hook must NOT run,
 *  and the model must still be open afterwards. This is the same shape as
 *  the Compressor finding -- a type-checked input means a busy flag would be
 *  a bypass that never fires, and the test says so.
 * ==================================================================== */
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
    /* And the borrowed handle really does work, so the rejection above is a
     * type check and not a broken code path. */
    const ok = new Path("/tmp/dyna_ml_attack_ok.dyns");
    m.save(ok);
    assert(ml.LinearRegression.load(ok) instanceof ml.LinearRegression,
           "the same call succeeds with a real Path");
}

/* ==================================================================== *
 *  8. Regression: an unbacked dimension is a denial of service
 *
 *  `k` in a k-NN record is not backed by any payload bytes, so nothing in the
 *  envelope bounds it. A forged k made a 1 KB record take 25 SECONDS to
 *  predict five rows -- the sweep above found it as a timing outlier, not as a
 *  crash. The reader now requires 1 <= k <= rows, which is the same rule fit()
 *  applies.
 * ==================================================================== */
{
    const m = new ml.KNClassifier(3);
    m.fit(X.slice(0, 30), ycls.slice(0, 30));
    const good = m.serialize();
    /* Layout: regressor u32 | weighted u32 | k u64 | rows u64 | cols u64 */
    const K_OFF = 20 + 4 + 4;
    /* JS `>>>` takes its shift mod 32, so the high four bytes have to be
     * written separately or they silently repeat the low ones. */
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
    /* And the honest value still loads. */
    const ok = ml.KNClassifier.deserialize(forgeK(3));
    assert(JSON.stringify(ok.predict(Xt)) === JSON.stringify(m.predict(Xt)),
           "k = 3 still round-trips");
}

print("test_ml_persist: all " + n + " assertions passed");
