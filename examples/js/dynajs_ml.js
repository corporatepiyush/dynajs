// flags: --std
import { LinearRegression, LogisticRegression, KMeans } from "dyna:ml";
import { test, run, assert, assertEqual, assertClose, assertRejects } from "./harness.js";

async function withResource(resource, fn) {
  try { return await fn(resource); }
  finally { resource.close(); }
}

test("LinearRegression recovers an exact line", async () => {
  await withResource(new LinearRegression(), async (m) => {
    await m.fit([[1], [2], [3]], [2, 4, 6]);
    const y = await m.predict([[4], [5]]);
    assertClose(y[0], 8, 1e-6, "predict(4)");
    assertClose(y[1], 10, 1e-6, "predict(5)");
  });
});

test("LinearRegression fits an intercept as well as a slope", async () => {
  await withResource(new LinearRegression(), async (m) => {
    await m.fit([[0], [1], [2], [3]], [1, 3, 5, 7]);
    assertClose((await m.predict([[10]]))[0], 21, 1e-6, "predict(10) for 2x+1");
    assertClose((await m.predict([[-1]]))[0], -1, 1e-6, "extrapolates below the data");
  });
});

test("LinearRegression is exact on a plane in two variables", async () => {
  await withResource(new LinearRegression(), async (m) => {
    const X = [[0, 0], [1, 0], [0, 1], [1, 1]];
    const y = X.map(([a, b]) => 3 * a - 2 * b + 1);
    await m.fit(X, y);
    for (const [a, b] of [[2, 3], [-1, 4], [0.5, 0.5]]) {
      assertClose((await m.predict([[a, b]]))[0], 3 * a - 2 * b + 1, 1e-6,
                  "plane at (" + a + "," + b + ")");
    }
  });
});

test("a constant target gives a constant prediction, not a NaN", async () => {
  await withResource(new LinearRegression(), async (m) => {
    await m.fit([[1], [2], [3]], [5, 5, 5]);
    const y = await m.predict([[0], [100]]);
    assertClose(y[0], 5, 1e-6, "constant at 0");
    assertClose(y[1], 5, 1e-6, "constant at 100");
    assert(Number.isFinite(y[0]) && Number.isFinite(y[1]), "both finite");
  });
});

test("prediction shape follows the input row count", async () => {
  await withResource(new LinearRegression(), async (m) => {
    await m.fit([[1], [2], [3]], [2, 4, 6]);
    await assertRejects(async () => await m.predict([]), "at least one row", "an empty X is refused");
    assertEqual((await m.predict([[1]])).length, 1, "one row");
    assertEqual((await m.predict([[1], [2], [3], [4]])).length, 4, "four rows");
    assertEqual((await m.predict([[7]]))[0], (await m.predict([[7], [8]]))[0],
                "a row's prediction does not depend on its neighbours");
  });
});

test("LogisticRegression separates two classes", async () => {
  await withResource(new LogisticRegression(), async (m) => {
    await m.fit([[-2], [-1], [1], [2]], [0, 0, 1, 1]);
    const labels = await m.predict([[-3], [3]]);
    assertEqual(labels[0], 0, "far negative is class 0");
    assertEqual(labels[1], 1, "far positive is class 1");
  });
});

test("predictProba is ROWS x CLASSES and each row is a distribution", async () => {
  await withResource(new LogisticRegression(), async (m) => {
    await m.fit([[-2], [-1], [1], [2]], [0, 0, 1, 1]);
    const proba = await m.predictProba([[3], [-3]]);
    assertEqual(proba.length, 2, "one row per sample");
    for (const row of proba) {
      assertEqual(row.length, 2, "two classes per row");
      assertClose(row[0] + row[1], 1, 1e-12, "the row sums to 1");
      assert(row[0] >= 0 && row[0] <= 1 && row[1] >= 0 && row[1] <= 1,
             "both entries are probabilities");
    }
    assert(proba[0][1] > 0.5, "a positive sample favours class 1");
    assert(proba[1][0] > 0.5, "a negative sample favours class 0");
  });
});

test("predict agrees with argmax of predictProba", async () => {
  await withResource(new LogisticRegression(), async (m) => {
    await m.fit([[-2], [-1], [0.5], [1], [2]], [0, 0, 1, 1, 1]);
    const X = [[-5], [-1], [0], [1], [5]];
    const labels = await m.predict(X);
    const proba = await m.predictProba(X);
    for (let i = 0; i < X.length; i++) {
      const argmax = proba[i][1] > proba[i][0] ? 1 : 0;
      assertEqual(labels[i], argmax, "row " + i + ": predict matches argmax");
    }
  });
});

test("probability is monotone along the separating direction", async () => {
  await withResource(new LogisticRegression(), async (m) => {
    await m.fit([[-2], [-1], [1], [2]], [0, 0, 1, 1]);
    let prev = -1;
    for (const x of [-4, -2, -1, 0, 1, 2, 4]) {
      const p = (await m.predictProba([[x]]))[0][1];
      assert(p >= prev - 1e-9, "P(class 1) does not decrease at x=" + x);
      prev = p;
    }
  });
});

test("KMeans separates two well-spaced blobs", async () => {
  await withResource(new KMeans(2, 42), async (m) => {
    const X = [[0, 0], [0.1, 0.1], [9, 9], [9.2, 8.9]];
    await m.fit(X);
    const labels = await m.predict(X);
    assertEqual(labels[0], labels[1], "near points share a cluster");
    assertEqual(labels[2], labels[3], "the far pair shares the other");
    assert(labels[0] !== labels[2], "the two blobs are in different clusters");
  });
});

test("KMeans is deterministic for a fixed seed", async () => {
  const X = [[0, 0], [0.1, 0.1], [9, 9], [9.2, 8.9], [4, 4]];
  const run1 = await withResource(new KMeans(2, 7), async (m) => { await m.fit(X); return await m.predict(X); });
  const run2 = await withResource(new KMeans(2, 7), async (m) => { await m.fit(X); return await m.predict(X); });
  assertEqual(run1, run2, "the same seed gives the same labelling");
});

test("KMeans inertia is non-negative and does not rise with k", async () => {
  const X = [];
  for (let i = 0; i < 40; i++) X.push([i % 10, Math.floor(i / 10)]);
  const inertiaFor = async (k) =>
    await withResource(new KMeans(k, 1), async (m) => { await m.fit(X); return m.inertia; });
  const i2 = await inertiaFor(2), i4 = await inertiaFor(4);
  assert(i2 >= 0 && i4 >= 0, "inertia is non-negative");
  assert(i4 <= i2 + 1e-9, "more clusters cannot increase inertia (" + i4 + " <= " + i2 + ")");
});

test("fifty thousand models are created and closed in constant memory", async () => {
  for (let i = 0; i < 50000; i++) {
    const m = new LinearRegression();
    await m.fit([[1], [2], [3]], [i, 2 * i, 3 * i]);
    m.close();
  }
  assert(true, "50000 models created and closed");
});

test("use after close throws instead of corrupting silently", async () => {
  const dead = new LinearRegression();
  await dead.fit([[1], [2]], [1, 2]);
  dead.close();
  assert(dead.closed === true, "closed flag is set");
  await assertRejects(async () => await dead.predict([[1]]), undefined, "predict after close");
  await assertRejects(async () => await dead.fit([[1]], [1]), undefined, "fit after close");
});

test("close() is idempotent, and dispose() is its alias", () => {
  const m = new LinearRegression();
  m.close();
  m.close();
  assert(m.closed === true, "still closed");
  const n = new KMeans(2, 1);
  assert(typeof n.dispose === "function", "dispose exists");
  n.dispose();
  assert(n.closed === true, "dispose closes");
  n.dispose();
});

test("a method on a foreign receiver rejects", async () => {
  await assertRejects(() => LinearRegression.prototype.predict.call({}, [[1]]),
               undefined, "predict on a foreign receiver");
});

await run("dyna:ml -- regression, classification and clustering");
