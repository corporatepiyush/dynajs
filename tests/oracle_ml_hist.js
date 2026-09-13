/* oracle_ml_hist.js -- the histogram split finder against the exact one
 * (W9.8a; production-plan item 4).
 *
 * THE CLAIM UNDER TEST, and it is an equality rather than an approximation:
 *
 *   A feature with d distinct values has exactly d-1 midpoints between
 *   consecutive values. Binning with maxBins >= d gives one bin per distinct
 *   value, and a split after bin b is placed between the values in the nearest
 *   NON-EMPTY bins either side of it -- the same two values the sorted sweep
 *   brackets. So the two finders enumerate the same candidates AND put the
 *   threshold in the same place. When the statistics over that candidate set
 *   are exact -- which for a classifier they are, Gini being a function of
 *   integer class counts -- the two must build the same tree.
 *
 * So the classification arm asserts BIT-IDENTICAL models: same leaf for every
 * row (apply), same leaf distributions (predictProba), same importances, same
 * depth. Nothing is compared with a tolerance.
 *
 * The regression arm cannot claim that everywhere and does not pretend to. The
 * exact sweep adds y values one at a time in sorted order; the histogram sums
 * each bin and prefix-sums the bins. Floating-point addition is not
 * associative, so two gains that are equal in exact arithmetic can differ in
 * the last bits, and a near-tie between two FEATURES can then fall the other
 * way. Measured: with all features considered at every node (a single tree, or
 * boosting) that never happens across 72 fits; with maxFeatures subsampling
 * (a forest) it happens a couple of times in 36, and the two models score
 * within 0.002 r2 of each other.
 *
 * THE COUNT IS BOUNDED RATHER THAN PINNED, and the reason is a platform fact:
 * clang defaults to -ffp-contract=on, arm64 has an FMA instruction and the
 * x86-64 baseline does not, so `sum += w * v` is one rounding on one and two
 * on the other. WHICH near-ties flip therefore differs by architecture -- 2 of
 * 36 on arm64 and 3 of 36 on x86-64. tools/xplat-verify.sh found that by
 * failing an exact pin, which is what it is for.
 *
 * THE ORACLE IS VALIDATED BY ITS OWN NEGATIVE: with maxBins deliberately below
 * the distinct count the comparison must FAIL. A test that only ever sees
 * agreement cannot tell agreement from a comparison that examines nothing.
 */
import {
    DecisionTreeClassifier, DecisionTreeRegressor,
    RandomForestClassifier, RandomForestRegressor,
    GradientBoostingRegressor, GradientBoostingClassifier,
    accuracy, r2Score,
} from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(a, b, eps, m) {
    n++;
    if (!(Math.abs(a - b) <= eps))
        throw new Error((m || "not close") + ": " + a + " vs " + b);
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

/* Data whose every column takes at most `levels` distinct values, so a fit with
 * maxBins >= levels is in the exactly-equivalent regime. The label stays a
 * noisy function of the features: pure classes make every tree a stump, which
 * would agree trivially. */
function gridData(seed, rows, cols, levels, classes) {
    const rnd = lcg(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const row = [];
        let s = 0;
        for (let j = 0; j < cols; j++) {
            const v = Math.floor(rnd() * levels);
            row.push(v * 0.25 - 1);          /* distinct, not consecutive ints */
            s += (j + 1) * v;
        }
        X.push(row);
        y.push(classes ? Math.floor((s + rnd() * levels) % classes)
                       : s + rnd() * 2 - 1);
    }
    return [X, y];
}

/* Continuous data: every value distinct, so any maxBins below `rows` quantises.
 * This is the regime the equivalence claim does NOT cover. */
function contData(seed, rows, cols, classes) {
    const rnd = lcg(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const row = [];
        let s = 0;
        for (let j = 0; j < cols; j++) {
            const v = rnd() * 4 - 2;
            row.push(v);
            s += (j + 1) * v;
        }
        X.push(row);
        y.push(classes ? (1 / (1 + Math.exp(-s)) > rnd() ? 1 : 0) : s + rnd() * 0.2);
    }
    return [X, y];
}

/* A model's structural fingerprint, taken through the public API only:
 * which leaf every training row lands in, what that leaf predicts, and how the
 * split gains were attributed. Two trees with the same fingerprint over the
 * training set make the same decisions about it. */
function fingerprint(model, X, proba) {
    const parts = [];
    const leaves = model.apply(X);
    for (const row of leaves) parts.push(Array.isArray(row) ? row.join(",") : String(row));
    const imp = model.featureImportances;
    for (const v of imp) parts.push(v.toExponential(17));
    const pred = proba ? model.predictProba(X) : model.predict(X);
    for (const row of pred)
        parts.push(Array.isArray(row) ? row.map(v => v.toExponential(17)).join(",")
                                      : row.toExponential(17));
    parts.push("depth=" + model.depth);
    return parts.join("|");
}

/* ==================================================================== *
 *  1. Classification: identical, bit for bit, whenever maxBins >= levels
 * ==================================================================== */
let cases = 0, mismatches = 0;
for (const levels of [2, 5, 16, 40]) {
    for (const cols of [1, 3, 7]) {
        for (const seed of [1, 7, 4242]) {
            for (const classes of [2, 3]) {
                const [X, y] = gridData(seed, 240, cols, levels, classes);
                for (const make of [
                    o => new DecisionTreeClassifier({ maxDepth: 6, ...o }),
                    o => new DecisionTreeClassifier({ maxDepth: 0, minSamplesLeaf: 3, ...o }),
                    o => new RandomForestClassifier({ nEstimators: 8, maxDepth: 5, seed: 3, ...o }),
                ]) {
                    const exact = make({}).fit(X, y);
                    const hist = make({ maxBins: Math.max(levels, 2) }).fit(X, y);
                    cases++;
                    if (fingerprint(exact, X, true) !== fingerprint(hist, X, true))
                        mismatches++;
                }
            }
        }
    }
}
assert(cases === 216, "case count: " + cases);
assert(mismatches === 0, "classification equivalence: " + mismatches + "/" + cases + " differ");

/* The negative control. Two bins cannot represent 40 levels, so the fingerprints
 * MUST diverge -- if they do not, `fingerprint` is comparing nothing. */
{
    const [X, y] = gridData(11, 240, 3, 40, 3);
    const exact = new DecisionTreeClassifier({ maxDepth: 6 }).fit(X, y);
    const coarse = new DecisionTreeClassifier({ maxDepth: 6, maxBins: 2 }).fit(X, y);
    assert(fingerprint(exact, X, true) !== fingerprint(coarse, X, true),
           "maxBins:2 over 40 levels must NOT reproduce the exact tree");
}

/* ==================================================================== *
 *  2. Regression: same structure, and the divergence count is reported
 * ==================================================================== */
{
    const makers = {
        /* Every feature is considered at every node, so no cross-feature tie
         * exists to be broken differently. */
        tree:     o => new DecisionTreeRegressor({ maxDepth: 6, ...o }),
        boosting: o => new GradientBoostingRegressor({ nEstimators: 10, maxDepth: 3, ...o }),
        /* maxFeatures defaults to cols/3, so this one draws a subset per node
         * and CAN see two features tie. */
        forest:   o => new RandomForestRegressor({ nEstimators: 8, maxDepth: 5, seed: 3, ...o }),
    };
    const diverged = { tree: 0, boosting: 0, forest: 0 };
    const total = { tree: 0, boosting: 0, forest: 0 };
    let worstR2 = 0;
    for (const levels of [2, 5, 16, 40]) {
        for (const cols of [1, 3, 7]) {
            for (const seed of [2, 9, 777]) {
                const [X, y] = gridData(seed, 240, cols, levels, 0);
                for (const [name, make] of Object.entries(makers)) {
                    const exact = make({}).fit(X, y);
                    const hist = make({ maxBins: Math.max(levels, 2) }).fit(X, y);
                    total[name]++;
                    const a = exact.predict(X), b = hist.predict(X);
                    let worst = 0;
                    for (let i = 0; i < a.length; i++)
                        worst = Math.max(worst, Math.abs(a[i] - b[i]));
                    if (worst > 1e-9) {
                        diverged[name]++;
                        worstR2 = Math.max(worstR2,
                            Math.abs(r2Score(y, a) - r2Score(y, b)));
                    }
                }
            }
        }
    }
    assert(total.tree === 36 && total.boosting === 36 && total.forest === 36,
           "regression case counts");
    assert(diverged.tree === 0, "single trees must agree exactly: " + diverged.tree);
    assert(diverged.boosting === 0, "boosting must agree exactly: " + diverged.boosting);
    /* BOUNDED, NOT PINNED, and the reason is measured rather than assumed.
     *
     * An exact count is not portable. clang defaults to -ffp-contract=on, and
     * arm64 has an FMA instruction while the x86-64 baseline (SSE2) does not:
     * `sum += w * v` is ONE rounding on arm64 and TWO on x86-64. Verified --
     * the same expression compiles to `fmadd` on arm64 and to a multiply plus
     * an add on x86-64, and .obj/dyna-ml.o carries 125 fused instructions. So
     * the last bits differ by platform, and WHICH near-ties flip differs with
     * them: this data gives 2 of 36 on arm64 and 3 of 36 on x86-64. The
     * cross-platform gate found that by failing, which is what it is for.
     *
     * A bound still catches the failure that matters. Tie-breaking can move a
     * handful of fits; a broken finder moves all 36. And the assertion below
     * it is the actual claim -- a flipped tie must not change what the model
     * is worth -- which is exact arithmetic on a score and portable. */
    assert(diverged.forest <= 6,
           "forest divergence is a few near-ties out of 36, saw " + diverged.forest);
    assert(diverged.forest > 0,
           "and it is not zero on this data -- a zero here means the two "
           + "finders stopped being compared at all");
    assert(worstR2 < 0.01,
           "a flipped near-tie must not change what the model is worth: " + worstR2);
}

/* ==================================================================== *
 *  3. Quantised regime: not identical, but not worse either
 * ==================================================================== */
{
    const [X, y] = contData(5150, 600, 5, 2);
    const exact = new RandomForestClassifier({ nEstimators: 25, maxDepth: 8, seed: 4 }).fit(X, y);
    const hist = new RandomForestClassifier({ nEstimators: 25, maxDepth: 8, seed: 4, maxBins: 32 }).fit(X, y);
    const ae = accuracy(y, exact.predict(X)), ah = accuracy(y, hist.predict(X));
    assert(ah >= ae - 0.05, "32 bins over 600 distinct values: " + ah + " vs " + ae);

    const [Xr, yr] = contData(6161, 600, 5, 0);
    const er = new RandomForestRegressor({ nEstimators: 25, maxDepth: 8, seed: 4 }).fit(Xr, yr);
    const hr = new RandomForestRegressor({ nEstimators: 25, maxDepth: 8, seed: 4, maxBins: 32 }).fit(Xr, yr);
    assert(r2Score(yr, hr.predict(Xr)) >= r2Score(yr, er.predict(Xr)) - 0.05,
           "binned regressor r2 must track the exact one");
}

/* ==================================================================== *
 *  4. A binned fit is still deterministic, still persistable
 * ==================================================================== */
{
    const [X, y] = contData(31337, 300, 4, 3);
    const a = new RandomForestClassifier({ nEstimators: 12, maxDepth: 6, seed: 99, maxBins: 24 }).fit(X, y);
    const b = new RandomForestClassifier({ nEstimators: 12, maxDepth: 6, seed: 99, maxBins: 24 }).fit(X, y);
    assert(fingerprint(a, X, true) === fingerprint(b, X, true),
           "same seed and bins must reproduce the fit exactly");

    const rec = a.serialize();
    const back = RandomForestClassifier.deserialize(rec);
    assert(fingerprint(back, X, true) === fingerprint(a, X, true),
           "a binned model must survive the round trip bit-identically");
    const again = back.serialize();
    assert(again.length === rec.length, "re-encode length");
    let same = true;
    for (let i = 0; i < rec.length; i++) if (rec[i] !== again[i]) same = false;
    assert(same, "re-encoding a decoded binned model must be byte-identical");
}

/* ==================================================================== *
 *  5. Degenerate columns the binner has to survive
 * ==================================================================== */
{
    /* A constant column has one distinct value and therefore ONE bin: there is
     * no interior edge to split on, and a finder that assumed nbin >= 2 would
     * read the edge array out of range. */
    const X = [], y = [];
    for (let i = 0; i < 60; i++) { X.push([1.5, i % 7, 0]); y.push(i % 2); }
    const m = new DecisionTreeClassifier({ maxDepth: 4, maxBins: 16 }).fit(X, y);
    assert(m.featureImportances[0] === 0, "a constant column earns no importance");
    assert(m.predict([[1.5, 3, 0]]).length === 1, "predicts");

    /* One row, and a column of two adjacent doubles whose midpoint is not
     * strictly between them -- the case dyn_hist_mid backs off for. */
    const tiny = new DecisionTreeRegressor({ maxBins: 8 }).fit([[1], [1]], [2, 2]);
    close(tiny.predict([[1]])[0], 2, 0, "constant target");
    const adj = Number.MIN_VALUE * 4;
    const near = new DecisionTreeRegressor({ maxBins: 8 })
        .fit([[adj], [adj * 2], [adj * 3], [adj * 4]], [0, 0, 1, 1]);
    assert(Number.isFinite(near.predict([[adj]])[0]), "adjacent doubles bin cleanly");
}

/* ==================================================================== *
 *  6. maxBins is validated, not clamped
 * ==================================================================== */
{
    let threw = 0;
    for (const bad of [1, 256, 100000]) {
        try { new DecisionTreeClassifier({ maxBins: bad }); } catch (e) { threw++; }
    }
    assert(threw === 3, "out-of-range maxBins must throw, not clamp");
    assert(new DecisionTreeClassifier({ maxBins: 0 }) instanceof DecisionTreeClassifier,
           "0 selects the exact splitter");
    assert(new DecisionTreeClassifier({ maxBins: 255 }) instanceof DecisionTreeClassifier,
           "255 is the maximum, because a bin code and its missing slot are one byte");
}

/* ==================================================================== *
 *  7. Boosting inherits the binning through its regression trees
 * ==================================================================== */
/* A boosted CLASSIFIER is the one place the two finders reliably disagree, and
 * the reason is worth stating because it is not quantisation. Its first round
 * fits the gradient of the deviance at a CONSTANT prediction, so the target
 * takes exactly two values -- and a two-valued target makes many candidate
 * splits score identically in exact arithmetic. Those are decided by the last
 * bits, which the two accumulation orders do not share. The models stay
 * equivalent: every predicted label agrees, and so does accuracy. */
{
    const [X, y] = gridData(808, 300, 4, 12, 3);
    const a = new GradientBoostingClassifier({ nEstimators: 15, maxDepth: 3 }).fit(X, y);
    const b = new GradientBoostingClassifier({ nEstimators: 15, maxDepth: 3, maxBins: 12 }).fit(X, y);
    const pa = a.predictProba(X), pb = b.predictProba(X);
    let worst = 0;
    for (let i = 0; i < pa.length; i++)
        for (let k = 0; k < pa[i].length; k++)
            worst = Math.max(worst, Math.abs(pa[i][k] - pb[i][k]));
    assert(worst < 0.05, "binned boosting stays close: " + worst);
    const la = a.predict(X), lb = b.predict(X);
    let same = 0;
    for (let i = 0; i < la.length; i++) if (la[i] === lb[i]) same++;
    assert(same === la.length, "every predicted label must still agree: " +
                               same + "/" + la.length);
    close(accuracy(y, la), accuracy(y, lb), 0, "and so must accuracy, exactly");

    /* The regressor has a continuous target and therefore no dense ties: it
     * agrees to the last bits, which is what makes the explanation above a
     * diagnosis rather than an excuse. */
    const [Xr, yr] = gridData(809, 300, 4, 12, 0);
    const ra = new GradientBoostingRegressor({ nEstimators: 15, maxDepth: 3 }).fit(Xr, yr);
    const rb = new GradientBoostingRegressor({ nEstimators: 15, maxDepth: 3, maxBins: 12 }).fit(Xr, yr);
    const va = ra.predict(Xr), vb = rb.predict(Xr);
    let rworst = 0;
    for (let i = 0; i < va.length; i++) rworst = Math.max(rworst, Math.abs(va[i] - vb[i]));
    assert(rworst <= 1e-9, "boosted regression must match exactly: " + rworst);
}

print("oracle_ml_hist: all " + n + " assertions passed (" + cases +
      " classification equivalence cases, 0 mismatches)");
