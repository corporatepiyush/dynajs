// covers: dyna:ml.r2Score
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_r2_score_xgb");
import { XGBRegressor, XGBClassifier, GradientBoostingRegressor, accuracy, r2Score } from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(a, b, eps, m) {
    n++;
    if (!(Math.abs(a - b) <= eps))
        throw new Error((m || "not close") + ": " + a + " vs " + b);
}
async function throws(fn, kind, m) {
    n++;
    try { await fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return String(e);
    }
    throw new Error((m || "expected a throw") + " but none happened");
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function linearData(seed, rows, noise) {
    const rnd = lcg(seed), X = [], y = [], yc = [];
    for (let i = 0; i < rows; i++) {
        const a = rnd() * 4 - 2, b = rnd() * 4 - 2, c = rnd() * 4 - 2;
        const s = 2 * a - b + 0.5 * c;
        X.push([a, b, c]);
        y.push(s + (rnd() * 2 - 1) * noise);
        yc.push(1 / (1 + Math.exp(-s)) > rnd() ? 1 : 0);
    }
    return { X, y, yc };
}

await __kitS.section("regressor fits", async () => {
    const d = linearData(7, 400, 0.3);
    const r = await new XGBRegressor({ nEstimators: 60, maxDepth: 4 }).fit(d.X, d.y);
    assert((await r2Score(d.y, await r.predict(d.X))) > 0.99, "regressor fits");
    const g = await new GradientBoostingRegressor({ nEstimators: 60, maxDepth: 4 }).fit(d.X, d.y);
    assert((await r2Score(d.y, await r.predict(d.X))) >= (await r2Score(d.y, await g.predict(d.X))) - 0.01,
           "and is not worse than the first-order booster it generalises");

    const c = await new XGBClassifier({ nEstimators: 60, maxDepth: 4 }).fit(d.X, d.yc);
    assert((await accuracy(d.yc, await c.predict(d.X))) > 0.95, "classifier fits");
    const P = await c.predictProba(d.X);
    assert(P.length === d.X.length && P[0].length === 2, "proba shape");
    for (let i = 0; i < 20; i++)
        close(P[i][0] + P[i][1], 1, 1e-12, "proba row sums to 1");

    const lab = await c.predict(d.X);
    let agree = 0;
    for (let i = 0; i < lab.length; i++)
        if (lab[i] === (P[i][1] > P[i][0] ? 1 : 0)) agree++;
    assert(agree === lab.length, "predict === argmax(predictProba)");

    const imp = r.featureImportances;
    assert(imp[0] > imp[1] && imp[1] > imp[2], "importance order: " + imp);
});

await __kitS.section("lambda must not increase the training fit:", async () => {
    const d = linearData(11, 500, 0.1);
    const fit = async p => await new XGBRegressor({ nEstimators: 100, maxDepth: 4, ...p }).fit(d.X, d.y);

    let prev = Infinity;
    for (const l of [0, 1, 10, 100, 1000]) {
        const s = await r2Score(d.y, await (await fit({ lambda: l })).predict(d.X));
        assert(s <= prev + 1e-9, "lambda must not increase the training fit: " + l);
        prev = s;
    }
    prev = Infinity;
    for (const a of [0, 1, 10]) {
        const s = await r2Score(d.y, await (await fit({ alpha: a })).predict(d.X));
        assert(s <= prev + 1e-9, "alpha must not increase the training fit: " + a);
        prev = s;
    }
    const flat = await fit({ gamma: 1e9 });
    assert(flat.depth === 0, "gamma=1e9 must leave every tree a single leaf");
    close(await r2Score(d.y, await flat.predict(d.X)), 0, 1e-12, "and predict the mean");

    const wide = await fit({ gamma: 100 });
    assert(wide.depth < 4, "a large gamma must prune depth: " + wide.depth);
});
__kitS.end();

print("test_ml_xgb: all " + n + " assertions passed");
