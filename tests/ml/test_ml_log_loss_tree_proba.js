// covers: dyna:ml.logLoss
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_log_loss_tree_proba");
import {
    DecisionTreeClassifier, DecisionTreeRegressor,
    RandomForestClassifier, RandomForestRegressor, GradientBoostingRegressor,
    logLoss, rocAuc, averagePrecision, accuracy,
} from "dyna:ml";

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
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

const rnd = lcg(31337);
const X = [], y = [];
for (let i = 0; i < 400; i++) {
    const a = rnd() * 4 - 2, b = rnd() * 4 - 2, noise = rnd() * 4 - 2;
    const p = 1 / (1 + Math.exp(-(a + 2 * b)));
    X.push([a, b, noise]);
    y.push(rnd() < p ? 1 : 0);
}
const Xt = X.slice(0, 60), yt = y.slice(0, 60);

await __kitS.section("logLoss is finite:", async () => {
    const rf = new RandomForestClassifier({ nEstimators: 40, maxDepth: 6, seed: 2 });
    await rf.fit(X, y);
    const score = (await rf.predictProba(Xt)).map(r => r[1]);
    const ll = await logLoss(yt, score);
    const auc = await rocAuc(yt, score);
    const ap = await averagePrecision(yt, score);
    assert(ll > 0 && isFinite(ll), "logLoss is finite: " + ll);
    assert(auc > 0.8 && auc <= 1, "rocAuc on separable-ish data: " + auc);
    assert(ap > 0.8 && ap <= 1, "averagePrecision: " + ap);
    assert((await accuracy(yt, await rf.predict(Xt))) > 0.8, "accuracy sanity");
});
__kitS.end();

print("test_ml_tree_proba: all " + n + " assertions passed");
