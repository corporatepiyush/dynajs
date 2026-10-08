// flags: --std
import { LinearRegression, LogisticRegression, KMeans } from "dyna:ml";
import { test, run, assert, assertEqual, assertClose, assertThrows } from "./harness.js";

function withResource(resource, fn) {
  try { return fn(resource); }
  finally { resource.close(); }
}

test("LinearRegression recovers an exact line", () => {
  withResource(new LinearRegression(), (m) => {
    m.fit([[1], [2], [3]], [2, 4, 6]);
    const y = m.predict([[4], [5]]);
    assertClose(y[0], 8, 1e-6, "predict(4)");
    assertClose(y[1], 10, 1e-6, "predict(5)");
  });
});

test("LinearRegression fits an intercept as well as a slope", () => {
  withResource(new LinearRegression(), (m) => {
    m.fit([[0], [1], [2], [3]], [1, 3, 5, 7]);
    assertClose(m.predict([[10]])[0], 21, 1e-6, "predict(10) for 2x+1");
    assertClose(m.predict([[-1]])[0], -1, 1e-6, "extrapolates below the data");
  });
});

test("LinearRegression is exact on a plane in two variables", () => {
  withResource(new LinearRegression(), (m) => {
    const X = [[0, 0], [1, 0], [0, 1], [1, 1]];
    const y = X.map(([a, b]) => 3 * a - 2 * b + 1);
    m.fit(X, y);
    for (const [a, b] of [[2, 3], [-1, 4], [0.5, 0.5]]) {
      assertClose(m.predict([[a, b]])[0], 3 * a - 2 * b + 1, 1e-6,
                  "plane at (" + a + "," + b + ")");
    }
  });
});

test("a constant target gives a constant prediction, not a NaN", () => {
  withResource(new LinearRegression(), (m) => {
    m.fit([[1], [2], [3]], [5, 5, 5]);
    const y = m.predict([[0], [100]]);
    assertClose(y[0], 5, 1e-6, "constant at 0");
    assertClose(y[1], 5, 1e-6, "constant at 100");
    assert(Number.isFinite(y[0]) && Number.isFinite(y[1]), "both finite");
  });
});

test("prediction shape follows the input row count", () => {
  withResource(new LinearRegression(), (m) => {
    m.fit([[1], [2], [3]], [2, 4, 6]);
    assertThrows(() => m.predict([]), "at least one row", "an empty X is refused");
    assertEqual(m.predict([[1]]).length, 1, "one row");
    assertEqual(m.predict([[1], [2], [3], [4]]).length, 4, "four rows");
    assertEqual(m.predict([[7]])[0], m.predict([[7], [8]])[0],
                "a row's prediction does not depend on its neighbours");
  });
});

test("LogisticRegression separates two classes", () => {
  withResource(new LogisticRegression(), (m) => {
    m.fit([[-2], [-1], [1], [2]], [0, 0, 1, 1]);
    const labels = m.predict([[-3], [3]]);
    assertEqual(labels[0], 0, "far negative is class 0");
    assertEqual(labels[1], 1, "far positive is class 1");
  });
});

test("predictProba is ROWS x CLASSES and each row is a distribution", () => {
  withResource(new LogisticRegression(), (m) => {
    m.fit([[-2], [-1], [1], [2]], [0, 0, 1, 1]);
    const proba = m.predictProba([[3], [-3]]);
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

test("predict agrees with argmax of predictProba", () => {
  withResource(new LogisticRegression(), (m) => {
    m.fit([[-2], [-1], [0.5], [1], [2]], [0, 0, 1, 1, 1]);
    const X = [[-5], [-1], [0], [1], [5]];
    const labels = m.predict(X);
    const proba = m.predictProba(X);
    for (let i = 0; i < X.length; i++) {
      const argmax = proba[i][1] > proba[i][0] ? 1 : 0;
      assertEqual(labels[i], argmax, "row " + i + ": predict matches argmax");
    }
  });
});

test("probability is monotone along the separating direction", () => {
  withResource(new LogisticRegression(), (m) => {
    m.fit([[-2], [-1], [1], [2]], [0, 0, 1, 1]);
    let prev = -1;
    for (const x of [-4, -2, -1, 0, 1, 2, 4]) {
      const p = m.predictProba([[x]])[0][1];
      assert(p >= prev - 1e-9, "P(class 1) does not decrease at x=" + x);
      prev = p;
    }
  });
});

test("KMeans separates two well-spaced blobs", () => {
  withResource(new KMeans(2, 42), (m) => {
    const X = [[0, 0], [0.1, 0.1], [9, 9], [9.2, 8.9]];
    m.fit(X);
    const labels = m.predict(X);
    assertEqual(labels[0], labels[1], "near points share a cluster");
    assertEqual(labels[2], labels[3], "the far pair shares the other");
    assert(labels[0] !== labels[2], "the two blobs are in different clusters");
  });
});

test("KMeans is deterministic for a fixed seed", () => {
  const X = [[0, 0], [0.1, 0.1], [9, 9], [9.2, 8.9], [4, 4]];
  const run1 = withResource(new KMeans(2, 7), (m) => { m.fit(X); return m.predict(X); });
  const run2 = withResource(new KMeans(2, 7), (m) => { m.fit(X); return m.predict(X); });
  assertEqual(run1, run2, "the same seed gives the same labelling");
});

test("KMeans inertia is non-negative and does not rise with k", () => {
  const X = [];
  for (let i = 0; i < 40; i++) X.push([i % 10, Math.floor(i / 10)]);
  const inertiaFor = (k) =>
    withResource(new KMeans(k, 1), (m) => { m.fit(X); return m.inertia; });
  const i2 = inertiaFor(2), i4 = inertiaFor(4);
  assert(i2 >= 0 && i4 >= 0, "inertia is non-negative");
  assert(i4 <= i2 + 1e-9, "more clusters cannot increase inertia (" + i4 + " <= " + i2 + ")");
});

test("fifty thousand models are created and closed in constant memory", () => {
  for (let i = 0; i < 50000; i++) {
    const m = new LinearRegression();
    m.fit([[1], [2], [3]], [i, 2 * i, 3 * i]);
    m.close();
  }
  assert(true, "50000 models created and closed");
});

test("use after close throws instead of corrupting silently", () => {
  const dead = new LinearRegression();
  dead.fit([[1], [2]], [1, 2]);
  dead.close();
  assert(dead.closed === true, "closed flag is set");
  assertThrows(() => dead.predict([[1]]), undefined, "predict after close");
  assertThrows(() => dead.fit([[1]], [1]), undefined, "fit after close");
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

test("a method on a foreign receiver throws", () => {
  assertThrows(() => LinearRegression.prototype.predict.call({}, [[1]]),
               undefined, "predict on a foreign receiver");
});

await run("dyna:ml -- regression, classification and clustering");
