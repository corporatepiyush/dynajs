// 21 · Churn classifier — trains, evaluates, saves and reloads a model that predicts customer churn.
//
// WHAT IT SHOWS
//   - dyna:ml trainTestSplit, StandardScaler, LogisticRegression, RandomForestClassifier
//   - choosing a model by held-out metrics (recall and ROC AUC matter more than accuracy here)
//   - scaling fitted on the training rows only, then reused: no leakage from the test set
//   - save/load: the artifact a batch job trains and a service loads
//
// RUN      dynajs examples/apps/21-churn-classifier.js

import { trainTestSplit, StandardScaler, LogisticRegression, RandomForestClassifier,
         accuracy, precision, recall, f1, rocAuc, confusionMatrix } from "dyna:ml";
import { Random } from "dyna:random";
import { makeTempDir, removeAll, stat } from "dyna:file";

// ---- data: [tenure months, monthly spend, support tickets, days since login] --
// Synthetic but shaped like the real thing: churn is likelier for new, unhappy,
// inactive customers, with noise so no model can be perfect.
const rng = new Random(7);
const X = [], y = [];
for (let i = 0; i < 1500; i++) {
    const tenure = rng.nextBounded(60) + 1;
    const spend = 20 + rng.nextFloat() * 100;
    const tickets = rng.poisson(1.2);
    const idle = rng.nextBounded(45);
    const risk = -2.2 - 0.05 * tenure + 0.55 * tickets + 0.09 * idle - 0.006 * spend;
    X.push([tenure, spend, tickets, idle]);
    y.push(rng.nextFloat() < 1 / (1 + Math.exp(-risk)) ? 1 : 0);
}
const churnRate = y.reduce((a, b) => a + b, 0) / y.length;

// ---- split and scale -------------------------------------------------------
const split = trainTestSplit(y.length, { testSize: 0.25, shuffle: true, seed: 1 });
const rows = (idx) => idx.map((i) => X[i]);
const labels = (idx) => idx.map((i) => y[i]);
const Xtrain = rows(split.train), ytrain = labels(split.train);
const Xtest = rows(split.test), ytest = labels(split.test);

// Fit the scaler on training rows ONLY. Fitting it on everything would let
// test-set statistics leak into training and flatter the score.
const scaler = new StandardScaler().fit(Xtrain);
const XtrainS = scaler.transform(Xtrain), XtestS = scaler.transform(Xtest);

// ---- train two candidates --------------------------------------------------
const logit = new LogisticRegression({ maxIter: 2000 }).fit(XtrainS, ytrain);
const forest = new RandomForestClassifier({ nEstimators: 60, maxDepth: 8, seed: 3 }).fit(Xtrain, ytrain);

function evaluate(name, predicted, scores) {
    const [[tn, fp], [fn, tp]] = confusionMatrix(ytest, predicted);
    return {
        name, accuracy: accuracy(ytest, predicted), precision: precision(ytest, predicted),
        recall: recall(ytest, predicted), f1: f1(ytest, predicted), auc: rocAuc(ytest, scores),
        confusion: { tn, fp, fn, tp },
    };
}
const positive = (probaRows) => probaRows.map((p) => p[1]);     // P(churn) per row
const results = [
    evaluate("logistic", logit.predict(XtestS), positive(logit.predictProba(XtestS))),
    evaluate("forest", forest.predict(Xtest), positive(forest.predictProba(Xtest))),
];

console.log(`customers: ${y.length}, churn rate ${(churnRate * 100).toFixed(1)}%, test rows ${ytest.length}`);
for (const r of results)
    console.log(`  ${r.name.padEnd(9)} acc ${r.accuracy.toFixed(3)}  precision ${r.precision.toFixed(3)}  ` +
                `recall ${r.recall.toFixed(3)}  f1 ${r.f1.toFixed(3)}  auc ${r.auc.toFixed(3)}`);
console.log("  forest feature importances [tenure, spend, tickets, idle]:",
            forest.featureImportances.map((v) => v.toFixed(2)).join(" "));

// ---- persist the winner and prove the reloaded model is the same model ------
const dir = makeTempDir("model");
const best = results[0].auc >= results[1].auc ? "logistic" : "forest";
const modelPath = dir.join("churn.model"), scalerPath = dir.join("churn.scaler");
(best === "logistic" ? logit : forest).save(modelPath);
scaler.save(scalerPath);

const loadedScaler = StandardScaler.load(scalerPath);
const loaded = best === "logistic" ? LogisticRegression.load(modelPath) : RandomForestClassifier.load(modelPath);
const customer = [[3, 35, 4, 30]];              // new, cheap plan, 4 tickets, idle a month
const input = best === "logistic" ? loadedScaler.transform(customer) : customer;
const risk = loaded.predictProba(input)[0][1];
console.log(`saved ${best} (${stat(modelPath).size} bytes); at-risk customer scores ${(risk * 100).toFixed(0)}% churn`);

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
check(results.every((r) => r.auc > 0.7), "both models beat chance clearly");
check(results.every((r) => r.accuracy > 1 - churnRate - 0.05), "accuracy is near or above the majority baseline");
const original = best === "logistic" ? logit.predictProba(XtestS) : forest.predictProba(Xtest);
const reloaded = loaded.predictProba(best === "logistic" ? loadedScaler.transform(Xtest) : Xtest);
check(original.every((p, i) => Math.abs(p[1] - reloaded[i][1]) < 1e-12), "the reloaded model predicts identically");
check(risk > churnRate, "an obviously at-risk customer scores above the base rate");
console.log("self-test passed");
for (const m of [scaler, logit, forest, loaded, loadedScaler]) m.close();
removeAll(dir);
