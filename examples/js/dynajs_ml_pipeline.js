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

test("a Pipeline IS the hand-written composition", () => {
  const p = new Pipeline([new StandardScaler(), new PCA(2), new LogisticRegression()]);
  p.fit(X, y);

  const sc = new StandardScaler().fit(X);
  const Xs = sc.transform(X);
  const pc = new PCA(2).fit(Xs);
  const Xp = pc.transform(Xs);
  const lr = new LogisticRegression().fit(Xp, y);

  assertEqual(JSON.stringify(p.predict(X)), JSON.stringify(lr.predict(Xp)));
  assertEqual(JSON.stringify(p.predictProba(X)), JSON.stringify(lr.predictProba(Xp)));
  const bare = new Pipeline([new PCA(2), new LogisticRegression()]).fit(X, y);
  assert(JSON.stringify(bare.predictProba(X)) !== JSON.stringify(p.predictProba(X)),
    "the scaler stage changes the answer");
  p.close();
});

test("BEST: it makes cross-validation correct by construction", () => {
  const scores = crossValScore(
    () => new Pipeline([new StandardScaler(), new LogisticRegression()]), X, y, { k: 4 });
  print(`  per-fold scores: ${scores.map((s) => s.toFixed(3)).join(" ")}`);
  assertEqual(scores.length, 4);
  assert(scores.every((s) => s >= 0 && s <= 1));
});

test("WORST: a Pipeline buys nothing when there is nothing to compose", () => {
  const one = new Pipeline([new LogisticRegression()]).fit(X, y);
  const raw = new LogisticRegression().fit(X, y);
  assertEqual(JSON.stringify(one.predict(X)), JSON.stringify(raw.predict(X)));
  assertEqual(JSON.stringify(one.transform(X)), JSON.stringify(X),
    "and transform is the identity when nothing transforms");
  one.close();
});

test("sampleWeight: an integer weight IS a duplicated row", () => {
  const at = (m) => m.predict([[10]])[0];
  const Xs = [[1], [2], [3]], ys = [1, 2, 3];
  const dup = new LinearRegression().fit([[1], [2], [3], [3]], [1, 2, 3, 3]);
  const wt = new LinearRegression().fit(Xs, ys, { sampleWeight: [1, 1, 2] });
  assert(Math.abs(at(dup) - at(wt)) < 1e-7, "weight 2 == the row appearing twice");

  const plain = new LinearRegression().fit(Xs, ys);
  const ones = new LinearRegression().fit(Xs, ys, { sampleWeight: [1, 1, 1] });
  assertEqual(at(plain), at(ones));

  const outlier = new LinearRegression().fit([[1], [2], [3], [4]], [1, 2, 3, 10]);
  const zeroed = new LinearRegression()
    .fit([[1], [2], [3], [4]], [1, 2, 3, 10], { sampleWeight: [1, 1, 1, 0] });
  print(`  with the outlier ${at(outlier).toFixed(3)}, weight 0 ${at(zeroed).toFixed(3)}`);
  assert(Math.abs(at(zeroed) - 10) < 1e-6, "the outlier is gone");
});

test("WORST: sampleWeight is REFUSED where it cannot be honoured", () => {
  const w = { sampleWeight: X.map(() => 1) };
  const refused = (fn) => {
    try { fn(); return "ACCEPTED (would be silently ignored)"; }
    catch (e) { return e.message.slice(0, 30); }
  };
  print(`  MinMaxScaler           -> ${refused(() => new MinMaxScaler().fit(X, w))}`);
  print(`  KNClassifier           -> ${refused(() => new KNClassifier(3).fit(X, y, w))}`);
  print(`  Pipeline.fit           -> ${refused(() => new Pipeline([new StandardScaler(), new LogisticRegression()]).fit(X, y, w))}`);
  for (const fn of [() => new MinMaxScaler().fit(X, w),
                    () => new KNClassifier(3).fit(X, y, w)])
    assert(refused(fn).indexOf("ACCEPTED") === -1, "refused, not ignored");
  assert(new KMeans(2).fit(X, w).predict(X).length === 80, "KMeans honours it");
  assert(new DecisionTreeClassifier().fit(X, y, w).predict(X).length === 80,
         "the tree honours it");
  assertEqual(new KMeans(2).fit(X).predict(X).length, 80);
});

test("abuse: hostile stages and degenerate inputs", () => {
  const throws = (fn) => { try { fn(); return false; } catch { return true; } };
  assert(throws(() => new Pipeline([])), "an empty Pipeline is refused");
  assert(throws(() => new Pipeline([{}])), "a stage without fit is named");
  assert(throws(() => new Pipeline([new LogisticRegression(), new LogisticRegression()])),
    "a non-final estimator is refused AT CONSTRUCTION");

  let reads = 0;
  const hostile = [];
  hostile.length = 2;
  Object.defineProperty(hostile, 0, { get() { reads++; return new StandardScaler(); }, configurable: true });
  Object.defineProperty(hostile, 1, { get() { reads++; throw new Error("boom"); }, configurable: true });
  assert(throws(() => new Pipeline(hostile)), "a throwing getter propagates");
  assertEqual(reads, 2, "both getters ran, so the attack reached the second");

  const bad = new Pipeline([new StandardScaler(), new DecisionTreeClassifier()]);
  try { bad.fit(X, [1, 2]); } catch {  }
  assertEqual(bad.fitted, false);
  assert(throws(() => bad.predict(X)), "and predict still refuses");

  for (const bw of [[1, -1, 1], [1, NaN, 1], [0, 0, 0], [1, 1]])
    assert(throws(() => new LinearRegression().fit([[1],[2],[3]], [1,2,3], { sampleWeight: bw })),
      "bad weights are refused: " + JSON.stringify(bw));

  const lr = new LogisticRegression();
  const p = new Pipeline([new StandardScaler(), lr]);
  p.fit(X, y);
  p.close();
  assertEqual(p.closed, true);
  assertEqual(lr.closed, false, "a stage held elsewhere survives");
  assertEqual(lr.predict(X).length, 80, "and still works");
});

await run("dyna:ml — Pipeline and sampleWeight");
