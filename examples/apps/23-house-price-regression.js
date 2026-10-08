// 23 · Price regression — compares a linear model with gradient boosting using cross-validation.
//
// WHAT IT SHOWS
//   - dyna:ml LinearRegression, GradientBoostingRegressor, crossValScore and gridSearch
//   - reading coefficients as "price per unit" and checking them for sense
//   - a non-linear effect (a premium that only applies in one district) that a
//     straight line cannot express and trees can
//   - reporting error in currency, not just R²
//
// RUN      dynajs examples/apps/23-house-price-regression.js

import { LinearRegression, GradientBoostingRegressor, crossValScore, gridSearch,
         trainTestSplit, meanAbsoluteError, r2Score } from "dyna:ml";
import { Random } from "dyna:random";

// ---- data: [area m², rooms, age years, distance to centre km, district 0..2] --
const rng = new Random(2024);
const X = [], y = [];
for (let i = 0; i < 900; i++) {
    const area = 35 + rng.nextFloat() * 165;
    const rooms = Math.max(1, Math.round(area / 28 + rng.normal(0, 0.7)));
    const age = rng.nextBounded(80);
    const dist = rng.nextFloat() * 25;
    const district = rng.nextBounded(3);
    // The waterfront district (2) adds a premium that GROWS with area: an
    // interaction, which is exactly what a linear model has no term for.
    const premium = district === 2 ? 900 * area : 0;
    const price = 40000 + 2800 * area + 9000 * rooms - 700 * age - 4200 * dist + premium + rng.normal(0, 15000);
    X.push([area, rooms, age, dist, district]);
    y.push(price);
}
const split = trainTestSplit(y.length, { testSize: 0.2, shuffle: true, seed: 9 });
const pick = (rows, idx) => idx.map((i) => rows[i]);
const Xtrain = pick(X, split.train), ytrain = pick(y, split.train);
const Xtest = pick(X, split.test), ytest = pick(y, split.test);

// ---- model 1: linear -------------------------------------------------------
const linear = new LinearRegression().fit(Xtrain, ytrain);
const linearPred = linear.predict(Xtest);
const names = ["area", "rooms", "age", "distance", "district"];
console.log("linear coefficients (currency per unit):");
names.forEach((n, i) => console.log(`  ${n.padEnd(9)} ${linear.coef[i].toFixed(0)}`));

// ---- model 2: boosting, with a small grid search ----------------------------
// gridSearch cross-validates every combination and returns the best one.
const negMae = (yt, yp) => -meanAbsoluteError(yt, yp);       // higher is better for the search
const search = gridSearch(
    (params) => new GradientBoostingRegressor({ ...params, seed: 1 }),
    Xtrain, ytrain,
    { nEstimators: [80, 160], maxDepth: [3, 4], learningRate: [0.1] },
    { k: 3, seed: 1, scoring: negMae });
const boosted = new GradientBoostingRegressor({ ...search.best, seed: 1 }).fit(Xtrain, ytrain);
const boostedPred = boosted.predict(Xtest);

// ---- compare ---------------------------------------------------------------
const report = (name, pred) => {
    const mae = meanAbsoluteError(ytest, pred);
    console.log(`  ${name.padEnd(8)} MAE ${mae.toFixed(0).padStart(7)}   R² ${r2Score(ytest, pred).toFixed(3)}`);
    return mae;
};
console.log("held-out performance:");
const linearMae = report("linear", linearPred);
const boostedMae = report("boosted", boostedPred);
console.log("  best boosting params:", JSON.stringify(search.best));

// Cross-validation gives a spread, not a single lucky number.
const cv = crossValScore(() => new LinearRegression(), X, y, { k: 5, seed: 3, scoring: (yt, yp) => meanAbsoluteError(yt, yp) });
console.log("  linear 5-fold MAE:", cv.map((v) => v.toFixed(0)).join(" "));

// A quote for one flat: 80 m², 3 rooms, 10 years old, 4 km out, waterfront.
const flat = [[80, 3, 10, 4, 2]];
console.log(`quote: linear ${linear.predict(flat)[0].toFixed(0)}, boosted ${boosted.predict(flat)[0].toFixed(0)}`);

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
check(linear.coef[0] > 2000 && linear.coef[2] < 0 && linear.coef[3] < 0, "linear signs make sense: bigger is dearer, older and farther are cheaper");
check(boostedMae < linearMae * 0.8, "boosting captures the district interaction the line cannot");
check(r2Score(ytest, boostedPred) > 0.9, "the boosted model explains most of the variance");
check(cv.length === 5 && Math.max(...cv) / Math.min(...cv) < 1.6, "fold scores are stable");
console.log("self-test passed");
linear.close(); boosted.close();
