// flags: --std
import { test, run, assert, assertEqual } from "./harness.js";
import {
  Pipeline, StandardScaler, PCA, LogisticRegression, LinearRegression,
  DecisionTreeClassifier, KMeans, MinMaxScaler, KNClassifier, crossValScore,
} from "dyna:ml";

const X = [], y = [];
for (let i = 0; i < 80; i++) {
  const a = (i % 2) ? 3 + Math.sin(i) * 0.4 : 0.5 + Math.sin(i) * 0.4;
  X.push([a * 100, a * 2 + 1, i % 7]);
  y.push(i % 2);
}

test("a Pipeline IS the hand-written composition", async () => {
  const p = new Pipeline([new StandardScaler(), new PCA(2), new LogisticRegression()]);
  await p.fit(X, y);

  const sc = await new StandardScaler().fit(X);
  const Xs = await sc.transform(X);
  const pc = await new PCA(2).fit(Xs);
  const Xp = await pc.transform(Xs);
  const lr = await new LogisticRegression().fit(Xp, y);

  assertEqual(JSON.stringify(await p.predict(X)), JSON.stringify(await lr.predict(Xp)));
  assertEqual(JSON.stringify(await p.predictProba(X)), JSON.stringify(await lr.predictProba(Xp)));
  const bare = await new Pipeline([new PCA(2), new LogisticRegression()]).fit(X, y);
  assert(JSON.stringify(await bare.predictProba(X)) !== JSON.stringify(await p.predictProba(X)),
    "the scaler stage changes the answer");
  p.close();
});

test("BEST: it makes cross-validation correct by construction", async () => {
  const scores = await crossValScore(
    () => new Pipeline([new StandardScaler(), new LogisticRegression()]), X, y, { k: 4 });
  print(`  per-fold scores: ${scores.map((s) => s.toFixed(3)).join(" ")}`);
  assertEqual(scores.length, 4);
  assert(scores.every((s) => s >= 0 && s <= 1));
});

test("WORST: a Pipeline buys nothing when there is nothing to compose", async () => {
  const one = await new Pipeline([new LogisticRegression()]).fit(X, y);
  const raw = await new LogisticRegression().fit(X, y);
  assertEqual(JSON.stringify(await one.predict(X)), JSON.stringify(await raw.predict(X)));
  assertEqual(JSON.stringify(await one.transform(X)), JSON.stringify(X),
    "and transform is the identity when nothing transforms");
  one.close();
});

test("sampleWeight: an integer weight IS a duplicated row", async () => {
  const at = async (m) => (await m.predict([[10]]))[0];
  const Xs = [[1], [2], [3]], ys = [1, 2, 3];
  const dup = await new LinearRegression().fit([[1], [2], [3], [3]], [1, 2, 3, 3]);
  const wt = await new LinearRegression().fit(Xs, ys, { sampleWeight: [1, 1, 2] });
  assert(Math.abs((await at(dup)) - (await at(wt))) < 1e-7, "weight 2 == the row appearing twice");

  const plain = await new LinearRegression().fit(Xs, ys);
  const ones = await new LinearRegression().fit(Xs, ys, { sampleWeight: [1, 1, 1] });
  assertEqual(await at(plain), await at(ones));

  const outlier = await new LinearRegression().fit([[1], [2], [3], [4]], [1, 2, 3, 10]);
  const zeroed = await new LinearRegression()
    .fit([[1], [2], [3], [4]], [1, 2, 3, 10], { sampleWeight: [1, 1, 1, 0] });
  print(`  with the outlier ${(await at(outlier)).toFixed(3)}, weight 0 ${(await at(zeroed)).toFixed(3)}`);
  assert(Math.abs((await at(zeroed)) - 10) < 1e-6, "the outlier is gone");
});

test("WORST: sampleWeight is REFUSED where it cannot be honoured", async () => {
  const w = { sampleWeight: X.map(() => 1) };
  const refused = async (fn) => {
    try { await fn(); return "ACCEPTED (would be silently ignored)"; }
    catch (e) { return e.message.slice(0, 30); }
  };
  print(`  MinMaxScaler           -> ${await refused(async () => await new MinMaxScaler().fit(X, w))}`);
  print(`  KNClassifier           -> ${await refused(async () => await new KNClassifier(3).fit(X, y, w))}`);
  print(`  Pipeline.fit           -> ${await refused(async () => await new Pipeline([new StandardScaler(), new LogisticRegression()]).fit(X, y, w))}`);
  for (const fn of [async () => await new MinMaxScaler().fit(X, w),
                    async () => await new KNClassifier(3).fit(X, y, w)])
    assert((await refused(fn)).indexOf("ACCEPTED") === -1, "refused, not ignored");
  assert((await (await new KMeans(2).fit(X, w)).predict(X)).length === 80, "KMeans honours it");
  assert((await (await new DecisionTreeClassifier().fit(X, y, w)).predict(X)).length === 80,
         "the tree honours it");
  assertEqual((await (await new KMeans(2).fit(X)).predict(X)).length, 80);
});

test("abuse: hostile stages and degenerate inputs", async () => {
  const throws = async (fn) => { try { await fn(); return false; } catch { return true; } };
  assert(await throws(() => new Pipeline([])), "an empty Pipeline is refused");
  assert(await throws(() => new Pipeline([{}])), "a stage without fit is named");
  assert(await throws(() => new Pipeline([new LogisticRegression(), new LogisticRegression()])),
    "a non-final estimator is refused AT CONSTRUCTION");

  let reads = 0;
  const hostile = [];
  hostile.length = 2;
  Object.defineProperty(hostile, 0, { get() { reads++; return new StandardScaler(); }, configurable: true });
  Object.defineProperty(hostile, 1, { get() { reads++; throw new Error("boom"); }, configurable: true });
  assert(await throws(() => new Pipeline(hostile)), "a throwing getter propagates");
  assertEqual(reads, 2, "both getters ran, so the attack reached the second");

  const bad = new Pipeline([new StandardScaler(), new DecisionTreeClassifier()]);
  try { await bad.fit(X, [1, 2]); } catch {  }
  assertEqual(bad.fitted, false);
  assert(await throws(async () => await bad.predict(X)), "and predict still refuses");

  for (const bw of [[1, -1, 1], [1, NaN, 1], [0, 0, 0], [1, 1]])
    assert(await throws(async () => await new LinearRegression().fit([[1],[2],[3]], [1,2,3], { sampleWeight: bw })),
      "bad weights are refused: " + JSON.stringify(bw));

  const lr = new LogisticRegression();
  const p = new Pipeline([new StandardScaler(), lr]);
  await p.fit(X, y);
  p.close();
  assertEqual(p.closed, true);
  assertEqual(lr.closed, false, "a stage held elsewhere survives");
  assertEqual((await lr.predict(X)).length, 80, "and still works");
});

await run("dyna:ml — Pipeline and sampleWeight");
